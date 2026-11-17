// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/shader_cache_v2/cache_file.h"
#include "video_core/shader_cache_v2/cache_format.h"
#include "video_core/shader_cache_v2/cache_share.h"

namespace {

namespace V2 = VideoCommon::ShaderCacheV2;
using V2::RecordKind;
using V2::ShareStatus;

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("eden-shader-cache-share-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

std::vector<u8> Payload(u8 seed, size_t size = 40) {
    std::vector<u8> payload(size);
    for (size_t i = 0; i < size; ++i) {
        payload[i] = static_cast<u8>(seed + i);
    }
    return payload;
}

void Append(const std::filesystem::path& file, RecordKind kind, u64 key,
            const std::vector<u8>& payload) {
    std::vector<u8> records;
    V2::EncodeRecord(records, kind, key, payload);
    REQUIRE(V2::AppendToLog(file, records) == V2::WriteResult::Written);
}

/// The live records of a log, by kind and key
std::map<std::pair<RecordKind, u64>, std::vector<u8>> Live(const std::filesystem::path& file) {
    const V2::LogFile log = V2::ReadLogFile(file);
    REQUIRE(log.parsed.header == V2::HeaderStatus::Valid);
    std::map<std::pair<RecordKind, u64>, std::vector<u8>> live;
    for (const V2::LogRecord& record : log.parsed.live) {
        live[{record.kind, record.key}].assign(record.payload.begin(), record.payload.end());
    }
    return live;
}

} // Anonymous namespace

TEST_CASE("Shader cache share: export keeps only the live shareable records",
          "[video_core][shader_cache_v2]") {
    const TempDir dir;
    const auto log = dir.path / "lichii-cache.v2.bin";
    const auto out = dir.path / "exported.bin";
    Append(log, RecordKind::Program, 1, Payload(1));
    Append(log, RecordKind::Graphics, 2, Payload(2));
    Append(log, RecordKind::Graphics, 2, Payload(3)); // supersedes the one before
    Append(log, RecordKind::Compute, 4, Payload(4));
    Append(log, RecordKind::Used, 5, Payload(5));
    Append(log, RecordKind::DeviceIdentity, 6, Payload(6));
    Append(log, RecordKind::Graphics, 7, Payload(7));
    Append(log, RecordKind::Tombstone, 7, V2::TombstonePayload(RecordKind::Graphics));

    const auto result = V2::ExportLog(log, out);
    REQUIRE(result.status == ShareStatus::Done);
    CHECK(result.written.programs == 1);
    CHECK(result.written.graphics == 1);
    CHECK(result.written.compute == 1);
    CHECK_FALSE(std::filesystem::exists(dir.path / "exported.bin.partial"));

    const auto live = Live(out);
    REQUIRE(live.size() == 3);
    CHECK(live.at({RecordKind::Program, 1}) == Payload(1));
    CHECK(live.at({RecordKind::Graphics, 2}) == Payload(3));
    CHECK(live.at({RecordKind::Compute, 4}) == Payload(4));

    // A second export replaces the file
    Append(log, RecordKind::Compute, 8, Payload(8));
    REQUIRE(V2::ExportLog(log, out).status == ShareStatus::Done);
    CHECK(Live(out).size() == 4);
}

TEST_CASE("Shader cache share: export of a log without shareable records",
          "[video_core][shader_cache_v2]") {
    const TempDir dir;
    const auto log = dir.path / "lichii-cache.v2.bin";
    const auto out = dir.path / "exported.bin";
    CHECK(V2::ExportLog(log, out).status == ShareStatus::NothingToShare);
    Append(log, RecordKind::Used, 1, Payload(1));
    CHECK(V2::ExportLog(log, out).status == ShareStatus::NothingToShare);
    CHECK_FALSE(std::filesystem::exists(out));
}

TEST_CASE("Shader cache share: import adds only the records the log lacks",
          "[video_core][shader_cache_v2]") {
    const TempDir dir;
    const auto log = dir.path / "lichii-cache.v2.bin";
    const auto other = dir.path / "other.bin";
    Append(log, RecordKind::Program, 1, Payload(1));
    Append(log, RecordKind::Graphics, 2, Payload(2));
    Append(log, RecordKind::Used, 9, Payload(9));

    Append(other, RecordKind::Program, 1, Payload(100)); // same key: the log keeps its own
    Append(other, RecordKind::Graphics, 3, Payload(3));
    Append(other, RecordKind::Compute, 2, Payload(4)); // same key, other kind: added
    Append(other, RecordKind::Used, 10, Payload(10));  // not shared
    Append(other, RecordKind::Graphics, 11, Payload(11));
    Append(other, RecordKind::Tombstone, 11, V2::TombstonePayload(RecordKind::Graphics));

    const auto result = V2::ImportLog(log, other);
    REQUIRE(result.status == ShareStatus::Done);
    CHECK(result.written.programs == 0);
    CHECK(result.written.graphics == 1);
    CHECK(result.written.compute == 1);
    CHECK(result.already_present == 1);

    const auto live = Live(log);
    CHECK(live.size() == 5);
    CHECK(live.at({RecordKind::Program, 1}) == Payload(1));
    CHECK(live.at({RecordKind::Graphics, 2}) == Payload(2));
    CHECK(live.at({RecordKind::Graphics, 3}) == Payload(3));
    CHECK(live.at({RecordKind::Compute, 2}) == Payload(4));
    CHECK(live.contains({RecordKind::Used, 9}));
    CHECK_FALSE(live.contains({RecordKind::Used, 10}));

    // Importing again changes nothing
    const auto again = V2::ImportLog(log, other);
    REQUIRE(again.status == ShareStatus::Done);
    CHECK(again.written.Total() == 0);
    CHECK(again.already_present == 3);
    CHECK(Live(log) == live);
}

TEST_CASE("Shader cache share: an export imports into a title without a log",
          "[video_core][shader_cache_v2]") {
    const TempDir dir;
    const auto source = dir.path / "source.bin";
    const auto exported = dir.path / "exported.bin";
    const auto fresh = dir.path / "fresh" / "lichii-cache.v2.bin";
    std::filesystem::create_directories(fresh.parent_path());
    Append(source, RecordKind::Program, 1, Payload(1, 5000));
    Append(source, RecordKind::Graphics, 2, Payload(2, 300));
    REQUIRE(V2::ExportLog(source, exported).status == ShareStatus::Done);

    const auto result = V2::ImportLog(fresh, exported);
    REQUIRE(result.status == ShareStatus::Done);
    CHECK(result.written.Total() == 2);
    CHECK(Live(fresh) == Live(source));
}

TEST_CASE("Shader cache share: import refuses files that are not a log",
          "[video_core][shader_cache_v2]") {
    const TempDir dir;
    const auto log = dir.path / "lichii-cache.v2.bin";
    Append(log, RecordKind::Program, 1, Payload(1));
    const auto before = Live(log);

    const auto junk = dir.path / "junk.bin";
    {
        std::ofstream stream(junk, std::ios::binary);
        stream << "this is not a pipeline cache, just some text that is long enough";
    }
    CHECK(V2::ImportLog(log, junk).status == ShareStatus::NotACache);
    CHECK(V2::ImportLog(log, dir.path / "missing.bin").status == ShareStatus::NothingToShare);

    const auto only_used = dir.path / "used.bin";
    Append(only_used, RecordKind::Used, 1, Payload(1));
    CHECK(V2::ImportLog(log, only_used).status == ShareStatus::NothingToShare);
    CHECK(Live(log) == before);
}
