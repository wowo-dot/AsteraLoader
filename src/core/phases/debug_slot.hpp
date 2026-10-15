#pragma once
#include <windows.h>
#include <optional>

namespace al::phases {
struct DebugSlot { unsigned index; DWORD64 address, control, target; };
inline DWORD64& debug_address(CONTEXT& context,unsigned slot) noexcept {
    switch(slot) { case 0:return context.Dr0; case 1:return context.Dr1; case 2:return context.Dr2; default:return context.Dr3; }
}
inline std::optional<DebugSlot> reserve_debug_slot(CONTEXT& context,DWORD64 target) noexcept {
    // Windows owns global-enable/general-detect bits when setting a context.
    // Refuse such state instead of risking normalization of another owner.
    if ((context.Dr7 & (0xaau | 0x2000u)) || (context.EFlags & 0x100u)) return {};
    for (unsigned slot=0; slot<4; ++slot) {
        const auto enable=DWORD64{3}<<(slot*2), control=DWORD64{15}<<(16+slot*4);
        if (context.Dr7 & (enable|control)) continue;
        DebugSlot saved{slot,debug_address(context,slot),context.Dr7 & (enable|control),target};
        debug_address(context,slot)=target;
        context.Dr7|=DWORD64{1}<<(slot*2);
        return saved;
    }
    return {};
}
// Puts back the slot's state from before reserve_debug_slot, unchecked.
inline void restore_debug_slot(CONTEXT& context,const DebugSlot& saved) noexcept {
    const auto enable=DWORD64{3}<<(saved.index*2), control=DWORD64{15}<<(16+saved.index*4);
    debug_address(context,saved.index)=saved.address;
    context.Dr7=(context.Dr7 & ~(enable|control)) | saved.control;
    context.Dr6 &= ~(DWORD64{1}<<saved.index);
}
inline bool release_debug_slot(CONTEXT& context,const DebugSlot& saved) noexcept {
    const auto enable=DWORD64{3}<<(saved.index*2), control=DWORD64{15}<<(16+saved.index*4);
    if (debug_address(context,saved.index)!=saved.target ||
        (context.Dr7 & (enable|control))!=(DWORD64{1}<<(saved.index*2))) return false;
    restore_debug_slot(context,saved);
    return true;
}

// What an exception context's Dr6 says about a hit at the slot's address:
// the slot (Windows); no breakpoint at all (Wine, whose exception contexts
// can show a stale copy of debug registers set from another thread); or
// another slot or a single step, which is not ours.
enum class SlotHit { shown, unreported, other };
inline SlotHit slot_hit(DWORD64 dr6,unsigned slot) noexcept {
    const auto own=DWORD64{1}<<slot;
    if (dr6 & ((0xf ^ own) | 0xe000u)) return SlotHit::other;
    return (dr6 & own) ? SlotHit::shown : SlotHit::unreported;
}
}  // namespace al::phases
