#include "../frame_source.h"
#include "RawConversionCore.h"
#include "api_common.h"
#include <QFile>
#include <fstream>
namespace optmeas::acquisition {
using namespace optmeas::api;
class ReplaySource final : public FrameSource {
    raw::File file_;
    QFile pixels_;
    std::ifstream metadata_;
    QByteArray scratch_;
    uint64_t next_{};
    void readPixels(uint16_t *out)
    {
        const auto &g = file_.geometry;
        const auto size = qint64(g.frameBytes());
        if (!g.bigEndian && g.stride == g.width * 2) {
            require(pixels_.read(reinterpret_cast<char *>(out), size) == size, OPTMEAS_IO_ERROR, "Incomplete RAW frame");
        } else {
            scratch_.resize(int(size));
            require(pixels_.read(scratch_.data(), size) == size, OPTMEAS_IO_ERROR, "Incomplete RAW frame");
            const auto frame = raw::decode(reinterpret_cast<const uint8_t *>(scratch_.constData()), size_t(size), g);
            std::copy(frame.begin(), frame.end(), out);
        }
    }
public:
    explicit ReplaySource(const char *directory) :
        file_(raw::inspect(std::filesystem::path(reinterpret_cast<const char8_t *>(directory)))) {}
    OptMeasGeometry geometry() const override {
        const auto &g = file_.geometry;
        return {g.width, g.height, g.bitDepth};
    }
    void start() override {
        raw::checkUnchanged(file_);
        std::ifstream index;
        if (file_.perFrameMetadata) {
            index.open(file_.table->path, std::ios::binary);
            require(index.is_open(), OPTMEAS_IO_ERROR, "Cannot open RAW index");
        }
        const auto path = file_.rawPath.u8string();
        pixels_.setFileName(QString::fromUtf8(reinterpret_cast<const char *>(path.data()), int(path.size())));
        require(pixels_.open(QIODevice::ReadOnly), OPTMEAS_IO_ERROR, "Cannot open RAW frame file");
        metadata_ = std::move(index);
        next_ = 0;
    }
    void stop() override {
        pixels_.close();
        if (metadata_.is_open()) metadata_.close();
    }
    void read(uint16_t *out, OptMeasFrameStamp &s, int) override {
        require(next_ < file_.frames, OPTMEAS_END_OF_STREAM, "End of RAW capture");
        try {
            const auto m = raw::metadataAt(file_, next_, metadata_);
            readPixels(out);
            if (m.applicationSequence) { s.sequence = *m.applicationSequence; s.fields |= OPTMEAS_HAS_SEQUENCE; }
            if (m.driverSequence) { s.driver_sequence = *m.driverSequence; s.fields |= OPTMEAS_HAS_DRIVER_SEQUENCE; }
            if (m.lost) { s.lost_frames = *m.lost; s.fields |= OPTMEAS_HAS_LOSS; }
            if (m.inverted.value_or(false)) s.fields |= OPTMEAS_PIXELS_INVERTED;
            const auto callback = m.fields.find("callback_sequence");
            if (callback != m.fields.end() && callback->second == "1") s.fields |= OPTMEAS_SEQUENCE_IS_CALLBACK;
            if (m.sourceNs) {
                s.timestamp_ns = *m.sourceNs;
                if (m.timestampOrigin == "received_monotonic_ns") s.time_origin = OPTMEAS_TIME_HOST;
                else if (m.timestampOrigin == "monotonic_ms") s.time_origin = OPTMEAS_TIME_RECORDED_MONOTONIC;
                else if (m.timestampOrigin == "relative_time_ms") s.time_origin = OPTMEAS_TIME_RECORDED_RELATIVE;
                else s.time_origin = OPTMEAS_TIME_SOURCE;
            }
            ++next_;
        } catch (const raw::AppError &e) { throw Error(OPTMEAS_IO_ERROR, e.what()); }
    }
};
std::unique_ptr<FrameSource> openReplay(const char *directory)
{
    try { return std::make_unique<ReplaySource>(directory); }
    catch (const raw::AppError &e) { throw Error(OPTMEAS_IO_ERROR, e.what()); }
}
}
