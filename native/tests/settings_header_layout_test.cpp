#include "settings_header_layout.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
float Width(const SettingsHeaderLayout::Rect& rect) { return rect.right - rect.left; }
float Height(const SettingsHeaderLayout::Rect& rect) { return rect.bottom - rect.top; }
}

int main() {
    try {
        using namespace SettingsHeaderLayout;
        for (float modal_width : {360.0f, 380.0f, 400.0f, 420.0f, 440.0f, 480.0f, 500.0f}) {
            for (float x : {-10.0f, 0.0f, 140.0f}) {
                for (float y : {0.0f, 40.0f, 200.0f}) {
                    for (float version_width : {95.0f, 108.0f, 130.0f, 180.0f, 320.0f}) {
                        const float actions_left = x + modal_width - 142.0f;
                        const auto header = Get(x, y, actions_left, 108, version_width);
                        for (const auto& rect : {header.title, header.version, header.version_text}) {
                            Check(Width(rect) > 0 && Height(rect) > 0, "Header text and badge need positive bounds");
                            Check(rect.left >= x + 22 && rect.right <= actions_left - gap,
                                "The version must not overlap Quick tour, close or modal edges");
                            Check(rect.top >= y && rect.bottom < y + height,
                                "All header content must stay above the existing divider");
                        }
                        Check(Width(header.title) >= 108, "SETTINGS must keep its full measured text width");
                        Check(header.version_text.left >= header.version.left + horizontal_padding &&
                              header.version_text.right <= header.version.right - horizontal_padding,
                            "The version label needs internal badge padding");
                        Check(header.version_font_scale > 0 && header.version_font_scale <= 1,
                            "Long resource versions may shrink, never overflow or inflate normal type");
                        Check(version_width * header.version_font_scale <= Width(header.version_text) + 0.01f,
                            "The complete measured version must fit, including future build revisions");
                        if (header.stacked) {
                            Check(header.title.bottom < header.version.top,
                                "Compact header rows must not overlap");
                        } else {
                            Check(header.title.right + gap <= header.version.left,
                                "Inline version and SETTINGS need a visible gap");
                            Check(header.version_font_scale == 1, "Normal inline badges must retain readable 17-DIP type");
                        }
                    }
                }
            }
        }
        Check(!Get(0, 0, 500 - 142, 108, 108).stacked, "Default Settings must put the badge beside Quick tour");
        Check(!Get(0, 0, 440 - 142, 108, 108).stacked, "Minimum supported main window must retain the prominent badge");
        Check(Get(0, 0, 360 - 142, 108, 108).stacked, "Extra-narrow dialogs must use the two-row fallback");
        Check(Get(0, 0, 440 - 142, 108, 320).stacked, "Long future versions must not push buttons out of the header");
        std::cout << "Settings version header geometry tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
