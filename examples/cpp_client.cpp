#include "IR_analysis.h"
#include "IR_station.h"
#include <cmath>
#include <cstdio>

int main()
{
    const uint16_t pixels[]{20, 22};
    double means[1]{};
    double deviations[1]{};
    double summary[2]{};
    char error[512]{};
    const auto status = IR_FrameStatisticsU16(pixels, 2, 2, 1, 1, 1,
        means, deviations, 1, summary, 2, error, sizeof error);
    if (status != IR_OK) {
        std::fprintf(stderr, "IR_FrameStatisticsU16: %s\n", error);
        return status;
    }
    std::printf("analysis_version=%s\nmean_dn=%.17g\ntemporal_noise_dn=%.17g\n",
                IR_AnalysisVersion(), summary[0], summary[1]);
    return std::abs(summary[0] - 21.0) < 1e-12 &&
           std::abs(summary[1] - std::sqrt(2.0)) < 1e-12 ? 0 : 1;
}
