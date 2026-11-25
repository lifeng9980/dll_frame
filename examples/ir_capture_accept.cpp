#include "IR_analysis.h"
#include "IR_device.h"
#include "IR_station.h"
#include "posix_stop_signal.h"
#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <csignal>
#endif

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

std::string pathUtf8(const fs::path &path)
{
    const auto value = path.u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}

fs::path pathFromUtf8(const std::string &value)
{
#ifdef _WIN32
    return fs::path(std::u8string(value.begin(), value.end()));
#else
    return fs::path(value);
#endif
}

#ifdef _WIN32
std::string wideToUtf8(const wchar_t *text, size_t len)
{
    if (!text || len == 0)
        return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text,
                                          static_cast<int>(len), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
        throw std::runtime_error("Cannot convert command-line argument to UTF-8");
    std::string out(static_cast<size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, static_cast<int>(len),
                            out.data(), bytes, nullptr, nullptr) != bytes)
        throw std::runtime_error("Cannot convert command-line argument to UTF-8");
    return out;
}
#endif

using ir::examples::StopSignalGuard;

struct FramesGuard {
    bool open = false;
    bool running = false;
    ~FramesGuard()
    {
        char err[512]{};
        if (running) {
            IR_StopFrames(err, sizeof err);
            running = false;
        }
        if (open) {
            IR_CloseFrames(err, sizeof err);
            open = false;
        }
    }
};

struct Arguments {
    bool listMode = false;
    std::string backendName;
    std::string source;
    int32_t frames = 0;
    int32_t timeoutMs = -1;
    int32_t validBits = 0;
    int32_t queueCapacity = 0;
    int32_t cycles = 1;
    int32_t flushBetween = -1;
    int32_t previewMs = 0;
    fs::path reportPath;
};

int32_t parseIntArg(const std::string &flag, const std::string &val, int32_t low, int32_t high)
{
    size_t idx = 0;
    long long parsed = 0;
    try {
        parsed = std::stoll(val, &idx, 10);
    } catch (...) {
        throw std::runtime_error(flag + " requires an integer");
    }
    if (idx != val.size() || parsed < low || parsed > high)
        throw std::runtime_error(flag + " is out of range");
    return static_cast<int32_t>(parsed);
}

int32_t parseBackend(const std::string &name)
{
    if (name == "directshow") return IR_DIRECTSHOW;
    if (name == "raw_replay") return IR_RAW_REPLAY;
    if (name == "v4l2") return IR_V4L2;
    throw std::runtime_error("Unsupported backend '" + name + "' (use raw_replay, directshow, or v4l2)");
}

void printUsage()
{
    std::puts(
        "Usage:\n"
        "  ir_capture_accept --list\n"
        "  ir_capture_accept --backend <raw_replay|directshow|v4l2> --source <path-or-name>\n"
        "                    --frames <N> [--timeout-ms <ms>] [--valid-bits <bits>]\n"
        "                    [--queue-capacity <2..4096>] [--cycles <K>] [--flush-between <F>]\n"
        "                    [--preview-ms <ms>] [--report <path.json>]");
}

Arguments parseArguments(const std::vector<std::string> &values)
{
    Arguments args;
    for (size_t i = 0; i < values.size(); ++i) {
        const auto &v = values[i];
        auto needVal = [&](const char *flag) -> const std::string & {
            if (i + 1 >= values.size())
                throw std::runtime_error(std::string(flag) + " requires a value");
            return values[++i];
        };
        if (v == "--list") {
            args.listMode = true;
        } else if (v == "--backend") {
            args.backendName = needVal("--backend");
        } else if (v == "--source") {
            args.source = needVal("--source");
        } else if (v == "--frames") {
            args.frames = parseIntArg("--frames", needVal("--frames"), 1, INT32_MAX);
        } else if (v == "--timeout-ms") {
            args.timeoutMs = parseIntArg("--timeout-ms", needVal("--timeout-ms"), 0, 86400000);
        } else if (v == "--valid-bits") {
            args.validBits = parseIntArg("--valid-bits", needVal("--valid-bits"), 0, 16);
        } else if (v == "--queue-capacity") {
            args.queueCapacity = parseIntArg("--queue-capacity", needVal("--queue-capacity"), 2, 4096);
        } else if (v == "--cycles") {
            args.cycles = parseIntArg("--cycles", needVal("--cycles"), 1, INT32_MAX);
        } else if (v == "--flush-between") {
            args.flushBetween = parseIntArg("--flush-between", needVal("--flush-between"), -1, 100000);
        } else if (v == "--preview-ms") {
            args.previewMs = parseIntArg("--preview-ms", needVal("--preview-ms"), 0, 60000);
        } else if (v == "--report") {
            args.reportPath = pathFromUtf8(needVal("--report"));
        } else {
            throw std::runtime_error("Unknown argument: " + v);
        }
    }
    if (!args.listMode) {
        if (args.backendName.empty() || args.source.empty() || args.frames <= 0)
            throw std::runtime_error("Specify --backend, --source, and --frames (or --list)");
        if (args.timeoutMs < 0)
            args.timeoutMs = (args.backendName == "raw_replay") ? 0 : 2000;
    }
    return args;
}

template <typename Fn>
std::string callJsonList(Fn &&fn)
{
    int32_t cap = 1024;
    for (int attempt = 0; attempt < 4; ++attempt) {
        std::vector<char> buf(static_cast<size_t>(cap), '\0');
        int32_t req = 0;
        char err[1024]{};
        const int32_t st = fn(buf.data(), cap, &req, err, static_cast<int32_t>(sizeof err));
        if (st == IR_OK)
            return std::string(buf.data());
        if (st == IR_BUFFER_TOO_SMALL && req > cap) {
            cap = req;
            continue;
        }
        throw std::runtime_error(err[0] ? err : ("Discovery error status=" + std::to_string(st)));
    }
    throw std::runtime_error("Discovery buffer did not converge");
}

int runListMode()
{
#ifdef _WIN32
    const int32_t platformBackend = IR_DIRECTSHOW;
#else
    const int32_t platformBackend = IR_V4L2;
#endif
    const std::string devsJson = callJsonList([&](char *buf, int32_t cap, int32_t *req, char *err, int32_t errBytes) {
        return IR_ListCaptureDevices(platformBackend, buf, cap, req, err, errBytes);
    });
    const std::string portsJson = callJsonList([&](char *buf, int32_t cap, int32_t *req, char *err, int32_t errBytes) {
        return IR_ListSerialPorts(buf, cap, req, err, errBytes);
    });
    const Json report = {
        {"station_version", IR_StationVersion()},
        {"capture_devices", Json::parse(devsJson)},
        {"serial_ports", Json::parse(portsJson)}
    };
    std::cout << report.dump(2) << '\n';
    return 0;
}

int runAcceptMode(const Arguments &args)
{
    const int32_t backend = parseBackend(args.backendName);
    std::string source = args.source;
    if (backend == IR_RAW_REPLAY)
        source = pathUtf8(fs::absolute(pathFromUtf8(args.source)).lexically_normal());

    char err[2048]{};
    StopSignalGuard stopGuard;
    FramesGuard guard;
    IRGeometry geom{};
    int32_t st = IR_OpenFrames(backend, source.c_str(), args.validBits, &geom, err, sizeof err);
    if (st != IR_OK) {
        std::fprintf(stderr, "IR_OpenFrames status=%d: %s\n", st, err);
        return st;
    }
    guard.open = true;

    if (args.queueCapacity > 0) {
        st = IR_SetFrameQueueCapacity(args.queueCapacity, err, sizeof err);
        if (st != IR_OK) {
            std::fprintf(stderr, "IR_SetFrameQueueCapacity status=%d: %s\n", st, err);
            return st;
        }
    }

    const int64_t framePixels = int64_t(geom.width) * geom.height;
    if (framePixels <= 0 || int64_t(args.frames) > INT64_MAX / framePixels) {
        std::fprintf(stderr, "Invalid geometry or batch frame count\n");
        return IR_BAD_ARGUMENT;
    }
    const int64_t batchElements = framePixels * args.frames;
    std::vector<uint16_t> pixels(static_cast<size_t>(batchElements), 0);
    std::vector<IRFrameStamp> stamps(static_cast<size_t>(args.frames));

    std::atomic<bool> previewStop{false};
    std::atomic<int64_t> previewPolls{0};
    std::atomic<int64_t> previewHits{0};
    std::atomic<int64_t> previewErrors{0};
    std::string previewErrorText;
    std::thread previewThread;
    if (args.previewMs > 0) {
        previewThread = std::thread([&] {
            std::vector<uint16_t> prevBuf(static_cast<size_t>(framePixels), 0);
            while (!previewStop.load(std::memory_order_acquire)) {
                previewPolls.fetch_add(1, std::memory_order_relaxed);
                IRGeometry pg{};
                IRFrameStamp ps{};
                int32_t hasFrame = 0;
                char pErr[512]{};
                int32_t pst = IR_TryReadLatestFrame(
                    prevBuf.data(), static_cast<int64_t>(prevBuf.size()),
                    &pg, &ps, &hasFrame, pErr, sizeof pErr);
                if (pst == IR_BUFFER_TOO_SMALL && pg.width > 0 && pg.height > 0) {
                    const int64_t need = int64_t(pg.width) * pg.height;
                    prevBuf.resize(static_cast<size_t>(need), 0);
                    pst = IR_TryReadLatestFrame(
                        prevBuf.data(), static_cast<int64_t>(prevBuf.size()),
                        &pg, &ps, &hasFrame, pErr, sizeof pErr);
                }
                if (pst != IR_OK) {
                    if (previewErrors.fetch_add(1, std::memory_order_relaxed) == 0)
                        previewErrorText = pErr[0] ? pErr : ("status=" + std::to_string(pst));
                } else if (hasFrame != 0) {
                    previewHits.fetch_add(1, std::memory_order_relaxed);
                }
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(args.previewMs);
                while (!previewStop.load(std::memory_order_acquire) &&
                       std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
        });
    }

    const auto stopPreview = [&] {
        previewStop.store(true, std::memory_order_release);
        if (previewThread.joinable())
            previewThread.join();
    };

    const bool continuousStream = (args.flushBetween >= 0);
    int32_t cyclesCompleted = 0;
    int64_t totalFrames = 0;
    int64_t invertedCount = 0;
    int32_t lastCompleted = 0;

    const auto runStart = std::chrono::steady_clock::now();
    {
        if (continuousStream) {
            st = IR_StartFrames(err, sizeof err);
            if (st != IR_OK) {
                stopPreview();
                std::fprintf(stderr, "IR_StartFrames status=%d: %s\n", st, err);
                return st;
            }
            guard.running = true;
        }

        for (int32_t c = 0; c < args.cycles; ++c) {
            if (!continuousStream) {
                st = IR_StartFrames(err, sizeof err);
                if (st != IR_OK) {
                    stopPreview();
                    std::fprintf(stderr, "IR_StartFrames cycle=%d status=%d: %s\n", c, st, err);
                    return st;
                }
                guard.running = true;
            } else if (c > 0) {
                int32_t discarded = 0;
                st = IR_FlushFrames(args.flushBetween, args.timeoutMs, &discarded, err, sizeof err);
                if (st != IR_OK) {
                    stopPreview();
                    std::fprintf(stderr, "IR_FlushFrames cycle=%d status=%d: %s\n", c, st, err);
                    return st;
                }
            }

            int32_t completed = 0;
            st = IR_ReadFrames(args.frames, args.timeoutMs, pixels.data(), batchElements,
                               stamps.data(), args.frames, &completed, err, sizeof err);
            totalFrames += completed;
            lastCompleted = completed;
            for (int32_t i = 0; i < completed; ++i) {
                if ((stamps[static_cast<size_t>(i)].fields & IR_PIXELS_INVERTED) != 0)
                    ++invertedCount;
            }
            if (st != IR_OK) {
                stopPreview();
                std::fprintf(stderr, "IR_ReadFrames cycle=%d completed=%d status=%d: %s\n",
                             c, completed, st, err);
                return st;
            }

            if (!continuousStream) {
                st = IR_StopFrames(err, sizeof err);
                guard.running = false;
                if (st != IR_OK) {
                    stopPreview();
                    std::fprintf(stderr, "IR_StopFrames cycle=%d status=%d: %s\n", c, st, err);
                    return st;
                }
            }
            ++cyclesCompleted;
        }

        if (continuousStream) {
            st = IR_StopFrames(err, sizeof err);
            guard.running = false;
            if (st != IR_OK) {
                stopPreview();
                std::fprintf(stderr, "IR_StopFrames status=%d: %s\n", st, err);
                return st;
            }
        }
    }
    stopPreview();
    const double elapsedSec = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - runStart).count();

    IRCaptureStats stats{};
    st = IR_GetCaptureStats(&stats, err, sizeof err);
    if (st != IR_OK) {
        std::fprintf(stderr, "IR_GetCaptureStats status=%d: %s\n", st, err);
        return st;
    }

    st = IR_CloseFrames(err, sizeof err);
    guard.open = false;
    if (st != IR_OK) {
        std::fprintf(stderr, "IR_CloseFrames status=%d: %s\n", st, err);
        return st;
    }

    bool hasMetrics = false;
    double meanDn = 0.0;
    double stdDn = 0.0;
    if (lastCompleted >= 2) {
        std::vector<double> meanMap(static_cast<size_t>(framePixels), 0.0);
        std::vector<double> stdMap(static_cast<size_t>(framePixels), 0.0);
        double summary[2]{};
        const int32_t statSt = IR_FrameStatisticsU16(
            pixels.data(), batchElements, lastCompleted, geom.height, geom.width, 0,
            meanMap.data(), stdMap.data(), framePixels, summary, 2, err, sizeof err);
        if (statSt != IR_OK) {
            std::fprintf(stderr, "IR_FrameStatisticsU16 status=%d: %s\n", statSt, err);
            return statSt;
        }
        meanDn = summary[0];
        stdDn = summary[1];
        hasMetrics = true;
    }

    const int64_t pPolls = previewPolls.load();
    const int64_t pHits = previewHits.load();
    const int64_t pErrors = previewErrors.load();
    if (pErrors > 0) {
        std::fprintf(stderr, "Preview polling recorded %lld error(s), first: %s\n",
                     static_cast<long long>(pErrors), previewErrorText.c_str());
        return IR_IO_ERROR;
    }

    const uint64_t firstSeq = lastCompleted > 0 ? stamps.front().sequence : 0;
    const uint64_t lastSeq = lastCompleted > 0 ? stamps[static_cast<size_t>(lastCompleted - 1)].sequence : 0;

    std::cout << "status=0\n"
              << "width=" << geom.width << " height=" << geom.height << " valid_bits=" << geom.valid_bits << '\n'
              << "cycles_completed=" << cyclesCompleted << " total_frames=" << totalFrames
              << " elapsed_s=" << std::setprecision(6) << elapsedSec
              << " estimated_fps=" << std::setprecision(3) << stats.estimated_fps << '\n'
              << "frames_received=" << stats.frames_received
              << " frames_delivered=" << stats.frames_delivered
              << " frames_dropped=" << stats.frames_dropped
              << " preview_frames=" << stats.preview_frames << '\n'
              << "preview_polls=" << pPolls << " preview_hits=" << pHits
              << " preview_errors=" << pErrors << '\n';
    if (hasMetrics) {
        std::cout << std::setprecision(15)
                  << "mean_dn=" << meanDn << '\n'
                  << "pop_std_dn=" << stdDn << '\n';
    }

    if (!args.reportPath.empty()) {
        Json report = {
            {"station_version", IR_StationVersion()},
            {"analysis_version", IR_AnalysisVersion()},
            {"backend", args.backendName},
            {"source", source},
            {"geometry", {{"width", geom.width}, {"height", geom.height}, {"valid_bits", geom.valid_bits}}},
            {"cycles_completed", cyclesCompleted},
            {"total_frames", totalFrames},
            {"elapsed_s", elapsedSec},
            {"continuous_stream", continuousStream},
            {"flush_between", args.flushBetween},
            {"inverted_frames", invertedCount},
            {"last_cycle_first_sequence", firstSeq},
            {"last_cycle_last_sequence", lastSeq},
            {"capture_stats", {
                {"frames_received", stats.frames_received},
                {"frames_delivered", stats.frames_delivered},
                {"frames_dropped", stats.frames_dropped},
                {"preview_frames", stats.preview_frames},
                {"queue_capacity", stats.queue_capacity},
                {"queue_depth", stats.queue_depth},
                {"estimated_fps", stats.estimated_fps}
            }},
            {"preview", {
                {"polls", pPolls},
                {"hits", pHits},
                {"errors", pErrors}
            }}
        };
        if (hasMetrics) {
            report["last_cycle_metrics"] = {
                {"mean_dn", meanDn},
                {"pop_std_dn", stdDn}
            };
        }
        const auto parent = args.reportPath.parent_path();
        if (!parent.empty())
            fs::create_directories(parent);
        std::ofstream ofs(args.reportPath, std::ios::binary | std::ios::out);
        if (!ofs)
            throw std::runtime_error("Cannot open --report path for writing: " + pathUtf8(args.reportPath));
        ofs << report.dump(2) << '\n';
        if (!ofs.good())
            throw std::runtime_error("Failed writing --report file");
    }
    return 0;
}

int execute(const std::vector<std::string> &values)
{
    try {
        const auto args = parseArguments(values);
        if (args.listMode)
            return runListMode();
        return runAcceptMode(args);
    } catch (const std::exception &ex) {
        std::fprintf(stderr, "ir_capture_accept: %s\n", ex.what());
        return 2;
    }
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t **argv)
{
    std::vector<std::string> values;
    for (int i = 1; i < argc; ++i)
        values.push_back(wideToUtf8(argv[i], std::wcslen(argv[i])));
    if (values.empty() || (values.size() == 1 && values.front() == "--help")) {
        printUsage();
        return 0;
    }
    return execute(values);
}
#else
int main(int argc, char **argv)
{
    std::vector<std::string> values;
    for (int i = 1; i < argc; ++i)
        values.emplace_back(argv[i]);
    if (values.empty() || (values.size() == 1 && values.front() == "--help")) {
        printUsage();
        return 0;
    }
    return execute(values);
}
#endif
