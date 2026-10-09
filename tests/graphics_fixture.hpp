#pragma once
#include <cstdint>

enum GraphicsToken : unsigned {
    ProbeCreated = 1, OwnerCreated, ProbePre, OwnerPre, PresentDetour, ProbePost, OwnerPost,
    ProbePreResize, OwnerPreResize, ResizeDetour, ProbePostResize, OwnerPostResize,
    ProbeQueue, OwnerQueue, FactoryDetour, ListDetour, DeviceDetour, SignalDetour, ExistingSignal,
    SwapchainPhase, FirstPresentPhase, ShutdownPhase, Invalid = 99
};
using GraphicsRecord = void (*)(unsigned, void*, std::uintptr_t, std::uintptr_t);
