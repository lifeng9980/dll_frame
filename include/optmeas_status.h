#pragma once
#include <stdint.h>

/* 公共返回码。err 可传 NULL 且 err_bytes=0 以省略错误文本。
 * 有缓冲区时 err_bytes 为字节数，错误文本为以 NUL 结束的 UTF-8。
 */
enum OptMeasStatus
{
    OPTMEAS_OK = 0,
    OPTMEAS_NULL_POINTER = 1,
    OPTMEAS_BAD_ARGUMENT = 2,
    OPTMEAS_BUFFER_TOO_SMALL = 3,
    OPTMEAS_COMPUTATION_FAILED = 4,
    OPTMEAS_INVALID_HANDLE = 5,
    OPTMEAS_IO_ERROR = 6,
    OPTMEAS_TIMEOUT = 7,
    OPTMEAS_CANCELLED = 8,
    OPTMEAS_BUSY = 9,
    OPTMEAS_UNSUPPORTED = 10,
    OPTMEAS_END_OF_STREAM = 11,
    OPTMEAS_FRAME_GAP = 12
};
