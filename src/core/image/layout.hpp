#pragma once

// Where a range of bytes lies in the game exe's image, for the services that
// change it ("al.hooks", "al.patch"): its code, its other sections, or a part
// AL never changes (the headers, the data directories, the DRM stub's
// section). Reads the PE headers once; changes nothing.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace al::image {

enum class Area {
    outside,    // not in the image, or across a section boundary
    headers,    // the PE headers
    code,       // an executable section
    data,       // any other section, read-only or writable
    directory,  // a data directory: imports, exports, exception data, ...
    drm,        // the DRM stub's section (".bind")
};

const char* area_name(Area area) noexcept;

class Layout {
public:
    explicit Layout(const void* image);

    bool valid() const noexcept { return valid_; }
    const std::uint8_t* base() const noexcept { return base_; }
    // The one area every byte of [address, address + size) is in; `outside`
    // for an empty range, one leaving the image or crossing sections.
    Area area(const void* address, std::size_t size) const noexcept;
    // Whether the range lies in a section whose pages are writable as mapped.
    bool writable(const void* address, std::size_t size) const noexcept;
    // Bytes from `address` to the end of its section, at most `most`.
    std::size_t room(const void* address, std::size_t most) const noexcept;
    std::uintptr_t rva(const void* address) const noexcept;
    // Executable sections, [begin, end) as addresses.
    const std::vector<std::pair<std::uintptr_t, std::uintptr_t>>& code() const noexcept { return code_; }

private:
    struct Section {
        std::uintptr_t begin = 0, end = 0;
        bool code = false, writable = false, drm = false;
    };
    const Section* section_of(std::uintptr_t begin, std::uintptr_t end) const noexcept;

    const std::uint8_t* base_ = nullptr;
    bool valid_ = false;
    std::uintptr_t headers_end_ = 0;
    std::uintptr_t image_end_ = 0;
    std::vector<Section> sections_;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> directories_;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> code_;
};

}  // namespace al::image
