#include "floating_monitor_layout.h"
#include "floating_monitor_text.h"
#include "live_readings.h"

#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace FloatingMonitorLayout;
size_t checks = 0;

void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

bool Empty(const Rect& rect) {
    return rect.left == rect.right && rect.top == rect.bottom;
}

bool Same(const Rect& a, const Rect& b) {
    return a.left == b.left && a.right == b.right && a.top == b.top && a.bottom == b.bottom;
}

void Bounds(const Layout& layout, int surface_width, int surface_height) {
    for (const auto& rect : std::array<Rect, 8>{layout.upload_label, layout.upload_value, layout.download_label, layout.download_value,
                                             layout.cpu_label, layout.cpu_value, layout.memory_label, layout.memory_value}) {
        Check(rect.left >= 0 && rect.left <= rect.right && rect.right <= surface_width, "Ordered horizontal bounds inside surface");
        Check(rect.top >= 0 && rect.top <= rect.bottom && rect.bottom <= surface_height, "Ordered vertical bounds inside surface");
    }
}

void ModesAndFonts() {
    Check(default_show_cpu && default_show_memory, "CPU and memory are enabled by default");
    Check(Width(12) == 188 && Height() == 56 && Height(12, false, false) == 28,
          "New defaults use two rows; disabling hardware restores the original compact strip");
    for (int font = minimum_font_size; font <= maximum_font_size; ++font) {
        for (bool cpu : {false, true}) for (bool memory : {false, true}) {
            const int w = Width(font), h = Height(font, cpu, memory);
            Check(h == RowHeight(font) * (cpu || memory ? 2 : 1), "No unused hardware row");
            const auto layout = Get(w, h, font, cpu, memory);
            Bounds(layout, w, h);
            const auto network = Get(w, h, font, false, false);
            Check(Same(layout.upload_label, network.upload_label) && Same(layout.upload_value, network.upload_value) &&
                  Same(layout.download_label, network.download_label) && Same(layout.download_value, network.download_value),
                  "Hardware settings never shrink, move, or replace the network readings");
            Check(cpu != Empty(layout.cpu_label) && cpu != Empty(layout.cpu_value), "CPU visibility matches its geometry");
            Check(memory != Empty(layout.memory_label) && memory != Empty(layout.memory_value), "Memory visibility matches its geometry");
            for (const auto& pair : {std::pair<bool, Rect>{cpu, layout.cpu_value}, {memory, layout.memory_value}}) {
                if (pair.first) {
                    Check(pair.second.top > layout.upload_value.bottom && pair.second.bottom - pair.second.top >= font,
                          "Hardware percentages fit on their own second row");
                    Check(pair.second.right - pair.second.left >= 48 * w / width, "100% retains a full-width percentage field");
                }
            }
            if (cpu && memory) Check(layout.cpu_label.right < layout.cpu_value.left &&
                layout.cpu_value.right < layout.memory_label.left && layout.memory_label.right < layout.memory_value.left,
                "CPU and memory labels/percentages never overlap");
            if (cpu != memory) {
                const auto label = cpu ? layout.cpu_label : layout.memory_label;
                const auto value = cpu ? layout.cpu_value : layout.memory_value;
                Check(label.left >= w / 4 && value.right <= w * 3 / 4 && label.right < value.left,
                      "A single optional reading is centered, not stranded in a half-empty column");
            }
        }
    }
    for (int font : {std::numeric_limits<int>::min(), -100, 0, 10}) {
        Check(Width(font) == Width(11) && Height(font) == Height(11), "Font lower bound is safe before multiplication");
    }
    for (int font : {17, 1000, std::numeric_limits<int>::max()}) {
        Check(Width(font) == Width(16) && Height(font) == Height(16), "Font upper bound is safe before multiplication");
    }
    for (int w : {0, 1, 20, 100, 188, 251, 400}) for (int h : {0, 1, 10, 28, 29, 40, 56, 100}) {
        for (bool cpu : {false, true}) for (bool memory : {false, true}) {
            Bounds(Get(w, h, 16, cpu, memory), w, h);
        }
    }
    Bounds(Get(-100, -100), 0, 0);
}

void TextWeightPolicy() {
    using namespace FloatingMonitorText;
    Check(default_weight == Weight::Light && Value(default_weight) == 300,
          "Absent new settings use readable Light, never the old hard-coded Bold");
    Check(weights.size() == 3, "Light, Regular and Bold are the same three settings/menu choices");
    Check(std::wstring(GdiFamily(Weight::Light)) == L"Segoe UI Light" &&
          std::wstring(GdiFamily(Weight::Regular)) == family && std::wstring(GdiFamily(Weight::Bold)) == family,
          "GDI names the actual Light face; Regular/Bold retain the base family");
    for (const auto weight : weights) {
        Check(FromSaved(Value(weight)) == weight, "Every offered weight survives persistence");
        Check(Label(weight)[0] != L'\0', "Every weight has a visible text label");
    }
    Check(Value(Weight::Regular) == 400 && Value(Weight::Bold) == 700,
          "GDI and DirectWrite get genuine OpenType weights, not synthetic outlines");
    for (int bad : {std::numeric_limits<int>::min(), -1, 0, 1, 299, 301, 350, 500, 600, 701, 900, std::numeric_limits<int>::max()}) {
        Check(FromSaved(bad) == default_weight && Value(static_cast<Weight>(bad)) == 300,
              "Invalid persisted/runtime weights safely use Light");
    }
}

void SampleAvailability() {
    DashboardSnapshot sample;
    Check(!WatchmanLiveReadings::MemoryReady(sample, 1000), "Missing memory sample is not a real zero");
    Check(!Telemetry::FreshCpuSample(sample.cpu_sample_available, sample.cpu_sampled_at_ms, 1000),
          "Missing CPU sample is not a real zero");
    sample.ram_sample_available = true;
    sample.ram_sampled_at_ms = 1000;
    sample.ram_total_bytes = 8ULL * 1024 * 1024 * 1024;
    sample.ram_usage_percent = 58;
    Check(WatchmanLiveReadings::MemoryReady(sample, 6000), "CPU/memory share the existing five-second freshness window");
    Check(!WatchmanLiveReadings::MemoryReady(sample, 6001) && !WatchmanLiveReadings::MemoryReady(sample, 999),
          "Stale/future memory samples cannot look live");
    for (float value : {-1.0f, 100.1f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        sample.ram_usage_percent = value;
        Check(!WatchmanLiveReadings::MemoryReady(sample, 1000), "Invalid memory percentages are unavailable");
    }
    sample.ram_usage_percent = 0;
    Check(WatchmanLiveReadings::MemoryReady(sample, 1000), "A real zero-percent memory sample remains available");
    sample.ram_usage_percent = 100;
    Check(WatchmanLiveReadings::MemoryReady(sample, 1000), "A real 100-percent memory sample remains available");
}
} // namespace

int main() {
    try {
        ModesAndFonts();
        SampleAvailability();
        TextWeightPolicy();
        std::cout << "FloatingMonitorLayout: " << checks << " checks passed (all visibility modes, sizes, clipping, sample availability and text weights)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FloatingMonitorLayout: " << error.what() << '\n';
        return 1;
    }
}
