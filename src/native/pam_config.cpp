// SPDX-License-Identifier: MIT
#include "pam_config.hpp"
#include "base.hpp"
#include "kde_face_policy.hpp"

#include <sys/stat.h>

namespace surface {
namespace {
constexpr std::string_view legacyHook = "auth       sufficient                  pam_surface_ir.so quiet";

void writePolicy(TemporaryFile &file, std::string_view policy, mode_t mode)
{
    writeAll(file.fd(), policy);
    if (fchmod(file.fd(), mode) < 0)
        syserror("Set PAM file permissions");
}
}

bool installKdeFacePam(const fs::path &directory)
{
    const auto path = directory / "kde-face";
    TemporaryFile candidate(path);
    writePolicy(candidate, kdeFacePolicy, 0644);

    if (link(candidate.path().c_str(), path.c_str()) == 0)
        return true;
    if (errno != EEXIST)
        syserror("Install KDE Face PAM policy");
    if (fs::is_regular_file(fs::symlink_status(path)) && readText(path) == kdeFacePolicy)
        return false;
    throw std::runtime_error("Existing KDE Face PAM policy needs manual review: " + path.string());
}

bool removeLegacyPamHook(const fs::path &file)
{
    const auto status = fs::symlink_status(file);
    if (!fs::exists(status))
        return false;
    if (!fs::is_regular_file(status))
        throw std::runtime_error("Unexpected PAM file type: " + file.string());

    const auto policy = readText(file);
    std::string filtered;
    std::string_view remaining = policy;
    while (!remaining.empty()) {
        const auto newline = remaining.find('\n');
        const auto length = newline == remaining.npos ? remaining.size() : newline + 1;
        if (remaining.substr(0, newline) != legacyHook)
            filtered.append(remaining.substr(0, length));
        remaining.remove_prefix(length);
    }
    if (policy == filtered)
        return false;

    TemporaryFile candidate(file);
    writePolicy(candidate, filtered, static_cast<mode_t>(status.permissions()) & 0777);
    candidate.commit(file);
    return true;
}
}
