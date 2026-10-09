#include "core/phases/imports.hpp"

#include "common/memory.hpp"

#include <windows.h>

#include <cstring>

namespace al::phases {
namespace {
// Bounds-checked views into one mapped image.
class Image {
public:
    explicit Image(const void* base) noexcept : base_(static_cast<const std::uint8_t*>(base)) {
        if (!base_ || !readable(base_, sizeof(IMAGE_DOS_HEADER))) return;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base_);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER)) ||
            dos->e_lfanew > 0x100000) return;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base_ + dos->e_lfanew);
        if (!readable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 ||
            nt->FileHeader.SizeOfOptionalHeader!=sizeof(IMAGE_OPTIONAL_HEADER64) ||
            nt->OptionalHeader.SizeOfImage < static_cast<std::uint64_t>(dos->e_lfanew)+sizeof(*nt)) return;
        nt_ = nt;
        size_ = nt->OptionalHeader.SizeOfImage;
    }

    bool valid() const noexcept { return nt_ != nullptr; }

    IMAGE_DATA_DIRECTORY directory(unsigned index) const noexcept {
        return index < IMAGE_NUMBEROF_DIRECTORY_ENTRIES && index < nt_->OptionalHeader.NumberOfRvaAndSizes ? nt_->OptionalHeader.DataDirectory[index]
                                                               : IMAGE_DATA_DIRECTORY{};
    }

    template <typename T>
    const T* at(std::uint64_t rva, std::uint64_t count = 1) const noexcept {
        if (!rva || count == 0 || rva >= size_ || count > (size_ - rva) / sizeof(T)) return nullptr;
        const auto* pointer = base_ + rva;
        return readable(pointer, static_cast<std::size_t>(count * sizeof(T))) ? reinterpret_cast<const T*>(pointer)
                                                                               : nullptr;
    }

    // A NUL-terminated name inside the image, at most 255 characters.
    std::string_view name(std::uint64_t rva) const noexcept {
        for (std::uint64_t length = 0; length < 256; ++length) {
            const auto* c = at<char>(rva + length);
            if (!c) return {};
            if (!*c) return {reinterpret_cast<const char*>(base_ + rva),static_cast<std::size_t>(length)};
        }
        return {};
    }

    void** slot(std::uint64_t rva) const noexcept {
        return const_cast<void**>(at<void*>(rva));
    }

private:
    const std::uint8_t* base_ = nullptr;
    const IMAGE_NT_HEADERS64* nt_ = nullptr;
    std::uint64_t size_ = 0;
};

bool same_dll(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        auto x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

// Index of the wanted entry in a name table (an array of thunks ending in 0).
std::optional<std::uint64_t> find_entry(const Image& image, std::uint64_t names, std::string_view function,
                                        std::uint16_t ordinal) noexcept {
    for (std::uint64_t index = 0; index < 0x10000; ++index) {
        const auto* thunk = image.at<std::uint64_t>(names + index * sizeof(std::uint64_t));
        if (!thunk || !*thunk) return std::nullopt;
        if (IMAGE_SNAP_BY_ORDINAL64(*thunk)) {
            if (function.empty() && IMAGE_ORDINAL64(*thunk) == ordinal) return index;
            continue;
        }
        if (!function.empty() && *thunk<=UINT32_MAX && image.name(*thunk + sizeof(WORD)) == function) return index;
    }
    return std::nullopt;
}
}  // namespace

std::optional<ImportSlot> find_import(const void* base, std::string_view dll, std::string_view function,
                                      std::uint16_t ordinal) noexcept {
    const Image image(base);
    if (!image.valid() || dll.empty() || (function.empty() && !ordinal)) return std::nullopt;

    const auto imports = image.directory(IMAGE_DIRECTORY_ENTRY_IMPORT);
    for (std::uint64_t i = 0; imports.VirtualAddress && i<imports.Size/sizeof(IMAGE_IMPORT_DESCRIPTOR); ++i) {
        const auto* descriptor =
            image.at<IMAGE_IMPORT_DESCRIPTOR>(imports.VirtualAddress + i * sizeof(IMAGE_IMPORT_DESCRIPTOR));
        if (!descriptor || !descriptor->Name) break;
        // Without a separate name table the names are gone once bound.
        if (!descriptor->OriginalFirstThunk || !same_dll(image.name(descriptor->Name), dll)) continue;
        if (const auto index = find_entry(image, descriptor->OriginalFirstThunk, function, ordinal)) {
            if (auto* slot = image.slot(descriptor->FirstThunk + *index * sizeof(void*))) return ImportSlot{slot, false};
        }
    }

    const auto delayed = image.directory(IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT);
    for (std::uint64_t i = 0; delayed.VirtualAddress && i<delayed.Size/sizeof(IMAGE_DELAYLOAD_DESCRIPTOR); ++i) {
        const auto* descriptor = image.at<IMAGE_DELAYLOAD_DESCRIPTOR>(delayed.VirtualAddress +
                                                                       i * sizeof(IMAGE_DELAYLOAD_DESCRIPTOR));
        if (!descriptor || !descriptor->DllNameRVA) break;
        // Old-style descriptors hold addresses instead of RVAs; MSVC has not
        // produced them for decades.
        if (!descriptor->Attributes.RvaBased || !same_dll(image.name(descriptor->DllNameRVA), dll)) continue;
        if (const auto index = find_entry(image, descriptor->ImportNameTableRVA, function, ordinal)) {
            if (auto* slot = image.slot(descriptor->ImportAddressTableRVA + *index * sizeof(void*))) {
                return ImportSlot{slot, true};
            }
        }
    }
    return std::nullopt;
}

bool swap_slot(void** slot, void* expected, void* value) noexcept {
    if (!slot || reinterpret_cast<std::uintptr_t>(slot)%alignof(void*) || !readable(slot, sizeof(void*))) return false;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    const bool swapped = InterlockedCompareExchangePointer(slot, value, expected) == expected;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    return swapped;
}

}  // namespace al::phases
