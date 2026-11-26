// SPDX-License-Identifier: MIT
#include "core.hpp"

#include <curl/curl.h>
#include <openssl/evp.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <format>
#include <iostream>

namespace surface {
namespace {
struct Model {
    const char *name;
    const char *repository;
    const char *sha256;
};
constexpr Model yunet{"face_detection_yunet_2023mar.onnx", "face_detection_yunet",
                      "8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4"};
constexpr Model sface{"face_recognition_sface_2021dec.onnx", "face_recognition_sface",
                      "0ba9fbfa01b5270c96627c4ef784da859931e02f04419c829e83484087c34e79"};

bool matchesDigest(const fs::path &path, std::string_view expected)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return false;
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hash(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!hash || !EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr))
        throw std::runtime_error("Could not initialize SHA-256");
    std::array<char, 65536> buffer{};
    while (stream.read(buffer.data(), buffer.size()) || stream.gcount()) {
        if (!EVP_DigestUpdate(hash.get(), buffer.data(), stream.gcount()))
            throw std::runtime_error("Could not hash model");
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned count = 0;
    if (stream.bad() || !EVP_DigestFinal_ex(hash.get(), digest.data(), &count))
        return false;
    std::string hex;
    for (unsigned i = 0; i < count; ++i)
        hex += std::format("{:02x}", digest[i]);
    return hex == expected;
}

int progress(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return interrupted ? 1 : 0;
}

bool download(const std::string &url, int fd)
{
    if (ftruncate(fd, 0) < 0 || lseek(fd, 0, SEEK_SET) < 0)
        syserror("Reset model download");
    const auto outputFd = dup(fd);
    if (outputFd < 0)
        syserror("Duplicate model download descriptor");
    const auto closeFile = [](FILE *file) { fclose(file); };
    std::unique_ptr<FILE, decltype(closeFile)> file(fdopen(outputFd, "wb"), closeFile);
    if (!file) {
        close(outputFd);
        syserror("Open model download stream");
    }
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl)
        throw std::runtime_error("Could not initialize HTTPS transfer");
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, file.get());
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl.get(), CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 180L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(50 * 1024 * 1024));
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progress);
    const auto result = curl_easy_perform(curl.get());
    if (interrupted)
        throw Cancelled{};
    return result == CURLE_OK && fflush(file.get()) == 0;
}

fs::path ensureModel(const fs::path &directory, Model model)
{
    const auto path = directory / model.name;
    if (matchesDigest(path, model.sha256))
        return path;

    fs::create_directories(directory);
    TemporaryFile temporary(path);
    const std::string repo = model.repository;
    const std::array sources{
        "https://huggingface.co/opencv/" + repo + "/resolve/main/",
        "https://media.githubusercontent.com/media/opencv/opencv_zoo/main/models/" + repo + "/",
        "https://hf-mirror.com/opencv/" + repo + "/resolve/main/",
    };
    for (const auto &source : sources) {
        std::cerr << "[models] Fetching " << model.name << " from " << source << '\n';
        if (!download(source + model.name, temporary.fd()) || !matchesDigest(temporary.path(), model.sha256))
            continue;
        if (fchmod(temporary.fd(), 0644) < 0)
            syserror("Set model permissions");
        temporary.commit(path);
        return path;
    }
    throw std::runtime_error("Could not download a SHA-256 verified copy of " + std::string(model.name));
}

cv::Mat orient(const cv::Mat &image, int angle)
{
    if (angle == 0)
        return image;
    cv::Mat rotated;
    cv::rotate(image, rotated, angle == 180 ? cv::ROTATE_180 :
               angle == 90 ? cv::ROTATE_90_CLOCKWISE : cv::ROTATE_90_COUNTERCLOCKWISE);
    return rotated;
}
}

cv::Mat normalize(const cv::Mat &frame, cv::Ptr<cv::CLAHE> clahe)
{
    const auto low = percentile(frame, 4, 0.01);
    const auto high = percentile(frame, 4, 0.995);
    cv::Mat scaled(frame.rows, frame.cols, CV_8UC1);
    const bool expand = high - low >= 32;
    const auto range = static_cast<float>(high - low);
    for (int y = 0; y < frame.rows; ++y) {
        const auto *input = frame.ptr<std::uint16_t>(y);
        auto *output = scaled.ptr<unsigned char>(y);
        for (int x = 0; x < frame.cols; ++x) {
            if (expand) {
                const auto value = (static_cast<float>(input[x]) - static_cast<float>(low)) * (255.0f / range);
                output[x] = static_cast<unsigned char>(std::clamp(value, 0.0f, 255.0f));
            } else {
                output[x] = input[x] >> 2;
            }
        }
    }
    if (!clahe)
        clahe = cv::createCLAHE(2.2, {8, 8});
    cv::Mat enhanced, bgr;
    clahe->apply(scaled, enhanced);
    cv::cvtColor(enhanced, bgr, cv::COLOR_GRAY2BGR);
    return bgr;
}

cv::Mat previewGray(const cv::Mat &frame, const Face *face)
{
    cv::Mat gray;
    cv::cvtColor(face ? face->image : normalize(frame), gray, cv::COLOR_BGR2GRAY);
    if (face)
        cv::rectangle(gray, face->bounds, 255, 4);
    return gray;
}

Engine::Engine(const fs::path &models)
{
    const auto detection = ensureModel(models, yunet);
    const auto recognition = ensureModel(models, sface);
    detector_ = cv::FaceDetectorYN::create(detection.string(), "", {640, 480}, 0.60f, 0.3f, 5000);
    recognizer_ = cv::FaceRecognizerSF::create(recognition.string(), "");
    clahe_ = cv::createCLAHE(2.2, {8, 8});
}

std::optional<Face> Engine::detect(const cv::Mat &frame, std::span<const int> angles, bool half)
{
    const auto base = normalize(frame, clahe_);
    for (const auto angle : angles) {
        auto image = orient(base, angle);
        cv::Mat input = image;
        if (half)
            cv::resize(image, input, {image.cols / 2, image.rows / 2}, 0, 0, cv::INTER_AREA);
        detector_->setInputSize(input.size());
        cv::Mat faces;
        detector_->detect(input, faces);
        if (faces.empty())
            continue;

        int largest = 0;
        for (int i = 1; i < faces.rows; ++i) {
            const auto *candidate = faces.ptr<float>(i);
            const auto *current = faces.ptr<float>(largest);
            if (candidate[2] * candidate[3] > current[2] * current[3])
                largest = i;
        }
        auto best = faces.row(largest);
        auto *face = best.ptr<float>();
        if (half) {
            for (int field = 0; field < 14; field += 2) {
                face[field] *= float(image.cols) / input.cols;
                face[field + 1] *= float(image.rows) / input.rows;
            }
        }
        if (face[2] < 55 || face[3] < 55)
            continue;

        cv::Mat crop, feature;
        recognizer_->alignCrop(image, best, crop);
        recognizer_->feature(crop, feature);
        return Face{image, feature.reshape(1, 1).clone(),
                    {std::max(0, int(face[0])), std::max(0, int(face[1])), int(face[2]), int(face[3])},
                    angle, face[14]};
    }
    return std::nullopt;
}

double Engine::compare(const cv::Mat &left, const cv::Mat &right) const
{
    return recognizer_->match(left, right, cv::FaceRecognizerSF::FR_COSINE);
}

Verification verify(Engine &engine, const Json::Value &samples, const Config &config, const Cancel &cancel)
{
    std::vector<cv::Mat> templates;
    std::array<int, 4> angles = rotations;
    std::array<int, 4> counts{};
    for (const auto &sample : samples) {
        const auto &values = sample["embedding"];
        if (!values.isArray() || values.size() != 128)
            continue;
        cv::Mat feature(1, 128, CV_32F);
        auto *out = feature.ptr<float>();
        bool valid = true;
        for (Json::ArrayIndex i = 0; i < values.size(); ++i) {
            if (!values[i].isNumeric() || !std::isfinite(out[i] = values[i].asFloat())) {
                valid = false;
                break;
            }
        }
        if (!valid || cv::norm(feature) <= 0)
            continue;
        templates.push_back(std::move(feature));
        const int angle = sample["angle"].isInt() ? sample["angle"].asInt() : 0;
        const auto found = std::find(angles.begin(), angles.end(), angle);
        if (found != angles.end())
            ++counts[static_cast<std::size_t>(angle / 90)];
    }
    if (templates.empty())
        return {false, "NO_MODEL"};
    std::stable_sort(angles.begin(), angles.end(), [&](int a, int b) { return counts[a / 90] > counts[b / 90]; });

    const auto start = Clock::now();
    const auto deadline = start + config.timeout;
    double best = -1.0, peak = 0;
    int attempts = 0, frames = 0, faces = 0;
    const auto elapsed = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    };

    Camera camera(config.ir, deadline);
    camera.applyExposure(false);
    const double ambient = percentile(camera.frame(std::min(deadline, Clock::now() + 1500ms)), 8, 0.95);
    camera.startEmitter();
    while (Clock::now() < deadline && frames < 45) {
        if (interrupted || (cancel && cancel()))
            throw Cancelled{};
        const auto now = Clock::now();
        // Leave time for cleanup and password fallback if the frame stream stalls.
        const auto image = camera.frame(std::min(deadline, now + 1500ms));
        ++frames;
        const double lit = frames % 3 == 0 ? camera.updateExposure(image) : std::max(percentile(image, 8, 0.95), 8.0);
        if (frames <= 3)
            continue;
        peak = std::max(peak, lit);
        if (config.activeIr && ambient < 750 && lit < ambient * 1.12 && lit < 60)
            continue;

        const bool full = ++attempts % 4 == 0;
        const auto values = std::span<const int>(angles).first(full ? angles.size() : 1);
        const auto face = engine.detect(image, values, !full);
        if (!face)
            continue;
        ++faces;
        double score = -1.0;
        for (const auto &feature : templates)
            score = std::max(score, engine.compare(face->embedding, feature));
        best = std::max(best, score);
        if (score >= config.threshold)
            return {true, std::format("OK score={:.3f} ms={} frames={}", score, elapsed(), frames)};
    }
    return {false, std::format("FAIL best={:.3f} ms={} frames={} faces={} p95={:.0f}/{:.0f}",
                               best, elapsed(), frames, faces, ambient, peak)};
}

Json::Value capture(Engine &engine, const IrConfig &config, int target, const Progress &report, const Cancel &cancel)
{
    Json::Value samples(Json::arrayValue);
    std::vector<cv::Mat> embeddings;
    Camera camera(config);
    camera.startEmitter();
    auto lastCapture = Clock::time_point::min(), lastReport = Clock::now() - 1s;
    std::string previous;
    const auto deadline = Clock::now() + std::max(25s, std::chrono::seconds(target * 3));
    for (int frame = 1; int(samples.size()) < target && Clock::now() < deadline; ++frame) {
        if (interrupted || (cancel && cancel()))
            throw Cancelled{};
        const auto image = camera.frame(std::min(deadline, Clock::now() + 1500ms));
        if (frame % 3 == 0)
            camera.updateExposure(image);
        const auto now = Clock::now();
        if (frame <= 12 || (lastCapture != Clock::time_point::min() && now - lastCapture < 600ms))
            continue;

        const auto face = engine.detect(image);
        std::string description;
        if (!face) {
            description = "Face not found. Center your face and move a little closer.";
        } else if (face->confidence < 0.72f) {
            description = "Face detected but unclear. Hold still and look toward the camera.";
        } else if (std::any_of(embeddings.begin(), embeddings.end(), [&](const auto &sample) {
            return engine.compare(face->embedding, sample) > 0.96;
        })) {
            description = "This view is similar to a sample already captured. Change your angle slightly.";
        } else {
            lastCapture = now;
            embeddings.push_back(face->embedding);
            Json::Value sample;
            auto &vector = sample["embedding"] = Json::Value(Json::arrayValue);
            const auto *feature = face->embedding.ptr<float>();
            for (int i = 0; i < face->embedding.cols; ++i)
                vector.append(std::round(double(feature[i]) * 1000000) / 1000000);
            sample["angle"] = face->angle;
            sample["ts"] = static_cast<Json::Int64>(std::time(nullptr));
            sample["det_score"] = std::round(double(face->confidence) * 10000) / 10000;
            samples.append(sample);
            description = std::format("Captured. Detection confidence {:.3f}. Slowly change your pose.", face->confidence);
        }
        const auto status = std::format("[{}/{}] {}", samples.size(), target, description);
        if (report && (status != previous || now - lastReport >= 250ms)) {
            report(image, face ? &*face : nullptr, samples.size(), status);
            lastReport = now;
            previous = status;
        }
    }
    return samples;
}
}
