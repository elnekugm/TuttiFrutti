// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "common/fs/file.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/shader_cache_v2/cache_file.h"
#include "video_core/shader_cache_v2/cache_share.h"

namespace VideoCommon::ShaderCacheV2 {
namespace {

/// Records are written in chunks of about this size: a 450 MB log is never held twice.
constexpr size_t ChunkBytes = size_t{32} << 20;

void Count(ShareCounts& counts, RecordKind kind) {
    switch (kind) {
    case RecordKind::Program:
        ++counts.programs;
        break;
    case RecordKind::Graphics:
        ++counts.graphics;
        break;
    case RecordKind::Compute:
        ++counts.compute;
        break;
    default:
        break;
    }
}

/// The status of a source log read for sharing, or Done when its records can be used
ShareStatus SourceStatus(const LogFile& source) {
    if (source.bytes.empty()) {
        return ShareStatus::NothingToShare;
    }
    switch (source.parsed.header) {
    case HeaderStatus::Valid:
        return ShareStatus::Done;
    case HeaderStatus::Empty:
        return ShareStatus::NothingToShare;
    case HeaderStatus::OtherVersion:
        return ShareStatus::OtherVersion;
    case HeaderStatus::Damaged:
        break;
    }
    return ShareStatus::NotACache;
}

ShareStatus FromWrite(WriteResult result) {
    switch (result) {
    case WriteResult::Written:
        return ShareStatus::Done;
    case WriteResult::Busy:
        return ShareStatus::Busy;
    case WriteResult::OtherVersion:
        return ShareStatus::OtherVersion;
    case WriteResult::Failed:
    case WriteResult::Exists:
        break;
    }
    return ShareStatus::Failed;
}

/// The record kinds IsShareable takes, each with the keys of a log
struct KeySets {
    std::unordered_set<u64> programs;
    std::unordered_set<u64> graphics;
    std::unordered_set<u64> compute;

    std::unordered_set<u64>* Of(RecordKind kind) {
        switch (kind) {
        case RecordKind::Program:
            return &programs;
        case RecordKind::Graphics:
            return &graphics;
        case RecordKind::Compute:
            return &compute;
        default:
            return nullptr;
        }
    }
};

} // Anonymous namespace

bool IsShareable(RecordKind kind) noexcept {
    return kind == RecordKind::Program || kind == RecordKind::Graphics ||
           kind == RecordKind::Compute;
}

ShareResult ExportLog(const std::filesystem::path& log, const std::filesystem::path& out) {
    using namespace Common::FS;
    ShareResult result;
    // Without the log's lock: records are only appended, and one cut off by an append in progress
    // is ignored.
    const LogFile source = ReadLogFile(log);
    if (result.status = SourceStatus(source); result.status != ShareStatus::Done) {
        return result;
    }
    ShareCounts counts;
    for (const LogRecord& record : source.parsed.live) {
        if (IsShareable(record.kind)) {
            Count(counts, record.kind);
        }
    }
    if (counts.Total() == 0) {
        result.status = ShareStatus::NothingToShare;
        return result;
    }

    auto temporary = out;
    temporary += ".partial";
    const auto fail = [&] {
        std::error_code error;
        std::filesystem::remove(temporary, error);
        result.status = ShareStatus::Failed;
        return result;
    };
    {
        IOFile target{temporary, FileAccessMode::Write, FileType::BinaryFile};
        if (!target.IsOpen()) {
            LOG_ERROR(Common_Filesystem, "Failed to create {}", PathToUTF8String(temporary));
            return fail();
        }
        const auto header = EncodeFileHeader(UnixNow());
        std::vector<u8> buffer(header.begin(), header.end());
        const auto flush = [&] {
            const bool written = target.WriteSpan(std::span<const u8>{buffer}) == buffer.size();
            buffer.clear();
            return written;
        };
        for (const LogRecord& record : source.parsed.live) {
            if (!IsShareable(record.kind)) {
                continue;
            }
            EncodeRecord(buffer, record.kind, record.key, record.payload, record.flags);
            if (buffer.size() >= ChunkBytes && !flush()) {
                return fail();
            }
        }
        // The rename can reach the disk before the data: commit the data first.
        if (!flush() || !target.Commit()) {
            return fail();
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, out, error);
    if (error) {
        LOG_ERROR(Common_Filesystem, "Failed to write {}: {}", PathToUTF8String(out),
                  error.message());
        return fail();
    }
    result.written = counts;
    LOG_INFO(Render_Vulkan,
             "Pipeline cache exported to {}: {} programs, {} graphics and {} compute pipelines",
             PathToUTF8String(out), counts.programs, counts.graphics, counts.compute);
    return result;
}

ShareResult ImportLog(const std::filesystem::path& log, const std::filesystem::path& in) {
    using namespace Common::FS;
    ShareResult result;
    const LogFile source = ReadLogFile(in);
    if (result.status = SourceStatus(source); result.status != ShareStatus::Done) {
        return result;
    }

    KeySets present;
    {
        const LogFile current = ReadLogFile(log);
        if (!current.bytes.empty() && current.parsed.header == HeaderStatus::OtherVersion) {
            // AppendToLog would refuse it too; say so before reading anything else.
            result.status = ShareStatus::OtherVersion;
            return result;
        }
        for (const LogRecord& record : current.parsed.live) {
            if (auto* const keys = present.Of(record.kind)) {
                keys->insert(record.key);
            }
        }
    }

    std::vector<u8> buffer;
    ShareCounts pending;
    const auto flush = [&] {
        if (buffer.empty()) {
            return ShareStatus::Done;
        }
        const ShareStatus status = FromWrite(AppendToLog(log, buffer));
        buffer.clear();
        if (status == ShareStatus::Done) {
            result.written.programs += pending.programs;
            result.written.graphics += pending.graphics;
            result.written.compute += pending.compute;
        }
        pending = {};
        return status;
    };
    for (const LogRecord& record : source.parsed.live) {
        auto* const keys = present.Of(record.kind);
        if (keys == nullptr) {
            continue;
        }
        if (!keys->insert(record.key).second) {
            ++result.already_present;
            continue;
        }
        EncodeRecord(buffer, record.kind, record.key, record.payload, record.flags);
        Count(pending, record.kind);
        if (buffer.size() >= ChunkBytes) {
            if (result.status = flush(); result.status != ShareStatus::Done) {
                return result;
            }
        }
    }
    result.status = flush();
    if (result.status == ShareStatus::Done && result.written.Total() == 0 &&
        result.already_present == 0) {
        result.status = ShareStatus::NothingToShare;
    }
    LOG_INFO(Render_Vulkan,
             "Pipeline cache import from {}: {} programs, {} graphics and {} compute pipelines "
             "added, {} already present",
             PathToUTF8String(in), result.written.programs, result.written.graphics,
             result.written.compute, result.already_present);
    return result;
}

} // namespace VideoCommon::ShaderCacheV2
