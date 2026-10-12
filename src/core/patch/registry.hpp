#pragma once

// "al.patch" (API 1.8), core side: finds and checks each change, claims its
// bytes in the claims it shares with "al.hooks", and decides when a change is
// written and when it is put back. Writing is the writer's job (Writer); the
// registry never touches the game's bytes itself.

#include "al/al_plugin.h"
#include "common/pattern.hpp"
#include "core/graphics/hub.hpp"  // Client
#include "core/image/claims.hpp"
#include "core/image/layout.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace al::patch {

using Client = graphics::Client;

// Writes to the game's image.
class Writer {
public:
    virtual ~Writer() = default;
    enum class Result { written, differs, failed };
    // Writes `bytes` over the `size` bytes at `at`, only if they equal
    // `expect`. On `differs`, `found` gets the bytes that were there and
    // nothing was written. `code`: the bytes may be running on other threads.
    // On `failed`, `problem` says why.
    virtual Result write(std::uint8_t* at, const std::uint8_t* expect, const std::uint8_t* bytes, std::size_t size,
                         bool code, std::uint8_t* found, std::string& problem) = 0;
    // Places a stub within a rel32's reach of `site` that jumps to
    // `function`. "" or why not.
    virtual std::string stub(const void* site, void* function, void** stub) = 0;
};

// Searches the game exe for a pattern: its code (true) or all of it (false).
using Finder = std::function<std::vector<const void*>(const Pattern&, bool code)>;

class Registry {
public:
    Registry(const void* image, Finder find, image::Claims& claims);
    ~Registry();
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    // Checks and claims one change; its handle, or 0 with the reason in
    // `problem`. Written now if the writer is ready, else when it is.
    std::uint32_t apply(Client& client, const al_patch_t& patch, std::string& problem);
    // "" or why not.
    std::string set_enabled(const Client& client, std::uint32_t handle, bool enabled);
    al_patch_state_t state(const Client& client, std::uint32_t handle) const;

    // The writer is ready (PRE_MAIN ended): writes the pending changes;
    // later ones are written at once.
    void set_writer(Writer* writer);
    // SHUTDOWN, after the plugins' own: puts back the DATA changes still
    // active. Code and call changes stay.
    void finish();
    // One line per change, for the core log.
    std::vector<std::string> summary() const;

private:
    struct Change;
    enum class Outcome { done, differs, failed };
    // Writes the replacement (true) or the expected bytes (false), with the
    // lock held. Updates the state and the crash table.
    Outcome write(Change& change, bool enable, std::string& problem);
    // Whether `pattern` matches at `at`, against the game's original bytes.
    bool matches_at(const Pattern& pattern, const std::uint8_t* at, image::Area area) const;
    void fail(Change& change, const std::string& problem);

    image::Layout layout_;
    Finder find_;
    image::Claims& claims_;
    mutable std::mutex mutex_;
    Writer* writer_ = nullptr;
    std::vector<std::unique_ptr<Change>> changes_;
    std::uint32_t next_handle_ = 1;
};

// "XX XX XX" for messages, at most `most` bytes.
std::string hex_bytes(const std::uint8_t* bytes, std::size_t size, std::size_t most = 16);

}  // namespace al::patch
