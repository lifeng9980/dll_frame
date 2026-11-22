#pragma once
#include "IR_device.h"
#include "IR_station.h"
#include <filesystem>
#include <string>
#include <vector>
namespace ir::procedures {
struct SavedBatchRef {
    std::string subdir;
    IRGeometry geometry{};
    const std::vector<uint16_t> *frames{};
    const std::vector<IRFrameStamp> *stamps{};
};
void checkDestination(const std::filesystem::path &path);
void writeFrameBatchFiles(const std::filesystem::path &directory, IRGeometry geometry,
    const std::vector<uint16_t> &frames, const std::vector<IRFrameStamp> &stamps);
void saveRecipeArtifacts(const std::filesystem::path &destination,
    const std::vector<SavedBatchRef> &batches, const std::string &resultJson);
void writeTextFileAtomicOrDirect(const std::filesystem::path &filePath, const std::string &content);
void saveFrameBatch(const std::filesystem::path &path, IRGeometry geometry,
    const std::vector<uint16_t> &frames, const std::vector<IRFrameStamp> &stamps,
    const std::string &resultJson);
void saveNoiseRun(const std::filesystem::path &path, const IRNoiseRecipe &recipe, int backend,
    const std::string &sourceName,
    IRGeometry geometry, const std::vector<uint16_t> &frames,
    const std::vector<IRFrameStamp> &stamps, const IRNoiseResult &result,
    double blackbodySetpointReadback, const IRBlackbodyReading *blackbodyReading,
    const std::string &startedUtc);
}
