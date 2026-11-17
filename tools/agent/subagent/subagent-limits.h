#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Pure helpers for subagent resource limits and result capping. No runtime
// dependencies (std only) so they are unit-tested directly in
// test-agent-subagent-limits.

// Final subagent responses longer than this are head/tail-capped when handed
// back to the parent (see subagent_cap_result).
constexpr size_t SUBAGENT_RESULT_CAP_BYTES = 32768;

// Trailing marker line appended to a response that stopped on the generated
// token budget.
constexpr const char * SUBAGENT_TOKEN_BUDGET_NOTE = "[subagent token budget exhausted]";

// Trailing marker appended when the loop hit the iteration limit. The partial
// narration accumulated across iterations is kept ahead of this note so the
// parent receives the draft instead of a bare failure string.
constexpr const char * SUBAGENT_MAX_ITER_NOTE = "[subagent reached maximum iterations — output above is a partial draft]";

// Trailing marker appended when the final answer stopped on the per-turn
// generation limit (server stop_type "limit"): the text ends mid-thought, so
// the parent must not treat it as a finished answer. Only added when the
// server actually reported the stop type.
constexpr const char * SUBAGENT_TRUNCATED_NOTE = "[finish: answer cut at the generation limit — partial draft]";

// Append assistant narration to the running partial draft kept across loop
// iterations. The draft is bounded to the last ~8 KB (cuts snapped to UTF-8
// boundaries) so a cut-off loop can return a useful draft without swamping
// the parent context.
void subagent_append_draft(std::string & draft, const std::string & text);

// Read-only bash gate for restricted subagents (explore). True only when EVERY
// command segment (split on ; | & and newlines) starts with one of the allowed
// prefixes at a token boundary. Empty commands/segments-only input is allowed;
// any unknown command denies. Pure std so it is unit-tested directly.
bool subagent_bash_allowed(const std::string & command, const std::vector<std::string> & prefixes);

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

// Tool-call budget (task tool "max_tool_calls"). Marked on the denied call's
// result so the model wraps up instead of exploring further.
constexpr const char * SUBAGENT_TOOL_BUDGET_NOTE =
    "[subagent tool budget exhausted - produce your final answer from what you have]";

enum subagent_tool_budget_state {
    SUBAGENT_TOOL_BUDGET_OK = 0,        // run the call
    SUBAGENT_TOOL_BUDGET_WARNING = 1,   // run it, annotate usage at >=80%
    SUBAGENT_TOOL_BUDGET_EXHAUSTED = 2, // deny the call with SUBAGENT_TOOL_BUDGET_NOTE
};

// max_calls <= 0 disables the budget (always OK). used counts calls already run.
subagent_tool_budget_state subagent_tool_budget_check(int64_t used, int64_t max_calls);

// "[tool budget: used X of Y]" - emitted for the WARNING state.
std::string subagent_tool_budget_note(int64_t used, int64_t max_calls);

// Aggregate per-call summary lines "name (123ms)" into one line per tool:
// "name xN, total Mms" (first-appearance order). Lines that do not parse are
// passed through unchanged so information is never silently lost.
std::vector<std::string> subagent_aggregate_tool_calls(const std::vector<std::string> & entries);

// Replacement body for an oversized subagent answer offloaded to a file:
// head preview (<= preview_bytes, snapped to a UTF-8 boundary) + a note with
// the path, byte counters and the resume hint. Pure text; writing the file is
// the caller's job (and its failure is the caller's fallback to subagent_cap_result).
std::string subagent_offload_note(const std::string & text, const std::string & path,
                                  const std::string & task_id, size_t preview_bytes);
