#include "reasoning-prune.h"

#include <cstddef>
#include <string>

namespace {

// Removes every complete <think>...</think> block (case-insensitive tag).
// Only well-formed pairs are removed; a dangling tag is left untouched.
void strip_think_blocks(std::string & text) {
    const std::string open_tag = "<think>";
    const std::string close_tag = "</think>";
    size_t from = 0;
    while (true) {
        size_t start = text.find(open_tag, from);
        if (start == std::string::npos) {
            break;
        }
        size_t body_start = start + open_tag.size();
        size_t end = text.find(close_tag, body_start);
        if (end == std::string::npos) {
            break;
        }
        text.erase(start, end + close_tag.size() - start);
        from = start;
    }
}

void strip_message(nlohmann::ordered_json & message) {
    if (!message.is_object() || message.value("role", "") != "assistant") {
        return;
    }
    message.erase("reasoning_content");
    if (!message.contains("content")) {
        return;
    }
    nlohmann::ordered_json & content = message["content"];
    if (content.is_string()) {
        std::string text = content.get<std::string>();
        strip_think_blocks(text);
        content = std::move(text);
    } else if (content.is_array()) {
        for (auto & part : content) {
            if (part.is_object() && part.contains("text") && part["text"].is_string()) {
                std::string text = part["text"].get<std::string>();
                strip_think_blocks(text);
                part["text"] = std::move(text);
            }
        }
    }
}

} // namespace

void prune_inter_turn_reasoning(nlohmann::ordered_json & messages) {
    if (!messages.is_array()) {
        return;
    }
    // The current turn starts after the last user message: earlier assistant
    // turns are pruned; the tail (this turn's thinking + tool hops) is kept.
    size_t last_user = messages.size();
    for (size_t i = messages.size(); i > 0; --i) {
        const nlohmann::ordered_json & message = messages[i - 1];
        if (message.is_object() && message.value("role", "") == "user") {
            last_user = i - 1;
            break;
        }
    }
    for (size_t i = 0; i < last_user && i < messages.size(); ++i) {
        strip_message(messages[i]);
    }
}
