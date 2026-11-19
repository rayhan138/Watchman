#include "temperature_provider.h"
#include <windows.h>
#include <roapi.h>
#include <iostream>
#include <stdexcept>

namespace {
using namespace WatchmanSensors;
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void RealSchema() {
    // LHM 0.9.6 uses formatted RawValue strings; newer versions use numeric
    // Celsius. Older versions omit RawValue and may display Fahrenheit.
    const std::wstring json = LR"({"Text":"Sensor","Children":[{"Text":"PC","Children":[
      {"Text":"CPU A","HardwareId":"/intelcpu/0","ImageURL":"images_icon/cpu.png","Children":[
        {"Text":"Temperatures","Children":[
          {"SensorId":"/intelcpu/0/temperature/0","Type":"Temperature","Text":"CPU Package","RawValue":52.25,"Value":"126.1 \u00b0F","Min":"5 C","Max":"120 C","Children":[]},
          {"SensorId":"/intelcpu/0/temperature/1","Type":"Temperature","Text":"CPU Core #1","RawValue":null,"Value":"99 C"},
          {"SensorId":"/intelcpu/0/load/0","Type":"Load","Text":"CPU Total","RawValue":88}
        ]}]},
      {"Text":"GPU A","HardwareId":"/gpu-nvidia/0","ImageURL":"images_icon/nvidia.png","Children":[
        {"SensorId":"/gpu-nvidia/0/temperature/0","Type":"Temperature","Text":"GPU Core","RawValue":"48,5 \u00b0C"},
        {"SensorId":"/gpu-nvidia/0/temperature/1","Type":"Temperature","Text":"GPU Hot Spot","RawValue":75}
      ]},
      {"Text":"SSD A","ImageURL":"images_icon/hdd.png","Children":[
        {"SensorId":"/hdd/0/temperature/0","Type":"Temperature","Text":"Temperature","Value":"104 \u00b0F"}
      ]},
      {"Text":"Board A","ImageURL":"images_icon/mainboard.png","Children":[
        {"Text":"SuperIO","ImageURL":"images_icon/chip.png","Children":[
          {"SensorId":"/lpc/chip/0/temperature/0","Type":"Temperature","Text":"System","RawValue":37},
          {"SensorId":"/lpc/chip/0/temperature/1","Type":"Temperature","Text":"CPU","RawValue":91}
        ]}
      ]}
    ]}]})";
    std::vector<Candidate> sensors;
    Check(ParseLibreHardwareMonitorJson(json, sensors), "Actual nested provider schema must parse");
    Check(sensors.size() == 6, "Null values and non-temperature sensors must not enter selection");
    const auto readings = Select(sensors, 1000, L"LibreHardwareMonitor");
    Check(std::abs(readings[0].celsius - 52.25) < 0.0001, "Numeric RawValue must stay Celsius despite a Fahrenheit UI");
    Check(std::abs(readings[1].celsius - 48.5) < 0.0001, "Localized formatted RawValue must parse");
    Check(std::abs(readings[2].celsius - 40.0) < 0.0001, "Older explicit Fahrenheit values must convert");
    Check(readings[3].celsius == 37, "A mainboard CPU socket sensor must not replace System");
    Check(readings[2].hardware_name == L"SSD A", "Hardware identity must survive grouping nodes");
}
void FailClosed() {
    std::vector<Candidate> sensors{{Kind::Cpu, L"old", L"old", L"CPU Package", 99}};
    for (const auto* json : {L"", L"{broken", L"[]", L"{}", L"{\"Text\":\"Other\",\"Children\":[]}",
                            L"{\"Text\":\"Sensor\",\"Children\":[42]}",
                            L"{\"Text\":\"Sensor\",\"Children\":{}}"}) {
        Check(!ParseLibreHardwareMonitorJson(json, sensors) && sensors.empty(), "Malformed or unrelated JSON must clear old readings");
    }
    const auto invalid = LR"({"Text":"Sensor","Children":[{"ImageURL":"images_icon/cpu.png","Children":[
      {"Type":"Temperature","SensorId":"/intelcpu/0/temperature/0","Text":"CPU Package","RawValue":0,"Value":"100 C"},
      {"Type":"Temperature","SensorId":"/intelcpu/0/temperature/1","Text":"CPU Core #1","RawValue":"NaN"},
      {"Type":"Temperature","SensorId":"/intelcpu/0/temperature/2","Text":"CPU Core #2","RawValue":999},
      {"Type":"Temperature","SensorId":"/intelcpu/0/temperature/3","Text":"CPU Core #3","RawValue":true},
      {"Type":"Temperature","SensorId":"/intelcpu/0/temperature/4","Text":"CPU Core #4","Value":"50"}
    ]}]})";
    Check(ParseLibreHardwareMonitorJson(invalid, sensors) && sensors.empty(), "Invalid sensors must not fall back to formatted/min/max guesses");
    Check(ParseLibreHardwareMonitorJson(L"{\"Text\":\"Sensor\",\"Children\":[]}", sensors) && sensors.empty(),
        "A connected provider with no sensors is not a made-up temperature");
    std::wstring deep = L"{\"Text\":\"Sensor\",\"Children\":[";
    for (int i = 0; i < 30; ++i) deep += L"{\"Children\":[";
    deep += L"{}";
    for (int i = 0; i < 30; ++i) deep += L"]}";
    deep += L"]}";
    Check(!ParseLibreHardwareMonitorJson(deep, sensors), "Tree recursion must have a hard limit");
    Check(!ParseLibreHardwareMonitorJson(std::wstring(1024 * 1024 + 1, L' '), sensors), "Response size must be bounded");
}
} // namespace
int main() {
    try {
        Check(SUCCEEDED(RoInitialize(RO_INIT_MULTITHREADED)), "Could not initialize Windows JSON runtime");
        struct RuntimeScope { ~RuntimeScope() { RoUninitialize(); } } runtime;
        RealSchema(); FailClosed();
        std::cout << "Temperature provider schema tests passed (no hardware/network access)\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
