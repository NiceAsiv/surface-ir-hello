// SPDX-License-Identifier: MIT
#include "core.hpp"

#include <toml++/toml.hpp>
#include <sys/file.h>
#include <sys/stat.h>
#include <pwd.h>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace surface {
namespace {
fs::path samplePath(const std::string &user, const fs::path &directory)
{
    if (!validUser(user))
        throw std::invalid_argument("Invalid account name");
    return directory / (user + ".json");
}

Fd lockSamples(const std::string &user, const fs::path &directory)
{
    if (!validUser(user))
        throw std::invalid_argument("Invalid account name");
    if (mkdir(directory.c_str(), 0700) < 0 && errno != EEXIST)
        syserror("Create face template directory");
    const auto info = fs::symlink_status(directory);
    if (!fs::is_directory(info) || (info.permissions() & (fs::perms::group_all | fs::perms::others_all)) != fs::perms::none)
        throw std::runtime_error("Face template directory must be private");
    auto guard = openFile(directory / ("." + user + ".lock"), O_CREAT | O_RDWR | O_NOFOLLOW);
    while (flock(guard.get(), LOCK_EX) < 0) {
        if (interrupted)
            throw Cancelled{};
        if (errno != EINTR)
            syserror("Lock face templates");
    }
    return guard;
}
}

Config loadConfig(const fs::path &path)
{
    Config cfg;
    if (!fs::exists(path))
        return cfg;

    try {
        const auto table = toml::parse_file(path.string());
        const auto ir = table["ir"];
        cfg.ir.current = ir["emitter_current_ma"].value_or(cfg.ir.current);
        cfg.ir.exposureMax = ir["exposure_max"].value_or(cfg.ir.exposureMax);
        cfg.ir.gainMax = ir["gain_max"].value_or(cfg.ir.gainMax);
        cfg.ir.targetP95 = ir["target_p95"].value_or(cfg.ir.targetP95);
        cfg.ir.led = ir["led_channel"].value_or(cfg.ir.led);
        const auto auth = table["auth"];
        cfg.threshold = auth["cosine_threshold"].value_or(cfg.threshold);
        const double seconds = auth["timeout_sec"].value_or(cfg.timeout.count() / 1000.0);
        cfg.activeIr = auth["require_active_ir_liveness"].value_or(cfg.activeIr);
        if (!std::isfinite(cfg.ir.current) || !std::isfinite(cfg.ir.gainMax) ||
            !std::isfinite(cfg.ir.targetP95) || !std::isfinite(cfg.threshold) || !std::isfinite(seconds))
            throw std::runtime_error("Configuration contains a non-finite value");
        cfg.timeout = std::chrono::milliseconds(static_cast<int>(std::clamp(seconds, 0.1, 6.0) * 1000));
        if (cfg.ir.led != 1 && cfg.ir.led != 2)
            throw std::runtime_error("IR LED channel must be 1 or 2");
        if (cfg.threshold <= 0 || cfg.threshold > 1)
            throw std::runtime_error("Cosine threshold must be greater than zero and at most one");
        return cfg;
    } catch (const std::exception &error) {
        std::cerr << "Config fallback: " << error.what() << '\n';
        return {};
    }
}

bool validUser(std::string_view user)
{
    return !user.empty() && user.size() <= 64 && user != "." && user != ".." &&
        user.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") == user.npos;
}

std::string userForUid(uid_t uid)
{
    const auto *account = getpwuid(uid);
    return account ? account->pw_name : std::string{};
}

std::string jsonText(const Json::Value &value)
{
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value);
}

Json::Value loadSamples(const std::string &user, const fs::path &directory)
{
    const auto path = samplePath(user, directory);
    if (!fs::exists(path))
        return Json::Value(Json::arrayValue);
    std::ifstream file(path);
    Json::CharReaderBuilder reader;
    Json::Value document;
    std::string errors;
    if (!file || !Json::parseFromStream(reader, file, &document, &errors) || !document.isObject()
        || !document["samples"].isArray())
        throw std::runtime_error("Invalid template file for account " + user);
    if (document.isMember("version") && document["version"] != 1)
        throw std::runtime_error("Unsupported template format for account " + user);
    return document["samples"];
}

int saveSamples(const std::string &user, const Json::Value &newSamples, bool append, const fs::path &directory)
{
    const auto guard = lockSamples(user, directory);
    Json::Value samples = append ? loadSamples(user, directory) : Json::Value(Json::arrayValue);
    for (const auto &sample : newSamples)
        samples.append(sample);
    const auto path = samplePath(user, directory);
    TemporaryFile temporary(path);
    Json::Value document;
    document["user"] = user;
    document["version"] = 1;
    document["samples"] = samples;
    writeAll(temporary.fd(), jsonText(document));
    if (fsync(temporary.fd()) < 0)
        syserror("Syncing face templates");
    temporary.commit(path);
    return samples.size();
}

bool clearSamples(const std::string &user, const fs::path &directory)
{
    const auto path = samplePath(user, directory);
    if (!fs::exists(directory))
        return false;
    const auto guard = lockSamples(user, directory);
    return fs::remove(path);
}
}
