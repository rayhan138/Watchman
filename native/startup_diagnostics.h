#pragma once

#include <cstdint>

// Nonblocking, bounded, transition-only startup diagnostics. No traffic, app
// names, settings, URLs or history contents are recorded. A logging failure
// must never prevent monitoring or painting.
namespace WatchmanStartupDiagnostics {
void Record(const char* component, const char* event, uint32_t error = 0) noexcept;
// Preserve the launch timestamp while waiting until the single-instance guard
// is owned. Duplicate launches must not start another writer on the same log.
void RecordAt(const char* component, const char* event, uint64_t timestamp_ms, uint32_t error = 0) noexcept;
}
