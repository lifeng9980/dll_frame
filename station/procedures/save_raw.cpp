#include "recipe_runtime_common.h"
#include "api_common.h"
#include "utf8_path.h"
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
#if defined(__linux__)
#include <linux/fs.h>
#endif
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace ir::procedures {
using namespace ir::api;
namespace fs = std::filesystem;
namespace {
// 文件系统异常的 what() 使用 ANSI 代码页，路径会乱码；统一改为 UTF-8 文本。
[[noreturn]] void throwIo(const char *what, const fs::path &path, const std::error_code &code)
{
    throw Error(IR_IO_ERROR, std::string(what) + " '" + utf8::pathToUtf8(path) + "': " +
                                 utf8::systemErrorText(code));
}

const char *timeOriginName(int32_t origin) noexcept
{
    switch (origin) {
    case IR_TIME_SOURCE: return "source";
    case IR_TIME_HOST: return "host";
    case IR_TIME_RECORDED_MONOTONIC: return "recorded_monotonic";
    case IR_TIME_RECORDED_RELATIVE: return "recorded_relative";
    default: return "none";
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
                throwIo("Cannot create capture staging directory", path_, error);
        }
        throwIo("Cannot choose a unique capture staging directory", parent,
                std::make_error_code(std::errc::file_exists));
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
            throwIo("Cannot publish capture directory", destination,
                    std::error_code(static_cast<int>(GetLastError()), std::system_category()));
#else
        bool published = false;
#if defined(__linux__) && defined(SYS_renameat2) && defined(RENAME_NOREPLACE)
        if (::syscall(SYS_renameat2, AT_FDCWD, path_.c_str(), AT_FDCWD,
                      destination.c_str(), RENAME_NOREPLACE) == 0) {
            published = true;
        } else {
            const auto renameError = errno;
            if (renameError != EINVAL && renameError != ENOSYS && renameError != EOPNOTSUPP)
                throwIo("Cannot publish capture directory", destination,
                        {renameError, std::system_category()});
        }
#endif
        if (!published) {
            std::error_code statusError;
            const auto target = fs::symlink_status(destination, statusError);
            if (statusError == std::errc::no_such_file_or_directory) statusError.clear();
            if (statusError)
                throwIo("Cannot inspect capture destination", destination, statusError);
            if (target.type() != fs::file_type::not_found)
                throwIo("Capture destination already exists", destination,
                        std::make_error_code(std::errc::file_exists));
            fs::rename(path_, destination, statusError);
            if (statusError) throwIo("Cannot publish capture directory", destination, statusError);
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
    if (error) throwIo("Cannot inspect capture destination", path, error);
    if (target.type() != fs::file_type::not_found)
        throw Error(IR_IO_ERROR, "Capture destination already exists: '" + utf8::pathToUtf8(path) + "'");

    auto parent = path.parent_path();
    if (parent.empty()) parent = fs::current_path(error);
    require(!error && fs::is_directory(parent, error) && !error, IR_IO_ERROR,
            "Specify a new capture directory under an existing parent");
}

void writeFrameBatchFiles(const fs::path &directory, const recipe::FrameBatch &batch)
{
    static_assert(std::endian::native == std::endian::little);
    const auto pixels = recipe::frameBatchArrayView(batch);
    const auto elementBytes = recipe::pixelElementBytes(pixels.data_type);
    const auto pixelFormat = recipe::pixelFormatName(pixels.data_type);
    require(elementBytes != 0, IR_UNSUPPORTED, "Cannot archive unsupported frame pixel type");
    const auto geometry = batch.geometry;
    {
        std::error_code ec;
        fs::create_directories(directory, ec);
        if (ec) throwIo("Cannot create output directory", directory, ec);
    }
    writeFile(directory / "frames.raw", static_cast<const char *>(pixels.data),
              static_cast<size_t>(pixels.byte_size));

    std::ostringstream csv;
    csv.imbue(std::locale::classic());
    csv << "frame_index,application_sequence,driver_frame_number,lost_frames,width,height,stride_bytes,pixel_format,bit_depth,endianness,"
           "source_timestamp_ns,received_monotonic_ns,monotonic_ms,relative_time_ms,callback_sequence,pixels_inverted,timestamp_ms,time_origin,delta_ms\n";
    for (size_t index = 0; index < batch.stamps.size(); ++index) {
        const auto &stamp = batch.stamps[index];
        const auto sourceTime = stamp.time_origin == IR_TIME_SOURCE ? integerText(stamp.timestamp_ns) : "";
        const auto hostTime = stamp.time_origin == IR_TIME_HOST ? integerText(stamp.timestamp_ns) : "";
        const auto monotonic = stamp.time_origin == IR_TIME_RECORDED_MONOTONIC
            ? decimalMilliseconds(stamp.timestamp_ns) : "";
        const auto relative = stamp.time_origin == IR_TIME_RECORDED_RELATIVE
            ? decimalMilliseconds(stamp.timestamp_ns) : "";
        const auto timestampMs = stamp.time_origin != IR_TIME_UNKNOWN
            ? decimalMilliseconds(stamp.timestamp_ns) : "";
        std::string deltaMs;
        if (index > 0 && stamp.time_origin != IR_TIME_UNKNOWN &&
            batch.stamps[index - 1].time_origin == stamp.time_origin) {
            deltaMs = decimalMilliseconds(stamp.timestamp_ns - batch.stamps[index - 1].timestamp_ns);
        }

        csv << index << ','
            << (stamp.fields & IR_HAS_SEQUENCE ? integerText(stamp.sequence) : "") << ','
            << (stamp.fields & IR_HAS_DRIVER_SEQUENCE ? integerText(stamp.driver_sequence) : "") << ','
            << (stamp.fields & IR_HAS_LOSS ? std::to_string(stamp.lost_frames) : "") << ','
            << geometry.width << ',' << geometry.height << ','
            << static_cast<uint64_t>(geometry.width) * elementBytes << ',' << pixelFormat << ','
            << geometry.valid_bits << ",little," << sourceTime << ',' << hostTime << ',' << monotonic << ','
            << relative << ',' << ((stamp.fields & IR_SEQUENCE_IS_CALLBACK) ? 1 : 0) << ','
            << ((stamp.fields & IR_PIXELS_INVERTED) ? 1 : 0) << ','
            << timestampMs << ',' << timeOriginName(stamp.time_origin) << ',' << deltaMs << '\n';
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
        require(batch.batch != nullptr, IR_NULL_POINTER, "Missing batch buffer");
        const auto targetDir = batch.subdir.empty()
            ? stage.path()
            : (stage.path() / ir::utf8::pathFromUtf8(batch.subdir));
        writeFrameBatchFiles(targetDir, *batch.batch);
    }
    writeFile(stage.path() / "result.json", resultJson.data(), resultJson.size());
    stage.publish(destination);
}

void writeTextFileAtomicOrDirect(const fs::path &filePath, const std::string &content)
{
    const auto parent = filePath.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
        if (ec) throwIo("Cannot create output directory", parent, ec);
    }
    fs::path tmpPath = filePath;
    tmpPath += ".tmp";
    writeFile(tmpPath, content.data(), content.size());
#ifdef _WIN32
    if (!MoveFileExW(tmpPath.c_str(), filePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const std::error_code cause(static_cast<int>(GetLastError()), std::system_category());
        std::error_code ec;
        fs::remove(tmpPath, ec);
        throwIo("Cannot finalize output text file", filePath, cause);
    }
#else
    std::error_code ec;
    fs::rename(tmpPath, filePath, ec);
    if (ec) {
        std::error_code rmEc;
        fs::remove(tmpPath, rmEc);
        throwIo("Cannot finalize output text file", filePath, ec);
    }
#endif
}

}
