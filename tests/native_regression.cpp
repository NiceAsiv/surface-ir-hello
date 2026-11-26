// SPDX-License-Identifier: MIT
#include "core.hpp"
#include "wire.hpp"

#include <cmath>
#include <sys/stat.h>
#include <sys/socket.h>
#include <iostream>

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
