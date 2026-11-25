#include "plugin_manager.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <mutex>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace ir::plugins {

namespace {

void setError(std::string *errOut, std::string msg)
{
    if (errOut != nullptr)
        *errOut = std::move(msg);
}

bool isNonEmptyCString(const char *s) noexcept
{
    return s != nullptr && s[0] != '\0';
}

std::string pathToUtf8(const std::filesystem::path &p)
{
    const auto u = p.u8string();
    return {reinterpret_cast<const char *>(u.data()), u.size()};
}

std::filesystem::path pathFromUtf8(const std::string &utf8)
{
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

#if defined(_WIN32)
std::string formatWin32Error(DWORD code)
{
    if (code == 0)
        return "unknown Win32 error";
    LPWSTR wideBuf = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD len = FormatMessageW(
        flags, nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&wideBuf), 0, nullptr);
    if (len == 0 || wideBuf == nullptr)
        return "Win32 error code " + std::to_string(code);
    const int utf8Len = WideCharToMultiByte(
        CP_UTF8, 0, wideBuf, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
    std::string out(utf8Len > 0 ? static_cast<size_t>(utf8Len) : 0, '\0');
    if (utf8Len > 0) {
        WideCharToMultiByte(CP_UTF8, 0, wideBuf, static_cast<int>(len),
                            out.data(), utf8Len, nullptr, nullptr);
    }
    LocalFree(wideBuf);
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    return out;
}
#endif

bool hasPlatformLibraryExtension(const std::filesystem::path &p)
{
    std::string ext = pathToUtf8(p.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? (c - 'A' + 'a') : c);
    });
#if defined(_WIN32)
    return ext == ".dll";
#elif defined(__APPLE__)
    return ext == ".dylib" || ext == ".so";
#else
    return ext == ".so";
#endif
}

class Sha256 {
public:
    void update(const uint8_t *data, size_t len) noexcept
    {
        for (size_t i = 0; i < len; ++i) {
            buffer_[bufferLen_++] = data[i];
            if (bufferLen_ == 64) {
                transform(buffer_.data());
                bitLen_ += 512;
                bufferLen_ = 0;
            }
        }
    }

    std::string finalizeHex()
    {
        size_t i = bufferLen_;
        if (bufferLen_ < 56) {
            buffer_[i++] = 0x80;
            while (i < 56)
                buffer_[i++] = 0x00;
        } else {
            buffer_[i++] = 0x80;
            while (i < 64)
                buffer_[i++] = 0x00;
            transform(buffer_.data());
            std::fill(buffer_.begin(), buffer_.begin() + 56, uint8_t{0});
        }

        bitLen_ += static_cast<uint64_t>(bufferLen_) * 8ULL;
        for (int b = 0; b < 8; ++b)
            buffer_[63 - b] = static_cast<uint8_t>((bitLen_ >> (b * 8)) & 0xFFU);
        transform(buffer_.data());

        static constexpr char kHex[] = "0123456789abcdef";
        std::string out(64, '0');
        for (size_t w = 0; w < 8; ++w) {
            for (int b = 0; b < 4; ++b) {
                const uint8_t byte = static_cast<uint8_t>((state_[w] >> ((3 - b) * 8)) & 0xFFU);
                out[w * 8 + size_t(b) * 2] = kHex[byte >> 4];
                out[w * 8 + size_t(b) * 2 + 1] = kHex[byte & 0x0F];
            }
        }
        return out;
    }

private:
    static inline uint32_t rotr(uint32_t x, uint32_t n) noexcept
    {
        return (x >> n) | (x << (32U - n));
    }

    void transform(const uint8_t *chunk) noexcept
    {
        static constexpr uint32_t kRound[64]{
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
            0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
            0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
            0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
            0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
        };
        uint32_t m[64]{};
        for (int i = 0; i < 16; ++i) {
            m[i] = (uint32_t(chunk[i * 4]) << 24) |
                   (uint32_t(chunk[i * 4 + 1]) << 16) |
                   (uint32_t(chunk[i * 4 + 2]) << 8) |
                   uint32_t(chunk[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(m[i - 15], 7) ^ rotr(m[i - 15], 18) ^ (m[i - 15] >> 3);
            const uint32_t s1 = rotr(m[i - 2], 17) ^ rotr(m[i - 2], 19) ^ (m[i - 2] >> 10);
            m[i] = m[i - 16] + s0 + m[i - 7] + s1;
        }
        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = h + s1 + ch + kRound[i] + m[i];
            const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    std::array<uint8_t, 64> buffer_{};
    size_t bufferLen_{0};
    uint64_t bitLen_{0};
    std::array<uint32_t, 8> state_{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };
};

bool computeFileSha256(const std::filesystem::path &path, std::string &hexOut, std::string *errOut)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        setError(errOut, "cannot open plugin library for hashing: " + pathToUtf8(path));
        return false;
    }
    Sha256 hasher;
    std::array<char, 65536> buf{};
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = in.gcount();
        if (got > 0)
            hasher.update(reinterpret_cast<const uint8_t *>(buf.data()), static_cast<size_t>(got));
    }
    if (in.bad()) {
        setError(errOut, "failed reading plugin library for hashing: " + pathToUtf8(path));
        return false;
    }
    hexOut = hasher.finalizeHex();
    return true;
}

} // namespace

struct LibraryHandle {
#if defined(_WIN32)
    HMODULE native{nullptr};
#else
    void *native{nullptr};
#endif
    std::string utf8Path;

    ~LibraryHandle() { close(); }

    void close() noexcept
    {
        if (native == nullptr)
            return;
#if defined(_WIN32)
        FreeLibrary(native);
#else
        dlclose(native);
#endif
        native = nullptr;
    }
};

PluginManager::PluginManager() = default;
PluginManager::~PluginManager() { unloadAll(); }

PluginManager::PluginManager(PluginManager &&other) noexcept
{
    std::unique_lock<std::shared_mutex> lock(other.mutex_);
    libraries_ = std::move(other.libraries_);
    metricsByMethod_ = std::move(other.metricsByMethod_);
}

PluginManager &PluginManager::operator=(PluginManager &&other) noexcept
{
    if (this == &other)
        return *this;
    std::scoped_lock lock(mutex_, other.mutex_);
    metricsByMethod_.clear();
    libraries_.clear();
    libraries_ = std::move(other.libraries_);
    metricsByMethod_ = std::move(other.metricsByMethod_);
    return *this;
}

bool PluginManager::isValidPluginMethodName(std::string_view methodName) noexcept
{
    constexpr std::string_view kPrefix = "plugin:";
    if (methodName.size() <= kPrefix.size() || methodName.substr(0, kPrefix.size()) != kPrefix)
        return false;
    const std::string_view rest = methodName.substr(kPrefix.size());
    bool segmentStart = true;
    for (char ch : rest) {
        if (ch == '.') {
            if (segmentStart)
                return false;
            segmentStart = true;
            continue;
        }
        const bool okChar = (ch >= 'a' && ch <= 'z') ||
                            (ch >= '0' && ch <= '9') ||
                            ch == '_';
        if (!okChar)
            return false;
        segmentStart = false;
    }
    return !segmentStart;
}

int32_t PluginManager::validateDescriptor(const IRMetricPluginV1 &desc,
                                          std::string *errOut)
{
    if (desc.header.struct_size != sizeof(IRPluginHeader)) {
        setError(errOut, "plugin header struct_size mismatch");
        return IR_UNSUPPORTED;
    }
    if (desc.header.abi_version != IR_PLUGIN_ABI_VERSION_1) {
        setError(errOut, "unsupported plugin abi_version: " +
                         std::to_string(desc.header.abi_version));
        return IR_UNSUPPORTED;
    }
    if (!isNonEmptyCString(desc.header.plugin_id) ||
        !isNonEmptyCString(desc.header.plugin_version) ||
        !isNonEmptyCString(desc.method_name)) {
        setError(errOut, "plugin_id, plugin_version and method_name must be non-empty");
        return IR_BAD_ARGUMENT;
    }
    if (!isValidPluginMethodName(desc.method_name)) {
        setError(errOut, std::string("plugin method_name must match plugin:<ns>.<name>: ") + desc.method_name);
        return IR_BAD_ARGUMENT;
    }
    if (desc.method_version < 1 || desc.method_version > 1000) {
        setError(errOut, "plugin method_version must be within [1, 1000]");
        return IR_BAD_ARGUMENT;
    }
    if (desc.reserved != 0) {
        setError(errOut, "plugin reserved field must be 0");
        return IR_BAD_ARGUMENT;
    }
    if (desc.prepare == nullptr || desc.execute == nullptr) {
        setError(errOut, "plugin prepare and execute callbacks must be non-null");
        return IR_NULL_POINTER;
    }
    return IR_OK;
}

int32_t PluginManager::loadLibrary(const std::filesystem::path &libraryPath,
                                   std::string *errOut)
{
    std::error_code ec;
    std::filesystem::path targetPath = std::filesystem::weakly_canonical(libraryPath, ec);
    if (ec || !targetPath.is_absolute()) {
        std::error_code absEc;
        targetPath = std::filesystem::absolute(libraryPath, absEc);
        if (absEc || !targetPath.is_absolute()) {
            setError(errOut, "cannot resolve absolute plugin library path: " + pathToUtf8(libraryPath));
            return IR_IO_ERROR;
        }
    }
    const std::string targetUtf8 = pathToUtf8(targetPath);

    if (!std::filesystem::exists(targetPath, ec) ||
        !std::filesystem::is_regular_file(targetPath, ec)) {
        setError(errOut, "plugin library not found: " + targetUtf8);
        return IR_IO_ERROR;
    }

    const uint64_t fileSize = std::filesystem::file_size(targetPath, ec);
    if (ec) {
        setError(errOut, "cannot query plugin file size: " + targetUtf8);
        return IR_IO_ERROR;
    }

    std::string sha256;
    if (!computeFileSha256(targetPath, sha256, errOut))
        return IR_IO_ERROR;

    auto handle = std::make_shared<LibraryHandle>();
    handle->utf8Path = targetUtf8;

#if defined(_WIN32)
    const DWORD flags =
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
    handle->native = LoadLibraryExW(targetPath.c_str(), nullptr, flags);
    if (handle->native == nullptr) {
        setError(errOut, "LoadLibraryExW failed for '" + targetUtf8 + "': " +
                         formatWin32Error(GetLastError()));
        return IR_IO_ERROR;
    }
    FARPROC rawSym = GetProcAddress(handle->native, IR_PLUGIN_QUERY_V1_SYMBOL);
    if (rawSym == nullptr) {
        setError(errOut, "missing exported symbol '" IR_PLUGIN_QUERY_V1_SYMBOL "' in " + targetUtf8);
        return IR_UNSUPPORTED;
    }
#else
    dlerror();
    handle->native = dlopen(targetUtf8.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle->native == nullptr) {
        const char *dlErr = dlerror();
        setError(errOut, std::string("dlopen failed for '") + targetUtf8 + "': " +
                         (dlErr ? dlErr : "unknown"));
        return IR_IO_ERROR;
    }
    dlerror();
    void *rawSym = dlsym(handle->native, IR_PLUGIN_QUERY_V1_SYMBOL);
    const char *symErr = dlerror();
    if (symErr != nullptr || rawSym == nullptr) {
        setError(errOut, "missing exported symbol '" IR_PLUGIN_QUERY_V1_SYMBOL "' in " + targetUtf8);
        return IR_UNSUPPORTED;
    }
#endif

    IRPluginQueryV1Fn queryFn = nullptr;
    static_assert(sizeof(queryFn) == sizeof(rawSym), "function pointer size mismatch");
    std::memcpy(&queryFn, &rawSym, sizeof(queryFn));

    const IRMetricPluginV1 *pluginList = nullptr;
    uint32_t pluginCount = 0;
    int32_t st = IR_COMPUTATION_FAILED;
    try {
        st = queryFn(&pluginList, &pluginCount);
    } catch (...) {
        setError(errOut, "C++ exception escaped " IR_PLUGIN_QUERY_V1_SYMBOL);
        return IR_COMPUTATION_FAILED;
    }
    if (st != IR_OK || pluginList == nullptr || pluginCount == 0) {
        setError(errOut, "plugin query failed or exported zero descriptors: " + targetUtf8);
        return st != IR_OK ? st : IR_BAD_ARGUMENT;
    }
    if (pluginCount > 1024U) {
        setError(errOut, "plugin descriptor count exceeds limit (1024): " + targetUtf8);
        return IR_BAD_ARGUMENT;
    }

    const std::string fileUtf8 = pathToUtf8(targetPath.filename());
    std::vector<LoadedMetricPlugin> staged;
    staged.reserve(pluginCount);
    for (uint32_t i = 0; i < pluginCount; ++i) {
        const IRMetricPluginV1 &desc = pluginList[i];
        std::string descErr;
        const int32_t vst = validateDescriptor(desc, &descErr);
        if (vst != IR_OK) {
            setError(errOut, "invalid plugin descriptor in " + fileUtf8 + ": " + descErr);
            return vst;
        }
        LoadedMetricPlugin entry;
        entry.library = handle;
        entry.descriptor = &desc;
        entry.pluginId = desc.header.plugin_id;
        entry.pluginVersion = desc.header.plugin_version;
        entry.methodName = desc.method_name;
        entry.methodVersion = desc.method_version;
        entry.pluginFile = fileUtf8;
        entry.pluginSha256 = sha256;
        entry.fileSizeBytes = fileSize;
        staged.push_back(std::move(entry));
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    for (const auto &entry : staged) {
        if (metricsByMethod_.find(entry.methodName) != metricsByMethod_.end()) {
            setError(errOut, "duplicate plugin method_name: " + entry.methodName);
            return IR_BUSY;
        }
    }
    for (size_t i = 0; i < staged.size(); ++i) {
        for (size_t j = i + 1; j < staged.size(); ++j) {
            if (staged[i].methodName == staged[j].methodName) {
                setError(errOut, "duplicate method_name inside library: " + staged[i].methodName);
                return IR_BAD_ARGUMENT;
            }
        }
    }

    for (auto &entry : staged) {
        std::string key = entry.methodName;
        metricsByMethod_.emplace(std::move(key), std::move(entry));
    }
    libraries_.push_back(std::move(handle));
    return IR_OK;
}

int32_t PluginManager::loadFromDirectory(const std::filesystem::path &pluginDir,
                                         std::string *errOut)
{
    std::error_code ec;
    const std::string dirUtf8 = pathToUtf8(pluginDir);
    if (!std::filesystem::exists(pluginDir, ec) ||
        !std::filesystem::is_directory(pluginDir, ec)) {
        setError(errOut, "plugin directory not found: " + dirUtf8);
        return IR_IO_ERROR;
    }
    std::vector<std::filesystem::path> candidates;
    std::filesystem::directory_iterator it(pluginDir, ec);
    if (ec) {
        setError(errOut, "failed opening plugin directory '" + dirUtf8 + "': " + ec.message());
        return IR_IO_ERROR;
    }
    for (std::filesystem::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            setError(errOut, "failed iterating plugin directory '" + dirUtf8 + "': " + ec.message());
            return IR_IO_ERROR;
        }
        std::error_code entryEc;
        if (it->is_regular_file(entryEc) && hasPlatformLibraryExtension(it->path()))
            candidates.push_back(it->path());
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
        return pathToUtf8(a) < pathToUtf8(b);
    });
    for (const auto &libPath : candidates) {
        const int32_t st = loadLibrary(libPath, errOut);
        if (st != IR_OK)
            return st;
    }
    return IR_OK;
}

int32_t PluginManager::loadConfiguredPaths(const std::vector<std::string> &utf8Paths,
                                           std::string *errOut)
{
    for (const auto &raw : utf8Paths) {
        const std::filesystem::path p = pathFromUtf8(raw);
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec)) {
            const int32_t st = loadFromDirectory(p, errOut);
            if (st != IR_OK)
                return st;
        } else {
            const int32_t st = loadLibrary(p, errOut);
            if (st != IR_OK)
                return st;
        }
    }
    return IR_OK;
}

std::optional<LoadedMetricPlugin> PluginManager::findMetric(std::string_view methodName) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto it = metricsByMethod_.find(std::string(methodName));
    if (it == metricsByMethod_.end())
        return std::nullopt;
    return it->second;
}

std::vector<LoadedMetricPlugin> PluginManager::listMetrics() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<LoadedMetricPlugin> out;
    out.reserve(metricsByMethod_.size());
    for (const auto &[_, entry] : metricsByMethod_)
        out.push_back(entry);
    std::sort(out.begin(), out.end(), [](const LoadedMetricPlugin &a, const LoadedMetricPlugin &b) {
        return a.methodName < b.methodName;
    });
    return out;
}

void PluginManager::unloadAll()
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    metricsByMethod_.clear();
    libraries_.clear();
}

} // namespace ir::plugins
