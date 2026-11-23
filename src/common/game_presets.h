// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Settings {

// Ocarina of Time (Switch 2). Its own upscaler lands the scene on 1440p docked and 1080p
// handheld; Settings::values.ocarina_resolution picks the scale instead (UpdateRescalingInfo).
inline constexpr u64 OcarinaOfTimeProgramId = 0x0400486024DEC000ULL;
inline constexpr u64 MarioKartWorldProgramId = 0x0400C3F00006E000ULL;
// Kirby Air Riders: its written compute storage pool is larger than the 8 MiB cap and its baked
// vertex lighting lives in images the collector used to drop (EDEN_NX2_SSBO_WRITTEN_SIZE_WORD and
// EDEN_NX2_TC_KEEP_NEWEST, both on for it by default).
inline constexpr u64 KirbyAirRidersProgramId = 0x040077201BE50000ULL;
// The online test build: the same game code, and the same pool and baked-lighting rules.
inline constexpr u64 KirbyAirRidersTestRideProgramId = 0x04005B902517C000ULL;
inline constexpr u64 HyruleWarriorsProgramId = 0x04003F40232DE000ULL;
inline constexpr u64 DonkeyKongBananzaProgramId = 0x040074A01BF12000ULL;

} // namespace Settings
