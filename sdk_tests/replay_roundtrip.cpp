#include "optmeas_station.h"
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
        check(QDir().mkpath(original), "fixture input directory");
        // Distinct row positions, high/low byte patterns, 12 effective bits and padding.
        const std::array<uint16_t,12> input{0x101,0x203,0xffff,0x401,0x802,0xffff,
                                           0x103,0x207,0xffff,0x407,0x80a,0xffff};
        write(original + "/frames.raw", QByteArray(reinterpret_cast<const char *>(input.data()),sizeof input));
        write(original + "/frames.csv",
            "frame_index,application_sequence,driver_frame_number,monotonic_ms,lost_frames,width,height,stride_bytes,pixel_format,bit_depth,endianness\n"
            "0,10,99,1,0,2,2,6,RAW16_LE,12,little\n"
            "1,11,100,5,0,2,2,6,RAW16_LE,12,little\n");
        char err[1024]{};
        OptMeasGeometry g{};
        const auto path = original.toUtf8();
        auto open = [&](const QByteArray &name) {
            check(OptMeas_OpenFrames(OPTMEAS_RAW_REPLAY,name.constData(),0,&g,err,sizeof err)==0,err);
            check(g.width==2 && g.height==2 && g.valid_bits==12,"geometry");
        };
        auto read = [&] {
            check(OptMeas_StartFrames(err,sizeof err)==0,err);
            uint16_t data[12]{};
            OptMeasFrameStamp stamps[3]{};
            int32_t readCount{};
            check(OptMeas_ReadFrames(3,0,data,12,stamps,3,&readCount,err,sizeof err)==OPTMEAS_END_OF_STREAM,"EOF");
            check(readCount==2,"partial count");
            const uint16_t expected[]{0x101,0x203,0x401,0x802,0x103,0x207,0x407,0x80a};
            check(std::equal(std::begin(expected),std::end(expected),data),"pixel order or padding");
            check(stamps[0].sequence==10 && stamps[1].driver_sequence==100 &&
                  stamps[1].time_origin==OPTMEAS_TIME_RECORDED_MONOTONIC &&
                  stamps[1].timestamp_ns==5000000, "recorded metadata");
            check(OptMeas_StopFrames(err,sizeof err)==0,err);
        };
        open(path); read();
        OptMeasNoiseRecipe recipe{};
        recipe.recipe_id = "roundtrip";
        recipe.frames = 2;
        OptMeasNoiseResult result{};
        const auto out = saved.toUtf8();
        check(OptMeas_RunNoise(&recipe,out.constData(),&result,err,sizeof err)==0,err);
        check(result.completed==2 && std::abs(result.temporal_noise_dn-5/std::sqrt(2.0))<1e-12,"sample noise");
        check(OptMeas_RunNoise(&recipe,out.constData(),&result,err,sizeof err)==OPTMEAS_IO_ERROR,"reject existing output");
        check(OptMeas_CloseFrames(err,sizeof err)==0,err);
        open(out); read();
        check(OptMeas_StartFrames(err,sizeof err)==0,err);
        OptMeas_RequestStop();
        uint16_t pixels[4]{};
        OptMeasFrameStamp stamp{};
        int32_t completed{};
        check(OptMeas_ReadFrames(1,0,pixels,4,&stamp,1,&completed,err,sizeof err)==OPTMEAS_CANCELLED && completed==0,
            "cancel read");
        check(OptMeas_CloseFrames(err,sizeof err)==0,err);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
