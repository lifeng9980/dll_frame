#include "station/acquisition/frame_source.h"
#include "api_common.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <niimaq.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

namespace ir::acquisition {
namespace {
using ir::api::Error;
using ir::api::require;

void checkImaq(Int32 status, const char *operation)
{
    if (status >= 0) return;
    Int8 detail[256]{};
    imgShowError(status, detail);
    throw Error(status == IMG_ERR_TIMEOUT ? IR_TIMEOUT : IR_IO_ERROR,
                std::string(operation) + ": " + reinterpret_cast<const char *>(detail));
}
}

class NiImaqSource final : public FrameSource {
public:
    NiImaqSource(const char *interfaceName, int validBits)
    {
        try {
            checkImaq(imgInterfaceOpen(reinterpret_cast<const Int8 *>(interfaceName), &interface_),
                      "Open NI-IMAQ interface");
            checkImaq(imgSessionOpen(interface_, &session_), "Open NI-IMAQ session");

            Int32 width{};
            Int32 height{};
            Int32 bitsPerPixel{};
            Int32 rowPixels{};
            checkImaq(imgGetAttribute(session_, IMG_ATTR_ROI_WIDTH, &width), "Read NI-IMAQ image width");
            checkImaq(imgGetAttribute(session_, IMG_ATTR_ROI_HEIGHT, &height), "Read NI-IMAQ image height");
            checkImaq(imgGetAttribute(session_, IMG_ATTR_BITSPERPIXEL, &bitsPerPixel),
                      "Read NI-IMAQ pixel depth");
            checkImaq(imgGetAttribute(session_, IMG_ATTR_ROWPIXELS, &rowPixels), "Read NI-IMAQ row stride");
            require(width > 0 && height > 0 && bitsPerPixel == 16 && rowPixels >= width,
                    IR_UNSUPPORTED, "NI-IMAQ source must provide 16-bit pixels and a valid row stride");

            geometry_ = {width, height, validBits};
            strideBytes_ = size_t(rowPixels) * sizeof(uint16_t);
            rowBytes_ = size_t(width) * sizeof(uint16_t);
        } catch (...) {
            close();
            throw;
        }
    }

    ~NiImaqSource() override { close(); }

    IRGeometry geometry() const override { return geometry_; }

    void start() override
    {
        require(!running_, IR_BUSY, "NI-IMAQ capture is already running");
        checkImaq(imgGrabSetup(session_, TRUE), "Start NI-IMAQ capture");
        running_ = true;
        applicationSequence_ = 0;
    }

    void read(uint16_t *pixels, IRFrameStamp &stamp, int timeoutMs) override
    {
        require(running_, IR_BAD_ARGUMENT, "Start NI-IMAQ capture before reading frames");
        checkImaq(imgSetAttribute2(session_, IMG_ATTR_FRAMEWAIT_MSEC, uInt32(std::max(timeoutMs, 0))),
                  "Set NI-IMAQ frame timeout");
        void *buffer = nullptr;
        checkImaq(imgGrab(session_, &buffer, TRUE), "Read NI-IMAQ frame");
        require(buffer, IR_IO_ERROR, "NI-IMAQ returned an empty frame buffer");

        auto *destination = reinterpret_cast<uint8_t *>(pixels);
        const auto *source = static_cast<const uint8_t *>(buffer);
        for (int32_t row = 0; row < geometry_.height; ++row)
            std::memcpy(destination + size_t(row) * rowBytes_,
                        source + size_t(row) * strideBytes_, rowBytes_);

        stamp = {};
        stamp.sequence = applicationSequence_++;
        stamp.fields = IR_HAS_SEQUENCE;
        Int32 frameNumber{};
        if (imgGetAttribute(session_, IMG_ATTR_FRAME_COUNT, &frameNumber) >= 0) {
            stamp.driver_sequence = uint32_t(frameNumber);
            stamp.fields |= IR_HAS_DRIVER_SEQUENCE;
        }
        Int32 lostFrames{};
        if (imgGetAttribute(session_, IMG_ATTR_LOST_FRAMES, &lostFrames) >= 0) {
            if (hasLostFrameCount_ && lostFrames > previousLostFrameCount_) {
                stamp.lost_frames = uint32_t(lostFrames - previousLostFrameCount_);
                stamp.fields |= IR_HAS_LOSS;
            }
            previousLostFrameCount_ = lostFrames;
            hasLostFrameCount_ = true;
        }
    }

    void stop() override
    {
        if (!running_) return;
        const auto status = imgSessionStopAcquisition(session_);
        running_ = false;
        checkImaq(status, "Stop NI-IMAQ capture");
    }

private:
    void close() noexcept
    {
        if (running_) {
            imgSessionStopAcquisition(session_);
            running_ = false;
        }
        if (session_) {
            imgClose(session_, TRUE);
            session_ = 0;
        }
        if (interface_) {
            imgClose(interface_, TRUE);
            interface_ = 0;
        }
    }

    INTERFACE_ID interface_{};
    SESSION_ID session_{};
    IRGeometry geometry_{};
    size_t rowBytes_{};
    size_t strideBytes_{};
    uint64_t applicationSequence_{};
    Int32 previousLostFrameCount_{};
    bool hasLostFrameCount_{};
    bool running_{};
};

std::unique_ptr<FrameSource> openNiImaq(const char *interfaceName, int validBits)
{
    return std::make_unique<NiImaqSource>(interfaceName, validBits);
}
}
