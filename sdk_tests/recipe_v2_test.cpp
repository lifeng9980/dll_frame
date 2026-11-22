#include "IR_recipe.h"
#include "IR_analysis.h"
#include "IR_station.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

int gFailures = 0;

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
        dir_ = fs::temp_directory_path() / ("ir_recipe_v2_test_" + name + "_" + std::to_string(suffix));
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

Json makeStation(const fs::path &sourceDir, int32_t queueCap = 64)
{
    return {
        {"schema_version", 1},
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
            {"schema_version", 2},
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
    CHECK(IR_RunRecipeJson(stText.c_str(), makeCalibRecipe(15.0).dump().c_str(),
                           utf8(outPass).c_str(), &rr, err, sizeof err) == IR_OK);
    CHECK(rr.cleanup_status == IR_OK && rr.completed_frames == 8);
    CHECK(countRawFiles(outPass) == 0);
    const auto recPass = Json::parse(readText(outPass / "result.json"));
    CHECK(recPass.at("schema_version") == 2);
    CHECK(recPass.at("run_status") == "PASS");
    const double rnu = recPass.at("metrics").at("gjb_init.after_response_nonuniformity_pct").at("value").get<double>();
    CHECK(rnu > 9.0 && rnu < 9.5);
    CHECK(recPass.at("raw_saved") == false);
    CHECK(recPass.at("metrics").contains("lrnu_t1.low_std_dn"));
    CHECK(recPass.at("metrics").contains("blind.nonzero_count"));

    // NG case
    const auto outNg = root.path() / "out_ng";
    CHECK(IR_RunRecipeJson(stText.c_str(), makeCalibRecipe(1.0e-12).dump().c_str(),
                           utf8(outNg).c_str(), &rr, err, sizeof err) == IR_OK);
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
        {"schema_version", 2},
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
    const auto st = IR_RunRecipeJson(station.dump().c_str(), recipe.dump().c_str(),
                                     utf8(outDir).c_str(), &rr, err, sizeof err);
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
        {"schema_version", 2},
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
    CHECK(IR_RunRecipeJson(makeStation(fixture).dump().c_str(), recipe.dump().c_str(),
                           utf8(outDir).c_str(), &rr, err, sizeof err) == IR_OK);
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
        {"schema_version", 2},
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
        runStatus = IR_RunRecipeJson(stJson.c_str(), recJson.c_str(),
                                     utf8(outDir).c_str(), &rr, err, sizeof err);
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
    CHECK(!fs::exists(outDir / "result.json"));
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
        {"schema_version", 2},
        {"recipe_id", "bad-slot"},
        {"steps", Json::array({
            {{"id", "a"}, {"type", "analyze"}, {"method", "frame_stats"}, {"input", "missing"}}
        })}
    };
    CHECK(IR_CheckRecipeJson(stOk.c_str(), rMissingSlot.dump().c_str(), err, sizeof err) == IR_BAD_ARGUMENT);

    // 2. Slot type mismatch (map passed where batch required)
    const Json rTypeMismatch = {
        {"schema_version", 2},
        {"recipe_id", "bad-type"},
        {"steps", Json::array({
            {{"id", "c"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}, {"output", "b"}},
            {{"id", "s"}, {"type", "analyze"}, {"method", "frame_stats"}, {"input", "b"},
             {"outputs", {{"mean_map", "m"}}}},
            {{"id", "bad"}, {"type", "analyze"}, {"method", "frame_stats"}, {"input", "m"}}
        })}
    };
    CHECK(IR_CheckRecipeJson(stOk.c_str(), rTypeMismatch.dump().c_str(), err, sizeof err) == IR_BAD_ARGUMENT);

    // 3. save_raw inside repeat_burst
    const Json rSaveInsideLoop = {
        {"schema_version", 2},
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
        {"schema_version", 2},
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
        {"schema_version", 2},
        {"recipe_id", "bad-subdir"},
        {"steps", Json::array({
            {{"id", "c"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}},
            {{"id", "s"}, {"type", "save_raw"}, {"input", "c"}, {"subdir", "../escape"}}
        })}
    };
    CHECK(IR_CheckRecipeJson(stOk.c_str(), rBadSubdir.dump().c_str(), err, sizeof err) == IR_BAD_ARGUMENT);

    const Json rBadCsv = {
        {"schema_version", 2},
        {"recipe_id", "bad-csv"},
        {"steps", Json::array({
            {{"id", "lp"}, {"type", "repeat_burst"}, {"max_iterations", 1}, {"csv_file", "sub/out.csv"},
             {"steps", Json::array({
                 {{"id", "c"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}}
             })}}
        })}
    };
    CHECK(IR_CheckRecipeJson(stOk.c_str(), rBadCsv.dump().c_str(), err, sizeof err) == IR_BAD_ARGUMENT);

    // 6. Invalid station queue_capacity
    const Json rSimple = {
        {"schema_version", 2},
        {"recipe_id", "ok"},
        {"steps", Json::array({{{"id", "c"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}}})}
    };
    CHECK(IR_CheckRecipeJson(makeStation(fixture, 1).dump().c_str(), rSimple.dump().c_str(), err, sizeof err) == IR_BAD_ARGUMENT);
    CHECK(IR_CheckRecipeJson(makeStation(fixture, 5000).dump().c_str(), rSimple.dump().c_str(), err, sizeof err) == IR_BAD_ARGUMENT);
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
        {"schema_version", 2},
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
    CHECK(IR_RunRecipeJson(stJson.c_str(), rJson.c_str(), utf8(outDir).c_str(), &rr, err, sizeof err) == IR_OK);
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
        {"schema_version", 2},
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
    CHECK(IR_RunRecipeJson(stJson.c_str(), rJson.c_str(), utf8(outDir).c_str(), &rr, err, sizeof err) == IR_OK);
    CHECK(countRawFiles(outDir) == 0);
    const auto res = Json::parse(readText(outDir / "result.json"));
    CHECK(res.at("run_status") == "PASS");
    CHECK(std::abs(res.at("metrics").at("fft.dc_magnitude").at("value").get<double>()) < 1e-9);
    CHECK(std::abs(res.at("metrics").at("fft.peak_ac_magnitude").at("value").get<double>() - 50.0) < 1e-9);
    CHECK(std::abs(res.at("metrics").at("mag_sum.max").at("value").get<double>() - 50.0) < 1e-9);
}

} // namespace

int main()
{
    testTwoPointCalibNoDiskPassAndNg();
    testRepeatBurstNoDiskDrift();
    testMultiBatchSaveRawSubdirs();
    testRepeatBurstCancelPreservesFlushedCsv();
    testPreflightRejections();
    testGaussianFilterAndFlickerDetectV2();
    testFft2dPipelineV2();
    if (gFailures == 0)
        std::cout << "recipe_v2_test: ALL PASSED\n";
    return gFailures == 0 ? 0 : 1;
}
