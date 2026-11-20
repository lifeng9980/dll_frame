#pragma once
#include "IR_station.h"
#include <QString>
#include <string>
#include <vector>
namespace ir::procedures {
void checkDestination(const QString &path);
void saveNoiseRun(const QString &path, const IRNoiseRecipe &recipe, int backend,
    const std::string &sourceName,
    IRGeometry geometry, const std::vector<uint16_t> &frames,
    const std::vector<IRFrameStamp> &stamps, const IRNoiseResult &result);
}
