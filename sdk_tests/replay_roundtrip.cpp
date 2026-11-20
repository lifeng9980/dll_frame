#include "IR_station.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value, const char *details)
{
    if (!value) throw std::runtime_error(details);
}
void write(const QString &path, const QByteArray &data)
{
    QFile file(path);
    check(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "fixture open");
    check(file.write(data) == data.size(), "fixture write");
}
}
int main()
{
    try {
        QTemporaryDir fixture;
        check(fixture.isValid(), "fixture directory");
        const auto original = fixture.filePath(QStringLiteral("原始帧"));
        const auto saved = fixture.filePath(QStringLiteral("保存帧"));
        const auto unknownBits = fixture.filePath(QStringLiteral("有效位未知"));
        const auto unknownBitsSaved = fixture.filePath(QStringLiteral("有效位未知_往返"));
        check(QDir().mkpath(original), "fixture input directory");
        check(QDir().mkpath(unknownBits), "unknown-bit fixture directory");
        // Distinct row positions, high/low byte patterns, 12 effective bits and padding.
        const std::array<uint16_t,12> input{0x101,0x203,0xffff,0x401,0x802,0xffff,
                                           0x103,0x207,0xffff,0x407,0x80a,0xffff};
        const std::array<uint16_t,8> unknownInput{0x101,0x203,0x401,0x802,
                                                  0x103,0x207,0x407,0x80a};
        write(original + "/frames.raw", QByteArray(reinterpret_cast<const char *>(input.data()),sizeof input));
        write(original + "/frames.csv",
            "frame_index,application_sequence,driver_frame_number,monotonic_ms,lost_frames,width,height,stride_bytes,pixel_format,bit_depth,endianness\n"
            "0,10,99,1,0,2,2,6,RAW16_LE,12,little\n"
            "1,11,100,5,0,2,2,6,RAW16_LE,12,little\n");
        write(unknownBits + "/frames.raw",
              QByteArray(reinterpret_cast<const char *>(unknownInput.data()), sizeof unknownInput));
        write(unknownBits + "/frames.csv",
            "frame_index,width,height,stride_bytes,pixel_format,bit_depth,endianness\n"
            "0,2,2,4,RAW16_LE,0,little\n1,2,2,4,RAW16_LE,0,little\n");
        char err[1024]{};
        IRGeometry g{};
        const auto missing = fixture.filePath(QStringLiteral("不存在的采集目录")).toUtf8();
        check(IR_OpenFrames(IR_RAW_REPLAY,missing.constData(),0,&g,err,sizeof err)==IR_IO_ERROR, "missing UTF-8 path");
        check(QByteArray(err).contains(missing), "missing path diagnostic preserves UTF-8");
        const auto path = original.toUtf8();
        auto open = [&](const QByteArray &name) {
            check(IR_OpenFrames(IR_RAW_REPLAY,name.constData(),0,&g,err,sizeof err)==0,err);
            check(g.width==2 && g.height==2 && g.valid_bits==12,"geometry");
        };
        auto read = [&] {
            check(IR_StartFrames(err,sizeof err)==0,err);
            uint16_t data[12]{};
            IRFrameStamp stamps[3]{};
            int32_t readCount{};
            check(IR_ReadFrames(3,0,data,12,stamps,3,&readCount,err,sizeof err)==IR_END_OF_STREAM,"EOF");
            check(readCount==2,"partial count");
            const uint16_t expected[]{0x101,0x203,0x401,0x802,0x103,0x207,0x407,0x80a};
            check(std::equal(std::begin(expected),std::end(expected),data),"pixel order or padding");
            check(stamps[0].sequence==10 && stamps[1].driver_sequence==100 &&
                  stamps[1].time_origin==IR_TIME_RECORDED_MONOTONIC &&
                  stamps[1].timestamp_ns==5000000, "recorded metadata");
            check(IR_StopFrames(err,sizeof err)==0,err);
        };
        open(path); read();
        IRNoiseRecipe recipe{};
        recipe.recipe_id = "roundtrip";
        recipe.frames = 2;
        IRNoiseResult result{};
        const auto out = saved.toUtf8();
        check(IR_RunNoise(&recipe,out.constData(),&result,err,sizeof err)==0,err);
        check(result.completed==2 && std::abs(result.temporal_noise_dn-5/std::sqrt(2.0))<1e-12,"sample noise");
        check(IR_RunNoise(&recipe,out.constData(),&result,err,sizeof err)==IR_IO_ERROR,"reject existing output");
        check(IR_CloseFrames(err,sizeof err)==0,err);
        open(out); read();
        check(IR_CloseFrames(err,sizeof err)==0,err);
        const auto unknownPath = unknownBits.toUtf8();
        check(IR_OpenFrames(IR_RAW_REPLAY, unknownPath.constData(), 0, &g, err, sizeof err) == 0, err);
        check(g.valid_bits == 0, "unknown valid bits");
        IRNoiseRecipe unknownRecipe{};
        unknownRecipe.recipe_id = "unknown-valid-bits";
        unknownRecipe.frames = 2;
        IRNoiseResult unknownResult{};
        const auto unknownSavedPath = unknownBitsSaved.toUtf8();
        check(IR_RunNoise(&unknownRecipe, unknownSavedPath.constData(), &unknownResult,
                          err, sizeof err) == 0, err);
        check(IR_CloseFrames(err, sizeof err) == 0, err);
        check(IR_OpenFrames(IR_RAW_REPLAY, unknownSavedPath.constData(), 0, &g, err, sizeof err) == 0, err);
        check(g.valid_bits == 0, "saved unknown valid bits");
        check(IR_CloseFrames(err, sizeof err) == 0, err);

        open(out);
        check(IR_StartFrames(err,sizeof err)==0,err);
        IR_RequestStop();
        uint16_t pixels[4]{};
        IRFrameStamp stamp{};
        int32_t completed{};
        check(IR_ReadFrames(1,0,pixels,4,&stamp,1,&completed,err,sizeof err)==IR_CANCELLED && completed==0,
            "cancel read");
        check(IR_CloseFrames(err,sizeof err)==0,err);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
