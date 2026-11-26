// SPDX-License-Identifier: MIT
#include "core.hpp"
#include "wire.hpp"

namespace surface {
Verification request(std::string_view query, const fs::path &socket)
{
    const Fd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!fd)
        syserror("Open face service socket");
    const auto address = socketAddress(socket);
    if (connect(fd.get(), reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0)
        syserror("Connect to surface-ir-hellod");
    if (peerUid(fd.get()) != 0)
        throw std::runtime_error("Face service socket is not owned by root");
    auto message = std::string(query) + "\n";
    writeAll(fd.get(), message);
    message = readLine(fd.get(), Clock::now() + 7s);
    if (message.empty())
        throw std::runtime_error("Face service did not return a result");
    return {message.starts_with("OK ") || message == "PONG", std::move(message)};
}
}
