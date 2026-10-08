#include "common/log_file.hpp"

#include "common/paths.hpp"

#include <windows.h>

namespace cmfm {

std::string to_utf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), size,
                        nullptr, nullptr);
    return utf8;
}

std::wstring win32_error_text(unsigned long code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = length ? std::wstring(buffer, length) : L"unknown error";
    if (buffer) {
        LocalFree(buffer);
    }
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ')) {
        text.pop_back();
    }
    return text + L" (" + std::to_wstring(code) + L")";
}

LogFile::~LogFile() {
    close();
}

bool LogFile::open(const std::wstring& path) {
    close();
    const std::wstring folder(directory_of(path));
    if (!folder.empty()) {
        CreateDirectoryW(folder.c_str(), nullptr);
    }
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    handle_ = file;
    good_ = true;
    return true;
}

bool LogFile::is_open() const {
    return handle_ != nullptr;
}

bool LogFile::good() const {
    return is_open() && good_;
}

void LogFile::line(std::wstring_view text) {
    if (!handle_) {
        return;
    }
    std::string utf8 = to_utf8(text);
    utf8 += "\r\n";
    DWORD written = 0;
    if (!WriteFile(handle_, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr) ||
        written != utf8.size()) good_ = false;
}

void LogFile::close() {
    if (handle_) {
        CloseHandle(handle_);
        handle_ = nullptr;
    }
    good_ = false;
}

}  // namespace cmfm
