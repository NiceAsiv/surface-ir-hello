// SPDX-License-Identifier: MIT
#include "core.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <format>
#include <mutex>
#include <thread>

namespace surface {
namespace {
struct Detection {
    std::optional<Face> face;
    EnrollmentHint hint = EnrollmentHint::Center;
    Clock::time_point frameTime{};
    int captured = 0;
};

struct Frame {
    cv::Mat image;
    Clock::time_point time;
};

Json::Value makeSample(const Face &face)
{
    Json::Value sample;
    auto &vector = sample["embedding"] = Json::Value(Json::arrayValue);
    const auto *feature = face.embedding.ptr<float>();
    for (int i = 0; i < face.embedding.cols; ++i)
        vector.append(std::round(double(feature[i]) * 1000000) / 1000000);
    sample["angle"] = face.angle;
    sample["ts"] = static_cast<Json::Int64>(std::time(nullptr));
    sample["det_score"] = std::round(double(face.confidence) * 10000) / 10000;
    return sample;
}

class Detector {
public:
    Detector(Engine &engine, int target)
        : thread_([this, &engine, target](std::stop_token stop) { run(engine, target, stop); })
    {
    }

    void submit(const cv::Mat &image, Clock::time_point now)
    {
        {
            std::lock_guard lock(mutex_);
            pending_ = Frame{image, now};
        }
        ready_.notify_one();
    }

    Detection latest()
    {
        std::lock_guard lock(mutex_);
        if (failure_)
            std::rethrow_exception(failure_);
        return latest_;
    }

    Json::Value finish()
    {
        thread_.request_stop();
        thread_.join();
        if (failure_)
            std::rethrow_exception(failure_);
        return std::move(samples_);
    }

private:
    void run(Engine &engine, int target, std::stop_token stop)
    {
        try {
            std::vector<cv::Mat> embeddings;
            auto nextCapture = Clock::time_point{};
            while (!stop.stop_requested() && int(samples_.size()) < target) {
                std::unique_lock lock(mutex_);
                if (!ready_.wait(lock, stop, [this] { return pending_.has_value(); }))
                    return;
                auto frame = std::exchange(pending_, std::nullopt);
                lock.unlock();
                if (stop.stop_requested() || frame->time < nextCapture)
                    continue;

                auto face = engine.detect(frame->image);
                if (stop.stop_requested())
                    return;
                auto hint = EnrollmentHint::Center;
                if (face) {
                    if (face->confidence < 0.72f) {
                        hint = EnrollmentHint::Hold;
                    } else if (std::any_of(embeddings.begin(), embeddings.end(), [&](const auto &sample) {
                        return engine.compare(face->embedding, sample) > 0.96;
                    })) {
                        hint = EnrollmentHint::Vary;
                    } else {
                        nextCapture = Clock::now() + 600ms;
                        embeddings.push_back(face->embedding);
                        samples_.append(makeSample(*face));
                        hint = EnrollmentHint::Captured;
                    }
                }
                lock.lock();
                latest_ = {std::move(face), hint, frame->time, int(samples_.size())};
            }
        } catch (...) {
            std::lock_guard lock(mutex_);
            failure_ = std::current_exception();
        }
    }

    std::mutex mutex_;
    std::condition_variable_any ready_;
    std::optional<Frame> pending_;
    Detection latest_;
    std::exception_ptr failure_;
    Json::Value samples_{Json::arrayValue};
    std::jthread thread_;
};
}

std::string enrollmentStatus(const EnrollmentProgress &progress, int target)
{
    std::string message;
    switch (progress.hint) {
    case EnrollmentHint::Warmup:
        message = "Adjusting infrared exposure.";
        break;
    case EnrollmentHint::Center:
        message = "Center your face and look at the camera.";
        break;
    case EnrollmentHint::Hold:
        message = "Hold still and keep your face in view.";
        break;
    case EnrollmentHint::Vary:
        message = "Turn your head slightly to capture a different angle.";
        break;
    case EnrollmentHint::Captured:
        message = std::format("Captured (detection confidence {:.3f}). Slowly change your angle.", progress.confidence);
        break;
    }
    return std::format("[{}/{}] {}", progress.captured, target, message);
}

Json::Value capture(Engine &engine, const IrConfig &config, int target, const Progress &report, const Cancel &cancel)
{
    Detector detector(engine, target);
    {
        Camera camera(config);
        camera.startEmitter();
        auto nextPreview = Clock::time_point{}, nextDetection = Clock::time_point{};
        const auto deadline = Clock::now() + std::max(25s, std::chrono::seconds(target * 3));
        int displayed = 0;
        for (int frame = 1; Clock::now() < deadline; ++frame) {
            if (interrupted || (cancel && cancel()))
                throw Cancelled{};
            const auto image = camera.frame(std::min(deadline, Clock::now() + 1500ms));
            if (frame % 3 == 0)
                camera.updateExposure(image);
            const auto now = Clock::now();
            const auto detection = detector.latest();
            if (frame > 12 && now >= nextDetection && detection.captured < target) {
                detector.submit(image, now);
                nextDetection = now + 100ms;
            }
            if (report && (now >= nextPreview || detection.captured != displayed)) {
                const auto *face = detection.face && now - detection.frameTime <= 500ms ? &*detection.face : nullptr;
                report({image, face, detection.captured,
                        frame <= 12 ? EnrollmentHint::Warmup : detection.hint,
                        detection.face ? detection.face->confidence : 0.0f});
                nextPreview = now + 60ms;
                displayed = detection.captured;
            }
            if (detection.captured >= target)
                break;
        }
    }
    return detector.finish();
}
}
