#include "recipe_config.h"
#include "IR_analysis.h"
#include "api_common.h"
#include <algorithm>
#include <iterator>

namespace ir::procedures::recipe {
using namespace ir::api;
namespace {
struct MetricMethod {
    const char *name;
    std::span<const MetricOutput> outputs;
    MetricOperations (*prepare)(const Json &, const std::string &, int32_t);
};

MetricOperations prepareNoise(const Json &fields, const std::string &path, int32_t frames)
{
    allowFields(fields, {"id", "type", "method", "name", "input"}, path);
    require(frames >= 2, IR_BAD_ARGUMENT, path + ": temporal_noise requires at least 2 frames");
    return {{}, [](const FrameBatch &batch) {
        const auto count = pixels(batch.geometry.height, batch.geometry.width);
        std::vector<double> means(static_cast<size_t>(count));
        std::vector<double> deviations(static_cast<size_t>(count));
        double summary[2]{};
        char error[1024]{};
        checkStatus(IR_FrameStatisticsU16(batch.pixels.data(), int64_t(batch.pixels.size()),
            int32_t(batch.stamps.size()), batch.geometry.height, batch.geometry.width, 1,
            means.data(), deviations.data(), count, summary, 2, error, sizeof error), error);
        return std::vector<double>{summary[0], summary[1]};
    }};
}

MetricOperations prepareRoiMean(const Json &fields, const std::string &path, int32_t)
{
    allowFields(fields, {"id", "type", "method", "name", "input", "x", "y", "width", "height"}, path);
    const auto x = integerField(fields, "x", 0, INT32_MAX, path);
    const auto y = integerField(fields, "y", 0, INT32_MAX, path);
    const auto width = integerField(fields, "width", 1, INT32_MAX, path);
    const auto height = integerField(fields, "height", 1, INT32_MAX, path);
    return {
        [=](IRGeometry geometry) {
            require(x <= geometry.width - width && y <= geometry.height - height,
                    IR_BAD_ARGUMENT, "ROI is outside the image geometry");
        },
        [=](const FrameBatch &batch) {
            double mean{};
            char error[1024]{};
            checkStatus(IR_ROIMeanU16(batch.pixels.data(), int64_t(batch.pixels.size()),
                int32_t(batch.stamps.size()), batch.geometry.height, batch.geometry.width,
                x, y, width, height, &mean, error, sizeof error), error);
            return std::vector<double>{mean};
        }
    };
}

// 新增配方指标时，在此声明输出和版本，并提供一个解析参数的计算入口。
constexpr MetricOutput noiseOutputs[]{
    {".mean_dn", "DN", "frame_mean", 1},
    {".temporal_noise_dn", "DN", "mean_pixel_sample_std", 1}
};
constexpr MetricOutput roiOutputs[]{{".mean_dn", "DN", "roi_mean", 1}};
constexpr MetricMethod methods[]{
    {"temporal_noise", noiseOutputs, prepareNoise},
    {"roi_mean", roiOutputs, prepareRoiMean}
};
}

AnalysisStep prepareAnalysis(StepRef ref, const Json &fields, const std::string &path, int32_t frames)
{
    const auto method = stringField(fields, "method", path);
    const auto found = std::find_if(std::begin(methods), std::end(methods),
        [&](const MetricMethod &candidate) { return method == candidate.name; });
    require(found != std::end(methods), IR_BAD_ARGUMENT, path + ".method is unsupported: " + method);
    return {std::move(ref), stringField(fields, "name", path), found->outputs,
            found->prepare(fields, path, frames)};
}
}
