// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>

namespace FrontendCommon {

std::filesystem::path ProfileImportStateDirectory();
bool NeedsLegacyProfileImport();

// Whether a portable build should offer to bring an existing non-portable Lichii profile into its
// local "user" directory. False outside portable mode, once the user has decided, or when there is
// no non-portable profile to import.
bool NeedsPortableProfileImport();

void CheckLegacyProfileBeforeConfig();
std::filesystem::path RemapImportedPath(const std::filesystem::path& value,
                                        const std::filesystem::path& source,
                                        const std::filesystem::path& destination);
void RemapImportedConfig(const std::filesystem::path& config_directory,
                         const std::filesystem::path& source,
                         const std::filesystem::path& destination);

} // namespace FrontendCommon
