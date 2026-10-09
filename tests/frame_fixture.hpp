#pragma once
#include <cstdint>

enum FrameToken : unsigned { OuterBegin = 1, Begin, InnerBegin, Body, InnerEnd, End, OuterEnd,
    TaskInitial, TaskFinal, Exiting, ExitRoutine, GameReady, Invalid = 99 };
using FrameRecord = void (*)(unsigned, std::uint64_t);
using FrameUpdate = std::uintptr_t (*)(void*, void*, void*, void*);
