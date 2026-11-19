#pragma once

// Persisted IDs: legacy full/network-only settings map to All/Network.
enum class TaskbarWidgetMode {
    All = 0,
    Network = 1,
    System = 2
};

namespace TaskbarWidgetModes {
inline constexpr TaskbarWidgetMode Normalize(int value) {
    return value == 1 ? TaskbarWidgetMode::Network
        : value == 2 ? TaskbarWidgetMode::System : TaskbarWidgetMode::All;
}
inline constexpr bool ShowsNetwork(TaskbarWidgetMode mode) {
    return Normalize(static_cast<int>(mode)) != TaskbarWidgetMode::System;
}
inline constexpr bool ShowsSystem(TaskbarWidgetMode mode) {
    return Normalize(static_cast<int>(mode)) != TaskbarWidgetMode::Network;
}
inline constexpr TaskbarWidgetMode FromLegacyNetworkOnly(bool network_only) {
    return network_only ? TaskbarWidgetMode::Network : TaskbarWidgetMode::All;
}
}
