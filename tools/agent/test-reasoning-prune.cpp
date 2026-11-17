// Unit tests for reasoning-prune: assistant reasoning stripped from wire
// payloads, everything else byte-identical, idempotent.

#include "reasoning-prune.h"

#include <cstdio>
#include <string>

static int failures = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            failures++;                                                     \
        }                                                                   \
    } while (0)



static json assistant_with(const std::string & content, const std::string & thinking) {
    json msg = {{"role", "assistant"}, {"content", content}};
    if (!thinking.empty()) {
        msg["reasoning_content"] = thinking;
    }
    return msg;
}

static void test_strips_every_assistant_block() {
    json messages = json::array();
    messages.push_back({{"role", "system"}, {"content", "sys"}});
    messages.push_back({{"role", "user"}, {"content", "q1"}});
    messages.push_back(assistant_with("a1", "deep thinking one"));
    messages.push_back({{"role", "tool"}, {"content", "result"}});
    messages.push_back(assistant_with("a2", "deep thinking two"));
    messages.push_back({{"role", "user"}, {"content", "q2"}});
    messages.push_back(assistant_with("a3", "current turn notes"));

    const std::size_t removed = strip_wire_reasoning(messages);

    CHECK(removed == 3);
    for (const json & msg : messages) {
        CHECK(!msg.contains("reasoning_content"));
    }
    CHECK(messages[2]["content"] == "a1");
    CHECK(messages[6]["content"] == "a3");
    CHECK(messages[0]["content"] == "sys");
}

static void test_noop_payload_unchanged() {
    json messages = json::array();
    messages.push_back({{"role", "system"}, {"content", "sys"}});
    messages.push_back({{"role", "user"}, {"content", "q"}});
    messages.push_back(assistant_with("a", ""));
    const json before = messages;

    CHECK(strip_wire_reasoning(messages) == 0);
    CHECK(messages == before);
}

static void test_idempotent_and_stable_prompt_prefix() {
    json messages = json::array();
    messages.push_back({{"role", "user"}, {"content", "q"}});
    messages.push_back(assistant_with("a1", "thinking"));
    strip_wire_reasoning(messages);
    const json after_first = messages;

    // A later turn may only append; the already-stripped prefix must not
    // change again (that is what keeps the server KV-cache valid).
    messages.push_back({{"role", "tool"}, {"content", "r"}});
    messages.push_back(assistant_with("a2", "more thinking"));
    CHECK(strip_wire_reasoning(messages) == 1);
    for (std::size_t i = 0; i < after_first.size(); i++) {
        CHECK(messages[i] == after_first[i]);
    }
}

static void test_ignores_malformed_entries() {
    json messages = json::array();
    messages.push_back("not-an-object");
    messages.push_back({{"role", "user"}, {"content", "x"}});
    json user_with_field = {{"role", "user"}, {"content", "y"}};
    user_with_field["reasoning_content"] = "user-side text kept";
    messages.push_back(user_with_field);

    CHECK(strip_wire_reasoning(messages) == 0);
    CHECK(messages[2]["reasoning_content"] == "user-side text kept");
}

static void test_non_array_is_noop() {
    json not_array = json::object();
    CHECK(strip_wire_reasoning(not_array) == 0);
}

int main() {
    test_strips_every_assistant_block();
    test_noop_payload_unchanged();
    test_idempotent_and_stable_prompt_prefix();
    test_ignores_malformed_entries();
    test_non_array_is_noop();

    if (failures == 0) {
        std::printf("reasoning-prune: all tests passed\n");
        return 0;
    }
    std::printf("reasoning-prune: %d FAILURES\n", failures);
    return 1;
}
