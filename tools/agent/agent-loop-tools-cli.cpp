#include "agent-loop.h"
#include "console.h"
#include "terminal-image.h"
#include "utf8-sanitize.h"

#include <chrono>
#include <filesystem>
#include <utility>

static tool_result tool_error(std::string message) {
    tool_result result;
    result.success = false;
    result.error = std::move(message);
    return result;
}

// Permission/prompt phase of execute_tool_call (blocking console prompts run
// on the loop thread only). The execution phase is factored out into
// run_prepared_tool_call so the concurrent task batch can move it to a worker.
agent_loop::prepared_tool_call agent_loop::prepare_tool_call(const common_chat_tool_call & call) {
    prepared_tool_call p;
    auto & registry = tool_registry::instance();

    // Check if tool exists
    const tool_def * tool = registry.get_tool(call.name);
    if (!tool) {
        p.early = tool_error("Unknown tool: " + call.name);
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
            p.early = tool_error(std::string("Invalid JSON arguments: ") + e2.what());
            return p;
        }
    }
    p.args = args;

    // Determine permission type
    permission_type ptype = permission_type::BASH;
    if (call.name == "read") ptype = permission_type::FILE_READ;
    else if (call.name == "write") ptype = permission_type::FILE_WRITE;
    else if (call.name == "edit") ptype = permission_type::FILE_EDIT;
    else if (call.name == "update_plan") ptype = permission_type::GLOB;

    // Build permission request
    permission_request req;
    req.type = ptype;
    req.tool_name = call.name;
    req.details = call.arguments;

    // Check for external directory access on file operations
    if (call.name == "read" || call.name == "write" || call.name == "edit") {
        std::string file_path = args.value("file_path", "");
        if (!file_path.empty()) {
            // Make path absolute for comparison
            std::filesystem::path path(file_path);
            if (path.is_relative()) {
                path = std::filesystem::path(tool_ctx_.working_dir) / path;
            }
            if (permission_mgr_.is_external_path(path.string())) {
                permission_request ext_req;
                ext_req.type = permission_type::EXTERNAL_DIR;
                ext_req.tool_name = call.name;
                ext_req.details = "External file: " + path.string();
                ext_req.is_dangerous = true;
                ext_req.description = "Operation outside working directory";

                auto response = permission_mgr_.prompt_user(ext_req);
                if (response == permission_response::DENY_ONCE ||
                    response == permission_response::DENY_ALWAYS) {
                    p.early = tool_error("Blocked: File is outside working directory");
                    return p;
                }
            }
        }
    }

    // Check for dangerous commands
    if (call.name == "bash") {
        std::string cmd = args.value("command", "");
        req.details = cmd;
        req.is_dangerous = permission_mgr_.is_dangerous_bash_command(cmd);
    }

    // Check doom loop
    std::hash<std::string> hasher;
    std::string args_hash = std::to_string(hasher(call.arguments));
    if (permission_mgr_.is_doom_loop(call.name, args_hash)) {
        req.description = "Detected repeated identical tool calls (doom loop)";
        auto response = permission_mgr_.prompt_user(req);
        if (response == permission_response::DENY_ONCE ||
            response == permission_response::DENY_ALWAYS) {
            p.early = tool_error("Blocked: Detected repeated identical tool calls");
            return p;
        }
    }

    // Check permission
    permission_state state = permission_mgr_.check_permission(req);
    if (state == permission_state::DENY || state == permission_state::DENY_SESSION) {
        p.early = tool_error("Permission denied for " + call.name);
        return p;
    }

    if (state == permission_state::ASK) {
        auto response = permission_mgr_.prompt_user(req);
        if (response == permission_response::DENY_ONCE ||
            response == permission_response::DENY_ALWAYS) {
            p.early = tool_error("User denied permission for " + call.name);
            return p;
        }
    }

    // Record this call
    permission_mgr_.record_tool_call(call.name, args_hash);

    p.proceed = true;
    return p;
}

// Execution + display phase. quiet_display suppresses the console header/result
// echo (used for concurrently executed task calls; the subagent display

// Sequential path: permission phase + execution phase, exactly as before.
tool_result agent_loop::execute_tool_call(const common_chat_tool_call & call) {
    prepared_tool_call p = prepare_tool_call(call);
    if (!p.proceed) {
        return p.early;
    }
    return run_prepared_tool_call(call, p.args);
}
