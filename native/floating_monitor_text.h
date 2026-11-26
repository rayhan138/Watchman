#pragma once

#include <array>

// GDI and DirectWrite use the same OpenType weight values. Keep the saved
// choice explicit: an absent/invalid new key means Light, never the old
// hard-coded Bold. Size and the user's existing colors are not migrated/reset.
namespace FloatingMonitorText {
enum class Weight : int { Light = 300, Regular = 400, Bold = 700 };
inline constexpr Weight default_weight = Weight::Light;
inline constexpr std::array<Weight, 3> weights{Weight::Light, Weight::Regular, Weight::Bold};
inline constexpr const wchar_t* family = L"Segoe UI";

constexpr Weight FromSaved(int value) {
    switch (value) {
    case 400: return Weight::Regular;
    case 700: return Weight::Bold;
    default: return default_weight;
    }
}

constexpr int Value(Weight weight) {
    return static_cast<int>(FromSaved(static_cast<int>(weight)));
}

// Legacy GDI style-linking does not reliably expose Light through the base
// Segoe UI family. Name that installed face explicitly; DirectWrite still uses
// the Segoe UI family + 300. Native tests require tmWeight == the chosen weight.
constexpr const wchar_t* GdiFamily(Weight weight) {
    return FromSaved(static_cast<int>(weight)) == Weight::Light ? L"Segoe UI Light" : family;
}

constexpr const wchar_t* Label(Weight weight) {
    switch (FromSaved(static_cast<int>(weight))) {
    case Weight::Regular: return L"Regular";
    case Weight::Bold: return L"Bold";
    default: return L"Light";
    }
}
} // namespace FloatingMonitorText
