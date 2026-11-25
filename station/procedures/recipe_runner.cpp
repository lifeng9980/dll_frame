#include "recipe_runtime_common.h"
#include "IR_analysis.h"
#include "IR_device.h"
#include "api_common.h"
#include "utf8_path.h"
#include "station/instruments/device_lease.h"
#include "station/version.h"
#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <thread>

namespace ir::procedures::recipe {
using namespace ir::api;
namespace fs = std::filesystem;

namespace {

using ir::utf8::pathFromUtf8;

struct SavedBatchSnapshot {
    std::string inputSlot;
    std::string subdir;
    FrameBatch batch;
};

uint64_t retainedRecipeStorage(const RecipeConfig &recipe, IRGeometry geometry,
                               int64_t maxBatchBytes)
{
    struct BatchSlot {
        int32_t currentFrames{};
        int32_t maxFrames{};
    };

    uint64_t total = 0;
    std::map<std::string, BatchSlot> batchSlots;
    const auto recordCapture = [&](const CaptureStep &step) {
        auto &slot = batchSlots[step.outputSlot];
        slot.currentFrames = step.frames;
        slot.maxFrames = std::max(slot.maxFrames, step.frames);
    };

    for (const auto &step : recipe.steps) {
        if (const auto *capture = std::get_if<CaptureStep>(&step)) {
            recordCapture(*capture);
        } else if (const auto *burst = std::get_if<RepeatBurstStep>(&step)) {
            for (const auto &inner : burst->steps)
                if (const auto *capture = std::get_if<CaptureStep>(&inner))
                    recordCapture(*capture);
        } else if (const auto *save = std::get_if<SaveRawStep>(&step)) {
            const auto frames = batchSlots.at(save->inputSlot).currentFrames;
            total = addBytes(total, batchStorageBytes(geometry, frames, maxBatchBytes));
        }
    }

    for (const auto &[name, slot] : batchSlots) {
        (void)name;
        total = addBytes(total, batchStorageBytes(geometry, slot.maxFrames, maxBatchBytes));
    }

    const uint64_t pixelCount = static_cast<uint64_t>(pixels(geometry.height, geometry.width));
    total = addBytes(total, multiplyBytes(
        multiplyBytes(pixelCount, sizeof(double)), recipe.doubleMapSlotCount));
    total = addBytes(total, multiplyBytes(pixelCount, recipe.byteMaskSlotCount));
    total = addBytes(total, multiplyBytes(sizeof(IRGjbDriftContext), recipe.driftContextSlotCount));
    return total;
}


void validateInnerGeometry(const InnerStep &step, IRGeometry geom, int64_t maxBatchElements)
{
    if (const auto *cap = std::get_if<CaptureStep>(&step)) {
        const auto elements = stackSize(pixels(geom.height, geom.width), cap->frames);
        require(elements <= maxBatchElements, IR_BAD_ARGUMENT,
                "Frame batch exceeds station.acquisition.max_batch_bytes");
    } else if (const auto *an = std::get_if<AnalysisStep>(&step)) {
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

struct TopStepInfo {
    const StepRef *ref{nullptr};
    const char *typeName{""};
    int32_t initialStage{IR_RECIPE_STAGE_NONE};
};

TopStepInfo topStepInfo(const RecipeStep &step) noexcept
{
    return std::visit([](const auto &s) -> TopStepInfo {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, BlackbodyStep>) {
            return {&s.ref, "blackbody_stabilize", IR_RECIPE_STAGE_INSTRUMENT};
        } else if constexpr (std::is_same_v<T, PowerStep>) {
            const char *name = "power_read";
            if (s.operation == PowerStep::Operation::set) name = "power_set";
            else if (s.operation == PowerStep::Operation::output) name = "power_output";
            return {&s.ref, name, IR_RECIPE_STAGE_INSTRUMENT};
        } else if constexpr (std::is_same_v<T, PowerWaitStableStep>) {
            return {&s.ref, "power_wait_stable", IR_RECIPE_STAGE_INSTRUMENT};
        } else if constexpr (std::is_same_v<T, OvenStabilizeStep>) {
            return {&s.ref, "oven_stabilize", IR_RECIPE_STAGE_INSTRUMENT};
        } else if constexpr (std::is_same_v<T, OvenStandbyStep>) {
            return {&s.ref, "oven_standby", IR_RECIPE_STAGE_INSTRUMENT};
        } else if constexpr (std::is_same_v<T, DmmReadStep>) {
            return {&s.ref, "dmm_read", IR_RECIPE_STAGE_INSTRUMENT};
        } else if constexpr (std::is_same_v<T, DetectorTimingStep>) {
            return {&s.ref, "detector_timing", IR_RECIPE_STAGE_INSTRUMENT};
        } else if constexpr (std::is_same_v<T, DetectorGpolStep>) {
            return {&s.ref, "detector_gpol", IR_RECIPE_STAGE_INSTRUMENT};
        } else if constexpr (std::is_same_v<T, DetectorRegisterStep>) {
            return {&s.ref, "detector_register", IR_RECIPE_STAGE_INSTRUMENT};
        } else if constexpr (std::is_same_v<T, DetectorCurrentStep>) {
            return {&s.ref, "detector_current", IR_RECIPE_STAGE_INSTRUMENT};
        } else if constexpr (std::is_same_v<T, CoolerReadStep>) {
            return {&s.ref, "cooler_read", IR_RECIPE_STAGE_INSTRUMENT};
        } else if constexpr (std::is_same_v<T, FlushStep>) {
            return {&s.ref, "flush_frames", IR_RECIPE_STAGE_CAPTURE};
        } else if constexpr (std::is_same_v<T, CaptureStep>) {
            return {&s.ref, "capture", IR_RECIPE_STAGE_CAPTURE};
        } else if constexpr (std::is_same_v<T, AnalysisStep>) {
            return {&s.ref, "analyze", IR_RECIPE_STAGE_ANALYZE};
        } else if constexpr (std::is_same_v<T, SaveRawStep>) {
            return {&s.ref, "save_raw", IR_RECIPE_STAGE_ARCHIVE};
        } else if constexpr (std::is_same_v<T, RepeatBurstStep>) {
            return {&s.ref, "repeat_burst", IR_RECIPE_STAGE_CAPTURE};
        }
        return {};
    }, step);
}

struct RuntimeState {
    ir::acquisition::Capture &capture;
    const StationConfig &station;
    const RecipeConfig &recipe;
    const fs::path &destination;
    RecipeProgress &progress;
    bool sourceOpened{false};
    bool sourceStarted{false};
    std::map<std::string, IRHandle> devices;
    std::optional<ir::instruments::DeviceLease> lease;
    std::vector<const PowerStep *> powerToSwitchOff;
    std::vector<std::string> ovensToStandby;
    ExecutionContext ctx;
    RecipeReadbacks readbacks;
    std::vector<SavedBatchSnapshot> savedSnapshots;
    uint64_t savedSnapshotBytes{0};
    std::vector<Json> burstRunsSummary;
    Json stepRecords = Json::array();
    bool destinationCreated{false};
    bool freshFramesRequired{false};

    void ensureCaptureStarted()
    {
        if (!sourceStarted) {
            capture.start();
            sourceStarted = true;
        }
    }

    void clearPendingFrames()
    {
        if (!freshFramesRequired || !capture.source || !capture.source->isLive())
            return;
        int32_t discarded = 0;
        capture.flush(0, 0, discarded);
        freshFramesRequired = false;
    }
};

void preflightRecipe(RuntimeState &rt)
{
    rt.progress.enter(IR_RECIPE_STAGE_PREPARE);
    rt.ctx.geometry = rt.capture.open(rt.station.backend, rt.station.source.c_str(),
                                      rt.station.validBits, rt.station.queueCapacity,
                                      rt.station.maxInflightBytes);
    rt.sourceOpened = true;
    rt.capture.checkStop();

    const int64_t maxBatchElements = rt.station.maxBatchBytes / int64_t(sizeof(uint16_t));
    for (const auto &step : rt.recipe.steps) {
        std::visit([&](const auto &s) {
            using T = std::decay_t<decltype(s)>;
            rt.progress.enter(IR_RECIPE_STAGE_PREPARE, &s.ref);
            if constexpr (std::is_same_v<T, CaptureStep>) {
                const auto elements = stackSize(
                    pixels(rt.ctx.geometry.height, rt.ctx.geometry.width), s.frames);
                require(elements <= maxBatchElements, IR_BAD_ARGUMENT,
                        "Frame batch exceeds station.acquisition.max_batch_bytes");
            } else if constexpr (std::is_same_v<T, AnalysisStep>) {
                if (s.operations.checkGeometry)
                    s.operations.checkGeometry(rt.ctx.geometry);
            } else if constexpr (std::is_same_v<T, RepeatBurstStep>) {
                for (const auto &inner : s.steps) {
                    std::visit([&](const auto &is) {
                        rt.progress.enter(IR_RECIPE_STAGE_PREPARE, &is.ref);
                        validateInnerGeometry(inner, rt.ctx.geometry, maxBatchElements);
                    }, inner);
                }
            }
        }, step);
    }
    uint64_t requiredStorage = rt.capture.bufferedBytes();
    requiredStorage = addBytes(requiredStorage,
                               retainedRecipeStorage(rt.recipe, rt.ctx.geometry, rt.station.maxBatchBytes));
    requireWithinMemoryLimit(requiredStorage, rt.station.maxInflightBytes);
    rt.capture.checkStop();
}

void openAndLeaseDevices(RuntimeState &rt)
{
    rt.progress.enter(IR_RECIPE_STAGE_RESERVE);
    std::vector<IRHandle> handles;
    handles.reserve(rt.recipe.deviceAliases.size());
    for (const auto &alias : rt.recipe.deviceAliases) {
        rt.capture.checkStop();
        const auto &cfg = rt.station.devices.at(alias);
        auto &handle = rt.devices[alias];
        char error[1024]{};
        const auto st = IR_OpenDevice(cfg.kind, cfg.connectionJson.c_str(), &handle, error, sizeof error);
        if (st != IR_OK)
            throw Error(st, "Cannot open device '" + alias + "': " + error);
        handles.push_back(handle);
    }
    rt.capture.checkStop();
    rt.lease.emplace(handles.data(), handles.size());
}

void executeCommonStep(RuntimeState &rt, const auto &s)
{
    using T = std::decay_t<decltype(s)>;
    if constexpr (std::is_same_v<T, BlackbodyStep>) {
        rt.progress.enter(IR_RECIPE_STAGE_INSTRUMENT, &s.ref);
        rt.capture.checkStop();
        runControlBlackbody(rt.capture, rt.devices.at(s.device), s, rt.readbacks);
        rt.freshFramesRequired = true;
        rt.capture.checkStop();
    } else if constexpr (std::is_same_v<T, PowerStep>) {
        rt.progress.enter(IR_RECIPE_STAGE_INSTRUMENT, &s.ref);
        rt.capture.checkStop();
        runControlPower(rt.devices.at(s.device), s, rt.readbacks, rt.powerToSwitchOff);
        if (s.operation != PowerStep::Operation::read)
            rt.freshFramesRequired = true;
        rt.capture.checkStop();
    } else if constexpr (std::is_same_v<T, PowerWaitStableStep>) {
        rt.progress.enter(IR_RECIPE_STAGE_INSTRUMENT, &s.ref);
        rt.capture.checkStop();
        runControlPowerWaitStable(rt.capture, rt.devices.at(s.device), s, rt.readbacks);
        rt.freshFramesRequired = true;
        rt.capture.checkStop();
    } else if constexpr (std::is_same_v<T, OvenStabilizeStep>) {
        rt.progress.enter(IR_RECIPE_STAGE_INSTRUMENT, &s.ref);
        rt.capture.checkStop();
        runControlOvenStabilize(rt.capture, rt.devices.at(s.device), s, rt.readbacks, rt.ovensToStandby);
        rt.freshFramesRequired = true;
        rt.capture.checkStop();
    } else if constexpr (std::is_same_v<T, OvenStandbyStep>) {
        rt.progress.enter(IR_RECIPE_STAGE_INSTRUMENT, &s.ref);
        rt.capture.checkStop();
        runControlOvenStandby(rt.devices.at(s.device), s, rt.readbacks);
        rt.freshFramesRequired = true;
        rt.capture.checkStop();
    } else if constexpr (std::is_same_v<T, DmmReadStep>) {
        rt.progress.enter(IR_RECIPE_STAGE_INSTRUMENT, &s.ref);
        rt.capture.checkStop();
        runControlDmmRead(rt.devices.at(s.device), s, rt.readbacks, rt.ctx);
        rt.capture.checkStop();
    } else if constexpr (std::is_same_v<T, DetectorTimingStep>) {
        rt.progress.enter(IR_RECIPE_STAGE_INSTRUMENT, &s.ref);
        rt.capture.checkStop();
        runControlDetectorTiming(rt.devices.at(s.device), s, rt.readbacks);
        rt.freshFramesRequired = true;
        rt.capture.checkStop();
    } else if constexpr (std::is_same_v<T, DetectorGpolStep>) {
        rt.progress.enter(IR_RECIPE_STAGE_INSTRUMENT, &s.ref);
        rt.capture.checkStop();
        runControlDetectorGpol(rt.devices.at(s.device), s, rt.readbacks);
        rt.freshFramesRequired = true;
        rt.capture.checkStop();
    } else if constexpr (std::is_same_v<T, DetectorRegisterStep>) {
        rt.progress.enter(IR_RECIPE_STAGE_INSTRUMENT, &s.ref);
        rt.capture.checkStop();
        runControlDetectorRegister(rt.devices.at(s.device), s, rt.readbacks);
        rt.freshFramesRequired = true;
        rt.capture.checkStop();
    } else if constexpr (std::is_same_v<T, DetectorCurrentStep>) {
        rt.progress.enter(IR_RECIPE_STAGE_INSTRUMENT, &s.ref);
        rt.capture.checkStop();
        runControlDetectorCurrent(rt.devices.at(s.device), s, rt.readbacks, rt.ctx);
        rt.capture.checkStop();
    } else if constexpr (std::is_same_v<T, CoolerReadStep>) {
        rt.progress.enter(IR_RECIPE_STAGE_INSTRUMENT, &s.ref);
        rt.capture.checkStop();
        runControlCoolerRead(rt.devices.at(s.device), s, rt.readbacks, rt.ctx);
        rt.capture.checkStop();
    } else if constexpr (std::is_same_v<T, FlushStep>) {
        rt.progress.enter(IR_RECIPE_STAGE_CAPTURE, &s.ref);
        rt.capture.checkStop();
        rt.ensureCaptureStarted();
        int32_t discarded = 0;
        rt.capture.flush(s.discardFrames, s.timeoutMs, discarded);
        rt.freshFramesRequired = false;
        rt.capture.checkStop();
    } else if constexpr (std::is_same_v<T, CaptureStep>) {
        rt.progress.enter(IR_RECIPE_STAGE_CAPTURE, &s.ref);
        rt.capture.checkStop();
        rt.ensureCaptureStarted();
        rt.clearPendingFrames();
        const int64_t elements = stackSize(
            pixels(rt.ctx.geometry.height, rt.ctx.geometry.width), s.frames);
        auto &batch = rt.ctx.batches[s.outputSlot];
        batch.geometry = rt.ctx.geometry;
        batch.pixels.resize(static_cast<size_t>(elements));
        batch.stamps.resize(static_cast<size_t>(s.frames));
        int32_t stepCompleted = 0;
        try {
            rt.capture.read(s.frames, s.timeoutMs, batch.pixels.data(), elements,
                            batch.stamps.data(), s.frames, stepCompleted);
        } catch (...) {
            rt.progress.result.completed_frames += stepCompleted;
            throw;
        }
        rt.progress.result.completed_frames += stepCompleted;
        rt.capture.checkStop();
        requireWithinMemoryLimit(addBytes(
            addBytes(rt.capture.bufferedBytes(), rt.ctx.retainedBytes()), rt.savedSnapshotBytes),
            rt.station.maxInflightBytes);
    } else if constexpr (std::is_same_v<T, AnalysisStep>) {
        rt.progress.enter(IR_RECIPE_STAGE_ANALYZE, &s.ref);
        rt.capture.checkStop();
        AnalysisRunContext runCtx{
            rt.ctx,
            [&capture = rt.capture]() { return capture.isStopRequested(); },
            addBytes(rt.capture.bufferedBytes(), rt.savedSnapshotBytes),
            rt.station.maxInflightBytes
        };
        s.operations.execute(runCtx);
        rt.capture.checkStop();
        requireWithinMemoryLimit(addBytes(
            addBytes(rt.capture.bufferedBytes(), rt.ctx.retainedBytes()), rt.savedSnapshotBytes),
            rt.station.maxInflightBytes);
    }
}

int32_t executeRepeatBurstStep(RuntimeState &rt, const RepeatBurstStep &rb)
{
    rt.progress.enter(IR_RECIPE_STAGE_CAPTURE, &rb.ref);
    const auto loopStart = std::chrono::steady_clock::now();
    std::ofstream csvStream;
    int32_t iterationsDone = 0;
    for (;;) {
        rt.progress.enter(IR_RECIPE_STAGE_CAPTURE, &rb.ref);
        rt.capture.checkStop();
        if (rb.maxIterations > 0 && iterationsDone >= rb.maxIterations)
            break;
        const double elapsedBefore = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - loopStart).count();
        if (rb.maxIterations == 0 && iterationsDone > 0 && elapsedBefore >= rb.durationSec)
            break;
        if (rb.durationSec > 0.0 && iterationsDone > 0 && elapsedBefore >= rb.durationSec)
            break;

        for (const auto &inner : rb.steps) {
            std::visit([&](const auto &s) { executeCommonStep(rt, s); }, inner);
        }

        rt.progress.enter(IR_RECIPE_STAGE_ARCHIVE, &rb.ref);
        if (!csvStream.is_open()) {
            if (!rt.destinationCreated) {
                std::error_code createError;
                const bool created = fs::create_directory(rt.destination, createError);
                require(created && !createError, IR_IO_ERROR,
                        "Cannot exclusively create the recipe output directory");
                rt.destinationCreated = true;
            }
            const fs::path csvPath = rt.destination / pathFromUtf8(rb.csvFile);
            csvStream.open(csvPath, std::ios::binary | std::ios::out);
            require(csvStream.is_open(), IR_IO_ERROR,
                    "Cannot open burst summary CSV: " + rb.csvFile);
            csvStream << "iteration,timestamp_utc,elapsed_s";
            for (const auto &col : rb.recordMetrics) {
                const auto itUnit = rt.recipe.metricUnits.find(col);
                if (itUnit != rt.recipe.metricUnits.end() && !itUnit->second.empty())
                    csvStream << ',' << col << " [" << itUnit->second << ']';
                else
                    csvStream << ',' << col;
            }
            csvStream << '\n';
        }
        const double iterElapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - loopStart).count();
        {
            std::ostringstream elapsedFmt;
            elapsedFmt.imbue(std::locale::classic());
            elapsedFmt << std::fixed << std::setprecision(3) << iterElapsed;
            csvStream << iterationsDone << ',' << utcTimestampNow() << ',' << elapsedFmt.str();
        }
        for (const auto &col : rb.recordMetrics) {
            const auto itVal = rt.ctx.scalarValues.find(col);
            require(itVal != rt.ctx.scalarValues.end(), IR_COMPUTATION_FAILED,
                    "repeat_burst record metric missing: " + col);
            csvStream << ',' << formatDoubleClassic(itVal->second);
        }
        csvStream << '\n';
        csvStream.flush();
        require(csvStream.good(), IR_IO_ERROR,
                "Failed flushing burst summary CSV: " + rb.csvFile);
        ++iterationsDone;
        if (rt.capture.source && rt.capture.source->isLive())
            rt.freshFramesRequired = true;

        const bool moreByCount = (rb.maxIterations == 0) || (iterationsDone < rb.maxIterations);
        const double elapsedAfter = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - loopStart).count();
        const bool moreByTime = (rb.durationSec <= 0.0) || (elapsedAfter < rb.durationSec);
        if (!moreByCount || !moreByTime)
            break;

        if (rb.intervalSec > 0.0) {
            rt.progress.enter(IR_RECIPE_STAGE_CAPTURE, &rb.ref);
            const auto sleepEnd = std::chrono::steady_clock::now() +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(rb.intervalSec));
            while (std::chrono::steady_clock::now() < sleepEnd) {
                rt.capture.checkStop();
                const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
                    sleepEnd - std::chrono::steady_clock::now()).count();
                if (rem <= 0) break;
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(std::min<int64_t>(rem, 50)));
            }
        }
    }
    rt.progress.enter(IR_RECIPE_STAGE_CAPTURE, &rb.ref);
    rt.burstRunsSummary.push_back({
        {"id", rb.ref.id},
        {"csv_file", rb.csvFile},
        {"iterations", iterationsDone}
    });
    return iterationsDone;
}

std::exception_ptr performSafetyCleanup(RuntimeState &rt, std::exception_ptr failure)
{
    RecipeCleanupFailure cleanup;
    if (rt.sourceOpened)
        cleanup.attempt(nullptr, [&] { rt.capture.close(); });
    for (auto it = rt.powerToSwitchOff.rbegin(); it != rt.powerToSwitchOff.rend(); ++it) {
        const auto &step = **it;
        const auto itDev = rt.devices.find(step.device);
        if (itDev == rt.devices.end() || !itDev->second)
            continue;
        cleanup.attempt(&step.ref, [&] {
            char error[1024]{};
            IRPowerReading reading{};
            checkStatus(IR_SetPowerOutput(itDev->second, step.channel, 0, &reading,
                                          error, sizeof error), error);
        });
    }
    if (rt.lease.has_value()) {
        for (const auto &sb : rt.recipe.standbyBlackbodies) {
            const auto itDev = rt.devices.find(sb.device);
            if (itDev == rt.devices.end() || !itDev->second)
                continue;
            cleanup.attempt(nullptr, [&] {
                char error[1024]{};
                double readback{};
                checkStatus(IR_SetBlackbody(itDev->second, sb.targetC, &readback,
                                            error, sizeof error), error);
            });
        }
        std::vector<std::string> uniqueStandbyOvens = rt.ovensToStandby;
        for (const auto &ov : rt.recipe.standbyOvens) {
            if (std::find(uniqueStandbyOvens.begin(), uniqueStandbyOvens.end(), ov) == uniqueStandbyOvens.end())
                uniqueStandbyOvens.push_back(ov);
        }
        for (auto it = uniqueStandbyOvens.rbegin(); it != uniqueStandbyOvens.rend(); ++it) {
            const auto itDev = rt.devices.find(*it);
            if (itDev == rt.devices.end() || !itDev->second)
                continue;
            cleanup.attempt(nullptr, [&] {
                char error[1024]{};
                checkStatus(IR_StopOven(itDev->second, error, sizeof error), error);
            });
        }
    }
    rt.lease.reset();
    for (auto it = rt.devices.rbegin(); it != rt.devices.rend(); ++it) {
        if (!it->second) continue;
        cleanup.attempt(nullptr, [&] {
            char error[1024]{};
            checkStatus(IR_CloseDevice(it->second, error, sizeof error), error);
        });
    }
    rt.progress.result.cleanup_status = cleanup.status;
    if (!failure && cleanup.status != IR_OK) {
        rt.progress.enter(IR_RECIPE_STAGE_CLEANUP, cleanup.step);
        return std::make_exception_ptr(Error(cleanup.status, cleanup.detail));
    }
    return failure;
}

void writeDiagnosticFailureResult(RuntimeState &rt, const std::string &startedUtc,
                                  std::chrono::steady_clock::time_point runStartSteady,
                                  int32_t errCode, const char *errMsg) noexcept
{
    if (errCode == IR_BUSY)
        return;
    try {
        Json metricsJson = Json::object();
        for (const auto &[key, val] : rt.ctx.metrics)
            metricsJson[key] = val;

        const Json record{
            {"schemav", 1},
            {"recipe_id", rt.recipe.id},
            {"recipe", rt.recipe.definition},
            {"run_status", errCode == IR_CANCELLED ? "CANCELLED" : "ERROR"},
            {"error_code", errCode},
            {"error_status", statusName(errCode)},
            {"error_message", errMsg ? errMsg : ""},
            {"failed_stage", rt.progress.result.failed_stage},
            {"failed_stage_name", stageName(rt.progress.result.failed_stage)},
            {"failed_step_index", rt.progress.result.failed_step_index},
            {"backend", backendName(rt.station.backend)},
            {"source_name", rt.station.source},
            {"width", rt.ctx.geometry.width},
            {"height", rt.ctx.geometry.height},
            {"valid_bits", rt.ctx.geometry.valid_bits},
            {"completed_frames", rt.progress.result.completed_frames},
            {"cleanup_status", rt.progress.result.cleanup_status},
            {"cleanup_status_name", statusName(rt.progress.result.cleanup_status)},
            {"raw_saved", false},
            {"saved_batches", Json::array()},
            {"burst_runs", rt.burstRunsSummary},
            {"step_records", rt.stepRecords},
            {"total_duration_ms", elapsedMsSince(runStartSteady)},
            {"station_version", IR_StationVersion()},
            {"analysis_version", IR_AnalysisVersion()},
            {"source_commit", ir::station::sourceCommit()},
            {"started_utc", startedUtc},
            {"finished_utc", utcTimestampNow()},
            {"conditions", rt.readbacks.conditions},
            {"instrument_readings", rt.readbacks.instruments},
            {"metrics", std::move(metricsJson)},
            {"limits", Json::array()}
        };
        if (!rt.destinationCreated) {
            std::error_code ec;
            fs::create_directory(rt.destination, ec);
            if (ec) return;
            rt.destinationCreated = true;
        }
        writeTextFileAtomicOrDirect(rt.destination / "result.json", record.dump(2) + '\n');
    } catch (...) {
    }
}

void archiveRecipeResult(RuntimeState &rt, const std::string &startedUtc,
                           std::chrono::steady_clock::time_point runStartSteady)
{
    rt.progress.enter(IR_RECIPE_STAGE_ARCHIVE);
    rt.capture.checkStop();

    bool limitsPass = true;
    int32_t passCount = 0;
    int32_t ngCount = 0;
    Json evaluatedLimits = Json::array();
    for (const auto &limit : rt.recipe.limits) {
        const auto itVal = rt.ctx.scalarValues.find(limit.metric);
        require(itVal != rt.ctx.scalarValues.end(), IR_COMPUTATION_FAILED,
                "Limit metric was not computed: " + limit.metric);
        const double actual = itVal->second;
        const bool pass = limit.isMaximum ? (actual <= limit.threshold) : (actual >= limit.threshold);
        const double margin = limit.isMaximum ? (limit.threshold - actual) : (actual - limit.threshold);
        limitsPass &= pass;
        if (pass) ++passCount;
        else ++ngCount;
        evaluatedLimits.push_back({
            {"metric", limit.metric},
            {"operator", limit.isMaximum ? "max" : "min"},
            {"value", limit.threshold},
            {"unit", limit.unit},
            {"actual", actual},
            {"margin", margin},
            {"result", pass ? "PASS" : "NG"}
        });
    }
    const char *runStatus = rt.recipe.limits.empty() ? "COMPLETED" : (limitsPass ? "PASS" : "NG");

    Json metricsJson = Json::object();
    for (const auto &[key, val] : rt.ctx.metrics)
        metricsJson[key] = val;

    Json savedBatchesMeta = Json::array();
    std::vector<SavedBatchRef> batchRefs;
    batchRefs.reserve(rt.savedSnapshots.size());
    for (const auto &snap : rt.savedSnapshots) {
        batchRefs.push_back({snap.subdir, snap.batch.geometry,
                             &snap.batch.pixels, &snap.batch.stamps});
        savedBatchesMeta.push_back({
            {"input", snap.inputSlot},
            {"subdir", snap.subdir},
            {"frames", snap.batch.stamps.size()},
            {"width", snap.batch.geometry.width},
            {"height", snap.batch.geometry.height}
        });
    }

    const Json record{
        {"schemav", 1},
        {"recipe_id", rt.recipe.id},
        {"recipe", rt.recipe.definition},
        {"run_status", runStatus},
        {"backend", backendName(rt.station.backend)},
        {"source_name", rt.station.source},
        {"width", rt.ctx.geometry.width},
        {"height", rt.ctx.geometry.height},
        {"valid_bits", rt.ctx.geometry.valid_bits},
        {"completed_frames", rt.progress.result.completed_frames},
        {"cleanup_status", rt.progress.result.cleanup_status},
        {"cleanup_status_name", statusName(rt.progress.result.cleanup_status)},
        {"raw_saved", !rt.savedSnapshots.empty()},
        {"saved_batches", std::move(savedBatchesMeta)},
        {"burst_runs", std::move(rt.burstRunsSummary)},
        {"step_records", std::move(rt.stepRecords)},
        {"total_duration_ms", elapsedMsSince(runStartSteady)},
        {"station_version", IR_StationVersion()},
        {"analysis_version", IR_AnalysisVersion()},
        {"source_commit", ir::station::sourceCommit()},
        {"started_utc", startedUtc},
        {"finished_utc", utcTimestampNow()},
        {"conditions", rt.readbacks.conditions},
        {"instrument_readings", rt.readbacks.instruments},
        {"metrics", std::move(metricsJson)},
        {"limits", std::move(evaluatedLimits)},
        {"limits_summary", {
            {"total", passCount + ngCount},
            {"pass", passCount},
            {"ng", ngCount}
        }}
    };
    const auto text = record.dump(2) + '\n';

    if (!rt.destinationCreated) {
        saveRecipeArtifacts(rt.destination, batchRefs, text);
    } else {
        for (const auto &ref : batchRefs) {
            const auto targetDir = ref.subdir.empty()
                ? rt.destination
                : (rt.destination / pathFromUtf8(ref.subdir));
            writeFrameBatchFiles(targetDir, ref.geometry, *ref.frames, *ref.stamps);
        }
        writeTextFileAtomicOrDirect(rt.destination / "result.json", text);
    }
    rt.progress.enter(IR_RECIPE_STAGE_NONE);
}

} // namespace

void runRecipe(ir::acquisition::Capture &capture, const StationConfig &station,
                 const RecipeConfig &recipe, const char *directory, IRRecipeResult &result,
                 const IRRecipeRunOptions *options)
{
    require(directory && *directory, IR_BAD_ARGUMENT, "Specify a new output directory");
    const auto destination = pathFromUtf8(directory);
    checkDestination(destination);
    capture.checkStop();
    const auto startedUtc = utcTimestampNow();
    const auto runStartSteady = std::chrono::steady_clock::now();

    RecipeProgress progress{result};
    RuntimeState rt{capture, station, recipe, destination, progress};
    std::exception_ptr failure;

    try {
        try {
            preflightRecipe(rt);
            openAndLeaseDevices(rt);

            for (const auto &step : recipe.steps) {
                const TopStepInfo sInfo = topStepInfo(step);
                const std::string stepStartUtc = utcTimestampNow();
                const auto stepStartSteady = std::chrono::steady_clock::now();
                emitRecipeStepEvent(options, IR_STEP_EVENT_STARTED, *sInfo.ref,
                                    sInfo.initialStage, IR_OK, 0.0, sInfo.typeName);
                int32_t burstIterationsForRecord = -1;
                try {
                    if (const auto *sr = std::get_if<SaveRawStep>(&step)) {
                        rt.progress.enter(IR_RECIPE_STAGE_ARCHIVE, &sr->ref);
                        rt.capture.checkStop();
                        const auto &batch = rt.ctx.batches.at(sr->inputSlot);
                        const uint64_t snapshotBytes = batchStorageBytes(
                            batch.geometry, static_cast<int32_t>(batch.stamps.size()),
                            station.maxBatchBytes);
                        const uint64_t nextSavedSnapshotBytes = addBytes(
                            rt.savedSnapshotBytes, snapshotBytes);
                        const uint64_t requiredStorage = addBytes(
                            addBytes(rt.capture.bufferedBytes(), rt.ctx.retainedBytes()),
                            nextSavedSnapshotBytes);
                        requireWithinMemoryLimit(requiredStorage, station.maxInflightBytes);
                        rt.savedSnapshots.push_back({sr->inputSlot, sr->subdir, batch});
                        rt.savedSnapshotBytes = nextSavedSnapshotBytes;
                    } else if (const auto *rb = std::get_if<RepeatBurstStep>(&step)) {
                        burstIterationsForRecord = executeRepeatBurstStep(rt, *rb);
                    } else {
                        std::visit([&](const auto &s) { executeCommonStep(rt, s); }, step);
                    }
                } catch (const Error &e) {
                    emitRecipeStepEvent(options, IR_STEP_EVENT_FAILED, *sInfo.ref,
                                        result.failed_stage, e.code, elapsedMsSince(stepStartSteady),
                                        sInfo.typeName, e.what());
                    throw;
                } catch (const std::exception &e) {
                    emitRecipeStepEvent(options, IR_STEP_EVENT_FAILED, *sInfo.ref,
                                        result.failed_stage, exceptionToStatus(e),
                                        elapsedMsSince(stepStartSteady), sInfo.typeName, e.what());
                    throw;
                } catch (...) {
                    emitRecipeStepEvent(options, IR_STEP_EVENT_FAILED, *sInfo.ref,
                                        result.failed_stage, IR_COMPUTATION_FAILED,
                                        elapsedMsSince(stepStartSteady), sInfo.typeName, "Unknown error");
                    throw;
                }
                const double durationMs = elapsedMsSince(stepStartSteady);
                const int32_t finalStage = sInfo.initialStage;
                Json rec{
                    {"index", sInfo.ref->index},
                    {"id", sInfo.ref->id},
                    {"type", sInfo.typeName},
                    {"stage", finalStage},
                    {"stage_name", stageName(finalStage)},
                    {"started_utc", stepStartUtc},
                    {"finished_utc", utcTimestampNow()},
                    {"duration_ms", durationMs},
                    {"status", "OK"}
                };
                if (burstIterationsForRecord >= 0)
                    rec["iterations"] = burstIterationsForRecord;
                rt.stepRecords.push_back(std::move(rec));
                emitRecipeStepEvent(options, IR_STEP_EVENT_COMPLETED, *sInfo.ref,
                                    finalStage, IR_OK, durationMs, sInfo.typeName);
            }
        } catch (...) {
            failure = std::current_exception();
        }

        failure = performSafetyCleanup(rt, failure);
        if (failure) {
            try {
                std::rethrow_exception(failure);
            } catch (const Error &e) {
                writeDiagnosticFailureResult(rt, startedUtc, runStartSteady, e.code, e.what());
                throw;
            } catch (const std::exception &e) {
                writeDiagnosticFailureResult(rt, startedUtc, runStartSteady, exceptionToStatus(e), e.what());
                throw;
            } catch (...) {
                writeDiagnosticFailureResult(rt, startedUtc, runStartSteady, IR_COMPUTATION_FAILED, "Unknown error");
                throw;
            }
        }
        archiveRecipeResult(rt, startedUtc, runStartSteady);
    } catch (const Error &error) {
        if (progress.step)
            throw Error(error.code, "Step '" + progress.step->id + "' failed: " + error.what());
        throw;
    }
}

} // namespace ir::procedures::recipe
