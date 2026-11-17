// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>

#include "common/common_types.h"
#include "video_core/shader_cache_v2/cache_format.h"

/// Sharing a title's pipeline cache log between machines.
///
/// Program, Graphics and Compute records hold the game's own shaders and pipeline state: no driver
/// or GPU is in them (the pipeline keys leave the driver out, and a key whose dynamic state this
/// GPU lacks is skipped when the log is loaded). Those are shared. Usage and migration records,
/// and the driver binaries (a file of their own) stay on the machine that made them.
namespace VideoCommon::ShaderCacheV2 {

/// Whether records of this kind are exported and imported
[[nodiscard]] bool IsShareable(RecordKind kind) noexcept;

struct ShareCounts {
    u64 programs{};
    u64 graphics{};
    u64 compute{};

    [[nodiscard]] u64 Total() const noexcept {
        return programs + graphics + compute;
    }
};

enum class ShareStatus {
    Done,
    NothingToShare, ///< The source holds no shareable record (missing or empty)
    NotACache,      ///< The source is not a pipeline cache log, or its header is damaged
    OtherVersion,   ///< A log of another format version
    Busy,           ///< Another process holds the destination log's lock
    Failed,         ///< A write failed; the destination is as it was
};

struct ShareResult {
    ShareStatus status{ShareStatus::Failed};
    /// Records written: all exported ones, or the imported ones the log lacked
    ShareCounts written;
    /// Import only: shareable records the log held already, left out
    u64 already_present{};
};

/// Writes the shareable live records of the log at `log` to a new log at `out`, replacing a file
/// there. The result is itself a complete log: it can be imported, or used as a title's log as is.
/// Written to a temporary file beside `out` and renamed over it, so `out` is never left partial.
[[nodiscard]] ShareResult ExportLog(const std::filesystem::path& log,
                                    const std::filesystem::path& out);

/// Appends the shareable live records of the log at `in` that the log at `log` lacks (by kind
/// and key) to it, creating it when missing. Records of a key the log holds stay as they are.
/// Takes the log's lock for each chunk it appends: Busy while another process holds it, with
/// the chunks before that already appended (they are complete records).
[[nodiscard]] ShareResult ImportLog(const std::filesystem::path& log,
                                    const std::filesystem::path& in);

} // namespace VideoCommon::ShaderCacheV2
