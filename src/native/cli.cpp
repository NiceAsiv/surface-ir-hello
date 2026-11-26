// SPDX-License-Identifier: MIT
#include "core.hpp"
#include "pam_config.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <format>
#include <iostream>
#include <opencv2/imgcodecs.hpp>
#include <sys/stat.h>
#include <termios.h>

namespace {
using namespace surface;
constexpr auto program = "surface-ir";

void requireRoot()
{
    if (geteuid() != 0)
        throw std::runtime_error("This command requires root privileges (run with sudo)");
}

void usage(std::ostream &out)
{
    out << "Usage: surface-ir COMMAND [options]\n"
           "  hwcheck [--save-debug FILE]                 Check the IR camera and emitter\n"
           "  enroll [USER] [--samples N] [--append] [--preview] [--check]\n"
           "                                              Enroll IR face samples\n"
           "  verify [USER]                               Test live IR verification\n"
           "  list                                        List enrolled users\n"
           "  clear [USER]                                Delete a user's samples\n"
           "  pam-enable                                  Install the dedicated KDE Face PAM policy\n"
           "  pam-disable                                 Disable legacy PAM hooks\n";
}

struct Options {
    std::string user, debugFile;
    int samples = 5;
    bool append = false, preview = false, check = false;
};

int sampleCount(std::string_view value)
{
    int parsed{};
    const auto [end, result] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result != std::errc{} || end != value.data() + value.size() || parsed < 1 || parsed > 100)
        throw std::invalid_argument("--samples must be an integer from 1 to 100");
    return parsed;
}

Options options(std::string_view command, int argc, char **argv)
{
    Options result;
    bool endOptions = false;
    for (int i = 2; i < argc; ++i) {
        const std::string_view raw(argv[i]);
        if (!endOptions && raw == "--") {
            endOptions = true;
            continue;
        }
        const auto equal = raw.find('=');
        const auto name = raw.substr(0, equal);
        const auto parameter = [&]() -> std::string_view {
            if (equal != raw.npos)
                return raw.substr(equal + 1);
            if (++i >= argc)
                throw std::invalid_argument("Missing value for " + std::string(name));
            return argv[i];
        };
        if (!endOptions && raw.starts_with('-')) {
            if (command == "enroll" && name == "--samples")
                result.samples = sampleCount(parameter());
            else if (command == "hwcheck" && name == "--save-debug")
                result.debugFile = parameter();
            else if (command == "enroll" && raw == "--append")
                result.append = true;
            else if (command == "enroll" && raw == "--preview")
                result.preview = true;
            else if (command == "enroll" && raw == "--check")
                result.check = true;
            else
                throw std::invalid_argument("Unknown option: " + std::string(raw));
        } else if (result.user.empty() && (command == "enroll" || command == "verify" || command == "clear")) {
            result.user = raw;
        } else {
            throw std::invalid_argument("Unexpected argument: " + std::string(raw));
        }
    }
    return result;
}

std::string targetUser(const Options &opts)
{
    std::string user = opts.user;
    if (user.empty()) {
        const char *caller = std::getenv("SUDO_USER");
        user = caller && geteuid() == 0 ? caller : userForUid(getuid());
    }
    if (!validUser(user))
        throw std::invalid_argument("Invalid account name");
    return user;
}

void hwcheck(const Options &opts)
{
    requireRoot();
    const auto topology = discoverTopology();
    std::cout << "=== Surface IR Hardware Topology ===\n"
              << "  product        : " << topology.product << '\n'
              << "  media          : " << topology.media << '\n'
              << "  sensor         : " << topology.sensor << '\n'
              << "  CSI            : " << topology.csi << '\n'
              << "  video          : " << topology.video << '\n'
              << "  subdevice      : " << topology.subdevice << '\n'
              << "  PMIC bus       : " << topology.pmicBus << '\n';
    const auto ir = loadConfig().ir;
    cv::Mat lit;
    {
        Camera camera(ir);
        camera.applyExposure(false);
        cv::Mat ambient;
        for (int i = 0; i < 3; ++i)
            ambient = camera.frame();
        const auto stats = [](const cv::Mat &image) {
            double maximum{};
            cv::minMaxLoc(image, nullptr, &maximum);
            return std::format("mean={:.1f}, p95={:.1f}, max={:.0f}", cv::mean(image)[0], percentile(image, 1, 0.95), maximum);
        };
        std::cout << "  Ambient frame  : " << stats(ambient) << '\n';
        const auto checkLed = [&](const int led) {
            camera.startEmitter(led);
            for (int i = 0; i < 8; ++i) {
                lit = camera.frame();
                camera.updateExposure(lit);
            }
            const auto status = camera.status();
            std::cout << std::format("  Emitter LED{}   : {}, GSTAT=0x{:02x}\n", led, stats(lit), status);
            return status;
        };
        if (checkLed(ir.led) & 0x08)
            checkLed(3 - ir.led);
    }
    Engine engine;
    if (const auto face = engine.detect(lit))
        std::cout << std::format("  Face detected  : YES (rotation={} deg, score={:.3f})\n", face->angle, face->confidence);
    else
        std::cout << "  Face detected  : None in frame\n";
    if (!opts.debugFile.empty()) {
        const auto extension = fs::path(opts.debugFile).extension().string();
        if (extension.empty())
            throw std::invalid_argument("Provide an image extension for --save-debug");
        std::vector<unsigned char> encoded;
        if (!cv::imencode(extension, normalize(lit), encoded))
            throw std::runtime_error("Could not encode the debug image");
        TemporaryFile output(opts.debugFile);
        writeAll(output.fd(), {reinterpret_cast<const char *>(encoded.data()), encoded.size()});
        output.commit(opts.debugFile);
        std::cout << "  Saved IR image : " << opts.debugFile << '\n';
    }
}

class AlternateScreen {
public:
    explicit AlternateScreen(bool enabled) : enabled_(enabled)
    {
        if (enabled_)
            std::cout << "\x1b[?1049hOpening the infrared camera. Press Ctrl+C to cancel.\n" << std::flush;
    }
    ~AlternateScreen()
    {
        if (enabled_)
            std::cout << "\x1b[?1049l" << std::flush;
    }

private:
    bool enabled_;
};

void terminalPreview(const cv::Mat &frame, const Face *face, std::string_view status)
{
    const auto gray = previewGray(frame, face);
    winsize screen{};
    const auto knownSize = ioctl(STDOUT_FILENO, TIOCGWINSZ, &screen) == 0 && screen.ws_col && screen.ws_row;
    const auto width = std::clamp(knownSize ? screen.ws_col - 1 : 48, 1, 48);
    const auto height = std::clamp(width * gray.rows / gray.cols / 2, 1, std::max(1, knownSize ? screen.ws_row - 3 : 16));
    cv::Mat resized;
    cv::resize(gray, resized, {width, height}, 0, 0, cv::INTER_AREA);
    constexpr std::string_view shades = " .:-=+*#%@";
    std::string text = "\x1b[H\x1b[J";
    for (int row = 0; row < resized.rows; ++row) {
        const auto *pixels = resized.ptr<unsigned char>(row);
        for (int col = 0; col < resized.cols; ++col)
            text += shades[pixels[col] * (shades.size() - 1) / 255];
        text += '\n';
    }
    std::cout << text << status << '\n' << std::flush;
}

int enroll(const Options &opts)
{
    requireRoot();
    if (opts.preview && !isatty(STDOUT_FILENO))
        throw std::invalid_argument("--preview requires an interactive terminal");
    if (opts.check && !isatty(STDIN_FILENO))
        throw std::invalid_argument("--check requires an interactive terminal");

    const auto user = targetUser(opts);
    const auto config = loadConfig();
    Engine engine;
    std::cout << std::format("=== Enrolling IR face for '{}' ({} samples) ===\n", user, opts.samples)
              << "Look at the top bezel camera. Slowly vary your head angle and distance.\n" << std::flush;
    Json::Value samples;
    {
        AlternateScreen screen(opts.preview);
        std::string previous;
        samples = capture(engine, config.ir, opts.samples, [&](const EnrollmentProgress &progress) {
            const auto status = enrollmentStatus(progress, opts.samples);
            if (opts.preview)
                terminalPreview(progress.frame, progress.face, status);
            else if (previous != status) {
                std::cout << status << std::endl;
                previous = status;
            }
        });
    }
    if (static_cast<int>(samples.size()) != opts.samples)
        throw std::runtime_error(std::format("Captured {}/{} samples before timeout. Existing templates were kept.",
                                            samples.size(), opts.samples));
    double minimum = 1, maximum = 0;
    for (const auto &sample : samples) {
        const double confidence = sample["det_score"].asDouble();
        minimum = std::min(minimum, confidence);
        maximum = std::max(maximum, confidence);
    }
    std::cout << std::format("Captured {} samples. Detection confidence {:.3f}-{:.3f}.\n", samples.size(), minimum, maximum);
    if (opts.check) {
        std::cout << "Return to your normal unlock position and press Enter for a fresh match. " << std::flush;
        std::string input;
        if (!std::getline(std::cin, input))
            throw Cancelled{};
        const auto check = verify(engine, samples, config);
        std::cout << std::format("New-sample verification (threshold {:.3f}): {}\n", config.threshold, check.message);
        if (!check.ok)
            throw std::runtime_error("Live verification did not pass. Existing templates were kept.");
    }

    const auto total = saveSamples(user, samples, opts.append);
    std::cout << std::format("Saved {} new sample(s) for '{}' ({} total).\n", samples.size(), user, total);
    return 0;
}

int checkFace(const Options &opts)
{
    const auto user = targetUser(opts);
    if (fs::exists(socketPath)) {
        const auto start = Clock::now();
        const auto result = request("AUTH " + user);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
        std::cout << std::format("Daemon response ({} ms): {}\n", elapsed, result.message);
        return result.ok ? 0 : 1;
    }
    requireRoot();
    Engine engine;
    const auto result = verify(engine, loadSamples(user), loadConfig());
    std::cout << result.message << '\n';
    return result.ok ? 0 : 1;
}

void listUsers()
{
    requireRoot();
    std::vector<std::string> users;
    if (fs::exists(facesDir)) {
        for (const auto &entry : fs::directory_iterator(facesDir)) {
            if (entry.path().extension() == ".json" && validUser(entry.path().stem().string()))
                users.push_back(entry.path().stem().string());
        }
    }
    std::sort(users.begin(), users.end());
    if (users.empty())
        std::cout << "No enrolled users yet.\n";
    for (const auto &user : users)
        std::cout << std::format("  {}: {} IR template(s)\n", user, loadSamples(user).size());
}

void changePam(bool enable)
{
    requireRoot();
    const fs::path directory = "/etc/pam.d";
    if (enable) {
        const bool installed = installKdeFacePam(directory);
        std::cout << (installed ? "Installed " : "Already installed ") << (directory / "kde-face").string() << '\n';
        return;
    }

    for (const auto *service : {"sudo", "kde", "sddm"}) {
        if (removeLegacyPamHook(directory / service))
            std::cout << "Disabled the legacy hook in " << (directory / service).string() << '\n';
    }
}
}

int main(int argc, char **argv)
{
    surface::installSignals();
    try {
        if (argc < 2) {
            usage(std::cerr);
            return 2;
        }
        const std::string_view command(argv[1]);
        if (command == "--version") {
            std::cout << "surface-ir " << SURFACE_IR_VERSION << " (C++)\n";
            return 0;
        }
        if (command == "--help" || command == "-h" ||
            (argc == 3 && (std::string_view(argv[2]) == "--help" || std::string_view(argv[2]) == "-h"))) {
            usage(std::cout);
            return 0;
        }

        const auto opts = options(command, argc, argv);
        if (command == "hwcheck")
            hwcheck(opts);
        else if (command == "enroll")
            return enroll(opts);
        else if (command == "verify")
            return checkFace(opts);
        else if (command == "list")
            listUsers();
        else if (command == "clear") {
            requireRoot();
            const auto user = targetUser(opts);
            std::cout << (surface::clearSamples(user) ? "Removed templates for " : "No templates found for ") << user << '\n';
        } else if (command == "pam-enable" || command == "pam-disable")
            changePam(command == "pam-enable");
        else
            throw std::invalid_argument("Unknown command: " + std::string(command));
        return 0;
    } catch (const surface::Cancelled &) {
        std::cerr << "Cancelled.\n";
        return 130;
    } catch (const std::invalid_argument &error) {
        std::cerr << program << ": " << error.what() << '\n';
        return 2;
    } catch (const std::exception &error) {
        std::cerr << program << ": " << error.what() << '\n';
        return 1;
    }
}
