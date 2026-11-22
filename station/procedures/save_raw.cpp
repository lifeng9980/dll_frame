#include "save_raw.h"
#include "measurement.h"
#include "IR_analysis.h"
#include "api_common.h"
#include "station/version.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <locale>
#include <random>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
#else
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace ir::procedures {
using namespace ir::api;
namespace fs = std::filesystem;
namespace {
const char *backendName(int backend)
{
    switch (backend) {
    case IR_RAW_REPLAY: return "raw_replay";
    case IR_DIRECTSHOW: return "directshow";
    case IR_V4L2: return "v4l2";
    default: return "unknown";
    }
}

std::string integerText(uint64_t value)
{
    return std::to_string(value);
}

std::string integerText(int64_t value)
{
    return std::to_string(value);
}

std::string decimalMilliseconds(int64_t nanoseconds)
{
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::fixed << std::setprecision(6) << static_cast<double>(nanoseconds) / 1'000'000.0;
    return text.str();
}

void writeFile(const fs::path &path, const char *bytes, size_t length)
{
    std::ofstream file(path, std::ios::binary | std::ios::out);
    require(file.is_open(), IR_IO_ERROR, "Cannot create capture file");
    file.write(bytes, static_cast<std::streamsize>(length));
    file.flush();
    require(file.good(), IR_IO_ERROR, "Cannot write capture file");
    file.close();
    require(file.good(), IR_IO_ERROR, "Cannot close capture file");
}

class StagingDirectory {
public:
    explicit StagingDirectory(const fs::path &parent)
    {
        static std::atomic<uint64_t> sequence{};
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        std::random_device random;
        for (int attempt = 0; attempt < 16; ++attempt) {
            const auto suffix = std::to_string(timestamp) + "-" +
                std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + "-" +
                std::to_string(random());
            path_ = parent / (".IR-" + suffix);
            std::error_code error;
            if (fs::create_directory(path_, error)) return;
            if (error && error != std::errc::file_exists)
                throw fs::filesystem_error("Cannot create capture staging directory", path_, error);
        }
        throw std::system_error(std::make_error_code(std::errc::file_exists),
                                "Cannot choose a unique capture staging directory");
    }

    StagingDirectory(const StagingDirectory &) = delete;
    StagingDirectory &operator=(const StagingDirectory &) = delete;

    ~StagingDirectory()
    {
        if (!path_.empty()) {
            std::error_code error;
            fs::remove_all(path_, error);
        }
    }

    const fs::path &path() const noexcept { return path_; }

    void publish(const fs::path &destination)
    {
#ifdef _WIN32
        if (!MoveFileW(path_.c_str(), destination.c_str()))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "Cannot publish capture directory");
#else
        if (::syscall(SYS_renameat2, AT_FDCWD, path_.c_str(), AT_FDCWD,
                      destination.c_str(), RENAME_NOREPLACE) != 0) {
            const auto renameError = errno;
            if (renameError != EINVAL && renameError != ENOSYS && renameError != EOPNOTSUPP)
                throw std::system_error(renameError, std::system_category(),
                                        "Cannot publish capture directory");

            // WSL's /mnt filesystems may not implement renameat2(RENAME_NOREPLACE).
            // Reserve the new name, then rename the complete staging directory over it.
            std::error_code createError;
            if (!fs::create_directory(destination, createError)) {
                if (createError)
                    throw fs::filesystem_error("Cannot reserve capture destination", destination, createError);
                throw std::system_error(std::make_error_code(std::errc::file_exists),
                                        "Capture destination already exists");
            }
            try {
                fs::rename(path_, destination);
            } catch (...) {
                std::error_code removeError;
                fs::remove(destination, removeError);
                throw;
            }
        }
#endif
        path_.clear();
    }

private:
    fs::path path_;
};
}

void checkDestination(const fs::path &path)
{
    require(!path.empty(), IR_IO_ERROR, "Specify a new capture directory under an existing parent");
    std::error_code error;
    const auto target = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) error.clear();
    require(!error, IR_IO_ERROR, "Cannot inspect capture destination");
    require(target.type() == fs::file_type::not_found, IR_IO_ERROR,
            "Capture destination already exists");

    auto parent = path.parent_path();
    if (parent.empty()) parent = fs::current_path(error);
    require(!error && fs::is_directory(parent, error) && !error, IR_IO_ERROR,
            "Specify a new capture directory under an existing parent");
}

void writeFrameBatchFiles(const fs::path &directory, IRGeometry geometry,
    const std::vector<uint16_t> &frames, const std::vector<IRFrameStamp> &stamps)
{
    static_assert(std::endian::native == std::endian::little);
    fs::create_directories(directory);
    writeFile(directory / "frames.raw", reinterpret_cast<const char *>(frames.data()),
              frames.size() * sizeof(uint16_t));

    std::ostringstream csv;
    csv.imbue(std::locale::classic());
    csv << "frame_index,application_sequence,driver_frame_number,lost_frames,width,height,stride_bytes,pixel_format,bit_depth,endianness,"
           "source_timestamp_ns,received_monotonic_ns,monotonic_ms,relative_time_ms,callback_sequence,pixels_inverted\n";
    for (size_t index = 0; index < stamps.size(); ++index) {
        const auto &stamp = stamps[index];
        const auto sourceTime = stamp.time_origin == IR_TIME_SOURCE ? integerText(stamp.timestamp_ns) : "";
        const auto hostTime = stamp.time_origin == IR_TIME_HOST ? integerText(stamp.timestamp_ns) : "";
        const auto monotonic = stamp.time_origin == IR_TIME_RECORDED_MONOTONIC
            ? decimalMilliseconds(stamp.timestamp_ns) : "";
        const auto relative = stamp.time_origin == IR_TIME_RECORDED_RELATIVE
            ? decimalMilliseconds(stamp.timestamp_ns) : "";

        csv << index << ','
            << (stamp.fields & IR_HAS_SEQUENCE ? integerText(stamp.sequence) : "") << ','
            << (stamp.fields & IR_HAS_DRIVER_SEQUENCE ? integerText(stamp.driver_sequence) : "") << ','
            << (stamp.fields & IR_HAS_LOSS ? std::to_string(stamp.lost_frames) : "") << ','
            << geometry.width << ',' << geometry.height << ',' << geometry.width * 2 << ",RAW16_LE,"
            << geometry.valid_bits << ",little," << sourceTime << ',' << hostTime << ',' << monotonic << ','
            << relative << ',' << ((stamp.fields & IR_SEQUENCE_IS_CALLBACK) ? 1 : 0) << ','
            << ((stamp.fields & IR_PIXELS_INVERTED) ? 1 : 0) << '\n';
    }
    const auto csvText = csv.str();
    writeFile(directory / "frames.csv", csvText.data(), csvText.size());
}

void saveRecipeArtifacts(const fs::path &destination, const std::vector<SavedBatchRef> &batches,
    const std::string &resultJson)
{
    static_assert(std::endian::native == std::endian::little);
    checkDestination(destination);
    StagingDirectory stage(destination.parent_path().empty() ? fs::current_path() : destination.parent_path());

    for (const auto &batch : batches) {
        require(batch.frames != nullptr && batch.stamps != nullptr, IR_NULL_POINTER, "Missing batch buffer");
        const auto targetDir = batch.subdir.empty()
            ? stage.path()
            : (stage.path() / fs::path(reinterpret_cast<const char8_t *>(batch.subdir.c_str())));
        writeFrameBatchFiles(targetDir, batch.geometry, *batch.frames, *batch.stamps);
    }
    writeFile(stage.path() / "result.json", resultJson.data(), resultJson.size());
    stage.publish(destination);
}

void writeTextFileAtomicOrDirect(const fs::path &filePath, const std::string &content)
{
    const auto parent = filePath.parent_path();
    if (!parent.empty())
        fs::create_directories(parent);
    fs::path tmpPath = filePath;
    tmpPath += ".tmp";
    writeFile(tmpPath, content.data(), content.size());
#ifdef _WIN32
    if (!MoveFileExW(tmpPath.c_str(), filePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec;
        fs::remove(tmpPath, ec);
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Cannot finalize output text file");
    }
#else
    std::error_code ec;
    fs::rename(tmpPath, filePath, ec);
    if (ec) {
        std::error_code rmEc;
        fs::remove(tmpPath, rmEc);
        throw fs::filesystem_error("Cannot finalize output text file", filePath, ec);
    }
#endif
}

void saveFrameBatch(const fs::path &path, IRGeometry geometry,
    const std::vector<uint16_t> &frames, const std::vector<IRFrameStamp> &stamps,
    const std::string &resultJson)
{
    saveRecipeArtifacts(path, {{"", geometry, &frames, &stamps}}, resultJson);
}

void saveNoiseRun(const fs::path &path, const IRNoiseRecipe &recipe, int backend,
    const std::string &sourceName, IRGeometry geometry,
    const std::vector<uint16_t> &frames, const std::vector<IRFrameStamp> &stamps,
    const IRNoiseResult &result, double blackbodySetpointReadback,
    const IRBlackbodyReading *blackbodyReading, const std::string &startedUtc)
{
    const auto finishedUtc = utcTimestampNow();
    nlohmann::json record{
        {"recipe_id", recipe.recipe_id},
        {"station_version", IR_StationVersion()},
        {"analysis_version", IR_AnalysisVersion()},
        {"source_commit", ir::station::sourceCommit()},
        {"started_utc", startedUtc},
        {"finished_utc", finishedUtc},
        {"backend", backendName(backend)},
        {"source_name", sourceName},
        {"width", geometry.width}, {"height", geometry.height},
        {"valid_bits", geometry.valid_bits}, {"requested_frames", recipe.frames},
        {"completed_frames", result.completed}, {"frame_timeout_ms", recipe.frame_timeout_ms},
        {"mean_dn", result.mean_dn}, {"temporal_noise_dn", result.temporal_noise_dn},
        {"estimator", "mean_pixel_sample_std"}, {"unit", "DN"}
    };
    if (recipe.detector) {
        record["detector"] = {{"integration_us", recipe.integration_us},
                              {"frame_hz", recipe.frame_hz}, {"empty_us", recipe.empty_us}};
    }
    if (recipe.blackbody) {
        record["blackbody"] = {
            {"temperature_c", recipe.temperature_c},
            {"setpoint_readback_c", blackbodySetpointReadback},
            {"actual_c", blackbodyReading->actual_c},
            {"device_stable", bool(blackbodyReading->stable)},
            {"tolerance_c", recipe.tolerance_c},
            {"stable_window_ms", recipe.stable_window_ms},
            {"stable_timeout_ms", recipe.stable_timeout_ms},
            {"require_device_stable", bool(recipe.require_device_stable)}
        };
    }
    auto jsonText = record.dump(2);
    jsonText.push_back('\n');
    saveFrameBatch(path, geometry, frames, stamps, jsonText);
}

}
