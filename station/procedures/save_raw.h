#pragma once
#include "IR_station.h"
#include <QString>
#include <vector>
namespace optmeas::procedures {
void checkDestination(const QString &path);
void saveNoiseRun(const QString &path, const OptMeasNoiseRecipe &recipe, int backend,
    OptMeasGeometry geometry, const std::vector<uint16_t> &frames,
    const std::vector<OptMeasFrameStamp> &stamps, const OptMeasNoiseResult &result);
}
