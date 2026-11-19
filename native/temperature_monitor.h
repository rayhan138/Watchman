#pragma once
#include "temperature_sensors.h"
#include <condition_variable>
#include <mutex>
#include <thread>

// Slow or absent sensor providers never run on the UI/one-second metrics thread.
// No custom driver, process, service, or privileged operation is installed here.
class TemperatureMonitor {
public:
    TemperatureMonitor();
    ~TemperatureMonitor();
    TemperatureMonitor(const TemperatureMonitor&) = delete;
    TemperatureMonitor& operator=(const TemperatureMonitor&) = delete;
    WatchmanSensors::Snapshot GetSnapshot() const;

private:
    void Run();
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    WatchmanSensors::Snapshot snapshot_;
    std::thread worker_;
};
