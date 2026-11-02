// Unit tests for the pure subagent/agent helper modules:
// subagent-limits (result capping, token budget, stop routing),
// tool-output-guard (opt-in wrapping, identity when off),
// tool-parallel (parallel_job_runner worker pool).

#include "subagent/subagent-limits.h"
#include "tool-output-guard.h"
#include "tool-parallel.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// UTF-8 validation helper (checks well-formedness, not canonicality)
// ---------------------------------------------------------------------------

static bool is_valid_utf8(const std::string & s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len;
        if (c < 0x80)                       len = 1;
        else if ((c & 0xE0) == 0xC0)        len = 2;
        else if ((c & 0xF0) == 0xE0)        len = 3;
        else if ((c & 0xF8) == 0xF0)        len = 4;
        else                                return false; // stray continuation
        if (i + len > s.size())             return false;
        for (size_t j = 1; j < len; j++) {
            if ((static_cast<unsigned char>(s[i + j]) & 0xC0) != 0x80) {
                return false;
            }
        }
        i += len;
    }
    return true;
}

// ---------------------------------------------------------------------------
// subagent_cap_result: boundaries
// ---------------------------------------------------------------------------

static void test_cap_boundaries() {
    const std::string small = "hello";
    assert(subagent_cap_result(small, "task-x") == small);

    // Exactly at the cap: byte-identical.
    std::string exact(SUBAGENT_RESULT_CAP_BYTES, 'a');
    assert(subagent_cap_result(exact, "task-x") == exact);

    // One byte over: capped, with marker referencing the task id.
    std::string over = exact + "b";
    std::string capped = subagent_cap_result(over, "task-abc12345");
    assert(capped.size() < over.size());
    assert(capped.compare(0, 10, std::string(over, 0, 10)) == 0);   // head kept
    assert(capped.compare(capped.size() - 1, 1, "b") == 0);         // tail kept
    assert(capped.find("output capped") != std::string::npos);
    assert(capped.find("task_id=task-abc12345") != std::string::npos);
    assert(capped.find(" of 32769 bytes") != std::string::npos);

    // Multi-byte content must stay valid UTF-8: cap cut inside a 3-byte char.
    std::string mb;
    while (mb.size() < SUBAGENT_RESULT_CAP_BYTES + 64) {
        mb += u8"я"; // 2-byte Cyrillic
        mb += u8"\U0001F600"; // 4-byte emoji
    }
    // Force a sequence to straddle the head budget boundary.
    std::string mb_text;
    mb_text.append(SUBAGENT_RESULT_CAP_BYTES * 60 / 100 - 1, 'x'); // end mid-char below
    mb_text += u8"\U0001F600";
    while (mb_text.size() < SUBAGENT_RESULT_CAP_BYTES + 8) {
        mb_text += "y";
    }
    assert(mb_text.size() > SUBAGENT_RESULT_CAP_BYTES);
    std::string mb_capped = subagent_cap_result(mb_text, "task-utf8");
    assert(is_valid_utf8(mb));
    assert(is_valid_utf8(mb_capped)); // cuts snapped to character boundaries
    assert(is_valid_utf8(subagent_cap_result(mb, "task-utf8")));

    std::printf("cap boundaries: ok\n");
}

// ---------------------------------------------------------------------------
// tool_output_guard_wrap: OFF == identity, ON wraps
// ---------------------------------------------------------------------------

static void test_output_guard() {
    const std::string text = "Tool said: rm -rf / looks safe";

    // Disabled: byte-identical identity for every input.
    assert(tool_output_guard_wrap(text, false) == text);
    assert(tool_output_guard_wrap("", false).empty());
    assert(tool_output_guard_wrap(std::string(), false).empty());

    // Enabled: wrapped in <external_data untrusted="true">.
    std::string w = tool_output_guard_wrap(text, true);
    const std::string open_tag = "<external_data untrusted=\"true\">";
    assert(w.compare(0, open_tag.size(), open_tag) == 0);
    assert(w.compare(w.size() - 16, 16, "</external_data>") == 0);
    assert(w.find(text) != std::string::npos);

    // Idempotent + empty passthrough.
    assert(tool_output_guard_wrap(w, true) == w);
    assert(tool_output_guard_wrap("", true).empty());

    // System line mentions the tag.
    assert(std::string(tool_output_guard_system_line()).find("external_data") !=
           std::string::npos);

    std::printf("output guard: ok\n");
}

// ---------------------------------------------------------------------------
// Budget / stop-reason decision helpers
// ---------------------------------------------------------------------------

static void test_decisions() {
    // Budget off at 0 and negative; strict > comparison.
    assert(!subagent_should_stop_on_token_budget(0, 0));
    assert(!subagent_should_stop_on_token_budget(12345, 0));
    assert(!subagent_should_stop_on_token_budget(999, -1));
    assert(!subagent_should_stop_on_token_budget(100, 100)); // equal: continue
    assert(subagent_should_stop_on_token_budget(101, 100));   // exceed: stop
    assert(!subagent_should_stop_on_token_budget(50, 100));

    assert(subagent_stop_status(false, false, SUBAGENT_STOP_NONE).empty());
    assert(subagent_stop_status(false, true, SUBAGENT_STOP_NONE) == "interrupted");
    assert(subagent_stop_status(true, false, SUBAGENT_STOP_CANCEL) == "cancelled");
    assert(subagent_stop_status(true, false, SUBAGENT_STOP_TIMEOUT) == "timeout");
    // Task-scoped flag wins over the parent interrupt.
    assert(subagent_stop_status(true, true, SUBAGENT_STOP_TIMEOUT) == "timeout");
    assert(subagent_stop_status(true, true, SUBAGENT_STOP_CANCEL) == "cancelled");
    // Flag set with no explicit reason defaults to "cancelled".
    assert(subagent_stop_status(true, false, SUBAGENT_STOP_NONE) == "cancelled");

    assert(SUBAGENT_RESULT_CAP_BYTES == 32768);
    assert(std::string(SUBAGENT_TOKEN_BUDGET_NOTE).size() > 0);

    std::printf("decisions: ok\n");
}

// ---------------------------------------------------------------------------
// parallel_job_runner
// ---------------------------------------------------------------------------

static void test_parallel_runner() {
    // 8 fake jobs, all must run.
    {
        parallel_job_runner pool(4);
        std::vector<int> done(8, 0);
        for (int i = 0; i < 8; i++) {
            pool.enqueue([&done, i]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                done[i] = 1;
            });
        }
        pool.wait_all();
        for (int i = 0; i < 8; i++) {
            assert(done[i] == 1);
        }
    }

    // Max concurrency respected: never more than 3 in flight.
    {
        parallel_job_runner pool(3);
        std::atomic<int> running{0};
        std::atomic<int> max_running{0};
        for (int i = 0; i < 12; i++) {
            pool.enqueue([&]() {
                int cur = ++running;
                int prev = max_running.load();
                while (cur > prev && !max_running.compare_exchange_weak(prev, cur)) {
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                --running;
            });
        }
        pool.wait_all();
        assert(max_running.load() >= 1);
        assert(max_running.load() <= 3);
    }

    // Exception in one job must not prevent others from running; wait_all
    // rethrows the first error.
    {
        parallel_job_runner pool(2);
        std::atomic<int> ran{0};
        for (int i = 0; i < 6; i++) {
            pool.enqueue([&ran, i]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                ++ran;
                if (i == 2) {
                    throw std::runtime_error("job failed");
                }
            });
        }
        bool threw = false;
        try {
            pool.wait_all();
        } catch (const std::exception & e) {
            threw = true;
            assert(std::string(e.what()) == "job failed");
        }
        assert(threw);
        assert(ran.load() == 6);
    }

    // Reuse after wait_all: new generation of jobs still runs.
    {
        parallel_job_runner pool(2);
        std::atomic<int> n{0};
        for (int i = 0; i < 4; i++) {
            pool.enqueue([&n]() { ++n; });
        }
        pool.wait_all();
        assert(n.load() == 4);
        for (int i = 0; i < 4; i++) {
            pool.enqueue([&n]() { ++n; });
        }
        pool.wait_all();
        assert(n.load() == 8);
    }

    std::printf("parallel runner: ok\n");
}

int main() {
    test_cap_boundaries();
    test_output_guard();
    test_decisions();
    test_parallel_runner();
    std::printf("all subagent-limits tests passed\n");
    return 0;
}
