// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "tests/common/temp_dir.h"

TEST_CASE("Filesystem calls take a path longer than MAX_PATH", "[fs][long_paths]") {
    Tests::TempDir temp{"lichii-long-path"};
    const std::filesystem::path root = temp.Path();

    // A save data path in a portable profile is this deep, and the folder the emulator was
    // extracted into adds its own: past 260 characters Windows needed the `\\?\` prefix the
    // filesystem calls add (ExtendWin32Path), or every call on it failed (Ocarina of Time 2.0.0
    // aborted as its save data could not be created).
    std::filesystem::path deep = root;
    while (deep.native().size() < 300) {
        deep /= "a-directory-name-long-enough-to-pass-max-path";
    }
    REQUIRE(deep.native().size() >= 300);
    REQUIRE(Common::FS::CreateDirs(deep));
    REQUIRE(Common::FS::IsDir(deep));

    const std::filesystem::path file = deep / "save.bin";
    const std::vector<u8> data(0x1000, 0x5a);
    {
        Common::FS::IOFile out{file, Common::FS::FileAccessMode::Write,
                               Common::FS::FileType::BinaryFile};
        REQUIRE(out.IsOpen());
        REQUIRE(out.Write(data) == data.size());
    }
    REQUIRE(Common::FS::Exists(file));
    REQUIRE(Common::FS::GetSize(file) == data.size());
    // Enumerating the deep directory works too, and gives the caller its own short paths: the
    // extended form the iteration needs must not become a file's identity (vfs_real stores it).
    std::vector<std::filesystem::path> entries;
    bool sizes_ok = true;
    Common::FS::IterateDirEntries(
        deep,
        [&entries, &sizes_ok](const std::filesystem::path& entry_path, size_t entry_size) {
            entries.push_back(entry_path);
            sizes_ok = sizes_ok && entry_size == 0x1000;
            return true;
        },
        Common::FS::DirEntryFilter::File);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0] == file);
    CHECK(sizes_ok);
    {
        Common::FS::IOFile in{file, Common::FS::FileAccessMode::Read,
                              Common::FS::FileType::BinaryFile};
        REQUIRE(in.IsOpen());
        std::vector<u8> read(data.size());
        REQUIRE(in.Read(read) == read.size());
        CHECK(read == data);
    }
    REQUIRE(Common::FS::RemoveFile(file));
    REQUIRE(Common::FS::RemoveDirRecursively(deep));
}