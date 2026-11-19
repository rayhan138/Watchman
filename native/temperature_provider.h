#pragma once
#include "temperature_sensors.h"

namespace WatchmanSensors {
// Parses LibreHardwareMonitor's read-only /data.json schema. Numeric RawValue
// is Celsius; older formatted values require an explicit C/F unit. No I/O.
bool ParseLibreHardwareMonitorJson(const std::wstring& json, std::vector<Candidate>& sensors);
}
