#include "optmeas_station.h"
#include "optmeas_analysis.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    const uint16_t frames[] = {20, 22};
    double mean, noise, summary[2];
    char error[512];
    int status = OptMeas_FrameStatisticsU16(frames, 2, 2, 1, 1, 1,
        &mean, &noise, 1, summary, 2, error, sizeof error);
    if (status || mean != 21 || fabs(noise - sqrt(2.0)) > 1e-12) {
        fprintf(stderr, "Analysis: %s\n", error);
        return 1;
    }
    OptMeasGeometry g;
    status = OptMeas_OpenFrames(0, "test", 0, &g, error, sizeof error);
    if (status != OPTMEAS_UNSUPPORTED || OptMeas_CloseFrames(error, sizeof error)) {
        fprintf(stderr, "Station: %s\n", error);
        return 1;
    }
    const char *version = OptMeas_AnalysisVersion();
    puts(version);
    return argc > 1 && strcmp(argv[1], version) != 0;
}
