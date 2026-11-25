#include "IR_plugin_abi.h"

namespace {

int32_t dummyPrep(const IRPrepareCallbacksV1 *, const char *, char *, int32_t)
{
    return IR_OK;
}

int32_t dummyExec(const IRHostCallbacksV1 *, const char *, char *, int32_t)
{
    return IR_OK;
}

const IRMetricPluginV1 kBadAbiPlugin[]{
    {
        {sizeof(IRPluginHeader), 999u, "ir.test.bad", "0.0.1", "SDK Tests"},
        "plugin:test.bad_abi",
        1,
        0,
        "frames",
        dummyPrep,
        nullptr,
        dummyExec
    }
};

} // namespace

extern "C" IR_PLUGIN_EXPORT int32_t ir_plugin_query_v1(
    const IRMetricPluginV1 **out_list, uint32_t *out_count)
{
    if (!out_list || !out_count)
        return IR_NULL_POINTER;
    *out_list = kBadAbiPlugin;
    *out_count = 1;
    return IR_OK;
}
