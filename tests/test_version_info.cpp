// Each shipped binary carries Windows version information: the product, the
// build's version label and its own file name (al_version_resource).
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
std::wstring text(const std::vector<unsigned char>& block, const wchar_t* name) {
    void* value = nullptr;
    UINT size = 0;
    const std::wstring path = std::wstring(L"\\StringFileInfo\\040904b0\\") + name;
    if (!VerQueryValueW(block.data(), path.c_str(), &value, &size) || !value || !size) return {};
    return static_cast<const wchar_t*>(value);
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    unsigned failures = 0;
    for (int i = 1; i < argc; ++i) {
        const std::wstring file = argv[i];
        const auto fail = [&](const char* why) { std::wcerr << file << L": "; std::cerr << why << '\n'; ++failures; };
        DWORD ignored = 0;
        const auto size = GetFileVersionInfoSizeW(file.c_str(), &ignored);
        if (!size) { fail("no version information"); continue; }
        std::vector<unsigned char> block(size);
        if (!GetFileVersionInfoW(file.c_str(), 0, size, block.data())) { fail("version information unreadable"); continue; }
        VS_FIXEDFILEINFO* fixed = nullptr;
        UINT length = 0;
        if (!VerQueryValueW(block.data(), L"\\", reinterpret_cast<void**>(&fixed), &length) || !fixed ||
            fixed->dwSignature != VS_FFI_SIGNATURE) { fail("no fixed version block"); continue; }
        if (HIWORD(fixed->dwFileVersionMS) != AL_TEST_VERSION_MAJOR || LOWORD(fixed->dwFileVersionMS) != AL_TEST_VERSION_MINOR ||
            HIWORD(fixed->dwFileVersionLS) != AL_TEST_VERSION_PATCH) fail("numeric file version differs from the project's");
        if (text(block, L"ProductName") != L"Astera Loader") fail("ProductName is not Astera Loader");
        if (text(block, L"FileVersion") != AL_BUILD_VERSION) fail("FileVersion differs from the build's version label");
        if (text(block, L"OriginalFilename") != std::filesystem::path(file).filename().wstring()) fail("OriginalFilename differs from the file");
        if (text(block, L"FileDescription").empty() || text(block, L"LegalCopyright").empty()) fail("no description or copyright");
        // The launcher shows the logo in Explorer and the taskbar.
        if (std::filesystem::path(file).extension() == L".exe") {
            const auto module = LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
            if (!module || !FindResourceW(module, MAKEINTRESOURCEW(1), RT_GROUP_ICON)) fail("no icon");
            if (module) FreeLibrary(module);
        }
    }
    if (failures) return 1;
    std::cout << "Version information present and consistent in " << (argc - 1) << " shipped binaries\n";
    return 0;
}
