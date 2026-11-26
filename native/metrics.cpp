#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include "metrics.h"
#include <ws2tcpip.h>
#include <iostream>
#include <pdh.h>
#include <powrprof.h>
#include <wlanapi.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <iomanip>
#include <initializer_list>
#include <map>
#include <utility>
#include <sstream>

namespace {

std::wstring FormatInterfaceGuid(const GUID& guid) {
    std::wostringstream stream;
    stream << std::uppercase << std::hex << std::setfill(L'0')
           << L'{' << std::setw(8) << guid.Data1
           << L'-' << std::setw(4) << guid.Data2
           << L'-' << std::setw(4) << guid.Data3
           << L'-' << std::setw(2) << static_cast<unsigned int>(guid.Data4[0])
           << std::setw(2) << static_cast<unsigned int>(guid.Data4[1])
           << L'-' << std::setw(2) << static_cast<unsigned int>(guid.Data4[2])
           << std::setw(2) << static_cast<unsigned int>(guid.Data4[3])
           << std::setw(2) << static_cast<unsigned int>(guid.Data4[4])
           << std::setw(2) << static_cast<unsigned int>(guid.Data4[5])
           << std::setw(2) << static_cast<unsigned int>(guid.Data4[6])
           << std::setw(2) << static_cast<unsigned int>(guid.Data4[7])
           << L'}';
    return stream.str();
}

bool SameInterfaceGuid(const GUID& left, const GUID& right) {
    return std::memcmp(&left, &right, sizeof(GUID)) == 0;
}

std::wstring ToLower(const std::wstring& value) {
    std::wstring lowered = value;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return lowered;
}

bool ContainsAny(const std::wstring& value, std::initializer_list<const wchar_t*> needles) {
    const std::wstring lowered = ToLower(value);
    for (const wchar_t* needle : needles) {
        if (lowered.find(needle) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

std::wstring ClassifyInterfaceType(const MIB_IF_ROW2& row) {
    if (row.Type == IF_TYPE_IEEE80211) {
        return L"Wi-Fi";
    }
    if (row.Type == IF_TYPE_TUNNEL ||
        ContainsAny(row.Alias, { L"vpn", L"wireguard", L"openvpn", L"tailscale", L"zerotier", L"wintun", L"tap", L"anyconnect", L"forti", L"globalprotect", L"nordlynx", L"protonvpn", L"expressvpn", L"surfshark", L"hamachi" }) ||
        ContainsAny(row.Description, { L"vpn", L"wireguard", L"openvpn", L"tailscale", L"zerotier", L"wintun", L"tap", L"anyconnect", L"forti", L"globalprotect", L"nordlynx", L"protonvpn", L"expressvpn", L"surfshark", L"hamachi" })) {
        return L"VPN";
    }
    if (row.Type == IF_TYPE_ETHERNET_CSMACD) {
        return L"Ethernet";
    }
    return L"Other";
}

bool IsInterfaceConnected(const MIB_IF_ROW2& row) {
    return row.OperStatus == IfOperStatusUp &&
        (row.MediaConnectState == MediaConnectStateConnected ||
         row.MediaConnectState == MediaConnectStateUnknown);
}

// Windows exposes protocol bindings, filter drivers, virtual Wi-Fi devices,
// Bluetooth PANs, and legacy miniports as normal interfaces. They are useful
// to the OS but are never a meaningful monitoring choice for a person.
bool IsWindowsInternalAdapter(const MIB_IF_ROW2& row) {
    const std::initializer_list<const wchar_t*> kInternalNames = {
        L"network monitor",
        L"packet scheduler",
        L"lightweight filter",
        L"filter driver",
        L"wfp",
        L"wan miniport",
        L"microsoft wi-fi direct",
        L"wi-fi direct virtual",
        L"bluetooth",
        L"kernel debugger",
        L"teredo",
        L"isatap",
        L"6to4",
        L"ras async",
        L"hyper-v",
        L"vethernet",
        L"vmware",
        L"virtualbox",
    };
    return ContainsAny(row.Alias, kInternalNames) ||
        ContainsAny(row.Description, kInternalNames);
}

bool IsMobileTetheringAdapter(const MIB_IF_ROW2& row) {
    const std::initializer_list<const wchar_t*> kMobileTetheringNames = {
        L"remote ndis",
        L"rndis",
        L"mobile usb",
    };
    return ContainsAny(row.Alias, kMobileTetheringNames) ||
        ContainsAny(row.Description, kMobileTetheringNames);
}

bool IsAdapterPickerCandidate(const MIB_IF_ROW2& row, const std::wstring& selected_adapter_id) {
    if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK) {
        return false;
    }

    const bool selected = !selected_adapter_id.empty() &&
        _wcsicmp(FormatInterfaceGuid(row.InterfaceGuid).c_str(), selected_adapter_id.c_str()) == 0;
    if (selected) {
        // Never make a stored manual lock impossible to undo from the picker.
        return true;
    }
    if (IsWindowsInternalAdapter(row)) {
        return false;
    }

    // USB phone tethering is a legitimate active connection. It uses the
    // Remote NDIS driver family, so it must not be confused with Windows'
    // unrelated hidden protocol bindings.
    if (IsMobileTetheringAdapter(row)) {
        return IsInterfaceConnected(row);
    }

    const std::wstring type = ClassifyInterfaceType(row);
    if (type == L"Wi-Fi" || type == L"Ethernet") {
        // A real wired or wireless adapter remains selectable even while its
        // cable or radio is disconnected, so users can intentionally lock it.
        return true;
    }

    // VPN, cellular, and other non-physical adapters are useful only when
    // they have an active connection.
    return IsInterfaceConnected(row);
}

} // namespace



// Helper to get local IP address of active adapter
static std::wstring GetLocalIPAddress(uint32_t adapter_index) {
    ULONG size = 0;
    if (GetIpAddrTable(nullptr, &size, FALSE) == ERROR_INSUFFICIENT_BUFFER) {
        if (size > 0) {
            std::vector<char> buffer(size);
            PMIB_IPADDRTABLE table = reinterpret_cast<PMIB_IPADDRTABLE>(buffer.data());
            if (GetIpAddrTable(table, &size, FALSE) == NO_ERROR) {
                for (DWORD i = 0; i < table->dwNumEntries; ++i) {
                    if (table->table[i].dwIndex == adapter_index) {
                        IN_ADDR addr;
                        addr.S_un.S_addr = table->table[i].dwAddr;
                        char ip_buf[INET_ADDRSTRLEN];
                        if (inet_ntop(AF_INET, &addr, ip_buf, INET_ADDRSTRLEN)) {
                            std::string ip_str(ip_buf);
                            return std::wstring(ip_str.begin(), ip_str.end());
                        }
                    }
                }
            }
        }
    }
    return L"127.0.0.1";
}

static std::wstring SocketAddressToString(const SOCKADDR* address) {
    if (address == nullptr) {
        return L"N/A";
    }

    wchar_t buffer[INET6_ADDRSTRLEN]{};
    if (address->sa_family == AF_INET) {
        const auto* ipv4 = reinterpret_cast<const SOCKADDR_IN*>(address);
        return InetNtopW(AF_INET, const_cast<IN_ADDR*>(&ipv4->sin_addr), buffer, ARRAYSIZE(buffer))
            ? std::wstring(buffer)
            : L"N/A";
    }
    if (address->sa_family == AF_INET6) {
        const auto* ipv6 = reinterpret_cast<const SOCKADDR_IN6*>(address);
        return InetNtopW(AF_INET6, const_cast<IN6_ADDR*>(&ipv6->sin6_addr), buffer, ARRAYSIZE(buffer))
            ? std::wstring(buffer)
            : L"N/A";
    }
    return L"N/A";
}

static std::wstring FormatMacAddress(const BYTE* bytes, ULONG length) {
    if (bytes == nullptr || length == 0) {
        return L"N/A";
    }

    std::wostringstream stream;
    stream << std::uppercase << std::hex << std::setfill(L'0');
    for (ULONG i = 0; i < length; ++i) {
        if (i > 0) {
            stream << L'-';
        }
        stream << std::setw(2) << static_cast<unsigned int>(bytes[i]);
    }
    return stream.str();
}

static std::wstring ReadRegistryString(HKEY root, const wchar_t* path, const wchar_t* value) {
    DWORD bytes = 0;
    const LSTATUS size_status = RegGetValueW(
        root, path, value, RRF_RT_REG_SZ, nullptr, nullptr, &bytes
    );
    if (size_status != ERROR_SUCCESS || bytes < sizeof(wchar_t)) {
        return L"";
    }

    std::vector<wchar_t> buffer((bytes / sizeof(wchar_t)) + 1, L'\0');
    if (RegGetValueW(root, path, value, RRF_RT_REG_SZ, nullptr, buffer.data(), &bytes) != ERROR_SUCCESS) {
        return L"";
    }
    return buffer.data();
}

static DWORD ReadRegistryDword(HKEY root, const wchar_t* path, const wchar_t* value) {
    DWORD result = 0;
    DWORD bytes = sizeof(result);
    if (RegGetValueW(root, path, value, RRF_RT_REG_DWORD, nullptr, &result, &bytes) != ERROR_SUCCESS) {
        return 0;
    }
    return result;
}

struct WifiConnectionDetails {
    std::wstring ssid;
    uint32_t signal_percent = 0;
    uint64_t receive_link_speed_bps = 0;
    uint64_t transmit_link_speed_bps = 0;
};

// WLAN information is requested only while the Network tab is visible. This
// avoids unnecessary Wi-Fi/location activity while Watchman is in the background.
static WifiConnectionDetails QueryWifiConnectionDetails(const GUID* target_interface_guid = nullptr) {
    HANDLE hClient = nullptr;
    DWORD dwMaxClient = 2;
    DWORD dwCurVersion = 0;
    WifiConnectionDetails details;

    if (WlanOpenHandle(dwMaxClient, nullptr, &dwCurVersion, &hClient) == ERROR_SUCCESS) {
        PWLAN_INTERFACE_INFO_LIST pIfList = nullptr;
        if (WlanEnumInterfaces(hClient, nullptr, &pIfList) == ERROR_SUCCESS) {
            for (DWORD i = 0; i < pIfList->dwNumberOfItems; ++i) {
                WLAN_INTERFACE_INFO* pIfInfo = &pIfList->InterfaceInfo[i];
                if (target_interface_guid != nullptr &&
                    !SameInterfaceGuid(pIfInfo->InterfaceGuid, *target_interface_guid)) {
                    continue;
                }
                if (pIfInfo->isState == wlan_interface_state_connected) {
                    PWLAN_CONNECTION_ATTRIBUTES pConnAttrs = nullptr;
                    DWORD dwSize = 0;
                    WLAN_OPCODE_VALUE_TYPE opType;
                    
                    if (WlanQueryInterface(hClient, &pIfInfo->InterfaceGuid,
                        wlan_intf_opcode_current_connection, nullptr,
                        &dwSize, (PVOID*)&pConnAttrs, &opType) == ERROR_SUCCESS) {
                        
                        DOT11_SSID* pSsid = &pConnAttrs->wlanAssociationAttributes.dot11Ssid;
                        if (pSsid->uSSIDLength > 0) {
                            std::string s(reinterpret_cast<char*>(pSsid->ucSSID), pSsid->uSSIDLength);
                            details.ssid = std::wstring(s.begin(), s.end());
                        }
                        details.signal_percent = std::min<uint32_t>(
                            100,
                            pConnAttrs->wlanAssociationAttributes.wlanSignalQuality
                        );
                        // WLAN association rates are reported in Kbits/sec and
                        // match the RX/TX values used by the Tauri implementation.
                        details.receive_link_speed_bps =
                            static_cast<uint64_t>(pConnAttrs->wlanAssociationAttributes.ulRxRate) * 1000ULL;
                        details.transmit_link_speed_bps =
                            static_cast<uint64_t>(pConnAttrs->wlanAssociationAttributes.ulTxRate) * 1000ULL;
                        WlanFreeMemory(pConnAttrs);
                        break;
                    }
                }
            }
            WlanFreeMemory(pIfList);
        }
        WlanCloseHandle(hClient, nullptr);
    }
    return details;
}

struct ProcessorPowerInformationRecord {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
};

static std::wstring TrimWhitespace(std::wstring value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t character) {
        return std::iswspace(character) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t character) {
        return std::iswspace(character) != 0;
    }).base();
    return first < last ? std::wstring(first, last) : L"";
}

static std::wstring NormalizeWindowsProductName(std::wstring product_name, const std::wstring& build_number) {
    product_name = TrimWhitespace(std::move(product_name));
    const unsigned long build = wcstoul(build_number.c_str(), nullptr, 10);
    if (build >= 22000 && product_name.rfind(L"Windows 10", 0) == 0) {
        product_name.replace(0, wcslen(L"Windows 10"), L"Windows 11");
    }
    return product_name;
}

static float QueryCurrentCpuSpeedGhz(uint32_t processor_count) {
    if (processor_count == 0) {
        return 0.0f;
    }

    std::vector<ProcessorPowerInformationRecord> processors(processor_count);
    const LONG status = CallNtPowerInformation(
        ProcessorInformation,
        nullptr,
        0,
        processors.data(),
        static_cast<ULONG>(processors.size() * sizeof(ProcessorPowerInformationRecord))
    );
    if (status != 0) {
        return 0.0f;
    }

    uint64_t total_mhz = 0;
    uint32_t measured_processors = 0;
    for (const ProcessorPowerInformationRecord& processor : processors) {
        if (processor.CurrentMhz > 0) {
            total_mhz += processor.CurrentMhz;
            ++measured_processors;
        }
    }
    return measured_processors > 0
        ? static_cast<float>(total_mhz) / static_cast<float>(measured_processors * 1000)
        : 0.0f;
}

MetricsManager::MetricsManager(GpuMonitor::InitializationHook before_gpu_initialize)
    : gpu_monitor_(std::move(before_gpu_initialize)) {
    uptime_seconds_ = 0;

    // Get CPU Core Count
    SYSTEM_INFO sys_info;
    ::GetSystemInfo(&sys_info);
    cpu_core_count_ = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (cpu_core_count_ == 0) {
        cpu_core_count_ = sys_info.dwNumberOfProcessors;
    }
    InitializeSystemInfo();

    InitCpuPdh();
    Update();
}

MetricsManager::~MetricsManager() {
    if (pdh_cpu_initialized_ && pdh_cpu_query_) {
        PdhCloseQuery(static_cast<HQUERY>(pdh_cpu_query_));
    }
}

void MetricsManager::InitializeSystemInfo() {
    constexpr const wchar_t* kWindowsVersionKey = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    constexpr const wchar_t* kCpuKey = L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";

    const std::wstring product_name = TrimWhitespace(ReadRegistryString(
        HKEY_LOCAL_MACHINE, kWindowsVersionKey, L"ProductName"
    ));
    const std::wstring build_number = TrimWhitespace(ReadRegistryString(
        HKEY_LOCAL_MACHINE, kWindowsVersionKey, L"CurrentBuildNumber"
    ));
    system_info_.os_name = NormalizeWindowsProductName(product_name, build_number);
    if (system_info_.os_name.empty()) {
        system_info_.os_name = L"Windows";
    }
    if (!build_number.empty()) {
        system_info_.os_name += L" (" + build_number + L")";
    }

    SYSTEM_INFO native_info{};
    GetNativeSystemInfo(&native_info);
    switch (native_info.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
        system_info_.architecture = L"x86_64";
        break;
    case PROCESSOR_ARCHITECTURE_ARM64:
        system_info_.architecture = L"ARM64";
        break;
    case PROCESSOR_ARCHITECTURE_INTEL:
        system_info_.architecture = L"x86";
        break;
    default:
        system_info_.architecture = L"N/A";
        break;
    }

    wchar_t hostname[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD hostname_length = ARRAYSIZE(hostname);
    if (GetComputerNameW(hostname, &hostname_length) != FALSE) {
        system_info_.hostname.assign(hostname, hostname_length);
    }

    const std::wstring cpu_name = TrimWhitespace(
        ReadRegistryString(HKEY_LOCAL_MACHINE, kCpuKey, L"ProcessorNameString")
    );
    if (!cpu_name.empty()) {
        system_info_.cpu_name = cpu_name;
    }

    // Registry MHz is only a fallback. UpdateSystemInfo supplies the live clock.
    const DWORD mhz = ReadRegistryDword(HKEY_LOCAL_MACHINE, kCpuKey, L"~MHz");
    if (mhz > 0) {
        system_info_.cpu_speed_ghz = static_cast<float>(mhz) / 1000.0f;
    }
}

void MetricsManager::UpdateSystemInfo() {
    const float current_speed_ghz = QueryCurrentCpuSpeedGhz(cpu_core_count_);
    if (current_speed_ghz > 0.0f) {
        system_info_.cpu_speed_ghz = current_speed_ghz;
    }
}

void MetricsManager::Update() {
    UpdateCpu();
    UpdateSystemInfo();
    UpdateRam();
    UpdateDisk();
    UpdateNetwork();
    UpdateUptime();
}

void MetricsManager::UpdateCpu() {
    const ULONGLONG now_ms = GetTickCount64();
    const auto action = cpu_sample_cadence_.Advance(now_ms);
    if (action == Telemetry::CpuSampleAction::Skip) return;
    if (action == Telemetry::CpuSampleAction::Prime) {
        cpu_time_sampler_.Reset();
        pdh_cpu_primed_ = false;
    }
    cpu_.sample_available = false;

    // Keep native time baselines warm even while PDH works. A broken or newly
    // primed PDH query must not freeze the display or starve the fallback.
    std::optional<double> native_percent;
    FILETIME idle_time{}, kernel_time{}, user_time{};
    if (GetSystemTimes(&idle_time, &kernel_time, &user_time)) {
        const auto ticks = [](const FILETIME& time) {
            return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        };
        native_percent = cpu_time_sampler_.Sample(ticks(idle_time), ticks(kernel_time), ticks(user_time));
    } else {
        cpu_time_sampler_.Reset();
    }

    // Failed performance libraries should not be reopened on every tick.
    if (!pdh_cpu_initialized_ &&
        (last_cpu_pdh_retry_ms_ == 0 || now_ms - last_cpu_pdh_retry_ms_ >= 30000)) {
        last_cpu_pdh_retry_ms_ = now_ms;
        InitCpuPdh();
    }

    std::optional<double> percent;
    if (pdh_cpu_initialized_ && pdh_cpu_query_ && pdh_cpu_counter_) {
        const HQUERY query = static_cast<HQUERY>(pdh_cpu_query_);
        bool failed = PdhCollectQueryData(query) != ERROR_SUCCESS;
        if (!failed && !pdh_cpu_primed_) {
            // Rate counters need two collections, separated by a real interval.
            pdh_cpu_primed_ = true;
        } else if (!failed) {
            PDH_FMT_COUNTERVALUE value{};
            const auto status = PdhGetFormattedCounterValue(
                static_cast<HCOUNTER>(pdh_cpu_counter_), PDH_FMT_DOUBLE, nullptr, &value);
            failed = status != ERROR_SUCCESS ||
                !Telemetry::ValidPdhCpuSample(value.CStatus, value.doubleValue);
            if (!failed) percent = value.doubleValue;
        }
        if (failed) {
            PdhCloseQuery(query);
            pdh_cpu_query_ = nullptr;
            pdh_cpu_counter_ = nullptr;
            pdh_cpu_initialized_ = false;
            pdh_cpu_primed_ = false;
            last_cpu_pdh_retry_ms_ = now_ms;
        }
    }

    // GetSystemTimes only covers the calling processor group on >64-thread
    // systems. Never mislabel that partial fallback as whole-machine usage.
    if (!percent && GetActiveProcessorGroupCount() == 1) percent = native_percent;
    if (percent) {
        cpu_.percent_used = static_cast<float>(std::clamp(*percent, 0.0, 100.0));
        cpu_.sample_available = true;
        cpu_.sampled_at_ms = now_ms;
    }
}

void MetricsManager::InitCpuPdh() {
    if (pdh_cpu_query_) {
        PdhCloseQuery(static_cast<HQUERY>(pdh_cpu_query_));
    }
    pdh_cpu_query_ = nullptr;
    pdh_cpu_counter_ = nullptr;
    pdh_cpu_initialized_ = false;
    pdh_cpu_primed_ = false;

    HQUERY query = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS || !query) {
        return;
    }

    // Measure busy processor TIME, not frequency-scaled Processor Utility.
    // Utility can roughly double the result under turbo boost. Updated Windows
    // 11 Task Manager uses standard utilization too (KB5053656, March 2025).
    // English paths work regardless of the Windows display language; _Total
    // covers all logical processors, including multiple processor groups.
    const bool single_group = GetActiveProcessorGroupCount() == 1;
    for (const wchar_t* path : Telemetry::cpu_time_counter_paths) {
        // The legacy Processor object can be group-limited too. On >64-thread
        // systems require Processor Information, never a partial fallback.
        if (!single_group && path != Telemetry::cpu_time_counter_paths[0]) continue;
        HCOUNTER counter = nullptr;
        if (PdhAddEnglishCounterW(query, path, 0, &counter) == ERROR_SUCCESS && counter) {
            pdh_cpu_query_ = query;
            pdh_cpu_counter_ = counter;
            pdh_cpu_initialized_ = true;
            return;
        }
    }

    PdhCloseQuery(query);
}

void MetricsManager::UpdateRam() {
    ram_.sample_available = false;
    MEMORYSTATUSEX mem_status{};
    mem_status.dwLength = sizeof(mem_status);
    if (GlobalMemoryStatusEx(&mem_status) && mem_status.ullTotalPhys > 0 && mem_status.ullAvailPhys <= mem_status.ullTotalPhys) {
        ram_.sample_available = true;
        ram_.sampled_at_ms = GetTickCount64();
        ram_.total_bytes = mem_status.ullTotalPhys;
        ram_.used_bytes = mem_status.ullTotalPhys - mem_status.ullAvailPhys;
        
        if (ram_.total_bytes > 0) {
            ram_.percent_used = (static_cast<float>(ram_.used_bytes) / ram_.total_bytes) * 100.0f;
        } else {
            ram_.percent_used = 0.0f;
        }
    }
}

void MetricsManager::UpdateDisk() {
    ULARGE_INTEGER free_bytes_avail, total_bytes, total_free_bytes;
    if (GetDiskFreeSpaceExW(L"C:\\", &free_bytes_avail, &total_bytes, &total_free_bytes)) {
        disk_.total_bytes = total_bytes.QuadPart;
        disk_.free_bytes = total_free_bytes.QuadPart;
        uint64_t used = total_bytes.QuadPart - total_free_bytes.QuadPart;
        disk_.percent_used = (static_cast<float>(used) / total_bytes.QuadPart) * 100.0f;
    }
}

void MetricsManager::UpdateNetwork() {
    const std::wstring monitored_adapter_id = GetMonitoredAdapterId();
    const bool adapter_locked = !monitored_adapter_id.empty();
    const uint32_t best_index = adapter_locked
        ? FindInterfaceIndexById(monitored_adapter_id)
        : FindBestInterfaceIndex();
    const bool interface_changed = best_index != active_interface_index_;
    active_interface_index_ = best_index;
    network_.connected = false;
    const ULONGLONG current_time = GetTickCount64();

    if (best_index == 0) {
        // A manual lock must never silently fall through to another route.
        // Auto remains the only mode that follows Windows' default interface.
        network_initialized_ = false;
        last_rx_bytes_ = 0;
        last_tx_bytes_ = 0;
        last_network_time_ms_ = 0;
        session_start_rx_ = 0;
        session_start_tx_ = 0;
        network_.download_speed_bps = 0.0;
        network_.upload_speed_bps = 0.0;
        network_.total_received_bytes = 0;
        network_.total_sent_bytes = 0;
        network_.session_received_bytes = 0;
        network_.session_sent_bytes = 0;
        network_.adapter_name = adapter_locked
            ? L"Selected adapter unavailable"
            : L"No Active Connection";
        network_.adapter_index = 0;
        network_.status = L"NORMAL";
        network_.adapter_type = adapter_locked ? L"Other" : L"Ethernet";
        network_.wifi_ssid = L"";
        network_.link_speed_bps = 0;
        network_.receive_link_speed_bps = 0;
        network_.wifi_signal_percent = 0;
        network_.local_ip = L"127.0.0.1";
        network_.subnet_mask = L"N/A";
        network_.ipv6_address = L"N/A";
        network_.mac_address = L"N/A";
        network_.dhcp_enabled = false;
        adapter_details_interface_index_ = 0;
        return;
    }

    MIB_IF_ROW2 row = {};
    row.InterfaceIndex = best_index;

    if (GetIfEntry2(&row) == NO_ERROR) {
        const bool interface_connected = IsInterfaceConnected(row);
        network_.adapter_name = row.Description;
        network_.adapter_index = best_index;
        network_.link_speed_bps = row.TransmitLinkSpeed;
        network_.receive_link_speed_bps = row.ReceiveLinkSpeed;
        network_.local_ip = GetLocalIPAddress(best_index);

        network_.adapter_type = ClassifyInterfaceType(row);
        if (network_.adapter_type != L"Wi-Fi") {
            network_.wifi_ssid = L"";
            network_.wifi_signal_percent = 0;
        }

        // Adapter addresses come from IP Helper, not the WLAN API. They are
        // inexpensive and do not trigger Windows' Wi-Fi location indicator.
        if (adapter_details_interface_index_ != best_index ||
            current_time - last_adapter_details_refresh_ms_ >= 20ULL * 60ULL * 1000ULL) {
            RefreshAdapterDetails(best_index);
            adapter_details_interface_index_ = best_index;
            last_adapter_details_refresh_ms_ = current_time;
        }

        uint64_t current_rx = row.InOctets;
        uint64_t current_tx = row.OutOctets;

        network_.connected = interface_connected;
        if (!interface_connected) {
            // Keep the lock's identity and hardware details visible, but do
            // not treat stale counters from a disconnected interface as live
            // traffic. Reconnection primes a fresh baseline below.
            network_initialized_ = false;
            last_rx_bytes_ = current_rx;
            last_tx_bytes_ = current_tx;
            last_network_time_ms_ = current_time;
            session_start_rx_ = current_rx;
            session_start_tx_ = current_tx;
            network_.download_speed_bps = 0.0;
            network_.upload_speed_bps = 0.0;
            network_.total_received_bytes = current_rx;
            network_.total_sent_bytes = current_tx;
            network_.session_received_bytes = 0;
            network_.session_sent_bytes = 0;
            network_.status = L"NORMAL";
            return;
        }

        if (interface_changed) {
            // GetIfEntry2 counters are per adapter. Prime a new baseline when
            // Auto moves between routes, or a user changes the manual lock.
            // The first reading is intentionally zero; the next one-second
            // interval is then measured against the right adapter.
            network_initialized_ = false;
            last_rx_bytes_ = current_rx;
            last_tx_bytes_ = current_tx;
            last_network_time_ms_ = current_time;
            session_start_rx_ = current_rx;
            session_start_tx_ = current_tx;
        }

        if (session_start_rx_ == 0) {
            session_start_rx_ = current_rx;
        }
        if (session_start_tx_ == 0) {
            session_start_tx_ = current_tx;
        }

        if (network_initialized_ && last_network_time_ms_ > 0 && current_time > last_network_time_ms_) {
            const uint64_t elapsed_ms = current_time - last_network_time_ms_;
            network_.download_speed_bps = Telemetry::CounterBitsPerSecond(current_rx, last_rx_bytes_, elapsed_ms);
            network_.upload_speed_bps = Telemetry::CounterBitsPerSecond(current_tx, last_tx_bytes_, elapsed_ms);
        } else {
            network_initialized_ = true;
            network_.download_speed_bps = 0.0;
            network_.upload_speed_bps = 0.0;
        }

        last_rx_bytes_ = current_rx;
        last_tx_bytes_ = current_tx;
        last_network_time_ms_ = current_time;
        
        network_.total_received_bytes = current_rx;
        network_.total_sent_bytes = current_tx;

        if (current_rx >= session_start_rx_) {
            network_.session_received_bytes = current_rx - session_start_rx_;
        } else {
            network_.session_received_bytes = 0;
        }

        if (current_tx >= session_start_tx_) {
            network_.session_sent_bytes = current_tx - session_start_tx_;
        } else {
            network_.session_sent_bytes = 0;
        }

        double total_bps = network_.download_speed_bps + network_.upload_speed_bps;
        if (total_bps > (10.0 * 1024.0 * 1024.0)) {
            network_.status = L"HEAVY";
        } else {
            network_.status = L"NORMAL";
        }
    } else {
        network_initialized_ = false;
        network_.connected = false;
        network_.download_speed_bps = 0.0;
        network_.upload_speed_bps = 0.0;
        network_.adapter_name = adapter_locked
            ? L"Selected adapter unavailable"
            : L"No Active Connection";
    }
}

void MetricsManager::RefreshAdapterDetails(uint32_t interface_index) {
    ULONG buffer_size = 0;
    constexpr ULONG flags = GAA_FLAG_INCLUDE_PREFIX;
    if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, nullptr, &buffer_size) != ERROR_BUFFER_OVERFLOW ||
        buffer_size == 0) {
        return;
    }

    std::vector<BYTE> buffer(buffer_size);
    auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses, &buffer_size) != NO_ERROR) {
        return;
    }

    for (auto* adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->IfIndex != interface_index && adapter->Ipv6IfIndex != interface_index) {
            continue;
        }

        if (adapter->FriendlyName != nullptr && adapter->FriendlyName[0] != L'\0') {
            network_.adapter_name = adapter->FriendlyName;
        }
        network_.mac_address = FormatMacAddress(adapter->PhysicalAddress, adapter->PhysicalAddressLength);
        network_.dhcp_enabled = adapter->Dhcpv4Enabled != 0;
        network_.subnet_mask = L"N/A";
        network_.ipv6_address = L"N/A";

        for (auto* address = adapter->FirstUnicastAddress; address != nullptr; address = address->Next) {
            const SOCKADDR* socket_address = address->Address.lpSockaddr;
            if (socket_address == nullptr) {
                continue;
            }

            if (socket_address->sa_family == AF_INET) {
                const std::wstring ipv4 = SocketAddressToString(socket_address);
                if (ipv4 != L"N/A") {
                    network_.local_ip = ipv4;
                }

                ULONG mask = 0;
                if (ConvertLengthToIpv4Mask(address->OnLinkPrefixLength, &mask) == NO_ERROR) {
                    IN_ADDR mask_address{};
                    mask_address.S_un.S_addr = mask;
                    wchar_t mask_text[INET_ADDRSTRLEN]{};
                    if (InetNtopW(AF_INET, &mask_address, mask_text, ARRAYSIZE(mask_text))) {
                        network_.subnet_mask = mask_text;
                    }
                }
            } else if (socket_address->sa_family == AF_INET6 && network_.ipv6_address == L"N/A") {
                network_.ipv6_address = SocketAddressToString(socket_address);
            }
        }
        return;
    }
}

void MetricsManager::UpdateUptime() {
    // This is Windows boot uptime, matching Tauri's System::uptime(), rather
    // than the age of the Watchman process.
    uptime_seconds_ = GetTickCount64() / 1000;
}

void MetricsManager::ResetSession() {
    session_start_rx_ = last_rx_bytes_;
    session_start_tx_ = last_tx_bytes_;
    network_.session_received_bytes = 0;
    network_.session_sent_bytes = 0;
}

void MetricsManager::SetMonitoredAdapterId(const std::wstring& adapter_id) {
    std::lock_guard<std::mutex> lock(monitored_adapter_mutex_);
    monitored_adapter_id_ = adapter_id;
}

std::wstring MetricsManager::GetMonitoredAdapterId() const {
    std::lock_guard<std::mutex> lock(monitored_adapter_mutex_);
    return monitored_adapter_id_;
}

uint32_t MetricsManager::FindBestInterfaceIndex() {
    DWORD best_index = 0;
    if (GetBestInterface(0x08080808, &best_index) == NO_ERROR && best_index != 0) {
        return best_index;
    }
    if (GetBestInterface(0x01010101, &best_index) == NO_ERROR && best_index != 0) {
        return best_index;
    }
    return 0;
}

uint32_t MetricsManager::FindInterfaceIndexById(const std::wstring& adapter_id) const {
    if (adapter_id.empty()) {
        return 0;
    }

    PMIB_IF_TABLE2 table = nullptr;
    uint32_t result = 0;
    if (GetIfTable2(&table) == NO_ERROR && table != nullptr) {
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const MIB_IF_ROW2& row = table->Table[i];
            if (_wcsicmp(FormatInterfaceGuid(row.InterfaceGuid).c_str(), adapter_id.c_str()) == 0) {
                result = row.InterfaceIndex;
                break;
            }
        }
        FreeMibTable(table);
    }
    return result;
}

std::vector<InterfaceInfo> MetricsManager::GetInterfaces() const {
    std::vector<InterfaceInfo> list;
    const std::wstring selected_adapter_id = GetMonitoredAdapterId();
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) == NO_ERROR && table != nullptr) {
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const MIB_IF_ROW2& row = table->Table[i];
            if (!IsAdapterPickerCandidate(row, selected_adapter_id)) {
                continue;
            }

            InterfaceInfo info;
            info.index = row.InterfaceIndex;
            info.id = FormatInterfaceGuid(row.InterfaceGuid);
            info.name = row.Alias;
            info.description = row.Description;
            info.type = ClassifyInterfaceType(row);
            info.is_active = (row.OperStatus == IfOperStatusUp);
            info.is_connected = IsInterfaceConnected(row);
            info.speed_bps = std::max(row.TransmitLinkSpeed, row.ReceiveLinkSpeed);
            list.push_back(info);
        }
        FreeMibTable(table);
    }

    std::sort(list.begin(), list.end(), [](const InterfaceInfo& left, const InterfaceInfo& right) {
        if (left.is_connected != right.is_connected) {
            return left.is_connected > right.is_connected;
        }
        if (left.is_active != right.is_active) {
            return left.is_active > right.is_active;
        }
        if (left.type != right.type) {
            return left.type < right.type;
        }
        return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
    });
    return list;
}

void MetricsManager::RefreshNetworkDetails() {
    if (network_.adapter_type == L"Wi-Fi") {
        MIB_IF_ROW2 row{};
        row.InterfaceIndex = network_.adapter_index;
        const GUID* target_interface_guid =
            GetIfEntry2(&row) == NO_ERROR ? &row.InterfaceGuid : nullptr;
        const WifiConnectionDetails details = QueryWifiConnectionDetails(target_interface_guid);
        network_.wifi_ssid = details.ssid;
        network_.wifi_signal_percent = details.signal_percent;
        if (details.receive_link_speed_bps > 0) {
            network_.receive_link_speed_bps = details.receive_link_speed_bps;
        }
        if (details.transmit_link_speed_bps > 0) {
            network_.link_speed_bps = details.transmit_link_speed_bps;
        }
    } else {
        network_.wifi_ssid.clear();
        network_.wifi_signal_percent = 0;
    }
}
