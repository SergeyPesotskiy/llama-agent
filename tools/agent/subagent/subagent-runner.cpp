#include "subagent-runner.h"
#include "subagent-display.h"
#include "subagent-output.h"
#include "../agent-loop.h"
#include "../inference-backend.h"
#include "../subagent-backends.h"
#include "../permission-async.h"
#include "console.h"

#include <chrono>
#include <random>
#include <sstream>

namespace {

// Waits params.timeout_sec for the subagent to finish; if it is still running
// when the deadline passes, raises the task-scoped interrupt flag with
// SUBAGENT_STOP_TIMEOUT. stop() (or destruction, at the end of the run) ends
// the wait early. Purely additive: with seconds <= 0 no thread is started.
struct subagent_timeout_watchdog {
    subagent_timeout_watchdog(std::atomic<bool> & flag, std::atomic<int> & reason, int seconds)
        : flag_(flag), reason_(reason) {
        if (seconds > 0) {
            th_ = std::thread([this, seconds] {
                std::unique_lock<std::mutex> lk(m_);
                if (!cv_.wait_for(lk, std::chrono::seconds(seconds),
                                  [this] { return done_.load(); })) {
                    reason_.store(SUBAGENT_STOP_TIMEOUT);
                    flag_.store(true);
                }
            });
        }
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(m_);
            done_.store(true);
        }
        cv_.notify_all();
        if (th_.joinable()) {
            th_.join();
        }
    }

    ~subagent_timeout_watchdog() {
        stop();
    }

    subagent_timeout_watchdog(const subagent_timeout_watchdog &) = delete;
    subagent_timeout_watchdog & operator=(const subagent_timeout_watchdog &) = delete;

private:
    std::atomic<bool> & flag_;
    std::atomic<int> & reason_;
    std::mutex m_;
    std::condition_variable cv_;
    std::atomic<bool> done_{false};
    std::thread th_;
};

} // namespace

subagent_runner::subagent_runner(const agent_config & parent_config,
                                  const tool_context & parent_tool_ctx)
    : parent_config_(parent_config)
    , parent_tool_ctx_(parent_tool_ctx)
{
}

subagent_runner::~subagent_runner() {
    // Join any remaining background threads so their std::thread members are
    // not joinable when destroyed. This matters at process exit: g_runners
    // (tool-task.cpp) is a static and is torn down in the exit handlers; if the
    // model exited without resuming a background task, its thread was still
    // joinable -> std::thread::~thread() called std::terminate() ("terminate
    // called without an active exception", benchmark D1 crash). The thread's
    // lambda already removes its output buffer on completion, so no extra
    // cleanup is needed here.
    //
    // First raise the shared interrupt flag: a leftover subagent may be blocked
    // in a tool call (e.g. a shell command with a long timeout) or streaming an
    // LLM response; both paths poll this flag, so the join below cannot stall
    // the whole process exit (observed on Windows: agent exited while a
    // background task was still running and ~subagent_runner blocked forever).
    if (parent_tool_ctx_.is_interrupted) {
        parent_tool_ctx_.is_interrupted->store(true);
    }
    for (auto & [id, task] : tasks_) {
        if (task->thread.joinable()) {
            task->thread.join();
        }
    }
}

std::string subagent_runner::build_system_prompt(subagent_type type) const {
    const auto & config = get_subagent_config(type);

    std::ostringstream prompt;

    // Start with parent's base prompt to enable KV cache prefix sharing
    // The server's prompt cache will detect the common prefix and reuse cached tokens
    if (!parent_tool_ctx_.base_system_prompt.empty()) {
        prompt << parent_tool_ctx_.base_system_prompt;
        prompt << "# Subagent Mode: " << config.name << "\n\n";
    } else {
        // Fallback if no base prompt (shouldn't happen in normal operation)
        prompt << "You are a specialized " << config.name << " subagent.\n\n";
    }

    prompt << config.description << "\n\n";

    // Add tool restrictions (overrides base prompt's tool list)
    prompt << "## Tools Available in This Mode\n\n";
    prompt << "You have access to: ";
    bool first = true;
    for (const auto & tool : config.allowed_tools) {
        if (!first) {
            prompt << ", ";
        }
        prompt << tool;
        first = false;
    }
    // This subagent runs at depth parent+1; it may nest further while
    // (parent_depth + 1) < max_depth, in which case the task tool is added
    // to its tool list as well.
    if (parent_tool_ctx_.subagent_depth + 1 < parent_config_.max_subagent_depth) {
        prompt << ", task";
    }
    prompt << "\n\n";

    // Add behavior guidelines based on type
    switch (type) {
        case subagent_type::EXPLORE:
#ifdef _WIN32
            prompt << R"(# Guidelines

You are in READ-ONLY mode. Your task is to explore and understand the codebase.

- Use `glob` to find files matching patterns
- Use `read` to examine file contents
- Use `bash` ONLY for read-only cmd.exe commands: dir, type, findstr, where, timeout /t N, echo, git status, git log, git diff
  (the shell is cmd.exe, not bash: no ls/cat/grep/sleep, chain with & not ;)
- DO NOT modify any files
- DO NOT run destructive commands

Be thorough but efficient. Report what you find clearly.
)";
#else
            prompt << R"(# Guidelines

You are in READ-ONLY mode. Your task is to explore and understand the codebase.

- Use `glob` to find files matching patterns
- Use `read` to examine file contents
- Use `bash` ONLY for read-only commands: ls, cat, head, tail, grep, find, git status, git log, git diff
- DO NOT modify any files
- DO NOT run destructive commands

Be thorough but efficient. Report what you find clearly.
)";
#endif
            break;

        case subagent_type::PLAN:
            prompt << R"(# Guidelines

You are a planning agent. Your task is to design an implementation approach.

- Use `glob` and `read` to understand existing code structure
- Identify patterns and conventions in the codebase
- Consider edge cases and potential issues
- Provide a clear, actionable plan

Output a structured plan with:
1. Overview of the approach
2. Files to modify/create
3. Step-by-step implementation details
4. Potential risks or considerations
)";
            break;

        case subagent_type::GENERAL:
            prompt << R"(# Guidelines

You are a general-purpose task agent. Complete the assigned task efficiently.

- Read files before modifying them
- Make targeted edits rather than full rewrites
- Test changes when possible
- Report what you accomplished
)";
            break;

        case subagent_type::BASH:
            prompt << R"(# Guidelines

You are a command execution agent. Run shell commands to complete the task.

- Execute commands carefully
- Check command output for errors
- Report results clearly
)";
            break;
    }

    return prompt.str();
}

std::string subagent_runner::generate_task_id() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 35);

    const char charset[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::string id = "task-";
    for (int i = 0; i < 8; ++i) {
        id += charset[dis(gen)];
    }
    return id;
}

subagent_result subagent_runner::run(const subagent_params & params) {
    // Synchronous run uses direct console output (no buffer)
    return run_internal(params, nullptr);
}

subagent_result subagent_runner::run_internal(const subagent_params & params,
                                               subagent_output_buffer * buffer,
                                               std::atomic<bool> * extra_interrupt,
                                               std::atomic<int> * stop_reason,
                                               const std::string & task_id) {
    subagent_result result;
    result.task_id = task_id.empty() ? generate_task_id() : task_id;

    // Per-run interrupt flag + reason code: owned by the caller for background
    // tasks (so cancel()/watchdogs can reach them) or local to a synchronous
    // run. The timeout watchdog raises both when params.timeout_sec elapses.
    std::atomic<bool> local_interrupt{false};
    std::atomic<int> local_reason{SUBAGENT_STOP_NONE};
    std::atomic<bool> * interrupt = extra_interrupt ? extra_interrupt : &local_interrupt;
    std::atomic<int> * reason = stop_reason ? stop_reason : &local_reason;
    std::unique_ptr<subagent_timeout_watchdog> watchdog;
    if (params.timeout_sec > 0) {
        watchdog.reset(new subagent_timeout_watchdog(*interrupt, *reason, params.timeout_sec));
    }

    const auto & type_config = get_subagent_config(params.type);

    // Start display scope
    // If buffer is provided (background mode), output goes to buffer
    // Otherwise (synchronous mode), output goes directly to console
    auto & display = subagent_display::instance();
    std::string desc = params.description.empty() ? "subagent" : params.description;
    std::string prompt_preview = params.prompt.substr(0, 60) + (params.prompt.length() > 60 ? "..." : "");

    // Use the appropriate constructor based on whether we have a buffer
    std::unique_ptr<subagent_display::scope> display_scope;
    if (buffer) {
        display_scope = std::make_unique<subagent_display::scope>(display, desc, params.type, prompt_preview, buffer);
    } else {
        display_scope = std::make_unique<subagent_display::scope>(display, desc, params.type, prompt_preview);
    }

    auto start_time = std::chrono::steady_clock::now();

    // Create subagent config with restricted iterations
    agent_config subagent_config = parent_config_;
    subagent_config.max_iterations = type_config.max_iterations;
    subagent_config.max_generate_tokens = params.max_tokens;  // 0 = off
    // --subagent-no-think: subagent completions request thinking-off so
    // extraction-style tasks answer directly instead of spending the whole
    // max_tokens budget on reasoning. Parent loop keeps server default.
    subagent_config.enable_thinking = !parent_config_.subagent_no_think;
    subagent_config.verbose = false;  // Suppress verbose output in subagent
    subagent_config.enable_skills = false;  // No skills for subagents
    subagent_config.enable_agents_md = false;  // No agents.md for subagents
    subagent_config.skills_prompt_section = "";
    subagent_config.agents_md_prompt_section = "";

    // Use parent's interrupted flag so Ctrl+C stops subagents too
    // Note: parent_tool_ctx_.is_interrupted is set by the parent agent_loop

    // Build system prompt
    std::string system_prompt = build_system_prompt(params.type);

    // Create tool call callback to report to display
    auto tool_callback = [&display_scope, &result](const std::string & tool_name,
                                                    const std::string & args_summary,
                                                    int elapsed_ms) {
        display_scope->report_tool_call(tool_name, args_summary, elapsed_ms);
        // Also track in result
        std::ostringstream summary;
        summary << tool_name << " (" << elapsed_ms << "ms)";
        result.tool_calls_summary.push_back(summary.str());
    };

    // Select the inference backend for this subagent.
    // Default: share the parent's backend (same server slot if pinned — KV prefix
    // sharing, but interleaved requests evict each other's working context).
    // With --subagent-own-slot: fresh backend bound to a free slot (id_slot = -1),
    // requires --parallel >= 2 on the llama-server.
    // Lazy init of the external backend pool (--subagent-backends): selector
    // for round-robin/explicit-index + clone cache, on first subagent run.
    {
        std::lock_guard<std::mutex> lock(backend_init_mutex_);
        if (!backend_selector_ && !parent_config_.subagent_backends.empty()) {
            backend_selector_ = std::make_unique<subagent_backends::backend_selector>(
                parent_config_.subagent_backends);
            backend_pool_ = std::make_unique<subagent_backends::backend_pool>(
                static_cast<inference_backend *>(parent_tool_ctx_.backend_ptr));
        }
    }

    // Select the inference backend for this subagent.
    // Explicit task "backend" index -> round-robin over --subagent-backends ->
    // default: share the parent's backend (same server slot if pinned — KV
    // prefix sharing, but interleaved requests evict each other's working
    // context). With --subagent-own-slot (parent backend only): fresh backend
    // bound to a free slot (id_slot = -1), requires --parallel >= 2.
    std::string backend_warning;
    std::unique_ptr<inference_backend> own_slot_backend;
    inference_backend * parent_backend = static_cast<inference_backend *>(parent_tool_ctx_.backend_ptr);
    inference_backend * subagent_backend = parent_backend;
    bool external_backend = false;
    // Lease on the chosen pool endpoint: held for the whole subagent run so
    // concurrent tasks see this endpoint as busy (least-in-flight routing).
    // Releases automatically at scope exit, including early returns/throws.
    subagent_backends::backend_lease backend_lease;
    if (backend_selector_ && !backend_selector_->empty()) {
        if (params.backend_index >= 0) {
            backend_lease = backend_selector_->acquire_lease_at(static_cast<size_t>(params.backend_index));
            if (backend_lease.url().empty()) {
                backend_warning = "[subagent-backend: invalid index " +
                                  std::to_string(params.backend_index) + ", using auto]";
                backend_lease = backend_selector_->acquire_lease();
            }
        } else {
            backend_lease = backend_selector_->acquire_lease();
        }
        const std::string url = backend_lease.url();
        if (!url.empty()) {
            inference_backend * ext = backend_pool_->get_or_create(url, parent_backend);
            if (ext) {
                subagent_backend = ext;
                external_backend = true;
                console::log("[subagent %s] inference backend: %s\n",
                             subagent_type_name(params.type), url.c_str());
            } else {
                std::string w = "[subagent-backend: " + url + " unavailable, using parent backend]";
                backend_warning = backend_warning.empty() ? w : backend_warning + " " + w;
                console::log("[subagent %s] inference backend: parent (fallback from %s)\n",
                             subagent_type_name(params.type), url.c_str());
                backend_lease.release();  // dead endpoint: don't count it as busy
            }
        }
    }
    if (!external_backend && parent_tool_ctx_.subagent_own_slot && subagent_backend) {
        own_slot_backend = subagent_backend->clone_for_subagent();
        if (own_slot_backend) {
            subagent_backend = own_slot_backend.get();
        } else {
            console::error("[subagent] own-slot not supported by backend, falling back to shared slot\n");
        }
    }

    // Create nested agent_loop with filtered tools
    // Pass incremented depth to prevent infinite subagent recursion
    // Pass bash_patterns to enforce read-only restrictions for EXPLORE subagents
    int new_depth = parent_tool_ctx_.subagent_depth + 1;
    std::vector<std::string> bash_patterns(type_config.bash_patterns.begin(), type_config.bash_patterns.end());
    agent_loop subagent(*subagent_backend, subagent_config, *parent_tool_ctx_.is_interrupted,
                        type_config.allowed_tools, bash_patterns, system_prompt, new_depth, tool_callback,
                        interrupt);

    // Run via run_streaming (linked into both the TUI and agent-server binaries;
    // the plain run() helper only exists in the TUI target). Subagent console
    // output is suppressed inside the loop; tool activity is forwarded to the
    // display scope through the event stream.
    permission_manager_async subagent_perms;
    subagent_perms.set_project_root(parent_tool_ctx_.working_dir);
    subagent_perms.set_yolo_mode(parent_config_.yolo_mode);

    std::string pending_args;
    auto on_event = [&display_scope, &result, &pending_args](const agent_event & event) {
        if (event.type == agent_event_type::TOOL_START) {
            pending_args = event.data.value("args", "");
        } else if (event.type == agent_event_type::TOOL_RESULT) {
            std::string name = event.data.value("name", "");
            int64_t ms = event.data.value("duration_ms", 0);
            std::string args_summary = pending_args;
            if (args_summary.length() > 100) {
                args_summary = args_summary.substr(0, 100) + "...";
            }
            display_scope->report_tool_call(name, args_summary, (int)ms);
            std::ostringstream summary;
            summary << name << " (" << ms << "ms)";
            result.tool_calls_summary.push_back(summary.str());
            pending_args.clear();
        }
    };

    agent_loop_result loop_result = subagent.run_streaming(params.prompt, on_event, nullptr, &subagent_perms);

    auto end_time = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    // Collect token stats from the subagent
    const auto & subagent_stats = subagent.get_stats();
    result.input_tokens = subagent_stats.total_input;
    result.output_tokens = subagent_stats.total_output;
    result.cached_tokens = subagent_stats.total_cached;
    int32_t total_tokens = subagent_stats.total_input + subagent_stats.total_output;

    // Accumulate into the parent's session stats (shown by /stats).
    // Subagent tokens are added to the totals so "Total" covers the whole session.
    if (auto * parent_stats = static_cast<session_stats *>(parent_tool_ctx_.session_stats_ptr)) {
        parent_stats->subagent_input += result.input_tokens;
        parent_stats->subagent_output += result.output_tokens;
        parent_stats->subagent_cached += result.cached_tokens;
        parent_stats->subagent_count += 1;
        parent_stats->total_input += result.input_tokens;
        parent_stats->total_output += result.output_tokens;
        parent_stats->total_cached += result.cached_tokens;
    }

    // Report completion with timing and token usage
    display_scope->report_done(static_cast<int>(elapsed_ms), total_tokens);

    // Convert result
    result.iterations = loop_result.iterations;

    switch (loop_result.stop_reason) {
        case agent_stop_reason::COMPLETED:
            result.success = true;
            result.output = loop_result.final_response;
            break;

        case agent_stop_reason::MAX_ITERATIONS:
            result.success = false;
            result.output = loop_result.final_response;
            result.error = "Reached maximum iterations (" + std::to_string(type_config.max_iterations) + ")";
            break;

        case agent_stop_reason::TOKEN_BUDGET_EXCEEDED:
            result.success = false;
            result.output = loop_result.final_response;
            result.error = "Token budget exhausted (max_tokens=" + std::to_string(params.max_tokens) + ")";
            break;

        case agent_stop_reason::USER_CANCELLED:
            result.success = false;
            result.error = "User cancelled";
            break;

        case agent_stop_reason::AGENT_ERROR:
            result.success = false;
            result.error = "Agent error: " + loop_result.final_response;
            break;
    }

    // Per-task stop status (cancel / timeout) vs parent-wide interrupt.
    {
        bool parent_int = parent_tool_ctx_.is_interrupted &&
                          parent_tool_ctx_.is_interrupted->load();
        result.status = subagent_stop_status(interrupt->load(), parent_int, reason->load());
        if (result.status == "cancelled") {
            result.success = false;
            result.error = "Task cancelled";
        } else if (result.status == "timeout") {
            result.success = false;
            result.error = "Task timed out";
        }
    }

    // Backend-assignment warnings become the first line of the task's text
    // result here (the shared point for the sync path and background get_result).
    if (!backend_warning.empty()) {
        result.output = backend_warning + "\n" + result.output;
    }

    // Keep the full output retrievable (resume task_id="..."); hand the parent
    // a capped copy so one verbose subagent cannot blow up the parent context.
    if (!result.output.empty()) {
        {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            full_results_[result.task_id] = result.output;
        }
        result.output = subagent_cap_result(result.output, result.task_id);
    }

    return result;
}

std::string subagent_runner::start_background(const subagent_params & params) {
    std::string task_id = generate_task_id();

    // Create output buffer for this background task
    auto & output_mgr = subagent_output_manager::instance();
    subagent_output_buffer * buffer = output_mgr.create_buffer(task_id);

    auto task = std::make_unique<subagent_task>();
    task->id = task_id;
    task->params = params;

    // Capture what we need for the thread
    auto * task_ptr = task.get();

    // Start background thread with buffer for output
    task->thread = std::thread([this, task_ptr, params, buffer, task_id]() {
        subagent_result result;
        try {
            result = this->run_internal(params, buffer, &task_ptr->cancelled,
                                        &task_ptr->stop_reason, task_id);
        } catch (const std::exception & e) {
            result.success = false;
            result.error = std::string("Exception: ") + e.what();
        }

        // Flush buffered output before completing
        buffer->flush(true);

        task_ptr->promise.set_value(std::move(result));
        task_ptr->complete.store(true);
        {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            done_cv_.notify_all();
        }

        // Cleanup buffer
        subagent_output_manager::instance().remove_buffer(task_id);
    });

    // Store task
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        tasks_[task_id] = std::move(task);
    }

    return task_id;
}

bool subagent_runner::is_complete(const std::string & task_id) const {
    std::lock_guard<std::mutex> lock(tasks_mutex_);

    // Check completed results first
    if (completed_results_.find(task_id) != completed_results_.end()) {
        return true;
    }

    // Check active tasks
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        return false;  // Task doesn't exist
    }

    return it->second->complete.load();
}

subagent_result subagent_runner::get_result(const std::string & task_id) {
    std::thread orphan;
    subagent_result result;
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);

        // Check completed results first
        auto completed_it = completed_results_.find(task_id);
        if (completed_it != completed_results_.end()) {
            return completed_it->second;
        }

        // Check active tasks
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) {
            subagent_result empty;
            empty.error = "Task not found: " + task_id;
            return empty;
        }

        if (!it->second->complete.load()) {
            subagent_result pending;
            pending.error = "Task still running: " + task_id;
            return pending;
        }

        // Get result (the worker already set the promise before signalling
        // complete, so this cannot block while we hold tasks_mutex_)
        try {
            std::future<subagent_result> future = it->second->promise.get_future();
            result = future.get();
        } catch (const std::exception & e) {
            result.success = false;
            result.error = std::string("Failed to get result: ") + e.what();
        }

        // Move the thread handle out and join OUTSIDE the lock: joining under
        // tasks_mutex_ blocked is_complete()/cancel() for every other task
        // while a finishing subagent wound down.
        if (it->second->thread.joinable()) {
            orphan = std::move(it->second->thread);
        }

        // Store in completed results and remove from active
        completed_results_[task_id] = result;
        tasks_.erase(it);
    }

    if (orphan.joinable()) {
        orphan.join();
    }

    return result;
}

void subagent_runner::cancel(const std::string & task_id) {
    std::lock_guard<std::mutex> lock(tasks_mutex_);

    auto it = tasks_.find(task_id);
    if (it != tasks_.end()) {
        // Per-task interrupt: the subagent loop polls (parent || per-task)
        // flags via agent_loop::interrupt_requested(), so this stops only the
        // named task; the parent and sibling tasks keep running.
        it->second->stop_reason.store(SUBAGENT_STOP_CANCEL);
        it->second->cancelled.store(true);
        done_cv_.notify_all();
    }
}

std::vector<std::string> subagent_runner::get_active_tasks() const {
    std::lock_guard<std::mutex> lock(tasks_mutex_);

    std::vector<std::string> active;
    for (const auto & [id, task] : tasks_) {
        if (!task->complete.load()) {
            active.push_back(id);
        }
    }
    return active;
}

void subagent_runner::cleanup_completed() {
    std::vector<std::thread> done_threads;
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        for (auto it = tasks_.begin(); it != tasks_.end(); ) {
            if (it->second->complete.load()) {
                // Move out; join outside the lock (see get_result()).
                if (it->second->thread.joinable()) {
                    done_threads.push_back(std::move(it->second->thread));
                }
                it = tasks_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto & th : done_threads) {
        if (th.joinable()) {
            th.join();
        }
    }
}

bool subagent_runner::wait_complete(const std::string & task_id, int wait_ms) {
    std::unique_lock<std::mutex> lock(tasks_mutex_);
    auto is_done = [&]() {
        if (completed_results_.find(task_id) != completed_results_.end()) {
            return true;  // collected earlier
        }
        auto it = tasks_.find(task_id);
        if (it == tasks_.end()) {
            return true;  // unknown task: let the caller report it immediately
        }
        return it->second->complete.load();
    };
    if (is_done() || wait_ms <= 0) {
        return is_done();
    }
    return done_cv_.wait_for(lock, std::chrono::milliseconds(wait_ms), is_done);
}

std::string subagent_runner::get_full_output(const std::string & task_id, size_t & total_out) {
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    auto it = full_results_.find(task_id);
    if (it == full_results_.end()) {
        total_out = 0;
        return std::string();
    }
    total_out = it->second.size();
    size_t & off = last_resume_offset_[task_id];
    if (off > total_out) {
        off = total_out;
    }
    std::string out = it->second.substr(off);
    off = total_out;  // subsequent calls only see output appended after this one
    return out;
}
