#include "app_version.h"

#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

int main() {
    try {
        using namespace WatchmanVersion;
        Check(FormatLabel({2, 0, 0, 0}) == L"Version 2.0.0", "Show all three release components");
        Check(FormatLabel({2, 0, 1, 0}) == L"Version 2.0.1", "Format the previous stable release");
        Check(FormatLabel({2, 0, 2, 0}) == L"Version 2.0.2", "Format the current candidate release metadata");
        Check(FormatLabel({12, 34, 56, 7}) == L"Version 12.34.56.7", "Do not hide a nonzero build revision");
        Check(FormatLabel({0, 0, 0, 0}) == L"Version 0.0.0", "Do not substitute an invented release version");
        Check(FormatLabel({65535, 65535, 65535, 65535}) == L"Version 65535.65535.65535.65535",
            "Preserve the complete Windows resource version range");
#ifdef WATCHMAN_VERSION_RESOURCE_TEST
        Check(Current() && Current()->major == 12 && Current()->minor == 34 && Current()->patch == 56 && Current()->revision == 7,
            "Expose the same genuine ProductVersion to the updater");
        Check(Label() == L"Version 12.34.56.7", "Read this EXE's ProductVersion, not FileVersion or a hard-coded number");
#else
        Check(!Current(), "Never compare an unversioned binary as invented version zero");
        Check(Label() == unavailable_label, "An unversioned executable must use the honest unavailable fallback");
#endif
        Check(&Label() == &Label(), "Cache the process's version rather than reading disk on every frame");
        std::cout << "App version formatting, resource/fallback and caching tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
