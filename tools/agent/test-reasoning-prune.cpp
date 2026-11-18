#include "reasoning-prune.h"

#include "agent-loop.h"

#include <cassert>
#include <cstdio>

int main() {
    // Current turn (after the last user message): reasoning stays verbatim,
    // the model must see its own notes between tool calls.
    {
        nlohmann::ordered_json messages = {
            {
                {"role", "user"},
                {"content", "check the repo"},
            },
            {
                {"role", "assistant"},
                {"content", ""},
                {"reasoning_content", "plan: read first"},
                {"tool_calls", {
                    {
                        {"id", "c1"},
                        {"type", "function"},
                        {"function", {{"name", "read"}, {"arguments", "{}"}}},
                    },
                }},
            },
            {
                {"role", "tool"},
                {"tool_call_id", "c1"},
                {"content", "file contents"},
            },
            {
                {"role", "assistant"},
                {"content", "done"},
                {"reasoning_content", "wrap up"},
            },
        };
        nlohmann::ordered_json expected = messages;
        prune_inter_turn_reasoning(messages);
        assert(messages == expected);
    }

    // Previous turns: reasoning fields and <think> blocks are stripped; the
    // text of ordinary answers and tool messages is untouched.
    {
        nlohmann::ordered_json messages = {
            {
                {"role", "system"},
                {"content", "<think> system mentions think, keep it </think>"},
            },
            {
                {"role", "user"},
                {"content", "old task"},
            },
            {
                {"role", "assistant"},
                {"content", "<think>hidden draft</think>visible answer"},
                {"reasoning_content", "draft notes"},
            },
            {
                {"role", "tool"},
                {"tool_call_id", "c1"},
                {"content", "result"},
            },
            {
                {"role", "user"},
                {"content", "new task"},
            },
            {
                {"role", "assistant"},
                {"content", "answer"},
                {"reasoning_content", "current thinking"},
            },
        };
        prune_inter_turn_reasoning(messages);
        assert(!messages[2].contains("reasoning_content"));
        assert(messages[2]["content"].get<std::string>() == "visible answer");
        // system message is not an assistant turn: content untouched
        assert(messages[0]["content"].get<std::string>() == "<think> system mentions think, keep it </think>");
        // tool result untouched
        assert(messages[3]["content"].get<std::string>() == "result");
        // current turn fully intact
        assert(messages[5]["reasoning_content"].get<std::string>() == "current thinking");
        assert(messages[5]["content"].get<std::string>() == "answer");
    }

    // Multimodal content parts: strip inside text parts of old assistant turns.
    {
        nlohmann::ordered_json messages = {
            {
                {"role", "user"},
                {"content", {
                    {{"type", "text"}, {"text", "look"}},
                }},
            },
            {
                {"role", "assistant"},
                {"content", {
                    {{"type", "text"}, {"text", "<think>draft</think>real text"}},
                    {{"type", "image_url"}, {"image_url", {{"url", "x"}}}},
                }},
            },
            {
                {"role", "user"},
                {"content", "next"},
            },
        };
        prune_inter_turn_reasoning(messages);
        assert(messages[1]["content"][0]["text"].get<std::string>() == "real text");
        assert(messages[1]["content"][1].contains("image_url"));
    }

    // Dangling (unclosed) <think> is left alone — only complete pairs are
    // safe to remove.
    {
        nlohmann::ordered_json messages = {
            {
                {"role", "user"},
                {"content", "a"},
            },
            {
                {"role", "assistant"},
                {"content", "<think>oops no closing"},
            },
            {
                {"role", "user"},
                {"content", "b"},
            },
        };
        nlohmann::ordered_json expected = messages;
        prune_inter_turn_reasoning(messages);
        assert(messages == expected);
    }

    // No user message at all: nothing is treated as a past turn.
    {
        nlohmann::ordered_json messages = {
            {
                {"role", "system"},
                {"content", "sys"},
            },
            {
                {"role", "assistant"},
                {"content", "hi"},
                {"reasoning_content", "thinking"},
            },
        };
        nlohmann::ordered_json expected = messages;
        prune_inter_turn_reasoning(messages);
        assert(messages == expected);
    }

    // Empty and non-array inputs are safe no-ops.
    {
        nlohmann::ordered_json messages = json::array();
        prune_inter_turn_reasoning(messages);
        assert(messages.empty());
        nlohmann::ordered_json broken = json::object();
        prune_inter_turn_reasoning(broken);
        assert(broken.is_object());
    }

    // Pruning is enabled by default; storage and separation are opt-in.
    {
        agent_config config;
        assert(config.reasoning_prune);
        assert(!config.store_reasoning);
        assert(!config.keep_reasoning_separate);
    }

    printf("OK: inter-turn reasoning pruning\n");
    return 0;
}
