#include "recipe_config.h"
#include "IR_analysis.h"
#include "IR_device.h"
#include "api_common.h"
#include "save_raw.h"
#include "station/acquisition/frame_source.h"
#include "station/instruments/device_lease.h"
#include "station/version.h"
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace ir::procedures::recipe {
using namespace ir::api;
namespace fs = std::filesystem;

namespace {

fs::path pathFromUtf8(const std::string &utf8)
{
    return fs::path(reinterpret_cast<const char8_t *>(utf8.c_str()));
}

void validateRelativeSingleName(const std::string &value, const std::string &path)
{
    require(!value.empty() && value != "." && value != "..", IR_BAD_ARGUMENT,
            path + " must be a valid single path segment");
    for (unsigned char ch : value) {
        require(ch != '\0' && ch != '/' && ch != '\\' && ch != ':', IR_BAD_ARGUMENT,
                path + " cannot contain path separators or drive markers");
    }
    require(value.find("..") == std::string::npos, IR_BAD_ARGUMENT,
            path + " cannot contain '..'");
}

const char *backendName(int backend)
{
    switch (backend) {
    case IR_RAW_REPLAY: return "raw_replay";
    case IR_DIRECTSHOW: return "directshow";
    case IR_V4L2: return "v4l2";
    default: return "unknown";
    }
}

Json powerJson(const IRPowerReading &reading, int channel)
{
    return {
        {"channel", channel},
        {"setpoint_v", reading.setpoint_v},
        {"current_limit_a", reading.current_limit_a},
        {"measured_v", reading.measured_v},
        {"measured_a", reading.measured_a},
        {"output_enabled", reading.output_enabled != 0}
    };
}

void registerBatchSlot(const std::string &slot, std::map<std::string, SlotKind> &knownSlots,
                       const std::string &path)
{
    const auto [it, inserted] = knownSlots.emplace(slot, SlotKind::batch);
    if (!inserted) {
        require(it->second == SlotKind::batch, IR_BAD_ARGUMENT,
                path + " cannot overwrite non-batch slot '" + slot + "'");
    }
}

template <class VariantType>
VariantType parseSharedControlOrCaptureOrFlushOrAnalyze(
    StepRef ref, const std::string &type, const Json &fields, const std::string &path,
    const StationConfig &station, std::map<std::string, SlotKind> &knownSlots,
    std::set<std::string> &usedDevices, std::set<std::string> &analysisNames,
    std::map<std::string, std::string> &metricUnits,
    std::vector<std::string> *producedMetricsOrder)
{
    if (type == "blackbody_stabilize") {
        auto step = parseBlackbodyStep(std::move(ref), fields, path, station);
        usedDevices.insert(step.device);
        return step;
    }
    if (type == "power_set" || type == "power_output" || type == "power_read") {
        auto step = parsePowerStep(std::move(ref), type, fields, path, station);
        usedDevices.insert(step.device);
        return step;
    }
    if (type == "flush_frames") {
        allowFields(fields, {"id", "type", "discard_frames", "timeout_ms"}, path);
        return FlushStep{
            std::move(ref),
            optionalInteger(fields, "discard_frames", 1, 0, 10000, path),
            optionalInteger(fields, "timeout_ms", 2000, 0, 86400000, path)
        };
    }
    if (type == "capture") {
        allowFields(fields, {"id", "type", "frames", "frame_timeout_ms", "output"}, path);
        std::string outputSlot = fields.contains("output")
            ? stringField(fields, "output", path)
            : ref.id;
        registerBatchSlot(outputSlot, knownSlots, path + ".output");
        return CaptureStepV2{
            std::move(ref),
            integerField(fields, "frames", 1, INT32_MAX, path),
            integerField(fields, "frame_timeout_ms", 0, 86400000, path),
            std::move(outputSlot)
        };
    }
    if (type == "analyze") {
        auto step = prepareAnalysisV2(std::move(ref), fields, path, knownSlots);
        require(analysisNames.insert(step.name).second, IR_BAD_ARGUMENT,
                path + ".name is duplicated: " + step.name);
        for (const auto &out : step.outputs) {
            const std::string fullMetric = step.name + out.suffix;
            require(metricUnits.emplace(fullMetric, out.unit).second, IR_BAD_ARGUMENT,
                    path + " produces a duplicate metric name: " + fullMetric);
            if (producedMetricsOrder)
                producedMetricsOrder->push_back(fullMetric);
        }
        return step;
    }
    throw Error(IR_BAD_ARGUMENT, path + ".type is unsupported: " + type);
}

struct V2Progress {
    IRRecipeResult &result;
    const StepRef *step{};

    void enter(IRRecipeStage stage, const StepRef *ref = nullptr) noexcept
    {
        step = ref;
        result.failed_stage = stage;
        result.failed_step_index = ref ? ref->index : -1;
    }
};

struct V2CleanupFailure {
    int32_t status{IR_OK};
    const StepRef *step{};
    char detail[1024]{};

    template <class Action>
    void attempt(const StepRef *ref, Action action) noexcept
    {
        char message[1024]{};
        const auto code = guarded(message, sizeof message, action);
        if (code != IR_OK && status == IR_OK) {
            status = code;
            step = ref;
            copyText(detail, sizeof detail, message);
        }
    }
};

struct V2Readbacks {
    Json conditions = Json::array();
    Json instruments = Json::array();
};

struct SavedBatchSnapshot {
    std::string inputSlot;
    std::string subdir;
    FrameBatch batch;
};

void runControlBlackbody(const ir::acquisition::Capture &capture, IRHandle handle,
                         const BlackbodyStep &step, V2Readbacks &readbacks)
{
    const auto &target = step.target;
    char error[1024]{};
    double setpoint{};
    checkStatus(IR_SetBlackbody(handle, target.temperatureC, &setpoint, error, sizeof error), error);
    const auto reading = waitForBlackbody(capture, handle, target);
    readbacks.conditions.push_back({
        {"step", step.ref.id},
        {"device", step.device},
        {"target_c", target.temperatureC},
        {"setpoint_readback_c", setpoint},
        {"actual_c", reading.actual_c},
        {"device_stable", reading.stable != 0},
        {"tolerance_c", target.toleranceC},
        {"stable_window_ms", target.stableWindowMs},
        {"timeout_ms", target.timeoutMs}
    });
}

void runControlPower(IRHandle handle, const PowerStep &step, V2Readbacks &readbacks,
                     std::vector<const PowerStep *> &powerToSwitchOff)
{
    using Operation = PowerStep::Operation;
    char error[1024]{};
    IRPowerReading reading{};
    const char *action = "read";
    switch (step.operation) {
    case Operation::set:
        action = "set";
        checkStatus(IR_SetPowerChannel(handle, step.channel, step.voltageV, step.currentLimitA,
                                       &reading, error, sizeof error), error);
        break;
    case Operation::output:
        action = "output";
        if (step.offAfterRun && std::none_of(powerToSwitchOff.begin(), powerToSwitchOff.end(),
            [&](const PowerStep *pending) {
                return pending->device == step.device && pending->channel == step.channel;
            })) {
            powerToSwitchOff.push_back(&step);
        }
        checkStatus(IR_SetPowerOutput(handle, step.channel, step.enabled ? 1 : 0,
                                      &reading, error, sizeof error), error);
        break;
    case Operation::read:
        checkStatus(IR_ReadPower(handle, step.channel, &reading, error, sizeof error), error);
        break;
    }
    readbacks.instruments.push_back({
        {"step", step.ref.id},
        {"device", step.device},
        {"operation", action},
        {"reading", powerJson(reading, step.channel)}
    });
}

void validateInnerGeometry(const InnerStepV2 &step, IRGeometry geom, int64_t maxBatchElements)
{
    if (const auto *cap = std::get_if<CaptureStepV2>(&step)) {
        const auto elements = stackSize(pixels(geom.height, geom.width), cap->frames);
        require(elements <= maxBatchElements, IR_BAD_ARGUMENT,
                "Frame batch exceeds station.acquisition.max_batch_bytes");
    } else if (const auto *an = std::get_if<AnalysisStepV2>(&step)) {
        if (an->operations.checkGeometry)
            an->operations.checkGeometry(geom);
    }
}

std::string formatDoubleClassic(double val)
{
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << std::setprecision(15) << val;
    return oss.str();
}

} // namespace

RecipeConfigV2 parseRecipeV2(const char *text, const StationConfig &station)
{
    auto root = parseJson(text, "recipe JSON");
    require(root.is_object(), IR_BAD_ARGUMENT, "recipe JSON must contain an object");
    allowFields(root, {"$schema", "schema_version", "recipe_id", "steps", "safety", "limits"}, "recipe");
    (void)optionalString(root, "$schema", "", "recipe");
    require(integerField(root, "schema_version", 2, 2, "recipe") == 2,
            IR_BAD_ARGUMENT, "recipe.schema_version must be 2");

    RecipeConfigV2 recipe;
    recipe.id = stringField(root, "recipe_id", "recipe");

    const auto stepsField = root.find("steps");
    require(stepsField != root.end() && stepsField->is_array() && !stepsField->empty() &&
            stepsField->size() <= INT32_MAX,
            IR_BAD_ARGUMENT, "recipe.steps must be a nonempty array");

    std::set<std::string> ids;
    std::set<std::string> analysisNames;
    std::set<std::string> usedDevices;
    std::set<std::string> savedSubdirs;
    std::set<std::string> csvFiles;
    std::map<std::string, SlotKind> knownSlots;
    std::map<std::string, std::string> metricUnits;

    for (size_t index = 0; index < stepsField->size(); ++index) {
        const auto &fields = (*stepsField)[index];
        const auto path = "recipe.steps[" + std::to_string(index) + "]";
        require(fields.is_object(), IR_BAD_ARGUMENT, path + " must be an object");
        StepRef ref{static_cast<int32_t>(index), stringField(fields, "id", path)};
        require(ids.insert(ref.id).second, IR_BAD_ARGUMENT, path + ".id is duplicated");
        const auto type = stringField(fields, "type", path);

        if (type == "save_raw") {
            allowFields(fields, {"id", "type", "input", "subdir"}, path);
            const auto inputSlot = stringField(fields, "input", path);
            const auto foundSlot = knownSlots.find(inputSlot);
            require(foundSlot != knownSlots.end() && foundSlot->second == SlotKind::batch,
                    IR_BAD_ARGUMENT, path + ".input must reference a captured batch slot");
            const auto subdir = optionalString(fields, "subdir", "", path);
            if (!subdir.empty())
                validateRelativeSingleName(subdir, path + ".subdir");
            require(savedSubdirs.insert(subdir).second, IR_BAD_ARGUMENT,
                    path + ": duplicate save_raw target directory '" + subdir + "'");
            recipe.steps.emplace_back(SaveRawStepV2{std::move(ref), inputSlot, subdir});
        } else if (type == "repeat_burst") {
            allowFields(fields, {"id", "type", "max_iterations", "duration_sec", "interval_sec",
                                 "csv_file", "record_metrics", "steps"}, path);
            RepeatBurstStep burst;
            burst.ref = std::move(ref);
            burst.maxIterations = optionalInteger(fields, "max_iterations", 0, 0, INT32_MAX, path);
            burst.durationSec = optionalNumber(fields, "duration_sec", 0.0, path);
            burst.intervalSec = optionalNumber(fields, "interval_sec", 0.0, path);
            require(burst.durationSec >= 0.0 && burst.intervalSec >= 0.0, IR_BAD_ARGUMENT,
                    path + ": duration_sec and interval_sec must be nonnegative");
            require(burst.maxIterations > 0 || burst.durationSec > 0.0, IR_BAD_ARGUMENT,
                    path + ": specify positive max_iterations or duration_sec");
            burst.csvFile = optionalString(fields, "csv_file", "burst_summary.csv", path);
            validateRelativeSingleName(burst.csvFile, path + ".csv_file");
            require(csvFiles.insert(burst.csvFile).second, IR_BAD_ARGUMENT,
                    path + ": duplicated csv_file '" + burst.csvFile + "'");

            const auto innerIt = fields.find("steps");
            require(innerIt != fields.end() && innerIt->is_array() && !innerIt->empty() &&
                    innerIt->size() <= INT32_MAX,
                    IR_BAD_ARGUMENT, path + ".steps must be a nonempty array");

            std::vector<std::string> innerMetrics;
            for (size_t innerIdx = 0; innerIdx < innerIt->size(); ++innerIdx) {
                const auto &innerFields = (*innerIt)[innerIdx];
                const auto innerPath = path + ".steps[" + std::to_string(innerIdx) + "]";
                require(innerFields.is_object(), IR_BAD_ARGUMENT, innerPath + " must be an object");
                StepRef innerRef{static_cast<int32_t>(innerIdx), stringField(innerFields, "id", innerPath)};
                require(ids.insert(innerRef.id).second, IR_BAD_ARGUMENT,
                        innerPath + ".id is duplicated: " + innerRef.id);
                const auto innerType = stringField(innerFields, "type", innerPath);
                require(innerType != "save_raw" && innerType != "repeat_burst", IR_BAD_ARGUMENT,
                        innerPath + ".type '" + innerType + "' is not allowed inside repeat_burst");

                burst.steps.emplace_back(parseSharedControlOrCaptureOrFlushOrAnalyze<InnerStepV2>(
                    std::move(innerRef), innerType, innerFields, innerPath, station,
                    knownSlots, usedDevices, analysisNames, metricUnits, &innerMetrics));
            }

            if (fields.contains("record_metrics")) {
                const auto &rm = fields.at("record_metrics");
                require(rm.is_array() && !rm.empty(), IR_BAD_ARGUMENT,
                        path + ".record_metrics must be a nonempty string array");
                std::set<std::string> innerSet(innerMetrics.begin(), innerMetrics.end());
                for (size_t ri = 0; ri < rm.size(); ++ri) {
                    require(rm[ri].is_string(), IR_BAD_ARGUMENT,
                            path + ".record_metrics[" + std::to_string(ri) + "] must be a string");
                    const auto mName = rm[ri].get<std::string>();
                    require(innerSet.contains(mName), IR_BAD_ARGUMENT,
                            path + ".record_metrics references unknown loop metric: " + mName);
                    burst.recordMetrics.push_back(mName);
                }
            } else {
                burst.recordMetrics = std::move(innerMetrics);
            }

            recipe.hasRepeatBurst = true;
            recipe.steps.emplace_back(std::move(burst));
        } else {
            recipe.steps.emplace_back(parseSharedControlOrCaptureOrFlushOrAnalyze<StepV2>(
                std::move(ref), type, fields, path, station,
                knownSlots, usedDevices, analysisNames, metricUnits, nullptr));
        }
    }

    if (const auto safetyIt = root.find("safety"); safetyIt != root.end()) {
        require(safetyIt->is_object(), IR_BAD_ARGUMENT, "recipe.safety must be an object");
        allowFields(*safetyIt, {"blackbody_standby"}, "recipe.safety");
        if (const auto bbIt = safetyIt->find("blackbody_standby"); bbIt != safetyIt->end()) {
            require(bbIt->is_array(), IR_BAD_ARGUMENT, "recipe.safety.blackbody_standby must be an array");
            for (size_t i = 0; i < bbIt->size(); ++i) {
                const auto &entry = (*bbIt)[i];
                const auto sPath = "recipe.safety.blackbody_standby[" + std::to_string(i) + "]";
                require(entry.is_object(), IR_BAD_ARGUMENT, sPath + " must be an object");
                allowFields(entry, {"device", "target_c"}, sPath);
                const auto alias = stringField(entry, "device", sPath);
                const auto devIt = station.devices.find(alias);
                require(devIt != station.devices.end() &&
                        (devIt->second.kind == IR_PCN7LT || devIt->second.kind == IR_DCN1000),
                        IR_BAD_ARGUMENT, sPath + ".device must name a configured blackbody");
                const double targetC = numberField(entry, "target_c", sPath);
                require(targetC >= -60.0 && targetC <= 100.0, IR_BAD_ARGUMENT,
                        sPath + ".target_c must be within [-60, 100]");
                usedDevices.insert(alias);
                recipe.standbyBlackbodies.push_back({alias, targetC});
            }
        }
    }

    if (const auto limitsIt = root.find("limits"); limitsIt != root.end()) {
        require(limitsIt->is_array(), IR_BAD_ARGUMENT, "recipe.limits must be an array");
        for (size_t index = 0; index < limitsIt->size(); ++index) {
            const auto &limit = (*limitsIt)[index];
            const auto path = "recipe.limits[" + std::to_string(index) + "]";
            require(limit.is_object(), IR_BAD_ARGUMENT, path + " must be an object");
            allowFields(limit, {"metric", "operator", "value", "unit"}, path);
            auto metric = stringField(limit, "metric", path);
            const auto found = metricUnits.find(metric);
            require(found != metricUnits.end(), IR_BAD_ARGUMENT,
                    path + ".metric is not produced by any analyze step: " + metric);
            const auto operation = stringField(limit, "operator", path);
            require(operation == "max" || operation == "min", IR_BAD_ARGUMENT,
                    path + ".operator must be max or min");
            auto unit = stringField(limit, "unit", path);
            require(unit == found->second, IR_BAD_ARGUMENT,
                    path + ".unit does not match metric unit '" + found->second + "'");
            recipe.limits.push_back({std::move(metric), std::move(unit), operation == "max",
                                     numberField(limit, "value", path)});
        }
    }

    recipe.deviceAliases.assign(usedDevices.begin(), usedDevices.end());
    recipe.definition = std::move(root);
    return recipe;
}

void runRecipeV2(ir::acquisition::Capture &capture, const StationConfig &station,
                 const RecipeConfigV2 &recipe, const char *directory, IRRecipeResult &result)
{
    require(directory && *directory, IR_BAD_ARGUMENT, "Specify a new output directory");
    const auto destination = fs::path(reinterpret_cast<const char8_t *>(directory));
    checkDestination(destination);
    capture.checkStop();
    const auto startedUtc = utcTimestampNow();

    V2Progress progress{result};
    bool sourceOpen = false;
    bool sourceStarted = false;
    std::map<std::string, IRHandle> devices;
    std::optional<ir::instruments::DeviceLease> lease;
    std::vector<const PowerStep *> powerToSwitchOff;
    std::exception_ptr failure;

    ExecutionContext ctx;
    V2Readbacks readbacks;
    std::vector<SavedBatchSnapshot> savedSnapshots;
    std::vector<Json> burstRunsSummary;
    bool destinationCreated = false;

    auto ensureCaptureStarted = [&] {
        if (!sourceStarted) {
            capture.start();
            sourceStarted = true;
        }
    };

    try {
        try {
            progress.enter(IR_RECIPE_STAGE_PREPARE);
            capture.setQueueCapacity(station.queueCapacity);
            ctx.geometry = capture.open(station.backend, station.source.c_str(), station.validBits);
            sourceOpen = true;
            capture.checkStop();

            const int64_t maxBatchElements = station.maxBatchBytes / int64_t(sizeof(uint16_t));
            for (const auto &step : recipe.steps) {
                std::visit([&](const auto &s) {
                    using T = std::decay_t<decltype(s)>;
                    progress.enter(IR_RECIPE_STAGE_PREPARE, &s.ref);
                    if constexpr (std::is_same_v<T, CaptureStepV2>) {
                        const auto elements = stackSize(pixels(ctx.geometry.height, ctx.geometry.width), s.frames);
                        require(elements <= maxBatchElements, IR_BAD_ARGUMENT,
                                "Frame batch exceeds station.acquisition.max_batch_bytes");
                    } else if constexpr (std::is_same_v<T, AnalysisStepV2>) {
                        if (s.operations.checkGeometry)
                            s.operations.checkGeometry(ctx.geometry);
                    } else if constexpr (std::is_same_v<T, RepeatBurstStep>) {
                        for (const auto &inner : s.steps) {
                            std::visit([&](const auto &is) {
                                progress.enter(IR_RECIPE_STAGE_PREPARE, &is.ref);
                                validateInnerGeometry(inner, ctx.geometry, maxBatchElements);
                            }, inner);
                        }
                    }
                }, step);
            }
            capture.checkStop();

            progress.enter(IR_RECIPE_STAGE_RESERVE);
            std::vector<IRHandle> handles;
            handles.reserve(recipe.deviceAliases.size());
            for (const auto &alias : recipe.deviceAliases) {
                capture.checkStop();
                const auto &cfg = station.devices.at(alias);
                auto &handle = devices[alias];
                char error[1024]{};
                const auto st = IR_OpenDevice(cfg.kind, cfg.connectionJson.c_str(), &handle, error, sizeof error);
                if (st != IR_OK)
                    throw Error(st, "Cannot open device '" + alias + "': " + error);
                handles.push_back(handle);
            }
            capture.checkStop();
            lease.emplace(handles.data(), handles.size());

            auto executeCommon = [&](const auto &s) {
                using T = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<T, BlackbodyStep>) {
                    progress.enter(IR_RECIPE_STAGE_INSTRUMENT, &s.ref);
                    capture.checkStop();
                    runControlBlackbody(capture, devices.at(s.device), s, readbacks);
                    capture.checkStop();
                } else if constexpr (std::is_same_v<T, PowerStep>) {
                    progress.enter(IR_RECIPE_STAGE_INSTRUMENT, &s.ref);
                    capture.checkStop();
                    runControlPower(devices.at(s.device), s, readbacks, powerToSwitchOff);
                    capture.checkStop();
                } else if constexpr (std::is_same_v<T, FlushStep>) {
                    progress.enter(IR_RECIPE_STAGE_CAPTURE, &s.ref);
                    capture.checkStop();
                    ensureCaptureStarted();
                    int32_t discarded = 0;
                    capture.flush(s.discardFrames, s.timeoutMs, discarded);
                    capture.checkStop();
                } else if constexpr (std::is_same_v<T, CaptureStepV2>) {
                    progress.enter(IR_RECIPE_STAGE_CAPTURE, &s.ref);
                    capture.checkStop();
                    ensureCaptureStarted();
                    const int64_t elements = stackSize(pixels(ctx.geometry.height, ctx.geometry.width), s.frames);
                    auto &batch = ctx.batches[s.outputSlot];
                    batch.geometry = ctx.geometry;
                    batch.pixels.resize(static_cast<size_t>(elements));
                    batch.stamps.resize(static_cast<size_t>(s.frames));
                    int32_t stepCompleted = 0;
                    try {
                        capture.read(s.frames, s.timeoutMs, batch.pixels.data(), elements,
                                     batch.stamps.data(), s.frames, stepCompleted);
                    } catch (...) {
                        result.completed_frames += stepCompleted;
                        throw;
                    }
                    result.completed_frames += stepCompleted;
                    capture.checkStop();
                } else if constexpr (std::is_same_v<T, AnalysisStepV2>) {
                    progress.enter(IR_RECIPE_STAGE_ANALYZE, &s.ref);
                    capture.checkStop();
                    s.operations.execute(ctx);
                    capture.checkStop();
                }
            };

            for (const auto &step : recipe.steps) {
                if (const auto *sr = std::get_if<SaveRawStepV2>(&step)) {
                    progress.enter(IR_RECIPE_STAGE_ARCHIVE, &sr->ref);
                    capture.checkStop();
                    const auto &batch = ctx.batches.at(sr->inputSlot);
                    savedSnapshots.push_back({sr->inputSlot, sr->subdir, batch});
                } else if (const auto *rb = std::get_if<RepeatBurstStep>(&step)) {
                    const auto loopStart = std::chrono::steady_clock::now();
                    std::ofstream csvStream;
                    int32_t iterationsDone = 0;
                    for (;;) {
                        capture.checkStop();
                        if (rb->maxIterations > 0 && iterationsDone >= rb->maxIterations)
                            break;
                        const double elapsedBefore = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - loopStart).count();
                        if (rb->maxIterations == 0 && iterationsDone > 0 && elapsedBefore >= rb->durationSec)
                            break;
                        if (rb->durationSec > 0.0 && iterationsDone > 0 && elapsedBefore >= rb->durationSec)
                            break;

                        for (const auto &inner : rb->steps) {
                            std::visit(executeCommon, inner);
                        }

                        if (!csvStream.is_open()) {
                            if (!destinationCreated) {
                                fs::create_directories(destination);
                                destinationCreated = true;
                            }
                            const fs::path csvPath = destination / pathFromUtf8(rb->csvFile);
                            csvStream.open(csvPath, std::ios::binary | std::ios::out);
                            require(csvStream.is_open(), IR_IO_ERROR,
                                    "Cannot open burst summary CSV: " + rb->csvFile);
                            csvStream << "iteration,timestamp_utc,elapsed_s";
                            for (const auto &col : rb->recordMetrics)
                                csvStream << ',' << col;
                            csvStream << '\n';
                        }
                        const double iterElapsed = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - loopStart).count();
                        csvStream << iterationsDone << ',' << utcTimestampNow() << ','
                                  << formatDoubleClassic(iterElapsed);
                        for (const auto &col : rb->recordMetrics) {
                            const auto itVal = ctx.scalarValues.find(col);
                            require(itVal != ctx.scalarValues.end(), IR_COMPUTATION_FAILED,
                                    "repeat_burst record metric missing: " + col);
                            csvStream << ',' << formatDoubleClassic(itVal->second);
                        }
                        csvStream << '\n';
                        csvStream.flush();
                        require(csvStream.good(), IR_IO_ERROR,
                                "Failed flushing burst summary CSV: " + rb->csvFile);
                        ++iterationsDone;

                        const bool moreByCount = (rb->maxIterations == 0) || (iterationsDone < rb->maxIterations);
                        const double elapsedAfter = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - loopStart).count();
                        const bool moreByTime = (rb->durationSec <= 0.0) || (elapsedAfter < rb->durationSec);
                        if (!moreByCount || !moreByTime)
                            break;

                        if (rb->intervalSec > 0.0) {
                            const auto sleepEnd = std::chrono::steady_clock::now() +
                                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                    std::chrono::duration<double>(rb->intervalSec));
                            while (std::chrono::steady_clock::now() < sleepEnd) {
                                capture.checkStop();
                                const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    sleepEnd - std::chrono::steady_clock::now()).count();
                                if (rem <= 0) break;
                                std::this_thread::sleep_for(
                                    std::chrono::milliseconds(std::min<int64_t>(rem, 50)));
                            }
                        }
                    }
                    burstRunsSummary.push_back({
                        {"id", rb->ref.id},
                        {"csv_file", rb->csvFile},
                        {"iterations", iterationsDone}
                    });
                } else {
                    std::visit(executeCommon, step);
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }

        // Deterministic safety cleanup on success, failure, and cancellation.
        V2CleanupFailure cleanup;
        if (sourceOpen)
            cleanup.attempt(nullptr, [&] { capture.close(); });
        for (auto it = powerToSwitchOff.rbegin(); it != powerToSwitchOff.rend(); ++it) {
            const auto &step = **it;
            cleanup.attempt(&step.ref, [&] {
                char error[1024]{};
                IRPowerReading reading{};
                checkStatus(IR_SetPowerOutput(devices.at(step.device), step.channel, 0, &reading,
                                              error, sizeof error), error);
            });
        }
        for (const auto &sb : recipe.standbyBlackbodies) {
            cleanup.attempt(nullptr, [&] {
                char error[1024]{};
                double readback{};
                checkStatus(IR_SetBlackbody(devices.at(sb.device), sb.targetC, &readback,
                                            error, sizeof error), error);
            });
        }
        lease.reset();
        for (auto it = devices.rbegin(); it != devices.rend(); ++it) {
            if (!it->second) continue;
            cleanup.attempt(nullptr, [&] {
                char error[1024]{};
                checkStatus(IR_CloseDevice(it->second, error, sizeof error), error);
            });
        }
        result.cleanup_status = cleanup.status;
        if (failure) std::rethrow_exception(failure);
        if (cleanup.status != IR_OK) {
            progress.enter(IR_RECIPE_STAGE_CLEANUP, cleanup.step);
            throw Error(cleanup.status, cleanup.detail);
        }

        progress.enter(IR_RECIPE_STAGE_ARCHIVE);
        capture.checkStop();

        bool limitsPass = true;
        Json evaluatedLimits = Json::array();
        for (const auto &limit : recipe.limits) {
            const auto itVal = ctx.scalarValues.find(limit.metric);
            require(itVal != ctx.scalarValues.end(), IR_COMPUTATION_FAILED,
                    "Limit metric was not computed: " + limit.metric);
            const double actual = itVal->second;
            const bool pass = limit.isMaximum ? (actual <= limit.threshold) : (actual >= limit.threshold);
            limitsPass &= pass;
            evaluatedLimits.push_back({
                {"metric", limit.metric},
                {"operator", limit.isMaximum ? "max" : "min"},
                {"value", limit.threshold},
                {"unit", limit.unit},
                {"actual", actual},
                {"result", pass ? "PASS" : "NG"}
            });
        }
        const char *runStatus = recipe.limits.empty() ? "COMPLETED" : (limitsPass ? "PASS" : "NG");

        Json metricsJson = Json::object();
        for (const auto &[key, val] : ctx.metrics)
            metricsJson[key] = val;

        Json savedBatchesMeta = Json::array();
        std::vector<SavedBatchRef> batchRefs;
        batchRefs.reserve(savedSnapshots.size());
        for (const auto &snap : savedSnapshots) {
            batchRefs.push_back({snap.subdir, snap.batch.geometry, &snap.batch.pixels, &snap.batch.stamps});
            savedBatchesMeta.push_back({
                {"input", snap.inputSlot},
                {"subdir", snap.subdir},
                {"frames", snap.batch.stamps.size()},
                {"width", snap.batch.geometry.width},
                {"height", snap.batch.geometry.height}
            });
        }

        const Json record{
            {"schema_version", 2},
            {"recipe_id", recipe.id},
            {"recipe", recipe.definition},
            {"run_status", runStatus},
            {"backend", backendName(station.backend)},
            {"source_name", station.source},
            {"width", ctx.geometry.width},
            {"height", ctx.geometry.height},
            {"valid_bits", ctx.geometry.valid_bits},
            {"completed_frames", result.completed_frames},
            {"cleanup_status", result.cleanup_status},
            {"raw_saved", !savedSnapshots.empty()},
            {"saved_batches", std::move(savedBatchesMeta)},
            {"burst_runs", std::move(burstRunsSummary)},
            {"station_version", IR_StationVersion()},
            {"analysis_version", IR_AnalysisVersion()},
            {"source_commit", ir::station::sourceCommit()},
            {"started_utc", startedUtc},
            {"finished_utc", utcTimestampNow()},
            {"conditions", readbacks.conditions},
            {"instrument_readings", readbacks.instruments},
            {"metrics", std::move(metricsJson)},
            {"limits", std::move(evaluatedLimits)}
        };
        const auto text = record.dump(2) + '\n';

        if (!destinationCreated) {
            saveRecipeArtifacts(destination, batchRefs, text);
        } else {
            for (const auto &ref : batchRefs) {
                const auto targetDir = ref.subdir.empty()
                    ? destination
                    : (destination / pathFromUtf8(ref.subdir));
                writeFrameBatchFiles(targetDir, ref.geometry, *ref.frames, *ref.stamps);
            }
            writeTextFileAtomicOrDirect(destination / "result.json", text);
        }
        progress.enter(IR_RECIPE_STAGE_NONE);
    } catch (const Error &error) {
        if (progress.step)
            throw Error(error.code, "Step '" + progress.step->id + "' failed: " + error.what());
        throw;
    }
}

} // namespace ir::procedures::recipe
