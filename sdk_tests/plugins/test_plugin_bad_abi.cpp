#include "IR_plugin_abi.h"

namespace {

int32_t dummyPrep(const IRPrepareCallbacks *, const char *, char *, int32_t)
{
    return IR_OK;
}

int32_t dummyExec(const IRHostCallbacks *, IRGeometry, const char *, char *, int32_t)
{
    return IR_OK;
}

const IRMetricPlugin kBadAbiPlugin[]{
    {
        999u,
        "ir.test.bad",
        "plugin:test.bad_abi",
        "frames",
        dummyPrep,
        dummyExec
    }
};

} // namespace

extern "C" IR_PLUGIN_EXPORT int32_t ir_plugin_query(
    const IRMetricPlugin **out_list, uint32_t *out_count)
{
    if (!out_list || !out_count)
        return IR_NULL_POINTER;
    *out_list = kBadAbiPlugin;
    *out_count = 1;
    return IR_OK;
}
