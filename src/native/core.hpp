// SPDX-License-Identifier: MIT
#pragma once
#include "camera.hpp"

#include <json/json.h>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect/face.hpp>

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace surface {
inline constexpr auto socketPath = "/run/surface-ir-hello/auth.sock";
inline constexpr auto facesDir = "/etc/surface-ir-hello/faces";
inline constexpr auto modelDir = "/usr/share/surface-ir-hello/models";
inline constexpr std::array<int, 4> rotations{0, 180, 90, 270};

struct Config {
    IrConfig ir;
    double threshold = 0.40;
    std::chrono::milliseconds timeout{2500};
    bool activeIr = true;
};

Config loadConfig(const fs::path &path = "/etc/surface-ir-hello/config.toml");
bool validUser(std::string_view user);
std::string userForUid(uid_t uid);

Json::Value loadSamples(const std::string &user, const fs::path &directory = facesDir);
int saveSamples(const std::string &user, const Json::Value &newSamples, bool append, const fs::path &directory = facesDir);
bool clearSamples(const std::string &user, const fs::path &directory = facesDir);
std::string jsonText(const Json::Value &value);

struct Face {
    cv::Mat embedding;
    cv::Rect bounds;
    int angle;
    float confidence;
};

cv::Mat normalize(const cv::Mat &frame, cv::Ptr<cv::CLAHE> clahe = {});
cv::Mat previewGray(const cv::Mat &frame, const Face *face);

class Engine {
public:
    explicit Engine(const fs::path &models = modelDir);
    std::optional<Face> detect(const cv::Mat &frame, std::span<const int> angles = rotations, bool half = false);
    double compare(const cv::Mat &left, const cv::Mat &right) const;

private:
    cv::Ptr<cv::FaceDetectorYN> detector_;
    cv::Ptr<cv::FaceRecognizerSF> recognizer_;
    cv::Ptr<cv::CLAHE> clahe_;
};

struct Verification {
    bool ok;
    std::string message;
};

using Cancel = std::function<bool()>;
enum class EnrollmentHint { Warmup, Center, Hold, Vary, Captured };
struct EnrollmentProgress {
    const cv::Mat &frame;
    const Face *face;
    int captured;
    EnrollmentHint hint;
    float confidence;
};
using Progress = std::function<void(const EnrollmentProgress &)>;
std::string enrollmentStatus(const EnrollmentProgress &progress, int target);
Verification verify(Engine &engine, const Json::Value &samples, const Config &config, const Cancel &cancel = {});
Json::Value capture(Engine &engine, const IrConfig &config, int target, const Progress &report, const Cancel &cancel = {});
Verification request(std::string_view query, const fs::path &socket = socketPath);
}
