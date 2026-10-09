#pragma once

// A fresh work folder for one test run under `root`. Windows reuses process
// IDs, so a folder named by the PID alone can still hold an earlier run's
// files; copying a fixture over one then throws, and the test dies with
// 0xc0000409 instead of reporting a check.
#include <windows.h>

#include <filesystem>
#include <string>

namespace cmfm_test {
inline std::filesystem::path fresh_work_dir(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    for (unsigned attempt = 0;; ++attempt) {
        auto work = root / (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(attempt));
        if (std::filesystem::create_directory(work)) return work;
    }
}
}  // namespace cmfm_test
