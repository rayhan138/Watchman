#include "scoped_handle.h"
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        // Test default state
        Watchman::ScopedHandle handle;
        Check(!handle.IsValid(), "Default handle must be invalid");
        Check(!handle, "Default handle boolean test must be false");

        // Test move semantics
        HANDLE fake_val = reinterpret_cast<HANDLE>(0x1234);
        Watchman::ScopedHandle h1(fake_val);
        Check(h1.IsValid(), "Constructed handle must be valid");
        Check(h1.Get() == fake_val, "Get() must return initial handle");

        Watchman::ScopedHandle h2(std::move(h1));
        Check(!h1.IsValid(), "Moved-from handle must be invalid");
        Check(h2.IsValid(), "Moved-to handle must be valid");
        Check(h2.Get() == fake_val, "Moved-to handle must hold value");

        HANDLE released = h2.Release();
        Check(released == fake_val, "Released handle value must match");
        Check(!h2.IsValid(), "Handle after Release() must be invalid");

        // Test ScopedHKey
        Watchman::ScopedHKey reg_key;
        Check(!reg_key.IsValid(), "Default registry key must be invalid");

        HKEY fake_key = reinterpret_cast<HKEY>(0x5678);
        Watchman::ScopedHKey k1(fake_key);
        Check(k1.IsValid(), "Constructed registry key must be valid");
        Check(k1.Get() == fake_key, "Get() must return initial key");

        Watchman::ScopedHKey k2 = std::move(k1);
        Check(!k1.IsValid(), "Moved-from key must be invalid");
        Check(k2.Get() == fake_key, "Moved-to key must hold value");
        k2.Release(); // Avoid calling RegCloseKey on fake pointer

        std::cout << "Scoped handle tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Scoped handle test failed: " << error.what() << '\n';
        return 1;
    }
}
