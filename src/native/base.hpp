// SPDX-License-Identifier: MIT
#pragma once
#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace surface {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

inline volatile std::sig_atomic_t interrupted = 0;
inline void interrupt(int) { interrupted = 1; }
struct Cancelled {};

inline void installSignals()
{
    struct sigaction handler {};
    handler.sa_handler = interrupt;
    sigemptyset(&handler.sa_mask);
    for (const int sig : {SIGINT, SIGTERM, SIGHUP})
        sigaction(sig, &handler, nullptr);
    std::signal(SIGPIPE, SIG_IGN);
}

[[noreturn]] inline void syserror(const std::string &message)
{
    throw std::system_error(errno, std::generic_category(), message);
}

class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { reset(); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    Fd(Fd &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd &operator=(Fd &&other) noexcept
    {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }
    void reset()
    {
        if (fd_ >= 0)
            ::close(std::exchange(fd_, -1));
    }

private:
    int fd_ = -1;
};

inline Fd openFile(const fs::path &path, int flags, mode_t mode = 0600)
{
    const int fd = open(path.c_str(), flags | O_CLOEXEC, mode);
    if (fd < 0)
        syserror(path.string());
    return Fd(fd);
}

template<class T>
void ioctlChecked(int fd, unsigned long request, T &arg, const char *operation)
{
    while (ioctl(fd, request, &arg) < 0) {
        if (errno != EINTR || interrupted)
            syserror(operation);
    }
}

inline bool readable(int fd, Clock::time_point deadline)
{
    pollfd watch{fd, POLLIN, 0};
    while (!interrupted) {
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
        const int ready = poll(&watch, 1, remaining > 0 ? static_cast<int>(remaining) : 0);
        if (ready >= 0)
            return ready > 0;
        if (errno != EINTR)
            syserror("poll");
    }
    throw Cancelled{};
}

inline void writeAll(int fd, std::string_view data)
{
    while (!data.empty()) {
        const auto written = write(fd, data.data(), data.size());
        if (written < 0 && errno == EINTR && !interrupted)
            continue;
        if (written <= 0)
            syserror("write");
        data.remove_prefix(written);
    }
}

inline std::string trim(std::string_view value)
{
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == value.npos)
        return {};
    return std::string(value.substr(start, value.find_last_not_of(" \t\r\n") - start + 1));
}

inline std::string readText(const fs::path &path)
{
    std::ifstream file(path);
    if (!file)
        throw std::runtime_error("Could not read " + path.string());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

class TemporaryFile {
public:
    explicit TemporaryFile(const fs::path &path)
    {
        std::string pattern = path.string() + ".XXXXXX";
        const auto fd = mkostemp(pattern.data(), O_CLOEXEC);
        if (fd < 0)
            syserror("mkstemp");
        fd_ = Fd(fd);
        path_ = std::move(pattern);
    }
    ~TemporaryFile() { unlink(path_.c_str()); }
    int fd() const { return fd_.get(); }
    const fs::path &path() const { return path_; }
    void commit(const fs::path &destination)
    {
        if (rename(path_.c_str(), destination.c_str()) < 0)
            syserror("rename " + destination.string());
        path_.clear();
    }

private:
    Fd fd_;
    fs::path path_;
};
}
