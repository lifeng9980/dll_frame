#include "IR_analysis.h"
#include <math.h>
#include <stdio.h>

int main(void)
{
    const uint16_t frames[] = {
        20, 40, 60, 80,
        22, 42, 62, 82
    };
    char error[256] = {0};
    double mean = 0.0;
    int32_t status = IR_ROIMeanU16(frames, 8, 2, 2, 2, 0, 0, 1, 2,
                                   &mean, error, sizeof error);
    if (status != IR_OK || fabs(mean - 41.0) > 1e-12) {
        fprintf(stderr, "left ROI mean: status=%d mean=%.17g error=%s\n", status, mean, error);
        return 1;
    }

    status = IR_ROIMeanU16(frames, 8, 2, 2, 2, 0, 0, 2, 2,
                           &mean, error, sizeof error);
    if (status != IR_OK || fabs(mean - 51.0) > 1e-12) {
        fprintf(stderr, "full image mean: status=%d mean=%.17g error=%s\n", status, mean, error);
        return 2;
    }

    status = IR_ROIMeanU16(frames, 8, 2, 2, 2, 1, 0, 2, 1,
                           &mean, error, sizeof error);
    if (status != IR_BAD_ARGUMENT) {
        fprintf(stderr, "outside ROI should be rejected, got status=%d\n", status);
        return 3;
    }
    return 0;
}
