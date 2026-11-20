#include "IR_analysis.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>

typedef struct ConditionFrames {
    double integration_us;
    const uint16_t *frames;
    int64_t frame_elements;
    int32_t frame_count;
    int32_t rows;
    int32_t cols;
} ConditionFrames;

typedef struct ConditionResult {
    double integration_us;
    double mean_dn;
    double temporal_noise_dn;
} ConditionResult;

/* 示例刻意只使用单像元数据，重点是把多个条件组交给同一个分析入口。 */
static int32_t analyzeConditions(const ConditionFrames *conditions, int32_t condition_count,
                                 ConditionResult *results, char *error, int32_t error_bytes)
{
    for (int32_t index = 0; index < condition_count; ++index) {
        const ConditionFrames *condition = &conditions[index];
        if (condition->rows != 1 || condition->cols != 1) {
            snprintf(error, (size_t)error_bytes, "This sample expects one pixel per frame");
            return IR_BAD_ARGUMENT;
        }

        double mean_map[1] = {0};
        double noise_map[1] = {0};
        double summary[2] = {0};
        const int32_t status = IR_FrameStatisticsU16(
            condition->frames, condition->frame_elements, condition->frame_count,
            condition->rows, condition->cols, 1, mean_map, noise_map, 1,
            summary, 2, error, error_bytes);
        if (status != IR_OK) return status;

        results[index].integration_us = condition->integration_us;
        results[index].mean_dn = summary[0];
        results[index].temporal_noise_dn = summary[1];
    }
    return IR_OK;
}

int main(void)
{
    const uint16_t short_exposure_frames[] = {20, 22};
    const uint16_t long_exposure_frames[] = {40, 42};
    const ConditionFrames conditions[] = {
        {1000.0, short_exposure_frames, 2, 2, 1, 1},
        {2000.0, long_exposure_frames, 2, 2, 1, 1}
    };
    ConditionResult results[2] = {0};
    char error[256] = {0};

    const int32_t status = analyzeConditions(conditions, 2, results, error, sizeof error);
    if (status != IR_OK) {
        fprintf(stderr, "Analysis failed: %s\n", error);
        return status;
    }
    if (results[0].integration_us != 1000.0 || results[1].integration_us != 2000.0 ||
        results[0].mean_dn != 21.0 || results[1].mean_dn != 41.0 ||
        fabs(results[0].temporal_noise_dn - sqrt(2.0)) > 1e-12 ||
        fabs(results[1].temporal_noise_dn - sqrt(2.0)) > 1e-12) {
        fprintf(stderr, "Unexpected per-condition mean or temporal noise\n");
        return 1;
    }

    for (int32_t index = 0; index < 2; ++index) {
        printf("integration_us=%.0f mean_dn=%.0f noise_dn=%.15g\n",
               results[index].integration_us, results[index].mean_dn,
               results[index].temporal_noise_dn);
    }
    return 0;
}
