// Unit tests for subagent_backends: normalize_backend_url / parse_backends_list /
// backend_selector (round-robin) / backend_pool (clone, cache-by-URL, dead endpoint
// returns nullptr and is NOT cached), against in-process mock llama-server endpoints
// (the backend ctor probe only needs GET /props).
#include "subagent-backends.h"
#include "http-inference-backend.h"

#include <cpp-httplib/httplib.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failures++;                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                  \
    } while (0)

static int start_mock_server(httplib::Server & server, const std::string & model_alias) {
    server.Get("/props", [model_alias](const httplib::Request &, httplib::Response & res) {
        res.set_content(json{
            {"model_alias", model_alias},
            {"total_slots", 1},
            {"default_generation_settings", {{"n_ctx", 2048}}},
        }.dump(), "application/json");
    });
    return server.bind_to_any_port("127.0.0.1");
}

int main() {
    using namespace subagent_backends;

    // --- pure helpers --------------------------------------------------------------

    // normalize_backend_url: default scheme, trim, strip trailing '/', reject garbage
    CHECK(normalize_backend_url("1.2.3.4:80") == "http://1.2.3.4:80");
    CHECK(normalize_backend_url("http://h:1/") == "http://h:1");
    CHECK(normalize_backend_url("") == "");
    CHECK(normalize_backend_url("   ") == "");
    CHECK(normalize_backend_url("a b") == "");
    CHECK(normalize_backend_url(" https://h:443// ") == "https://h:443");
    CHECK(normalize_backend_url("http://h:1/path") == "http://h:1/path");

    // parse_backends_list: trims, dedupes, skips empties, keeps order
    auto urls = parse_backends_list("a:1, b:2 ,a:1,,x");
    CHECK(urls.size() == 3);
    if (urls.size() == 3) {
        CHECK(urls[0] == "http://a:1");
        CHECK(urls[1] == "http://b:2");
        CHECK(urls[2] == "http://x");
    }
    CHECK(parse_backends_list("").empty());

    // backend_selector: explicit index validation + round-robin order
    backend_selector none_sel;
    CHECK(none_sel.empty());
    CHECK(none_sel.next_url() == "");
    CHECK(none_sel.url_at(0) == "");

    backend_selector sel({ "a:1", "http://b:2" });
    CHECK(!sel.empty());
    CHECK(sel.urls().size() == 2);
    CHECK(sel.url_at(0) == "http://a:1");
    CHECK(sel.url_at(1) == "http://b:2");
    CHECK(sel.url_at(99) == "");
    CHECK(sel.next_url() == "http://a:1");
    CHECK(sel.next_url() == "http://b:2");
    CHECK(sel.next_url() == "http://a:1");

    // --- pool / real backends against two mock llama-servers -----------------------

    httplib::Server server_a, server_b;
    int port_a = start_mock_server(server_a, "brain-A");
    int port_b = start_mock_server(server_b, "brain-B");
    CHECK(port_a > 0);
    CHECK(port_b > 0);
    std::thread th_a([&]() { server_a.listen_after_bind(); });
    std::thread th_b([&]() { server_b.listen_after_bind(); });

    const std::string url_a   = "http://127.0.0.1:" + std::to_string(port_a);
    const std::string url_b   = "http://127.0.0.1:" + std::to_string(port_b);
    const std::string url_dead = "http://127.0.0.1:1";  // nothing listens -> instant refusal

    http_inference_backend_config pcfg;
    pcfg.base_url = url_a;
    http_inference_backend parent(pcfg);
    CHECK(parent.meta().is_llama_server);
    CHECK(parent.meta().model_name == "brain-A");

    // clone_with_base_url rebinds to the other endpoint (distinct model via its /props)
    auto clone_b = parent.clone_with_base_url(url_b);
    CHECK(clone_b != nullptr);
    if (clone_b) {
        CHECK(clone_b->meta().is_llama_server);
        CHECK(clone_b->meta().model_name == "brain-B");
    }
    clone_b.reset();

    backend_pool pool(&parent);
    CHECK(pool.get_or_create("", nullptr) == nullptr);

    auto * b1 = pool.get_or_create(url_b, nullptr);
    CHECK(b1 != nullptr);
    auto * b2 = pool.get_or_create(url_b, nullptr);
    CHECK(b1 == b2);  // cached by URL: second call must return the same instance
    if (b1) {
        CHECK(b1->meta().model_name == "brain-B");
    }
    auto * ba = pool.get_or_create(url_a, nullptr);
    CHECK(ba != nullptr);
    CHECK(ba != b1);
    if (ba) {
        CHECK(ba->meta().model_name == "brain-A");  // distinct endpoints stay distinct
    }

    // explicit parent argument works even for a pool without a default parent
    backend_pool orphan(nullptr);
    CHECK(orphan.get_or_create(url_b, nullptr) == nullptr);
    CHECK(orphan.get_or_create(url_b, &parent) != nullptr);

    // dead endpoint: nullptr, fast (ctor probe must not hang on timeout),
    // and NOT cached -> every call retries (a recovered endpoint is usable again)
    auto t0 = std::chrono::steady_clock::now();
    CHECK(pool.get_or_create(url_dead, nullptr) == nullptr);
    CHECK(pool.get_or_create(url_dead, nullptr) == nullptr);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    CHECK(elapsed_ms < 5000);

    server_a.stop();
    server_b.stop();
    if (th_a.joinable()) th_a.join();
    if (th_b.joinable()) th_b.join();

    printf("subagent-backends: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
