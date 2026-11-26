#include "IR_recipe.h"
#include "IR_station.h"
#include "IR_plugin_abi.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

extern "C" int32_t ir_plugin_query(const IRMetricPlugin **out_list, uint32_t *out_count);

namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

int gFailures = 0;
void check(bool cond, const char *label, int line);
#define CHECK(cond) check((cond), #cond, __LINE__)

struct U32PluginHost {
    IRArrayView array{};
    IRFrameStamp stamps[2]{};
    std::vector<double> spanMap;
    std::map<std::string, double> scalars;
};

int32_t getU32Slot(void *hostCtx, const char *, IRSlotView *outView)
{
    if (!hostCtx || !outView)
        return IR_NULL_POINTER;
    auto &host = *static_cast<U32PluginHost *>(hostCtx);
    *outView = IRSlotView{};
    outView->slot_kind = IR_PLUGIN_SLOT_BATCH;
    outView->array = host.array;
    outView->valid_bits = 20;
    outView->stamps = host.stamps;
    return IR_OK;
}

int32_t hasU32Output(void *, const char *key)
{
    return key && std::string(key) == "span_map" ? 1 : 0;
}

int32_t allocateU32Span(void *hostCtx, const char *key, int32_t width, int32_t height,
                        double **outData)
{
    if (!hostCtx || !key || !outData)
        return IR_NULL_POINTER;
    auto &host = *static_cast<U32PluginHost *>(hostCtx);
    if (std::string(key) != "span_map" || width <= 0 || height <= 0)
        return IR_BAD_ARGUMENT;
    host.spanMap.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0.0);
    *outData = host.spanMap.data();
    return IR_OK;
}

int32_t emitU32Scalar(void *hostCtx, const char *suffix, double value)
{
    if (!hostCtx || !suffix)
        return IR_NULL_POINTER;
    static_cast<U32PluginHost *>(hostCtx)->scalars[suffix] = value;
    return IR_OK;
}

void testU32PluginView()
{
    const IRMetricPlugin *plugins = nullptr;
    uint32_t count = 0;
    CHECK(ir_plugin_query(&plugins, &count) == IR_OK);
    CHECK(plugins != nullptr && count > 0);
    if (!plugins || count == 0)
        return;

    const IRMetricPlugin *pixelSpan = nullptr;
    for (uint32_t i = 0; i < count; ++i) {
        if (std::string(plugins[i].method_name) == "plugin:test.pixel_span") {
            pixelSpan = &plugins[i];
            break;
        }
    }
    CHECK(pixelSpan != nullptr);
    if (!pixelSpan)
        return;

    const uint32_t pixels[]{0, 70000, 100000, 200000, 10, 65000, 110000, 210000};
    U32PluginHost context;
    context.array = IRArrayView{pixels, sizeof(pixels), IR_TYPE_U32, 2, 2, 2};
    IRHostCallbacks callbacks{};
    callbacks.host_ctx = &context;
    callbacks.has_slot = &hasU32Output;
    callbacks.get_slot = &getU32Slot;
    callbacks.alloc_double_map = &allocateU32Span;
    callbacks.emit_scalar = &emitU32Scalar;
    char err[256]{};
    CHECK(pixelSpan->execute(&callbacks, IRGeometry{2, 2, 20}, "{}", err, sizeof err) == IR_OK);
    CHECK(context.array.data == pixels);
    CHECK(context.spanMap.size() == 4);
    if (context.spanMap.size() == 4) {
        CHECK(context.spanMap[0] == 10.0);
        CHECK(context.spanMap[1] == 5000.0);
        CHECK(context.spanMap[2] == 10000.0);
        CHECK(context.spanMap[3] == 10000.0);
    }
    CHECK(context.scalars.contains(".min_dn"));
    CHECK(context.scalars.contains(".max_dn"));
    CHECK(context.scalars.contains(".span_dn"));
    if (context.scalars.contains(".min_dn")) CHECK(context.scalars.at(".min_dn") == 0.0);
    if (context.scalars.contains(".max_dn")) CHECK(context.scalars.at(".max_dn") == 210000.0);
    if (context.scalars.contains(".span_dn")) CHECK(context.scalars.at(".span_dn") == 210000.0);

    context.array.byte_size = sizeof(pixels) - 1;
    context.spanMap.clear();
    context.scalars.clear();
    CHECK(pixelSpan->execute(&callbacks, IRGeometry{2, 2, 20}, "{}", err, sizeof err) == IR_BAD_ARGUMENT);
}

void check(bool cond, const char *label, int line)
{
    if (!cond) {
        std::cerr << "FAIL line " << line << ": " << label << '\n';
        ++gFailures;
    }
}

std::string utf8(const fs::path &p)
{
    const auto u = p.u8string();
    return {reinterpret_cast<const char *>(u.data()), u.size()};
}

fs::path fromUtf8(const std::string &s)
{
    return fs::path(std::u8string(s.begin(), s.end()));
}

std::string readText(const fs::path &p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

class TempDirGuard {
public:
    explicit TempDirGuard(const std::string &suffixUtf8)
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::string name = "ir_plugin_test_" + suffixUtf8 + "_" + std::to_string(stamp);
        dir_ = fs::temp_directory_path() / fromUtf8(name);
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

void write2x2Fixture(const fs::path &dir, int32_t framesCount = 2)
{
    fs::create_directories(dir);
    std::vector<uint16_t> pixels;
    for (int32_t f = 0; f < framesCount; ++f) {
        const uint16_t base = static_cast<uint16_t>(f * 2);
        pixels.push_back(static_cast<uint16_t>(20 + base));
        pixels.push_back(static_cast<uint16_t>(40 + base));
        pixels.push_back(static_cast<uint16_t>(60 + base));
        pixels.push_back(static_cast<uint16_t>(80 + base));
    }
    {
        std::ofstream raw(dir / "frames.raw", std::ios::binary);
        raw.write(reinterpret_cast<const char *>(pixels.data()),
                  std::streamsize(pixels.size() * sizeof(uint16_t)));
    }
    {
        std::ofstream csv(dir / "frames.csv", std::ios::binary);
        csv << "frame_index,width,height,stride_bytes,pixel_format,bit_depth,endianness\n";
        for (int32_t f = 0; f < framesCount; ++f)
            csv << f << ",2,2,4,RAW16_LE,16,little\n";
    }
}

void writeU32Fixture(const fs::path &dir)
{
    fs::create_directories(dir);
    const uint32_t pixels[]{70000, 71000, 72000, 73000,
                            70002, 71002, 72002, 73002};
    {
        std::ofstream raw(dir / "frames.raw", std::ios::binary);
        raw.write(reinterpret_cast<const char *>(pixels), sizeof(pixels));
    }
    {
        std::ofstream csv(dir / "frames.csv", std::ios::binary);
        csv << "frame_index,width,height,stride_bytes,pixel_format,bit_depth,endianness\n"
            << "0,2,2,8,RAW32_LE,20,little\n"
            << "1,2,2,8,RAW32_LE,20,little\n";
    }
}

std::string pluginFilename(const char *stem)
{
#ifdef _WIN32
    return std::string(stem) + ".dll";
#else
    return std::string("lib") + stem + ".so";
#endif
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t **argv)
#else
int main(int argc, char **argv)
#endif
{
    if (argc < 2) {
        std::cerr << "Usage: plugin_abi_test <plugin_build_dir>\n";
        return 2;
    }

    try {
    testU32PluginView();
    const fs::path srcPluginDir(argv[1]);
    const fs::path srcGood = srcPluginDir / pluginFilename("ir_test_plugin_good");
    const fs::path srcBad = srcPluginDir / pluginFilename("ir_test_plugin_bad_abi");
    const fs::path srcDuplicate = srcPluginDir / pluginFilename("ir_test_plugin_duplicate");
    CHECK(fs::is_regular_file(srcGood));
    CHECK(fs::is_regular_file(srcBad));
    CHECK(fs::is_regular_file(srcDuplicate));

    // Chinese + spaces directory to verify UTF-8 path handling across Windows & Linux.
    TempDirGuard root("\xE6\xB5\x8B\xE8\xAF\x95 \xE6\x8F\x92\xE4\xBB\xB6 \xE7\x9B\xAE\xE5\xBD\x95");
    const fs::path fixture = root.path() / "fixture";
    write2x2Fixture(fixture, 6);

    const fs::path goodPluginDir = root.path() / fromUtf8("\xE6\xAD\xA3\xE5\xB8\xB8 \xE6\x8F\x92\xE4\xBB\xB6");
    const fs::path badPluginDir = root.path() / fromUtf8("\xE5\xBC\x82\xE5\xB8\xB8 \xE6\x8F\x92\xE4\xBB\xB6");
    fs::create_directories(goodPluginDir);
    fs::create_directories(badPluginDir);
    const fs::path goodCopy = goodPluginDir / srcGood.filename();
    const fs::path badCopy = badPluginDir / srcBad.filename();
    fs::copy_file(srcGood, goodCopy, fs::copy_options::overwrite_existing);
    fs::copy_file(srcBad, badCopy, fs::copy_options::overwrite_existing);

    const Json stationGood{
        {"schemav", 1},
        {"acquisition", {
            {"backend", "raw_replay"},
            {"source", utf8(fixture)},
            {"valid_bits", 16},
            {"queue_capacity", 16}
        }},
        {"plugins", Json::array({utf8(goodPluginDir)})}
    };
    const std::string stGood = stationGood.dump();

    const Json stationBad{
        {"schemav", 1},
        {"acquisition", {
            {"backend", "raw_replay"},
            {"source", utf8(fixture)},
            {"valid_bits", 16}
        }},
        {"plugins", Json::array({utf8(badCopy)})}
    };

    const Json stationNoPlugins{
        {"schemav", 1},
        {"acquisition", {
            {"backend", "raw_replay"},
            {"source", utf8(fixture)},
            {"valid_bits", 16}
        }}
    };

    char err[2048]{};

    // 1. ABI mismatch rejection
    const Json minPluginRecipe{
        {"schemav", 1},
        {"recipe_id", "bad-abi-check"},
        {"steps", Json::array({
            {{"id", "cap"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}},
            {{"id", "an"}, {"type", "analyze"}, {"method", "plugin:test.bad_abi"}, {"input", "cap"}}
        })}
    };
    CHECK(IR_CheckRecipeJson(stationBad.dump().c_str(), minPluginRecipe.dump().c_str(),
                             err, sizeof err) == IR_UNSUPPORTED);

    const Json duplicateStation{
        {"schemav", 1},
        {"acquisition", {
            {"backend", "raw_replay"},
            {"source", utf8(fixture)},
            {"valid_bits", 16}
        }},
        {"plugins", Json::array({utf8(srcDuplicate)})}
    };
    CHECK(IR_CheckRecipeJson(duplicateStation.dump().c_str(), minPluginRecipe.dump().c_str(),
                             err, sizeof err) == IR_BAD_ARGUMENT);
    if (std::string(err).find("Duplicate plugin method name") == std::string::npos)
        std::cerr << "duplicate plugin check error=" << err << '\n';
    CHECK(std::string(err).find("Duplicate plugin method name") != std::string::npos);

    // 2. Missing station.plugins rejection & preflight checks
    const Json goodPassRecipe{
        {"schemav", 1},
        {"recipe_id", "plugin-pixel-span-pass"},
        {"steps", Json::array({
            {{"id", "cap"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}, {"output", "b0"}},
            {{"id", "ps"}, {"type", "analyze"}, {"method", "plugin:test.pixel_span"},
             {"name", "span_step"}, {"input", "b0"},
             {"params", {{"min_frames", 2}, {"min_width", 2}}},
             {"outputs", {{"span_map", "smap"}}}},
            {{"id", "ms"}, {"type", "analyze"}, {"method", "map_summary"},
             {"name", "span_sum"}, {"input", "smap"}}
        })},
        {"limits", Json::array({
            {{"metric", "span_step.span_dn"}, {"operator", "max"}, {"value", 70.0}, {"unit", "DN"}},
            {{"metric", "span_sum.mean"}, {"operator", "min"}, {"value", 1.5}, {"unit", "DN"}}
        })}
    };
    CHECK(IR_CheckRecipeJson(stationNoPlugins.dump().c_str(), goodPassRecipe.dump().c_str(),
                             err, sizeof err) == IR_BAD_ARGUMENT);
    CHECK(IR_CheckRecipeJson(stGood.c_str(), goodPassRecipe.dump().c_str(),
                             err, sizeof err) == IR_OK);

    // Unknown input field, wrong version, unit mismatch
    {
        Json badInput = goodPassRecipe;
        badInput["steps"][1]["inputs"] = {{"unknown_key", "b0"}};
        CHECK(IR_CheckRecipeJson(stGood.c_str(), badInput.dump().c_str(), err, sizeof err) == IR_BAD_ARGUMENT);

        Json badVer = goodPassRecipe;
        badVer["steps"][1]["version"] = 2;
        CHECK(IR_CheckRecipeJson(stGood.c_str(), badVer.dump().c_str(), err, sizeof err) == IR_BAD_ARGUMENT);

        Json badUnit = goodPassRecipe;
        badUnit["limits"][0]["unit"] = "mK";
        CHECK(IR_CheckRecipeJson(stGood.c_str(), badUnit.dump().c_str(), err, sizeof err) == IR_BAD_ARGUMENT);
    }

    // 3. End-to-end PASS + NG + plugin provenance + step_records
    const fs::path outPass = root.path() / "out_pass";
    IRRecipeResult rr{};
    const auto checkPreparationFailure = [&](const Json &station, const Json &recipe,
                                               const char *outputName, int32_t expectedStatus) {
        const auto output = root.path() / outputName;
        CHECK(IR_RunRecipeJson(station.dump().c_str(), recipe.dump().c_str(), utf8(output).c_str(),
                               nullptr, &rr, err, sizeof err) == expectedStatus);
        CHECK(rr.failed_stage == IR_RECIPE_STAGE_VALIDATE && rr.completed_frames == 0);
        const auto diagnostic = Json::parse(readText(output / "result.json"));
        CHECK(diagnostic.at("run_status") == "ERROR");
        CHECK(diagnostic.at("error_code") == expectedStatus);
        CHECK(diagnostic.at("failed_stage") == IR_RECIPE_STAGE_VALIDATE);
    };
    checkPreparationFailure(stationBad, minPluginRecipe, "out_load_fail", IR_UNSUPPORTED);
    Json rejectedParams = goodPassRecipe;
    rejectedParams["steps"][1]["params"]["min_frames"] = 0;
    checkPreparationFailure(stationGood, rejectedParams, "out_prepare_fail", IR_BAD_ARGUMENT);

    const int32_t runStatus = IR_RunRecipeJson(stGood.c_str(), goodPassRecipe.dump().c_str(), utf8(outPass).c_str(), nullptr, &rr, err, sizeof err);
    if (runStatus != IR_OK)
        std::cerr << "good plugin recipe run failed status=" << runStatus << " error=" << err << '\n';
    CHECK(runStatus == IR_OK);
    if (runStatus != IR_OK)
        return 1;
    CHECK(rr.cleanup_status == IR_OK && rr.completed_frames == 2);
    const Json recPass = Json::parse(readText(outPass / "result.json"));
    CHECK(recPass.at("run_status") == "PASS");
    CHECK(std::abs(recPass.at("metrics").at("span_step.min_dn").at("value").get<double>() - 20.0) < 1e-12);
    CHECK(std::abs(recPass.at("metrics").at("span_step.max_dn").at("value").get<double>() - 82.0) < 1e-12);
    CHECK(std::abs(recPass.at("metrics").at("span_step.span_dn").at("value").get<double>() - 62.0) < 1e-12);
    CHECK(std::abs(recPass.at("metrics").at("span_sum.mean").at("value").get<double>() - 2.0) < 1e-12);
    const auto &mSpan = recPass.at("metrics").at("span_step.span_dn");
    CHECK(mSpan.at("plugin_id") == "ir.test.suite");
    CHECK(mSpan.at("plugin_file") == pluginFilename("ir_test_plugin_good"));
    CHECK(!mSpan.contains("plugin_sha256"));
    CHECK(!mSpan.contains("library_path"));
    CHECK(recPass.at("step_records").size() == 3);

    // U32 replay must reach the real plugin bridge without narrowing its pixels.
    const fs::path u32Fixture = root.path() / "fixture_u32";
    writeU32Fixture(u32Fixture);
    Json stationU32 = stationGood;
    stationU32["acquisition"]["source"] = utf8(u32Fixture);
    stationU32["acquisition"]["valid_bits"] = 20;
    const Json recipeU32{
        {"schemav", 1},
        {"recipe_id", "plugin-u32-replay"},
        {"steps", Json::array({
            {{"id", "cap"}, {"type", "capture"}, {"frames", 2},
             {"frame_timeout_ms", 0}, {"output", "u32_batch"}},
            {{"id", "span"}, {"type", "analyze"}, {"method", "plugin:test.pixel_span"},
             {"name", "u32_span"}, {"input", "u32_batch"},
             {"params", {{"min_frames", 2}, {"min_width", 2}}}}
        })}
    };
    CHECK(IR_CheckRecipeJson(stationU32.dump().c_str(), recipeU32.dump().c_str(),
                             err, sizeof err) == IR_OK);
    const fs::path outU32 = root.path() / "out_u32";
    const auto u32Status = IR_RunRecipeJson(stationU32.dump().c_str(), recipeU32.dump().c_str(),
        utf8(outU32).c_str(), nullptr, &rr, err, sizeof err);
    if (u32Status != IR_OK)
        std::cerr << "U32 plugin recipe failed status=" << u32Status << " error=" << err << '\n';
    CHECK(u32Status == IR_OK);
    if (u32Status != IR_OK)
        return 1;
    const Json recU32 = Json::parse(readText(outU32 / "result.json"));
    CHECK(recU32.at("data_type") == "U32");
    CHECK(recU32.at("valid_bits") == 20);
    CHECK(std::abs(recU32.at("metrics").at("u32_span.min_dn").at("value").get<double>() - 70000.0) < 1e-12);
    CHECK(std::abs(recU32.at("metrics").at("u32_span.max_dn").at("value").get<double>() - 73002.0) < 1e-12);
    CHECK(std::abs(recU32.at("metrics").at("u32_span.span_dn").at("value").get<double>() - 3002.0) < 1e-12);

    Json ngRecipe = goodPassRecipe;
    ngRecipe["limits"][0]["value"] = 50.0;
    const fs::path outNg = root.path() / "out_ng";
    CHECK(IR_RunRecipeJson(stGood.c_str(), ngRecipe.dump().c_str(), utf8(outNg).c_str(), nullptr, &rr, err, sizeof err) == IR_OK);
    CHECK(Json::parse(readText(outNg / "result.json")).at("run_status") == "NG");

    // 4. repeat_burst reuses its output slot under a tight memory budget.
    // sizeof(IRFrameStamp) == 40: 2x2x2 batch = 8*2 + 2*40 = 96 B; queue(2) = 4*8 + 2*40 = 112 B;
    // span_map = 4*8 = 32 B. The capture batch and output map fit within 240 B.
    Json tightStation = stationGood;
    tightStation["acquisition"]["queue_capacity"] = 2;
    tightStation["acquisition"]["max_inflight_bytes"] = 240;
    const Json burstRecipe{
        {"schemav", 1},
        {"recipe_id", "plugin-burst-overwrite"},
        {"steps", Json::array({
            {{"id", "lp"}, {"type", "repeat_burst"}, {"max_iterations", 3},
             {"record_metrics", Json::array({"p.span_dn"})},
             {"steps", Json::array({
                 {{"id", "c"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}, {"output", "b"}},
                 {{"id", "p"}, {"type", "analyze"}, {"method", "plugin:test.pixel_span"},
                  {"name", "p"}, {"input", "b"},
                  {"outputs", {{"span_map", "smap"}}}}
             })}}
        })}
    };
    const fs::path outBurst = root.path() / "out_burst";
    CHECK(IR_RunRecipeJson(tightStation.dump().c_str(), burstRecipe.dump().c_str(), utf8(outBurst).c_str(), nullptr, &rr, err, sizeof err) == IR_OK);
    CHECK(rr.completed_frames == 6);

    // One 5x1 output slot, with two captured frames and a queue of two.
    const fs::path resizeFixture = root.path() / "resize_fixture";
    fs::create_directories(resizeFixture);
    {
        const uint16_t pixels[]{10, 20, 30, 40, 50, 12, 22, 32, 42, 52};
        std::ofstream raw(resizeFixture / "frames.raw", std::ios::binary);
        raw.write(reinterpret_cast<const char *>(pixels), sizeof(pixels));
        std::ofstream csv(resizeFixture / "frames.csv", std::ios::binary);
        csv << "width,height,pixel_format\n5,1,RAW16_LE\n";
    }
    auto resizeStation = tightStation;
    resizeStation["acquisition"]["source"] = utf8(resizeFixture);
    const Json resizeCapture{{"id", "cap"}, {"type", "capture"}, {"frames", 2},
                             {"frame_timeout_ms", 0}, {"output", "b"}};
    const uint64_t captureBytes = (2 + 2 + 2) * 5 * sizeof(uint16_t) + 4 * sizeof(IRFrameStamp);
    const auto runResize = [&](const Json &recipe, uint64_t budget, const std::string &outputName) {
        resizeStation["acquisition"]["max_inflight_bytes"] = budget;
        return IR_RunRecipeJson(resizeStation.dump().c_str(), recipe.dump().c_str(),
            utf8(root.path() / outputName).c_str(), nullptr, &rr, err, sizeof err);
    };
    for (const bool useMask : {false, true}) {
        const std::string key = useMask ? "span_mask" : "span_map";
        const auto resizeStep = [&](const char *id, int32_t width) -> Json {
            return {{"id", id}, {"type", "analyze"}, {"method", "plugin:test.pixel_span"},
                    {"input", "b"}, {"params", {{"out_width", width}}},
                    {"outputs", {{key, "resized"}}}};
        };
        const Json growth{{"recipe_id", "slot-growth"}, {"steps", Json::array({
            resizeCapture, resizeStep("first", 4), resizeStep("grow", 5)
        })}};
        const Json reuse{{"recipe_id", "slot-reuse"}, {"steps", Json::array({
            resizeCapture, resizeStep("first", 5), resizeStep("shrink", 2), resizeStep("restore", 5)
        })}};
        const uint64_t itemBytes = useMask ? sizeof(uint8_t) : sizeof(double);
        const uint64_t steadyBytes = captureBytes + 5 * itemBytes;
        const uint64_t growthPeak = steadyBytes + 4 * itemBytes;
        CHECK(runResize(growth, growthPeak - 1, key + "_grow_reject") == IR_BAD_ARGUMENT);
        CHECK(rr.failed_stage == IR_RECIPE_STAGE_ANALYZE && rr.failed_step_index == 2);
        CHECK(std::string(err).find("frame storage " + std::to_string(growthPeak)) != std::string::npos);
        CHECK(runResize(growth, growthPeak, key + "_grow_fit") == IR_OK);
        CHECK(runResize(reuse, steadyBytes, key + "_reuse_fit") == IR_OK);
    }

    // 4b. Heterogeneous output dimensions under the conservative 272 B preflight budget.
    const Json heteroRecipe{
        {"schemav", 1},
        {"recipe_id", "plugin-hetero-map-add"},
        {"steps", Json::array({
            {{"id", "c"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}, {"output", "b"}},
            {{"id", "ps"}, {"type", "analyze"}, {"method", "plugin:test.pixel_span"},
             {"input", "b"}, {"params", {{"out_width", 1}, {"out_height", 2}}},
             {"outputs", {{"span_map", "bm"}}}},
            {{"id", "add"}, {"type", "analyze"}, {"method", "plugin:test.map_add"},
             {"name", "add"}, {"inputs", {{"in_map", "bm"}}}, {"outputs", {{"out_map", "bm_out"}}}}
        })}
    };
    const fs::path outHetero = root.path() / "out_hetero";
    Json heteroStation = tightStation;
    // Preflight conservatively budgets each declared map at full frame geometry.
    heteroStation["acquisition"]["max_inflight_bytes"] = 272;
    const int32_t heteroStatus = IR_RunRecipeJson(heteroStation.dump().c_str(), heteroRecipe.dump().c_str(), utf8(outHetero).c_str(), nullptr, &rr, err, sizeof err);
    if (heteroStatus != IR_OK)
        std::cerr << "heterogeneous plugin recipe run failed status=" << heteroStatus << " error=" << err << '\n';
    CHECK(heteroStatus == IR_OK);
    if (heteroStatus != IR_OK)
        return 1;
    CHECK(std::abs(Json::parse(readText(outHetero / "result.json")).at("metrics").at("add.mean_out").at("value").get<double>() - 12.0) < 1e-12);

    // 5. Runtime over-budget plugin allocation inside execute() rejected at ANALYZE with IR_BAD_ARGUMENT
    Json tinyStation = stationGood;
    tinyStation["acquisition"]["queue_capacity"] = 2;
    tinyStation["acquisition"]["max_inflight_bytes"] = 272; // 112+96+64=272 passes PREPARE; 4x4 big_map2 (+128=368 > 272) rejected
    const Json overBudgetRecipe{
        {"schemav", 1},
        {"recipe_id", "plugin-over-budget"},
        {"steps", Json::array({
            {{"id", "c"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}, {"output", "b"}},
            {{"id", "ob"}, {"type", "analyze"}, {"method", "plugin:test.over_budget"},
             {"input", "b"}, {"outputs", {{"big_map", "bm"}, {"big_map2", "bm2"}}}}
        })}
    };
    const fs::path outBudget = root.path() / "out_budget";
    CHECK(IR_RunRecipeJson(tinyStation.dump().c_str(), overBudgetRecipe.dump().c_str(), utf8(outBudget).c_str(), nullptr, &rr, err, sizeof err) == IR_BAD_ARGUMENT);
    CHECK(rr.failed_stage == IR_RECIPE_STAGE_ANALYZE);
    CHECK(rr.completed_frames == 2);
    CHECK(Json::parse(readText(outBudget / "result.json")).at("run_status") == "ERROR");

    // A plugin that ignores a failed repeated allocation still fails its execute() call.
    const Json duplicateAllocRecipe{
        {"schemav", 1},
        {"recipe_id", "plugin-duplicate-alloc"},
        {"steps", Json::array({
            {{"id", "cap"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}, {"output", "b0"}},
            {{"id", "dup"}, {"type", "analyze"}, {"method", "plugin:test.duplicate_alloc"},
             {"input", "b0"}, {"outputs", {{"map", "duplicate_map"}}}}
        })}
    };
    const fs::path outDuplicateAlloc = root.path() / "out_duplicate_alloc";
    CHECK(IR_RunRecipeJson(stGood.c_str(), duplicateAllocRecipe.dump().c_str(),
                           utf8(outDuplicateAlloc).c_str(), nullptr, &rr, err, sizeof err) == IR_BAD_ARGUMENT);
    CHECK(rr.failed_stage == IR_RECIPE_STAGE_ANALYZE);
    CHECK(std::string(err).find("already allocated") != std::string::npos);

    // 6. C++ exception isolation -> IR_COMPUTATION_FAILED
    const Json throwRecipe{
        {"schemav", 1},
        {"recipe_id", "plugin-throw-cxx"},
        {"steps", Json::array({
            {{"id", "c"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}, {"output", "b"}},
            {{"id", "th"}, {"type", "analyze"}, {"method", "plugin:test.throw_cxx"}, {"input", "b"}}
        })}
    };
    const fs::path outThrow = root.path() / "out_throw";
    CHECK(IR_RunRecipeJson(stGood.c_str(), throwRecipe.dump().c_str(), utf8(outThrow).c_str(), nullptr, &rr, err, sizeof err) == IR_COMPUTATION_FAILED);
    CHECK(rr.failed_stage == IR_RECIPE_STAGE_ANALYZE);
    CHECK(rr.failed_step_index == 1);
    CHECK(rr.cleanup_status == IR_OK);
    CHECK(std::string(err).find("intentional plugin C++ exception") != std::string::npos);

    // 7. Cross-thread IR_RequestStop cancellation via IR_RunRecipeJsonEx event callback
    const Json slowRecipe{
        {"schemav", 1},
        {"recipe_id", "plugin-slow-cancel"},
        {"steps", Json::array({
            {{"id", "c"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}, {"output", "b"}},
            {{"id", "sl"}, {"type", "analyze"}, {"method", "plugin:test.slow_cancel"}, {"input", "b"}}
        })}
    };
    struct CancelContext {
        std::atomic<bool> sawSlowStart{false};
        std::atomic<bool> sawSlowFailed{false};
        std::thread stopper;
    } cancelCtx;
    auto onStep = [](const IRRecipeStepEvent *ev, void *ud) {
        auto *c = static_cast<CancelContext *>(ud);
        if (!ev || !ev->step_id_utf8) return;
        if (std::string(ev->step_id_utf8) == "sl") {
            if (ev->event_kind == IR_STEP_EVENT_STARTED && !c->sawSlowStart.exchange(true)) {
                c->stopper = std::thread([] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    IR_RequestStop();
                });
            } else if (ev->event_kind == IR_STEP_EVENT_FAILED && ev->status == IR_CANCELLED) {
                c->sawSlowFailed.store(true);
            }
        }
    };
    IRRecipeRunOptions opts{sizeof(IRRecipeRunOptions), 0, onStep, &cancelCtx};
    const fs::path outCancel = root.path() / "out_cancel";
    const int32_t cancelStatus = IR_RunRecipeJson(
        stGood.c_str(), slowRecipe.dump().c_str(), utf8(outCancel).c_str(),
        &opts, &rr, err, sizeof err);
    if (cancelCtx.stopper.joinable())
        cancelCtx.stopper.join();
    CHECK(cancelStatus == IR_CANCELLED);
    CHECK(rr.cleanup_status == IR_OK);
    CHECK(cancelCtx.sawSlowStart.load());
    CHECK(cancelCtx.sawSlowFailed.load());
    CHECK(Json::parse(readText(outCancel / "result.json")).at("run_status") == "CANCELLED");

    // 8. Same-step input/output aliasing and duplicate output slot names are rejected.
    const Json aliasRecipe{
        {"schemav", 1},
        {"recipe_id", "plugin-alias-safe"},
        {"steps", Json::array({
            {{"id", "cap"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}, {"output", "b0"}},
            {{"id", "ps"}, {"type", "analyze"}, {"method", "plugin:test.pixel_span"},
             {"name", "ps"}, {"input", "b0"}, {"outputs", {{"span_map", "smap"}}}},
            {{"id", "add"}, {"type", "analyze"}, {"method", "plugin:test.map_add"},
             {"name", "add"}, {"inputs", {{"in_map", "smap"}}}, {"outputs", {{"out_map", "smap"}}}}
        })}
    };
    CHECK(IR_CheckRecipeJson(stGood.c_str(), aliasRecipe.dump().c_str(),
                             err, sizeof err) == IR_BAD_ARGUMENT);
    CHECK(std::string(err).find("aliases input") != std::string::npos);

    const Json dupOutputRecipe{
        {"schemav", 1},
        {"recipe_id", "plugin-dup-out"},
        {"steps", Json::array({
            {{"id", "cap"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}, {"output", "b0"}},
            {{"id", "ob"}, {"type", "analyze"}, {"method", "plugin:test.over_budget"},
             {"input", "b0"}, {"outputs", {{"big_map", "dup"}, {"big_map2", "dup"}}}}
        })}
    };
    CHECK(IR_CheckRecipeJson(stGood.c_str(), dupOutputRecipe.dump().c_str(),
                             err, sizeof err) == IR_BAD_ARGUMENT);

    // 9. Non-finite scalar emission rejected with IR_COMPUTATION_FAILED
    const Json nanRecipe{
        {"schemav", 1},
        {"recipe_id", "plugin-emit-nan"},
        {"steps", Json::array({
            {{"id", "cap"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}, {"output", "b0"}},
            {{"id", "nn"}, {"type", "analyze"}, {"method", "plugin:test.emit_nan"}, {"input", "b0"}}
        })}
    };
    const fs::path outNan = root.path() / "out_nan";
    CHECK(IR_RunRecipeJson(stGood.c_str(), nanRecipe.dump().c_str(), utf8(outNan).c_str(), nullptr, &rr, err, sizeof err) == IR_COMPUTATION_FAILED);
    CHECK(Json::parse(readText(outNan / "result.json")).at("run_status") == "ERROR");

    if (gFailures == 0)
        std::cout << "plugin_abi_test: ALL PASSED\n";
    return gFailures == 0 ? 0 : 1;
    } catch (const std::exception &e) {
        std::cerr << "UNHANDLED exception in plugin_abi_test: " << e.what() << '\n';
        return 1;
    }
}
