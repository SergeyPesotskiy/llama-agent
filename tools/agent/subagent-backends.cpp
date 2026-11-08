#include "subagent-backends.h"

#include <algorithm>
#include <cctype>
#include <climits>

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

// Pick the endpoint with the fewest in-flight leases; ties broken by the
// round-robin cursor so idle-but-untouched endpoints get used fairly.
backend_lease backend_selector::acquire_lease() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (urls_.empty()) return {};
    size_t best = next_ % urls_.size();
    int best_load = INT_MAX;
    for (size_t i = 0; i < urls_.size(); i++) {
        size_t cand = (next_ + i) % urls_.size();
        int load = in_flight_[urls_[cand]];
        if (load < best_load) {
            best_load = load;
            best = cand;
        }
    }
    next_ = (best + 1) % urls_.size();
    const std::string & url = urls_[best];
    in_flight_[url]++;
    return backend_lease(this, url);
}

backend_lease backend_selector::acquire_lease_at(size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= urls_.size()) return {};
    const std::string & url = urls_[index];
    in_flight_[url]++;
    return backend_lease(this, url);
}

void backend_selector::release(const std::string & url) {
    if (url.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = in_flight_.find(url);
    if (it != in_flight_.end() && it->second > 0) it->second--;
}

int backend_selector::inflight(const std::string & url) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = in_flight_.find(url);
    return it == in_flight_.end() ? 0 : it->second;
}

backend_lease::backend_lease(backend_selector * sel, std::string url)
    : sel_(sel), url_(std::move(url)) {
    if (url_.empty()) sel_ = nullptr;
}

backend_lease::backend_lease(backend_lease && o) noexcept
    : sel_(o.sel_), url_(std::move(o.url_)) {
    o.sel_ = nullptr;
}

backend_lease & backend_lease::operator=(backend_lease && o) noexcept {
    if (this != &o) {
        release();
        sel_ = o.sel_;
        url_ = std::move(o.url_);
        o.sel_ = nullptr;
    }
    return *this;
}

backend_lease::~backend_lease() {
    release();
}

void backend_lease::release() {
    if (sel_) {
        sel_->release(url_);
        sel_ = nullptr;
        url_.clear();
    }
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
