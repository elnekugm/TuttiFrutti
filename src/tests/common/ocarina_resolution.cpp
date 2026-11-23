// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/game_presets.h"
#include "common/scope_exit.h"
#include "common/settings.h"

namespace {

/// Restores the settings and the program id the test changes.
struct ResolutionScope {
    Settings::ResolutionSetup setup = Settings::values.resolution_setup.GetValue();
    Settings::OcarinaResolution ocarina = Settings::values.ocarina_resolution.GetValue();
    Settings::ResolutionScalingInfo info = Settings::values.resolution_info;
    u64 program_id = Settings::GetCurrentProgramID();
    ~ResolutionScope() {
        Settings::values.resolution_setup.SetValue(setup);
        Settings::values.ocarina_resolution.SetValue(ocarina);
        Settings::values.resolution_info = info;
        Settings::SetCurrentProgramID(program_id);
    }
};

} // namespace

TEST_CASE("Ocarina of Time's output resolution picks the scale of its upscale", "[settings]") {
    ResolutionScope scope;
    Settings::SetCurrentProgramID(Settings::OcarinaOfTimeProgramId);
    Settings::values.resolution_setup.SetValue(Settings::ResolutionSetup::Res1X);

    Settings::values.ocarina_resolution.SetValue(Settings::OcarinaResolution::Auto);
    Settings::UpdateRescalingInfo();
    CHECK(Settings::values.resolution_info.up_factor == 1.0f);
    CHECK_FALSE(Settings::values.resolution_info.active);

    Settings::values.ocarina_resolution.SetValue(Settings::OcarinaResolution::Res1080p);
    Settings::UpdateRescalingInfo();
    CHECK(Settings::values.resolution_info.up_scale == 3);
    CHECK(Settings::values.resolution_info.down_shift == 2);
    CHECK(Settings::values.resolution_info.up_factor == 0.75f);

    Settings::values.ocarina_resolution.SetValue(Settings::OcarinaResolution::Res1440p);
    Settings::UpdateRescalingInfo();
    CHECK(Settings::values.resolution_info.up_factor == 1.0f);

    Settings::values.ocarina_resolution.SetValue(Settings::OcarinaResolution::Res2160p);
    Settings::UpdateRescalingInfo();
    CHECK(Settings::values.resolution_info.up_scale == 3);
    CHECK(Settings::values.resolution_info.down_shift == 1);
    CHECK(Settings::values.resolution_info.up_factor == 1.5f);
}

TEST_CASE("Other titles keep the Resolution setting whatever Ocarina's choice says", "[settings]") {
    ResolutionScope scope;
    Settings::values.ocarina_resolution.SetValue(Settings::OcarinaResolution::Res2160p);
    Settings::values.resolution_setup.SetValue(Settings::ResolutionSetup::Res2X);

    Settings::SetCurrentProgramID(Settings::MarioKartWorldProgramId);
    Settings::UpdateRescalingInfo();
    CHECK(Settings::values.resolution_info.up_factor == 2.0f);

    Settings::SetCurrentProgramID(0);
    Settings::UpdateRescalingInfo();
    CHECK(Settings::values.resolution_info.up_factor == 2.0f);
}