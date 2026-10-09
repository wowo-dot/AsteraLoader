#pragma once
#include <cstdint>

using GameDamage = double (*)(std::uint64_t, double, std::uint64_t, double, std::uint64_t, double, std::uint64_t, double);
using GameSecond = int (*)(int);
using GameLater = int (*)(int, int, int, int, int, int, int, int);
using GameRecord = void (*)(unsigned);
constexpr double damage_result = 1 + 2.25 * 3 + 3 * 5 - 4.5 * 7 + 5 * 11 + 6.75 * 13 + 7 * 17 - 8.125 * 19 + 23;
