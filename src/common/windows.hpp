#pragma once

#include <windows.h>

#include <string>

namespace al {

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    HANDLE get() const { return value_; }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    void reset(HANDLE value = nullptr) {
        if (*this) CloseHandle(value_);
        value_ = value;
    }

private:
    HANDLE value_ = nullptr;
};

inline std::wstring module_path(HMODULE module = nullptr) {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    path.resize(length);
    return path;
}

inline std::wstring final_path(HANDLE file, DWORD volume = VOLUME_NAME_DOS) {
    std::wstring path(32768, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(file, path.data(),
                                                  static_cast<DWORD>(path.size()), volume);
    if (!length || length >= path.size()) return {};
    path.resize(length);
    return path;
}

inline std::wstring environment(const wchar_t* name) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (!needed) return {};
    std::wstring value(needed, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(), needed);
    if (!length || length >= needed) return {};
    value.resize(length);
    return value;
}

inline bool regular_file(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace al
