// SPDX-License-Identifier: MIT
#include "wire.hpp"

#include <cstdlib>
#include <glob.h>
#include <security/pam_ext.h>
#include <security/pam_modules.h>
#include <sys/time.h>

namespace {
bool lidClosed()
{
    glob_t paths{};
    const int found = glob("/proc/acpi/button/lid/*/state", 0, nullptr, &paths);
    bool closed = false;
    if (found == 0) {
        for (std::size_t i = 0; i < paths.gl_pathc && !closed; ++i) {
            std::ifstream file(paths.gl_pathv[i]);
            std::string line;
            closed = std::getline(file, line) && line.find("closed") != line.npos;
        }
    }
    globfree(&paths);
    return closed;
}

bool remote(pam_handle_t *handle)
{
    const void *item = nullptr;
    const bool remoteHost = pam_get_item(handle, PAM_RHOST, &item) == PAM_SUCCESS && item &&
        *static_cast<const char *>(item) != '\0';
    return remoteHost || std::getenv("SSH_CONNECTION") || std::getenv("SSH_TTY") || std::getenv("SSH_CLIENT");
}

bool validUser(const char *user)
{
    const std::string_view name(user ? user : "");
    return !name.empty() && name.size() <= 64 && name != "." && name != ".." &&
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") == name.npos;
}

bool verifyFace(const char *user)
{
    using namespace surface;
    const Fd fd(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!fd)
        return false;
    const timeval timeout{1, 0};
    if (setsockopt(fd.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0)
        return false;
    const auto address = socketAddress("/run/surface-ir-hello/auth.sock");
    if (connect(fd.get(), reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0 || peerUid(fd.get()) != 0)
        return false;
    const auto message = std::string("AUTH ") + user + "\n";
    if (send(fd.get(), message.data(), message.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(message.size()))
        return false;
    return readLine(fd.get(), Clock::now() + 4500ms).starts_with("OK ");
}
}

extern "C" {
PAM_EXTERN int pam_sm_authenticate(pam_handle_t *handle, int, int argc, const char **argv)
{
    try {
        if (remote(handle) || lidClosed())
            return PAM_IGNORE;
        const char *user = nullptr;
        if (pam_get_user(handle, &user, nullptr) != PAM_SUCCESS || !validUser(user) || !verifyFace(user))
            return PAM_IGNORE;
        for (int i = 0; i < argc; ++i) {
            if (std::string_view(argv[i]) == "quiet")
                return PAM_SUCCESS;
        }
        pam_info(handle, "%s", "Surface IR: Face recognized.");
        return PAM_SUCCESS;
    } catch (...) {
        return PAM_IGNORE;
    }
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t *, int, int, const char **) { return PAM_SUCCESS; }
PAM_EXTERN int pam_sm_acct_mgmt(pam_handle_t *, int, int, const char **) { return PAM_IGNORE; }
}
