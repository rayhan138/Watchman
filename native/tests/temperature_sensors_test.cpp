#include "temperature_sensors.h"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace WatchmanSensors;
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void Near(double actual, double expected, const char* message) {
    Check(std::isfinite(actual) && std::abs(actual - expected) < 0.00001, message);
}
void ValuesAndUnits() {
    Near(*ParseTemperatureText(L"48.5 \u00B0C"), 48.5, "Celsius strings must preserve actual readings");
    Near(*ParseTemperatureText(L" 48,5 \u00B0C "), 48.5, "Comma decimal is not a thousands separator");
    Near(*ParseTemperatureText(L"122 \u00B0F"), 50.0, "Fahrenheit UI values must convert to Celsius");
    Near(*ParseTemperatureText(L"50\u202F\u00B0C"), 50.0, "Narrow nonbreaking space must parse");
    for (const auto* invalid : {L"", L"50", L"0 C", L"-273.15 C", L"2731 C", L"NaN C", L"inf C",
                               L"50 %", L"50 C junk", L"1,2,3 C", L"1,234.5 C", L"50 60 C", L"-- C"}) {
        Check(!ParseTemperatureText(invalid), "Missing, untyped, invalid or implausible temperature must be rejected");
    }
    Check(!ValidCelsius(std::numeric_limits<double>::quiet_NaN()), "NaN must be unavailable");
    Check(!ValidCelsius(std::numeric_limits<double>::infinity()), "Infinity must be unavailable");
    Check(ValidCelsius(150.0) && !ValidCelsius(150.1), "Sensor upper bound");
}
void Freshness() {
    Reading reading{true, 50.25, 0, L"CPU Package", L"CPU 0", L"Test backend"};
    Check(Fresh(reading, freshness_ms), "A zero boot timestamp and exact age limit are valid");
    Check(!Fresh(reading, freshness_ms + 1), "Stale sensors cannot be shown as live");
    Check(Format(reading, 0, 1) == L"50.2\u00B0C", "Detailed display must include Celsius");
    Check(Format(reading, freshness_ms + 1) == L"Unavailable", "Stale display must be explicit");
    reading.sampled_at_ms = 200;
    Check(!Fresh(reading, 199), "Future timestamp must not underflow");
    reading.available = false;
    Check(!Fresh(reading, 200), "Unavailable reading must not reuse its temperature");
    reading.available = true;
    reading.celsius = 0;
    Check(!Fresh(reading, 200), "Provider zero sentinel must not look like an ice-cold CPU");
}
void Selection() {
    std::vector<Candidate> sensors{
        {Kind::Cpu, L"cpu0", L"Intel CPU", L"CPU Package", 62.5},
        {Kind::Cpu, L"cpu0", L"Intel CPU", L"CPU Core #1", 80},
        {Kind::Cpu, L"cpu0", L"Intel CPU", L"Distance to TjMax", 95},
        {Kind::Gpu, L"gpu0", L"GPU A", L"GPU Core", 54},
        {Kind::Gpu, L"gpu0", L"GPU A", L"GPU Memory Junction", 100},
        {Kind::Gpu, L"gpu0", L"GPU A", L"GPU Hot Spot", 90},
        {Kind::Disk, L"disk0", L"SSD A", L"Temperature", 45},
        {Kind::Disk, L"disk0", L"SSD A", L"Controller", 70},
        {Kind::Disk, L"disk1", L"SSD B", L"Composite", 49},
        {Kind::Mainboard, L"board0", L"Board A", L"System", 39},
        {Kind::Mainboard, L"board0", L"Board A", L"CPU socket", 79},
        {Kind::Mainboard, L"board0", L"Board A", L"VRM", 85},
        {Kind::Mainboard, L"board0", L"Board A", L"Temperature #1", 127},
    };
    auto readings = Select(sensors, 1000, L"Test provider");
    Near(readings[0].celsius, 62.5, "A core reading or TjMax distance must not replace the package sensor");
    Near(readings[1].celsius, 54, "GPU core must not be confused with memory or hotspot");
    Near(readings[2].celsius, 49, "Hottest physical disk's composite/main sensor must be selected");
    Near(readings[3].celsius, 39, "Only labelled system/mainboard sensors qualify as board temperature");
    Check(readings[0].source == L"Test provider" && readings[0].sensor_name == L"CPU Package" &&
        readings[0].sampled_at_ms == 1000, "Sensor identity, source and sample time must survive selection");
    sensors.push_back({Kind::Cpu, L"cpu1", L"AMD CPU", L"Core (Tctl)", 88});
    sensors.push_back({Kind::Cpu, L"cpu1", L"AMD CPU", L"Core (Tdie)", 58});
    readings = Select(sensors, 2000, L"Test provider");
    Near(readings[0].celsius, 62.5, "AMD control offsets must not override the real die temperature");
    sensors.push_back({Kind::Cpu, L"cpu2", L"CPU without package sensor", L"CPU Core #2", 78});
    sensors.push_back({Kind::Cpu, L"cpu2", L"CPU without package sensor", L"CPU Core #1", 74});
    readings = Select(sensors, 3000, L"Test provider");
    Near(readings[0].celsius, 78, "A cooler package from another CPU must not mask a hotter core fallback");
    Check(readings[0].sensor_name == L"CPU Core #2", "A fallback must retain its real name");
    sensors.push_back({Kind::Gpu, L"gpu1", L"GPU B", L"GPU Core", 60});
    readings = Select(sensors, 4000, L"Test provider");
    Near(readings[1].celsius, 60, "Multiple GPUs use the hottest core, not an arbitrary adapter");
    for (const auto& reading : Select({}, 0, L"Test provider")) Check(!reading.available, "No sensor is unavailable, not zero Celsius");
}
void HardwareClassification() {
    Check(HardwareKind(L"/intelcpu/0/temperature/0", L"") == Kind::Cpu, "Intel CPU identifier");
    Check(HardwareKind(L"/amdcpu/0", L"") == Kind::Cpu, "AMD CPU identifier");
    Check(HardwareKind(L"/gpu-nvidia/0", L"") == Kind::Gpu, "GPU identifier");
    Check(HardwareKind(L"/hdd/0", L"") == Kind::Disk, "Physical disk identifier");
    Check(HardwareKind(L"/lpc/nct/0", L"images_icon/chip.png") == Kind::Mainboard, "SuperIO belongs to mainboard, not CPU");
    Check(!HardwareKind(L"ACPI\\ThermalZone\\0", L""), "An unlabelled ACPI zone must not masquerade as CPU temperature");
}
} // namespace
int main() {
    try { ValuesAndUnits(); Freshness(); Selection(); HardwareClassification(); std::cout << "Temperature sensor policy tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
