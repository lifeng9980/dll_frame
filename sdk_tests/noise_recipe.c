/* 在含 frames.csv 和 frames.raw 的目录运行。第二个参数是黑体连接 JSON。
 * 示例通过真实的 C 接口发送 32 °C 设温命令，自动测试连接模拟黑体。 */
#include "IR_device.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[1], "tcp") && strcmp(argv[1], "serial"))) {
        fprintf(stderr, "noise_recipe <tcp|serial> <connection-json>\n");
        return 1;
    }
    int kind = strcmp(argv[1], "tcp") == 0 ? IR_PCN7LT : IR_DCN1000;
    char error[512] = {0};
    IRHandle blackbody = 0;
    int status = IR_OpenDevice(kind, argv[2], &blackbody, error, sizeof error);
    if (status != IR_OK) {
        fprintf(stderr, "%s\n", error);
        return status;
    }

    IRGeometry geometry;
    IRNoiseResult result = {0};
    status = IR_OpenFrames(IR_RAW_REPLAY, ".", 0, &geometry, error, sizeof error);
    if (status == IR_OK) {
        IRNoiseRecipe recipe = {0};
        recipe.recipe_id = "two-frame-sample";
        recipe.blackbody = blackbody;
        recipe.temperature_c = 32.0;
        recipe.tolerance_c = 0.01;
        recipe.stable_window_ms = 100;
        recipe.stable_timeout_ms = 1500;
        recipe.require_device_stable = 1;
        recipe.frames = 2;
        status = IR_RunNoise(&recipe, NULL, &result, error, sizeof error);
        int close_status = IR_CloseFrames(NULL, 0);
        if (status == IR_OK) status = close_status;
    }
    int close_status = IR_CloseDevice(blackbody, NULL, 0);
    if (status == IR_OK) status = close_status;
    printf("completed=%d\n", (int)result.completed);
    if (status != IR_OK) {
        fprintf(stderr, "%s\n", error);
        return status;
    }
    printf("mean_dn=%.17g\nnoise_dn=%.17g\n", result.mean_dn, result.temporal_noise_dn);
    return 0;
}
