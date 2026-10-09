#pragma once
#include <windows.h>
#include <optional>

namespace cmfm::phases {
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
inline bool release_debug_slot(CONTEXT& context,const DebugSlot& saved) noexcept {
    const auto enable=DWORD64{3}<<(saved.index*2), control=DWORD64{15}<<(16+saved.index*4);
    if (debug_address(context,saved.index)!=saved.target ||
        (context.Dr7 & (enable|control))!=(DWORD64{1}<<(saved.index*2))) return false;
    debug_address(context,saved.index)=saved.address;
    context.Dr7=(context.Dr7 & ~(enable|control)) | saved.control;
    context.Dr6 &= ~(DWORD64{1}<<saved.index);
    return true;
}
}  // namespace cmfm::phases
