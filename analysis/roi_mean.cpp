#include "IR_analysis.h"
#include "api_common.h"
#include <cstdint>

using namespace ir::api;

extern "C" int32_t IR_ROIMeanU16(const uint16_t *frames, int64_t elements,
    int32_t frameCount, int32_t rows, int32_t cols, int32_t x, int32_t y,
    int32_t roiWidth, int32_t roiHeight, double *meanDn, char *err, int32_t errBytes)
{
    if (meanDn) *meanDn = 0.0;
    return guarded(err, errBytes, [&] {
        const auto pixelCount = pixels(rows, cols);
        require(frameCount > 0, IR_BAD_ARGUMENT, "At least one frame is required");
        needed(frames, elements, stackSize(pixelCount, frameCount));
        needed(meanDn, 1, 1);
        require(x >= 0 && y >= 0 && roiWidth > 0 && roiHeight > 0 &&
                x <= cols - roiWidth && y <= rows - roiHeight,
                IR_BAD_ARGUMENT, "ROI is outside the image geometry");

        long double sum = 0.0L;
        for (int32_t frame = 0; frame < frameCount; ++frame) {
            const auto frameOffset = int64_t(frame) * pixelCount;
            for (int32_t row = y; row < y + roiHeight; ++row) {
                const auto rowOffset = frameOffset + int64_t(row) * cols + x;
                for (int32_t col = 0; col < roiWidth; ++col)
                    sum += frames[rowOffset + col];
            }
        }
        const auto samples = int64_t(frameCount) * roiWidth * roiHeight;
        *meanDn = static_cast<double>(sum / samples);
    });
}
