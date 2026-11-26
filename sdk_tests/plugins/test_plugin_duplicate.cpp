#include "IR_plugin_abi.h"

namespace {

int32_t prepare(const IRPrepareCallbacks *, const char *, char *, int32_t)
{
    return IR_OK;
}

int32_t execute(const IRHostCallbacks *, IRGeometry, const char *, char *, int32_t)
{
    return IR_OK;
}

const IRMetricPlugin kPlugins[]{
    {IR_PLUGIN_ABI_VERSION, "ir.test.duplicate", "plugin:test.duplicate", "frames", prepare, execute},
    {IR_PLUGIN_ABI_VERSION, "ir.test.duplicate", "plugin:test.duplicate", "frames", prepare, execute}
};

} // namespace

extern "C" IR_PLUGIN_EXPORT int32_t ir_plugin_query(
    const IRMetricPlugin **out_list, uint32_t *out_count)
{
    if (!out_list || !out_count)
        return IR_NULL_POINTER;
    *out_list = kPlugins;
    *out_count = static_cast<uint32_t>(sizeof(kPlugins) / sizeof(kPlugins[0]));
    return IR_OK;
}
