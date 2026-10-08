// The plugin header must build as C++, with the same layout as C.
#include "abi_layout.h"

int main() {
    return CMFM_PHASE_BIT(CMFM_PHASE_GAME_READY) == 0x20u ? 0 : 1;
}
