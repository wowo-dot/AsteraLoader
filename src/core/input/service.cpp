#include "core/input/service.hpp"
#include "core/plugins/guard.hpp"
#include <windows.h>
#include <algorithm>
#include <cstring>

namespace al::input {
namespace {
bool stopped(const Client& c) noexcept { return c.stopped && c.stopped->load(std::memory_order_relaxed); }
void fault(Client& c, const char* kind, unsigned long code) noexcept {
    try { if (c.disable) c.disable(std::string("its input ") + kind + " callback raised exception " + std::to_string(code)); } catch (...) {}
}
int mouse_bit(std::uint32_t key) noexcept {
    switch (key) { case VK_LBUTTON: return 0; case VK_RBUTTON: return 1; case VK_MBUTTON: return 2;
    case VK_XBUTTON1: return 3; case VK_XBUTTON2: return 4; default: return -1; }
}
}
struct Service::Hotkey {
    Client* client{};
    std::uint32_t handle{}, key{}, modifiers{};
    int pad{-1};
    std::string name;
    al_hotkey_fn callback{};
    void* user{};
    std::atomic<bool> removed{false};
    bool down{}; // frame thread only, combination edge
};
struct Service::Message {
    Client* client{};
    std::int32_t order{};
    al_message_fn callback{};
    void* user{};
    std::atomic<bool> removed{false};
};
struct Service::Capture {
    Client* client{};
    std::vector<std::uint32_t> stack;
    std::atomic<std::uint32_t> mask{0};
};
struct Service::List { std::vector<Hotkey*> keys; std::vector<Message*> messages; std::vector<Capture*> captures; };
Service::Service() = default;
Service::~Service() = default;
void Service::focus(bool focused) noexcept {
    if(focused_.exchange(focused) != focused) focus_epoch_.fetch_add(1);
}
void Service::set_native(std::function<void(void*)> window, std::function<void(std::uint32_t)> begin, std::function<void()> end) { attach_=std::move(window); begin_=std::move(begin); end_=std::move(end); }
void Service::attach_window(void* hwnd) { if(wanted() && attach_) attach_(hwnd); }
void Service::begin_frame(std::uint32_t thread) noexcept { try { if(wanted() && state_!=AL_INPUT_STOPPED && begin_) begin_(thread); } catch (...) {} }
void Service::publish() {
    auto list = std::make_unique<List>();
    for (auto& k : keys_) if (!k->removed) list->keys.push_back(k.get());
    for (auto& m : messages_) if (!m->removed) list->messages.push_back(m.get());
    for (auto& c : captures_) list->captures.push_back(c.get());
    std::stable_sort(list->keys.begin(), list->keys.end(), [](auto a, auto b) { return a->client->load_index < b->client->load_index; });
    std::stable_sort(list->messages.begin(), list->messages.end(), [](auto a, auto b) {
        return a->order != b->order ? a->order < b->order : a->client->load_index < b->client->load_index;
    });
    const auto raw = list.get(); kept_.push_back(std::move(list)); list_.store(raw, std::memory_order_release);
}
std::uint32_t Service::add(Client& c, std::uint32_t key, std::uint32_t modifiers, const char* name,
                          al_hotkey_fn fn, void* user, std::string& problem, int pad) {
    problem.clear(); request();
    if (!fn || (modifiers & ~7u) || (pad < 0 ? !key || key > 255 : pad > 3 || !key || (key & ~0xf3ffu))) {
        problem = "invalid key, pad buttons, modifiers or callback"; return 0;
    }
    std::lock_guard lock(mutex_);
    auto k = std::make_unique<Hotkey>(); k->client = &c; k->handle = next_handle_++;
    k->key = key; k->modifiers = modifiers; k->pad = pad;
    k->name = name ? std::string(name, strnlen(name, 128)) : ""; k->callback = fn; k->user = user;
    const auto handle = k->handle;
    for (const auto& old : keys_) if (!old->removed && old->key == key && old->modifiers == modifiers && old->pad == pad) {
        const auto text = "al.input: hotkey conflict between " + old->client->name + " and " + c.name;
        if (c.log) c.log("WARN", text); if (old->client->log) old->client->log("WARN", text);
    }
    keys_.push_back(std::move(k));
    try { publish(); } catch (...) { keys_.pop_back(); throw; }
    return handle;
}
std::string Service::remove(const Client& c, std::uint32_t handle) {
    std::lock_guard lock(mutex_);
    for (auto& k : keys_) if (k->handle == handle) {
        if (k->client != &c) return "the hotkey belongs to another plugin";
        const auto old=k->removed.exchange(true);
        try { publish(); } catch (...) { k->removed=old; throw; }
        return {};
    }
    return "unknown hotkey handle";
}
std::string Service::messages(Client& c, std::int32_t order, al_message_fn fn, void* user) {
    request(); std::lock_guard lock(mutex_);
    std::unique_ptr<Message> m;
    if (fn) { m = std::make_unique<Message>(); m->client = &c; m->order = order; m->callback = fn; m->user = user; }
    std::vector<Message*> old;
    for(auto& item:messages_) if(item->client==&c && !item->removed) old.push_back(item.get());
    const auto adding=m != nullptr;
    if(m) messages_.push_back(std::move(m));
    for(auto item:old) item->removed=true;
    try { publish(); } catch (...) { for(auto item:old) item->removed=false; if(adding) messages_.pop_back(); throw; }
    return {};
}
Service::Capture& Service::owner(Client& c) {
    for (auto& item : captures_) if (item->client == &c) return *item;
    auto item = std::make_unique<Capture>(); item->client = &c;
    captures_.push_back(std::move(item));
    try { publish(); } catch (...) { captures_.pop_back(); throw; }
    return *captures_.back();
}
std::string Service::capture(Client& c, std::uint32_t mask) {
    if (!mask || (mask & ~AL_INPUT_ALL)) return "invalid capture devices";
    std::lock_guard lock(mutex_);
    if (stopped(c) || state_ != AL_INPUT_RUNNING || (mask & devices_.load()) != mask) return "requested input paths are not available";
    auto& item = owner(c); item.stack.push_back(mask); item.mask.fetch_or(mask); return {};
}
std::string Service::release(Client& c) {
    std::lock_guard lock(mutex_);
    for (auto& item : captures_) if (item->client == &c) {
        if (item->stack.empty()) return "this plugin holds no capture";
        item->stack.pop_back(); std::uint32_t mask = 0; for (auto held : item->stack) mask |= held;
        item->mask.store(mask); return {};
    }
    return "this plugin holds no capture";
}
void Service::cancel(const Client& c) noexcept {
    try { std::lock_guard lock(mutex_); for (auto& item : captures_) if (item->client == &c) { item->mask = 0; item->stack.clear(); } } catch (...) {}
}
std::uint32_t Service::capture_mask() const noexcept {
    if (!focused() || state_.load() != AL_INPUT_RUNNING) return 0;
    const auto* list = list_.load(std::memory_order_acquire); std::uint32_t mask = 0;
    if (list) for (auto c : list->captures) if (!stopped(*c->client)) mask |= c->mask.load(std::memory_order_relaxed);
    return mask;
}
bool Service::other_capture(const Client& client) const noexcept {
    const auto* list = list_.load(std::memory_order_acquire);
    if (list) for (auto c : list->captures) if (c->client != &client && !stopped(*c->client) && c->mask.load()) return true;
    return false;
}
bool Service::key_down(std::uint32_t key) const noexcept {
    if (!focused() || state_.load() != AL_INPUT_RUNNING || key > 255) return false;
    const auto button = mouse_bit(key);
    const auto epoch=focus_epoch_.load();
    if (button >= 0) return mouse_epoch_.load()==epoch && (mouse_.load(std::memory_order_relaxed) & (1u << button)) != 0;
    if(keyboard_epoch_.load()!=epoch) return false;
    return (keyboard_[key / 64].load(std::memory_order_relaxed) & (1ull << (key % 64))) != 0;
}
void Service::keyboard(const std::uint32_t* held) noexcept {
    const auto epoch=focus_epoch_.load();
    std::array<std::uint64_t, 4> bits{};
    for (unsigned i = 0; i < 8; ++i) bits[i / 2] |= static_cast<std::uint64_t>(held[i]) << (32 * (i % 2));
    for (const auto pair : {std::array<unsigned,3>{VK_CONTROL,VK_LCONTROL,VK_RCONTROL},
                            std::array<unsigned,3>{VK_SHIFT,VK_LSHIFT,VK_RSHIFT}, std::array<unsigned,3>{VK_MENU,VK_LMENU,VK_RMENU}}) {
        if ((bits[pair[1]/64] & (1ull << (pair[1]%64))) || (bits[pair[2]/64] & (1ull << (pair[2]%64)))) bits[pair[0]/64] |= 1ull << (pair[0]%64);
    }
    for (unsigned i = 0; i < 4; ++i) keyboard_[i].store(bits[i], std::memory_order_relaxed);
    keyboard_epoch_.store(epoch);
}
void Service::mouse(std::uint32_t buttons) noexcept {
    const auto epoch=focus_epoch_.load();
    mouse_.store(buttons & 0x1f, std::memory_order_relaxed);
    mouse_epoch_.store(epoch);
}
void Service::pad(std::uint32_t slot, const al_pad_state_t& data) noexcept {
    if (slot >= 4) return;
    const auto epoch=focus_epoch_.load();
    const auto raw = reinterpret_cast<const unsigned char*>(&data);
    // Atomic payload words and a sequence give readers a coherent snapshot
    // without a data race or a lock in the device call (one frame-thread writer).
    std::uint64_t payload{}; std::memcpy(&payload, raw + offsetof(al_pad_state_t, buttons), 8);
    std::uint32_t tail{}; std::memcpy(&tail, raw + offsetof(al_pad_state_t, right_x), 4);
    const auto a = data.packet | (static_cast<std::uint64_t>(tail) << 32);
    auto& p = pads_[slot]; p.sequence.fetch_add(1, std::memory_order_seq_cst);
    p.a.store(a, std::memory_order_seq_cst); p.b.store(payload, std::memory_order_seq_cst);
    p.connected.store(data.connected != 0, std::memory_order_seq_cst);
    p.epoch.store(epoch,std::memory_order_seq_cst);
    p.sequence.fetch_add(1, std::memory_order_seq_cst);
}
bool Service::pad_state(std::uint32_t slot, al_pad_state_t& out) const noexcept {
    if (slot >= 4 || out.size < 4) return false;
    al_pad_state_t data{}; data.size = sizeof(data);
    if (focused() && state_.load() == AL_INPUT_RUNNING) {
        const auto& p = pads_[slot]; std::uint32_t before, after; std::uint64_t a, b, epoch;
        do { before = p.sequence.load(std::memory_order_seq_cst); a = p.a.load(std::memory_order_seq_cst);
            b = p.b.load(std::memory_order_seq_cst); data.connected = p.connected.load(std::memory_order_seq_cst); epoch=p.epoch.load(std::memory_order_seq_cst);
            after = p.sequence.load(std::memory_order_seq_cst); } while ((before & 1) || before != after);
        if(epoch==focus_epoch_.load()) {
            data.packet = static_cast<std::uint32_t>(a);
            std::memcpy(&data.buttons, &b, 8); const auto tail = static_cast<std::uint32_t>(a >> 32); std::memcpy(&data.right_x, &tail, 4);
        } else data.connected=0;
    }
    const auto size = (std::min)(out.size, static_cast<std::uint32_t>(sizeof(data))); std::memcpy(&out, &data, size); out.size = size;
    return true;
}
void Service::end_frame() noexcept {
    try { if(wanted() && state_!=AL_INPUT_STOPPED && end_) end_(); } catch (...) {}
    const auto* list = list_.load(std::memory_order_acquire); if (!list) return;
    const auto epoch=focus_epoch_.load(); const auto new_focus=edge_epoch_!=epoch; edge_epoch_=epoch;
    const auto modifiers = (key_down(VK_CONTROL) ? AL_MOD_CTRL : 0) | (key_down(VK_SHIFT) ? AL_MOD_SHIFT : 0) | (key_down(VK_MENU) ? AL_MOD_ALT : 0);
    for (auto k : list->keys) {
        bool down = false;
        if (k->pad < 0) down = key_down(k->key);
        else { al_pad_state_t p{}; p.size = sizeof(p); pad_state(static_cast<std::uint32_t>(k->pad), p); down = p.connected && (p.buttons & k->key) == k->key; }
        down = down && (modifiers & k->modifiers) == k->modifiers;
        const auto edge = down && !k->down; k->down = down;
        if (!edge || new_focus || k->removed || stopped(*k->client) || other_capture(*k->client)) continue;
        unsigned long code = 0;
        if (!plugins::guarded_hotkey(k->callback, k->client->self, k->handle, k->user, code)) { fault(*k->client, "hotkey", code); cancel(*k->client); }
        else fired_.fetch_add(1, std::memory_order_relaxed);
    }
}
bool Service::message(void* hwnd, std::uint32_t id, std::uintptr_t wp, std::intptr_t lp) noexcept {
    const auto* list = list_.load(std::memory_order_acquire); if (!list || state_ == AL_INPUT_STOPPED) return false;
    for (auto m : list->messages) {
        if (m->removed || stopped(*m->client)) continue;
        unsigned long code = 0; std::uint32_t consumed = 0;
        if (!plugins::guarded_message(m->callback, m->client->self, hwnd, id, wp, lp, m->user, consumed, code)) {
            fault(*m->client, "message", code); cancel(*m->client);
        } else if (consumed) { consumed_.fetch_add(1, std::memory_order_relaxed); return true; }
    }
    return false;
}
void Service::ready(std::uint32_t devices) noexcept { devices_.fetch_or(devices); auto expected = AL_INPUT_WAITING; state_.compare_exchange_strong(expected, AL_INPUT_RUNNING); }
void Service::missing(std::string reason) { std::lock_guard lock(mutex_); if (state_ == AL_INPUT_UNAVAILABLE || state_ == AL_INPUT_STOPPED) return; reason_ = std::move(reason); }
void Service::unavailable(std::string reason) { std::lock_guard lock(mutex_); if (state_ == AL_INPUT_UNAVAILABLE || state_ == AL_INPUT_STOPPED) return; reason_ = std::move(reason); state_.store(AL_INPUT_UNAVAILABLE); }
void Service::stop() noexcept { state_ = AL_INPUT_STOPPED; focused_ = false; }
void Service::status(al_input_status_t& out) const {
    std::lock_guard lock(mutex_); al_input_status_t full{sizeof(full), state_.load(), devices_.load(), capture_mask(), window(), reason_.c_str()};
    const auto size = (std::min)(out.size, static_cast<std::uint32_t>(sizeof(full))); if (size >= 4) { std::memcpy(&out, &full, size); out.size = size; }
}
std::vector<std::string> Service::summary() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> lines{"input_state=" + std::to_string(state_.load()) + " devices=" + std::to_string(devices_.load()) +
        " captured=" + std::to_string(capture_mask()) + " hotkeys_fired=" + std::to_string(fired_.load()) + " messages_consumed=" + std::to_string(consumed_.load())};
    if (!reason_.empty()) lines.push_back((state_.load() == AL_INPUT_UNAVAILABLE ? "input_unavailable reason=" : "input_missing reason=") + reason_);
    return lines;
}
} // namespace al::input
