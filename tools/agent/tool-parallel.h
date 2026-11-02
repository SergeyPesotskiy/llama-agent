#pragma once

#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
#include <vector>

// Small bounded worker pool used to run multiple `task` tool calls from one
// assistant message concurrently (capped at max_threads worker threads).
//
// Order guarantee: each job is expected to write its own result into
// caller-owned per-index storage, so result order in the conversation is the
// submission order regardless of completion order. wait_all() rethrows the
// first job exception. Pure logic (std only) — unit-tested in
// test-agent-subagent-limits with fake jobs.
class parallel_job_runner {
public:
    explicit parallel_job_runner(size_t max_threads = 4);

    ~parallel_job_runner();

    parallel_job_runner(const parallel_job_runner &) = delete;
    parallel_job_runner & operator=(const parallel_job_runner &) = delete;

    // Queue a job. Spawns a worker while the pool is below max_threads;
    // otherwise queued jobs are picked up by the existing workers.
    void enqueue(std::function<void()> job);

    // Block until every queued job has run and all workers have exited.
    // Rethrows the first exception thrown by any job.
    void wait_all();

private:
    void worker_loop(size_t generation);

    const size_t max_threads_;
    size_t generation_ = 0;  // bumped by wait_all to retire current workers

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    std::vector<std::thread> workers_;
    std::exception_ptr first_error_;
};
