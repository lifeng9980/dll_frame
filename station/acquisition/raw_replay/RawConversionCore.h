#pragma once
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
    int width{}, height{}, stride{}, bitDepth{16};
    bool bigEndian{};
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
std::vector<uint16_t> decode(const uint8_t *, size_t, const Geometry &);
} // namespace ir::raw
