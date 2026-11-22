// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <string>

#include "common/common_types.h"

namespace CoverArt {

enum class Status {
    /// The cover, the banner or both were saved.
    Saved,
    /// The service has no art for this program.
    NotFound,
    NetworkError,
    Cancelled,
};

struct Result {
    Status status{Status::NotFound};
    bool cover_saved{};
    bool banner_saved{};
    std::string detail;
};

/// Downloads a game's cover (square, up to 1024 pixels) and its wide banner (up to 2560x1440) into
/// the files QtCommon::Game::DownloadedCoverPath and DownloadedBannerPath name. First from the
/// LaunchBox Games Database by title (its "Square" picture and its background fanart, full size);
/// what it lacks from api.nlib.cc by program ID (Switch games), then from Nintendo's eShop search
/// (searching.nintendo-europe.com) by title. The title searches stay on the same console and take
/// only an exact or near-exact name match. The images are
/// decoded and encoded again before they are saved, so only valid pictures land on disk. A game
/// neither service knows gets a marker file so it is not asked again (see NoCoverMarkerPath).
/// Blocking: run it off the UI thread. It stops as soon as cancel is set.
Result Download(u64 program_id, const std::string& title, const std::atomic_bool& cancel);

/// True when the program has a downloaded cover, or the service is known to have none.
bool IsSettled(u64 program_id);

} // namespace CoverArt
