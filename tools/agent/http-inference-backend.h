#pragma once

#include "inference-backend.h"

#include <map>
#include <mutex>
#include <string>

// Pure retry policy for HTTP inference requests: retry on connection-establishment
// failure (conn_error, status == 0), HTTP 429, or 5xx — but never once the stream
// has produced content (got_content), because replaying would duplicate output.
bool agent_http_should_retry(int status, bool conn_error, bool got_content);

struct http_inference_backend_config {
    std::string base_url;
    std::string model;
    std::string api_key;
    std::map<std::string, std::string> headers;
    int timeout_ms = 0;
    bool llama_server_extensions = false;
    bool detect_llama_server = true;
    int32_t id_slot = -1;
};

class http_inference_backend : public inference_backend {
public:
    explicit http_inference_backend(http_inference_backend_config config);

    const inference_backend_meta & meta() const override;

    inference_result complete(
        const inference_request & request,
        std::function<void(const inference_event &)> on_event,
        std::function<bool()> should_stop) override;

    // Accurate token count via llama-server /tokenize. -1 on any error. Caches the
    // last request (length + hash) to avoid repeat round-trips during compaction.
    int count_tokens(const std::string & text) override;

    std::unique_ptr<inference_backend> clone_for_subagent() const override;

private:
    void fetch_props();

    std::string chat_completions_path() const;
    std::string props_path() const;
    std::string tokenize_path() const;
    json build_request_body(const inference_request & request) const;

    void process_sse_payload(
        const std::string & payload,
        const inference_request & request,
        inference_result & result,
        std::string & content,
        std::string & reasoning,
        std::vector<common_chat_tool_call> & native_tool_calls,
        std::function<void(const inference_event &)> emit) const;

    http_inference_backend_config config_;
    inference_backend_meta meta_;
    bool use_llama_server_extensions_ = false;

    // Single-entry cache for count_tokens: (text length, text hash) -> count.
    std::mutex count_cache_mutex_;
    size_t count_cache_len_ = 0;
    size_t count_cache_hash_ = 0;
    int count_cache_value_ = -1;
    bool count_cache_valid_ = false;
};
