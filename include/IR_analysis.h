#pragma once
#include "IR_status.h"

#if defined(_WIN32) && defined(IR_ANALYSIS_BUILD)
#define IR_ANALYSIS_API __declspec(dllexport)
#elif defined(_WIN32)
#define IR_ANALYSIS_API __declspec(dllimport)
#elif defined(IR_ANALYSIS_BUILD) && defined(__GNUC__)
#define IR_ANALYSIS_API __attribute__((visibility("default")))
#else
#define IR_ANALYSIS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif
/* 版本字符串为 UTF-8，由库持有，库卸载前有效。 */
IR_ANALYSIS_API const char *IR_AnalysisVersion(void);

/* 输入帧在调用期间有效，按 [帧][行][列] 紧密排列，容量按元素计。
 * sample_std=1 计算样本标准差（帧数至少 2），0 计算总体标准差（帧数至少 1）。
 * mean_map、std_map 各需 rows*cols 个 double，summary 需 2 个 double。
 * 输出单位均为 DN，summary 依次是全图均值、各像元时域标准差的全图均值。
 * 输入和输出缓冲区由调用方提供且不重叠，失败时输出无效。
 * 此调用同步等待计算完成，可在应用的工作线程执行。错误文本的约定见 IR_status.h。
 */
IR_ANALYSIS_API int32_t IR_FrameStatisticsU16(const uint16_t *frames, int64_t frame_elements,
    int32_t frame_count, int32_t rows, int32_t cols, int32_t sample_std, double *mean_map,
    double *std_map, int64_t pixel_capacity, double *summary, int32_t summary_count,
    char *err, int32_t err_bytes);
#ifdef __cplusplus
}
#endif
