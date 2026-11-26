// SPDX-License-Identifier: MIT
#pragma once
#include "base.hpp"

#include <opencv2/core.hpp>
#include <memory>

namespace surface {
struct IrConfig {
    double current = 390.0;
    int exposureMax = 380;
    double gainMax = 8.0;
    double targetP95 = 600.0;
    int led = 2;
};

struct Topology {
    std::string product, media, sensor, csi, video, subdevice, csiSubdevice;
    int sensorBus = 0, sensorAddress = 0, pmicBus = 0;
    bool linkEnabled = false;
};

Topology discoverTopology();
double percentile(const cv::Mat &frame, int step, double q);
cv::Mat unpackIp3y(const unsigned char *data, unsigned stride, int width, int height);

class Camera {
public:
    explicit Camera(const IrConfig &config, Clock::time_point deadline = Clock::now() + 2s);
    ~Camera();
    cv::Mat frame(Clock::time_point deadline = Clock::now() + 1500ms);
    void startEmitter(int led = 0);
    void applyExposure(bool strobe);
    double updateExposure(const cv::Mat &frame);
    unsigned status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
