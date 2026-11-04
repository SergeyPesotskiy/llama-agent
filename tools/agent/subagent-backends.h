#ifndef SUBAGENT_BACKENDS_H
#define SUBAGENT_BACKENDS_H

#include <mutex>
#include <string>
#include <vector>

// Pure helpers + thread-safe round-robin selector for the pool of inference
// backends available to subagents (--subagent-backends ip:port[,...]).
// Assignment order: explicit task "backend" index -> round-robin over live
// endpoints -> parent backend (empty result). No per-type mapping by design:
// the main agent assigns roles to endpoints at task creation time.
namespace subagent_backends {

// Normalize a user-supplied endpoint: trim spaces, default scheme http://,
// strip trailing '/'. Returns empty string for empty/garbage input.
std::string normalize_backend_url(const std::string & in);

// Parse "--subagent-backends a,b,c" value into a normalized, deduplicated,
// order-preserving list (invalid entries skipped).
std::vector<std::string> parse_backends_list(const std::string & csv);

// Thread-safe round-robin picker over a fixed URL list.
class backend_selector {
public:
    backend_selector() = default;
    explicit backend_selector(std::vector<std::string> urls);

    bool empty() const;
    const std::vector<std::string> & urls() const { return urls_; }

    // Next URL in round-robin order; empty string when no endpoints.
    std::string next_url();

    // Validate explicit task index (from the "backend" tool argument).
    // Returns normalized URL or empty string when out of range.
    std::string url_at(size_t index) const;

private:
    std::vector<std::string> urls_;
    mutable std::mutex mutex_;
    size_t next_ = 0;
};

} // namespace subagent_backends

#endif // SUBAGENT_BACKENDS_H
