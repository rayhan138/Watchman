#include "app_traffic_presentation.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}
int main() {
    try {
        AppActivityItem small, large, upload_only;
        small.name = L"Small live app";
        small.total_download_bytes = 75;
        small.total_upload_bytes = 25;
        small.current_download_bps = 1000000;
        large.name = L"Largest idle app";
        large.total_download_bytes = 200;
        upload_only.name = L"Upload only";
        upload_only.total_upload_bytes = 50;
        std::vector<AppActivityItem> apps{small, upload_only, large};
        std::sort(apps.begin(), apps.end(), AppTrafficPresentation::MostUsageFirst);
        Check(apps[0].name == large.name && apps[1].name == small.name && apps[2].name == upload_only.name,
            "Rank must use session bytes, not current rate");
        const auto split = AppTrafficPresentation::UsageBar(small, 200);
        Check(std::abs(split.download - .375) < 1e-9 && std::abs(split.upload - .125) < 1e-9,
            "Bar must preserve split and scale to largest user");
        const auto upload = AppTrafficPresentation::UsageBar(upload_only, 200);
        Check(upload.download == 0 && upload.upload == .25, "Upload-only bar incorrect");
        const auto empty = AppTrafficPresentation::UsageBar({}, 0);
        Check(empty.download == 0 && empty.upload == 0, "Empty data must not divide by zero");
        auto same = small;
        same.name = L"A tie";
        Check(AppTrafficPresentation::MostUsageFirst(same, small), "Name must stabilize equal-usage ordering");
        same = small;
        same.pid = 1;
        small.pid = 2;
        Check(AppTrafficPresentation::MostUsageFirst(same, small), "PID must stabilize equal-name ordering");
        Check(!AppTrafficPresentation::MostUsageFirst(same, same), "Ordering must be strict");
        const auto maximum = (std::numeric_limits<uint64_t>::max)();
        large.total_download_bytes = maximum;
        large.total_upload_bytes = maximum;
        Check(AppTrafficPresentation::SessionBytes(large) == maximum, "Session total must saturate, not wrap");
        const auto saturated = AppTrafficPresentation::UsageBar(large, maximum);
        Check(std::abs(saturated.download + saturated.upload - 1) < 1e-9, "Extreme bar must stay inside track");
        std::cout << "App traffic presentation checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
