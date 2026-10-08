#pragma once

#include <string>
#include <string_view>

namespace cmfm {

std::string to_utf8(std::wstring_view text);

// The text Windows gives for a Win32 error code, without the trailing newline.
std::wstring win32_error_text(unsigned long code);

// A UTF-8 log file, written through on every line so nothing is lost when
// the process ends abruptly.
class LogFile {
public:
    LogFile() = default;
    LogFile(const LogFile&) = delete;
    LogFile& operator=(const LogFile&) = delete;
    ~LogFile();

    // Creates the folder if needed and truncates the file.
    bool open(const std::wstring& path);
    bool is_open() const;
    bool good() const;
    void line(std::wstring_view text);
    void close();

private:
    void* handle_ = nullptr;
    bool good_ = false;
};

}  // namespace cmfm
