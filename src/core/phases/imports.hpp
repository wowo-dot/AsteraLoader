#pragma once

// The game's own import slots: the pointers its code calls through for an
// imported function. CMFM observes a call by swapping one such pointer, a
// data change in the game's import table that chains through whatever the
// function itself is (hooked by another mod or not) instead of stacking a
// second inline patch on it.

#include <cstdint>
#include <optional>
#include <string_view>

namespace cmfm::phases {

struct ImportSlot {
    void** slot = nullptr;  // the pointer the game's code calls through
    bool delayed = false;   // a delay-load import: the slot first holds the
                            // exe's own loader thunk, which resolves the
                            // function and overwrites the slot on first call
};

// The import slot in `image` (a mapped PE32+ module) for `function` from
// `dll`, by name, or by `ordinal` when `function` is empty. DLL names compare
// case-insensitively. Normal imports are searched first, then delay imports.
// Every read is bounds- and readability-checked; nothing is written.
std::optional<ImportSlot> find_import(const void* image, std::string_view dll, std::string_view function,
                                      std::uint16_t ordinal = 0) noexcept;

// Writes `value` into `slot` if it still holds `expected`, changing the
// page protection only for the write. False if the slot changed meanwhile or
// the protection could not be changed.
bool swap_slot(void** slot, void* expected, void* value) noexcept;

}  // namespace cmfm::phases
