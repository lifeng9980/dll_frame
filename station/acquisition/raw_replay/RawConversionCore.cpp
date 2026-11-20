#include "RawConversionCore.h"
#include <cmath>
#include <QString>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace optmeas::raw
{
namespace
{
constexpr int kBytesPerPixel = 2;
std::wstring fromUtf8(std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<int>(text.size())).toStdWString();
}

std::string toUtf8(std::wstring_view text)
{
    return QString::fromWCharArray(text.data(), static_cast<int>(text.size())).toUtf8().toStdString();
}

std::wstring widePath(const fs::path &path)
{
    const auto bytes = path.u8string();
    return fromUtf8({reinterpret_cast<const char *>(bytes.data()), bytes.size()});
}

std::wstring toWide(long long value)
{
    // 用窄字符版 to_chars 再手工加宽，避免不同标准库对 wchar_t 重载的支持差异。
    char buffer[32]{};
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value);
    std::wstring result;
    result.reserve(static_cast<size_t>(converted.ptr - buffer));
    for (const char *it = buffer; it != converted.ptr; ++it)
    {
        result.push_back(static_cast<wchar_t>(*it));
    }
    return result;
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

} // namespace
AppError::AppError(std::wstring message) : std::runtime_error(toUtf8(message)), message_(std::move(message))
{
}
namespace
{
void cancel(const Hooks &h)
{
    if (h.cancelled && h.cancelled())
        throw Cancelled();
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
            throw AppError(L"frames.csv line exceeds 64KiB");
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
        throw AppError(L"frames.csv row/header column count differs");
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
        throw AppError(L"Invalid frames.csv number: " + fromUtf8(key));
    return value;
}
template <class T> T required(const std::map<std::string, std::string> &row, const char *key)
{
    const auto value = number<T>(row, key);
    if (!value)
        throw AppError(L"Missing frames.csv number: " + fromUtf8(key));
    return *value;
}
std::optional<int64_t> millis(const std::map<std::string, std::string> &row, const char *key)
{
    const auto value = number<double>(row, key);
    if (!value)
        return {};
    const long double ns = static_cast<long double>(*value) * 1000000;
    if (!std::isfinite(*value) || ns < INT64_MIN || ns > INT64_MAX)
        throw AppError(L"Recorded time exceeds int64 nanoseconds");
    return static_cast<int64_t>(std::round(ns));
}
Metadata parseMetadata(std::map<std::string, std::string> row)
{
    if (const auto storage = number<int>(row, "storage_bits"))
        if (*storage != 16)
            throw AppError(L"Only 16-bit RAW storage is supported");
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
            throw AppError(L"Invalid pixels_inverted value");
        m.inverted = *value != 0;
    }
    (void)number<uint64_t>(m.fields, "source_id");
    (void)number<int>(m.fields, "source_kind");
    (void)number<uint64_t>(m.fields, "frame_index");
    return m;
}
std::shared_ptr<Table> readTable(const fs::path &path, const Options &options, bool strict, const Hooks &h)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw AppError(L"Cannot open " + widePath(path));
    auto table = std::make_shared<Table>();
    table->path = path;
    table->bytes = fs::file_size(path);
    table->modified = fs::last_write_time(path);
    std::string line;
    if (!recordLine(input, line))
        throw AppError(L"Empty frames.csv");
    if (line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0)
        line.erase(0, 3);
    table->headings = split(line);
    std::map<std::string, bool> unique;
    for (const auto &name : table->headings)
        if (name.empty() || !unique.emplace(name, true).second)
            throw AppError(L"Empty/duplicate frames.csv heading");
    const bool geometryColumns = unique.count("width") && unique.count("height");
    if (!options.hasWidth && unique.count("width") != unique.count("height"))
        throw AppError(L"frames.csv missing width/height pair");
    for (;;)
    {
        cancel(h);
        const auto offset = input.tellg();
        if (!recordLine(input, line))
            break;
        if (trimNarrow(line).empty())
            continue;
        if (table->offsets.size() >= options.maxIndexBytes / sizeof(uint64_t))
            throw AppError(L"frames.csv metadata index budget exceeded");
        const auto row = fields(*table, line);
        if (geometryColumns && !options.hasWidth)
        {
            Geometry current;
            current.width = required<int>(row, "width");
            current.height = required<int>(row, "height");
            if (current.width <= 0 || current.width > INT32_MAX / 2)
                throw AppError(L"RAW width exceeds bounds");
            current.stride = number<int>(row, "stride_bytes").value_or(current.width * 2);
            current.bitDepth = number<int>(row, "bit_depth").value_or(16);
            current.source = L"frames.csv";
            const auto text = [&](const char *key) {
                const auto it = row.find(key);
                return it == row.end() ? std::string() : it->second;
            };
            std::string format = text("pixel_format"), endian = text("endianness");
            for (auto &c : format)
                c = char(std::toupper(static_cast<unsigned char>(c)));
            for (auto &c : endian)
                c = char(std::tolower(static_cast<unsigned char>(c)));
            if (!format.empty() && format != "RAW16" && format != "RAW16_LE" && format != "RAW16_BE")
                throw AppError(L"Unsupported pixel_format in frames.csv");
            if (!endian.empty() && endian != "little" && endian != "big")
                throw AppError(L"Unsupported endianness in frames.csv");
            current.bigEndian = endian == "big" || (endian.empty() && format == "RAW16_BE");
            if ((format == "RAW16_LE" && current.bigEndian) || (format == "RAW16_BE" && !current.bigEndian))
                throw AppError(L"RAW format and byte order conflict");
            current = validateGeometry(current);
            if (table->geometry)
            {
                const auto &first = *table->geometry;
                if (first.width != current.width || first.height != current.height ||
                    first.stride != current.stride || first.bitDepth != current.bitDepth ||
                    first.bigEndian != current.bigEndian)
                    throw AppError(L"frames.csv geometry changes between rows");
            }
            else
                table->geometry = current;
        }
        if (strict)
        {
            const auto m = parseMetadata(row);
            if (m.inverted)
            {
                if (table->inverted && *table->inverted != *m.inverted)
                    throw AppError(L"Recorded pixel domain changes within dataset");
                table->inverted = m.inverted;
            }
            for (const auto &key : {"source_id", "source_kind"})
            {
                const auto it = row.find(key);
                if (it == row.end() || it->second.empty())
                    continue;
                auto &previous = std::string(key) == "source_id" ? table->sourceId : table->sourceKind;
                if (previous && *previous != it->second)
                    throw AppError(L"Recorded source changes within dataset");
                previous = it->second;
            }
        }
        if (table->offsets.size() == table->offsets.capacity())
            table->offsets.reserve(size_t(std::min<uint64_t>(
                options.maxIndexBytes / 8, std::max<uint64_t>(1, table->offsets.capacity() * 2))));
        table->offsets.push_back(uint64_t(offset));
    }
    if (input.bad() || table->offsets.empty())
        throw AppError(L"frames.csv has no readable frame records");
    if (fs::file_size(path) != table->bytes || fs::last_write_time(path) != table->modified)
        throw AppError(L"frames.csv changed while indexing");
    return table;
}
fs::path resolve(const fs::path &target)
{
    if (fs::is_regular_file(target))
        return fs::absolute(target);
    if (!fs::is_directory(target))
        throw AppError(L"Path does not exist: " + widePath(target));
    for (const auto &name : {target / L"frames.raw", target / L"RAW" / L"frames.raw"})
        if (fs::is_regular_file(name))
            return fs::absolute(name);
    throw AppError(L"No frames.raw in directory/RAW subdirectory: " + widePath(target));
}
bool contains(const fs::path &directory)
{
    std::error_code e;
    return fs::is_regular_file(directory / L"frames.raw", e) ||
           fs::is_regular_file(directory / L"RAW" / L"frames.raw", e);
}
Geometry fromMean(const fs::path &path, const Hooks &h)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw AppError(L"Cannot open " + widePath(path));
    Geometry g;
    g.source = L"mean.csv";
    std::string line;
    while (std::getline(input, line))
    {
        cancel(h);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        const auto cols = std::count(line.begin(), line.end(), ',') + 1;
        if (cols > INT32_MAX / 2 || g.height == INT32_MAX)
            throw AppError(L"mean.csv dimensions exceed bounds");
        if (!g.width)
            g.width = int(cols);
        else if (g.width != cols)
            throw AppError(L"mean.csv rows have different lengths");
        ++g.height;
    }
    g.stride = g.width * 2;
    return validateGeometry(g);
}
} // namespace
Geometry validateGeometry(Geometry g)
{
    if (g.width <= 0 || g.height <= 0 || g.width > INT32_MAX / 2 ||
        uint64_t(g.width) * g.height > INT32_MAX || g.stride < g.width * 2 || g.stride % 2 ||
        g.bitDepth < 1 || g.bitDepth > 16 || g.frameBytes() > INT32_MAX)
        throw AppError(L"Invalid RAW width/height/stride/depth or frame size");
    return g;
}
fs::path resolveRawPath(const fs::path &path)
{
    return resolve(path);
}
std::wstring Geometry::describe() const
{
    return toWide(width) + L" x " + toWide(height) + L", stride " + toWide(stride) + L", " +
           (bigEndian ? L"RAW16_BE" : L"RAW16_LE") + L", " + toWide(bitDepth) + L" bit, source " + source;
}
File inspect(const fs::path &target, const Options &options, bool strict, const Hooks &h)
{
    cancel(h);
    if (options.hasWidth != options.hasHeight)
        throw AppError(L"Width and height must be supplied together");
    if (options.hasStride && !options.hasWidth)
        throw AppError(L"Stride requires explicit width/height");
    if (options.hasWidth && (options.width <= 0 || options.width > INT32_MAX / 2))
        throw AppError(L"Explicit width exceeds bounds");
    File f;
    f.rawPath = resolve(target);
    const auto csv = f.rawPath.parent_path() / L"frames.csv";
    if (fs::is_regular_file(csv) && (strict || !options.hasWidth))
        f.table = readTable(csv, options, strict, h);
    if (options.hasWidth)
    {
        f.geometry = {
            options.width,     options.height, options.hasStride ? options.stride : options.width * 2, 16,
            options.bigEndian, L"explicit"};
    }
    else if (f.table && f.table->geometry)
        f.geometry = *f.table->geometry;
    else
        f.geometry = fromMean(f.rawPath.parent_path() / L"mean.csv", h);
    if (options.hasByteOrder)
        f.geometry.bigEndian = options.bigEndian;
    f.geometry = validateGeometry(f.geometry);
    if (f.geometry.frameBytes() > options.maxFrameBytes)
        throw AppError(L"RAW frame byte budget exceeded");
    f.bytes = fs::file_size(f.rawPath);
    f.modified = fs::last_write_time(f.rawPath);
    f.frames = f.bytes / f.geometry.frameBytes();
    f.tail = f.bytes % f.geometry.frameBytes();
    if (!f.frames || (f.tail && !options.allowTrailing))
        throw AppError(L"RAW length is not a complete positive frame stack");
    if (strict && f.table && f.table->offsets.size() != 1 && f.table->offsets.size() != f.frames)
        throw AppError(L"RAW frame count differs from metadata row count");
    f.perFrameMetadata = f.table && f.table->offsets.size() == f.frames;
    return f;
}
void checkUnchanged(const File &f)
{
    if (fs::file_size(f.rawPath) != f.bytes || fs::last_write_time(f.rawPath) != f.modified)
        throw AppError(L"RAW input changed after inspection");
    if (f.table && (fs::file_size(f.table->path) != f.table->bytes ||
                    fs::last_write_time(f.table->path) != f.table->modified))
        throw AppError(L"frames.csv changed after inspection");
}
Metadata metadata(const File &f, uint64_t index)
{
    checkUnchanged(f);
    if (!f.perFrameMetadata)
        return {};
    std::ifstream input(f.table->path, std::ios::binary);
    return metadataAt(f, index, input);
}
Metadata metadataAt(const File &f, uint64_t index, std::istream &input)
{
    if (index >= f.frames)
        throw AppError(L"RAW frame index outside dataset");
    if (!f.perFrameMetadata)
        return {};
    input.seekg(std::streamoff(f.table->offsets.at(size_t(index))));
    std::string line;
    if (!recordLine(input, line))
        throw AppError(L"Cannot read indexed frames.csv row");
    return parseMetadata(fields(*f.table, line));
}
std::vector<uint16_t> decode(const uint8_t *bytes, size_t count, const Geometry &g)
{
    if (!bytes || count != g.frameBytes())
        throw AppError(L"Incomplete RAW frame");
    std::vector<uint16_t> pixels(size_t(g.width) * g.height);
    for (int row = 0; row < g.height; ++row)
    {
        const auto *data = bytes + size_t(row) * g.stride;
        auto *out = pixels.data() + size_t(row) * g.width;
        if (!g.bigEndian)
            std::memcpy(out, data, size_t(g.width) * 2);
        else
            for (int col = 0; col < g.width; ++col)
                out[col] = uint16_t((uint16_t(data[col * 2]) << 8) | data[col * 2 + 1]);
    }
    return pixels;
}
std::vector<uint16_t> readFrame(const File &f, uint64_t index)
{
    checkUnchanged(f);
    if (index >= f.frames)
        throw AppError(L"RAW frame index outside dataset");
    std::ifstream input(f.rawPath, std::ios::binary);
    input.seekg(std::streamoff(index * f.geometry.frameBytes()));
    std::vector<uint8_t> data(size_t(f.geometry.frameBytes()));
    input.read(reinterpret_cast<char *>(data.data()), std::streamsize(data.size()));
    if (size_t(input.gcount()) != data.size())
        throw AppError(L"Cannot read complete RAW frame");
    return decode(data.data(), data.size(), f.geometry);
}
} // namespace optmeas::raw
