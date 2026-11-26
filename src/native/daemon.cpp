// SPDX-License-Identifier: MIT
#include "core.hpp"
#include "wire.hpp"

#include <iostream>
#include <sys/file.h>
#include <sys/stat.h>

namespace surface {
namespace {
class SocketEntry {
public:
    explicit SocketEntry(const fs::path &path) : path_(path) {}
    ~SocketEntry()
    {
        if (bound)
            unlink(path_.c_str());
    }
    bool bound = false;

private:
    fs::path path_;
};

void serve(const fs::path &path)
{
    fs::create_directories(path.parent_path());
    struct stat info{};
    if (lstat(path.parent_path().c_str(), &info) < 0)
        syserror("Inspect face socket directory");
    if (!S_ISDIR(info.st_mode) || info.st_uid != 0 || (info.st_mode & 0022))
        throw std::runtime_error("Face socket directory must be writable only by root");

    const auto lock = openFile(path.string() + ".lock", O_CREAT | O_RDWR | O_NOFOLLOW);
    if (flock(lock.get(), LOCK_EX | LOCK_NB) < 0)
        syserror("A face service is already running on this socket");
    if (lstat(path.c_str(), &info) == 0) {
        if (!S_ISSOCK(info.st_mode) || info.st_uid != 0)
            throw std::runtime_error("Face socket path is occupied");
        if (unlink(path.c_str()) < 0)
            syserror("Remove stale face socket");
    }
    std::cout << "[surface-ir-hellod] Pre-loading YuNet and SFace\n";
    Engine engine;
    const Fd server(socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!server)
        syserror("Create Unix face socket");
    SocketEntry entry(path);
    const auto address = socketAddress(path);
    if (bind(server.get(), reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0)
        syserror("Bind face socket");
    entry.bound = true;
    if (chmod(path.c_str(), 0666) < 0 || listen(server.get(), 8) < 0)
        syserror("Listen on face socket");
    std::cout << "[surface-ir-hellod] Listening on " << path.string() << '\n';

    while (!interrupted) {
        if (!readable(server.get(), Clock::now() + 1s))
            continue;
        const Fd connection(accept4(server.get(), nullptr, nullptr, SOCK_CLOEXEC));
        if (!connection) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            syserror("Accept face socket client");
        }
        const auto reply = [&](std::string_view response) {
            const auto line = std::string(response) + "\n";
            const auto sent = send(connection.get(), line.data(), line.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
            if (sent < 0 && errno != EPIPE && errno != ECONNRESET)
                syserror("Write face socket reply");
        };
        try {
            const auto line = readLine(connection.get(), Clock::now() + 2s);
            if (line == "PING") {
                reply("PONG");
                continue;
            }
            if (!line.starts_with("AUTH ") || !validUser(std::string_view(line).substr(5))) {
                reply("ERR invalid_request");
                continue;
            }
            const auto user = line.substr(5);
            const auto uid = peerUid(connection.get());
            if (uid != 0 && userForUid(uid) != user) {
                reply("ERR unauthorized_uid");
                continue;
            }
            const auto verdict = verify(engine, loadSamples(user), loadConfig());
            std::cout << "[auth] user=" << user << " ok=" << verdict.ok << ' ' << verdict.message << '\n';
            reply(verdict.message);
        } catch (const std::exception &error) {
            std::cerr << "[auth-err] " << error.what() << '\n';
            try {
                reply("ERR internal");
            } catch (const std::exception &) {
                // The client may have closed its connection.
            }
        }
    }
}
}
}

int main(int argc, char **argv)
{
    surface::installSignals();
    std::cout << std::unitbuf;
    try {
        if (geteuid() != 0)
            throw std::runtime_error("surface-ir-hellod must run as root");
        if (argc != 1 && !(argc == 3 && std::string_view(argv[1]) == "--socket"))
            throw std::runtime_error("Usage: surface-ir-hellod [--socket PATH]");
        surface::serve(argc == 3 ? argv[2] : surface::socketPath);
    } catch (const surface::Cancelled &) {
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "surface-ir-hellod: " << error.what() << '\n';
        return 1;
    }
}
