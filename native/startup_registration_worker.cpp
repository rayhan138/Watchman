#include "startup_registration.h"

#include <utility>

namespace WatchmanStartup {
Worker::Worker(Work work) : work_(std::move(work)) {}
Worker::~Worker() { Stop(); }

uint64_t Worker::Submit(bool enabled, bool explicit_change, std::wstring executable) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return 0;
    pending_ = Request{++sequence_, enabled, explicit_change, std::move(executable)};
    if (!worker_.joinable()) {
        try { worker_ = std::thread([this] { Run(); }); }
        catch (...) { pending_.reset(); throw; }
    }
    wake_.notify_one();
    return sequence_;
}

bool Worker::TakeResult(Result& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!completed_) return false;
    result = *completed_;
    completed_.reset();
    return true;
}

void Worker::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_one();
    if (worker_.joinable()) worker_.join();
}

void Worker::Run() {
    for (;;) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
            if (!pending_) return;
            request = std::move(*pending_);
            pending_.reset();
        }
        Result result;
        try { result = work_(request); }
        catch (...) { result.error = unexpected_error; result.status = Status::Failed; }
        result.sequence = request.sequence;
        result.requested_enabled = request.enabled;
        result.explicit_change = request.explicit_change;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            completed_ = result;
        }
    }
}
} // namespace WatchmanStartup
