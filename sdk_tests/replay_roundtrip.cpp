#include "IR_station.h"
#include "IR_recipe.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {
void check(bool value, const char *details)
{
    if (!value) throw std::runtime_error(details);
}

std::string utf8(const fs::path &path)
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

std::string escapeJson(const std::string &value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        if (ch == '\\' || ch == '"') out.push_back('\\');
        out.push_back(ch);
    }
    return out;
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() / pathFromUtf8("IR SDK 测试-") /
            fs::path(std::to_string(suffix));
        fs::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    const fs::path &path() const noexcept { return path_; }

private:
    fs::path path_;
};

void writeFile(const fs::path &path, const void *data, size_t length)
{
    std::ofstream file(path, std::ios::binary | std::ios::out);
    check(file.is_open(), "fixture open");
    file.write(static_cast<const char *>(data), static_cast<std::streamsize>(length));
    file.close();
    check(file.good(), "fixture write");
}

void writeText(const fs::path &path, const std::string &value)
{
    writeFile(path, value.data(), value.size());
}
}

int main()
{
    try {
        TemporaryDirectory fixture;
        const auto original = fixture.path() / pathFromUtf8("原始帧");
        const auto saved = fixture.path() / pathFromUtf8("保存帧");
        const auto unknownBits = fixture.path() / pathFromUtf8("有效位未知");
        const auto unknownBitsSaved = fixture.path() / pathFromUtf8("有效位未知_往返");
        const auto widePixels = fixture.path() / pathFromUtf8("RAW32高位宽");
        fs::create_directories(original);
        fs::create_directories(unknownBits);
        fs::create_directories(widePixels);

        // Different rows and byte patterns expose row order, padding, endian, and valid-bit errors.
        const std::array<uint16_t, 12> input{0x101, 0x203, 0xffff, 0x401, 0x802, 0xffff,
                                             0x103, 0x207, 0xffff, 0x407, 0x80a, 0xffff};
        const std::array<uint16_t, 8> unknownInput{0x101, 0x203, 0x401, 0x802,
                                                   0x103, 0x207, 0x407, 0x80a};
        writeFile(original / "frames.raw", input.data(), sizeof input);
        writeText(original / "frames.csv",
            "frame_index,application_sequence,driver_frame_number,monotonic_ms,lost_frames,width,height,stride_bytes,pixel_format,bit_depth,endianness\n"
            "0,10,99,1,0,2,2,6,RAW16_LE,12,little\n"
            "1,11,100,5,0,2,2,6,RAW16_LE,12,little\n");
        writeFile(unknownBits / "frames.raw", unknownInput.data(), sizeof unknownInput);
        writeText(unknownBits / "frames.csv",
            "frame_index,width,height,stride_bytes,pixel_format,bit_depth,endianness\n"
            "0,2,2,4,RAW16_LE,0,little\n1,2,2,4,RAW16_LE,0,little\n");

        const std::array<uint32_t, 4> wideInput{70010, 0xABCDE, 0x10001, 0xFFFFF};
        std::array<uint8_t, sizeof(wideInput)> wideBytes{};
        for (size_t i = 0; i < wideInput.size(); ++i) {
            for (size_t b = 0; b < sizeof(uint32_t); ++b)
                wideBytes[i * sizeof(uint32_t) + b] = uint8_t(wideInput[i] >> (b * 8));
        }
        writeFile(widePixels / "frames.raw", wideBytes.data(), wideBytes.size());
        writeText(widePixels / "frames.csv",
            "frame_index,width,height,stride_bytes,pixel_format,bit_depth,endianness\n"
            "0,2,2,8,RAW32_LE,20,little\n");

        char error[1024]{};
        IRFrameSourceInfo sourceInfo{};
        const auto missing = utf8(fixture.path() / pathFromUtf8("不存在的采集目录"));
        check(IR_OpenFrames(IR_RAW_REPLAY, missing.c_str(), 0, &sourceInfo, error, sizeof error) == IR_IO_ERROR,
              "missing UTF-8 path");
        check(std::string(error).find(missing) != std::string::npos,
              "missing path diagnostic preserves UTF-8");

        const auto inputPath = utf8(original);
        auto open = [&](const std::string &name) {
            check(IR_OpenFrames(IR_RAW_REPLAY, name.c_str(), 0, &sourceInfo, error, sizeof error) == IR_OK, error);
            check(sourceInfo.data_type == IR_TYPE_U16 && sourceInfo.geometry.width == 2 &&
                  sourceInfo.geometry.height == 2 && sourceInfo.geometry.valid_bits == 12, "geometry");
        };
        auto read = [&] {
            check(IR_StartFrames(error, sizeof error) == IR_OK, error);
            uint16_t pixels[12]{};
            IRFrameStamp stamps[3]{};
            IRFrameBatchView batch{{pixels, sizeof pixels, IR_TYPE_U16, 2, 2, 3}, 0, stamps, 3};
            check(IR_ReadFrames(0, &batch, error, sizeof error) == IR_END_OF_STREAM, "EOF");
            check(batch.pixels.frames == 2, "partial count");
            const uint16_t expected[]{0x101, 0x203, 0x401, 0x802, 0x103, 0x207, 0x407, 0x80a};
            check(std::equal(std::begin(expected), std::end(expected), pixels), "pixel order or padding");
            check(stamps[0].sequence == 10 && stamps[1].driver_sequence == 100 &&
                  stamps[1].time_origin == IR_TIME_RECORDED_MONOTONIC &&
                  stamps[1].timestamp_ns == 5000000, "recorded metadata");
            check(IR_StopFrames(error, sizeof error) == IR_OK, error);
        };

        open(inputPath);
        read();
        check(IR_CloseFrames(error, sizeof error) == IR_OK, error);

        const auto widePath = utf8(widePixels);
        const auto readWide = [&] {
            check(IR_OpenFrames(IR_RAW_REPLAY, widePath.c_str(), 0, &sourceInfo, error, sizeof error) == IR_OK, error);
            check(sourceInfo.data_type == IR_TYPE_U32 && sourceInfo.geometry.valid_bits == 20 &&
                  sourceInfo.geometry.width == 2 && sourceInfo.geometry.height == 2, "RAW32 source info");
            check(IR_StartFrames(error, sizeof error) == IR_OK, error);
            std::array<uint32_t, 8> wideOutput{};
            IRFrameStamp wideStamps[2]{};
            IRFrameBatchView wideBatch{{wideOutput.data(), sizeof wideOutput, IR_TYPE_U32, 2, 2, 2},
                                       0, wideStamps, 2};
            check(IR_ReadFrames(0, &wideBatch, error, sizeof error) == IR_END_OF_STREAM,
                  "one RAW32 frame at EOF");
            check(wideBatch.pixels.frames == 1 && wideBatch.valid_bits == 20 &&
                  std::equal(wideInput.begin(), wideInput.end(), wideOutput.begin()),
                  "RAW32 high values survive replay");
            std::array<uint32_t, 4> previewPixels{};
            IRFrameStamp previewStamp{};
            IRFrameBatchView previewBatch{{previewPixels.data(), sizeof previewPixels, IR_TYPE_U32, 2, 2, 1},
                                          0, &previewStamp, 1};
            check(IR_TryReadLatestFrame(&previewBatch, error, sizeof error) == IR_OK, error);
            check(previewBatch.pixels.frames == 1 && previewPixels == wideInput,
                  "RAW32 preview preserves type and values");
            check(IR_StopFrames(error, sizeof error) == IR_OK, error);
            check(IR_CloseFrames(error, sizeof error) == IR_OK, error);
        };
        readWide();
        writeText(widePixels / "mean.csv", "70010,703710\n65537,1048575\n");
        writeText(widePixels / "frames.csv", "frame_index,pixel_format,bit_depth\n0,RAW32_LE,20\n");
        readWide();

        std::array<uint8_t, 24> paddedWide{};
        paddedWide.fill(0xff);
        std::copy_n(wideBytes.begin(), 8, paddedWide.begin());
        std::copy_n(wideBytes.begin() + 8, 8, paddedWide.begin() + 12);
        writeFile(widePixels / "frames.raw", paddedWide.data(), paddedWide.size());
        writeText(widePixels / "frames.csv",
            "pixel_format,bit_depth,stride_bytes,endianness,storage_bits\nRAW32_LE,20,12,little,32\n");
        readWide();
        writeText(widePixels / "frames.csv", "pixel_format,storage_bits\nRAW32_LE,16\n");
        check(IR_OpenFrames(IR_RAW_REPLAY, widePath.c_str(), 0, &sourceInfo, error, sizeof error) == IR_IO_ERROR &&
              std::string(error).find("storage_bits conflicts") != std::string::npos,
              "reject storage width inconsistent with explicit RAW32");
        writeText(widePixels / "frames.csv", "pixel_format,endianness\nRAW32_LE,big\n");
        check(IR_OpenFrames(IR_RAW_REPLAY, widePath.c_str(), 0, &sourceInfo, error, sizeof error) == IR_IO_ERROR &&
              std::string(error).find("byte order conflict") != std::string::npos,
              "reject byte order inconsistent with explicit RAW32_LE");
        writeText(widePixels / "frames.csv", "frame_index,pixel_format\n0,RAW20\n");
        check(IR_OpenFrames(IR_RAW_REPLAY, widePath.c_str(), 0, &sourceInfo, error, sizeof error) == IR_IO_ERROR &&
              std::string(error).find("Unsupported pixel_format") != std::string::npos,
              "reject an unsupported encoding even when sizes come from mean.csv");

        const std::string outputPath = utf8(saved);
        const std::string stationJson = std::string("{\"schemav\":1,\"acquisition\":{\"backend\":\"raw_replay\",\"source\":\"") +
            escapeJson(inputPath) + "\",\"valid_bits\":0}}";
        const std::string recipeJson = R"({"schemav":1,"recipe_id":"roundtrip","steps":[)"
            R"({"id":"cap","type":"capture","frames":2,"frame_timeout_ms":0,"output":"b"},)"
            R"({"id":"noise","type":"analyze","method":"temporal_noise","name":"noise","input":"b"},)"
            R"({"id":"save","type":"save_raw","input":"b"}]})";
        IRRecipeResult rr{};
        check(IR_RunRecipeJson(stationJson.c_str(), recipeJson.c_str(), outputPath.c_str(),
                               nullptr, &rr, error, sizeof error) == IR_OK, error);
        check(rr.completed_frames == 2, "roundtrip completed_frames");
        check(IR_RunRecipeJson(stationJson.c_str(), recipeJson.c_str(), outputPath.c_str(),
                               nullptr, &rr, error, sizeof error) == IR_IO_ERROR,
              "reject existing output");
        open(outputPath);
        read();
        check(IR_CloseFrames(error, sizeof error) == IR_OK, error);

        const auto unknownPath = utf8(unknownBits);
        check(IR_OpenFrames(IR_RAW_REPLAY, unknownPath.c_str(), 0, &sourceInfo, error, sizeof error) == IR_OK, error);
        check(sourceInfo.geometry.valid_bits == 0, "unknown valid bits");
        check(IR_CloseFrames(error, sizeof error) == IR_OK, error);

        const std::string unknownStationJson = std::string("{\"schemav\":1,\"acquisition\":{\"backend\":\"raw_replay\",\"source\":\"") +
            escapeJson(unknownPath) + "\",\"valid_bits\":0}}";
        const auto unknownSavedPath = utf8(unknownBitsSaved);
        check(IR_RunRecipeJson(unknownStationJson.c_str(), recipeJson.c_str(), unknownSavedPath.c_str(),
                               nullptr, &rr, error, sizeof error) == IR_OK, error);
        check(IR_OpenFrames(IR_RAW_REPLAY, unknownSavedPath.c_str(), 0, &sourceInfo,
                            error, sizeof error) == IR_OK, error);
        check(sourceInfo.geometry.valid_bits == 0, "saved unknown valid bits");
        check(IR_CloseFrames(error, sizeof error) == IR_OK, error);

        open(outputPath);
        check(IR_StartFrames(error, sizeof error) == IR_OK, error);
        IR_RequestStop();
        uint16_t pixels[4]{};
        IRFrameStamp stamp{};
        IRFrameBatchView cancelledBatch{{pixels, sizeof pixels, IR_TYPE_U16, 2, 2, 1},
                                        12, &stamp, 1};
        check(IR_ReadFrames(0, &cancelledBatch, error, sizeof error) == IR_CANCELLED &&
              cancelledBatch.pixels.frames == 0,
              "cancel read");
        check(IR_CloseFrames(error, sizeof error) == IR_OK, error);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
