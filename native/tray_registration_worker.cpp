#include "tray_registration.h"

namespace WatchmanTray {
bool Worker::Request(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || !generation) return false;
    if (!thread_.joinable()) thread_ = std::thread(&Worker::Run, this);
    pending_ = generation;
    wake_.notify_one();
    return true;
}

bool Worker::Take(Result& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!result_) return false;
    result = *result_;
    result_.reset();
    return true;
}

void Worker::Cancel() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        pending_.reset();
        result_.reset();
    }
    wake_.notify_one();
}

void Worker::Stop() {
    Cancel();
    if (thread_.joinable()) thread_.join();
}

void Worker::Run() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
        if (stopping_) return;
        const uint64_t generation = *pending_;
        pending_.reset();
        lock.unlock();
        bool ready = false;
        try { ready = work_(); } catch (...) { /* Retry on the UI-owned schedule. */ }
        lock.lock();
        if (!stopping_) result_ = Result{generation, ready};
    }
}
} // namespace WatchmanTray
