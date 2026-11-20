#include "station/acquisition/frame_source.h"
#include "api_common.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace ir::acquisition {
namespace {
using ir::api::Error;
using ir::api::require;

struct Mapping {
    void *address{MAP_FAILED};
    size_t length{};
};

[[noreturn]] void ioError(const char *operation)
{
    throw Error(IR_IO_ERROR, std::string(operation) + ": " + std::strerror(errno));
}

template<class Argument>
void ioctlCall(int fd, unsigned long request, Argument *argument, const char *operation)
{
    int status;
    do {
        status = ioctl(fd, request, argument);
    } while (status < 0 && errno == EINTR);
    if (status < 0) ioError(operation);
}
}

class V4l2Source final : public FrameSource {
public:
    V4l2Source(const char *path, int validBits)
    {
        try {
            fd_ = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (fd_ < 0) ioError("Open V4L2 device");

        v4l2_capability capabilities{};
        ioctlCall(fd_, VIDIOC_QUERYCAP, &capabilities, "Query V4L2 capabilities");
        const auto deviceCapabilities = capabilities.capabilities & V4L2_CAP_DEVICE_CAPS
            ? capabilities.device_caps : capabilities.capabilities;
        require(deviceCapabilities & V4L2_CAP_VIDEO_CAPTURE, IR_UNSUPPORTED,
                "V4L2 node is not a video capture device");
        require(deviceCapabilities & V4L2_CAP_STREAMING, IR_UNSUPPORTED,
                "V4L2 device does not support streaming capture");

        v4l2_format format{};
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctlCall(fd_, VIDIOC_G_FMT, &format, "Read V4L2 capture format");
        const auto &pixels = format.fmt.pix;
        require(pixels.pixelformat == V4L2_PIX_FMT_YUYV, IR_UNSUPPORTED,
                "FX3 V4L2 capture must expose its 16-bit YUYV transport format");
        require(pixels.width > 0 && pixels.height > 0 && pixels.field == V4L2_FIELD_NONE,
                IR_UNSUPPORTED, "FX3 V4L2 capture has unsupported geometry or field mode");

        const auto rowBytes = uint64_t(pixels.width) * sizeof(uint16_t);
        require(pixels.bytesperline >= rowBytes &&
                pixels.sizeimage >= uint64_t(pixels.bytesperline) * pixels.height,
                IR_IO_ERROR, "V4L2 frame stride or image size is smaller than the image");

            geometry_ = {int32_t(pixels.width), int32_t(pixels.height), validBits};
            stride_ = pixels.bytesperline;
        } catch (...) {
            if (fd_ >= 0) close(fd_);
            fd_ = -1;
            throw;
        }
    }

    ~V4l2Source() override
    {
        releaseStream();
        if (fd_ >= 0) close(fd_);
    }

    IRGeometry geometry() const override { return geometry_; }

    void start() override
    {
        require(!streaming_, IR_BUSY, "V4L2 capture is already streaming");

        v4l2_requestbuffers request{};
        request.count = BufferCount;
        request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        request.memory = V4L2_MEMORY_MMAP;
        ioctlCall(fd_, VIDIOC_REQBUFS, &request, "Allocate V4L2 capture buffers");
        require(request.count >= 2, IR_IO_ERROR, "V4L2 returned fewer than two capture buffers");

        try {
            mappings_.resize(request.count);
            for (uint32_t index = 0; index < request.count; ++index) {
                v4l2_buffer buffer{};
                buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buffer.memory = V4L2_MEMORY_MMAP;
                buffer.index = index;
                ioctlCall(fd_, VIDIOC_QUERYBUF, &buffer, "Query V4L2 capture buffer");

                auto &mapping = mappings_[index];
                mapping.length = buffer.length;
                mapping.address = mmap(nullptr, mapping.length, PROT_READ | PROT_WRITE,
                                       MAP_SHARED, fd_, buffer.m.offset);
                if (mapping.address == MAP_FAILED) ioError("Map V4L2 capture buffer");
                ioctlCall(fd_, VIDIOC_QBUF, &buffer, "Queue V4L2 capture buffer");
            }

            auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctlCall(fd_, VIDIOC_STREAMON, &type, "Start V4L2 capture");
            streaming_ = true;
            hasPreviousFrame_ = false;
            applicationSequence_ = 0;
        } catch (...) {
            releaseBuffers();
            throw;
        }
    }

    void read(uint16_t *pixels, IRFrameStamp &stamp, int timeoutMs) override
    {
        require(streaming_, IR_BAD_ARGUMENT, "Start V4L2 capture before reading frames");
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs);

        pollfd descriptor{fd_, POLLIN, 0};
        bool receivedBuffer = false;
        for (;;) {
            if (receivedBuffer && std::chrono::steady_clock::now() >= deadline)
                throw Error(IR_TIMEOUT, "V4L2 frame wait timed out");
            descriptor.revents = 0;
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            const int waitMs = int(std::clamp<int64_t>(remaining, 0, INT32_MAX));
            const int status = poll(&descriptor, 1, waitMs);
            if (status == 0) throw Error(IR_TIMEOUT, "V4L2 frame wait timed out");
            if (status < 0 && errno == EINTR) continue;
            if (status < 0) ioError("Wait for V4L2 frame");
            require(!(descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)), IR_IO_ERROR,
                    "V4L2 capture device stopped responding");

            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            ioctlCall(fd_, VIDIOC_DQBUF, &buffer, "Dequeue V4L2 capture buffer");
            receivedBuffer = true;
            require(buffer.index < mappings_.size(), IR_IO_ERROR,
                    "V4L2 returned an invalid capture buffer");

            const auto rowBytes = size_t(geometry_.width) * sizeof(uint16_t);
            const auto frameBytes = stride_ * size_t(geometry_.height);
            const auto bytesUsed = buffer.bytesused;
            if (bytesUsed < frameBytes || (buffer.flags & V4L2_BUF_FLAG_ERROR)) {
                /* FX3 may emit incomplete buffers until the detector stream is enabled. */
                ioctlCall(fd_, VIDIOC_QBUF, &buffer, "Requeue incomplete V4L2 capture buffer");
                continue;
            }
            const auto *source = static_cast<const uint8_t *>(mappings_[buffer.index].address);
            auto *destination = reinterpret_cast<uint8_t *>(pixels);
            for (int32_t row = 0; row < geometry_.height; ++row)
                std::memcpy(destination + size_t(row) * rowBytes,
                            source + size_t(row) * stride_, rowBytes);

            stamp = {};
            stamp.sequence = applicationSequence_++;
            stamp.driver_sequence = buffer.sequence;
            stamp.fields = IR_HAS_SEQUENCE | IR_HAS_DRIVER_SEQUENCE | IR_HAS_LOSS;
            if (hasPreviousFrame_) {
                const auto difference = buffer.sequence - previousDriverSequence_;
                if (difference > 1) stamp.lost_frames = difference - 1;
            }
            previousDriverSequence_ = buffer.sequence;
            hasPreviousFrame_ = true;

            if ((buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC &&
                buffer.timestamp.tv_sec >= 0 && buffer.timestamp.tv_usec >= 0) {
                stamp.timestamp_ns = int64_t(buffer.timestamp.tv_sec) * 1000000000LL +
                                     int64_t(buffer.timestamp.tv_usec) * 1000LL;
                stamp.time_origin = IR_TIME_HOST;
            }

            ioctlCall(fd_, VIDIOC_QBUF, &buffer, "Requeue V4L2 capture buffer");
            return;
        }
    }

    void stop() override
    {
        if (!streaming_) return;

        auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        int status;
        do {
            status = ioctl(fd_, VIDIOC_STREAMOFF, &type);
        } while (status < 0 && errno == EINTR);
        const int streamError = status < 0 ? errno : 0;
        streaming_ = false;
        releaseBuffers();
        if (streamError) {
            errno = streamError;
            ioError("Stop V4L2 capture");
        }
    }

private:
    static constexpr uint32_t BufferCount = 4;

    void releaseBuffers() noexcept
    {
        for (const auto &mapping : mappings_)
            if (mapping.address != MAP_FAILED) munmap(mapping.address, mapping.length);
        mappings_.clear();

        if (fd_ >= 0) {
            v4l2_requestbuffers request{};
            request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            request.memory = V4L2_MEMORY_MMAP;
            ioctl(fd_, VIDIOC_REQBUFS, &request);
        }
    }

    void releaseStream() noexcept
    {
        if (fd_ < 0) return;
        if (streaming_) {
            auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(fd_, VIDIOC_STREAMOFF, &type);
            streaming_ = false;
        }
        releaseBuffers();
    }

    int fd_{-1};
    IRGeometry geometry_{};
    size_t stride_{};
    std::vector<Mapping> mappings_;
    uint32_t previousDriverSequence_{};
    uint64_t applicationSequence_{};
    bool hasPreviousFrame_{};
    bool streaming_{};
};

std::unique_ptr<FrameSource> openV4l2(const char *devicePath, int validBits)
{
    return std::make_unique<V4l2Source>(devicePath, validBits);
}
}
