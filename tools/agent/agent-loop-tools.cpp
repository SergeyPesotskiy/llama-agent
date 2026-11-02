#include "agent-loop.h"
#include "utf8-sanitize.h"
#include "console.h"
#include "terminal-image.h"
#include "../cli/cli-ui.h"

#include <chrono>
#include <filesystem>
#include <functional>

// Permission/prompt phase of execute_tool_call_async for the streaming API.
// Always runs on the loop thread: it may block on user prompts and mutates the
// async permission manager state. The execution phase (run_prepared_tool_call)
// is stateless and safe to move to a worker thread for the concurrent task
// batch.
agent_loop::prepared_tool_call agent_loop::prepare_tool_call_async(
    const common_chat_tool_call & call,
    agent_event_callback on_event,
    permission_manager_async & async_perms,
    std::function<bool()> should_stop) {

    prepared_tool_call p;
    auto & registry = tool_registry::instance();

    // Check if tool exists
    const tool_def * tool = registry.get_tool(call.name);
    if (!tool) {
        p.early = {false, "", "Unknown tool: " + call.name};
        return p;
    }

    // Parse arguments (retry after UTF-8 sanitize: the model may echo legacy
    // encoding bytes it saw in tool output into the argument string)
    json args;
    try {
        args = json::parse(call.arguments);
    } catch (const json::parse_error & e) {
        auto san = utf8_sanitize(call.arguments);
        try {
            args = json::parse(san.text);
        } catch (const json::parse_error & e2) {
            p.early = {false, "", std::string("Invalid JSON arguments: ") + e2.what()};
            return p;
        }
    }

    // Determine permission type
    permission_type ptype = permission_type::BASH;
    if (call.name == "read") {
        ptype = permission_type::FILE_READ;
    } else if (call.name == "write") {
        ptype = permission_type::FILE_WRITE;
    } else if (call.name == "edit") {
        ptype = permission_type::FILE_EDIT;
    } else if (call.name == "update_plan") {
        ptype = permission_type::GLOB;
    }

    // Build permission request
    permission_request req;
    req.type = ptype;
    req.tool_name = call.name;
    req.details = call.arguments;

    // Check for external directory access on file operations
    if (call.name == "read" || call.name == "write" || call.name == "edit") {
        std::string file_path = args.value("file_path", "");
        if (!file_path.empty()) {
            std::filesystem::path path(file_path);
            if (path.is_relative()) {
                path = std::filesystem::path(tool_ctx_.working_dir) / path;
            }
            // In yolo mode all permissions are auto-approved, so skip the
            // external-dir prompt (it would otherwise block in non-TUI runs).
            if (async_perms.is_external_path(path.string()) && !async_perms.is_yolo()) {
                permission_request ext_req;
                ext_req.type = permission_type::EXTERNAL_DIR;
                ext_req.tool_name = call.name;
                ext_req.details = "External file: " + path.string();
                ext_req.is_dangerous = true;
                ext_req.description = "Operation outside working directory";

                // Request permission asynchronously
                std::string req_id = async_perms.request_permission(ext_req);
                on_event(agent_event::permission_required(
                    req_id, call.name, ext_req.description, ext_req.details, true));

                // Wait for response (cancellable via should_stop)
                auto response = async_perms.wait_for_response_or_stop(req_id, 300000, should_stop);
                if (should_stop()) {
                    on_event(agent_event::permission_resolved(req_id, false));
                    p.early = {false, "", "Operation cancelled"};
                    return p;
                }
                if (!response || !response->allowed) {
                    on_event(agent_event::permission_resolved(req_id, false));
                    p.early = {false, "", "Blocked: File is outside working directory"};
                    return p;
                }
                on_event(agent_event::permission_resolved(req_id, true));
            }
        }
    }

    // Check for dangerous commands
    if (call.name == "bash") {
        std::string cmd = args.value("command", "");
        req.details = cmd;
        req.is_dangerous = async_perms.is_dangerous_bash_command(cmd);
    }

    // Check doom loop
    std::hash<std::string> hasher;
    std::string args_hash = std::to_string(hasher(call.arguments));
    // In yolo mode all permissions are auto-approved (no blocking prompt).
    if (async_perms.is_doom_loop(call.name, args_hash) && !async_perms.is_yolo()) {
        req.description = "Detected repeated identical tool calls (doom loop)";

        std::string req_id = async_perms.request_permission(req);
        on_event(agent_event::permission_required(req_id, call.name, req.description, req.details, true));

        auto response = async_perms.wait_for_response_or_stop(req_id, 300000, should_stop);
        if (should_stop()) {
            on_event(agent_event::permission_resolved(req_id, false));
            p.early = {false, "", "Operation cancelled"};
            return p;
        }
        if (!response || !response->allowed) {
            on_event(agent_event::permission_resolved(req_id, false));
            p.early = {false, "", "Blocked: Detected repeated identical tool calls"};
            return p;
        }
        on_event(agent_event::permission_resolved(req_id, true));
    }

    // Check permission
    permission_state state = async_perms.check_permission(req);
    if (state == permission_state::DENY || state == permission_state::DENY_SESSION) {
        p.early = {false, "", "Permission denied for " + call.name};
        return p;
    }

    if (state == permission_state::ASK) {
        // Request permission asynchronously
        std::string req_id = async_perms.request_permission(req);
        on_event(agent_event::permission_required(
            req_id, call.name, req.description, req.details, req.is_dangerous));

        // Wait for response (cancellable via should_stop)
        auto response = async_perms.wait_for_response_or_stop(req_id, 300000, should_stop);

        if (should_stop()) {
            on_event(agent_event::permission_resolved(req_id, false));
            p.early = {false, "", "Operation cancelled"};
            return p;
        }

        if (!response) {
            on_event(agent_event::permission_resolved(req_id, false));
            p.early = {false, "", "Permission request timed out"};
            return p;
        }

        on_event(agent_event::permission_resolved(req_id, response->allowed));

        if (!response->allowed) {
            p.early = {false, "", "User denied permission for " + call.name};
            return p;
        }
    }

    // Record this call
    async_perms.record_tool_call(call.name, args_hash);

    p.proceed = true;
    p.args = std::move(args);
    return p;
}

tool_result agent_loop::run_prepared_tool_call(const common_chat_tool_call & call,
                                               const json & args,
                                               bool quiet_display) {
    auto & registry = tool_registry::instance();

    // Build a short argument summary for display / subagent reporting
    std::string args_summary;
    if (call.name == "bash") {
        args_summary = args.value("command", "");
    } else if (call.name == "read" || call.name == "write" || call.name == "edit") {
        args_summary = args.value("path", args.value("file_path", ""));
    } else {
        args_summary = call.arguments;
    }
    if (args_summary.length() > 100) {
        args_summary = args_summary.substr(0, 100) + "...";
    }

    // Display tool execution (main agent only; subagents report via on_tool_call_)
    const bool show_header = !is_subagent_ && !quiet_display;
    if (show_header) {
        console::set_display(DISPLAY_TYPE_INFO);
        console::log("\n› %s %s", call.name.c_str(), args_summary.c_str());
        console::spinner::start();
        console::set_display(DISPLAY_TYPE_RESET);
    }

    // Execute the tool with timing (subagents: filtered tool set + bash patterns)
    auto start_time = std::chrono::steady_clock::now();
    tool_result result = registry.execute_filtered(call.name, args, tool_ctx_, bash_patterns_);
    auto end_time = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    if (show_header) {
        console::spinner::stop();
        console::log("\n");
    } else if (on_tool_call_) {
        // Report this tool call to the parent display
        on_tool_call_(call.name, args_summary, (int)elapsed_ms);
    }

    // Display result summary (main agent only)
    if (is_subagent_ || quiet_display) {
        return result;
    }
    if (result.success) {
        // Truncate long output for display (unless the tool opted out for rich,
        // pre-formatted output that must not be cut mid-escape/mid-codepoint).
        std::string display_output = result.output;
        if (!result.no_truncate_display && display_output.length() > 500) {
            display_output = display_output.substr(0, 500) + "\n... (truncated)";
        }
        console::log("%s\n", display_output.c_str());
        if (!result.image_bytes.empty()) {
            render_image_to_terminal(result.image_bytes.data(), result.image_bytes.size(),
                                     result.image_mime);
        }
    } else {
        // Show output if available (e.g., bash stderr), plus error if set
        if (!result.output.empty()) {
            std::string display_output = result.output;
            if (display_output.length() > 500) {
                display_output = display_output.substr(0, 500) + "\n... (truncated)";
            }
            console::error("%s\n", display_output.c_str());
        }
        if (!result.error.empty()) {
            console::error("Error: %s\n", result.error.c_str());
        }
        if (result.output.empty() && result.error.empty()) {
            console::error("Error: Tool failed with no output\n");
        }
    }

    // Display elapsed time
    console::set_display(DISPLAY_TYPE_INFO);
    if (elapsed_ms < 1000) {
        console::log("└─ %lldms\n", (long long)elapsed_ms);
    } else {
        console::log("└─ %.1fs\n", elapsed_ms / 1000.0);
    }
    console::set_display(DISPLAY_TYPE_RESET);

    return result;
}


// Async version of execute_tool_call for API use
// Uses async permission manager and emits events instead of blocking on console
tool_result agent_loop::execute_tool_call_async(
    const common_chat_tool_call & call,
    agent_event_callback on_event,
    permission_manager_async & async_perms,
    std::function<bool()> should_stop) {

    prepared_tool_call p = prepare_tool_call_async(call, on_event, async_perms, should_stop);
    if (!p.proceed) {
        return p.early;
    }
    return run_prepared_tool_call(call, p.args);
}
