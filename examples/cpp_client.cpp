#include "IR_analysis.h"
#include "IR_station.h"
#include <cmath>
#include <cstdio>
#include <cstring>

int main()
{
    const uint16_t pixels[]{20, 22};
    double means[1]{};
    double deviations[1]{};
    double summary[2]{};
    char error[512]{};
    const IRArrayView input{pixels, sizeof pixels, IR_TYPE_U16, 1, 1, 2};
    const auto status = IR_FrameStatistics(&input, 1, means, deviations,
        1, summary, 2, error, sizeof error);
    if (status != IR_OK) {
        std::fprintf(stderr, "IR_FrameStatistics: %s\n", error);
        return status;
    }
    const char *stationVersion = IR_StationVersion();
    const char *analysisVersion = IR_AnalysisVersion();
    std::printf("station_version=%s\nanalysis_version=%s\nmean_dn=%.17g\ntemporal_noise_dn=%.17g\n",
                stationVersion ? stationVersion : "(null)",
                analysisVersion ? analysisVersion : "(null)",
                summary[0], summary[1]);
    return stationVersion && analysisVersion &&
           std::strcmp(stationVersion, analysisVersion) == 0 &&
           std::abs(summary[0] - 21.0) < 1e-12 &&
           std::abs(summary[1] - std::sqrt(2.0)) < 1e-12 ? 0 : 1;
}
