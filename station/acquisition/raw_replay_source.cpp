#include "frame_source.h"
#include "api_common.h"
#include "utf8_path.h"
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
namespace ir::raw
{
namespace fs = std::filesystem;
class AppError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};
struct Geometry
{
    int width{}, height{}, stride{}, bitDepth{16}, dataType{IR_TYPE_U16};
    bool bigEndian{};
    uint64_t bytesPerPixel() const
    {
        return dataType == IR_TYPE_U32 ? sizeof(uint32_t) : sizeof(uint16_t);
    }
    uint64_t frameBytes() const
    {
        return uint64_t(stride) * height;
    }
};
struct Table
{
    fs::path path;
    std::vector<std::string> headings;
    std::vector<uint64_t> offsets;
    Geometry geometry;
    uint64_t bytes{};
    fs::file_time_type modified{};
    std::optional<bool> inverted;
    std::optional<std::string> sourceId, sourceKind;
};
struct File
{
    fs::path rawPath;
    Geometry geometry;
    uint64_t bytes{}, frames{};
    fs::file_time_type modified{};
    std::shared_ptr<const Table> table;
    bool perFrameMetadata{};
};
struct Metadata
{
    std::map<std::string, std::string> fields;
    std::optional<uint64_t> applicationSequence, inputSequence, driverSequence;
    std::optional<int64_t> sourceNs, hostNs;
    std::optional<uint32_t> lost;
    std::optional<bool> inverted;
    std::string timestampOrigin;
};
File inspect(const fs::path &target);
void checkUnchanged(const File &);
Metadata metadataAt(const File &, uint64_t index, std::istream &input);
void decode(const uint8_t *, size_t, const Geometry &, void *pixels);
} // namespace ir::raw

#include <cmath>

#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ir::raw
{
namespace
{
constexpr uint64_t maxIndexBytes = 64 * 1024 * 1024;

Geometry validateGeometry(Geometry g)
{
    const int bytesPerPixel = static_cast<int>(g.bytesPerPixel());
    if ((g.dataType != IR_TYPE_U16 && g.dataType != IR_TYPE_U32) ||
        g.width <= 0 || g.height <= 0 || g.width > INT32_MAX / bytesPerPixel ||
        uint64_t(g.width) * g.height > INT32_MAX || g.stride < g.width * bytesPerPixel ||
        g.stride % bytesPerPixel || g.bitDepth < 0 || g.bitDepth > bytesPerPixel * 8 ||
        g.frameBytes() > INT32_MAX)
        throw AppError("Invalid RAW width/height/stride/depth or frame size");
    return g;
}

std::string trimNarrow(std::string text)
{
    while (!text.empty() &&
           (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '\t'))
    {
        text.pop_back();
    }
    size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t'))
        ++begin;
    return text.substr(begin);
}

bool recordLine(std::istream &input, std::string &line)
{
    line.clear();
    char c;
    while (input.get(c))
    {
        if (c == '\n')
            return true;
        if (line.size() >= 65536)
            throw AppError("frames.csv line exceeds 64KiB");
        line += c;
    }
    return !line.empty();
}
std::vector<std::string> split(const std::string &line)
{
    std::vector<std::string> result;
    std::string current;
    for (char c : line)
    {
        if (c == ',')
        {
            result.push_back(trimNarrow(current));
            current.clear();
        }
        else
            current += c;
    }
    result.push_back(trimNarrow(current));
    return result;
}
std::map<std::string, std::string> fields(const Table &table, const std::string &line)
{
    const auto cells = split(line);
    if (cells.size() != table.headings.size())
        throw AppError("frames.csv row/header column count differs");
    std::map<std::string, std::string> values;
    for (size_t i = 0; i < cells.size(); ++i)
        values.emplace(table.headings[i], cells[i]);
    return values;
}
template <class T> std::optional<T> number(const std::map<std::string, std::string> &row, const char *key)
{
    const auto it = row.find(key);
    if (it == row.end() || it->second.empty())
        return {};
    T value{};
    const auto &s = it->second;
    const auto parsed = std::from_chars(s.data(), s.data() + s.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != s.data() + s.size())
        throw AppError("Invalid frames.csv number: " + std::string(key));
    return value;
}
template <class T> T required(const std::map<std::string, std::string> &row, const char *key)
{
    const auto value = number<T>(row, key);
    if (!value)
        throw AppError("Missing frames.csv number: " + std::string(key));
    return *value;
}
std::optional<int64_t> millis(const std::map<std::string, std::string> &row, const char *key)
{
    const auto value = number<double>(row, key);
    if (!value)
        return {};
    const long double ns = static_cast<long double>(*value) * 1000000;
    if (!std::isfinite(*value) || ns < INT64_MIN || ns > INT64_MAX)
        throw AppError("Recorded time exceeds int64 nanoseconds");
    return static_cast<int64_t>(std::round(ns));
}
Metadata parseMetadata(std::map<std::string, std::string> row, int expectedStorageBits)
{
    if (const auto storage = number<int>(row, "storage_bits"))
        if ((*storage != 16 && *storage != 32) || *storage != expectedStorageBits)
            throw AppError("storage_bits conflicts with RAW pixel type");
    Metadata m;
    m.fields = std::move(row);
    m.applicationSequence = number<uint64_t>(m.fields, "application_sequence");
    m.inputSequence = number<uint64_t>(m.fields, "input_sequence");
    m.driverSequence = number<uint64_t>(m.fields, "driver_frame_number");
    if (!m.driverSequence)
        m.driverSequence = number<uint64_t>(m.fields, "driver_sequence");
    m.lost = number<uint32_t>(m.fields, "lost_frames");
    if (!m.lost)
        m.lost = number<uint32_t>(m.fields, "lost_since_previous");
    m.hostNs = number<int64_t>(m.fields, "received_monotonic_ns");
    m.sourceNs = number<int64_t>(m.fields, "source_timestamp_ns");
    if (m.sourceNs)
        m.timestampOrigin = "source_timestamp_ns";
    else if (m.hostNs)
    {
        m.sourceNs = m.hostNs;
        m.timestampOrigin = "received_monotonic_ns";
    }
    else if (const auto value = millis(m.fields, "monotonic_ms"))
    {
        m.sourceNs = value;
        m.timestampOrigin = "monotonic_ms";
    }
    else if (const auto value = millis(m.fields, "relative_time_ms"))
    {
        m.sourceNs = value;
        m.timestampOrigin = "relative_time_ms";
    }
    if (const auto value = number<int>(m.fields, "pixels_inverted"))
    {
        if (*value != 0 && *value != 1)
            throw AppError("Invalid pixels_inverted value");
        m.inverted = *value != 0;
    }
    (void)number<uint64_t>(m.fields, "source_id");
    (void)number<int>(m.fields, "source_kind");
    (void)number<uint64_t>(m.fields, "frame_index");
    return m;
}
Geometry fromMean(const fs::path &path);

Geometry parseLayout(const std::map<std::string, std::string> &row, Geometry current)
{
    const auto text = [&](const char *key) {
        const auto it = row.find(key);
        return it == row.end() ? std::string() : it->second;
    };
    std::string format = text("pixel_format"), endian = text("endianness");
    for (auto &c : format)
        c = char(std::toupper(static_cast<unsigned char>(c)));
    for (auto &c : endian)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    if (!format.empty() && format != "RAW16" && format != "RAW16_LE" &&
        format != "RAW16_BE" && format != "RAW32_LE")
        throw AppError("Unsupported pixel_format in frames.csv");
    if (!endian.empty() && endian != "little" && endian != "big")
        throw AppError("Unsupported endianness in frames.csv");

    current.dataType = format == "RAW32_LE" ? IR_TYPE_U32 : IR_TYPE_U16;
    const int bytesPerPixel = static_cast<int>(current.bytesPerPixel());
    if (current.width <= 0 || current.width > INT32_MAX / bytesPerPixel)
        throw AppError("RAW width exceeds bounds for pixel type");
    current.stride = number<int>(row, "stride_bytes").value_or(current.width * bytesPerPixel);
    current.bitDepth = number<int>(row, "bit_depth").value_or(bytesPerPixel * 8);
    current.bigEndian = endian == "big" || (endian.empty() && format == "RAW16_BE");
    if (((format == "RAW16_LE" || format == "RAW32_LE") && current.bigEndian) ||
        (format == "RAW16_BE" && !current.bigEndian))
        throw AppError("RAW format and byte order conflict");
    return validateGeometry(current);
}

std::shared_ptr<Table> readTable(const fs::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw AppError("Cannot open " + ir::utf8::pathToUtf8(path));
    auto table = std::make_shared<Table>();
    table->path = path;
    table->bytes = fs::file_size(path);
    table->modified = fs::last_write_time(path);
    std::string line;
    if (!recordLine(input, line))
        throw AppError("Empty frames.csv");
    if (line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0)
        line.erase(0, 3);
    table->headings = split(line);
    std::map<std::string, bool> unique;
    for (const auto &name : table->headings)
        if (name.empty() || !unique.emplace(name, true).second)
            throw AppError("Empty/duplicate frames.csv heading");
    const bool geometryColumns = unique.count("width") && unique.count("height");
    if (unique.count("width") != unique.count("height"))
        throw AppError("frames.csv missing width/height pair");
    const Geometry fallback = geometryColumns ? Geometry{} : fromMean(path.parent_path() / "mean.csv");
    for (;;)
    {
        const auto offset = input.tellg();
        if (!recordLine(input, line))
            break;
        if (trimNarrow(line).empty())
            continue;
        if (table->offsets.size() >= maxIndexBytes / sizeof(uint64_t))
            throw AppError("frames.csv metadata index budget exceeded");
        const auto row = fields(*table, line);
        Geometry current = fallback;
        if (geometryColumns)
        {
            current.width = required<int>(row, "width");
            current.height = required<int>(row, "height");
        }
        current = parseLayout(row, current);
        if (table->offsets.empty())
            table->geometry = current;
        else
        {
            const auto &first = table->geometry;
            if (first.width != current.width || first.height != current.height ||
                first.stride != current.stride || first.bitDepth != current.bitDepth ||
                first.dataType != current.dataType || first.bigEndian != current.bigEndian)
                throw AppError("frames.csv geometry changes between rows");
        }
        {
            const auto m = parseMetadata(row, static_cast<int>(current.bytesPerPixel()) * 8);
            if (m.inverted)
            {
                if (table->inverted && *table->inverted != *m.inverted)
                    throw AppError("Recorded pixel domain changes within dataset");
                table->inverted = m.inverted;
            }
            for (const auto &key : {"source_id", "source_kind"})
            {
                const auto it = row.find(key);
                if (it == row.end() || it->second.empty())
                    continue;
                auto &previous = std::string(key) == "source_id" ? table->sourceId : table->sourceKind;
                if (previous && *previous != it->second)
                    throw AppError("Recorded source changes within dataset");
                previous = it->second;
            }
        }
        if (table->offsets.size() == table->offsets.capacity())
            table->offsets.reserve(size_t(std::min<uint64_t>(
                maxIndexBytes / 8, std::max<uint64_t>(1, table->offsets.capacity() * 2))));
        table->offsets.push_back(uint64_t(offset));
    }
    if (input.bad() || table->offsets.empty())
        throw AppError("frames.csv has no readable frame records");
    if (fs::file_size(path) != table->bytes || fs::last_write_time(path) != table->modified)
        throw AppError("frames.csv changed while indexing");
    return table;
}
fs::path resolve(const fs::path &target)
{
    if (fs::is_regular_file(target))
        return fs::absolute(target);
    if (!fs::is_directory(target))
        throw AppError("Path does not exist: " + ir::utf8::pathToUtf8(target));
    for (const auto &name : {target / "frames.raw", target / "RAW" / "frames.raw"})
        if (fs::is_regular_file(name))
            return fs::absolute(name);
    throw AppError("No frames.raw in directory/RAW subdirectory: " + ir::utf8::pathToUtf8(target));
}
Geometry fromMean(const fs::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw AppError("Cannot open " + ir::utf8::pathToUtf8(path));
    Geometry g;
    std::string line;
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        const auto cols = std::count(line.begin(), line.end(), ',') + 1;
        if (cols > INT32_MAX / 2 || g.height == INT32_MAX)
            throw AppError("mean.csv dimensions exceed bounds");
        if (!g.width)
            g.width = int(cols);
        else if (g.width != cols)
            throw AppError("mean.csv rows have different lengths");
        ++g.height;
    }
    g.stride = g.width * 2;
    return validateGeometry(g);
}
} // namespace

File inspect(const fs::path &target)
{
    File f;
    f.rawPath = resolve(target);
    const auto csv = f.rawPath.parent_path() / "frames.csv";
    if (fs::is_regular_file(csv))
        f.table = readTable(csv);
    if (f.table)
        f.geometry = f.table->geometry;
    else
        f.geometry = fromMean(f.rawPath.parent_path() / "mean.csv");
    f.bytes = fs::file_size(f.rawPath);
    f.modified = fs::last_write_time(f.rawPath);
    f.frames = f.bytes / f.geometry.frameBytes();
    if (!f.frames || f.bytes % f.geometry.frameBytes())
        throw AppError("RAW length is not a complete positive frame stack");
    if (f.table && f.table->offsets.size() != 1 && f.table->offsets.size() != f.frames)
        throw AppError("RAW frame count differs from metadata row count");
    f.perFrameMetadata = f.table && f.table->offsets.size() == f.frames;
    return f;
}

void checkUnchanged(const File &f)
{
    if (fs::file_size(f.rawPath) != f.bytes || fs::last_write_time(f.rawPath) != f.modified)
        throw AppError("RAW input changed after inspection");
    if (f.table && (fs::file_size(f.table->path) != f.table->bytes ||
                    fs::last_write_time(f.table->path) != f.table->modified))
        throw AppError("frames.csv changed after inspection");
}
Metadata metadataAt(const File &f, uint64_t index, std::istream &input)
{
    if (index >= f.frames)
        throw AppError("RAW frame index outside dataset");
    if (!f.perFrameMetadata)
        return {};
    input.seekg(std::streamoff(f.table->offsets.at(size_t(index))));
    std::string line;
    if (!recordLine(input, line))
        throw AppError("Cannot read indexed frames.csv row");
    return parseMetadata(fields(*f.table, line),
                         f.geometry.dataType == IR_TYPE_U32 ? 32 : 16);
}
void decode(const uint8_t *bytes, size_t count, const Geometry &g, void *pixels)
{
    if (!bytes || !pixels || count != g.frameBytes())
        throw AppError("Incomplete RAW frame");
    for (int row = 0; row < g.height; ++row)
    {
        const auto *data = bytes + size_t(row) * g.stride;
        if (g.dataType == IR_TYPE_U16) {
            auto *out = static_cast<uint16_t *>(pixels) + size_t(row) * g.width;
            if (!g.bigEndian)
                std::memcpy(out, data, size_t(g.width) * sizeof(uint16_t));
            else
                for (int col = 0; col < g.width; ++col)
                    out[col] = uint16_t((uint16_t(data[col * 2]) << 8) | data[col * 2 + 1]);
        } else {
            auto *out = static_cast<uint32_t *>(pixels) + size_t(row) * g.width;
            for (int col = 0; col < g.width; ++col)
                out[col] = uint32_t(data[col * 4]) |
                           (uint32_t(data[col * 4 + 1]) << 8) |
                           (uint32_t(data[col * 4 + 2]) << 16) |
                           (uint32_t(data[col * 4 + 3]) << 24);
        }
    }
}
} // namespace ir::raw

#include <fstream>
namespace ir::acquisition {
using namespace ir::api;
class ReplaySource final : public FrameSource {
    raw::File file_;
    std::ifstream pixels_;
    std::ifstream metadata_;
    std::vector<uint8_t> scratch_;
    uint64_t next_{};
    void readPixels(void *out)
    {
        const auto &g = file_.geometry;
        const auto size = std::streamsize(g.frameBytes());
        if (!g.bigEndian && std::endian::native == std::endian::little &&
            g.stride == g.width * static_cast<int>(g.bytesPerPixel())) {
            require(bool(pixels_.read(reinterpret_cast<char *>(out), size)), IR_IO_ERROR, "Incomplete RAW frame");
        } else {
            scratch_.resize(size_t(size));
            require(bool(pixels_.read(reinterpret_cast<char *>(scratch_.data()), size)), IR_IO_ERROR, "Incomplete RAW frame");
            raw::decode(scratch_.data(), size_t(size), g, out);
        }
    }
public:
    explicit ReplaySource(const char *directory) :
        file_(raw::inspect(ir::utf8::pathFromUtf8(directory ? directory : ""))) {}
    IRGeometry geometry() const override {
        const auto &g = file_.geometry;
        return {g.width, g.height, g.bitDepth};
    }
    int32_t dataType() const noexcept override { return file_.geometry.dataType; }
    bool isLive() const noexcept override { return false; }
    uint64_t bufferedBytes() const noexcept override
    {
        const auto &g = file_.geometry;
        const bool directRead = !g.bigEndian && std::endian::native == std::endian::little &&
            g.stride == g.width * static_cast<int>(g.bytesPerPixel());
        return directRead ? 0 : g.frameBytes();
    }
    void start() override {
        try { raw::checkUnchanged(file_); }
        catch (const raw::AppError &e) { throw Error(IR_IO_ERROR, e.what()); }
        std::ifstream index;
        if (file_.perFrameMetadata) {
            index.open(file_.table->path, std::ios::binary);
            require(index.is_open(), IR_IO_ERROR, "Cannot open RAW index");
        }
        pixels_.open(file_.rawPath, std::ios::binary);
        require(pixels_.is_open(), IR_IO_ERROR, "Cannot open RAW frame file");
        metadata_ = std::move(index);
        next_ = 0;
    }
    void stop() override {
        pixels_.close();
        if (metadata_.is_open()) metadata_.close();
    }
    void read(void *out, IRFrameStamp &s, int) override {
        require(next_ < file_.frames, IR_END_OF_STREAM, "End of RAW capture");
        try {
            const auto m = raw::metadataAt(file_, next_, metadata_);
            readPixels(out);
            if (m.applicationSequence) { s.sequence = *m.applicationSequence; s.fields |= IR_HAS_SEQUENCE; }
            if (m.driverSequence) { s.driver_sequence = *m.driverSequence; s.fields |= IR_HAS_DRIVER_SEQUENCE; }
            if (m.lost) { s.lost_frames = *m.lost; s.fields |= IR_HAS_LOSS; }
            if (m.inverted.value_or(false)) s.fields |= IR_PIXELS_INVERTED;
            const auto callback = m.fields.find("callback_sequence");
            if (callback != m.fields.end() && callback->second == "1") s.fields |= IR_SEQUENCE_IS_CALLBACK;
            if (m.sourceNs) {
                s.timestamp_ns = *m.sourceNs;
                if (m.timestampOrigin == "received_monotonic_ns") s.time_origin = IR_TIME_HOST;
                else if (m.timestampOrigin == "monotonic_ms") s.time_origin = IR_TIME_RECORDED_MONOTONIC;
                else if (m.timestampOrigin == "relative_time_ms") s.time_origin = IR_TIME_RECORDED_RELATIVE;
                else s.time_origin = IR_TIME_SOURCE;
            }
            ++next_;
        } catch (const raw::AppError &e) { throw Error(IR_IO_ERROR, e.what()); }
    }
};
std::unique_ptr<FrameSource> openReplay(const char *directory)
{
    try { return std::make_unique<ReplaySource>(directory); }
    catch (const raw::AppError &e) { throw Error(IR_IO_ERROR, e.what()); }
}
}
