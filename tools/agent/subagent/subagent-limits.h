#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Pure helpers for subagent resource limits and result capping. No runtime
// dependencies (std only) so they are unit-tested directly in
// test-agent-subagent-limits.

// Final subagent responses longer than this are head/tail-capped when handed
// back to the parent (see subagent_cap_result).
constexpr size_t SUBAGENT_RESULT_CAP_BYTES = 32768;

// Trailing marker line appended to a response that stopped on the generated
// token budget.
constexpr const char * SUBAGENT_TOKEN_BUDGET_NOTE = "[subagent token budget exhausted]";

// Reason codes stored alongside a per-task interrupt flag (subagent-runner).
enum subagent_stop_flag_reason {
    SUBAGENT_STOP_NONE = 0,    // flag not set
    SUBAGENT_STOP_CANCEL = 1,  // cancel(task_id) requested
    SUBAGENT_STOP_TIMEOUT = 2, // timeout expired
};

// Cap a subagent final response for the parent: when text exceeds
// SUBAGENT_RESULT_CAP_BYTES, keep the first 60% and last 40% of that byte
// budget (cuts snapped to UTF-8 boundaries) and replace the middle with:
//   \n[... output capped: showing X of Y bytes; full output available via resume task_id=<id> ...]\n
// Text at or below the cap is returned byte-identical. The full text must be
// stored under task_id (subagent_output_manager) so it stays retrievable.
std::string subagent_cap_result(const std::string & text, const std::string & task_id);

// Decision function for the per-loop generated-token budget: returns true when
// the loop's generated-token counter exceeds max_generate_tokens. A budget of
// zero (or below) means the limit is off.
bool subagent_should_stop_on_token_budget(int64_t generated_tokens, int64_t max_generate_tokens);

// Route an interruption to a task status string. The task-scoped flag wins
// over the parent's shared interrupt: a per-task cancel/timeout must be
// distinguishable from a parent-wide Ctrl+C. Returns "cancelled", "timeout",
// "interrupted" (parent) or "" (not an interruption).
std::string subagent_stop_status(bool task_interrupted, bool parent_interrupted, int reason);
