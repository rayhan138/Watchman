#pragma once

#include <windows.h>
#include <utility>

namespace Watchman {

// RAII wrapper for generic Win32 HANDLEs that close via CloseHandle.
class ScopedHandle {
public:
    ScopedHandle() noexcept : handle_(INVALID_HANDLE_VALUE) {}
    explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}

    ~ScopedHandle() noexcept {
        Close();
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept : handle_(other.Release()) {}

    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            Reset(other.Release());
        }
        return *this;
    }

    bool IsValid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    explicit operator bool() const noexcept {
        return IsValid();
    }

    HANDLE Get() const noexcept {
        return handle_;
    }

    HANDLE* Put() noexcept {
        Close();
        return &handle_;
    }

    HANDLE Release() noexcept {
        HANDLE temp = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return temp;
    }

    void Reset(HANDLE new_handle = INVALID_HANDLE_VALUE) noexcept {
        if (handle_ != new_handle) {
            Close();
            handle_ = new_handle;
        }
    }

    void Close() noexcept {
        if (IsValid()) {
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE handle_;
};

// RAII wrapper for Windows Registry HKEYs that close via RegCloseKey.
class ScopedHKey {
public:
    ScopedHKey() noexcept : hkey_(nullptr) {}
    explicit ScopedHKey(HKEY hkey) noexcept : hkey_(hkey) {}

    ~ScopedHKey() noexcept {
        Close();
    }

    ScopedHKey(const ScopedHKey&) = delete;
    ScopedHKey& operator=(const ScopedHKey&) = delete;

    ScopedHKey(ScopedHKey&& other) noexcept : hkey_(other.Release()) {}

    ScopedHKey& operator=(ScopedHKey&& other) noexcept {
        if (this != &other) {
            Reset(other.Release());
        }
        return *this;
    }

    bool IsValid() const noexcept {
        return hkey_ != nullptr;
    }

    explicit operator bool() const noexcept {
        return IsValid();
    }

    HKEY Get() const noexcept {
        return hkey_;
    }

    HKEY* Put() noexcept {
        Close();
        return &hkey_;
    }

    HKEY Release() noexcept {
        HKEY temp = hkey_;
        hkey_ = nullptr;
        return temp;
    }

    void Reset(HKEY new_hkey = nullptr) noexcept {
        if (hkey_ != new_hkey) {
            Close();
            hkey_ = new_hkey;
        }
    }

    void Close() noexcept {
        if (hkey_ != nullptr) {
            ::RegCloseKey(hkey_);
            hkey_ = nullptr;
        }
    }

private:
    HKEY hkey_;
};

} // namespace Watchman
