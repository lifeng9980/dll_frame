#include "IR_recipe.h"
#include "IR_analysis.h"
#include "IR_device.h"
#include "IR_station.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <exception>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

int gFailures = 0;
fs::path gTempRoot;

void check(bool cond, const char *label, int line)
{
    if (!cond) {
        std::cerr << "FAIL line " << line << ": " << label << '\n';
        ++gFailures;
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

std::string utf8(const fs::path &p)
{
    const auto u = p.u8string();
    return {reinterpret_cast<const char *>(u.data()), u.size()};
}

std::string readText(const fs::path &p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

class TempDirGuard {
public:
    explicit TempDirGuard(const std::string &name)
    {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        dir_ = gTempRoot / ("ir_recipe_v2_test_" + name + "_" + std::to_string(suffix));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_);
    }
    ~TempDirGuard()
    {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    const fs::path &path() const { return dir_; }

private:
    fs::path dir_;
};

fs::path testTempRoot()
{
#ifdef _WIN32
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                             static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size())
        throw std::runtime_error("Cannot locate recipe_test executable");
    executable.resize(length);
    return fs::path(executable).parent_path().parent_path().parent_path() / "ctest-tmp";
#else
    return fs::temp_directory_path();
#endif
}

template <typename Test>
bool runTestCase(const char *name, Test &&test)
{
    try {
        test();
        return true;
    } catch (const std::exception &e) {
        std::cerr << name << " threw an exception: " << e.what() << '\n';
        ++gFailures;
    } catch (...) {
        std::cerr << name << " threw an unknown exception\n";
        ++gFailures;
    }
    return false;
}

void buildDriftFixture(const fs::path &dir, int32_t rows, int32_t cols,
                       int32_t calibFrames, int32_t burstFrames, int32_t bursts)
{
    std::vector<uint16_t> all;
    auto gainAt = [&](int32_t c) { return 0.85 + 0.30 * (double(c) / double(cols - 1)); };
    auto push = [&](auto fn) {
        for (int32_t r = 0; r < rows; ++r)
            for (int32_t c = 0; c < cols; ++c)
                all.push_back(fn(r, c));
    };

    // Cold T0 (4 frames)
    for (int f = 0; f < calibFrames; ++f) {
        const double tn = (f % 2 == 0) ? -4.0 : 4.0;
        push([&](int32_t r, int32_t c) {
            return static_cast<uint16_t>(std::llround(30000.0 + gainAt(c) * 100.0 + 40.0 * r + tn));
        });
    }
    // Garbage transition frame discarded by flush_frames
    push([](int32_t, int32_t) { return uint16_t{65000}; });
    // Hot T1 (4 frames)
    for (int f = 0; f < calibFrames; ++f) {
        const double tn = (f % 2 == 0) ? -4.0 : 4.0;
        push([&](int32_t r, int32_t c) {
            return static_cast<uint16_t>(std::llround(30000.0 + gainAt(c) * 1100.0 + 40.0 * r + tn));
        });
    }
    // Bursts preceded by 1 garbage frame each
    for (int b = 0; b < bursts; ++b) {
        push([](int32_t, int32_t) { return uint16_t{64000}; });
        const double sig = 600.0 + 100.0 * b;
        for (int f = 0; f < burstFrames; ++f) {
            const double tn = (f % 2 == 0) ? -4.0 : 4.0;
            push([&](int32_t r, int32_t c) {
                return static_cast<uint16_t>(std::llround(30000.0 + gainAt(c) * sig + 40.0 * r + tn));
            });
        }
    }

    const int32_t nPix = rows * cols;
    const int32_t totalFrames = int32_t(all.size() / size_t(nPix));
    {
        std::ofstream raw(dir / "frames.raw", std::ios::binary);
        raw.write(reinterpret_cast<const char *>(all.data()), std::streamsize(all.size() * sizeof(uint16_t)));
    }
    {
        std::ofstream csv(dir / "frames.csv", std::ios::binary);
        csv << "frame_index,width,height,stride_bytes,pixel_format,bit_depth,endianness\n";
        for (int32_t i = 0; i < totalFrames; ++i)
            csv << i << "," << cols << "," << rows << "," << cols * 2 << ",RAW16_LE,16,little\n";
    }
}

void buildU32ReplayFixture(const fs::path &dir)
{
    const std::vector<uint32_t> pixels{
        70000, 71000, 72000, 73000,
        70002, 71002, 72002, 73002
    };
    {
        std::ofstream raw(dir / "frames.raw", std::ios::binary);
        raw.write(reinterpret_cast<const char *>(pixels.data()),
                  std::streamsize(pixels.size() * sizeof(uint32_t)));
    }
    {
        std::ofstream csv(dir / "frames.csv", std::ios::binary);
        csv << "frame_index,width,height,stride_bytes,pixel_format,bit_depth,endianness,storage_bits\n"
            << "0,2,2,8,RAW32_LE,20,little,32\n"
            << "1,2,2,8,RAW32_LE,20,little,32\n";
    }
}

void buildPaddedU32ReplayFixture(const fs::path &dir)
{
    const uint32_t pixels[]{70000, 71000, 72000, 73000};
    std::array<uint8_t, 24> bytes{};
    for (int row = 0; row < 2; ++row)
        for (int col = 0; col < 2; ++col) {
            const auto value = pixels[row * 2 + col];
            const size_t offset = size_t(row) * 12 + size_t(col) * 4;
            for (int byte = 0; byte < 4; ++byte)
                bytes[offset + size_t(byte)] = uint8_t(value >> (byte * 8));
        }
    {
        std::ofstream raw(dir / "frames.raw", std::ios::binary);
        raw.write(reinterpret_cast<const char *>(bytes.data()),
                  std::streamsize(bytes.size()));
    }
    {
        std::ofstream csv(dir / "frames.csv", std::ios::binary);
        csv << "frame_index,width,height,stride_bytes,pixel_format,bit_depth,endianness,storage_bits\n"
            << "0,2,2,12,RAW32_LE,20,little,32\n";
    }
}

Json makeStation(const fs::path &sourceDir, int32_t queueCap = 64)
{
    return {
        {"schemav", 1},
        {"acquisition", {
            {"backend", "raw_replay"},
            {"source", utf8(sourceDir)},
            {"valid_bits", 16},
            {"queue_capacity", queueCap}
        }}
    };
}

int countRawFiles(const fs::path &dir)
{
    int count = 0;
    if (!fs::exists(dir)) return 0;
    for (const auto &e : fs::recursive_directory_iterator(dir)) {
        if (e.is_regular_file() && e.path().extension() == ".raw")
            ++count;
    }
    return count;
}

void testU32RecipeBatchAndArchive()
{
    TempDirGuard root("u32_recipe");
    const auto fixture = root.path() / "fixture";
    fs::create_directories(fixture);
    buildU32ReplayFixture(fixture);

    Json station = makeStation(fixture, 16);
    station["acquisition"]["valid_bits"] = 20;
    const Json recipe{
        {"schemav", 1},
        {"recipe_id", "u32-replay-batch"},
        {"steps", Json::array({
            {{"id", "capture"}, {"type", "capture"}, {"frames", 2},
             {"frame_timeout_ms", 0}, {"output", "raw"}},
            {{"id", "stats"}, {"type", "analyze"}, {"method", "frame_stats"},
             {"name", "stats"}, {"input", "raw"},
             {"outputs", {{"mean_map", "mean"}, {"std_map", "noise"}}}},
            {{"id", "archive"}, {"type", "save_raw"}, {"input", "raw"},
             {"subdir", "u32"}}
        })}
    };

    const auto stationText = station.dump();
    const auto recipeText = recipe.dump();
    char err[2048]{};
    CHECK(IR_CheckRecipeJson(stationText.c_str(), recipeText.c_str(), err, sizeof err) == IR_OK);

    const auto output = root.path() / "out";
    IRRecipeResult result{};
    const auto status = IR_RunRecipeJson(stationText.c_str(), recipeText.c_str(),
        utf8(output).c_str(), nullptr, &result, err, sizeof err);
    if (status != IR_OK)
        std::cerr << "U32 recipe failed status=" << status << " error=" << err << '\n';
    CHECK(status == IR_OK);
    if (status != IR_OK)
        return;

    const auto record = Json::parse(readText(output / "result.json"));
    CHECK(record.at("data_type") == "U32");
    CHECK(record.at("valid_bits") == 20);
    CHECK(record.at("completed_frames") == 2);
    CHECK(std::abs(record.at("metrics").at("stats.mean_dn").at("value").get<double>() - 71501.0) < 1e-9);
    CHECK(std::abs(record.at("metrics").at("stats.noise_dn").at("value").get<double>() - 1.0) < 1e-9);
    CHECK(record.at("saved_batches").size() == 1);
    CHECK(record.at("saved_batches")[0].at("data_type") == "U32");
    CHECK(record.at("saved_batches")[0].at("valid_bits") == 20);

    const auto rawPath = output / "u32" / "frames.raw";
    std::ifstream raw(rawPath, std::ios::binary);
    std::vector<uint32_t> archived(8);
    raw.read(reinterpret_cast<char *>(archived.data()),
             std::streamsize(archived.size() * sizeof(uint32_t)));
    CHECK(raw.gcount() == std::streamsize(archived.size() * sizeof(uint32_t)));
    CHECK(archived == std::vector<uint32_t>({
        70000, 71000, 72000, 73000,
        70002, 71002, 72002, 73002
    }));
    const auto csv = readText(output / "u32" / "frames.csv");
    CHECK(csv.find(",8,RAW32_LE,20,little,") != std::string::npos);
}

void testPaddedU32ReplayMemoryBudget()
{
    TempDirGuard root("u32_padded_budget");
    const auto fixture = root.path() / "fixture";
    fs::create_directories(fixture);
    buildPaddedU32ReplayFixture(fixture);

    Json station = makeStation(fixture, 2);
    station["acquisition"]["valid_bits"] = 20;
    station["acquisition"]["max_inflight_bytes"] = 223;
    const Json recipe{
        {"schemav", 1},
        {"recipe_id", "u32-padded-budget"},
        {"steps", Json::array({
            {{"id", "capture"}, {"type", "capture"}, {"frames", 1},
             {"frame_timeout_ms", 0}, {"output", "raw"}}
        })}
    };
    char err[2048]{};
    IRRecipeResult result{};
    const auto tooSmall = root.path() / "too_small";
    CHECK(IR_RunRecipeJson(station.dump().c_str(), recipe.dump().c_str(),
        utf8(tooSmall).c_str(), nullptr, &result, err, sizeof err) == IR_BAD_ARGUMENT);

    station["acquisition"]["max_inflight_bytes"] = 224;
    const auto exact = root.path() / "exact";
    const auto status = IR_RunRecipeJson(station.dump().c_str(), recipe.dump().c_str(),
        utf8(exact).c_str(), nullptr, &result, err, sizeof err);
    if (status != IR_OK)
        std::cerr << "Padded U32 recipe failed status=" << status << " error=" << err << '\n';
    CHECK(status == IR_OK);
    if (status == IR_OK) {
        const auto record = Json::parse(readText(exact / "result.json"));
        CHECK(record.at("data_type") == "U32");
        CHECK(record.at("completed_frames") == 1);
    }
}

void testHighBitPhysicalMetricsRejected()
{
    TempDirGuard root("u32_physical_model");
    const auto fixture = root.path() / "fixture";
    fs::create_directories(fixture);
    buildU32ReplayFixture(fixture);

    Json station = makeStation(fixture, 2);
    station["acquisition"]["valid_bits"] = 20;
    const Json recipe{
        {"schemav", 1},
        {"recipe_id", "u32-gb-physical-model"},
        {"steps", Json::array({
            {{"id", "capture"}, {"type", "capture"}, {"frames", 2},
             {"frame_timeout_ms", 0}, {"output", "raw"}},
            {{"id", "stats"}, {"type", "analyze"}, {"method", "frame_stats"},
             {"name", "stats"}, {"input", "raw"},
             {"outputs", {{"mean_map", "mean"}, {"std_map", "noise"}}}},
            {{"id", "gb"}, {"type", "analyze"}, {"method", "gb_evaluate"},
             {"name", "gb"},
             {"inputs", {{"v1_map", "mean"}, {"v2_map", "mean"}, {"vn_map", "noise"}}},
             {"params", {{"t0_c", 20.0}, {"t1_c", 25.0}}}}
        })}
    };
    char err[2048]{};
    CHECK(IR_CheckRecipeJson(station.dump().c_str(), recipe.dump().c_str(),
                             err, sizeof err) == IR_OK);
    IRRecipeResult result{};
    const auto output = root.path() / "out";
    const auto status = IR_RunRecipeJson(station.dump().c_str(), recipe.dump().c_str(),
        utf8(output).c_str(), nullptr, &result, err, sizeof err);
    CHECK(status == IR_UNSUPPORTED);
    CHECK(std::string(err).find("valid_bits is not digital_full") != std::string::npos);
    CHECK(fs::is_regular_file(output / "result.json"));

    const Json burstRecipe{
        {"schemav", 1},
        {"recipe_id", "u32-gjb-burst-physical-model"},
        {"steps", Json::array({
            {{"id", "loop"}, {"type", "repeat_burst"}, {"max_iterations", 1},
             {"steps", Json::array({
                {{"id", "capture"}, {"type", "capture"}, {"frames", 2},
                 {"frame_timeout_ms", 0}, {"output", "raw"}},
                {{"id", "stats"}, {"type", "analyze"}, {"method", "frame_stats"},
                 {"name", "stats"}, {"input", "raw"},
                 {"outputs", {{"mean_map", "mean"}, {"std_map", "noise"}}}},
                {{"id", "burst_metric"}, {"type", "analyze"}, {"method", "gjb_burst"},
                 {"name", "gjb"},
                 {"inputs", {{"mean_map", "mean"}, {"vn_map", "noise"}}},
                 {"params", {{"volts_per_count", 0.1}, {"delta_t_k", 10.0},
                    {"ref_response_count", 1.0}, {"ref_responsivity", 1.0},
                    {"vmax_per_count", 1.0}, {"burst_frame_count", 2},
                    {"lrnu_kernel_size", 1}}}}
             })}}
        })}
    };
    CHECK(IR_CheckRecipeJson(station.dump().c_str(), burstRecipe.dump().c_str(),
                             err, sizeof err) == IR_OK);
    const auto burstOutput = root.path() / "burst_out";
    const auto burstStatus = IR_RunRecipeJson(station.dump().c_str(), burstRecipe.dump().c_str(),
        utf8(burstOutput).c_str(), nullptr, &result, err, sizeof err);
    CHECK(burstStatus == IR_UNSUPPORTED);
    CHECK(std::string(err).find("valid_bits is not digital_full") != std::string::npos);
    CHECK(fs::is_regular_file(burstOutput / "result.json"));
}

std::vector<std::string> splitCsvLine(const std::string &line)
{
    std::vector<std::string> cols;
    std::stringstream ss(line);
    std::string item;
    while (std::getline(ss, item, ','))
        cols.push_back(item);
    return cols;
}

void testTwoPointCalibNoDiskPassAndNg()
{
    TempDirGuard root("calib_nodisk");
    const auto fixture = root.path() / "fixture";
    fs::create_directories(fixture);
    buildDriftFixture(fixture, 16, 16, 4, 8, 1);

    const Json station = makeStation(fixture, 32);
    auto makeCalibRecipe = [](double limitMaxStd) {
        return Json{
            {"schemav", 1},
            {"recipe_id", "two-point-calib-nodisk-v2"},
            {"steps", Json::array({
                {{"id", "cap_t0"}, {"type", "capture"}, {"frames", 4}, {"frame_timeout_ms", 1000}, {"output", "t0"}},
                {{"id", "fl_01"}, {"type", "flush_frames"}, {"discard_frames", 1}, {"timeout_ms", 1000}},
                {{"id", "cap_t1"}, {"type", "capture"}, {"frames", 4}, {"frame_timeout_ms", 1000}, {"output", "t1"}},
                {{"id", "st_t0"}, {"type", "analyze"}, {"method", "frame_stats"}, {"name", "t0_stats"},
                 {"input", "t0"}, {"params", {{"sample_std", 0}}},
                 {"outputs", {{"mean_map", "v2"}, {"std_map", "vn2"}}}},
                {{"id", "st_t1"}, {"type", "analyze"}, {"method", "frame_stats"}, {"name", "t1_stats"},
                 {"input", "t1"}, {"params", {{"sample_std", 0}}},
                 {"outputs", {{"mean_map", "v1"}, {"std_map", "vn1"}}}},
                {{"id", "gjb0"}, {"type", "analyze"}, {"method", "gjb_evaluate"}, {"name", "gjb_init"},
                 {"inputs", {{"v1_map", "v1"}, {"v2_map", "v2"}, {"vn_map", "vn1"}}},
                 {"params", {{"t0_c", 20.0}, {"t1_c", 30.0}, {"k1", 1.5}, {"k2", 1.5}, {"k3", 0.4}, {"k4", 0.4}, {"cycle_upper", 2}}},
                 {"outputs", {{"valid_mask", "valid"}, {"drift_ctx", "dctx"}}}},
                {{"id", "nuc0"}, {"type", "analyze"}, {"method", "two_point_nuc"}, {"name", "nuc_tbl"},
                 {"inputs", {{"v1_map", "v1"}, {"v2_map", "v2"}, {"valid_mask", "valid"}}},
                 {"outputs", {{"k_map", "k"}, {"b_map", "b"}}}},
                {{"id", "inv0"}, {"type", "analyze"}, {"method", "mask_combine"}, {"name", "blind"},
                 {"inputs", {{"mask_a", "valid"}}}, {"params", {{"op", "not"}}},
                 {"outputs", {{"out_mask", "blind_mask"}}}},
                {{"id", "sf0"}, {"type", "analyze"}, {"method", "spatial_filter"}, {"name", "lrnu_t1"},
                 {"inputs", {{"in_map", "v1"}, {"valid_mask", "valid"}}},
                 {"params", {{"filter_kind", "box_mean"}, {"kernel_w", 15}, {"kernel_h", 15}}},
                 {"outputs", {{"low_map", "v1_low"}, {"high_map", "v1_high"}}}}
            })},
            {"limits", Json::array({
                {{"metric", "gjb_init.after_response_nonuniformity_pct"}, {"operator", "max"},
                 {"value", limitMaxStd}, {"unit", "%"}}
            })}
        };
    };

    const auto stText = station.dump();
    char err[2048]{};
    IRRecipeResult rr{};

    // PASS case (synthetic linear gain span 0.85..1.15 has ~9.22% response nonuniformity)
    const auto outPass = root.path() / "out_pass";
    const int32_t runStatus = IR_RunRecipeJson(stText.c_str(), makeCalibRecipe(15.0).dump().c_str(), utf8(outPass).c_str(), nullptr, &rr, err, sizeof err);
    if (runStatus != IR_OK)
        std::cerr << "recipe_test run failed status=" << runStatus << " error=" << err << '\n';
    CHECK(runStatus == IR_OK);
    if (runStatus != IR_OK)
        return;
    CHECK(rr.cleanup_status == IR_OK && rr.completed_frames == 8);
    CHECK(countRawFiles(outPass) == 0);
    const auto recPass = Json::parse(readText(outPass / "result.json"));
    CHECK(recPass.at("schemav") == 1);
    CHECK(recPass.at("run_status") == "PASS");
    const double rnu = recPass.at("metrics").at("gjb_init.after_response_nonuniformity_pct").at("value").get<double>();
    CHECK(rnu > 9.0 && rnu < 9.5);
    CHECK(recPass.at("raw_saved") == false);
    CHECK(recPass.at("metrics").contains("lrnu_t1.low_std_dn"));
    CHECK(recPass.at("metrics").contains("blind.nonzero_count"));
    CHECK(recPass.contains("step_records") && recPass.at("step_records").is_array());
    CHECK(recPass.at("step_records").size() == 9);
    CHECK(recPass.at("step_records")[0].at("id") == "cap_t0");
    CHECK(recPass.at("step_records")[0].at("status") == "OK");

    // NG case
    const auto outNg = root.path() / "out_ng";
    CHECK(IR_RunRecipeJson(stText.c_str(), makeCalibRecipe(1.0e-12).dump().c_str(), utf8(outNg).c_str(), nullptr, &rr, err, sizeof err) == IR_OK);
    CHECK(countRawFiles(outNg) == 0);
    const auto recNg = Json::parse(readText(outNg / "result.json"));
    CHECK(recNg.at("run_status") == "NG");
}

void testRepeatBurstNoDiskDrift()
{
    TempDirGuard root("repeat_burst_drift");
    const auto fixture = root.path() / "fixture";
    fs::create_directories(fixture);
    buildDriftFixture(fixture, 16, 16, 4, 8, 3);

    const Json station = makeStation(fixture, 64);
    const Json recipe = {
        {"schemav", 1},
        {"recipe_id", "drift-3burst-nodisk-v2"},
        {"steps", Json::array({
            {{"id", "cap_t0"}, {"type", "capture"}, {"frames", 4}, {"frame_timeout_ms", 1000}, {"output", "t0"}},
            {{"id", "fl_01"}, {"type", "flush_frames"}, {"discard_frames", 1}, {"timeout_ms", 1000}},
            {{"id", "cap_t1"}, {"type", "capture"}, {"frames", 4}, {"frame_timeout_ms", 1000}, {"output", "t1"}},
            {{"id", "st_t0"}, {"type", "analyze"}, {"method", "frame_stats"}, {"name", "t0_stats"},
             {"input", "t0"}, {"params", {{"sample_std", 0}}},
             {"outputs", {{"mean_map", "v2"}, {"std_map", "vn2"}}}},
            {{"id", "st_t1"}, {"type", "analyze"}, {"method", "frame_stats"}, {"name", "t1_stats"},
             {"input", "t1"}, {"params", {{"sample_std", 0}}},
             {"outputs", {{"mean_map", "v1"}, {"std_map", "vn1"}}}},
            {{"id", "gjb0"}, {"type", "analyze"}, {"method", "gjb_evaluate"}, {"name", "gjb_init"},
             {"inputs", {{"v1_map", "v1"}, {"v2_map", "v2"}, {"vn_map", "vn1"}}},
             {"params", {{"t0_c", 20.0}, {"t1_c", 30.0}, {"burst_frame_count", 8}, {"lrnu_kernel_size", 15}}},
             {"outputs", {{"valid_mask", "valid"}, {"drift_ctx", "dctx"}}}},
            {{"id", "nuc0"}, {"type", "analyze"}, {"method", "two_point_nuc"}, {"name", "nuc_tbl"},
             {"inputs", {{"v1_map", "v1"}, {"v2_map", "v2"}, {"valid_mask", "valid"}}},
             {"outputs", {{"k_map", "k"}, {"b_map", "b"}}}},
            {{"id", "inv0"}, {"type", "analyze"}, {"method", "mask_combine"}, {"name", "blind"},
             {"inputs", {{"mask_a", "valid"}}}, {"params", {{"op", "not"}}},
             {"outputs", {{"out_mask", "blind_mask"}}}},
            {{"id", "drift_loop"}, {"type", "repeat_burst"}, {"max_iterations", 3}, {"interval_sec", 0.0},
             {"csv_file", "burst_summary.csv"},
             {"record_metrics", Json::array({
                 "org_gjb.mean_dn", "org_gjb.spatial_std_dn", "nuc_gjb.spatial_std_dn",
                 "n3d_org.delta_total_dn", "n3d_nuc.delta_total_dn",
                 "dyn_thresh.pass_count", "ch_spec.ch0_peak_freq_hz"
             })},
             {"steps", Json::array({
                 {{"id", "rb_fl"}, {"type", "flush_frames"}, {"discard_frames", 1}, {"timeout_ms", 1000}},
                 {{"id", "rb_cap"}, {"type", "capture"}, {"frames", 8}, {"frame_timeout_ms", 1000}, {"output", "burst"}},
                 {{"id", "rb_org_st"}, {"type", "analyze"}, {"method", "frame_stats"}, {"name", "org_stats"},
                  {"input", "burst"}, {"params", {{"sample_std", 0}}},
                  {"outputs", {{"mean_map", "org_mean"}, {"std_map", "org_vn"}}}},
                 {{"id", "rb_org_gjb"}, {"type", "analyze"}, {"method", "gjb_burst"}, {"name", "org_gjb"},
                  {"inputs", {{"mean_map", "org_mean"}, {"vn_map", "org_vn"}, {"valid_mask", "valid"}, {"drift_ctx", "dctx"}}}},
                 {{"id", "rb_nuc_st"}, {"type", "analyze"}, {"method", "frame_stats"}, {"name", "nuc_stats"},
                  {"inputs", {{"frames", "burst"}, {"k_map", "k"}, {"b_map", "b"}, {"valid_mask", "valid"}}},
                  {"params", {{"sample_std", 0}}},
                  {"outputs", {{"mean_map", "nuc_mean"}, {"std_map", "nuc_vn"}}}},
                 {{"id", "rb_nuc_gjb"}, {"type", "analyze"}, {"method", "gjb_burst"}, {"name", "nuc_gjb"},
                  {"inputs", {{"mean_map", "nuc_mean"}, {"vn_map", "nuc_vn"}, {"valid_mask", "valid"}, {"drift_ctx", "dctx"}}}},
                 {{"id", "rb_thr"}, {"type", "analyze"}, {"method", "threshold_mask"}, {"name", "dyn_thresh"},
                  {"inputs", {{"map", "org_vn"}, {"base_valid_mask", "valid"}}},
                  {"params", {{"mode", "upper_only"}, {"low", 0.0}, {"high", 5.0}}},
                  {"outputs", {{"out_mask", "dyn_mask"}}}},
                 {{"id", "rb_fft"}, {"type", "analyze"}, {"method", "channel_spectrum"}, {"name", "ch_spec"},
                  {"input", "burst"},
                  {"params", {{"axis", "columns"}, {"channel_count", 4}, {"sample_rate_hz", 50.0}}}},
                 {{"id", "rb_n3d_org"}, {"type", "analyze"}, {"method", "noise3d_compute"}, {"name", "n3d_org"},
                  {"inputs", {{"frames", "burst"}, {"blind_mask", "blind_mask"}}},
                  {"params", {{"mode", "org"}}}},
                 {{"id", "rb_n3d_nuc"}, {"type", "analyze"}, {"method", "noise3d_compute"}, {"name", "n3d_nuc"},
                  {"inputs", {{"frames", "burst"}, {"blind_mask", "blind_mask"}, {"k_map", "k"}, {"b_map", "v1"}}},
                  {"params", {{"mode", "nuc"}}}}
             })}}
        })},
        {"limits", Json::array({
            {{"metric", "nuc_gjb.spatial_std_dn"}, {"operator", "max"}, {"value", 1e-9}, {"unit", "DN"}}
        })}
    };

    const auto outDir = root.path() / "out_drift";
    char err[2048]{};
    IRRecipeResult rr{};
    const auto st = IR_RunRecipeJson(station.dump().c_str(), recipe.dump().c_str(), utf8(outDir).c_str(), nullptr, &rr, err, sizeof err);
    if (st != IR_OK)
        std::cerr << "testRepeatBurstNoDiskDrift err: " << err << '\n';
    CHECK(st == IR_OK);
    CHECK(rr.cleanup_status == IR_OK && rr.completed_frames == 4 + 4 + 24);
    CHECK(countRawFiles(outDir) == 0);
    CHECK(fs::is_regular_file(outDir / "burst_summary.csv"));
    CHECK(fs::is_regular_file(outDir / "result.json"));

    const auto csvText = readText(outDir / "burst_summary.csv");
    std::stringstream ss(csvText);
    std::string line;
    std::vector<std::vector<std::string>> rows;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) rows.push_back(splitCsvLine(line));
    }
    CHECK(rows.size() == 4); // header + 3 burst rows
    if (rows.size() == 4) {
        const double m0 = std::stod(rows[1][3]);
        const double m1 = std::stod(rows[2][3]);
        const double m2 = std::stod(rows[3][3]);
        CHECK(m1 > m0 && m2 > m1);
        const double orgStd = std::stod(rows[3][4]);
        const double nucStd = std::stod(rows[3][5]);
        const double n3dOrg = std::stod(rows[3][6]);
        const double n3dNuc = std::stod(rows[3][7]);
        const double passPix = std::stod(rows[3][8]);
        const double peakFreq = std::stod(rows[3][9]);
        CHECK(orgStd > 50.0);
        CHECK(nucStd < 1e-10);
        CHECK(n3dNuc < n3dOrg);
        CHECK(passPix == 256.0);
        CHECK(std::abs(peakFreq - 25.0) < 1e-9);
    }

    const auto rec = Json::parse(readText(outDir / "result.json"));
    CHECK(rec.at("run_status") == "PASS");
    CHECK(rec.at("raw_saved") == false);
    CHECK(rec.at("burst_runs").size() == 1 && rec.at("burst_runs")[0].at("iterations") == 3);
}

void testMultiBatchSaveRawSubdirs()
{
    TempDirGuard root("multisave");
    const auto fixture = root.path() / "fixture";
    fs::create_directories(fixture);
    buildDriftFixture(fixture, 16, 16, 4, 8, 1);

    const Json recipe = {
        {"schemav", 1},
        {"recipe_id", "multi-save-v2"},
        {"steps", Json::array({
            {{"id", "cap_t0"}, {"type", "capture"}, {"frames", 4}, {"frame_timeout_ms", 1000}, {"output", "b_t0"}},
            {{"id", "fl_01"}, {"type", "flush_frames"}, {"discard_frames", 1}, {"timeout_ms", 1000}},
            {{"id", "cap_t1"}, {"type", "capture"}, {"frames", 4}, {"frame_timeout_ms", 1000}, {"output", "b_t1"}},
            {{"id", "save_0"}, {"type", "save_raw"}, {"input", "b_t0"}, {"subdir", "t0"}},
            {{"id", "save_1"}, {"type", "save_raw"}, {"input", "b_t1"}, {"subdir", "t1"}}
        })}
    };

    const auto outDir = root.path() / "out_multi";
    char err[2048]{};
    IRRecipeResult rr{};
    CHECK(IR_RunRecipeJson(makeStation(fixture).dump().c_str(), recipe.dump().c_str(), utf8(outDir).c_str(), nullptr, &rr, err, sizeof err) == IR_OK);
    CHECK(fs::is_regular_file(outDir / "t0" / "frames.raw"));
    CHECK(fs::is_regular_file(outDir / "t0" / "frames.csv"));
    CHECK(fs::is_regular_file(outDir / "t1" / "frames.raw"));
    CHECK(fs::is_regular_file(outDir / "t1" / "frames.csv"));
    const auto rec = Json::parse(readText(outDir / "result.json"));
    CHECK(rec.at("raw_saved") == true);
    CHECK(rec.at("saved_batches").size() == 2);
}

void testRepeatBurstCancelPreservesFlushedCsv()
{
    TempDirGuard root("repeat_cancel");
    const auto fixture = root.path() / "fixture";
    fs::create_directories(fixture);
    buildDriftFixture(fixture, 16, 16, 4, 8, 5);

    const Json recipe = {
        {"schemav", 1},
        {"recipe_id", "repeat-cancel-v2"},
        {"steps", Json::array({
            {{"id", "loop"}, {"type", "repeat_burst"}, {"max_iterations", 5}, {"interval_sec", 0.5},
             {"csv_file", "partial.csv"},
             {"steps", Json::array({
                 {{"id", "c"}, {"type", "capture"}, {"frames", 4}, {"frame_timeout_ms", 1000}, {"output", "b"}},
                 {{"id", "a"}, {"type", "analyze"}, {"method", "frame_stats"}, {"name", "st"},
                  {"input", "b"}, {"params", {{"sample_std", 0}}}}
             })}}
        })}
    };

    const auto outDir = root.path() / "out_cancel";
    const auto csvPath = outDir / "partial.csv";
    const std::string stJson = makeStation(fixture).dump();
    const std::string recJson = recipe.dump();

    int32_t runStatus = IR_OK;
    IRRecipeResult rr{};
    char err[2048]{};

    std::thread runner([&] {
        runStatus = IR_RunRecipeJson(stJson.c_str(), recJson.c_str(), utf8(outDir).c_str(), nullptr, &rr, err, sizeof err);
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool sawFirstRow = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (fs::exists(csvPath)) {
            const auto text = readText(csvPath);
            if (std::count(text.begin(), text.end(), '\n') >= 2) {
                sawFirstRow = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(sawFirstRow);
    IR_RequestStop();
    runner.join();

    CHECK(runStatus == IR_CANCELLED);
    CHECK(rr.cleanup_status == IR_OK);
    CHECK(fs::is_regular_file(csvPath));
    CHECK(Json::parse(readText(outDir / "result.json")).at("run_status") == "CANCELLED");
}

void testBurstCsvQuotesMetricNames()
{
    TempDirGuard root("burst_csv_quotes");
    const auto fixture = root.path() / "fixture";
    fs::create_directories(fixture);
    buildDriftFixture(fixture, 4, 4, 2, 2, 1);
    const std::string name = "sensor,\"A\"\r\nambient";
    const Json recipe{{"recipe_id", "quoted-csv"}, {"steps", Json::array({
        {{"id", "burst"}, {"type", "repeat_burst"}, {"max_iterations", 1},
         {"record_metrics", Json::array({name + ".mean_dn"})},
         {"steps", Json::array({
             {{"id", "cap"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}},
             {{"id", "stats"}, {"type", "analyze"}, {"method", "frame_stats"},
              {"name", name}, {"input", "cap"}}
         })}}
    })}};
    char err[2048]{};
    IRRecipeResult result{};
    const auto out = root.path() / "out";
    CHECK(IR_RunRecipeJson(makeStation(fixture).dump().c_str(), recipe.dump().c_str(),
                          utf8(out).c_str(), nullptr, &result, err, sizeof err) == IR_OK);
    CHECK(readText(out / "burst_summary.csv").starts_with(
        "iteration,timestamp_utc,elapsed_s,\"sensor,\"\"A\"\"\r\nambient.mean_dn [DN]\"\n"));
}

void testPreflightRejections()
{
    TempDirGuard root("preflight_v2");
    const auto fixture = root.path() / "fixture";
    fs::create_directories(fixture);
    buildDriftFixture(fixture, 4, 4, 2, 2, 1);
    const std::string stOk = makeStation(fixture).dump();
    char err[1024]{};

    // 1. Missing/future slot
    const Json rMissingSlot = {
        {"schemav", 1},
        {"recipe_id", "bad-slot"},
        {"steps", Json::array({
            {{"id", "a"}, {"type", "analyze"}, {"method", "frame_stats"}, {"input", "missing"}}
        })}
    };
    CHECK(IR_CheckRecipeJson(stOk.c_str(), rMissingSlot.dump().c_str(), err, sizeof err) == IR_BAD_ARGUMENT);

    // 2. Slot type mismatch (map passed where batch required)
    const Json rTypeMismatch = {
        {"schemav", 1},
        {"recipe_id", "bad-type"},
        {"steps", Json::array({
            {{"id", "c"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}, {"output", "b"}},
            {{"id", "s"}, {"type", "analyze"}, {"method", "frame_stats"}, {"input", "b"},
             {"outputs", {{"mean_map", "m"}}}},
            {{"id", "bad"}, {"type", "analyze"}, {"method", "frame_stats"}, {"input", "m"}}
        })}
    };
    CHECK(IR_CheckRecipeJson(stOk.c_str(), rTypeMismatch.dump().c_str(), err, sizeof err) == IR_BAD_ARGUMENT);

    Json aliasedSlot = rTypeMismatch;
    aliasedSlot["steps"][2] = {{"id", "fft"}, {"type", "analyze"}, {"method", "fft2d"},
                                {"input", "m"}, {"outputs", {{"magnitude_map", "m"}}}};
    CHECK(IR_CheckRecipeJson(stOk.c_str(), aliasedSlot.dump().c_str(), err, sizeof err) == IR_BAD_ARGUMENT);
    CHECK(std::string(err).find("aliases input slot 'm'") != std::string::npos);

    Json duplicateSlot = rTypeMismatch;
    duplicateSlot["steps"].erase(2);
    duplicateSlot["steps"][1]["outputs"]["std_map"] = "m";
    CHECK(IR_CheckRecipeJson(stOk.c_str(), duplicateSlot.dump().c_str(), err, sizeof err) == IR_BAD_ARGUMENT);
    CHECK(std::string(err).find("binds duplicate output slot 'm'") != std::string::npos);

    // 3. save_raw inside repeat_burst
    const Json rSaveInsideLoop = {
        {"schemav", 1},
        {"recipe_id", "bad-loop-save"},
        {"steps", Json::array({
            {{"id", "lp"}, {"type", "repeat_burst"}, {"max_iterations", 2},
             {"steps", Json::array({
                 {{"id", "c"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}},
                 {{"id", "sr"}, {"type", "save_raw"}, {"input", "c"}}
             })}}
        })}
    };
    CHECK(IR_CheckRecipeJson(stOk.c_str(), rSaveInsideLoop.dump().c_str(), err, sizeof err) == IR_BAD_ARGUMENT);

    // 4. Nested repeat_burst
    const Json rNestedLoop = {
        {"schemav", 1},
        {"recipe_id", "bad-nested"},
        {"steps", Json::array({
            {{"id", "lp"}, {"type", "repeat_burst"}, {"max_iterations", 2},
             {"steps", Json::array({
                 {{"id", "lp2"}, {"type", "repeat_burst"}, {"max_iterations", 1},
                  {"steps", Json::array({{{"id", "c"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}}})}}
             })}}
        })}
    };
    CHECK(IR_CheckRecipeJson(stOk.c_str(), rNestedLoop.dump().c_str(), err, sizeof err) == IR_BAD_ARGUMENT);

    // 5. Path traversal subdir / csv_file
    const Json rBadSubdir = {
        {"schemav", 1},
        {"recipe_id", "bad-subdir"},
        {"steps", Json::array({
            {{"id", "c"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}},
            {{"id", "s"}, {"type", "save_raw"}, {"input", "c"}, {"subdir", "../escape"}}
        })}
    };
    CHECK(IR_CheckRecipeJson(stOk.c_str(), rBadSubdir.dump().c_str(), err, sizeof err) == IR_BAD_ARGUMENT);

    const Json rBadCsv = {
        {"schemav", 1},
        {"recipe_id", "bad-csv"},
        {"steps", Json::array({
            {{"id", "lp"}, {"type", "repeat_burst"}, {"max_iterations", 1}, {"csv_file", "sub/out.csv"},
             {"steps", Json::array({
                 {{"id", "c"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}}
             })}}
        })}
    };
    CHECK(IR_CheckRecipeJson(stOk.c_str(), rBadCsv.dump().c_str(), err, sizeof err) == IR_BAD_ARGUMENT);

    auto repeatWithCsv = [](const std::string &id, const std::string &csvFile) {
        return Json{
            {"id", id}, {"type", "repeat_burst"}, {"max_iterations", 1},
            {"csv_file", csvFile},
            {"steps", Json::array({
                {{"id", id + "_capture"}, {"type", "capture"},
                 {"frames", 1}, {"frame_timeout_ms", 0}}
            })}
        };
    };
    const auto checkInvalidOutputNames = [&](Json steps, const char *recipeId) {
        const Json recipe{{"schemav", 1}, {"recipe_id", recipeId}, {"steps", std::move(steps)}};
        return IR_CheckRecipeJson(stOk.c_str(), recipe.dump().c_str(), err, sizeof err);
    };
    CHECK(checkInvalidOutputNames(Json::array({repeatWithCsv("reserved", "result.json")}),
                                  "reserved-output") == IR_BAD_ARGUMENT);
    CHECK(checkInvalidOutputNames(Json::array({repeatWithCsv("device", "CON.csv")}),
                                  "reserved-device") == IR_BAD_ARGUMENT);
    CHECK(checkInvalidOutputNames(Json::array({
        repeatWithCsv("upper", "Burst.csv"), repeatWithCsv("lower", "burst.CSV")}),
        "case-fold-csv") == IR_BAD_ARGUMENT);
    CHECK(checkInvalidOutputNames(Json::array({
        {{"id", "source"}, {"type", "capture"}, {"frames", 1}, {"frame_timeout_ms", 0}},
        {{"id", "archive"}, {"type", "save_raw"}, {"input", "source"}, {"subdir", "burst.csv"}},
        repeatWithCsv("loop", "Burst.CSV")}), "csv-directory-collision") == IR_BAD_ARGUMENT);

    const Json budgetRecipe{
        {"schemav", 1}, {"recipe_id", "storage-budget"},
        {"steps", Json::array({
            {{"id", "capture"}, {"type", "capture"}, {"frames", 100},
             {"frame_timeout_ms", 0}}
        })}
    };
    Json limitedStation = makeStation(fixture);
    limitedStation["acquisition"]["queue_capacity"] = 2;
    limitedStation["acquisition"]["max_inflight_bytes"] = 1500;
    const auto outDir = root.path() / "storage_budget_out";
    IRRecipeResult result{};
    CHECK(IR_RunRecipeJson(limitedStation.dump().c_str(), budgetRecipe.dump().c_str(), utf8(outDir).c_str(), nullptr, &result, err, sizeof err) == IR_BAD_ARGUMENT);
    CHECK(std::string(err).find("max_inflight_bytes") != std::string::npos);
    CHECK(Json::parse(readText(outDir / "result.json")).at("run_status") == "ERROR");

    const Json scratchRecipe{{"recipe_id", "frame-stats-scratch-budget"}, {"steps", Json::array({
        {{"id", "capture"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}},
        {{"id", "stats"}, {"type", "analyze"}, {"method", "frame_stats"}, {"input", "capture"}}
    })}};
    // The retained estimate includes no output slots, but frame_stats still needs two temporary maps.
    constexpr uint64_t scratchPeak = (2 + 2 + 2) * 16 * sizeof(uint16_t) +
        4 * sizeof(IRFrameStamp) + 2 * 16 * sizeof(double);
    limitedStation["acquisition"]["max_inflight_bytes"] = scratchPeak - 1;
    CHECK(IR_RunRecipeJson(limitedStation.dump().c_str(), scratchRecipe.dump().c_str(),
                           utf8(root.path() / "scratch_reject").c_str(), nullptr, &result,
                           err, sizeof err) == IR_BAD_ARGUMENT);
    CHECK(result.failed_stage == IR_RECIPE_STAGE_ANALYZE && result.failed_step_index == 1);
    CHECK(std::string(err).find("frame storage " + std::to_string(scratchPeak)) != std::string::npos);
    limitedStation["acquisition"]["max_inflight_bytes"] = scratchPeak;
    CHECK(IR_RunRecipeJson(limitedStation.dump().c_str(), scratchRecipe.dump().c_str(),
                           utf8(root.path() / "scratch_fit").c_str(), nullptr, &result,
                           err, sizeof err) == IR_OK);

    auto savedStatsRecipe = scratchRecipe;
    auto &savedSteps = savedStatsRecipe["steps"];
    savedSteps.insert(savedSteps.begin() + 1, Json{
        {"id", "save_first"}, {"type", "save_raw"}, {"input", "capture"}, {"subdir", "first"}});
    savedSteps.insert(savedSteps.begin() + 2, Json{
        {"id", "save_second"}, {"type", "save_raw"}, {"input", "capture"}, {"subdir", "second"}});
    const auto savedStatsOut = root.path() / "saved_stats";
    CHECK(IR_RunRecipeJson(limitedStation.dump().c_str(), savedStatsRecipe.dump().c_str(),
                           utf8(savedStatsOut).c_str(), nullptr, &result, err, sizeof err) == IR_OK);
    const auto firstRaw = readText(savedStatsOut / "first" / "frames.raw");
    CHECK(firstRaw.size() == 2 * 16 * sizeof(uint16_t));
    CHECK(firstRaw == readText(savedStatsOut / "second" / "frames.raw"));

    const auto repeatedFixture = root.path() / "repeated_capture_fixture";
    fs::create_directories(repeatedFixture);
    {
        std::ofstream raw(repeatedFixture / "frames.raw", std::ios::binary);
        for (uint16_t value = 0; value < 320; ++value)
            raw.write(reinterpret_cast<const char *>(&value), sizeof value);
        std::ofstream csv(repeatedFixture / "frames.csv", std::ios::binary);
        csv << "frame_index,width,height,stride_bytes,pixel_format,bit_depth,endianness\n";
        for (int frame = 0; frame < 20; ++frame)
            csv << frame << ",4,4,8,RAW16_LE,16,little\n";
    }
    Json reusedSlotRecipe{
        {"schemav", 1}, {"recipe_id", "reused-capture-slot"},
        {"steps", Json::array()}
    };
    for (int index = 0; index < 10; ++index) {
        reusedSlotRecipe["steps"].push_back({
            {"id", "capture_" + std::to_string(index)}, {"type", "capture"},
            {"frames", 2}, {"frame_timeout_ms", 0}, {"output", "same_batch"}
        });
    }
    reusedSlotRecipe["steps"].push_back({
        {"id", "archive"}, {"type", "save_raw"}, {"input", "same_batch"}
    });
    Json reuseStation = makeStation(repeatedFixture);
    reuseStation["acquisition"]["max_inflight_bytes"] = 5500;
    const auto reuseOut = root.path() / "reused_slot_output";
    CHECK(IR_RunRecipeJson(reuseStation.dump().c_str(), reusedSlotRecipe.dump().c_str(), utf8(reuseOut).c_str(), nullptr, &result, err, sizeof err) == IR_OK);
    CHECK(result.completed_frames == 20);
    CHECK(fs::is_regular_file(reuseOut / "frames.raw"));

    const Json growthRecipe{{"recipe_id", "batch-growth-budget"}, {"steps", Json::array({
        {{"id", "capture_first"}, {"type", "capture"}, {"frames", 4}, {"frame_timeout_ms", 0}, {"output", "b"}},
        {{"id", "capture_second"}, {"type", "capture"}, {"frames", 5}, {"frame_timeout_ms", 0}, {"output", "b"}}
    })}};
    auto growthStation = makeStation(repeatedFixture, 2);
    constexpr uint64_t captureBuffers = 4 * 16 * sizeof(uint16_t) + 2 * sizeof(IRFrameStamp);
    constexpr uint64_t bytesPerStoredFrame = 16 * sizeof(uint16_t) + sizeof(IRFrameStamp);
    constexpr uint64_t growthPeak = captureBuffers + (4 + 5) * bytesPerStoredFrame;
    growthStation["acquisition"]["max_inflight_bytes"] = growthPeak - 1;
    CHECK(IR_RunRecipeJson(growthStation.dump().c_str(), growthRecipe.dump().c_str(),
                           utf8(root.path() / "growth_rejected").c_str(), nullptr, &result,
                           err, sizeof err) == IR_BAD_ARGUMENT);
    CHECK(result.failed_stage == IR_RECIPE_STAGE_PREPARE && result.completed_frames == 0);
    growthStation["acquisition"]["max_inflight_bytes"] = growthPeak;
    CHECK(IR_RunRecipeJson(growthStation.dump().c_str(), growthRecipe.dump().c_str(),
                           utf8(root.path() / "growth_fit").c_str(), nullptr, &result,
                           err, sizeof err) == IR_OK);
    CHECK(result.completed_frames == 9);

    auto growthThenMaps = growthRecipe;
    growthThenMaps["steps"].push_back({
        {"id", "stats"}, {"type", "analyze"}, {"method", "frame_stats"}, {"input", "b"},
        {"outputs", {{"mean_map", "mean"}, {"std_map", "std"}}}
    });
    CHECK(IR_RunRecipeJson(growthStation.dump().c_str(), growthThenMaps.dump().c_str(),
                           utf8(root.path() / "growth_then_maps").c_str(), nullptr, &result,
                           err, sizeof err) == IR_OK);

    const Json archiveThenReuseRecipe{
        {"schemav", 1}, {"recipe_id", "archive-before-reuse"},
        {"steps", Json::array({
            {{"id", "capture_first"}, {"type", "capture"}, {"frames", 2},
             {"frame_timeout_ms", 0}, {"output", "same_batch"}},
            {{"id", "archive_first"}, {"type", "save_raw"},
             {"input", "same_batch"}, {"subdir", "first"}},
            {{"id", "capture_second"}, {"type", "capture"}, {"frames", 18},
             {"frame_timeout_ms", 0}, {"output", "same_batch"}},
            {{"id", "archive_second"}, {"type", "save_raw"},
             {"input", "same_batch"}, {"subdir", "second"}}
        })}
    };
    const auto archiveReuseOut = root.path() / "archive_before_reuse_output";
    auto archiveStation = makeStation(repeatedFixture, 2);
    archiveStation["acquisition"]["max_inflight_bytes"] = captureBuffers + (2 + 18) * bytesPerStoredFrame;
    CHECK(IR_RunRecipeJson(archiveStation.dump().c_str(), archiveThenReuseRecipe.dump().c_str(), utf8(archiveReuseOut).c_str(), nullptr, &result, err, sizeof err) == IR_OK);
    CHECK(result.completed_frames == 20);
    const auto rawMatches = [](const fs::path &path, uint16_t firstValue, size_t pixelCount) {
        const std::string bytes = readText(path);
        if (bytes.size() != pixelCount * sizeof(uint16_t))
            return false;
        for (size_t index = 0; index < pixelCount; ++index) {
            const uint16_t expected = static_cast<uint16_t>(firstValue + index);
            if (static_cast<uint8_t>(bytes[index * 2]) != static_cast<uint8_t>(expected & 0xff) ||
                static_cast<uint8_t>(bytes[index * 2 + 1]) != static_cast<uint8_t>(expected >> 8))
                return false;
        }
        return true;
    };
    CHECK(rawMatches(archiveReuseOut / "first" / "frames.raw", 0, 2 * 16));
    CHECK(rawMatches(archiveReuseOut / "second" / "frames.raw", 32, 18 * 16));

    const auto existingOut = root.path() / "existing_output";
    fs::create_directory(existingOut);
    {
        std::ofstream marker(existingOut / "keep.txt", std::ios::binary);
        marker << "preserve";
    }
    const Json oneFrameRecipe{
        {"schemav", 1}, {"recipe_id", "existing-output-check"},
        {"steps", Json::array({{{"id", "capture"}, {"type", "capture"},
                                 {"frames", 1}, {"frame_timeout_ms", 0}}})}
    };
    CHECK(IR_RunRecipeJson(stOk.c_str(), oneFrameRecipe.dump().c_str(), utf8(existingOut).c_str(), nullptr, &result, err, sizeof err) == IR_IO_ERROR);
    CHECK(readText(existingOut / "keep.txt") == "preserve");

    // 公共错误缓冲区应保留输出路径的精确 UTF-8 字节。
    const auto cnOut = root.path() / fs::path(u8"已存在输出");
    fs::create_directory(cnOut);
    CHECK(IR_RunRecipeJson(stOk.c_str(), oneFrameRecipe.dump().c_str(), utf8(cnOut).c_str(), nullptr, &result, err, sizeof err) == IR_IO_ERROR);
    CHECK(std::string(err).find(utf8(cnOut)) != std::string::npos);

    // 6. Invalid station queue_capacity
    const Json rSimple = {
        {"schemav", 1},
        {"recipe_id", "ok"},
        {"steps", Json::array({{{"id", "c"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}}})}
    };
    CHECK(IR_CheckRecipeJson(makeStation(fixture, 1).dump().c_str(), rSimple.dump().c_str(), err, sizeof err) == IR_BAD_ARGUMENT);
    CHECK(IR_CheckRecipeJson(makeStation(fixture, 5000).dump().c_str(), rSimple.dump().c_str(), err, sizeof err) == IR_BAD_ARGUMENT);

    IRFrameSourceInfo existingInfo{};
    CHECK(IR_OpenFrames(IR_RAW_REPLAY, utf8(fixture).c_str(), 16,
                        &existingInfo, err, sizeof err) == IR_OK);
    IRCaptureStats existingStats{};
    CHECK(IR_GetCaptureStats(&existingStats, err, sizeof err) == IR_OK);
    const int32_t existingCapacity = existingStats.queue_capacity;
    const Json smallerQueueStation = makeStation(fixture, existingCapacity == 2 ? 3 : 2);
    const auto existingSourceRecipeOutput = root.path() / "existing_source_recipe_output";
    IRRecipeResult existingSourceResult{};
    CHECK(IR_RunRecipeJson(smallerQueueStation.dump().c_str(), rSimple.dump().c_str(), utf8(existingSourceRecipeOutput).c_str(), nullptr, &existingSourceResult, err, sizeof err) == IR_BUSY);
    CHECK(!fs::exists(existingSourceRecipeOutput));
    CHECK(IR_GetCaptureStats(&existingStats, err, sizeof err) == IR_OK);
    CHECK(existingStats.queue_capacity == existingCapacity);
    CHECK(IR_StartFrames(err, sizeof err) == IR_OK);
    CHECK(IR_StopFrames(err, sizeof err) == IR_OK);
    CHECK(IR_CloseFrames(err, sizeof err) == IR_OK);

    const Json nestedCaptureFailure{
        {"schemav", 1}, {"recipe_id", "nested-failure-index"},
        {"steps", Json::array({
            {{"id", "before"}, {"type", "capture"}, {"frames", 1},
             {"frame_timeout_ms", 0}},
            {{"id", "burst"}, {"type", "repeat_burst"}, {"max_iterations", 1},
             {"steps", Json::array({
                 {{"id", "exhaust_source"}, {"type", "capture"}, {"frames", 20},
                  {"frame_timeout_ms", 0}}
             })}}
        })}
    };
    const auto nestedFailureOutput = root.path() / "nested_failure_output";
    IRRecipeResult nestedFailureResult{};
    CHECK(IR_RunRecipeJson(stOk.c_str(), nestedCaptureFailure.dump().c_str(), utf8(nestedFailureOutput).c_str(), nullptr, &nestedFailureResult, err, sizeof err) == IR_END_OF_STREAM);
    CHECK(nestedFailureResult.failed_stage == IR_RECIPE_STAGE_CAPTURE);
    CHECK(nestedFailureResult.failed_step_index == 1);
}

void testGaussianFilterAndFlickerDetectV2()
{
    TempDirGuard root("gaussian_flicker_v2");
    const auto fixture = root.path() / "fixture";
    fs::create_directories(fixture);

    // 4x4, 4 frames: 15 pixels steady at 1000, pixel 15 spikes to 1040 in frames 0,1,2 and returns to 1000 in frame 3
    std::vector<uint16_t> frames(64, 1000);
    frames[0 * 16 + 15] = 1040;
    frames[1 * 16 + 15] = 1040;
    frames[2 * 16 + 15] = 1040;
    {
        std::ofstream raw(fixture / "frames.raw", std::ios::binary);
        raw.write(reinterpret_cast<const char *>(frames.data()), std::streamsize(frames.size() * sizeof(uint16_t)));
    }
    {
        std::ofstream csv(fixture / "frames.csv", std::ios::binary);
        csv << "frame_index,width,height,stride_bytes,pixel_format,bit_depth,endianness\n";
        for (int i = 0; i < 4; ++i)
            csv << i << ",4,4,8,RAW16_LE,16,little\n";
    }

    const Json recipe = {
        {"schemav", 1},
        {"recipe_id", "v2-gaussian-flicker"},
        {"steps", Json::array({
            {{"id", "cap"}, {"type", "capture"}, {"frames", 4}, {"frame_timeout_ms", 0}, {"output", "b"}},
            {{"id", "stats"}, {"type", "analyze"}, {"method", "frame_stats"}, {"input", "b"},
             {"outputs", {{"mean_map", "m"}}}},
            {{"id", "gauss"}, {"type", "analyze"}, {"method", "spatial_filter"},
             {"inputs", {{"in_map", "m"}}},
             {"params", {{"filter_kind", "gaussian"}, {"kernel_w", 3}, {"kernel_h", 3}}},
             {"outputs", {{"low_map", "g_low"}, {"high_map", "g_high"}}}},
            {{"id", "flick"}, {"type", "analyze"}, {"method", "flicker_detect"},
             {"inputs", {{"frames", "b"}}},
             {"params", {{"std_dev_multiplier", 2.0}, {"exceed_count_threshold", 2}}},
             {"outputs", {{"flicker_mask", "f_mask"}}}}
        })},
        {"limits", Json::array({
            {{"metric", "flick.flicker_count"}, {"operator", "max"}, {"value", 1.0}, {"unit", "count"}},
            {{"metric", "flick.flicker_ratio_pct"}, {"operator", "max"}, {"value", 6.5}, {"unit", "%"}}
        })}
    };

    const std::string stJson = makeStation(fixture).dump();
    const std::string rJson = recipe.dump();
    char err[1024]{};
    CHECK(IR_CheckRecipeJson(stJson.c_str(), rJson.c_str(), err, sizeof err) == IR_OK);

    const auto outDir = root.path() / "out_gf";
    IRRecipeResult rr{};
    CHECK(IR_RunRecipeJson(stJson.c_str(), rJson.c_str(), utf8(outDir).c_str(), nullptr, &rr, err, sizeof err) == IR_OK);
    CHECK(countRawFiles(outDir) == 0);
    const auto res = Json::parse(readText(outDir / "result.json"));
    CHECK(res.at("run_status") == "PASS");
    CHECK(res.at("metrics").at("gauss.low_std_dn").at("method") == "spatial_gaussian_low");
    CHECK(std::abs(res.at("metrics").at("flick.flicker_count").at("value").get<double>() - 1.0) < 1e-12);
    CHECK(std::abs(res.at("metrics").at("flick.flicker_ratio_pct").at("value").get<double>() - 6.25) < 1e-12);
}

void testFft2dPipelineV2()
{
    TempDirGuard root("fft2d_v2");
    const auto fixture = root.path() / "fixture";
    fs::create_directories(fixture);

    // 4x4, 2 frames: 1000 + round(100 * cos(2*pi*c/4)) -> [1100, 1000, 900, 1000] per row
    std::vector<uint16_t> frames(32);
    const double pi = std::acos(-1.0);
    for (int f = 0; f < 2; ++f) {
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                const double val = 1000.0 + 100.0 * std::cos(2.0 * pi * double(c) / 4.0);
                frames[size_t(f * 16 + r * 4 + c)] = static_cast<uint16_t>(std::llround(val));
            }
        }
    }
    {
        std::ofstream raw(fixture / "frames.raw", std::ios::binary);
        raw.write(reinterpret_cast<const char *>(frames.data()), std::streamsize(frames.size() * sizeof(uint16_t)));
    }
    {
        std::ofstream csv(fixture / "frames.csv", std::ios::binary);
        csv << "frame_index,width,height,stride_bytes,pixel_format,bit_depth,endianness\n";
        for (int i = 0; i < 2; ++i)
            csv << i << ",4,4,8,RAW16_LE,16,little\n";
    }

    const Json recipe = {
        {"schemav", 1},
        {"recipe_id", "v2-fft2d"},
        {"steps", Json::array({
            {{"id", "cap"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}, {"output", "b"}},
            {{"id", "stats"}, {"type", "analyze"}, {"method", "frame_stats"}, {"input", "b"},
             {"outputs", {{"mean_map", "m"}}}},
            {{"id", "fft"}, {"type", "analyze"}, {"method", "fft2d"},
             {"inputs", {{"map", "m"}}},
             {"params", {{"window", "rect"}, {"detrend_mean", true}, {"shift_dc_center", true}}},
             {"outputs", {{"magnitude_map", "mag"}}}},
            {{"id", "mag_sum"}, {"type", "analyze"}, {"method", "map_summary"},
             {"inputs", {{"map", "mag"}}}}
        })},
        {"limits", Json::array({
            {{"metric", "fft.peak_ac_magnitude"}, {"operator", "min"}, {"value", 49.0}, {"unit", "DN"}},
            {{"metric", "mag_sum.max"}, {"operator", "max"}, {"value", 51.0}, {"unit", "DN"}}
        })}
    };

    const std::string stJson = makeStation(fixture).dump();
    const std::string rJson = recipe.dump();
    char err[1024]{};
    CHECK(IR_CheckRecipeJson(stJson.c_str(), rJson.c_str(), err, sizeof err) == IR_OK);

    const auto outDir = root.path() / "out_fft2d";
    IRRecipeResult rr{};
    CHECK(IR_RunRecipeJson(stJson.c_str(), rJson.c_str(), utf8(outDir).c_str(), nullptr, &rr, err, sizeof err) == IR_OK);
    CHECK(countRawFiles(outDir) == 0);
    const auto res = Json::parse(readText(outDir / "result.json"));
    CHECK(res.at("run_status") == "PASS");
    CHECK(std::abs(res.at("metrics").at("fft.dc_magnitude").at("value").get<double>()) < 1e-9);
    CHECK(std::abs(res.at("metrics").at("fft.peak_ac_magnitude").at("value").get<double>() - 50.0) < 1e-9);
    CHECK(std::abs(res.at("metrics").at("mag_sum.max").at("value").get<double>() - 50.0) < 1e-9);
}

void testRunRecipeJsonExStepCallbacks()
{
    TempDirGuard root("run_ex_callbacks");
    const auto fixture = root.path() / "fixture";
    fs::create_directories(fixture);
    buildDriftFixture(fixture, 2, 2, 2, 2, 0);

    const std::string stJson = makeStation(fixture, 16).dump();
    const Json okRecipe{
        {"schemav", 1},
        {"recipe_id", "callbacks-ok"},
        {"steps", Json::array({
            {{"id", "cap"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}, {"output", "b"}},
            {{"id", "tn"}, {"type", "analyze"}, {"method", "temporal_noise"}, {"input", "b"}}
        })}
    };

    struct EventLog {
        std::vector<std::pair<int32_t, std::string>> items;
        int32_t lastFailedStatus{IR_OK};
    } log;
    auto cb = [](const IRRecipeStepEvent *ev, void *ud) {
        auto *l = static_cast<EventLog *>(ud);
        if (!ev) return;
        l->items.emplace_back(ev->event_kind, ev->step_id_utf8 ? ev->step_id_utf8 : "");
        if (ev->event_kind == IR_STEP_EVENT_FAILED)
            l->lastFailedStatus = ev->status;
    };
    IRRecipeRunOptions opts{sizeof(IRRecipeRunOptions), 0, cb, &log};
    IRRecipeResult rr{};
    char err[1024]{};

    const auto outOk = root.path() / "out_ok";
    CHECK(IR_RunRecipeJson(stJson.c_str(), okRecipe.dump().c_str(),
                             utf8(outOk).c_str(), &opts, &rr, err, sizeof err) == IR_OK);
    CHECK(log.items.size() == 4);
    CHECK(log.items[0] == std::make_pair(int32_t(IR_STEP_EVENT_STARTED), std::string("cap")));
    CHECK(log.items[1] == std::make_pair(int32_t(IR_STEP_EVENT_COMPLETED), std::string("cap")));
    CHECK(log.items[2] == std::make_pair(int32_t(IR_STEP_EVENT_STARTED), std::string("tn")));
    CHECK(log.items[3] == std::make_pair(int32_t(IR_STEP_EVENT_COMPLETED), std::string("tn")));

    // Invalid options rejected cleanly with VALIDATE stage
    IRRecipeRunOptions badOpts{0, 0, cb, &log};
    CHECK(IR_RunRecipeJson(stJson.c_str(), okRecipe.dump().c_str(),
                             utf8(root.path() / "out_bad").c_str(), &badOpts, &rr, err, sizeof err) == IR_BAD_ARGUMENT);
    CHECK(rr.failed_stage == IR_RECIPE_STAGE_VALIDATE);

    // Failed step triggers IR_STEP_EVENT_FAILED
    log.items.clear();
    const Json failRecipe{
        {"schemav", 1},
        {"recipe_id", "callbacks-fail"},
        {"steps", Json::array({
            {{"id", "cap_too_many"}, {"type", "capture"}, {"frames", 99}, {"frame_timeout_ms", 0}, {"output", "b"}}
        })}
    };
    CHECK(IR_RunRecipeJson(stJson.c_str(), failRecipe.dump().c_str(),
                             utf8(root.path() / "out_fail").c_str(), &opts, &rr, err, sizeof err) == IR_END_OF_STREAM);
    CHECK(log.items.size() == 2);
    CHECK(log.items[0] == std::make_pair(int32_t(IR_STEP_EVENT_STARTED), std::string("cap_too_many")));
    CHECK(log.items[1] == std::make_pair(int32_t(IR_STEP_EVENT_FAILED), std::string("cap_too_many")));
    CHECK(log.lastFailedStatus == IR_END_OF_STREAM);
}

void testFinalRecipeFailureDiagnostics()
{
    TempDirGuard root("final_diagnostics");
    const auto fixture = root.path() / "fixture";
    fs::create_directories(fixture);
    buildDriftFixture(fixture, 2, 2, 2, 2, 0);

    const auto station = makeStation(fixture).dump();
    const Json capture{{"id", "cap"}, {"type", "capture"}, {"frames", 2},
                       {"frame_timeout_ms", 0}, {"output", "b"}};
    const Json analyze{{"id", "an"}, {"type", "analyze"}, {"method", "temporal_noise"},
                       {"input", "b"}};
    const Json recipe{{"recipe_id", "late-failure"}, {"steps", Json::array({capture, analyze})}};

    struct AtCompletion {
        const char *stepId;
        std::function<void()> action;
    };
    const auto run = [&](const Json &definition, const fs::path &output, AtCompletion event,
                         int32_t expectedStatus, int32_t expectedStage) {
        IRRecipeRunOptions opts{sizeof(IRRecipeRunOptions), 0,
            [](const IRRecipeStepEvent *step, void *opaque) {
                const auto &handler = *static_cast<AtCompletion *>(opaque);
                if (step->event_kind == IR_STEP_EVENT_COMPLETED &&
                    std::string_view(step->step_id_utf8) == handler.stepId)
                    handler.action();
            }, &event};
        IRRecipeResult result{};
        char error[1024]{};
        const auto status = IR_RunRecipeJson(station.c_str(), definition.dump().c_str(),
            utf8(output).c_str(), &opts, &result, error, sizeof error);
        if (status != expectedStatus)
            std::cerr << "recipe diagnostic status=" << status << " error=" << error << '\n';
        CHECK(status == expectedStatus);
        CHECK(result.failed_stage == expectedStage);
        CHECK(result.cleanup_status == IR_OK);
    };

    const auto cancelledOut = root.path() / "cancelled";
    run(recipe, cancelledOut, {"an", [] { IR_RequestStop(); }},
        IR_CANCELLED, IR_RECIPE_STAGE_ARCHIVE);
    const auto cancelled = Json::parse(readText(cancelledOut / "result.json"));
    CHECK(cancelled.at("run_status") == "CANCELLED");
    CHECK(cancelled.at("error_code") == IR_CANCELLED);
    CHECK(cancelled.at("step_records").size() == 2);

    const auto occupiedOut = root.path() / "occupied";
    const std::string priorResult = R"({"recipe_id":"another-run"})";
    run(recipe, occupiedOut, {"cap", [&] {
        CHECK(fs::create_directory(occupiedOut));
        std::ofstream(occupiedOut / "result.json", std::ios::binary) << priorResult;
        IR_RequestStop();
    }}, IR_CANCELLED, IR_RECIPE_STAGE_ANALYZE);
    CHECK(readText(occupiedOut / "result.json") == priorResult);

    const Json burst{{"id", "loop"}, {"type", "repeat_burst"}, {"max_iterations", 1},
                     {"steps", Json::array({capture, analyze})}};
    const auto originalWorkDir = root.path() / "first_workdir";
    const auto otherWorkDir = root.path() / "second_workdir";
    fs::create_directory(originalWorkDir);
    fs::create_directories(otherWorkDir / "run");
    std::ofstream(otherWorkDir / "run" / "result.json", std::ios::binary) << priorResult;
    {
        struct RestoreWorkDir {
            fs::path original;
            ~RestoreWorkDir()
            {
                std::error_code error;
                fs::current_path(original, error);
            }
        } restore{fs::current_path()};
        fs::current_path(originalWorkDir);
        run(Json{{"recipe_id", "relative-burst"}, {"steps", Json::array({burst})}},
            "run", {"loop", [&] {
                fs::current_path(otherWorkDir);
                IR_RequestStop();
            }}, IR_CANCELLED, IR_RECIPE_STAGE_ARCHIVE);
    }
    CHECK(readText(otherWorkDir / "run" / "result.json") == priorResult);
    const auto relative = Json::parse(readText(originalWorkDir / "run" / "result.json"));
    CHECK(relative.at("run_status") == "CANCELLED");
    CHECK(fs::is_regular_file(originalWorkDir / "run" / "burst_summary.csv"));

    const fs::path blockedName(u8"归档冲突");
    const Json save{{"id", "save"}, {"type", "save_raw"}, {"input", "b"}, {"subdir", utf8(blockedName)}};
    const Json archiveRecipe{{"recipe_id", "archive-failure"}, {"steps", Json::array({burst, save})}};
    const auto archiveOut = root.path() / "archiving";
    run(archiveRecipe, archiveOut, {"save", [&] {
        std::ofstream(archiveOut / blockedName, std::ios::binary) << "regular file";
    }}, IR_IO_ERROR, IR_RECIPE_STAGE_ARCHIVE);
    const auto diagnostic = readText(archiveOut / "result.json");
    CHECK(!diagnostic.empty());
    if (diagnostic.empty()) return;
    const auto archived = Json::parse(diagnostic);
    CHECK(archived.at("run_status") == "ERROR");
    CHECK(archived.at("error_code") == IR_IO_ERROR);
    CHECK(archived.at("error_message").get<std::string>().find(utf8(archiveOut / blockedName)) != std::string::npos);
    CHECK(archived.at("step_records").size() == 2);
    CHECK(archived.at("burst_runs").size() == 1);
    CHECK(fs::is_regular_file(archiveOut / "burst_summary.csv"));
    CHECK(readText(archiveOut / blockedName) == "regular file");
}

bool isIso8601UtcMillis(const std::string &value)
{
    if (value.size() != 24) return false;
    const char *pattern = "dddd-dd-ddTdd:dd:dd.dddZ";
    for (size_t i = 0; i < 24; ++i) {
        if (pattern[i] == 'd') {
            if (!std::isdigit(static_cast<unsigned char>(value[i]))) return false;
        } else if (value[i] != pattern[i]) {
            return false;
        }
    }
    return true;
}

void testSchemavProvenanceAndTemplates(const fs::path *examplesDir)
{
    TempDirGuard root("schemav_prov");
    const auto fixture = root.path() / "fixture";
    fs::create_directories(fixture);
    buildDriftFixture(fixture, 2, 2, 2, 2, 0);

    const Json station = makeStation(fixture, 16);
    const Json recipe{
        {"schemav", 1},
        {"recipe_id", "roi-and-noise-test"},
        {"steps", Json::array({
            {{"id", "batch"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}, {"output", "batch"}},
            {{"id", "noise"}, {"type", "analyze"}, {"method", "temporal_noise"}, {"name", "noise"}, {"input", "batch"}},
            {{"id", "left"}, {"type", "analyze"}, {"method", "roi_mean"}, {"name", "left"}, {"input", "batch"},
             {"params", {{"x", 0}, {"y", 0}, {"width", 1}, {"height", 2}}}},
            {{"id", "archive"}, {"type", "save_raw"}, {"input", "batch"}}
        })},
        {"limits", Json::array({
            {{"metric", "noise.temporal_noise_dn"}, {"operator", "max"}, {"value", 10000.0}, {"unit", "DN"}}
        })}
    };

    char err[1024]{};
    auto defaultStation = station;
    defaultStation.erase("schemav");
    auto defaultRecipe = recipe;
    defaultRecipe.erase("schemav");
    CHECK(IR_CheckRecipeJson(defaultStation.dump().c_str(), defaultRecipe.dump().c_str(), err, sizeof err) == IR_OK);

    auto badStationSchema = station;
    badStationSchema["$schema"] = "../schemas/station.schema.json";
    CHECK(IR_CheckRecipeJson(badStationSchema.dump().c_str(), recipe.dump().c_str(), err, sizeof err) == IR_BAD_ARGUMENT);

    auto badRecipeSchema = recipe;
    badRecipeSchema["$schema"] = "../schemas/recipe.schema.json";
    CHECK(IR_CheckRecipeJson(station.dump().c_str(), badRecipeSchema.dump().c_str(), err, sizeof err) == IR_BAD_ARGUMENT);

    auto legacyStationVer = defaultStation;
    legacyStationVer["schema_version"] = 1;
    CHECK(IR_CheckRecipeJson(legacyStationVer.dump().c_str(), recipe.dump().c_str(), err, sizeof err) == IR_BAD_ARGUMENT);

    auto legacyRecipeVer = defaultRecipe;
    legacyRecipeVer["schema_version"] = 2;
    CHECK(IR_CheckRecipeJson(station.dump().c_str(), legacyRecipeVer.dump().c_str(), err, sizeof err) == IR_BAD_ARGUMENT);

    const auto passOutput = root.path() / "pass-run";
    IRRecipeResult rr{};
    CHECK(IR_RunRecipeJson(station.dump().c_str(), recipe.dump().c_str(),
                           utf8(passOutput).c_str(), nullptr, &rr, err, sizeof err) == IR_OK);
    const auto pass = Json::parse(readText(passOutput / "result.json"));
    CHECK(pass.at("run_status") == "PASS");
    CHECK(pass.at("station_version") == IR_StationVersion());
    CHECK(pass.at("analysis_version") == IR_AnalysisVersion());
    CHECK(pass.at("source_commit").is_string() && !pass.at("source_commit").get<std::string>().empty());
    const auto startedUtc = pass.at("started_utc").get<std::string>();
    const auto finishedUtc = pass.at("finished_utc").get<std::string>();
    CHECK(isIso8601UtcMillis(startedUtc));
    CHECK(isIso8601UtcMillis(finishedUtc));
    CHECK(startedUtc <= finishedUtc);

    if (examplesDir != nullptr && fs::is_directory(*examplesDir)) {
        const fs::path stationsDir = *examplesDir / "stations";
        const fs::path recipesDir = *examplesDir / "recipes";
        const std::vector<std::pair<std::string, std::string>> pairs{
            {"offline.json", "noise_roi.json"},
            {"offline.json", "nodisk_roi.json"},
            {"sim_tcp.json", "noise_blackbody.json"},
            {"sim_power_serial.json", "power_serial_output_cycle.json"},
            {"sim_virtual_station_tcp.json", "virtual_station_hil.json"},
        };
        for (const auto &[stationName, recipeName] : pairs) {
            const auto stPath = stationsDir / stationName;
            const auto rcPath = recipesDir / recipeName;
            if (!fs::exists(stPath) || !fs::exists(rcPath))
                continue;
            const auto stText = readText(stPath);
            const auto rcText = readText(rcPath);
            const auto stJson = Json::parse(stText);
            const auto rcJson = Json::parse(rcText);
            CHECK(!stJson.contains("$schema") && !rcJson.contains("$schema"));
            CHECK(!stJson.contains("schema_version") && !rcJson.contains("schema_version"));
            CHECK(IR_CheckRecipeJson(stText.c_str(), rcText.c_str(), err, sizeof err) == IR_OK);
        }
    }
}

void testInstrumentConnectionPreflight()
{
    auto station = makeStation("unused-preflight-source");
    constexpr const char *recipe = R"({"recipe_id":"connection-preflight","steps":[
        {"id":"batch","type":"capture","frames":1,"frame_timeout_ms":0}]})";
    station["devices"] = {
        {"blackbody-tcp", {{"kind", "pcn7-lt"}, {"connection", {{"host", "unused.invalid"}}}}},
        {"blackbody-serial", {{"kind", "dcn1000"},
                              {"connection", {{"port", "unused-serial"}, {"expected_head_serial", " HEAD "}}}}},
        {"supply", {{"kind", "it6332a"}, {"connection", {{"port", "unused-serial"}, {"parity", " EvEn "}}}}},
        {"chamber", {{"kind", "gws-sten"}, {"connection", {{"host", "unused.invalid"}, {"delimiter", " CrLf "}}}}},
        {"usb-meter", {{"kind", "dmm6500"}, {"connection", {{"visa_resource", "USB0::0x05E6::0x6500::TEST::INSTR"}}}}},
        {"meter", {{"kind", "dmm6500"}, {"connection", {{"host", "unused.invalid"}}}}},
        {"detector", {{"kind", "detector-serial"}, {"connection", {{"host", "unused.invalid"}}}}},
        {"cooler", {{"kind", "cooler-modbus"}, {"connection", {{"port", "unused-serial"}}}}}
    };

    char error[1024]{};
    CHECK(IR_CheckRecipeJson(station.dump().c_str(), recipe, error, sizeof error) == IR_OK);

    const struct {
        const char *kindName;
        IRDeviceKind kind;
        Json connection;
        const char *field;
    } invalid[] = {
        {"pcn7-lt", IR_PCN7LT, {{"host", "unused.invalid"}, {"expected_head_serial", "HEAD"}}, ".expected_head_serial"},
        {"dcn1000", IR_DCN1000, {{"port", "unused-serial"}, {"baud_rate", 115200}}, ".baud_rate"},
        {"dcn1000", IR_DCN1000, {{"port", "unused-serial"}, {"head", 0}}, ".head"},
        {"it6332a", IR_IT6332A, {{"host", "unused.invalid"}, {"port", "unused-serial"}}, ""},
        {"gws-sten", IR_OVEN_GWS_STEN, {{"host", "unused.invalid"}, {"delimiter", "bad"}}, ".delimiter"},
        {"dmm6500", IR_DMM6500, {{"visa_resource", " "}}, ".visa_resource"},
        {"dmm6500", IR_DMM6500, {{"visa_resource", "meter"}, {"host", "localhost"}}, ".host"},
        {"dmm6500", IR_DMM6500, {{"visa_resource", "meter"}, {"port", "COM8"}}, ".port"},
        {"dmm6500", IR_DMM6500, {{"host", " \t "}}, ".host"},
        {"dmm6500", IR_DMM6500, {{"host", "127.0.0.1"}, {"tcp_port", 1}, {"timeout_ms", 20}, {"stop_bits", 2}}, ".stop_bits"},
        {"detector-serial", IR_DETECTOR_SERIAL, {{"port", "unused-serial"}, {"tcp_port", 2102}}, ".tcp_port"},
        {"cooler-modbus", IR_COOLER_MODBUS, {{"port", "unused-serial"}, {"slave_address", 248}}, ".slave_address"}
    };
    for (const auto &entry : invalid) {
        station["devices"] = {{"instrument-a", {{"kind", entry.kindName}, {"connection", entry.connection}}}};
        CHECK(IR_CheckRecipeJson(station.dump().c_str(), recipe, error, sizeof error) == IR_BAD_ARGUMENT);
        CHECK(std::string_view(error).starts_with(std::string("station.devices.instrument-a.connection") + entry.field));

        IRHandle handle{1};
        CHECK(IR_OpenDevice(entry.kind, entry.connection.dump().c_str(), &handle, error, sizeof error) == IR_BAD_ARGUMENT);
        CHECK(handle == 0);
        CHECK(std::string_view(error).starts_with(std::string("connection") + entry.field));
    }
}

} // namespace

int main(int argc, char **argv)
{
    try {
        gTempRoot = testTempRoot();
        fs::create_directories(gTempRoot);
    } catch (const std::exception &e) {
        std::cerr << "recipe_test cannot prepare its temporary directory: " << e.what() << '\n';
        return 2;
    }

    const fs::path examplesDir = argc > 1 ? fs::path(argv[1]) : fs::path{};
    bool continueTests = true;
    auto run = [&](const char *name, const auto &test) {
        if (continueTests)
            continueTests = runTestCase(name, test);
    };
    run("testTwoPointCalibNoDiskPassAndNg", [] { testTwoPointCalibNoDiskPassAndNg(); });
    run("testU32RecipeBatchAndArchive", [] { testU32RecipeBatchAndArchive(); });
    run("testPaddedU32ReplayMemoryBudget", [] { testPaddedU32ReplayMemoryBudget(); });
    run("testHighBitPhysicalMetricsRejected", [] { testHighBitPhysicalMetricsRejected(); });
    run("testRepeatBurstNoDiskDrift", [] { testRepeatBurstNoDiskDrift(); });
    run("testMultiBatchSaveRawSubdirs", [] { testMultiBatchSaveRawSubdirs(); });
    run("testRepeatBurstCancelPreservesFlushedCsv", [] { testRepeatBurstCancelPreservesFlushedCsv(); });
    run("testBurstCsvQuotesMetricNames", [] { testBurstCsvQuotesMetricNames(); });
    run("testPreflightRejections", [] { testPreflightRejections(); });
    run("testInstrumentConnectionPreflight", [] { testInstrumentConnectionPreflight(); });
    run("testGaussianFilterAndFlickerDetectV2", [] { testGaussianFilterAndFlickerDetectV2(); });
    run("testFft2dPipelineV2", [] { testFft2dPipelineV2(); });
    run("testRunRecipeJsonExStepCallbacks", [] { testRunRecipeJsonExStepCallbacks(); });
    run("testFinalRecipeFailureDiagnostics", [] { testFinalRecipeFailureDiagnostics(); });
    run("testSchemavProvenanceAndTemplates", [&] {
        testSchemavProvenanceAndTemplates(argc > 1 ? &examplesDir : nullptr);
    });
    if (gFailures == 0)
        std::cout << "recipe_flow: ALL PASSED\n";
    return gFailures == 0 ? 0 : 1;
}
