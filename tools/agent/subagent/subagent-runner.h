#pragma once

#include "subagent-types.h"
#include "subagent-limits.h"
#include "../tool-registry.h"

#include <atomic>
#include <condition_variable>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Forward declarations
class inference_backend;
struct agent_config;
class subagent_output_buffer;

// Parameters for running a subagent
struct subagent_params {
    subagent_type type = subagent_type::GENERAL;
    std::string prompt;
    std::string description;  // Short description for display
    int64_t max_tokens = 0;   // Generated-token budget for the sub-loop (0 = off)
    int timeout_sec = 0;      // Wall-clock timeout in seconds (0 = no timeout)
};

// Result from a subagent run
struct subagent_result {
    bool success = false;
    std::string output;
    std::string error;
    int iterations = 0;
    std::vector<std::string> tool_calls_summary;  // List of tools called with timing

    // Task id this result belongs to ("sync-*" for synchronous runs). The
    // full output text is kept in the subagent_output_manager under this id.
    std::string task_id;
    // Interruption status: "" (none), "cancelled", "timeout", "interrupted"
    // (parent-wide). See subagent_stop_status.
    std::string status;

    // Token statistics from the subagent run
    int32_t input_tokens = 0;
    int32_t output_tokens = 0;
    int32_t cached_tokens = 0;
};

// Background task state
struct subagent_task {
    std::string id;
    std::thread thread;
    std::promise<subagent_result> promise;
    std::atomic<bool> complete{false};
    // Per-task interrupt flag: cancel(task_id) / the timeout watchdog set this
    // (and stop_reason) without touching the parent's shared interrupt. The
    // subagent loop polls it via agent_loop's extra_interrupt hook.
    std::atomic<bool> cancelled{false};
    std::atomic<int> stop_reason{SUBAGENT_STOP_NONE};  // subagent_stop_flag_reason
    subagent_params params;  // Store params for display

    // Non-copyable, non-movable (atomics prevent move)
    subagent_task() = default;
    subagent_task(const subagent_task &) = delete;
    subagent_task & operator=(const subagent_task &) = delete;
    subagent_task(subagent_task &&) = delete;
    subagent_task & operator=(subagent_task &&) = delete;
};

// Runs a subagent with restricted tool access.
// By default the subagent shares the parent's inference backend (and its server
// slot, if pinned). With --subagent-own-slot each subagent gets a fresh backend
// bound to a free slot (requires --parallel >= 2 on the llama-server).
class subagent_runner {
public:
    subagent_runner(const agent_config & parent_config,
                    const tool_context & parent_tool_ctx);

    ~subagent_runner();

    // Run a subagent synchronously (blocking)
    subagent_result run(const subagent_params & params);

    // Run a subagent in the background (non-blocking)
    // Returns a task ID that can be used to check status and get results
    std::string start_background(const subagent_params & params);

    // Check if a background task is complete
    bool is_complete(const std::string & task_id) const;

    // Wait up to wait_ms (clamped 0..30000) for a task to complete.
    // Returns true when the task is complete (or already reaped).
    bool wait_for_completion(const std::string & task_id, int wait_ms);

    // Get the result of a completed background task
    // Returns empty result if task doesn't exist or isn't complete
    subagent_result get_result(const std::string & task_id);

    // Parent-facing output for resume calls: first call returns the full text
    // capped via subagent_cap_result; later calls for the same task return
    // only the output beyond the previous offset (delta mode) plus a total
    // length note. Tracks last_resume_offset per task.
    std::string take_parent_output(const std::string & task_id, const std::string & full_text);

    // Cancel a background task (sets only that task's flag; the parent's
    // shared interrupt is left untouched)
    void cancel(const std::string & task_id);

    // Long-poll for completion: waits up to wait_ms (0 = instant check).
    // Returns true if the task is complete (or unknown) by return time.
    bool wait_complete(const std::string & task_id, int wait_ms);

    // Full (uncapped) output stored for a finished task. Returns the text
    // appended since the previous call for this id (first call = everything)
    // and reports the total length via total_out. Empty + total 0 = no store.
    std::string get_full_output(const std::string & task_id, size_t & total_out);

    // Get all active task IDs
    std::vector<std::string> get_active_tasks() const;

    // Cleanup completed tasks (call periodically)
    void cleanup_completed();

private:
    const agent_config & parent_config_;
    tool_context parent_tool_ctx_;

    // Background task management
    mutable std::mutex tasks_mutex_;
    std::condition_variable completion_cv_;  // notified when a task completes
    std::map<std::string, std::unique_ptr<subagent_task>> tasks_;
    std::map<std::string, subagent_result> completed_results_;  // Store results after thread joins
    std::map<std::string, size_t> last_resume_offset_;         // Delta-mode resume tracking
    std::map<std::string, std::string> full_results_;          // Full (uncapped) outputs by task id
    std::condition_variable done_cv_;                          // Signals task completion / cancellation

    // Build a system prompt for the subagent type
    std::string build_system_prompt(subagent_type type) const;

    // Generate unique task ID
    static std::string generate_task_id();

    // Internal run method with optional buffer for background tasks.
    // extra_interrupt/stop_reason: per-task interrupt flag + reason code
    // (cancel/timeout); task_id keys the full-result store (empty = don't store).
    subagent_result run_internal(const subagent_params & params,
                                  subagent_output_buffer * buffer,
                                  std::atomic<bool> * extra_interrupt = nullptr,
                                  std::atomic<int> * stop_reason = nullptr,
                                  const std::string & task_id = "");
};
