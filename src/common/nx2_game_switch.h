// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "common/common_types.h"
#include "common/game_presets.h"
#include "common/logging.h"
#include "common/settings.h"

/// A workaround or optimisation that is on for the titles it was written for, and that a player can
/// turn on or off for any title without a new build:
///   EDEN_NX2_<NAME> ................ everyone
///   EDEN_NX2_<NAME>_<program id> ... that title only (16 hex digits, as AGENTS lists them)
/// The per-title value wins over the global one, which wins over the default. Both come from the
/// environment, which nx2_switches.txt fills at startup (src/lichii/nx2_switches.h), so a line
/// `EDEN_NX2_TC_KEEP_NEWEST_0400C3F00006E000=1` tries Kirby's rule in Mario Kart World.
namespace Common::Nx2 {

using SwitchDefault = bool (*)(u64 program_id);

/// Off unless the build says otherwise (the switch exists so a title can be tried without one).
[[nodiscard]] inline bool OffForAll(u64) {
    return false;
}

[[nodiscard]] inline bool OnForAll(u64) {
    return true;
}

[[nodiscard]] inline bool OnForTitle(u64 program_id, u64 title_id) {
    return program_id == title_id;
}

/// Kirby Air Riders and its online test build (the same game code).
[[nodiscard]] inline bool OnForKirbyAirRiders(u64 program_id) {
    return program_id == Settings::KirbyAirRidersProgramId ||
           program_id == Settings::KirbyAirRidersTestRideProgramId;
}

class GameSwitch {
public:
    constexpr explicit GameSwitch(std::string_view name_, SwitchDefault default_ = OffForAll)
        : name{name_}, default_value{default_} {}

    /// Whether it is on for the running title. Cheap on hot paths: the answer for a program id is
    /// kept until another title runs.
    [[nodiscard]] bool Enabled() const {
        const u64 program_id = Settings::GetCurrentProgramID();
        if (cached_program_id.load(std::memory_order_acquire) == program_id) {
            return cached_value.load(std::memory_order_relaxed);
        }
        const bool value = Read(program_id);
        cached_value.store(value, std::memory_order_relaxed);
        cached_program_id.store(program_id, std::memory_order_release);
        // One line per title and switch, so a bug report names the workarounds that were on.
        LOG_INFO(Common, "Switch {} for {:016X}: {}", name, program_id, value ? "on" : "off");
        return value;
    }

private:
    [[nodiscard]] bool Read(u64 program_id) const {
        if (const char* const per_title = Lookup(program_id)) {
            return Parse(per_title);
        }
        if (const char* const global = std::getenv(std::string{name}.c_str())) {
            return Parse(global);
        }
        return default_value(program_id);
    }

    /// The value set for this title alone, if any (EDEN_NX2_<NAME>_<16 hex digits>).
    [[nodiscard]] const char* Lookup(u64 program_id) const {
        thread_local std::string key;
        key.assign(name);
        key.push_back('_');
        char digits[17]{};
        std::snprintf(digits, sizeof(digits), "%016llX",
                      static_cast<unsigned long long>(program_id));
        key.append(digits);
        return std::getenv(key.c_str());
    }

    /// "0" and an empty value turn a switch off; anything else turns it on, as the other
    /// EDEN_NX2_* switches read theirs.
    [[nodiscard]] static bool Parse(const char* value) {
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }

    std::string_view name;
    SwitchDefault default_value;
    mutable std::atomic<u64> cached_program_id{~u64{0}};
    mutable std::atomic<bool> cached_value{false};
};

} // namespace Common::Nx2