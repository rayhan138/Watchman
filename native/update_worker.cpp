#include "update_checker.h"

namespace WatchmanUpdates {
bool Worker::Request() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || busy_ || result_) return false;
    if (!thread_.joinable()) thread_ = std::thread(&Worker::Run, this);
    busy_ = requested_ = true;
    ready_.notify_one();
    return true;
}
bool Worker::Take(Result& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!result_) return false;
    result = std::move(*result_);
    result_.reset();
    return true;
}
void Worker::Stop() {
    stopping_ = true;
    ready_.notify_all();
    if (thread_.joinable()) thread_.join();
}
void Worker::Run() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        ready_.wait(lock, [&] { return requested_ || stopping_; });
        if (stopping_) return;
        requested_ = false;
        lock.unlock();
        Result result;
        try { result = check_(stopping_); }
        catch (...) { result = {Status::Unavailable, {}, 1}; }
        lock.lock();
        busy_ = false;
        if (!stopping_) result_ = std::move(result);
    }
}
} // namespace WatchmanUpdates
