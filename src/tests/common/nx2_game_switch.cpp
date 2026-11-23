// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>

#include <catch2/catch_test_macros.hpp>

#include "common/game_presets.h"
#include "common/nx2_game_switch.h"
#include "common/scope_exit.h"
#include "common/settings.h"

namespace {

void SetSwitch(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value != nullptr ? value : "");
#else
    if (value != nullptr) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

/// Restores the program id the test changes.
struct ProgramScope {
    u64 saved = Settings::GetCurrentProgramID();
    ~ProgramScope() {
        Settings::SetCurrentProgramID(saved);
    }
};

} // namespace

TEST_CASE("A per-title switch is on only for its title by default", "[settings]") {
    ProgramScope scope;
    const Common::Nx2::GameSwitch kirby_only{
        "EDEN_NX2_TEST_KIRBY_ONLY",
        [](u64 program_id) { return program_id == Settings::KirbyAirRidersProgramId; }};
    Settings::SetCurrentProgramID(Settings::KirbyAirRidersProgramId);
    CHECK(kirby_only.Enabled());
    Settings::SetCurrentProgramID(Settings::MarioKartWorldProgramId);
    CHECK_FALSE(kirby_only.Enabled());
}

TEST_CASE("The global variable turns a switch on for every title", "[settings]") {
    ProgramScope scope;
    constexpr const char* name = "EDEN_NX2_TEST_GLOBAL_SWITCH";
    SetSwitch(name, "1");
    SCOPE_EXIT {
        SetSwitch(name, nullptr);
    };
    const Common::Nx2::GameSwitch switch_{name};
    Settings::SetCurrentProgramID(Settings::OcarinaOfTimeProgramId);
    CHECK(switch_.Enabled());
    Settings::SetCurrentProgramID(Settings::MarioKartWorldProgramId);
    CHECK(switch_.Enabled());
}

TEST_CASE("The per-title variable wins and leaves other titles alone", "[settings]") {
    ProgramScope scope;
    constexpr const char* name = "EDEN_NX2_TEST_TITLE_ONLY";
    // The name the file uses: the switch name, an underscore, and the program id in 16 hex digits.
    SetSwitch("EDEN_NX2_TEST_TITLE_ONLY_0400C3F00006E000", "1");
    SCOPE_EXIT {
        SetSwitch("EDEN_NX2_TEST_TITLE_ONLY_0400C3F00006E000", nullptr);
    };
    const Common::Nx2::GameSwitch switch_{name};
    Settings::SetCurrentProgramID(Settings::MarioKartWorldProgramId);
    CHECK(switch_.Enabled());
    Settings::SetCurrentProgramID(Settings::OcarinaOfTimeProgramId);
    CHECK_FALSE(switch_.Enabled());
}

TEST_CASE("Zero turns a switch off for the title, even when it is on by default", "[settings]") {
    ProgramScope scope;
    constexpr const char* name = "EDEN_NX2_TEST_DEFAULT_ON";
    SetSwitch("EDEN_NX2_TEST_DEFAULT_ON_040077201BE50000", "0");
    SCOPE_EXIT {
        SetSwitch("EDEN_NX2_TEST_DEFAULT_ON_040077201BE50000", nullptr);
    };
    const Common::Nx2::GameSwitch switch_{name, Common::Nx2::OnForAll};
    Settings::SetCurrentProgramID(Settings::KirbyAirRidersProgramId);
    CHECK_FALSE(switch_.Enabled());
    Settings::SetCurrentProgramID(Settings::MarioKartWorldProgramId);
    CHECK(switch_.Enabled());
}