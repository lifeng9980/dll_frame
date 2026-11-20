#include "IR_analysis.h"
#include "api_common.h"
#include <vector>

using namespace ir::api;
extern "C" const char *IR_AnalysisVersion(void)
{
    return IR_ANALYSIS_VERSION;
}

// Welford 更新逐像元均值和平方差，避免高直流背景下的减法消差。
extern "C" int32_t IR_FrameStatisticsU16(const uint16_t *frames, int64_t elements, int32_t t, int32_t rows,
                                              int32_t cols, int32_t sample, double *means, double *stds,
                                              int64_t capacity, double *summary, int32_t sn, char *err, int32_t en)
{
    return guarded(err, en, [&] {
        const auto n = pixels(rows, cols);
        needed(frames, elements, stackSize(n, t));
        require((sample == 0 || sample == 1) && t > sample, IR_BAD_ARGUMENT,
                "Invalid standard deviation convention or frame count");
        needed(means, capacity, n);
        needed(stds, capacity, n);
        needed(summary, sn, 2);
        std::vector<double> m(size_t(n), 0), m2(size_t(n), 0);
        for (int f = 0; f < t; ++f)
        {
            for (int64_t p = 0; p < n; ++p)
            {
                const double delta = frames[int64_t(f) * n + p] - m[p];
                m[p] += delta / (f + 1);
                m2[p] += delta * (frames[int64_t(f) * n + p] - m[p]);
            }
        }
        double average = 0, averageStd = 0;
        for (int64_t p = 0; p < n; ++p)
        {
            m2[p] = std::sqrt(std::max(0.0, m2[p] / (t - sample)));
            average += m[p] / n;
            averageStd += m2[p] / n;
        }
        std::copy(m.begin(), m.end(), means);
        std::copy(m2.begin(), m2.end(), stds);
        summary[0] = average;
        summary[1] = averageStd;
    });
}
