#include "subagent-backends.h"

#include <algorithm>
#include <cctype>

namespace subagent_backends {

namespace {

std::string trim(const std::string & s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

} // namespace

std::string normalize_backend_url(const std::string & in) {
    std::string s = trim(in);
    if (s.empty()) return {};
    // already has a scheme -> keep as-is (lowercased scheme part is left alone)
    if (s.find("://") == std::string::npos) {
        s = "http://" + s;
    }
    while (!s.empty() && s.back() == '/') s.pop_back();
    // minimal sanity: must have a host part
    size_t p = s.find("://");
    std::string host = s.substr(p + 3);
    if (host.empty() || host.find_first_of(" \t") != std::string::npos) {
        return {};
    }
    return s;
}

std::vector<std::string> parse_backends_list(const std::string & csv) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= csv.size()) {
        size_t comma = csv.find(',', start);
        std::string part = csv.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        std::string url = normalize_backend_url(part);
        if (!url.empty() && std::find(out.begin(), out.end(), url) == out.end()) {
            out.push_back(url);
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

backend_selector::backend_selector(std::vector<std::string> urls) : urls_(std::move(urls)) {
    for (std::string & u : urls_) {
        u = normalize_backend_url(u);
    }
    urls_.erase(std::remove_if(urls_.begin(), urls_.end(),
                               [](const std::string & s) { return s.empty(); }),
                urls_.end());
    urls_.erase(std::unique(urls_.begin(), urls_.end()), urls_.end());
}

bool backend_selector::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return urls_.empty();
}

std::string backend_selector::next_url() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (urls_.empty()) return {};
    std::string url = urls_[next_ % urls_.size()];
    next_ = (next_ + 1) % urls_.size();
    return url;
}

std::string backend_selector::url_at(size_t index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= urls_.size()) return {};
    return urls_[index];
}

backend_pool::backend_pool(inference_backend * parent) : parent_(parent) {
}

inference_backend * backend_pool::get_or_create(const std::string & url, inference_backend * parent) {
    if (url.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pool_.find(url);
    if (it != pool_.end()) {
        return it->second.get();
    }
    inference_backend * src = parent ? parent : parent_;
    if (!src) return nullptr;
    std::unique_ptr<inference_backend> clone;
    try {
        clone = src->clone_with_base_url(url);
    } catch (...) {
        clone.reset();
    }
    if (!clone) return nullptr;  // failures are not cached (endpoint may recover)
    inference_backend * raw = clone.get();
    pool_[url] = std::move(clone);
    return raw;
}

} // namespace subagent_backends
