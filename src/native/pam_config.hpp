// SPDX-License-Identifier: MIT
#pragma once
#include <filesystem>

namespace surface {
bool installKdeFacePam(const std::filesystem::path &directory);
bool removeLegacyPamHook(const std::filesystem::path &file);
}
