#include "IR_station.h"
#include "IR_analysis.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace
{

int gFailures = 0;

void checkTrue(bool cond, const char *expr, int line)
{
    if (!cond)
    {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expr);
        ++gFailures;
    }
}

#define CHECK(cond) checkTrue((cond), #cond, __LINE__)

struct TempDirGuard
{
    std::filesystem::path dir;
    explicit TempDirGuard(std::filesystem::path p) : dir(std::move(p))
    {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
    }
    ~TempDirGuard()
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

} // namespace

int main()
{
    // Create a synthetic raw_replay fixture representing continuous acquisition across:
    // T0 (4 frames) + transition(1) + T1 (4 frames) + 3 drift bursts (8 frames each) separated by transition frames
    const int32_t rows = 16, cols = 16;
    const int64_t nPix = int64_t(rows) * cols;
    const TempDirGuard fixture(std::filesystem::temp_directory_path() / "ir_sdk_drift_nodisk_fixture");

    std::vector<uint16_t> allFrames;
    auto pushFrame = [&](auto pixelFn) {
        for (int32_t r = 0; r < rows; ++r)
            for (int32_t c = 0; c < cols; ++c)
                allFrames.push_back(pixelFn(r, c));
    };

    // Pixel fixed pattern gain/offset: g(r,c) = 0.85..1.15 across cols, offset = 50*(r/15)
    auto gainAt = [&](int32_t c) { return 0.85 + 0.30 * (static_cast<double>(c) / (cols - 1)); };

    // T0 calibration: 4 frames (base 30000 + offset + temporal +/- 4)
    for (int f = 0; f < 4; ++f)
    {
        const double tNoise = ((f % 2) == 0) ? -4.0 : 4.0;
        pushFrame([&](int32_t r, int32_t c) -> uint16_t {
            const double val = 30000.0 + gainAt(c) * 100.0 + 40.0 * r + tNoise;
            return static_cast<uint16_t>(std::llround(val));
        });
    }
    // 1 transition frame (garbage high value that IR_FlushFrames(1) must discard)
    pushFrame([&](int32_t, int32_t) -> uint16_t { return 65000; });

    // T1 calibration: 4 frames (higher radiance signal scaled by per-pixel gain)
    for (int f = 0; f < 4; ++f)
    {
        const double tNoise = ((f % 2) == 0) ? -4.0 : 4.0;
        pushFrame([&](int32_t r, int32_t c) -> uint16_t {
            const double val = 30000.0 + gainAt(c) * 1100.0 + 40.0 * r + tNoise;
            return static_cast<uint16_t>(std::llround(val));
        });
    }

    // 3 drift bursts (8 frames each), separated by 1 garbage transition frame before each burst
    for (int b = 0; b < 3; ++b)
    {
        pushFrame([&](int32_t, int32_t) -> uint16_t { return 64000; });
        const double signal = 600.0 + 100.0 * b; // slight thermal drift across bursts (exact integer gain*signal)
        for (int f = 0; f < 8; ++f)
        {
            const double tNoise = ((f % 2) == 0) ? -4.0 : 4.0;
            pushFrame([&](int32_t r, int32_t c) -> uint16_t {
                const double val = 30000.0 + gainAt(c) * signal + 40.0 * r + tNoise;
                return static_cast<uint16_t>(std::llround(val));
            });
        }
    }

    const int32_t totalFrames = static_cast<int32_t>(allFrames.size() / static_cast<size_t>(nPix));
    {
        std::ofstream raw(fixture.dir / "frames.raw", std::ios::binary);
        raw.write(reinterpret_cast<const char *>(allFrames.data()),
                  static_cast<std::streamsize>(allFrames.size() * sizeof(uint16_t)));
        std::ofstream csv(fixture.dir / "frames.csv", std::ios::binary);
        csv << "frame_index,width,height,stride_bytes,pixel_format,bit_depth,endianness\n";
        for (int32_t idx = 0; idx < totalFrames; ++idx)
            csv << idx << ",16,16,32,RAW16_LE,16,little\n";
    }

    char err[512]{};
    IRGeometry geom{};
    const auto u8path = fixture.dir.u8string();
    const std::string pathStr(u8path.begin(), u8path.end());
    CHECK(IR_OpenFrames(IR_RAW_REPLAY, pathStr.c_str(), 16, &geom, err, sizeof err) == IR_OK);
    CHECK(geom.width == cols && geom.height == rows);
    CHECK(IR_StartFrames(err, sizeof err) == IR_OK);

    // Preallocated buffers reused across all steps (zero RAW written to disk)
    std::vector<uint16_t> rawCalib(static_cast<size_t>(4 * nPix));
    std::vector<IRFrameStamp> stamps(8);
    std::vector<double> v1Mean(static_cast<size_t>(nPix)), vn1(static_cast<size_t>(nPix)),
        v2Mean(static_cast<size_t>(nPix)), vn2(static_cast<size_t>(nPix));
    double statSum[2]{};
    int32_t completed = 0, discarded = 0;

    // 1. Read T0
    CHECK(IR_ReadFrames(4, 1000, rawCalib.data(), int64_t(rawCalib.size()),
                        stamps.data(), 8, &completed, err, sizeof err) == IR_OK);
    CHECK(completed == 4);
    CHECK(IR_FrameStatisticsU16(rawCalib.data(), int64_t(rawCalib.size()), 4, rows, cols, 0,
                                v2Mean.data(), vn2.data(), nPix, statSum, 2, err, sizeof err) == IR_OK);

    // 2. Flush 1 transition frame without stopping stream, read T1
    CHECK(IR_FlushFrames(1, 1000, &discarded, err, sizeof err) == IR_OK);
    CHECK(discarded == 1);
    CHECK(IR_ReadFrames(4, 1000, rawCalib.data(), int64_t(rawCalib.size()),
                        stamps.data(), 8, &completed, err, sizeof err) == IR_OK);
    CHECK(completed == 4);
    CHECK(IR_FrameStatisticsU16(rawCalib.data(), int64_t(rawCalib.size()), 4, rows, cols, 0,
                                v1Mean.data(), vn1.data(), nPix, statSum, 2, err, sizeof err) == IR_OK);

    // 3. t=0 Calibration (GJB + TwoPoint NUC)
    IRCalcParameters calcParams{};
    calcParams.t0_c = 20.0;
    calcParams.t1_c = 30.0;
    calcParams.integration_time_us = 1000;
    calcParams.pixel_pitch_um = 15.0;
    calcParams.emissivity = 1.0;
    calcParams.system_gain = 1.0;
    calcParams.adc_swing_v = 2.0;
    calcParams.digital_full = 65535;
    calcParams.f_number = 1.0;
    calcParams.vmax_per_count = 2.0;
    const IRGjbCriteria gjbCrit{1.5, 1.5, 0.4, 0.4, 2, 0};

    std::vector<uint8_t> validMask(size_t(nPix), 1);
    IRGjbBadPixelMasks gjbMasks{};
    gjbMasks.valid_mask = validMask.data();
    gjbMasks.pixel_capacity = nPix;
    IRGjbSummary gjbInit{};
    CHECK(IR_GjbEvaluateMaps(v1Mean.data(), v2Mean.data(), vn1.data(), v2Mean.data(),
                             rows, cols, nPix, &calcParams, &gjbCrit, &gjbInit,
                             &gjbMasks, err, sizeof err) == IR_OK);

    std::vector<double> kMap(static_cast<size_t>(nPix)), bMap(static_cast<size_t>(nPix));
    double nucMeta[4]{};
    CHECK(IR_BuildTwoPointNucF64(v1Mean.data(), v2Mean.data(), validMask.data(), nullptr,
                                  rows, cols, nPix, kMap.data(), bMap.data(),
                                  nucMeta, 4, err, sizeof err) == IR_OK);

    // Prepare Noise3D blind mask (invert valid_mask -> 0=valid, 1=blind)
    std::vector<uint8_t> blindMask(size_t(nPix), 0);
    int32_t blindCount = 0;
    CHECK(IR_MaskCombine(validMask.data(), nullptr, rows, cols, nPix, IR_MASK_NOT,
                         blindMask.data(), &blindCount, err, sizeof err) == IR_OK);

    const IRGjbDriftContext driftCtx{
        gjbInit.volts_per_count, 10.0,
        gjbInit.before_avg_pixel_response_count, gjbInit.after_avg_responsivity, 2.0,
        1.0, 8, IR_NOISE_SUBTRACT_VARIANCE_QUADRATURE, 15, 0
    };

    // 4. Multi-burst drift loop (zero disk writes, Pre-NUC vs Post-NUC GJB & Noise3D)
    std::vector<uint16_t> burstRaw(static_cast<size_t>(8 * nPix));
    std::vector<double> orgMean(static_cast<size_t>(nPix)), orgVn(static_cast<size_t>(nPix));
    std::vector<double> nucMean(static_cast<size_t>(nPix)), nucVn(static_cast<size_t>(nPix));
    double prevOrgMean = 0.0;

    for (int b = 0; b < 3; ++b)
    {
        CHECK(IR_FlushFrames(1, 1000, &discarded, err, sizeof err) == IR_OK);
        CHECK(discarded == 1);
        CHECK(IR_ReadFrames(8, 1000, burstRaw.data(), int64_t(burstRaw.size()),
                            stamps.data(), 8, &completed, err, sizeof err) == IR_OK);
        CHECK(completed == 8);

        // Pre-NUC stats + GJB partial metrics
        CHECK(IR_FrameStatisticsU16(burstRaw.data(), int64_t(burstRaw.size()), 8, rows, cols, 0,
                                    orgMean.data(), orgVn.data(), nPix, statSum, 2, err, sizeof err) == IR_OK);
        IRGjbBurstMetrics orgGjb{};
        CHECK(IR_GjbEvaluateBurstMetrics(orgMean.data(), orgVn.data(), validMask.data(),
                                         rows, cols, nPix, &driftCtx, &orgGjb, err, sizeof err) == IR_OK);

        // Post-NUC lossless stats + GJB partial metrics (RFPN / RNU)
        CHECK(IR_FrameStatisticsNucU16(burstRaw.data(), int64_t(burstRaw.size()), 8, rows, cols, 0,
                                      kMap.data(), bMap.data(), nullptr, validMask.data(),
                                      nucMean.data(), nucVn.data(), nPix, statSum, 2, err, sizeof err) == IR_OK);
        IRGjbBurstMetrics nucGjb{};
        CHECK(IR_GjbEvaluateBurstMetrics(nucMean.data(), nucVn.data(), validMask.data(),
                                         rows, cols, nPix, &driftCtx, &nucGjb, err, sizeof err) == IR_OK);

        // Post-NUC spatial nonuniformity (RFPN) must be practically 0 << Pre-NUC spatial std
        CHECK(orgGjb.spatial_std_dn > 50.0);
        CHECK(nucGjb.spatial_std_dn < 1e-10);
        CHECK(nucGjb.lrnu_std_dn < 1e-10);

        if (b > 0)
            CHECK(orgGjb.mean_dn > prevOrgMean); // verifies monotonic drift progression
        prevOrgMean = orgGjb.mean_dn;

        // Also run Noise3D before/after NUC on last burst
        if (b == 2)
        {
            const int32_t histInterval = 4;
            double scalarsOrg[IR_NOISE3D_SCALAR_COUNT]{}, scalarsNuc[IR_NOISE3D_SCALAR_COUNT]{};
            double hist[IR_NOISE3D_HIST_GROUP_COUNT * 4]{}, histMeta[IR_NOISE3D_HIST_GROUP_COUNT * 3]{};
            std::vector<double> mapIJ(static_cast<size_t>(nPix)),
                mapTI(static_cast<size_t>(8 * rows)), mapTJ(static_cast<size_t>(8 * cols));

            IRNoise3DComputeOutputs n3dOut{};
            n3dOut.hist = hist;
            n3dOut.hist_meta = histMeta;
            n3dOut.map_ij = mapIJ.data();
            n3dOut.map_ti = mapTI.data();
            n3dOut.map_tj = mapTJ.data();
            n3dOut.hist_capacity = IR_NOISE3D_HIST_GROUP_COUNT * histInterval;
            n3dOut.map_ij_capacity = nPix;
            n3dOut.map_ti_capacity = 8 * rows;
            n3dOut.map_tj_capacity = 8 * cols;
            n3dOut.scalars_capacity = IR_NOISE3D_SCALAR_COUNT;
            n3dOut.hist_meta_capacity = IR_NOISE3D_HIST_GROUP_COUNT * 3;

            const IRNoise3DComputeParams pOrg{IR_NOISE3D_MODE_ORG, 1, histInterval, 0, 0.0, -999.0};
            n3dOut.scalars = scalarsOrg;
            CHECK(IR_Noise3DComputeFramesDn(burstRaw.data(), int64_t(burstRaw.size()), 8, rows, cols,
                                            blindMask.data(), nPix, nullptr, nullptr, 0,
                                            &pOrg, &n3dOut, err, sizeof err) == IR_OK);

            const IRNoise3DComputeParams pNuc{IR_NOISE3D_MODE_NUC, 1, histInterval, 0, nucMeta[0], -999.0};
            n3dOut.scalars = scalarsNuc;
            CHECK(IR_Noise3DComputeFramesDn(burstRaw.data(), int64_t(burstRaw.size()), 8, rows, cols,
                                            blindMask.data(), nPix, kMap.data(), v1Mean.data(), nPix,
                                            &pNuc, &n3dOut, err, sizeof err) == IR_OK);

            // Dynamic noise threshold re-tuning on existing orgVn map (strict vs loose)
            std::vector<uint8_t> strictPass(static_cast<size_t>(nPix));
            int32_t passCnt = 0, failCnt = 0;
            CHECK(IR_ThresholdMaskF64(orgVn.data(), validMask.data(), rows, cols, nPix,
                                      IR_THRESH_UPPER_ONLY, 0.0, 5.0, strictPass.data(),
                                      &passCnt, &failCnt, err, sizeof err) == IR_OK);
            CHECK(passCnt == nPix);

            CHECK(scalarsNuc[IR_NOISE3D_SCALAR_DELTA_I] < scalarsOrg[IR_NOISE3D_SCALAR_DELTA_I]);
            CHECK(scalarsNuc[IR_NOISE3D_SCALAR_DELTA_J] < scalarsOrg[IR_NOISE3D_SCALAR_DELTA_J]);
        }
    }

    CHECK(IR_StopFrames(err, sizeof err) == IR_OK);
    CHECK(IR_CloseFrames(err, sizeof err) == IR_OK);

    // Ensure only the input fixture files exist (no result.json or output RAW written)
    CHECK(!std::filesystem::exists(fixture.dir / "result.json"));

    if (gFailures == 0)
        std::printf("drift_nodisk_test: ALL PASSED\n");
    return gFailures == 0 ? 0 : 1;
}
