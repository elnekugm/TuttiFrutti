// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Settings {

// Ocarina of Time (Switch 2). Its own upscaler lands the scene on 1440p docked and 1080p
// handheld; Settings::values.ocarina_resolution picks the scale instead (UpdateRescalingInfo).
inline constexpr u64 OcarinaOfTimeProgramId = 0x0400486024DEC000ULL;
inline constexpr u64 MarioKartWorldProgramId = 0x0400C3F00006E000ULL;
inline constexpr u64 HyruleWarriorsProgramId = 0x04003F40232DE000ULL;
inline constexpr u64 DonkeyKongBananzaProgramId = 0x040074A01BF12000ULL;

} // namespace Settings
