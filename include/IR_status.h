#pragma once
#include <stdint.h>

/* IR SDK 公共返回码。err 可传 NULL 且 err_bytes=0 以省略错误文本。
 * 有缓冲区时 err_bytes 为字节数，错误文本为以 NUL 结束的 UTF-8。
 */
enum IRStatus
{
    IR_OK = 0,
    IR_NULL_POINTER = 1,
    IR_BAD_ARGUMENT = 2,
    IR_BUFFER_TOO_SMALL = 3,
    IR_COMPUTATION_FAILED = 4,
    IR_INVALID_HANDLE = 5,
    IR_IO_ERROR = 6,
    IR_TIMEOUT = 7,
    IR_CANCELLED = 8,
    IR_BUSY = 9,
    IR_UNSUPPORTED = 10,
    IR_END_OF_STREAM = 11,
    IR_FRAME_GAP = 12
};
