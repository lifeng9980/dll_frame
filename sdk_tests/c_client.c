#include "IR_station.h"
#include "IR_analysis.h"
#include "IR_device.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    const uint16_t frames[] = {20, 22};
    double mean, noise, summary[2];
    char error[512];
    int status = IR_FrameStatisticsU16(frames, 2, 2, 1, 1, 1,
        &mean, &noise, 1, summary, 2, error, sizeof error);
    if (status || mean != 21 || fabs(noise - sqrt(2.0)) > 1e-12) {
        fprintf(stderr, "Analysis: %s\n", error);
        return 1;
    }
    IRGeometry g;
    status = IR_OpenFrames(0, "test", 0, &g, error, sizeof error);
    if (status != IR_UNSUPPORTED || IR_CloseFrames(error, sizeof error)) {
        fprintf(stderr, "Station: %s\n", error);
        return 1;
    }
    status = IR_OpenFrames(IR_NI_IMAQ, "img0", 0, &g, error, sizeof error);
    if (status != IR_UNSUPPORTED) {
        fprintf(stderr, "Reserved NI-IMAQ backend: %s\n", error);
        return 1;
    }
    IRHandle device = 1;
    status = IR_OpenDevice(0, "{}", &device, error, sizeof error);
    if (status != IR_UNSUPPORTED || device != 0) {
        fprintf(stderr, "Device: %s\n", error);
        return 1;
    }
    const char *analysis_version = IR_AnalysisVersion();
    const char *station_version = IR_StationVersion();
    if (!analysis_version || !station_version || strcmp(analysis_version, station_version) != 0) {
        fprintf(stderr, "Version mismatch: analysis=%s station=%s\n",
            analysis_version ? analysis_version : "(null)",
            station_version ? station_version : "(null)");
        return 1;
    }
    puts(station_version);
    return argc > 1 && strcmp(argv[1], station_version) != 0;
}
