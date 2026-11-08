#pragma once

#include "chat.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using json = nlohmann::ordered_json;

struct inference_backend_meta {
    std::string model_name;
    std::string build_info;
    int32_t n_ctx = 0;
    bool has_vision = false;
    bool has_audio = false;
    bool image_support_known = true;
    bool is_llama_server = false;
    int32_t total_slots = 0;
};

struct inference_timings {
    int32_t cache_n = -1;

    int32_t prompt_n = -1;
    double prompt_ms = 0.0;
    double prompt_per_token_ms = 0.0;
    double prompt_per_second = 0.0;

    int32_t predicted_n = -1;
    double predicted_ms = 0.0;
    double predicted_per_token_ms = 0.0;
    double predicted_per_second = 0.0;

    int32_t draft_n = 0;
    int32_t draft_n_accepted = 0;
};

struct inference_prompt_progress {
    int32_t total = 0;
    int32_t cache = 0;
    int32_t processed = 0;
    int64_t time_ms = 0;
};

struct inference_request {
    json messages;
    std::vector<common_chat_tool> tools;

    int32_t n_predict = -1;
    int32_t n_keep = 0;
    bool stream = true;
    bool timings_per_token = true;

    bool cache_prompt = true;
    bool return_progress = true;
    int32_t id_slot = -1;

    bool parse_tool_calls = true;
    bool summary = false;

    // Chat-template thinking mode (Qwen3-style enable_thinking). True keeps the
    // server default; false sends chat_template_kwargs {"enable_thinking": false}
    // so extraction-style calls answer directly instead of filling max_tokens
    // with reasoning. Ignored by servers without chat_template_kwargs support.
    bool enable_thinking = true;
};

enum class inference_event_type {
    TEXT_DELTA,
    REASONING_DELTA,
    TOOL_CALL_DELTA,
    PROMPT_PROGRESS,
    ERROR,
};

struct inference_event {
    inference_event_type type = inference_event_type::TEXT_DELTA;
    common_chat_msg_diff diff;
    inference_prompt_progress progress;
    std::string error;
    json data;
};

struct inference_result {
    common_chat_msg message;
    inference_timings timings;
    int32_t prompt_tokens = 0;
    int32_t cached_prompt_tokens = 0;
    bool context_overflow = false;
    bool cancelled = false;
    std::string error;
};

class inference_backend {
public:
    virtual ~inference_backend() = default;

    virtual const inference_backend_meta & meta() const = 0;

    virtual inference_result complete(
        const inference_request & request,
        std::function<void(const inference_event &)> on_event,
        std::function<bool()> should_stop) = 0;

    // Accurate token count for a text fragment. -1 = unknown / not supported;
    // callers must fall back to a heuristic. Default impl keeps mocks compiling.
    virtual int count_tokens(const std::string & text) {
        (void) text;
        return -1;
    }

    // Clone for subagent use (--subagent-own-slot): returns a fresh backend that
    // will be routed to a free server slot (id_slot = -1). nullptr = not supported.
    virtual std::unique_ptr<inference_backend> clone_for_subagent() const { return nullptr; }

    // Clone bound to a different base URL (external endpoint, --subagent-backends).
    // Overridden by http backends; nullptr = not supported / endpoint not reachable.
    virtual std::unique_ptr<inference_backend> clone_with_base_url(const std::string & url) const {
        (void) url;
        return nullptr;
    }
};
