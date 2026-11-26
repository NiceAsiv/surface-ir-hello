// SPDX-License-Identifier: MIT
#include "core.hpp"
#include "pam_config.hpp"
#include "wire.hpp"

#include <cmath>
#include <sys/stat.h>
#include <sys/socket.h>
#include <iostream>
#include <future>

namespace {
using namespace surface;
void expect(bool condition, std::string_view description)
{
    if (!condition)
        throw std::runtime_error("Regression: " + std::string(description));
}

void packedFrames()
{
    constexpr int columns = 26, rows = 2, stride = 96;
    std::array<unsigned char, stride * rows> bytes{};
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < columns; ++x) {
            const unsigned value = y * 511 + x * 19;
            const int bit = (x % 25) * 10;
            const int byte = y * stride + (x / 25) * 32 + bit / 8;
            bytes[byte] |= static_cast<unsigned char>(value << (bit % 8));
            bytes[byte + 1] |= static_cast<unsigned char>(value >> (8 - bit % 8));
        }
    }
    const auto frame = unpackIp3y(bytes.data(), stride, columns, rows);
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < columns; ++x)
            expect(frame.at<std::uint16_t>(y, x) == y * 511 + x * 19, "IPU3 stride or bit order");
    std::array<std::uint16_t, 3> measurements{100, 5, 15};
    const cv::Mat values(1, 3, CV_16UC1, measurements.data());
    expect(std::abs(percentile(values, 1, 0.25) - 10.0) < 0.001, "NumPy-compatible percentile");
}

void compatibility()
{
    std::string path = (fs::temp_directory_path() / "surface-ir-regression.XXXXXX").string();
    if (!mkdtemp(path.data()))
        syserror("Create test directory");
    struct Directory {
        fs::path path;
        ~Directory() { std::error_code error; fs::remove_all(path, error); }
    } directory{path};
    {
        std::ofstream config(directory.path / "config.toml");
        config << "[ir]\nemitter_current_ma = 390\nexposure_max = 340\n"
                  "[auth]\ncosine_threshold = 0.45\ntimeout_sec = 2.75\nrequire_active_ir_liveness = true\n";
    }
    const auto pam = directory.path / "kde-face";
    expect(installKdeFacePam(directory.path), "KDE Face PAM policy installed");
    const auto expectedPolicy = readText(pam);
    expect(expectedPolicy.find("pam_surface_ir.so") != expectedPolicy.npos &&
           expectedPolicy.find("pam_deny.so") != expectedPolicy.npos, "dedicated PAM policy has an explicit denial fallback");
    expect(!installKdeFacePam(directory.path), "dedicated policy installation is idempotent");
    const auto regular = directory.path / "sudo";
    {
        std::ofstream output(regular);
        output << "#%PAM-1.0\n"
                  "auth       sufficient                  pam_surface_ir.so quiet\n"
                  "# Custom comment mentioning pam_surface_ir.so\n"
                  "auth include system-auth";
    }
    expect(removeLegacyPamHook(regular), "legacy hook removed");
    expect(readText(regular) == "#%PAM-1.0\n# Custom comment mentioning pam_surface_ir.so\nauth include system-auth",
           "legacy cleanup preserves other policy text");
    expect(!removeLegacyPamHook(regular), "legacy cleanup is idempotent");
    constexpr std::string_view customPolicy = "#%PAM-1.0\nauth required pam_deny.so\n";
    {
        std::ofstream output(pam);
        output << customPolicy;
    }
    bool refusedOverwrite = false;
    try {
        installKdeFacePam(directory.path);
    } catch (const std::runtime_error &) {
        refusedOverwrite = true;
    }
    expect(refusedOverwrite && readText(pam) == customPolicy, "custom PAM policy preserved");

    const auto config = loadConfig(directory.path / "config.toml");
    expect(config.ir.current == 390.0 && config.ir.exposureMax == 340 &&
           config.timeout == 2750ms && config.threshold == 0.45 && config.activeIr, "existing TOML keys and numeric types");
    {
        std::ofstream document(directory.path / "demo.json");
        document << R"({"user":"demo","version":1,"samples":[{"embedding":[1,0],"det_score":0.82,"extra":"keep"}]})";
    }
    const auto original = loadSamples("demo", directory.path);
    expect(original.size() == 1, "legacy JSON format");
    expect(saveSamples("demo", original, true, directory.path) == 2, "append preserves existing templates");
    const auto samples = loadSamples("demo", directory.path);
    expect(samples[0]["extra"].asString() == "keep" && samples[1]["det_score"].asDouble() == 0.82, "old sample fields");
    struct stat info{};
    expect(stat((directory.path / "demo.json").c_str(), &info) == 0 && (info.st_mode & 0777) == 0600, "private file");
    expect(clearSamples("demo", directory.path) && loadSamples("demo", directory.path).empty(), "clear");
    expect(!validUser("../demo") && !validUser("..") && validUser("local.user"), "account validation");
    expect(validUser(std::string(64, 'a')) && !validUser(std::string(65, 'a')), "account length");
    {
        std::ofstream corrupt(directory.path / "demo.json");
        corrupt << R"({"version":2,"samples":[]})";
    }
    bool rejectedFormat = false;
    try {
        loadSamples("demo", directory.path);
    } catch (const std::runtime_error &) {
        rejectedFormat = true;
    }
    expect(rejectedFormat, "unknown template version");

    constexpr int writers = 8;
    std::array<std::future<int>, writers> operations;
    for (int i = 0; i < writers; ++i) {
        operations[i] = std::async(std::launch::async, [&, i] {
            auto sample = original;
            sample[0]["test_writer"] = i;
            return saveSamples("parallel", sample, true, directory.path);
        });
    }
    std::array<bool, writers> savedCounts{};
    for (auto &operation : operations) {
        const auto count = operation.get();
        expect(count >= 1 && count <= writers, "concurrent append count");
        savedCounts[count - 1] = true;
    }
    const auto combined = loadSamples("parallel", directory.path);
    expect(combined.size() == writers, "all concurrent appends survived");
    for (const auto saved : savedCounts)
        expect(saved, "commits were serialized");
}

void protocol()
{
    int descriptors[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, descriptors) < 0)
        syserror("Create test socket pair");
    Fd input(descriptors[0]), output(descriptors[1]);
    writeAll(output.get(), "AUTH local.user\n");
    expect(readLine(input.get(), Clock::now() + 1s) == "AUTH local.user", "legacy socket wire protocol");
}
}

int main()
{
    try {
        packedFrames();
        compatibility();
        protocol();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
