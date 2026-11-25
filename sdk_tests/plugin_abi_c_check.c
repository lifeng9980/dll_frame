#include "IR_plugin_abi.h"
#include "IR_recipe.h"
#include "IR_status.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    if (IR_PLUGIN_ABI_VERSION_1 != 1u) {
        fprintf(stderr, "Unexpected IR_PLUGIN_ABI_VERSION_1: %u\n", IR_PLUGIN_ABI_VERSION_1);
        return 1;
    }
    if (strcmp(IR_PLUGIN_QUERY_V1_SYMBOL, "ir_plugin_query_v1") != 0) {
        fprintf(stderr, "Unexpected query symbol: %s\n", IR_PLUGIN_QUERY_V1_SYMBOL);
        return 1;
    }
    if (sizeof(IRRecipeStepEvent) != 56 ||
        sizeof(IRRecipeRunOptions) != 24 ||
        sizeof(IRPluginHeader) != 32 ||
        sizeof(IRSlotView) != 48 ||
        sizeof(IRPrepareCallbacksV1) != 40 ||
        sizeof(IRHostCallbacksV1) != 72 ||
        sizeof(IRMetricPluginV1) != 80) {
        fprintf(stderr, "Unexpected ABI struct size: step=%zu opts=%zu hdr=%zu slot=%zu prep=%zu host=%zu desc=%zu\n",
                sizeof(IRRecipeStepEvent), sizeof(IRRecipeRunOptions),
                sizeof(IRPluginHeader), sizeof(IRSlotView),
                sizeof(IRPrepareCallbacksV1), sizeof(IRHostCallbacksV1), sizeof(IRMetricPluginV1));
        return 1;
    }

    IRPluginHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.struct_size = (uint32_t)sizeof(IRPluginHeader);
    hdr.abi_version = IR_PLUGIN_ABI_VERSION_1;

    IRSlotView view;
    memset(&view, 0, sizeof(view));
    view.struct_size = (uint32_t)sizeof(IRSlotView);
    view.slot_kind = IR_PLUGIN_SLOT_BATCH;

    IRMetricPluginV1 desc;
    memset(&desc, 0, sizeof(desc));
    desc.header = hdr;
    desc.method_name = "plugin:test.c_check";
    desc.method_version = 1;

    if (desc.header.abi_version != IR_PLUGIN_ABI_VERSION_1 || view.slot_kind != IR_PLUGIN_SLOT_BATCH) {
        return 1;
    }
    return 0;
}
