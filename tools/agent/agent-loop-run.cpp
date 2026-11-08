#include "agent-loop.h"
#include <cstdlib>
#include "subagent/subagent-limits.h"
#include "tool-parallel.h"
#include "console.h"

agent_loop_result agent_loop::run(const json & user_content) {
    agent_loop_result result;
    result.iterations = 0;

    // Add user message (content can be a string or an array of content blocks)
    messages_.push_back({
        {"role", "user"},
        {"content", user_content}
    });
    if (session_file_) session_file_->append_message(messages_.back());

    // Partial narration kept for graceful-degrade at the iteration limit
    // (mirrors run_streaming).
    std::string iter_draft;

    while (config_.max_iterations <= 0 || result.iterations < config_.max_iterations) {
        if (interrupt_requested()) {
            result.stop_reason = agent_stop_reason::USER_CANCELLED;
            return result;
        }

        result.iterations++;

        if (config_.verbose) {
            if (config_.max_iterations > 0) {
                console::log("\n[Iteration %d/%d]\n", result.iterations, config_.max_iterations);
            } else {
                console::log("\n[Iteration %d]\n", result.iterations);
            }
        }

        // Generate completion - returns parsed message with tool calls
        inference_result completion = generate_completion();
        common_chat_msg parsed = completion.message;

        accumulate_stats(completion);

        // Overflow recovery: compact and retry this iteration
        if (parsed.content.empty() && parsed.tool_calls.empty() && last_completion_overflowed_) {
            last_completion_overflowed_ = false;
            if (config_.compaction.enabled && try_compact()) {
                console::log("[Context compacted, retrying...]\n");
                result.iterations--;
                continue;
            }
            // Compaction disabled or could not free space: surface the error instead of
            // falling through to a fake empty "completed" turn (mirrors run_streaming).
            console::error("Context overflow: compaction could not free enough space\n");
            result.stop_reason = agent_stop_reason::AGENT_ERROR;
            return result;
        }

        if (parsed.content.empty() && parsed.tool_calls.empty() && interrupt_requested()) {
            result.stop_reason = agent_stop_reason::USER_CANCELLED;
            return result;
        }

        // Generated-token budget (task tool "max_tokens"): stop cleanly with a
        // distinct reason and mark the final response (mirrors run_streaming).
        if (subagent_should_stop_on_token_budget(stats_.total_output.load(),
                                                 config_.max_generate_tokens)) {
            if (!parsed.content.empty() || !parsed.tool_calls.empty()) {
                json assistant_msg = build_assistant_msg(parsed, result.iterations);
                messages_.push_back(assistant_msg);
                if (session_file_) session_file_->append_message(assistant_msg);
            }
            result.stop_reason = agent_stop_reason::TOKEN_BUDGET_EXCEEDED;
            result.final_response = parsed.content.empty()
                ? std::string(SUBAGENT_TOKEN_BUDGET_NOTE)
                : parsed.content + "\n" + SUBAGENT_TOKEN_BUDGET_NOTE;
            return result;
        }

        // Threshold compaction after successful completion
        if (config_.compaction.enabled) {
            try_compact();
        }

        // Empty response - don't save to history, just end the turn
        if (parsed.content.empty() && parsed.tool_calls.empty()) {
            result.stop_reason = agent_stop_reason::COMPLETED;
            result.final_response = "";
            return result;
        }

        // Add assistant message to history
        json assistant_msg = build_assistant_msg(parsed, result.iterations);
        messages_.push_back(assistant_msg);
        if (session_file_) session_file_->append_message(assistant_msg);
        subagent_append_draft(iter_draft, parsed.content);

        // If no tool calls, we're done
        if (parsed.tool_calls.empty()) {
            result.stop_reason = agent_stop_reason::COMPLETED;
            result.final_response = parsed.content;
            return result;
        }

        console::log("\n");

        // Concurrent execution of multiple task calls in one assistant turn
        // (only when subagents are enabled): `task` calls run on a worker pool
        // capped at 4 threads, all other tools stay strictly sequential as
        // before. Results are appended to the conversation in the ORIGINAL
        // call order regardless of completion order.
        const bool batch_tasks = tool_ctx_.subagents_enabled &&
                                 should_batch_task_calls(parsed.tool_calls);
        if (batch_tasks) {
            std::vector<tool_result> results(parsed.tool_calls.size());
            // Concurrency cap is tunable for A/B measurement: with 1 the batch
            // degrades to sequential execution (same code path, no threads race).
            int max_parallel = 4;
            if (const char * env = getenv("LLAMA_AGENT_MAX_PARALLEL_TASKS")) {
                int v = atoi(env);
                if (v < 1) {
                    v = 1;
                }
                if (v > 4) {
                    v = 4;
                }
                max_parallel = v;
            }
            parallel_job_runner task_pool(max_parallel);
            bool cancelled_mid = false;

            for (size_t ci = 0; ci < parsed.tool_calls.size(); ci++) {
                const auto & call = parsed.tool_calls[ci];
                if (interrupt_requested()) {
                    cancelled_mid = true;
                    break;
                }

                if (call.name == "task") {
                    // Permission prompts stay sequential on this thread; only
                    // the (long) subagent run moves to a worker.
                    prepared_tool_call prepared = prepare_tool_call(call);
                    if (!prepared.proceed) {
                        results[ci] = prepared.early;
                        continue;
                    }
                    const common_chat_tool_call call_copy = call;
                    json args = std::move(prepared.args);
                    task_pool.enqueue([this, ci, call_copy, args, &results]() {
                        results[ci] = run_prepared_tool_call(call_copy, args, true);
                    });
                } else {
                    results[ci] = execute_tool_call(call);
                }
            }

            task_pool.wait_all();

            if (cancelled_mid) {
                result.stop_reason = agent_stop_reason::USER_CANCELLED;
                return result;
            }

            // Append results in the original call order
            for (size_t ci = 0; ci < parsed.tool_calls.size(); ci++) {
                const auto & call = parsed.tool_calls[ci];
                std::string call_id = call.id.empty() ? ("call_" + std::to_string(result.iterations)) : call.id;
                add_tool_result_message(call.name, call_id, results[ci]);
            }
            continue;
        }

        // Execute each tool call
        for (const auto & call : parsed.tool_calls) {
            if (interrupt_requested()) {
                result.stop_reason = agent_stop_reason::USER_CANCELLED;
                return result;
            }

            tool_result tool_res = execute_tool_call(call);
            std::string call_id = call.id.empty() ? ("call_" + std::to_string(result.iterations)) : call.id;
            add_tool_result_message(call.name, call_id, tool_res);
        }
    }

    result.stop_reason = agent_stop_reason::MAX_ITERATIONS;
    if (iter_draft.empty()) {
        result.final_response = "Reached maximum iterations (" + std::to_string(config_.max_iterations) + ")";
    } else {
        result.final_response = iter_draft + "\n" + SUBAGENT_MAX_ITER_NOTE;
    }
    return result;
}
