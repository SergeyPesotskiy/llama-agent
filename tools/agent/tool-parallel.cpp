#include "tool-parallel.h"

parallel_job_runner::parallel_job_runner(size_t max_threads)
    : max_threads_(max_threads == 0 ? 1 : max_threads) {
}

parallel_job_runner::~parallel_job_runner() {
    try {
        wait_all();
    } catch (...) {
        // Swallow job exceptions in the destructor; the loop calls wait_all()
        // explicitly and sees them there.
    }
}

void parallel_job_runner::enqueue(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.emplace_back(std::move(job));
        if (workers_.size() < max_threads_) {
            workers_.emplace_back([this, gen = generation_] { worker_loop(gen); });
        }
    }
    cv_.notify_one();
}

void parallel_job_runner::wait_all() {
    std::vector<std::thread> to_join;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;  // current workers exit once the queue drains
    }
    cv_.notify_all();
    to_join.swap(workers_);
    for (auto & t : to_join) {
        if (t.joinable()) {
            t.join();
        }
    }
    std::exception_ptr err;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        err = first_error_;
        first_error_ = nullptr;
    }
    if (err) {
        std::rethrow_exception(err);
    }
}

void parallel_job_runner::worker_loop(size_t generation) {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this, generation] {
                return generation_ != generation || !jobs_.empty();
            });
            if (jobs_.empty()) {
                if (generation_ != generation) {
                    return;  // wait_all() requested retirement
                }
                continue;
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        try {
            job();
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!first_error_) {
                first_error_ = std::current_exception();
            }
        }
    }
}
