#include "temperature_provider.h"
#include <windows.data.json.h>
#include <roapi.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

namespace WatchmanSensors {
namespace {
// Use the native WinRT ABI, not C++/WinRT's coroutine projection. This keeps
// Watchman's C++17 baseline and avoids deprecated experimental coroutine headers.
using namespace ABI::Windows::Data::Json;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HString;
using Microsoft::WRL::Wrappers::HStringReference;

struct Hardware {
    std::optional<Kind> kind;
    std::wstring id;
    std::wstring name;
};

std::wstring String(const HString& value) {
    UINT32 length = 0;
    const wchar_t* text = value.GetRawBuffer(&length);
    return text ? std::wstring(text, length) : std::wstring{};
}
std::wstring StringProperty(IJsonObject* node, const wchar_t* key) {
    HString text;
    if (FAILED(node->GetNamedString(HStringReference(key).Get(), text.GetAddressOf()))) return {};
    return String(text);
}
ComPtr<IJsonValue> Property(IJsonObject* node, const wchar_t* key) {
    ComPtr<IJsonValue> value;
    if (FAILED(node->GetNamedValue(HStringReference(key).Get(), &value))) return {};
    return value;
}
JsonValueType Type(IJsonValue* value) {
    JsonValueType type = JsonValueType_Null;
    if (value) value->get_ValueType(&type);
    return type;
}

std::optional<double> Temperature(IJsonObject* node) {
    auto raw = Property(node, L"RawValue");
    if (raw) {
        // A present null is unavailable, not permission to reuse a formatted
        // minimum/maximum or stale-looking field from elsewhere in the object.
        if (Type(raw.Get()) == JsonValueType_Number) {
            double value = 0.0;
            if (SUCCEEDED(raw->GetNumber(&value)) && ValidCelsius(value)) return value;
        } else if (Type(raw.Get()) == JsonValueType_String) {
            HString text;
            if (SUCCEEDED(raw->GetString(text.GetAddressOf()))) return ParseTemperatureText(String(text));
        }
        return {};
    }
    return ParseTemperatureText(StringProperty(node, L"Value"));
}

bool Visit(IJsonObject* node, Hardware hardware, std::vector<Candidate>& sensors,
           size_t depth, size_t& budget) {
    if (depth > 24 || budget == 0) return false;
    --budget;
    const std::wstring id = StringProperty(node, L"HardwareId");
    const auto kind = HardwareKind(id, StringProperty(node, L"ImageURL"));
    if (kind) hardware = {*kind, id, StringProperty(node, L"Text")};

    if (StringProperty(node, L"Type") == L"Temperature") {
        const std::wstring sensor_id = StringProperty(node, L"SensorId");
        const auto id_kind = HardwareKind(sensor_id, L"");
        const auto sensor_kind = id_kind ? id_kind : hardware.kind;
        const auto value = Temperature(node);
        if (sensor_kind && value && !sensor_id.empty()) {
            // Older LHM versions omit HardwareId. SensorId still identifies
            // the physical device without relying on translated display text.
            const size_t marker = sensor_id.rfind(L"/temperature/");
            const std::wstring hardware_id = hardware.id.empty()
                ? sensor_id.substr(0, marker) : hardware.id;
            sensors.push_back({*sensor_kind, hardware_id, hardware.name, StringProperty(node, L"Text"), *value});
        }
    }
    const auto children = Property(node, L"Children");
    if (!children) return true;
    if (Type(children.Get()) != JsonValueType_Array) return false;
    ComPtr<IJsonArray> array;
    ComPtr<ABI::Windows::Foundation::Collections::IVector<IJsonValue*>> vector;
    UINT32 size = 0;
    if (FAILED(children->GetArray(&array)) || FAILED(array.As(&vector)) || FAILED(vector->get_Size(&size)) || size > budget) return false;
    for (UINT32 i = 0; i < size; ++i) {
        ComPtr<IJsonObject> child;
        if (FAILED(array->GetObjectAt(i, &child)) || !Visit(child.Get(), hardware, sensors, depth + 1, budget)) return false;
    }
    return true;
}
} // namespace

bool ParseLibreHardwareMonitorJson(const std::wstring& json, std::vector<Candidate>& sensors) {
    sensors.clear();
    if (json.empty() || json.size() > 1024 * 1024) return false;
    ComPtr<IJsonObjectStatics> factory;
    if (FAILED(RoGetActivationFactory(HStringReference(L"Windows.Data.Json.JsonObject").Get(), IID_PPV_ARGS(&factory)))) return false;
    HString input;
    if (FAILED(input.Set(json.data(), static_cast<UINT32>(json.size())))) return false;
    ComPtr<IJsonObject> root;
    boolean parsed = false;
    if (FAILED(factory->TryParse(input.Get(), &root, &parsed)) || !parsed || !root || StringProperty(root.Get(), L"Text") != L"Sensor") return false;
    const auto children = Property(root.Get(), L"Children");
    if (!children || Type(children.Get()) != JsonValueType_Array) return false;
    std::vector<Candidate> candidates;
    size_t budget = 4096;
    if (!Visit(root.Get(), {}, candidates, 0, budget)) return false;
    sensors.swap(candidates);
    return true;
}
} // namespace WatchmanSensors
