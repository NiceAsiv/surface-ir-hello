// SPDX-License-Identifier: MIT
#include "core.hpp"
#include "wire.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <opencv2/imgcodecs.hpp>
#include <openssl/evp.h>

namespace {
using namespace surface;
constexpr int sampleCount = 5;

void event(std::string_view type, std::string_view message, Json::Value values = Json::Value(Json::objectValue))
{
    values["event"] = std::string(type);
    values["message"] = std::string(message);
    std::cout << jsonText(values) << std::endl;
    if (!std::cout)
        throw Cancelled{};
}

void preview(const cv::Mat &frame, const Face *face, int captured, std::string_view status)
{
    const auto gray = previewGray(frame, face);
    cv::Mat small;
    cv::resize(gray, small, {160, static_cast<int>(std::round(160.0 * gray.rows / gray.cols))}, 0, 0, cv::INTER_AREA);
    std::vector<unsigned char> png;
    if (!cv::imencode(".png", small, png))
        return;
    std::string encoded(4 * ((png.size() + 2) / 3) + 1, '\0');
    const auto length = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()), png.data(), png.size());
    if (length <= 0)
        return;
    encoded.resize(length);
    Json::Value payload;
    payload["captured"] = captured;
    payload["image"] = "data:image/png;base64," + encoded;
    event("preview", status, std::move(payload));
}

std::string authorizedUser(int argc)
{
    const char *caller = std::getenv("PKEXEC_UID");
    if (geteuid() != 0 || argc != 1 || !caller)
        throw std::runtime_error("Run the enrollment helper through Polkit without arguments");
    const std::string_view raw(caller);
    uid_t uid{};
    const auto [end, status] = std::from_chars(raw.data(), raw.data() + raw.size(), uid);
    if (status != std::errc{} || end != raw.data() + raw.size() || uid == 0)
        throw std::runtime_error("An authenticated desktop user is required");
    const auto user = userForUid(uid);
    if (!validUser(user))
        throw std::runtime_error("The desktop account name cannot be used for enrollment");
    return user;
}

int enroll(const std::string &user)
{
    const auto cancel = [] { return readable(STDIN_FILENO, Clock::now()); };
    if (cancel())
        throw Cancelled{};
    const auto config = loadConfig();
    Engine engine;
    const auto samples = capture(engine, config.ir, sampleCount, preview, cancel);
    if (samples.size() != sampleCount) {
        event("result", "The capture timed out before all five samples were collected. Existing samples were kept.");
        return 1;
    }
    event("ready", "Return to your normal unlock position, then choose Verify and save.");
    if (readLine(STDIN_FILENO, Clock::now() + 45s) != "verify")
        throw Cancelled{};
    const auto result = verify(engine, samples, config, cancel);
    if (!result.ok) {
        event("result", "Live match failed. Existing samples were kept. " + result.message);
        return 1;
    }
    if (cancel())
        throw Cancelled{};
    const auto total = saveSamples(user, samples, true);
    event("result", "Saved five new face samples (" + std::to_string(total) + " total). " + result.message);
    return 0;
}
}

int main(int argc, char **)
{
    surface::installSignals();
    try {
        return enroll(authorizedUser(argc));
    } catch (const surface::Cancelled &) {
        return 130;
    } catch (const std::exception &error) {
        std::cerr << "Surface IR enrollment: " << error.what() << '\n';
        try {
            event("result", "Could not finish enrollment. Check that the infrared camera is available.");
        } catch (const surface::Cancelled &) {
            return 130;
        }
        return 1;
    }
}
