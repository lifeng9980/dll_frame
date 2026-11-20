#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
namespace optmeas::raw
{
namespace fs = std::filesystem;
class AppError : public std::runtime_error
{
  public:
    explicit AppError(std::wstring message);
    const std::wstring &message() const noexcept
    {
        return message_;
    }

  private:
    std::wstring message_;
};
class Cancelled : public AppError
{
  public:
    Cancelled() : AppError(L"RAW operation cancelled") {};
};
struct Hooks
{
    std::function<void(std::wstring_view, bool)> log;
    std::function<bool()> cancelled;
    std::function<void(uint64_t, uint64_t)> progress;
};
struct Options
{
    std::vector<std::wstring> targets;
    std::wstring output;
    int width{}, height{}, stride{};
    bool hasWidth{}, hasHeight{}, hasStride{}, bigEndian{}, hasByteOrder{};
    long long start{}, count{-1};
    int digits{};
    bool overwrite{}, allowTrailing{}, info{}, quiet{}, noPause{}, recursive{};
    uint64_t maxFrameBytes{INT32_MAX}, maxIndexBytes{67108864};
    size_t maxDatasets{10000};
};
struct Geometry
{
    int width{}, height{}, stride{}, bitDepth{16};
    bool bigEndian{};
    std::wstring source;
    uint64_t frameBytes() const
    {
        return uint64_t(stride) * height;
    }
    std::wstring describe() const;
};
struct Table
{
    fs::path path;
    std::vector<std::string> headings;
    std::vector<uint64_t> offsets;
    std::optional<Geometry> geometry;
    uint64_t bytes{};
    fs::file_time_type modified{};
    std::optional<bool> inverted;
    std::optional<std::string> sourceId, sourceKind;
};
struct File
{
    fs::path rawPath;
    Geometry geometry;
    uint64_t bytes{}, frames{}, tail{};
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
Geometry validateGeometry(Geometry geometry);
fs::path resolveRawPath(const fs::path &);
File inspect(const fs::path &, const Options &options = {}, bool strictMetadata = true,
             const Hooks &hooks = {});
void checkUnchanged(const File &);
Metadata metadataAt(const File &, uint64_t index, std::istream &input);
Metadata metadata(const File &, uint64_t index);
std::vector<uint16_t> decode(const uint8_t *, size_t, const Geometry &);
std::vector<uint16_t> readFrame(const File &, uint64_t index);
} // namespace optmeas::raw
