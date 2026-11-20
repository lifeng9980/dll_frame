#include "save_raw.h"
#include "IR_analysis.h"
#include "api_common.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <bit>
namespace ir::procedures {
using namespace ir::api;
void checkDestination(const QString &path)
{
    QFileInfo target(path);
    require(!path.isEmpty() && !target.exists() && !target.isSymLink() && target.dir().exists(),
        IR_IO_ERROR, "Specify a new capture directory under an existing parent");
}
static void writeFile(const QString &name, const char *bytes, qint64 length)
{
    QFile file(name);
    require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), IR_IO_ERROR, "Cannot create capture file");
    require(file.write(bytes, length) == length && file.flush(), IR_IO_ERROR, "Cannot write capture file");
}
void saveNoiseRun(const QString &path, const IRNoiseRecipe &r, int backend, IRGeometry g,
    const std::vector<uint16_t> &frames, const std::vector<IRFrameStamp> &stamps, const IRNoiseResult &result)
{
    static_assert(std::endian::native == std::endian::little);
    const QFileInfo dest(path);
    QTemporaryDir stage(dest.dir().absoluteFilePath(".IR-XXXXXX"));
    require(stage.isValid(), IR_IO_ERROR, "Cannot create capture staging directory");
    writeFile(stage.filePath("frames.raw"), reinterpret_cast<const char *>(frames.data()), qint64(frames.size()) * 2);
    QByteArray csv = "frame_index,application_sequence,driver_frame_number,lost_frames,width,height,stride_bytes,pixel_format,bit_depth,endianness,"
        "source_timestamp_ns,received_monotonic_ns,monotonic_ms,relative_time_ms,callback_sequence,pixels_inverted\n";
    for (size_t i = 0; i < stamps.size(); ++i) {
        const auto &s = stamps[i];
        const auto ns = QByteArray::number(qlonglong(s.timestamp_ns));
        const auto ms = QByteArray::number(s.timestamp_ns / 1e6, 'f', 6);
        const QList<QByteArray> row{QByteArray::number(qulonglong(i)),
            s.fields & IR_HAS_SEQUENCE ? QByteArray::number(qulonglong(s.sequence)) : QByteArray{},
            s.fields & IR_HAS_DRIVER_SEQUENCE ? QByteArray::number(qulonglong(s.driver_sequence)) : QByteArray{},
            s.fields & IR_HAS_LOSS ? QByteArray::number(s.lost_frames) : QByteArray{},
            QByteArray::number(g.width), QByteArray::number(g.height), QByteArray::number(g.width * 2),
            "RAW16_LE", QByteArray::number(g.valid_bits), "little",
            s.time_origin == IR_TIME_SOURCE ? ns : QByteArray{},
            s.time_origin == IR_TIME_HOST ? ns : QByteArray{},
            s.time_origin == IR_TIME_RECORDED_MONOTONIC ? ms : QByteArray{},
            s.time_origin == IR_TIME_RECORDED_RELATIVE ? ms : QByteArray{},
            s.fields & IR_SEQUENCE_IS_CALLBACK ? "1" : "0", s.fields & IR_PIXELS_INVERTED ? "1" : "0"};
        csv += row.join(',') + '\n';
    }
    writeFile(stage.filePath("frames.csv"), csv.constData(), csv.size());
    QJsonObject record{{"recipe_id",QString::fromUtf8(r.recipe_id)},{"analysis_version",IR_AnalysisVersion()},
        {"backend",backend == IR_RAW_REPLAY ? "raw_replay" : "directshow"},
        {"width",g.width},{"height",g.height},{"valid_bits",g.valid_bits},{"requested_frames",r.frames},
        {"completed_frames",result.completed},{"frame_timeout_ms",r.frame_timeout_ms},
        {"mean_dn",result.mean_dn},{"temporal_noise_dn",result.temporal_noise_dn},
        {"estimator","mean_pixel_sample_std"},{"unit","DN"}};
    if (r.detector) record["detector"] = QJsonObject{{"integration_us",r.integration_us},{"frame_hz",r.frame_hz},{"empty_us",r.empty_us}};
    if (r.blackbody) record["blackbody"] = QJsonObject{{"temperature_c",r.temperature_c},{"tolerance_c",r.tolerance_c},
        {"stable_window_ms",r.stable_window_ms},{"stable_timeout_ms",r.stable_timeout_ms},{"require_device_stable",bool(r.require_device_stable)}};
    const auto json = QJsonDocument(record).toJson();
    writeFile(stage.filePath("result.json"), json.constData(), json.size());
    require(QDir().rename(stage.path(), dest.absoluteFilePath()), IR_IO_ERROR,"Cannot publish capture directory");
    stage.setAutoRemove(false);
}
}
