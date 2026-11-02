#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>

using json = nlohmann::ordered_json;

struct compaction_settings {
    bool    enabled            = true;
    int32_t reserve_tokens     = 16384;  // trigger when ctx_used > ctx_size - reserve
    int32_t keep_recent_tokens = 20000;  // recent tokens NOT summarized
};

// Optional exact tokenizer: returns the token count for a text fragment, or -1
// when unknown (the estimation helpers then fall back to the chars/4 heuristic).
using compaction_tokenizer = std::function<int(const std::string &)>;

// Estimate token count for a single OAI-compat JSON message (chars/4 heuristic,
// or exact counts via tokenizer when it is provided and returns >= 0)
int32_t compaction_estimate_tokens(const json & msg,
                                   const compaction_tokenizer & tokenizer = nullptr);

// Sum estimated tokens across all messages
int32_t compaction_estimate_total_tokens(const json & messages,
                                         const compaction_tokenizer & tokenizer = nullptr);

// Find the index of the first message to KEEP (everything before it gets summarized).
// Returns >= 1 (never cuts the system prompt at index 0).
// Snaps to user message boundaries — never cuts mid-turn.
size_t compaction_find_cut_point(const json & messages, int32_t keep_recent_tokens,
                                 const compaction_tokenizer & tokenizer = nullptr);

// Serialize messages[start..end) into text for the summarization prompt.
// Tool results are truncated to 2000 chars.
std::string compaction_serialize_conversation(const json & messages, size_t start, size_t end);

// Prompt constants
extern const char * SUMMARIZATION_SYSTEM_PROMPT;
extern const char * SUMMARIZATION_PROMPT;
extern const char * UPDATE_SUMMARIZATION_PROMPT;
