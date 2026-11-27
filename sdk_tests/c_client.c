#include "IR_station.h"
#include "IR_analysis.h"
#include "IR_device.h"
#include "IR_recipe.h"
#include "IR_plugin_abi.h"
#include "IR_power_driver_abi.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    const uint16_t frames[] = {20, 22};
    double mean, noise, summary[2];
    char error[512];
    const IRArrayView input = {frames, sizeof frames, IR_TYPE_U16, 1, 1, 2};
    int status = IR_FrameStatistics(&input, 1, &mean, &noise,
        1, summary, 2, error, sizeof error);
    if (status || mean != 21 || fabs(noise - sqrt(2.0)) > 1e-12) {
        fprintf(stderr, "Analysis: %s\n", error);
        return 1;
    }
    IRFrameSourceInfo source_info;
    status = IR_OpenFrames(0, "test", 0, &source_info, error, sizeof error);
    if (status != IR_UNSUPPORTED || IR_CloseFrames(error, sizeof error)) {
        fprintf(stderr, "Station: %s\n", error);
        return 1;
    }
    status = IR_OpenFrames(IR_NI_IMAQ, "img0", 0, &source_info, error, sizeof error);
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
    if (sizeof(IRCaptureStats) != 64) {
        fprintf(stderr, "Unexpected IRCaptureStats ABI size: %zu\n", sizeof(IRCaptureStats));
        return 1;
    }
    if (strcmp(IR_PLUGIN_QUERY_SYMBOL, "ir_plugin_query") != 0 ||
        IR_POWER_DRIVER_ABI_VERSION != 1u ||
        strcmp(IR_POWER_DRIVER_QUERY_SYMBOL, "ir_power_driver_query") != 0 ||
        sizeof(IRRecipeStepEvent) != 56 ||
        sizeof(IRRecipeRunOptions) != 24 ||
        sizeof(IRSlotView) != 72 ||
        sizeof(IRPrepareCallbacks) != 32 ||
        sizeof(IRHostCallbacks) != 64 ||
        sizeof(IRMetricPlugin) != 48 ||
        IR_PLUGIN_ABI_VERSION != 2u) {
        fprintf(stderr, "Unexpected recipe/plugin ABI size or symbol\n");
        return 1;
    }
    {
        IRRecipeRunOptions bad_opts = {0, 0, NULL, NULL};
        IRRecipeResult rr = {0};
        status = IR_RunRecipeJson("{}", "{}", "out", &bad_opts, &rr, error, sizeof error);
        if (status != IR_BAD_ARGUMENT || rr.failed_stage != IR_RECIPE_STAGE_VALIDATE) {
            fprintf(stderr, "RunRecipeJson bad_opts: status=%d stage=%d err=%s\n",
                status, rr.failed_stage, error);
            return 1;
        }
    }
    IRCaptureStats stats = {0};
    uint16_t dummy_px = 0;
    status = IR_SetFrameQueueCapacity(64, error, sizeof error);
    if (status != IR_OK) {
        fprintf(stderr, "SetFrameQueueCapacity: %s\n", error);
        return 1;
    }
    status = IR_GetCaptureStats(&stats, error, sizeof error);
    if (status != IR_OK || stats.queue_capacity != 64) {
        fprintf(stderr, "GetCaptureStats: status=%d cap=%d err=%s\n", status, stats.queue_capacity, error);
        return 1;
    }
    IRFrameBatchView latest = {{&dummy_px, sizeof dummy_px, IR_TYPE_U16, 0, 0, 1}, 0, NULL, 0};
    status = IR_TryReadLatestFrame(&latest, error, sizeof error);
    if (status != IR_OK || latest.pixels.frames != 0) {
        fprintf(stderr, "TryReadLatestFrame: status=%d frames=%d err=%s\n",
            status, latest.pixels.frames, error);
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
