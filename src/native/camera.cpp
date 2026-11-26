// SPDX-License-Identifier: MIT
#include "camera.hpp"

#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <linux/media-bus-format.h>
#include <linux/media.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <sys/file.h>
#include <sys/mman.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <optional>
#include <regex>
#include <thread>
#include <vector>

namespace surface {
namespace {
constexpr int width = 640, height = 480;
constexpr unsigned pmicAddress = 0x4d, pmicRevision = 0x21;
constexpr unsigned statusReg = 0x01, maxCurrent = 0x2f, timeoutReg = 0x30;
constexpr unsigned current1 = 0x34, current2 = 0x35, control = 0x36, revision = 0xff;
constexpr unsigned padEnable = 0x3005, strobeMask = 0x3b81, strobeSpan = 0x3b8c;
constexpr int exposureLimit = 400, spanMargin = 20;
constexpr std::array savedPmic{0x28u, 0x2du, maxCurrent, timeoutReg, current1, current2};

std::string nodeNamed(const std::string &prefix, const std::string &name)
{
    for (const auto &entry : fs::directory_iterator("/sys/class/video4linux")) {
        try {
            const auto filename = entry.path().filename().string();
            if (filename.starts_with(prefix) && trim(readText(entry.path() / "name")) == name)
                return "/dev/" + filename;
        } catch (const std::exception &) {
            // A media node can disappear while enumerating the directory.
        }
    }
    throw std::runtime_error("Missing video node for " + name);
}

struct MediaRoute {
    Topology topology;
    media_link_desc link{};
};

MediaRoute findRoute()
{
    const std::regex mediaName(R"(media[0-9]+)");
    const std::regex sensorName(R"(ov7251 ([0-9]+)-([0-9a-f]{4}))");
    const std::regex csiName(R"(ipu3-csi2 ([0-9]+))");
    for (const auto &entry : fs::directory_iterator("/dev")) {
        if (!std::regex_match(entry.path().filename().string(), mediaName))
            continue;
        const Fd media(open(entry.path().c_str(), O_RDWR | O_CLOEXEC));
        if (!media)
            continue;

        std::vector<media_entity_desc> entities;
        media_entity_desc entity{};
        entity.id = MEDIA_ENT_ID_FLAG_NEXT;
        while (ioctl(media.get(), MEDIA_IOC_ENUM_ENTITIES, &entity) == 0) {
            entities.push_back(entity);
            const auto next = entity.id | MEDIA_ENT_ID_FLAG_NEXT;
            entity = {};
            entity.id = next;
        }
        for (const auto &sensor : entities) {
            const std::string name = sensor.name;
            std::smatch address;
            if (!std::regex_match(name, address, sensorName))
                continue;
            std::vector<media_pad_desc> pads(sensor.pads);
            std::vector<media_link_desc> links(sensor.links);
            media_links_enum query{};
            query.entity = sensor.id;
            query.pads = pads.data();
            query.links = links.data();
            ioctlChecked(media.get(), MEDIA_IOC_ENUM_LINKS, query, "MEDIA_IOC_ENUM_LINKS");
            for (const auto &link : links) {
                if (link.source.entity != sensor.id || link.source.index != 0 || link.sink.index != 0)
                    continue;
                const auto sink = std::find_if(entities.begin(), entities.end(), [&](const auto &item) {
                    return item.id == link.sink.entity;
                });
                if (sink == entities.end())
                    continue;
                const std::string csi = sink->name;
                std::smatch channel;
                if (!std::regex_match(csi, channel, csiName))
                    continue;
                MediaRoute result{};
                auto &topology = result.topology;
                topology.product = trim(readText("/sys/class/dmi/id/product_name"));
                if (topology.product != "Surface Go" && topology.product != "Surface Go 2" &&
                    topology.product != "Surface Go 3")
                    throw std::runtime_error("Unsupported board for IR strobe: " + topology.product);
                topology.media = entry.path().string();
                topology.sensor = name;
                topology.csi = csi;
                topology.sensorBus = std::stoi(address[1]);
                topology.sensorAddress = std::stoi(address[2], nullptr, 16);
                topology.video = nodeNamed("video", "ipu3-cio2 " + channel[1].str());
                topology.subdevice = nodeNamed("v4l-subdev", name);
                topology.csiSubdevice = nodeNamed("v4l-subdev", csi);
                topology.linkEnabled = (link.flags & MEDIA_LNK_FL_ENABLED) != 0;
                result.link = link;
                for (const auto &device : fs::directory_iterator("/sys/bus/i2c/devices")) {
                    if (!device.path().filename().string().starts_with("i2c-INT3472:") ||
                        !fs::exists(device.path() / "tps68470-gpio"))
                        continue;
                    const auto parent = fs::canonical(device.path()).parent_path().filename().string();
                    if (parent.starts_with("i2c-")) {
                        topology.pmicBus = std::stoi(parent.substr(4));
                        return result;
                    }
                }
                throw std::runtime_error("No TPS68470 PMIC found");
            }
        }
    }
    throw std::runtime_error("No OV7251 to IPU3 CSI-2 media route found");
}

struct I2c {
    Fd fd;
    unsigned short address = 0;
    bool wide = false;

    std::vector<unsigned char> read(unsigned reg, unsigned short count = 1) const
    {
        unsigned char command[] = {static_cast<unsigned char>(reg >> 8), static_cast<unsigned char>(reg)};
        std::vector<unsigned char> data(count);
        i2c_msg messages[] = {
            {address, 0, static_cast<unsigned short>(wide ? 2 : 1), wide ? command : command + 1},
            {address, I2C_M_RD, count, data.data()},
        };
        i2c_rdwr_ioctl_data transfer{messages, 2};
        ioctlChecked(fd.get(), I2C_RDWR, transfer, "I2C register read");
        return data;
    }

    void write(unsigned reg, unsigned value) const
    {
        unsigned char data[] = {
            static_cast<unsigned char>(reg >> 8), static_cast<unsigned char>(reg), static_cast<unsigned char>(value),
        };
        i2c_msg message{address, 0, static_cast<unsigned short>(wide ? 3 : 2), wide ? data : data + 1};
        i2c_rdwr_ioctl_data transfer{&message, 1};
        ioctlChecked(fd.get(), I2C_RDWR, transfer, "I2C register write");
    }
};

bool supportsControl(int fd, unsigned id)
{
    v4l2_queryctrl query{};
    query.id = id;
    return ioctl(fd, VIDIOC_QUERYCTRL, &query) == 0 && !(query.flags & V4L2_CTRL_FLAG_DISABLED);
}

std::pair<int, int> limits(int fd, unsigned id)
{
    v4l2_queryctrl query{};
    query.id = id;
    ioctlChecked(fd, VIDIOC_QUERYCTRL, query, "VIDIOC_QUERYCTRL");
    return {query.minimum, query.maximum};
}

int getControl(int fd, unsigned id)
{
    v4l2_control ctrl{id, 0};
    ioctlChecked(fd, VIDIOC_G_CTRL, ctrl, "VIDIOC_G_CTRL");
    return ctrl.value;
}

void setControl(int fd, unsigned id, int value)
{
    v4l2_control ctrl{id, value};
    ioctlChecked(fd, VIDIOC_S_CTRL, ctrl, "VIDIOC_S_CTRL");
}

void setPadFormat(int fd, unsigned pad)
{
    v4l2_subdev_format request{};
    request.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    request.pad = pad;
    ioctlChecked(fd, VIDIOC_SUBDEV_G_FMT, request, "VIDIOC_SUBDEV_G_FMT");
    request.format.width = width;
    request.format.height = height;
    request.format.code = MEDIA_BUS_FMT_Y10_1X10;
    ioctlChecked(fd, VIDIOC_SUBDEV_S_FMT, request, "VIDIOC_SUBDEV_S_FMT");
    if (request.format.width != width || request.format.height != height || request.format.code != MEDIA_BUS_FMT_Y10_1X10)
        throw std::runtime_error("IR subdevice did not select Y10 at 640x480");
}

struct Mapping {
    void *address;
    std::size_t length;
};

void cleanup(const auto &action) noexcept
{
    try {
        action();
    } catch (const std::exception &error) {
        std::cerr << "IR cleanup: " << error.what() << '\n';
    }
}
}

Topology discoverTopology() { return findRoute().topology; }

cv::Mat unpackIp3y(const unsigned char *data, unsigned stride, int columns, int rows)
{
    cv::Mat frame(rows, columns, CV_16UC1);
    for (int y = 0; y < rows; ++y) {
        const auto *line = data + y * stride;
        auto *pixels = frame.ptr<std::uint16_t>(y);
        for (int i = 0; i < columns;) {
            const auto *block = line + (i / 25) * 32;
            for (int p = 0; p < 25 && i < columns; ++p, ++i) {
                const auto byte = p * 10 / 8;
                const auto word = unsigned(block[byte]) | unsigned(block[byte + 1]) << 8;
                pixels[i] = (word >> (p * 10 % 8)) & 0x3ff;
            }
        }
    }
    return frame;
}

double percentile(const cv::Mat &frame, int step, double q)
{
    std::array<int, 1024> counts{};
    int samples = 0;
    for (int y = 0; y < frame.rows; y += step) {
        const auto *row = frame.ptr<std::uint16_t>(y);
        for (int x = 0; x < frame.cols; x += step) {
            ++counts[row[x]];
            ++samples;
        }
    }
    const double rank = q * (samples - 1);
    const int lower = static_cast<int>(rank);
    int sum = 0, first = -1;
    for (int value = 0; value < static_cast<int>(counts.size()); ++value) {
        sum += counts[value];
        if (first < 0 && sum > lower)
            first = value;
        if (sum > static_cast<int>(std::ceil(rank)))
            return first + (value - first) * (rank - lower);
    }
    return 0;
}

struct Camera::Impl {
    Fd lock, media, sub, video;
    I2c pmic, sensor;
    media_link_desc link{};
    IrConfig cfg;
    std::vector<Mapping> buffers;
    std::vector<std::pair<unsigned, unsigned>> pmicSaved, sensorSaved;
    std::vector<std::pair<unsigned, int>> controlsSaved;
    bool changedLink = false, streaming = false;
    unsigned gainId = 0, stride = 0;
    v4l2_buf_type captureType = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    bool multiplanar() const { return captureType == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; }

    v4l2_buffer bufferFor(v4l2_plane &plane) const
    {
        v4l2_buffer buffer{};
        buffer.type = captureType;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (multiplanar()) {
            buffer.length = 1;
            buffer.m.planes = &plane;
        }
        return buffer;
    }
    int minimumExposure = 0, maximumExposure = 0, minimumGain = 0, maximumGain = 0;
    int aeHold = 0;
    std::optional<int> currentExposure, currentGain;
    double level = 220.0;

    ~Impl()
    {
        if (!pmicSaved.empty()) {
            cleanup([&] { pmic.write(control, 0); });
            for (const auto &[reg, value] : pmicSaved)
                cleanup([&] { pmic.write(reg, value); });
            cleanup([&] { pmic.write(statusReg, 0xfe); });
        }
        for (const auto &[reg, value] : sensorSaved)
            cleanup([&] { sensor.write(reg, value); });
        for (const auto &[id, value] : controlsSaved)
            cleanup([&] { setControl(sub.get(), id, value); });
        if (streaming) {
            auto type = captureType;
            cleanup([&] { ioctlChecked(video.get(), VIDIOC_STREAMOFF, type, "VIDIOC_STREAMOFF"); });
        }
        for (const auto &buffer : buffers)
            munmap(buffer.address, buffer.length);
        video.reset();
        if (changedLink)
            cleanup([&] { ioctlChecked(media.get(), MEDIA_IOC_SETUP_LINK, link, "Restore media link"); });
    }

    void init(const IrConfig &config, Clock::time_point deadline)
    {
        cfg = config;
        lock = openFile("/run/surface-ir-hello.cam.lock", O_CREAT | O_RDWR | O_NOFOLLOW);
        while (flock(lock.get(), LOCK_EX | LOCK_NB) < 0) {
            if (errno != EWOULDBLOCK)
                syserror("IR camera lock");
            if (interrupted)
                throw Cancelled{};
            if (Clock::now() >= deadline)
                throw std::runtime_error("IR camera is busy with another request");
            std::this_thread::sleep_for(10ms);
        }

        const auto route = findRoute();
        const auto &topology = route.topology;
        link = route.link;
        media = openFile(topology.media, O_RDWR);
        pmic = {openFile("/dev/i2c-" + std::to_string(topology.pmicBus), O_RDWR), pmicAddress, false};
        if (pmic.read(revision).front() != pmicRevision)
            throw std::runtime_error("Unexpected TPS68470 revision");
        sensor = {openFile("/dev/i2c-" + std::to_string(topology.sensorBus), O_RDWR),
                  static_cast<unsigned short>(topology.sensorAddress), true};
        sub = openFile(topology.subdevice, O_RDWR);
        gainId = supportsControl(sub.get(), V4L2_CID_GAIN) ? V4L2_CID_GAIN : V4L2_CID_ANALOGUE_GAIN;
        const auto [minExp, maxExp] = limits(sub.get(), V4L2_CID_EXPOSURE);
        const auto [minGain, maxGain] = limits(sub.get(), gainId);
        minimumExposure = std::max(1, minExp);
        maximumExposure = std::min(maxExp, std::clamp(cfg.exposureMax, 16, exposureLimit));
        minimumGain = std::max(1, minGain);
        maximumGain = static_cast<int>(std::clamp(cfg.gainMax * minimumGain, double(minimumGain), double(maxGain)));
        if (maximumExposure < minimumExposure)
            throw std::runtime_error("No safe exposure range for the IR sensor");
        controlsSaved = {{V4L2_CID_EXPOSURE, getControl(sub.get(), V4L2_CID_EXPOSURE)},
                         {gainId, getControl(sub.get(), gainId)}};

        if (!topology.linkEnabled) {
            auto enabled = link;
            enabled.flags |= MEDIA_LNK_FL_ENABLED;
            ioctlChecked(media.get(), MEDIA_IOC_SETUP_LINK, enabled, "Enable IR media link");
            changedLink = true;
        }
        const auto csi = openFile(topology.csiSubdevice, O_RDWR);
        setPadFormat(sub.get(), 0);
        setPadFormat(csi.get(), 0);
        setPadFormat(csi.get(), 1);
        video = openFile(topology.video, O_RDWR | O_NONBLOCK);
        v4l2_capability capability{};
        ioctlChecked(video.get(), VIDIOC_QUERYCAP, capability, "VIDIOC_QUERYCAP");
        const auto caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) ? capability.device_caps : capability.capabilities;
        if (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
            captureType = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        v4l2_format format{};
        format.type = captureType;
        ioctlChecked(video.get(), VIDIOC_G_FMT, format, "VIDIOC_G_FMT");
        if (multiplanar()) {
            format.fmt.pix_mp.width = width;
            format.fmt.pix_mp.height = height;
            format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_IPU3_Y10;
            format.fmt.pix_mp.field = V4L2_FIELD_NONE;
        } else {
            format.fmt.pix.width = width;
            format.fmt.pix.height = height;
            format.fmt.pix.pixelformat = V4L2_PIX_FMT_IPU3_Y10;
            format.fmt.pix.field = V4L2_FIELD_NONE;
        }
        ioctlChecked(video.get(), VIDIOC_S_FMT, format, "VIDIOC_S_FMT");
        const bool expected = multiplanar()
            ? format.fmt.pix_mp.width == width && format.fmt.pix_mp.height == height &&
                format.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_IPU3_Y10 && format.fmt.pix_mp.num_planes == 1
            : format.fmt.pix.width == width && format.fmt.pix.height == height &&
                format.fmt.pix.pixelformat == V4L2_PIX_FMT_IPU3_Y10;
        if (!expected)
            throw std::runtime_error("IR video node did not select IPU3 Y10 at 640x480");
        stride = multiplanar() ? format.fmt.pix_mp.plane_fmt[0].bytesperline : format.fmt.pix.bytesperline;
        if (stride < (width + 24) / 25 * 32)
            throw std::runtime_error("Unexpected IPU3 frame stride");

        v4l2_requestbuffers requested{};
        requested.count = 4;
        requested.type = captureType;
        requested.memory = V4L2_MEMORY_MMAP;
        ioctlChecked(video.get(), VIDIOC_REQBUFS, requested, "VIDIOC_REQBUFS");
        if (requested.count < 2)
            throw std::runtime_error("Could not allocate IR video buffers");
        buffers.reserve(requested.count);
        for (unsigned i = 0; i < requested.count; ++i) {
            v4l2_plane plane{};
            auto buffer = bufferFor(plane);
            buffer.index = i;
            ioctlChecked(video.get(), VIDIOC_QUERYBUF, buffer, "VIDIOC_QUERYBUF");
            const auto length = multiplanar() ? plane.length : buffer.length;
            const auto offset = multiplanar() ? plane.m.mem_offset : buffer.m.offset;
            auto *address = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, video.get(), offset);
            if (address == MAP_FAILED)
                syserror("mmap IR buffer");
            buffers.push_back({address, length});
            ioctlChecked(video.get(), VIDIOC_QBUF, buffer, "VIDIOC_QBUF");
        }
        auto type = captureType;
        ioctlChecked(video.get(), VIDIOC_STREAMON, type, "VIDIOC_STREAMON");
        streaming = true;
    }

    void span(int oldValue, int newValue)
    {
        const auto high = [&] { sensor.write(strobeSpan + 2, newValue >> 8); };
        const auto low = [&] { sensor.write(strobeSpan + 3, newValue & 0xff); };
        if (newValue > oldValue) {
            low();
            high();
        } else {
            high();
            low();
        }
    }
};

Camera::Camera(const IrConfig &config, Clock::time_point deadline) : impl_(std::make_unique<Impl>())
{
    impl_->init(config, deadline);
    frame(std::min(deadline, Clock::now() + 1500ms));
}
Camera::~Camera() = default;

cv::Mat Camera::frame(Clock::time_point deadline)
{
    const auto &c = *impl_;
    while (readable(c.video.get(), deadline)) {
        v4l2_plane plane{};
        auto buffer = c.bufferFor(plane);
        if (ioctl(c.video.get(), VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                if (Clock::now() >= deadline)
                    break;
                continue;
            }
            syserror("VIDIOC_DQBUF");
        }
        cv::Mat result;
        try {
            const auto &mapping = c.buffers.at(buffer.index);
            const auto bytes = c.multiplanar() ? plane.bytesused : buffer.bytesused;
            const auto offset = c.multiplanar() ? plane.data_offset : 0u;
            if (!(buffer.flags & V4L2_BUF_FLAG_ERROR) && bytes >= offset + c.stride * height &&
                mapping.length >= offset + c.stride * height)
                result = unpackIp3y(static_cast<const unsigned char *>(mapping.address) + offset, c.stride, width, height);
        } catch (...) {
            cleanup([&] { ioctlChecked(c.video.get(), VIDIOC_QBUF, buffer, "VIDIOC_QBUF"); });
            throw;
        }
        ioctlChecked(c.video.get(), VIDIOC_QBUF, buffer, "VIDIOC_QBUF");
        if (!result.empty())
            return result;
        if (Clock::now() >= deadline)
            break;
    }
    throw std::runtime_error("Timed out waiting for IPU3 IR frame");
}

void Camera::applyExposure(bool strobe)
{
    auto &c = *impl_;
    const int e = static_cast<int>(std::clamp(c.level, double(c.minimumExposure), double(c.maximumExposure)));
    const int g = static_cast<int>(std::clamp(c.minimumGain * c.level / e, double(c.minimumGain), double(c.maximumGain)));
    if (e != c.currentExposure) {
        if (!strobe) {
            setControl(c.sub.get(), V4L2_CID_EXPOSURE, e);
        } else {
            if (!c.currentExposure) {
                for (unsigned reg = strobeSpan; reg < strobeSpan + 4; ++reg)
                    c.sensor.write(reg, 0);
            }
            const int oldSpan = c.currentExposure ? *c.currentExposure + spanMargin : 0;
            if (!c.currentExposure || e > *c.currentExposure) {
                c.span(oldSpan, e + spanMargin);
                setControl(c.sub.get(), V4L2_CID_EXPOSURE, e);
            } else {
                setControl(c.sub.get(), V4L2_CID_EXPOSURE, e);
                c.span(oldSpan, e + spanMargin);
            }
        }
    }
    if (g != c.currentGain)
        setControl(c.sub.get(), c.gainId, g);
    c.currentExposure = e;
    c.currentGain = g;
}

double Camera::updateExposure(const cv::Mat &frame)
{
    auto &c = *impl_;
    const double p95 = std::max(percentile(frame, 8, 0.95), 8.0);
    if (c.aeHold > 0) {
        --c.aeHold;
        return p95;
    }
    const double top = double(c.maximumExposure) * c.maximumGain / c.minimumGain;
    const double ratio = std::sqrt(std::clamp(c.cfg.targetP95 / p95, 0.5, 2.0));
    const double next = std::clamp(c.level * ratio, double(c.minimumExposure), top);
    if (std::abs(next / c.level - 1.0) > 0.04) {
        c.level = next;
        c.aeHold = 2;
        applyExposure(true);
    }
    return p95;
}

void Camera::startEmitter(int led)
{
    auto &c = *impl_;
    if (c.pmicSaved.empty()) {
        std::vector<std::pair<unsigned, unsigned>> saved;
        for (const auto reg : savedPmic)
            saved.emplace_back(reg, c.pmic.read(reg).front());
        c.pmicSaved = std::move(saved);
    }
    if (c.sensorSaved.empty()) {
        std::vector<std::pair<unsigned, unsigned>> saved{
            {padEnable, c.sensor.read(padEnable).front()}, {strobeMask, c.sensor.read(strobeMask).front()},
        };
        const auto span = c.sensor.read(strobeSpan, 4);
        for (unsigned i = 0; i < span.size(); ++i)
            saved.emplace_back(strobeSpan + i, span[i]);
        c.sensorSaved = std::move(saved);
    }
    c.currentExposure.reset();
    applyExposure(true);
    c.sensor.write(strobeMask, 0xff);
    c.sensor.write(padEnable, c.sensorSaved.front().second | 0x08);
    const bool useLed2 = (led == 0 ? c.cfg.led : led) == 2;
    const unsigned disableOther = useLed2 ? 0x08 : 0x10;
    const auto code = static_cast<unsigned>(std::lround(std::clamp(c.cfg.current, 32.5, 520.0) / 32.5));
    c.pmic.write(control, 0);
    c.pmic.write(statusReg, 0xfe);
    c.pmic.write(timeoutReg, 0x07); // TPS68470 852 ms hardware watchdog.
    c.pmic.write(maxCurrent, code);
    c.pmic.write(current1, useLed2 ? 0 : code);
    c.pmic.write(current2, useLed2 ? code : 0);
    c.pmic.write(control, disableOther);
    c.pmic.write(control, disableOther | 0x04);
}

unsigned Camera::status() const { return impl_->pmic.read(statusReg).front(); }
}
