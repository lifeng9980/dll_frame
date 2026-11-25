#include "IR_recipe.h"
#include "IR_analysis.h"
#include "api_common.h"
#include "recipe_config.h"
#include "save_raw.h"
#include "station/acquisition/frame_source.h"
#include "station/instruments/device_lease.h"
#include "station/version.h"
#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <optional>

namespace {
using namespace ir::api;
using namespace ir::procedures;
using namespace ir::procedures::recipe;
namespace fs = std::filesystem;

void emitV1StepEvent(const IRRecipeRunOptions *options, int32_t kind, const StepRef &ref,
                     int32_t stage, int32_t status, double durationMs,
                     const char *stepType, const char *detail = "") noexcept
{
    if (!options || !options->on_step_event)
        return;
    IRRecipeStepEvent ev{};
    ev.struct_size = sizeof(IRRecipeStepEvent);
    ev.event_kind = kind;
    ev.step_index = ref.index;
    ev.stage = stage;
    ev.status = status;
    ev.reserved = 0;
    ev.duration_ms = durationMs;
    ev.step_id_utf8 = ref.id.c_str();
    ev.step_type_utf8 = stepType ? stepType : "";
    ev.detail_utf8 = detail ? detail : "";
    try {
        options->on_step_event(&ev, options->user_data);
    } catch (...) {
    }
}

double elapsedMsSince(std::chrono::steady_clock::time_point start) noexcept
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

const char *v1ControlTypeName(const std::variant<BlackbodyStep, PowerStep> &control) noexcept
{
    if (std::holds_alternative<BlackbodyStep>(control))
        return "blackbody_stabilize";
    const auto &pw = std::get<PowerStep>(control);
    switch (pw.operation) {
    case PowerStep::Operation::set: return "power_set";
    case PowerStep::Operation::output: return "power_output";
    case PowerStep::Operation::read: return "power_read";
    }
    return "power";
}

struct Progress {
    IRRecipeResult &result;
    const StepRef *step{};

    void enter(IRRecipeStage stage, const StepRef *ref = nullptr) noexcept
    {
        step = ref;
        result.failed_stage = stage;
        result.failed_step_index = ref ? ref->index : -1;
    }
};

struct Readbacks {
    Json conditions = Json::array();
    Json instruments = Json::array();
};

struct CleanupFailure {
    int32_t status{IR_OK};
    const StepRef *step{};
    char detail[1024]{};

    // 每项收尾都尝试执行，只保留第一个错误。
    template<class Action>
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

fs::path outputPath(const char *path)
{
    require(path && *path, IR_BAD_ARGUMENT, "Specify a new output directory");
    const std::string text(path);
    return fs::path(std::u8string(text.begin(), text.end()));
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
    return {{"channel", channel}, {"setpoint_v", reading.setpoint_v},
        {"current_limit_a", reading.current_limit_a}, {"measured_v", reading.measured_v},
        {"measured_a", reading.measured_a}, {"output_enabled", reading.output_enabled != 0}};
}

void executeControl(const ir::acquisition::Capture &capture, IRHandle handle,
                    const BlackbodyStep &step, Readbacks &readbacks, std::vector<const PowerStep *> &)
{
    const auto &target = step.target;
    char error[1024]{};
    double setpoint{};
    checkStatus(IR_SetBlackbody(handle, target.temperatureC, &setpoint, error, sizeof error), error);
    const auto reading = waitForBlackbody(capture, handle, target);
    readbacks.conditions.push_back({{"device", step.device}, {"target_c", target.temperatureC},
        {"setpoint_readback_c", setpoint}, {"actual_c", reading.actual_c},
        {"device_stable", reading.stable != 0}, {"tolerance_c", target.toleranceC},
        {"stable_window_ms", target.stableWindowMs}, {"timeout_ms", target.timeoutMs}});
}

void executeControl(const ir::acquisition::Capture &, IRHandle handle, const PowerStep &step,
                    Readbacks &readbacks, std::vector<const PowerStep *> &powerToSwitchOff)
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
        // 写入可能到达设备却没有应答，因此先记下收尾动作，再发开关命令。
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
    readbacks.instruments.push_back({{"step", step.ref.id}, {"device", step.device},
        {"operation", action}, {"reading", powerJson(reading, step.channel)}});
}

void acquireBatch(ir::acquisition::Capture &capture, const StationConfig &station, const RecipeConfig &recipe,
                  FrameBatch &batch, Readbacks &readbacks, Progress &progress,
                  const IRRecipeRunOptions *options)
{
    bool sourceOpen = false;
    std::map<std::string, IRHandle> devices;
    std::optional<ir::instruments::DeviceLease> lease;
    std::vector<const PowerStep *> powerToSwitchOff;
    std::exception_ptr failure;

    try {
        progress.enter(IR_RECIPE_STAGE_PREPARE, &recipe.capture.ref);
        batch.geometry = capture.open(station.backend, station.source.c_str(), station.validBits,
                                      station.queueCapacity, station.maxInflightBytes);
        sourceOpen = true;
        capture.checkStop();
        const uint64_t batchBytes = batchStorageBytes(
            batch.geometry, recipe.capture.frames, station.maxBatchBytes);
        requireWithinMemoryLimit(addBytes(capture.bufferedBytes(), batchBytes), station.maxInflightBytes);
        const auto elements = stackSize(pixels(batch.geometry.height, batch.geometry.width), recipe.capture.frames);

        for (const auto &step : recipe.analysis) {
            if (step.operations.checkGeometry) {
                progress.enter(IR_RECIPE_STAGE_PREPARE, &step.ref);
                step.operations.checkGeometry(batch.geometry);
            }
        }
        progress.enter(IR_RECIPE_STAGE_PREPARE, &recipe.capture.ref);
        batch.pixels.resize(static_cast<size_t>(elements));
        batch.stamps.resize(static_cast<size_t>(recipe.capture.frames));
        powerToSwitchOff.reserve(recipe.controls.size());
        capture.checkStop();

        progress.enter(IR_RECIPE_STAGE_RESERVE);
        std::vector<IRHandle> handles;
        handles.reserve(recipe.deviceAliases.size());
        for (const auto &alias : recipe.deviceAliases) {
            capture.checkStop();
            const auto &config = station.devices.at(alias);
            auto &handle = devices[alias];
            char error[1024]{};
            const auto status = IR_OpenDevice(config.kind, config.connectionJson.c_str(), &handle, error, sizeof error);
            if (status != IR_OK)
                throw Error(status, "Cannot open device '" + alias + "': " + error);
            handles.push_back(handle);
        }
        capture.checkStop();
        lease.emplace(handles.data(), handles.size());

        for (const auto &control : recipe.controls) {
            std::visit([&](const auto &step) {
                const char *typeName = v1ControlTypeName(control);
                progress.enter(IR_RECIPE_STAGE_INSTRUMENT, &step.ref);
                emitV1StepEvent(options, IR_STEP_EVENT_STARTED, step.ref,
                                IR_RECIPE_STAGE_INSTRUMENT, IR_OK, 0.0, typeName);
                const auto t0 = std::chrono::steady_clock::now();
                try {
                    capture.checkStop();
                    executeControl(capture, devices.at(step.device), step, readbacks, powerToSwitchOff);
                    capture.checkStop();
                    emitV1StepEvent(options, IR_STEP_EVENT_COMPLETED, step.ref,
                                    IR_RECIPE_STAGE_INSTRUMENT, IR_OK, elapsedMsSince(t0), typeName);
                } catch (const Error &e) {
                    emitV1StepEvent(options, IR_STEP_EVENT_FAILED, step.ref,
                                    IR_RECIPE_STAGE_INSTRUMENT, e.code, elapsedMsSince(t0), typeName, e.what());
                    throw;
                } catch (const std::exception &e) {
                    emitV1StepEvent(options, IR_STEP_EVENT_FAILED, step.ref,
                                    IR_RECIPE_STAGE_INSTRUMENT, IR_COMPUTATION_FAILED, elapsedMsSince(t0), typeName, e.what());
                    throw;
                }
            }, control);
        }

        progress.enter(IR_RECIPE_STAGE_CAPTURE, &recipe.capture.ref);
        emitV1StepEvent(options, IR_STEP_EVENT_STARTED, recipe.capture.ref,
                        IR_RECIPE_STAGE_CAPTURE, IR_OK, 0.0, "capture");
        const auto capStart = std::chrono::steady_clock::now();
        try {
            capture.start();
            capture.read(recipe.capture.frames, recipe.capture.timeoutMs, batch.pixels.data(), elements,
                         batch.stamps.data(), recipe.capture.frames, progress.result.completed_frames);
            emitV1StepEvent(options, IR_STEP_EVENT_COMPLETED, recipe.capture.ref,
                            IR_RECIPE_STAGE_CAPTURE, IR_OK, elapsedMsSince(capStart), "capture");
        } catch (const Error &e) {
            emitV1StepEvent(options, IR_STEP_EVENT_FAILED, recipe.capture.ref,
                            IR_RECIPE_STAGE_CAPTURE, e.code, elapsedMsSince(capStart), "capture", e.what());
            throw;
        } catch (const std::exception &e) {
            emitV1StepEvent(options, IR_STEP_EVENT_FAILED, recipe.capture.ref,
                            IR_RECIPE_STAGE_CAPTURE, IR_COMPUTATION_FAILED, elapsedMsSince(capStart), "capture", e.what());
            throw;
        }
    } catch (...) {
        failure = std::current_exception();
    }

    CleanupFailure cleanup;
    if (sourceOpen)
        cleanup.attempt(&recipe.capture.ref, [&] { capture.close(); });
    for (auto it = powerToSwitchOff.rbegin(); it != powerToSwitchOff.rend(); ++it) {
        const auto &step = **it;
        cleanup.attempt(&step.ref, [&] {
            char error[1024]{};
            IRPowerReading reading{};
            checkStatus(IR_SetPowerOutput(devices.at(step.device), step.channel, 0, &reading,
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

    progress.result.cleanup_status = cleanup.status;
    if (failure) std::rethrow_exception(failure);
    if (cleanup.status != IR_OK) {
        progress.enter(IR_RECIPE_STAGE_CLEANUP, cleanup.step);
        throw Error(cleanup.status, cleanup.detail);
    }
}

Json analyzeBatch(const FrameBatch &batch, const RecipeConfig &recipe,
                  const ir::acquisition::Capture &capture, Progress &progress,
                  const IRRecipeRunOptions *options)
{
    Json metrics = Json::object();
    for (const auto &step : recipe.analysis) {
        progress.enter(IR_RECIPE_STAGE_ANALYZE, &step.ref);
        emitV1StepEvent(options, IR_STEP_EVENT_STARTED, step.ref,
                        IR_RECIPE_STAGE_ANALYZE, IR_OK, 0.0, "analyze");
        const auto t0 = std::chrono::steady_clock::now();
        try {
            capture.checkStop();
            const auto values = step.operations.calculate(batch);
            capture.checkStop();
            require(values.size() == step.outputs.size(), IR_COMPUTATION_FAILED,
                    "Metric returned a different number of values than declared");
            for (size_t index = 0; index < values.size(); ++index) {
                const auto &output = step.outputs[index];
                metrics[step.name + output.suffix] = {{"value", values[index]}, {"unit", output.unit},
                                                     {"method", output.method}, {"version", output.version}};
            }
            emitV1StepEvent(options, IR_STEP_EVENT_COMPLETED, step.ref,
                            IR_RECIPE_STAGE_ANALYZE, IR_OK, elapsedMsSince(t0), "analyze");
        } catch (const Error &e) {
            emitV1StepEvent(options, IR_STEP_EVENT_FAILED, step.ref,
                            IR_RECIPE_STAGE_ANALYZE, e.code, elapsedMsSince(t0), "analyze", e.what());
            throw;
        } catch (const std::exception &e) {
            emitV1StepEvent(options, IR_STEP_EVENT_FAILED, step.ref,
                            IR_RECIPE_STAGE_ANALYZE, IR_COMPUTATION_FAILED, elapsedMsSince(t0), "analyze", e.what());
            throw;
        }
    }
    return metrics;
}

void archive(const StationConfig &station, const RecipeConfig &recipe, const FrameBatch &batch,
             const Readbacks &readbacks, Json metrics, const fs::path &destination,
             const ir::acquisition::Capture &capture, const IRRecipeResult &result,
             const std::string &startedUtc)
{
    bool limitsPass = true;
    Json evaluatedLimits = Json::array();
    for (const auto &limit : recipe.limits) {
        const auto actual = metrics.at(limit.metric).at("value").get<double>();
        const bool pass = limit.isMaximum ? actual <= limit.threshold : actual >= limit.threshold;
        limitsPass &= pass;
        evaluatedLimits.push_back({{"metric", limit.metric}, {"operator", limit.isMaximum ? "max" : "min"},
            {"value", limit.threshold}, {"unit", limit.unit}, {"actual", actual},
            {"result", pass ? "PASS" : "NG"}});
    }
    const char *runStatus = recipe.limits.empty() ? "COMPLETED" : (limitsPass ? "PASS" : "NG");
    const auto &geometry = batch.geometry;
    const auto finishedUtc = utcTimestampNow();
    const Json record{
        {"schema_version", 1}, {"recipe_id", recipe.id}, {"recipe", recipe.definition},
        {"run_status", runStatus}, {"backend", backendName(station.backend)}, {"source_name", station.source},
        {"width", geometry.width}, {"height", geometry.height}, {"valid_bits", geometry.valid_bits},
        {"requested_frames", recipe.capture.frames}, {"completed_frames", result.completed_frames},
        {"cleanup_status", result.cleanup_status}, {"station_version", IR_StationVersion()},
        {"analysis_version", IR_AnalysisVersion()}, {"source_commit", ir::station::sourceCommit()},
        {"started_utc", startedUtc}, {"finished_utc", finishedUtc},
        {"conditions", readbacks.conditions}, {"instrument_readings", readbacks.instruments},
        {"metrics", std::move(metrics)}, {"limits", std::move(evaluatedLimits)}
    };
    const auto text = record.dump(2) + '\n';
    capture.checkStop();
    saveFrameBatch(destination, geometry, batch.pixels, batch.stamps, text);
}

void runRecipe(ir::acquisition::Capture &capture, const StationConfig &station, const RecipeConfig &recipe,
               const char *directory, Progress &progress, const IRRecipeRunOptions *options)
{
    const auto destination = outputPath(directory);
    checkDestination(destination);
    capture.checkStop();
    const auto startedUtc = utcTimestampNow();
    FrameBatch batch;
    Readbacks readbacks;
    try {
        acquireBatch(capture, station, recipe, batch, readbacks, progress, options);
        auto metrics = analyzeBatch(batch, recipe, capture, progress, options);
        progress.enter(IR_RECIPE_STAGE_ARCHIVE, &recipe.archive);
        emitV1StepEvent(options, IR_STEP_EVENT_STARTED, recipe.archive,
                        IR_RECIPE_STAGE_ARCHIVE, IR_OK, 0.0, "save_raw");
        const auto archStart = std::chrono::steady_clock::now();
        try {
            archive(station, recipe, batch, readbacks, std::move(metrics), destination, capture, progress.result, startedUtc);
            emitV1StepEvent(options, IR_STEP_EVENT_COMPLETED, recipe.archive,
                            IR_RECIPE_STAGE_ARCHIVE, IR_OK, elapsedMsSince(archStart), "save_raw");
        } catch (const Error &e) {
            emitV1StepEvent(options, IR_STEP_EVENT_FAILED, recipe.archive,
                            IR_RECIPE_STAGE_ARCHIVE, e.code, elapsedMsSince(archStart), "save_raw", e.what());
            throw;
        } catch (const std::exception &e) {
            emitV1StepEvent(options, IR_STEP_EVENT_FAILED, recipe.archive,
                            IR_RECIPE_STAGE_ARCHIVE, IR_COMPUTATION_FAILED, elapsedMsSince(archStart), "save_raw", e.what());
            throw;
        }
    } catch (const Error &error) {
        if (progress.step)
            throw Error(error.code, "Step '" + progress.step->id + "' failed: " + error.what());
        throw;
    }
    progress.enter(IR_RECIPE_STAGE_NONE);
}
}

extern "C" int32_t IR_CheckRecipeJson(const char *stationJson, const char *recipeJson,
    char *err, int32_t errBytes)
{
    return guarded(err, errBytes, [&] {
        const auto station = parseStation(stationJson);
        const int schemaVer = detectRecipeSchemaVersion(recipeJson);
        if (schemaVer == 2)
            (void)parseRecipeV2(recipeJson, station);
        else
            (void)parseRecipe(recipeJson, station);
    });
}

extern "C" int32_t IR_RunRecipeJsonEx(const char *stationJson, const char *recipeJson,
    const char *directory, const IRRecipeRunOptions *options,
    IRRecipeResult *result, char *err, int32_t errBytes)
{
    IRRecipeResult report{0, IR_RECIPE_STAGE_VALIDATE, -1, IR_OK};
    Progress progress{report};
    const auto status = guarded(err, errBytes, [&] {
        if (options != nullptr) {
            require(options->struct_size == sizeof(IRRecipeRunOptions), IR_BAD_ARGUMENT,
                    "Invalid IRRecipeRunOptions struct_size");
            require(options->flags == 0, IR_BAD_ARGUMENT,
                    "IRRecipeRunOptions.flags must be 0");
        }
        progress.enter(IR_RECIPE_STAGE_RESERVE);
        Measurement measurement;
        progress.enter(IR_RECIPE_STAGE_VALIDATE);
        const auto station = parseStation(stationJson);
        measurement.frames().checkStop();
        const int schemaVer = detectRecipeSchemaVersion(recipeJson);
        measurement.frames().checkStop();
        if (schemaVer == 2) {
            const auto recipe = parseRecipeV2(recipeJson, station);
            measurement.frames().checkStop();
            runRecipeV2(measurement.frames(), station, recipe, directory, report, options);
        } else {
            const auto recipe = parseRecipe(recipeJson, station);
            measurement.frames().checkStop();
            runRecipe(measurement.frames(), station, recipe, directory, progress, options);
        }
    });
    if (result) *result = report;
    return status;
}

extern "C" int32_t IR_RunRecipeJson(const char *stationJson, const char *recipeJson,
    const char *directory, IRRecipeResult *result, char *err, int32_t errBytes)
{
    return IR_RunRecipeJsonEx(stationJson, recipeJson, directory, nullptr, result, err, errBytes);
}
