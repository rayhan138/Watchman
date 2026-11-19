#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <iomanip>
#include <locale>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <utility>

// Sensor policy and presentation are independent of Windows/COM/network I/O.
namespace WatchmanSensors {
enum class Kind : size_t { Cpu, Gpu, Disk, Mainboard, Count };
inline constexpr size_t count = static_cast<size_t>(Kind::Count);
inline constexpr uint64_t freshness_ms = 15000;

struct Reading {
    bool available = false;
    double celsius = 0.0;
    uint64_t sampled_at_ms = 0;
    std::wstring sensor_name;
    std::wstring hardware_name;
    std::wstring source;
};
struct Snapshot {
    std::array<Reading, count> readings{};
    bool scanning = true;
    bool provider_connected = false;
};
struct Candidate {
    Kind kind = Kind::Cpu;
    std::wstring hardware_id;
    std::wstring hardware_name;
    std::wstring sensor_name;
    double celsius = 0.0;
};

inline const wchar_t* Name(Kind kind) {
    switch (kind) {
    case Kind::Cpu: return L"CPU";
    case Kind::Gpu: return L"GPU";
    case Kind::Disk: return L"Disk";
    case Kind::Mainboard: return L"Mainboard";
    default: return L"Sensor";
    }
}
inline std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return value;
}
inline bool ValidCelsius(double value) {
    // These PC sensor backends use zero for missing data. Reject that sentinel,
    // NaN, infinity and implausible firmware values instead of inventing a temp.
    return std::isfinite(value) && value > 0.0 && value <= 150.0;
}
inline bool Fresh(const Reading& reading, uint64_t now_ms) {
    return reading.available && ValidCelsius(reading.celsius) &&
        now_ms >= reading.sampled_at_ms && now_ms - reading.sampled_at_ms <= freshness_ms;
}
inline std::wstring Format(const Reading& reading, uint64_t now_ms, int decimals = 0) {
    if (!Fresh(reading, now_ms)) return L"Unavailable";
    std::wostringstream text;
    text.imbue(std::locale::classic());
    text << std::fixed << std::setprecision(decimals) << reading.celsius << L"\u00B0C";
    return text.str();
}
inline std::wstring Trim(std::wstring text) {
    const auto space = [](wchar_t c) { return std::iswspace(c) || c == 0xA0 || c == 0x202F; };
    while (!text.empty() && space(text.back())) text.pop_back();
    const auto start = std::find_if_not(text.begin(), text.end(), space);
    text.erase(text.begin(), start);
    return text;
}
inline std::optional<double> ParseTemperatureText(std::wstring text) {
    text = Trim(std::move(text));
    if (text.empty()) return {};
    const wchar_t unit = static_cast<wchar_t>(std::towupper(text.back()));
    if (unit != L'C' && unit != L'F') return {}; // Never guess a formatted value's unit.
    text.pop_back();
    text = Trim(std::move(text));
    if (!text.empty() && text.back() == L'\u00B0') text.pop_back();
    text = Trim(std::move(text));
    // LHM formats strings with the user's decimal separator. There are no
    // thousands in a valid temperature; mixed separators are invalid data.
    if (text.find(L',') != std::wstring::npos) {
        if (text.find(L'.') != std::wstring::npos || std::count(text.begin(), text.end(), L',') != 1) return {};
        std::replace(text.begin(), text.end(), L',', L'.');
    }
    std::wistringstream parser(text);
    parser.imbue(std::locale::classic());
    double value = 0.0;
    wchar_t trailing = 0;
    if (!(parser >> value) || (parser >> trailing)) return {};
    if (unit == L'F') value = (value - 32.0) / 1.8;
    return ValidCelsius(value) ? std::optional<double>(value) : std::nullopt;
}

inline std::optional<Kind> HardwareKind(const std::wstring& identifier, const std::wstring& image) {
    const auto id = Lower(identifier);
    const auto icon = Lower(image);
    if (id.rfind(L"/intelcpu/", 0) == 0 || id.rfind(L"/amdcpu/", 0) == 0 ||
        icon == L"images_icon/cpu.png") return Kind::Cpu;
    if (id.rfind(L"/gpu-", 0) == 0 || icon == L"images_icon/nvidia.png" ||
        icon == L"images_icon/ati.png" || icon == L"images_icon/intel.png") return Kind::Gpu;
    if (id.rfind(L"/hdd/", 0) == 0 || id.rfind(L"/nvme/", 0) == 0 ||
        id.rfind(L"/ssd/", 0) == 0 || icon == L"images_icon/hdd.png") return Kind::Disk;
    if (icon == L"images_icon/mainboard.png" || icon == L"images_icon/chip.png") return Kind::Mainboard;
    return {};
}

inline int Priority(const Candidate& sensor) {
    const auto name = Lower(sensor.sensor_name);
    const auto has = [&](const wchar_t* word) { return name.find(word) != std::wstring::npos; };
    switch (sensor.kind) {
    case Kind::Cpu:
        if (has(L"distance") || has(L"tjmax") || has(L"tj. max")) return -1;
        if (has(L"package") || name == L"core (tdie)") return 0;
        if (has(L"tctl/tdie") || has(L"tdie")) return 1;
        if (has(L"core max")) return 2;
        if (has(L"core") && !has(L"average")) return 3;
        // Tctl may include a vendor control offset; use it only as a labelled
        // fallback, never in preference to a measured die/package reading.
        if (has(L"tctl") || name == L"temperature") return 4;
        return -1;
    case Kind::Gpu:
        if (has(L"memory") || has(L"hot spot") || has(L"hotspot") || has(L"junction") || has(L"vrm")) return -1;
        return has(L"core") || name == L"gpu" || name == L"temperature" ? 0 : -1;
    case Kind::Disk:
        return name == L"temperature" || has(L"composite") ? 0 : 1;
    case Kind::Mainboard:
        // A generic ACPI zone, VRM, chipset, or CPU socket is NOT a known
        // mainboard/system sensor. Keep its absence explicit.
        return has(L"system") || has(L"motherboard") || has(L"mainboard") ? 0 : -1;
    default:
        return -1;
    }
}

inline std::array<Reading, count> Select(const std::vector<Candidate>& sensors,
                                       uint64_t sampled_at_ms, const std::wstring& source) {
    // Choose the right sensor per physical device first, then the hottest
    // device of each kind. A cooler CPU package must not mask another CPU's
    // hottest-core fallback. The UI discloses this max-across-devices policy.
    std::map<std::pair<Kind, std::wstring>, const Candidate*> devices;
    for (const auto& sensor : sensors) {
        const int priority = Priority(sensor);
        if (priority < 0 || !ValidCelsius(sensor.celsius)) continue;
        auto& selected = devices[{sensor.kind, sensor.hardware_id}];
        if (!selected || priority < Priority(*selected) ||
            (priority == Priority(*selected) && sensor.celsius > selected->celsius)) selected = &sensor;
    }
    std::array<Reading, count> readings{};
    for (const auto& entry : devices) {
        const auto& sensor = *entry.second;
        auto& reading = readings[static_cast<size_t>(sensor.kind)];
        if (!reading.available || sensor.celsius > reading.celsius) {
            reading = {true, sensor.celsius, sampled_at_ms, sensor.sensor_name, sensor.hardware_name, source};
        }
    }
    return readings;
}
} // namespace WatchmanSensors
