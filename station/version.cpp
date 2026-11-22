#include "IR_station.h"
#include "station/version.h"
#include "ir_build_info.h"

#ifndef IR_STATION_VERSION
#define IR_STATION_VERSION "0.0.0"
#endif

#ifndef IR_SDK_SOURCE_COMMIT
#define IR_SDK_SOURCE_COMMIT "unknown"
#endif

extern "C" const char *IR_StationVersion(void)
{
    return IR_STATION_VERSION;
}

namespace ir::station {
const char *sourceCommit() noexcept
{
    return IR_SDK_SOURCE_COMMIT;
}
}
