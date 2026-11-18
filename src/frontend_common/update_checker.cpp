// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <charconv>
#include <limits>
#include "common/logging.h"
#include "common/scm_rev.h"
#include "frontend_common/update_checker.h"

namespace UpdateChecker {

namespace {

std::optional<u64> ParseBuildNumber(std::string_view tag) {
    constexpr std::string_view prefix = "build-";
    if (!tag.starts_with(prefix)) {
        return std::nullopt;
    }
    tag.remove_prefix(prefix.size());
    u64 number{};
    const auto [end, error] = std::from_chars(tag.data(), tag.data() + tag.size(), number);
    if (error != std::errc{} || end == tag.data() || number == 0) {
        return std::nullopt;
    }
    if (end == tag.data() + tag.size()) {
        return number;
    }
    // build-1.1 is build 1, minor 1; it sorts after build-1 and before build-2.
    if (*end != '.') {
        return std::nullopt;
    }
    u64 minor{};
    const auto [minor_end, minor_error] =
        std::from_chars(end + 1, tag.data() + tag.size(), minor);
    if (minor_error != std::errc{} || minor_end != tag.data() + tag.size()) {
        return std::nullopt;
    }
    constexpr u64 max = std::numeric_limits<u64>::max();
    if (number > (max - minor) / 1000) {
        return std::nullopt;
    }
    return number * 1000 + minor;
}

} // namespace

Failure ClassifyFailure(int status) {
    if (status == 403 || status == 429) {
        return Failure::RateLimited;
    }
    return status >= 400 ? Failure::Http : Failure::Network;
}

std::optional<Common::Net::Release> SelectUpdate(std::span<const Common::Net::Release> releases,
                                                 std::string_view build_tag,
                                                 std::string_view platform,
                                                 Common::Net::LinuxPackage package) {
    auto newest = ParseBuildNumber(build_tag);
    if (!newest) {
        return std::nullopt;
    }

    const Common::Net::Release* selected = nullptr;
    for (const auto& release : releases) {
        const auto number = ParseBuildNumber(release.tag);
        if (number && *number > *newest && !release.GetPlatformAssets(platform, package).empty()) {
            newest = number;
            selected = &release;
        }
    }
    return selected ? std::make_optional(*selected) : std::nullopt;
}

bool IsReleaseBuild(std::string_view build_tag, std::string_view platform) {
    return ParseBuildNumber(build_tag) && !platform.empty();
}

CheckResult Check() {
    if (!IsReleaseBuild(Common::g_build_release_tag, Common::g_build_release_platform)) {
        return {.failure = Failure::NotReleaseBuild};
    }

    const auto list = Common::Net::GetReleases();
    if (!list.ok) {
        const auto failure = ClassifyFailure(list.status);
        LOG_WARNING(Frontend, "Update check failed with status {}", list.status);
        return {.failure = failure, .status = list.status};
    }

    auto update = SelectUpdate(list.releases, Common::g_build_release_tag,
                               Common::g_build_release_platform,
                               Common::Net::CurrentLinuxPackage());
    if (update) {
        LOG_INFO(Frontend, "Received update {}", update->title);
    }
    return {.update = std::move(update)};
}

std::optional<Common::Net::Release> GetUpdate() {
    return Check().update;
}

} // namespace UpdateChecker
