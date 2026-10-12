#include "core/image/layout.hpp"

#include "common/memory.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace al::image {

const char* area_name(Area area) noexcept {
    switch (area) {
    case Area::headers: return "the exe's headers";
    case Area::code: return "the game's code";
    case Area::data: return "the game's data";
    case Area::directory: return "a data directory (imports, exports, exception data, relocations, resources, TLS, "
                                 "load config or debug)";
    case Area::drm: return "the DRM stub's section";
    case Area::outside: break;
    }
    return "outside the game exe, or across two of its sections";
}

Layout::Layout(const void* image) : base_(static_cast<const std::uint8_t*>(image)) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base_);
    if (!base_ || !readable(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base_ + dos->e_lfanew);
    if (!readable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return;
    }
    const auto origin = reinterpret_cast<std::uintptr_t>(base_);
    headers_end_ = origin + nt->OptionalHeader.SizeOfHeaders;
    image_end_ = origin + nt->OptionalHeader.SizeOfImage;
    const auto* section = IMAGE_FIRST_SECTION(nt);
    if (!readable(section, sizeof(IMAGE_SECTION_HEADER) * nt->FileHeader.NumberOfSections)) return;
    for (WORD k = 0; k < nt->FileHeader.NumberOfSections; ++k, ++section) {
        Section entry;
        entry.begin = origin + section->VirtualAddress;
        entry.end = entry.begin + (section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData);
        entry.code = (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        entry.writable = (section->Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
        char name[IMAGE_SIZEOF_SHORT_NAME + 1]{};
        std::memcpy(name, section->Name, IMAGE_SIZEOF_SHORT_NAME);
        // Steam's DRM stub lives in ".bind"; AL leaves it alone.
        entry.drm = std::strcmp(name, ".bind") == 0;
        sections_.push_back(entry);
        if (entry.code && !entry.drm) code_.emplace_back(entry.begin, entry.end);
    }
    // Every data directory but the certificate table, whose address is a
    // file offset, not part of the mapped image.
    for (DWORD k = 0; k < nt->OptionalHeader.NumberOfRvaAndSizes && k < IMAGE_NUMBEROF_DIRECTORY_ENTRIES; ++k) {
        if (k == IMAGE_DIRECTORY_ENTRY_SECURITY) continue;
        const auto& entry = nt->OptionalHeader.DataDirectory[k];
        if (!entry.VirtualAddress || !entry.Size) continue;
        directories_.emplace_back(origin + entry.VirtualAddress, origin + entry.VirtualAddress + entry.Size);
    }
    valid_ = true;
}

const Layout::Section* Layout::section_of(std::uintptr_t begin, std::uintptr_t end) const noexcept {
    for (const auto& section : sections_) {
        if (begin >= section.begin && end <= section.end) return &section;
    }
    return nullptr;
}

Area Layout::area(const void* address, std::size_t size) const noexcept {
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto end = begin + size;
    if (!valid_ || !size || end < begin || begin < reinterpret_cast<std::uintptr_t>(base_) || end > image_end_) {
        return Area::outside;
    }
    if (begin < headers_end_) return Area::headers;
    for (const auto& [first, last] : directories_) {
        if (begin < last && end > first) return Area::directory;
    }
    const auto* section = section_of(begin, end);
    if (!section) return Area::outside;
    if (section->drm) return Area::drm;
    return section->code ? Area::code : Area::data;
}

bool Layout::writable(const void* address, std::size_t size) const noexcept {
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const auto* section = section_of(begin, begin + size);
    return section && section->writable;
}

std::size_t Layout::room(const void* address, std::size_t most) const noexcept {
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    for (const auto& section : sections_) {
        if (at >= section.begin && at < section.end) return (std::min)(most, section.end - at);
    }
    return 0;
}

std::uintptr_t Layout::rva(const void* address) const noexcept {
    return reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(base_);
}

}  // namespace al::image
