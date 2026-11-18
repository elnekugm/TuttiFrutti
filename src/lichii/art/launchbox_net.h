// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QImage>
#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>

#include "lichii/art/launchbox.h"

namespace LaunchBox {

/// At most this much is read from one image response.
constexpr qint64 MaxImageBytes = 32LL << 20;

struct ImageResult {
    enum class Status { Ok, Network, Timeout, Cancelled, TooLarge, BadType, Decode, NotFound };
    Status status{Status::Network};
    QImage image;
    QString detail;
};

/// Downloads Metadata.zip (about 110 MB) to `dest`. Writes `dest.part` and renames it when whole.
/// `report(received, total)` may return false to stop; `cancel` is checked per chunk.
bool DownloadMetadata(const QString& dest, const std::atomic_bool& cancel, QString* error,
                      const std::function<bool(qint64, qint64)>& report = {});

/// Downloads one image over HTTPS and checks its content type, size and that it decodes.
ImageResult FetchImage(const QString& file_name, const std::atomic_bool& cancel);

}  // namespace LaunchBox