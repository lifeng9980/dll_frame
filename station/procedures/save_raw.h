#pragma once
#include "IR_device.h"
#include "IR_station.h"
#include <filesystem>
#include <string>
#include <vector>
namespace ir::procedures {
void checkDestination(const std::filesystem::path &path);
void saveNoiseRun(const std::filesystem::path &path, const IRNoiseRecipe &recipe, int backend,
    const std::string &sourceName,
    IRGeometry geometry, const std::vector<uint16_t> &frames,
    const std::vector<IRFrameStamp> &stamps, const IRNoiseResult &result,
    double blackbodySetpointReadback, const IRBlackbodyReading *blackbodyReading);
}
