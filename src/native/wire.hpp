// SPDX-License-Identifier: MIT
#pragma once
#include "base.hpp"

#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>

namespace surface {
inline sockaddr_un socketAddress(const fs::path &path)
{
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string name = path.string();
    if (name.empty() || name.size() >= sizeof(address.sun_path))
        throw std::invalid_argument("Invalid Unix socket path");
    std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
    return address;
}

inline uid_t peerUid(int fd)
{
    struct ucred info{};
    socklen_t bytes = sizeof(info);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &info, &bytes) < 0)
        syserror("SO_PEERCRED");
    return info.uid;
}

inline std::string readLine(int fd, Clock::time_point deadline)
{
    std::string buffer;
    while (buffer.size() < 256 && readable(fd, deadline)) {
        char part[128];
        const auto count = read(fd, part, sizeof(part));
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            syserror("Read protocol line");
        }
        if (count == 0)
            break;
        buffer.append(part, count);
        if (const auto end = buffer.find('\n'); end != std::string::npos)
            return std::string(buffer.substr(0, end));
    }
    return {};
}
}
