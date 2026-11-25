#include "IR_recipe.h"
#include "IR_station.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
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

fs::path fromUtf8(const std::string &s)
{
    return fs::path(std::u8string(s.begin(), s.end()));
}

std::string readText(const fs::path &p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

bool isHex64(const std::string &s)
{
    if (s.size() != 64) return false;
    for (char ch : s) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
    }
    return true;
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
    const fs::path srcPluginDir(argv[1]);
    const fs::path srcGood = srcPluginDir / pluginFilename("ir_test_plugin_good");
    const fs::path srcBad = srcPluginDir / pluginFilename("ir_test_plugin_bad_abi");
    CHECK(fs::is_regular_file(srcGood));
    CHECK(fs::is_regular_file(srcBad));

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
        {"schema_version", 1},
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
        {"schema_version", 1},
        {"acquisition", {
            {"backend", "raw_replay"},
            {"source", utf8(fixture)},
            {"valid_bits", 16}
        }},
        {"plugins", Json::array({utf8(badCopy)})}
    };

    const Json stationNoPlugins{
        {"schema_version", 1},
        {"acquisition", {
            {"backend", "raw_replay"},
            {"source", utf8(fixture)},
            {"valid_bits", 16}
        }}
    };

    char err[2048]{};

    // 1. ABI mismatch rejection
    const Json minPluginRecipe{
        {"schema_version", 2},
        {"recipe_id", "bad-abi-check"},
        {"steps", Json::array({
            {{"id", "cap"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}},
            {{"id", "an"}, {"type", "analyze"}, {"method", "plugin:test.bad_abi"}, {"input", "cap"}}
        })}
    };
    CHECK(IR_CheckRecipeJson(stationBad.dump().c_str(), minPluginRecipe.dump().c_str(),
                             err, sizeof err) == IR_UNSUPPORTED);

    // 2. Missing station.plugins rejection & preflight checks
    const Json goodPassRecipe{
        {"schema_version", 2},
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
    CHECK(IR_RunRecipeJson(stGood.c_str(), goodPassRecipe.dump().c_str(),
                           utf8(outPass).c_str(), &rr, err, sizeof err) == IR_OK);
    CHECK(rr.cleanup_status == IR_OK && rr.completed_frames == 2);
    const Json recPass = Json::parse(readText(outPass / "result.json"));
    CHECK(recPass.at("run_status") == "PASS");
    CHECK(std::abs(recPass.at("metrics").at("span_step.min_dn").at("value").get<double>() - 20.0) < 1e-12);
    CHECK(std::abs(recPass.at("metrics").at("span_step.max_dn").at("value").get<double>() - 82.0) < 1e-12);
    CHECK(std::abs(recPass.at("metrics").at("span_step.span_dn").at("value").get<double>() - 62.0) < 1e-12);
    CHECK(std::abs(recPass.at("metrics").at("span_sum.mean").at("value").get<double>() - 2.0) < 1e-12);
    const auto &mSpan = recPass.at("metrics").at("span_step.span_dn");
    CHECK(mSpan.at("plugin_id") == "ir.test.suite");
    CHECK(mSpan.at("plugin_version") == "1.2.3");
    CHECK(mSpan.at("plugin_file") == pluginFilename("ir_test_plugin_good"));
    CHECK(isHex64(mSpan.at("plugin_sha256").get<std::string>()));
    CHECK(!mSpan.contains("library_path"));
    CHECK(recPass.at("step_records").size() == 3);

    Json ngRecipe = goodPassRecipe;
    ngRecipe["limits"][0]["value"] = 50.0;
    const fs::path outNg = root.path() / "out_ng";
    CHECK(IR_RunRecipeJson(stGood.c_str(), ngRecipe.dump().c_str(),
                           utf8(outNg).c_str(), &rr, err, sizeof err) == IR_OK);
    CHECK(Json::parse(readText(outNg / "result.json")).at("run_status") == "NG");

    // 4. repeat_burst reusing span_map under tight max_inflight_bytes (must not double-charge)
    // sizeof(IRFrameStamp) == 40: 2x2x2 batch = 8*2 + 2*40 = 96 B; queue(2) = 4*8 + 2*40 = 112 B;
    // span_map = 4*8 = 32 B. During HostCallbackBridge staging on iter>=2: 112 + 96 + 32 + 32 = 272 B.
    Json tightStation = stationGood;
    tightStation["acquisition"]["queue_capacity"] = 2;
    tightStation["acquisition"]["max_inflight_bytes"] = 272;
    const Json burstRecipe{
        {"schema_version", 2},
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
    CHECK(IR_RunRecipeJson(tightStation.dump().c_str(), burstRecipe.dump().c_str(),
                           utf8(outBurst).c_str(), &rr, err, sizeof err) == IR_OK);
    CHECK(rr.completed_frames == 6);

    // 5. Runtime over-budget plugin allocation inside execute() rejected at ANALYZE with IR_BAD_ARGUMENT
    Json tinyStation = stationGood;
    tinyStation["acquisition"]["queue_capacity"] = 2;
    tinyStation["acquisition"]["max_inflight_bytes"] = 256; // 112+96+32=240 passes PREPARE & ps, staging in_place_add needs 272 > 256
    const Json overBudgetRecipe{
        {"schema_version", 2},
        {"recipe_id", "plugin-over-budget"},
        {"steps", Json::array({
            {{"id", "c"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}, {"output", "b"}},
            {{"id", "ps"}, {"type", "analyze"}, {"method", "plugin:test.pixel_span"},
             {"input", "b"}, {"outputs", {{"span_map", "bm"}}}},
            {{"id", "ob"}, {"type", "analyze"}, {"method", "plugin:test.in_place_add"},
             {"inputs", {{"in_map", "bm"}}}, {"outputs", {{"out_map", "bm"}}}}
        })}
    };
    const fs::path outBudget = root.path() / "out_budget";
    CHECK(IR_RunRecipeJson(tinyStation.dump().c_str(), overBudgetRecipe.dump().c_str(),
                           utf8(outBudget).c_str(), &rr, err, sizeof err) == IR_BAD_ARGUMENT);
    CHECK(rr.failed_stage == IR_RECIPE_STAGE_ANALYZE);
    CHECK(rr.completed_frames == 2);
    CHECK(!fs::exists(outBudget));

    // 6. C++ exception isolation -> IR_COMPUTATION_FAILED
    const Json throwRecipe{
        {"schema_version", 2},
        {"recipe_id", "plugin-throw-cxx"},
        {"steps", Json::array({
            {{"id", "c"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}, {"output", "b"}},
            {{"id", "th"}, {"type", "analyze"}, {"method", "plugin:test.throw_cxx"}, {"input", "b"}}
        })}
    };
    const fs::path outThrow = root.path() / "out_throw";
    CHECK(IR_RunRecipeJson(stGood.c_str(), throwRecipe.dump().c_str(),
                           utf8(outThrow).c_str(), &rr, err, sizeof err) == IR_COMPUTATION_FAILED);
    CHECK(rr.failed_stage == IR_RECIPE_STAGE_ANALYZE);
    CHECK(rr.failed_step_index == 1);
    CHECK(rr.cleanup_status == IR_OK);
    CHECK(std::string(err).find("intentional plugin C++ exception") != std::string::npos);

    // 7. Cross-thread IR_RequestStop cancellation via IR_RunRecipeJsonEx event callback
    const Json slowRecipe{
        {"schema_version", 2},
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
    const int32_t cancelStatus = IR_RunRecipeJsonEx(
        stGood.c_str(), slowRecipe.dump().c_str(), utf8(outCancel).c_str(),
        &opts, &rr, err, sizeof err);
    if (cancelCtx.stopper.joinable())
        cancelCtx.stopper.join();
    CHECK(cancelStatus == IR_CANCELLED);
    CHECK(rr.cleanup_status == IR_OK);
    CHECK(cancelCtx.sawSlowStart.load());
    CHECK(cancelCtx.sawSlowFailed.load());
    CHECK(!fs::exists(outCancel));

    // 8. Same-step input/output slot aliasing safety & duplicate output slot rejection
    const Json aliasRecipe{
        {"schema_version", 2},
        {"recipe_id", "plugin-alias-safe"},
        {"steps", Json::array({
            {{"id", "cap"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}, {"output", "b0"}},
            {{"id", "ps"}, {"type", "analyze"}, {"method", "plugin:test.pixel_span"},
             {"name", "ps"}, {"input", "b0"}, {"outputs", {{"span_map", "smap"}}}},
            {{"id", "add"}, {"type", "analyze"}, {"method", "plugin:test.in_place_add"},
             {"name", "add"}, {"inputs", {{"in_map", "smap"}}}, {"outputs", {{"out_map", "smap"}}}}
        })}
    };
    const fs::path outAlias = root.path() / "out_alias";
    CHECK(IR_RunRecipeJson(stGood.c_str(), aliasRecipe.dump().c_str(),
                           utf8(outAlias).c_str(), &rr, err, sizeof err) == IR_OK);
    const Json recAlias = Json::parse(readText(outAlias / "result.json"));
    CHECK(std::abs(recAlias.at("metrics").at("add.mean_out").at("value").get<double>() - 12.0) < 1e-12);

    const Json dupOutputRecipe{
        {"schema_version", 2},
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
        {"schema_version", 2},
        {"recipe_id", "plugin-emit-nan"},
        {"steps", Json::array({
            {{"id", "cap"}, {"type", "capture"}, {"frames", 2}, {"frame_timeout_ms", 0}, {"output", "b0"}},
            {{"id", "nn"}, {"type", "analyze"}, {"method", "plugin:test.emit_nan"}, {"input", "b0"}}
        })}
    };
    const fs::path outNan = root.path() / "out_nan";
    CHECK(IR_RunRecipeJson(stGood.c_str(), nanRecipe.dump().c_str(),
                           utf8(outNan).c_str(), &rr, err, sizeof err) == IR_COMPUTATION_FAILED);
    CHECK(!fs::exists(outNan));

    if (gFailures == 0)
        std::cout << "plugin_abi_test: ALL PASSED\n";
    return gFailures == 0 ? 0 : 1;
    } catch (const std::exception &e) {
        std::cerr << "UNHANDLED exception in plugin_abi_test: " << e.what() << '\n';
        return 1;
    }
}
