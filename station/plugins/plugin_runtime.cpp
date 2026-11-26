#include "plugin_runtime.h"
#include "api_common.h"
#include "utf8_path.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace ir::plugins {

struct LibraryHandle {
#if defined(_WIN32)
    HMODULE native{nullptr};
#else
    void *native{nullptr};
#endif
    std::string utf8Path;

    ~LibraryHandle()
    {
        if (native != nullptr) {
#if defined(_WIN32)
            FreeLibrary(native);
#else
            dlclose(native);
#endif
            native = nullptr;
        }
    }
};

namespace {

using ir::utf8::pathFromUtf8;
using ir::utf8::pathToUtf8;

void setError(std::string *errOut, std::string msg)
{
    if (errOut != nullptr)
        *errOut = std::move(msg);
}

bool isNonEmptyCString(const char *s) noexcept
{
    return s != nullptr && s[0] != '\0';
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
    std::string out;
    if (len > 0 && wideBuf != nullptr) {
        const int needed = WideCharToMultiByte(
            CP_UTF8, 0, wideBuf, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
        if (needed > 0) {
            out.resize(static_cast<size_t>(needed));
            WideCharToMultiByte(
                CP_UTF8, 0, wideBuf, static_cast<int>(len), out.data(), needed, nullptr, nullptr);
        }
    }
    if (wideBuf != nullptr)
        LocalFree(wideBuf);
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    if (out.empty())
        out = "Win32 error " + std::to_string(code);
    return out;
}
#endif

struct NativeLibraryCache {
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<LibraryHandle>> handles;
};

NativeLibraryCache &nativeLibraryCache()
{
    // 进程级常驻缓存：避免进程退出时在 Loader Lock 内触发 FreeLibrary 死锁。
    static auto *cache = new NativeLibraryCache();
    return *cache;
}

std::shared_ptr<LibraryHandle> findCachedNativeLibrary(const std::string &targetUtf8)
{
    auto &cache = nativeLibraryCache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (const auto it = cache.handles.find(targetUtf8); it != cache.handles.end())
        return it->second;
    return nullptr;
}

std::shared_ptr<LibraryHandle> openNativeLibrary(
    const std::filesystem::path &targetPath,
    const std::string &targetUtf8,
    std::string *errOut)
{
    auto handle = std::make_shared<LibraryHandle>();
    handle->utf8Path = targetUtf8;
#if defined(_WIN32)
    const DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
    handle->native = LoadLibraryExW(targetPath.c_str(), nullptr, flags);
    if (handle->native == nullptr) {
        setError(errOut, "Failed to load plugin library '" + targetUtf8 + "': " +
                             formatWin32Error(GetLastError()));
        return nullptr;
    }
#else
    dlerror();
    handle->native = dlopen(targetUtf8.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle->native == nullptr) {
        const char *dlErr = dlerror();
        setError(errOut, "Failed to load plugin library '" + targetUtf8 + "': " +
                             (dlErr ? dlErr : "unknown dlopen error"));
        return nullptr;
    }
#endif
    return handle;
}

std::shared_ptr<LibraryHandle> cacheNativeLibrary(
    const std::string &targetUtf8, const std::shared_ptr<LibraryHandle> &handle)
{
    auto &cache = nativeLibraryCache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    return cache.handles.try_emplace(targetUtf8, handle).first->second;
}

bool isPluginFileExtension(const std::filesystem::path &p)
{
    std::string ext = pathToUtf8(p.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
#if defined(_WIN32)
    return ext == ".dll";
#else
    return ext == ".so";
#endif
}

bool isValidIdentifierSegment(std::string_view seg) noexcept
{
    if (seg.empty())
        return false;
    const unsigned char first = static_cast<unsigned char>(seg.front());
    if (!(std::isalpha(first) || first == '_'))
        return false;
    for (unsigned char c : seg) {
        if (!(std::isalnum(c) || c == '_' || c == '-'))
            return false;
    }
    return true;
}

int32_t validateDescriptor(const IRMetricPlugin &desc, std::string *errOut)
{
    if (desc.abi_version != IR_PLUGIN_ABI_VERSION) {
        setError(errOut, "Unsupported plugin ABI version " + std::to_string(desc.abi_version) +
                             " (expected " + std::to_string(IR_PLUGIN_ABI_VERSION) + ")");
        return IR_UNSUPPORTED;
    }
    if (!isNonEmptyCString(desc.plugin_id)) {
        setError(errOut, "Plugin descriptor has empty plugin_id");
        return IR_BAD_ARGUMENT;
    }
    if (!isNonEmptyCString(desc.method_name) ||
        !PluginMetricProvider::isValidPluginMethodName(desc.method_name)) {
        setError(errOut, std::string("Invalid plugin method_name (expected 'plugin:<ns>.<name>'): ") +
                             (desc.method_name ? desc.method_name : "(null)"));
        return IR_BAD_ARGUMENT;
    }
    if (desc.prepare == nullptr || desc.execute == nullptr) {
        setError(errOut, std::string("Plugin descriptor missing prepare/execute callback: ") +
                             desc.method_name);
        return IR_BAD_ARGUMENT;
    }
    return IR_OK;
}

bool toRecipeSlotKind(int32_t abiKind, ir::procedures::recipe::SlotKind &out) noexcept
{
    using SlotKind = ir::procedures::recipe::SlotKind;
    switch (abiKind) {
    case IR_PLUGIN_SLOT_BATCH: out = SlotKind::batch; return true;
    case IR_PLUGIN_SLOT_DOUBLE_MAP: out = SlotKind::double_map; return true;
    case IR_PLUGIN_SLOT_BYTE_MASK: out = SlotKind::byte_mask; return true;
    case IR_PLUGIN_SLOT_DRIFT_CONTEXT: out = SlotKind::drift_context; return true;
    default: return false;
    }
}

int32_t toAbiSlotKind(ir::procedures::recipe::SlotKind kind) noexcept
{
    using SlotKind = ir::procedures::recipe::SlotKind;
    switch (kind) {
    case SlotKind::batch: return IR_PLUGIN_SLOT_BATCH;
    case SlotKind::double_map: return IR_PLUGIN_SLOT_DOUBLE_MAP;
    case SlotKind::byte_mask: return IR_PLUGIN_SLOT_BYTE_MASK;
    case SlotKind::drift_context: return IR_PLUGIN_SLOT_DRIFT_CONTEXT;
    }
    return 0;
}

extern "C" {
static int32_t cbRequireInputSlot(void *, const char *, int32_t, int32_t, int32_t *) noexcept;
static int32_t cbBindOutputSlot(void *, const char *, int32_t, int32_t, int32_t *) noexcept;
static int32_t cbDeclareScalarOutput(void *, const char *, const char *) noexcept;
}

class PrepareCallbackBridge {
public:
    PrepareCallbackBridge(const LoadedMetricPlugin &plugin,
                          const ir::procedures::recipe::Json &inputsJson,
                          const ir::procedures::recipe::Json &outputsJson,
                          std::map<std::string, ir::procedures::recipe::SlotKind> &knownSlots,
                          const std::string &stepPath)
        : plugin_(plugin),
          inputsJson_(inputsJson),
          outputsJson_(outputsJson),
          knownSlots_(knownSlots),
          inputSlots_(knownSlots),
          outputSlots_(knownSlots),
          stepPath_(stepPath)
    {
        vtable_.prepare_ctx = this;
        vtable_.require_input_slot = &cbRequireInputSlot;
        vtable_.bind_output_slot = &cbBindOutputSlot;
        vtable_.declare_scalar_output = &cbDeclareScalarOutput;
    }

    PluginStepBinding prepare(const ir::procedures::recipe::Json &paramsJson)
    {
        const std::string paramsText = paramsJson.dump();
        char errBuf[1024]{};
        int32_t st = IR_COMPUTATION_FAILED;
        try {
            st = plugin_.descriptor->prepare(
                &vtable_, paramsText.c_str(), errBuf, static_cast<int32_t>(sizeof(errBuf)));
        } catch (...) {
            throw ir::api::Error(IR_COMPUTATION_FAILED,
                                 "C++ exception escaped plugin prepare(): " + plugin_.methodName);
        }
        errBuf[sizeof(errBuf) - 1] = '\0';
        if (st != IR_OK || firstCallbackErrorCode_ != IR_OK) {
            const int32_t finalCode = (firstCallbackErrorCode_ != IR_OK) ? firstCallbackErrorCode_ : st;
            std::string msg = errBuf[0] != '\0'
                ? std::string(errBuf)
                : (lastError_[0] != '\0' ? std::string(lastError_)
                                         : ("Plugin prepare() failed: " + plugin_.methodName));
            throw ir::api::Error(finalCode, std::move(msg));
        }

        for (const auto &[key, _] : inputsJson_.items()) {
            ir::api::require(queriedInputs_.count(key) > 0, IR_BAD_ARGUMENT,
                             stepPath_ + ".inputs contains unexpected key '" + key +
                                 "' for plugin " + plugin_.methodName);
        }
        for (const auto &[key, _] : outputsJson_.items()) {
            ir::api::require(queriedOutputs_.count(key) > 0, IR_BAD_ARGUMENT,
                             stepPath_ + ".outputs contains unexpected key '" + key +
                                 "' for plugin " + plugin_.methodName);
        }
        for (const auto &outputBinding : binding_.outputs) {
            const auto &output = outputBinding.second;
            for (const auto &[inputKey, input] : binding_.inputs) {
                ir::api::require(output.first != input.first, IR_BAD_ARGUMENT,
                                 stepPath_ + ".outputs slot '" + output.first +
                                     "' aliases input '" + inputKey + "' in the same plugin step");
            }
        }
        knownSlots_ = std::move(outputSlots_);
        return std::move(binding_);
    }

private:
    friend int32_t cbRequireInputSlot(void *, const char *, int32_t, int32_t, int32_t *) noexcept;
    friend int32_t cbBindOutputSlot(void *, const char *, int32_t, int32_t, int32_t *) noexcept;
    friend int32_t cbDeclareScalarOutput(void *, const char *, const char *) noexcept;

    int32_t recordCallbackError(int32_t code, const std::string &msg) noexcept
    {
        return recordCallbackError(code, msg.c_str());
    }

    int32_t recordCallbackError(int32_t code, const char *msg) noexcept
    {
        if (firstCallbackErrorCode_ == IR_OK) {
            firstCallbackErrorCode_ = code;
            ir::api::copyText(lastError_, sizeof(lastError_), msg ? msg : "Plugin prepare callback failed");
        }
        return code;
    }

    int32_t handleRequireInputSlot(const char *inputKey, int32_t expectedSlotKind,
                                   int32_t required, int32_t *outIsBound) noexcept
    {
        if (outIsBound) *outIsBound = 0;
        if (!isNonEmptyCString(inputKey))
            return recordCallbackError(IR_NULL_POINTER, "require_input_slot: input_key must not be empty");
        ir::procedures::recipe::SlotKind slotKind{};
        if (!toRecipeSlotKind(expectedSlotKind, slotKind))
            return recordCallbackError(IR_BAD_ARGUMENT, "require_input_slot: invalid expected_slot_kind");

        try {
            queriedInputs_.insert(inputKey);
            std::string slotName = (required != 0)
                ? ir::procedures::recipe::requireInputSlot(inputsJson_, inputKey, slotKind, inputSlots_, stepPath_)
                : ir::procedures::recipe::optionalInputSlot(inputsJson_, inputKey, slotKind, inputSlots_, stepPath_);
            if (!slotName.empty()) {
                binding_.inputs.emplace(std::string(inputKey), std::make_pair(std::move(slotName), slotKind));
                if (outIsBound) *outIsBound = 1;
            }
            return IR_OK;
        } catch (const ir::api::Error &e) {
            return recordCallbackError(e.code, e.what());
        } catch (const std::exception &e) {
            return recordCallbackError(IR_BAD_ARGUMENT, e.what());
        } catch (...) {
            return recordCallbackError(IR_COMPUTATION_FAILED, "Unexpected error in require_input_slot");
        }
    }

    int32_t handleBindOutputSlot(const char *outputKey, int32_t slotKind,
                                 int32_t required, int32_t *outIsBound) noexcept
    {
        if (outIsBound) *outIsBound = 0;
        if (!isNonEmptyCString(outputKey))
            return recordCallbackError(IR_NULL_POINTER, "bind_output_slot: output_key must not be empty");
        ir::procedures::recipe::SlotKind kind{};
        if (!toRecipeSlotKind(slotKind, kind))
            return recordCallbackError(IR_BAD_ARGUMENT, "bind_output_slot: invalid slot_kind");
        if (kind != ir::procedures::recipe::SlotKind::double_map &&
            kind != ir::procedures::recipe::SlotKind::byte_mask)
            return recordCallbackError(IR_BAD_ARGUMENT, "bind_output_slot: only double_map and byte_mask are supported");

        try {
            queriedOutputs_.insert(outputKey);
            std::string slotName = ir::procedures::recipe::bindOutputSlot(
                outputsJson_, outputKey, kind, required != 0, outputSlots_, stepPath_);
            if (!slotName.empty()) {
                if (!boundOutputSlotNames_.insert(slotName).second) {
                    return recordCallbackError(
                        IR_BAD_ARGUMENT,
                        stepPath_ + ".outputs binds duplicate slot name '" + slotName + "'");
                }
                binding_.outputs.emplace(std::string(outputKey), std::make_pair(std::move(slotName), kind));
                if (outIsBound) *outIsBound = 1;
            }
            return IR_OK;
        } catch (const ir::api::Error &e) {
            return recordCallbackError(e.code, e.what());
        } catch (const std::exception &e) {
            return recordCallbackError(IR_BAD_ARGUMENT, e.what());
        } catch (...) {
            return recordCallbackError(IR_COMPUTATION_FAILED, "Unexpected error in bind_output_slot");
        }
    }

    int32_t handleDeclareScalarOutput(const char *suffixUtf8, const char *unitUtf8) noexcept
    {
        if (suffixUtf8 == nullptr || unitUtf8 == nullptr)
            return recordCallbackError(IR_NULL_POINTER, "declare_scalar_output: suffix and unit must not be null");
        if (suffixUtf8[0] != '.' || suffixUtf8[1] == '\0' || unitUtf8[0] == '\0')
            return recordCallbackError(IR_BAD_ARGUMENT, "declare_scalar_output: requires '.suffix' and non-empty unit");

        try {
            if (!declaredSuffixes_.insert(suffixUtf8).second) {
                return recordCallbackError(IR_BAD_ARGUMENT,
                                           std::string("Duplicate plugin scalar output suffix: ") + suffixUtf8);
            }
            binding_.scalarOutputs.push_back({
                suffixUtf8,
                unitUtf8,
                plugin_.methodName,
                1,
                ir::procedures::recipe::PluginProvenance{plugin_.pluginId, plugin_.pluginFile}
            });
            return IR_OK;
        } catch (const std::exception &e) {
            return recordCallbackError(IR_COMPUTATION_FAILED, e.what());
        } catch (...) {
            return recordCallbackError(IR_COMPUTATION_FAILED, "Unexpected error in declare_scalar_output");
        }
    }

    const LoadedMetricPlugin &plugin_;
    const ir::procedures::recipe::Json &inputsJson_;
    const ir::procedures::recipe::Json &outputsJson_;
    std::map<std::string, ir::procedures::recipe::SlotKind> &knownSlots_;
    std::map<std::string, ir::procedures::recipe::SlotKind> inputSlots_;
    std::map<std::string, ir::procedures::recipe::SlotKind> outputSlots_;
    const std::string &stepPath_;
    PluginStepBinding binding_;
    std::set<std::string> queriedInputs_;
    std::set<std::string> queriedOutputs_;
    std::set<std::string> boundOutputSlotNames_;
    std::set<std::string> declaredSuffixes_;
    int32_t firstCallbackErrorCode_{IR_OK};
    char lastError_[1024]{};
    IRPrepareCallbacks vtable_{};
};

extern "C" {
static int32_t cbRequireInputSlot(void *ctx, const char *inputKey, int32_t expectedSlotKind,
                                  int32_t required, int32_t *outIsBound) noexcept
{
    if (!ctx) return IR_NULL_POINTER;
    return static_cast<PrepareCallbackBridge *>(ctx)->handleRequireInputSlot(
        inputKey, expectedSlotKind, required, outIsBound);
}

static int32_t cbBindOutputSlot(void *ctx, const char *outputKey, int32_t slotKind,
                                int32_t required, int32_t *outIsBound) noexcept
{
    if (!ctx) return IR_NULL_POINTER;
    return static_cast<PrepareCallbackBridge *>(ctx)->handleBindOutputSlot(
        outputKey, slotKind, required, outIsBound);
}

static int32_t cbDeclareScalarOutput(void *ctx, const char *suffixUtf8, const char *unitUtf8) noexcept
{
    if (!ctx) return IR_NULL_POINTER;
    return static_cast<PrepareCallbackBridge *>(ctx)->handleDeclareScalarOutput(suffixUtf8, unitUtf8);
}

static int32_t cbHasSlot(void *, const char *) noexcept;
static int32_t cbGetSlot(void *, const char *, IRSlotView *) noexcept;
static int32_t cbAllocDoubleMap(void *, const char *, int32_t, int32_t, double **) noexcept;
static int32_t cbAllocByteMask(void *, const char *, int32_t, int32_t, uint8_t **) noexcept;
static int32_t cbEmitScalar(void *, const char *, double) noexcept;
static int32_t cbIsCancelled(void *) noexcept;
static void cbLogMessage(void *, int32_t, const char *) noexcept;
}

class HostCallbackBridge {
public:
    HostCallbackBridge(const LoadedMetricPlugin &plugin,
                       const PluginStepBinding &binding,
                       const std::string &stepName,
                       ir::procedures::recipe::AnalysisRunContext &runCtx)
        : plugin_(plugin),
          binding_(binding),
          stepName_(stepName),
          runCtx_(runCtx)
    {
        vtable_.host_ctx = this;
        vtable_.has_slot = &cbHasSlot;
        vtable_.get_slot = &cbGetSlot;
        vtable_.alloc_double_map = &cbAllocDoubleMap;
        vtable_.alloc_byte_mask = &cbAllocByteMask;
        vtable_.emit_scalar = &cbEmitScalar;
        vtable_.is_cancelled = &cbIsCancelled;
        vtable_.log_message = &cbLogMessage;
    }

    void execute(const std::string &paramsText)
    {
        char errBuf[1024]{};
        int32_t st = IR_COMPUTATION_FAILED;
        try {
            st = plugin_.descriptor->execute(
                &vtable_, runCtx_.ctx.geometry, paramsText.c_str(),
                errBuf, static_cast<int32_t>(sizeof(errBuf)));
        } catch (...) {
            throw ir::api::Error(IR_COMPUTATION_FAILED,
                                 "C++ exception escaped plugin execute(): " + plugin_.methodName);
        }
        errBuf[sizeof(errBuf) - 1] = '\0';
        if (st == IR_CANCELLED || (runCtx_.isStopRequested && runCtx_.isStopRequested())) {
            const std::string msg = errBuf[0] != '\0'
                ? std::string(errBuf)
                : "Measurement or acquisition cancelled";
            throw ir::api::Error(IR_CANCELLED, msg);
        }
        if (firstCallbackErrorCode_ != IR_OK) {
            const std::string msg = lastError_[0] != '\0'
                ? std::string(lastError_)
                : "Plugin ignored a failed host callback";
            throw ir::api::Error(firstCallbackErrorCode_, msg);
        }
        if (st != IR_OK) {
            const std::string msg = errBuf[0] != '\0'
                ? std::string(errBuf)
                : (lastError_[0] != '\0' ? std::string(lastError_)
                                         : ("Plugin execute() failed: " + plugin_.methodName));
            throw ir::api::Error(st, msg);
        }

        for (const auto &[outKey, _] : binding_.outputs) {
            ir::api::require(allocatedOutputs_.count(outKey) > 0, IR_COMPUTATION_FAILED,
                             "Plugin did not allocate bound output slot '" + outKey +
                                 "' in " + plugin_.methodName);
        }

        std::vector<double> orderedScalars;
        orderedScalars.reserve(binding_.scalarOutputs.size());
        for (const auto &outDecl : binding_.scalarOutputs) {
            const auto it = emittedScalars_.find(outDecl.suffix);
            ir::api::require(it != emittedScalars_.end(), IR_COMPUTATION_FAILED,
                             "Plugin did not emit declared scalar output '" +
                                 stepName_ + outDecl.suffix + "'");
            orderedScalars.push_back(it->second);
        }
        ir::procedures::recipe::recordScalarOutputs(
            runCtx_.ctx, stepName_, binding_.scalarOutputs, orderedScalars);
    }

private:
    friend int32_t cbHasSlot(void *, const char *) noexcept;
    friend int32_t cbGetSlot(void *, const char *, IRSlotView *) noexcept;
    friend int32_t cbAllocDoubleMap(void *, const char *, int32_t, int32_t, double **) noexcept;
    friend int32_t cbAllocByteMask(void *, const char *, int32_t, int32_t, uint8_t **) noexcept;
    friend int32_t cbEmitScalar(void *, const char *, double) noexcept;
    friend int32_t cbIsCancelled(void *) noexcept;

    void setLastError(const char *msg) noexcept
    {
        if (lastError_[0] == '\0')
            ir::api::copyText(lastError_, sizeof(lastError_), msg ? msg : "Host callback failed");
    }

    void recordCallbackError(int32_t code) noexcept
    {
        if (firstCallbackErrorCode_ == IR_OK)
            firstCallbackErrorCode_ = code;
        if (lastError_[0] == '\0')
            setLastError("Host callback failed");
    }

    bool cancelled() const noexcept
    {
        return runCtx_.isStopRequested && runCtx_.isStopRequested();
    }

    int32_t handleHasSlot(const char *ioKey) noexcept
    {
        if (!isNonEmptyCString(ioKey))
            return 0;
        try {
            return (binding_.inputs.find(ioKey) != binding_.inputs.end() ||
                    binding_.outputs.find(ioKey) != binding_.outputs.end()) ? 1 : 0;
        } catch (...) {
            setLastError("Unexpected error checking plugin slot");
            recordCallbackError(IR_COMPUTATION_FAILED);
            return 0;
        }
    }

    int32_t handleGetSlot(const char *inputKey, IRSlotView *outView) noexcept
    {
        if (!isNonEmptyCString(inputKey) || outView == nullptr) {
            setLastError("get_slot: input_key and out_view must not be null");
            return IR_NULL_POINTER;
        }
        *outView = IRSlotView{};

        try {
            const auto it = binding_.inputs.find(inputKey);
            if (it == binding_.inputs.end()) {
                setLastError("get_slot: undeclared or unbound input_key");
                return IR_BAD_ARGUMENT;
            }
            const std::string &slotName = it->second.first;
            const auto kind = it->second.second;

            outView->slot_kind = toAbiSlotKind(kind);
            switch (kind) {
            case ir::procedures::recipe::SlotKind::batch: {
                const auto &batch = ir::procedures::recipe::getBatch(runCtx_.ctx, slotName);
                outView->valid_bits = batch.geometry.valid_bits;
                outView->array = ir::procedures::recipe::frameBatchArrayView(batch);
                outView->stamps = batch.stamps.data();
                return IR_OK;
            }
            case ir::procedures::recipe::SlotKind::double_map: {
                const auto &map = ir::procedures::recipe::getMap(runCtx_.ctx, slotName);
                outView->array = IRArrayView{
                    map.data.data(),
                    static_cast<uint64_t>(map.data.size()) * sizeof(double),
                    IR_TYPE_F64,
                    map.geometry.width,
                    map.geometry.height,
                    1
                };
                return IR_OK;
            }
            case ir::procedures::recipe::SlotKind::byte_mask: {
                const auto &mask = ir::procedures::recipe::getMask(runCtx_.ctx, slotName);
                outView->array = IRArrayView{
                    mask.data.data(),
                    static_cast<uint64_t>(mask.data.size()) * sizeof(uint8_t),
                    IR_TYPE_U8,
                    mask.geometry.width,
                    mask.geometry.height,
                    1
                };
                return IR_OK;
            }
            case ir::procedures::recipe::SlotKind::drift_context: {
                const auto &dctx = ir::procedures::recipe::getDriftContext(runCtx_.ctx, slotName);
                outView->opaque_data = &dctx;
                outView->opaque_byte_size = sizeof(IRGjbDriftContext);
                return IR_OK;
            }
            }
        } catch (const ir::api::Error &e) {
            setLastError(e.what());
            return e.code;
        } catch (const std::exception &e) {
            setLastError(e.what());
            return IR_COMPUTATION_FAILED;
        } catch (...) {
            setLastError("Unexpected error in get_slot");
            return IR_COMPUTATION_FAILED;
        }
        return IR_BAD_ARGUMENT;
    }

    template <typename MapT, typename T>
    int32_t allocSlotBuffer(const char *outputKey, ir::procedures::recipe::SlotKind expectedKind,
                            int32_t width, int32_t height, T **outData,
                            std::map<std::string, MapT> &container) noexcept
    {
        if (cancelled()) {
            setLastError("Measurement or acquisition cancelled");
            return IR_CANCELLED;
        }
        if (!isNonEmptyCString(outputKey) || outData == nullptr) {
            setLastError("alloc output: output_key and out_data must not be null");
            return IR_NULL_POINTER;
        }
        *outData = nullptr;
        if (width <= 0 || height <= 0) {
            setLastError("alloc output: width and height must be positive");
            return IR_BAD_ARGUMENT;
        }

        try {
            const auto it = binding_.outputs.find(outputKey);
            if (it == binding_.outputs.end() || it->second.second != expectedKind) {
                setLastError("alloc output: undeclared or incompatible output_key");
                return IR_BAD_ARGUMENT;
            }
            const std::string &slotName = it->second.first;
            if (allocatedOutputs_.count(outputKey) != 0) {
                setLastError("alloc output: output_key was already allocated in this execute()");
                return IR_BAD_ARGUMENT;
            }

            const uint64_t count = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
            if (count > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) / sizeof(T)) {
                setLastError("alloc output: pixel count overflows");
                return IR_BAD_ARGUMENT;
            }
            const size_t elements = static_cast<size_t>(count);
            const auto existingIt = container.find(slotName);
            const bool needsAllocation =
                existingIt == container.end() || elements > existingIt->second.data.capacity();
            ir::procedures::recipe::requireAdditionalBytes(
                runCtx_, needsAllocation ? count * sizeof(T) : 0);

            // A fresh sized vector avoids geometric growth. The old buffer stays live
            // until the new allocation succeeds, so the peak includes both buffers.
            std::vector<T> replacement;
            if (needsAllocation)
                replacement = std::vector<T>(elements);
            auto &slot = container[slotName];
            if (needsAllocation) {
                slot.data = std::move(replacement);
            } else {
                slot.data.resize(elements);
                std::fill(slot.data.begin(), slot.data.end(), T{});
            }
            slot.geometry = IRGeometry{width, height, runCtx_.ctx.geometry.valid_bits};
            allocatedOutputs_.insert(outputKey);
            *outData = slot.data.data();
            return IR_OK;
        } catch (const ir::api::Error &e) {
            setLastError(e.what());
            return e.code;
        } catch (const std::exception &e) {
            setLastError(e.what());
            return IR_COMPUTATION_FAILED;
        } catch (...) {
            setLastError("Unexpected error allocating plugin output");
            return IR_COMPUTATION_FAILED;
        }
    }

    int32_t handleAllocDoubleMap(const char *outputKey, int32_t width, int32_t height,
                                 double **outData) noexcept
    {
        return allocSlotBuffer(outputKey, ir::procedures::recipe::SlotKind::double_map,
                               width, height, outData, runCtx_.ctx.maps);
    }

    int32_t handleAllocByteMask(const char *outputKey, int32_t width, int32_t height,
                                uint8_t **outData) noexcept
    {
        return allocSlotBuffer(outputKey, ir::procedures::recipe::SlotKind::byte_mask,
                               width, height, outData, runCtx_.ctx.masks);
    }

    int32_t handleEmitScalar(const char *suffixUtf8, double value) noexcept
    {
        if (cancelled()) {
            setLastError("Measurement or acquisition cancelled");
            return IR_CANCELLED;
        }
        if (suffixUtf8 == nullptr) {
            setLastError("emit_scalar: suffix must not be null");
            return IR_NULL_POINTER;
        }
        if (!std::isfinite(value)) {
            setLastError("emit_scalar: value must be finite");
            return IR_COMPUTATION_FAILED;
        }
        try {
            const bool declared = std::any_of(
                binding_.scalarOutputs.begin(), binding_.scalarOutputs.end(),
                [suffixUtf8](const ir::procedures::recipe::MetricOutput &o) {
                    return o.suffix == suffixUtf8;
                });
            if (!declared) {
                setLastError("emit_scalar: undeclared scalar output suffix");
                return IR_BAD_ARGUMENT;
            }
            if (!emittedScalars_.emplace(suffixUtf8, value).second) {
                setLastError("emit_scalar: duplicate scalar emission in single execute()");
                return IR_BAD_ARGUMENT;
            }
            return IR_OK;
        } catch (const std::exception &e) {
            setLastError(e.what());
            return IR_COMPUTATION_FAILED;
        } catch (...) {
            setLastError("Unexpected error in emit_scalar");
            return IR_COMPUTATION_FAILED;
        }
    }

    int32_t handleIsCancelled() const noexcept
    {
        return cancelled() ? 1 : 0;
    }

    const LoadedMetricPlugin &plugin_;
    const PluginStepBinding &binding_;
    const std::string &stepName_;
    ir::procedures::recipe::AnalysisRunContext &runCtx_;
    std::set<std::string> allocatedOutputs_;
    std::map<std::string, double, std::less<>> emittedScalars_;
    int32_t firstCallbackErrorCode_{IR_OK};
    char lastError_[1024]{};
    IRHostCallbacks vtable_{};
};

extern "C" {
static int32_t cbHasSlot(void *ctx, const char *ioKey) noexcept
{
    return ctx ? static_cast<HostCallbackBridge *>(ctx)->handleHasSlot(ioKey) : 0;
}

static int32_t cbGetSlot(void *ctx, const char *inputKey, IRSlotView *outView) noexcept
{
    if (!ctx) return IR_NULL_POINTER;
    auto *bridge = static_cast<HostCallbackBridge *>(ctx);
    const int32_t st = bridge->handleGetSlot(inputKey, outView);
    if (st != IR_OK) bridge->recordCallbackError(st);
    return st;
}

static int32_t cbAllocDoubleMap(void *ctx, const char *outputKey,
                                int32_t width, int32_t height, double **outData) noexcept
{
    if (!ctx) return IR_NULL_POINTER;
    auto *bridge = static_cast<HostCallbackBridge *>(ctx);
    const int32_t st = bridge->handleAllocDoubleMap(outputKey, width, height, outData);
    if (st != IR_OK) bridge->recordCallbackError(st);
    return st;
}

static int32_t cbAllocByteMask(void *ctx, const char *outputKey,
                               int32_t width, int32_t height, uint8_t **outData) noexcept
{
    if (!ctx) return IR_NULL_POINTER;
    auto *bridge = static_cast<HostCallbackBridge *>(ctx);
    const int32_t st = bridge->handleAllocByteMask(outputKey, width, height, outData);
    if (st != IR_OK) bridge->recordCallbackError(st);
    return st;
}

static int32_t cbEmitScalar(void *ctx, const char *suffixUtf8, double value) noexcept
{
    if (!ctx) return IR_NULL_POINTER;
    auto *bridge = static_cast<HostCallbackBridge *>(ctx);
    const int32_t st = bridge->handleEmitScalar(suffixUtf8, value);
    if (st != IR_OK) bridge->recordCallbackError(st);
    return st;
}

static int32_t cbIsCancelled(void *ctx) noexcept
{
    return ctx ? static_cast<HostCallbackBridge *>(ctx)->handleIsCancelled() : 0;
}

static void cbLogMessage(void *, int32_t level, const char *messageUtf8) noexcept
{
    if (!messageUtf8 || messageUtf8[0] == '\0')
        return;
    const char *tag = "INFO";
    switch (level) {
    case IR_PLUGIN_LOG_DEBUG: tag = "DEBUG"; break;
    case IR_PLUGIN_LOG_INFO: tag = "INFO"; break;
    case IR_PLUGIN_LOG_WARN: tag = "WARN"; break;
    case IR_PLUGIN_LOG_ERROR: tag = "ERROR"; break;
    default: break;
    }
    try {
        std::fprintf(stderr, "[plugin][%s] %s\n", tag, messageUtf8);
    } catch (...) {
    }
}
}

} // namespace

bool PluginMetricProvider::isValidPluginMethodName(std::string_view methodName) noexcept
{
    constexpr std::string_view prefix = "plugin:";
    if (methodName.size() <= prefix.size() || methodName.substr(0, prefix.size()) != prefix)
        return false;
    const std::string_view rest = methodName.substr(prefix.size());
    const size_t dotPos = rest.find('.');
    if (dotPos == std::string_view::npos || rest.find('.', dotPos + 1) != std::string_view::npos)
        return false;
    return isValidIdentifierSegment(rest.substr(0, dotPos)) &&
           isValidIdentifierSegment(rest.substr(dotPos + 1));
}

int32_t PluginMetricProvider::loadLibrary(const std::filesystem::path &libraryPath,
                                          bool requireQuerySymbol, std::string *errOut)
{
    std::error_code ec;
    const auto absPath = std::filesystem::absolute(libraryPath, ec);
    if (ec) {
        setError(errOut, "Cannot resolve absolute plugin path: " + pathToUtf8(libraryPath));
        return IR_IO_ERROR;
    }
    const auto canonicalPath = std::filesystem::weakly_canonical(absPath, ec);
    const auto &targetPath = ec ? absPath : canonicalPath;
    const std::string targetUtf8 = pathToUtf8(targetPath);

    if (!std::filesystem::is_regular_file(targetPath, ec)) {
        setError(errOut, "Plugin library file not found: " + targetUtf8);
        return IR_IO_ERROR;
    }

    auto lib = findCachedNativeLibrary(targetUtf8);
    const bool fromCache = (lib != nullptr);
    if (!lib) {
        lib = openNativeLibrary(targetPath, targetUtf8, errOut);
        if (!lib)
            return IR_IO_ERROR;
    }

#if defined(_WIN32)
    const auto rawSym = GetProcAddress(lib->native, IR_PLUGIN_QUERY_SYMBOL);
#else
    const auto rawSym = dlsym(lib->native, IR_PLUGIN_QUERY_SYMBOL);
#endif
    if (rawSym == nullptr) {
        if (!requireQuerySymbol) {
            if (!fromCache)
                cacheNativeLibrary(targetUtf8, lib);
            return IR_OK;
        }
        setError(errOut, "Plugin missing symbol '" IR_PLUGIN_QUERY_SYMBOL "': " + targetUtf8);
        return IR_UNSUPPORTED;
    }
    const auto queryFn = reinterpret_cast<IRPluginQueryFn>(rawSym);

    const IRMetricPlugin *list = nullptr;
    uint32_t count = 0;
    int32_t queryStatus = IR_COMPUTATION_FAILED;
    try {
        queryStatus = queryFn(&list, &count);
    } catch (...) {
        setError(errOut, "Unhandled C++ exception inside " IR_PLUGIN_QUERY_SYMBOL " of " + targetUtf8);
        return IR_COMPUTATION_FAILED;
    }
    if (queryStatus != IR_OK) {
        setError(errOut, std::string(IR_PLUGIN_QUERY_SYMBOL) + " failed with status " +
                             std::to_string(queryStatus) + " in " + targetUtf8);
        return queryStatus;
    }
    if (list == nullptr || count == 0) {
        setError(errOut, "Plugin exported zero descriptors: " + targetUtf8);
        return IR_BAD_ARGUMENT;
    }

    std::vector<LoadedMetricPlugin> candidateEntries;
    candidateEntries.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const IRMetricPlugin &desc = list[i];
        std::string descErr;
        const int32_t descStatus = validateDescriptor(desc, &descErr);
        if (descStatus != IR_OK) {
            setError(errOut, targetUtf8 + " [" + std::to_string(i) + "]: " + descErr);
            return descStatus;
        }
        LoadedMetricPlugin entry;
        entry.library = lib;
        entry.descriptor = &desc;
        entry.pluginId = desc.plugin_id;
        entry.methodName = desc.method_name;
        entry.pluginFile = pathToUtf8(targetPath.filename());
        candidateEntries.push_back(std::move(entry));
    }

    std::set<std::string> candidateNames;
    for (const auto &entry : candidateEntries) {
        if (metrics_.find(entry.methodName) != metrics_.end() ||
            !candidateNames.insert(entry.methodName).second) {
            setError(errOut, "Duplicate plugin method name: " + entry.methodName);
            return IR_BAD_ARGUMENT;
        }
    }
    if (!fromCache)
        lib = cacheNativeLibrary(targetUtf8, lib);
    for (auto &entry : candidateEntries) {
        entry.library = lib;
        metrics_.emplace(entry.methodName, std::move(entry));
    }
    return IR_OK;
}

int32_t PluginMetricProvider::loadFromDirectory(const std::filesystem::path &pluginDir,
                                                std::string *errOut)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(pluginDir, ec)) {
        setError(errOut, "Plugin directory not found: " + pathToUtf8(pluginDir));
        return IR_IO_ERROR;
    }

    std::vector<std::filesystem::path> files;
    for (const auto &entry : std::filesystem::directory_iterator(pluginDir, ec)) {
        if (ec) {
            setError(errOut, "Failed iterating plugin directory: " + pathToUtf8(pluginDir));
            return IR_IO_ERROR;
        }
        if (entry.is_regular_file(ec) && isPluginFileExtension(entry.path()))
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());

    for (const auto &file : files) {
        const int32_t st = loadLibrary(file, false, errOut);
        if (st != IR_OK)
            return st;
    }
    return IR_OK;
}

int32_t PluginMetricProvider::loadConfiguredPaths(const std::vector<std::string> &utf8Paths,
                                                  std::string *errOut)
{
    for (const auto &rawPath : utf8Paths) {
        const std::filesystem::path p = pathFromUtf8(rawPath);
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec)) {
            const int32_t st = loadFromDirectory(p, errOut);
            if (st != IR_OK)
                return st;
        } else if (std::filesystem::is_regular_file(p, ec)) {
            const int32_t st = loadLibrary(p, true, errOut);
            if (st != IR_OK)
                return st;
        } else {
            setError(errOut, "Configured plugin path does not exist: " + rawPath);
            return IR_IO_ERROR;
        }
    }
    return IR_OK;
}

std::optional<LoadedMetricPlugin> PluginMetricProvider::findMetric(std::string_view methodName) const
{
    const auto it = metrics_.find(std::string(methodName));
    if (it == metrics_.end())
        return std::nullopt;
    return it->second;
}

bool PluginMetricProvider::handlesMethod(std::string_view methodName) const noexcept
{
    return methodName.rfind("plugin:", 0) == 0;
}

ir::procedures::recipe::AnalysisStep PluginMetricProvider::prepareMetric(
    ir::procedures::recipe::StepRef ref,
    const std::string &stepName,
    const std::string &method,
    int32_t reqVersion,
    const ir::procedures::recipe::Json &fields,
    const std::string &path,
    std::map<std::string, ir::procedures::recipe::SlotKind> &knownSlots) const
{
    using namespace ir::api;
    using namespace ir::procedures::recipe;

    require(isValidPluginMethodName(method), IR_BAD_ARGUMENT,
            path + ".method has invalid plugin name: " + method);
    const auto found = findMetric(method);
    require(found.has_value(), IR_BAD_ARGUMENT,
            path + ".method plugin is not loaded: " + method);
    const auto &plugin = *found;
    require(reqVersion == 1, IR_BAD_ARGUMENT,
            path + ".version=" + std::to_string(reqVersion) + " does not match plugin method version 1");

    const Json normalizedInputs = normalizeInputsObject(
        fields, plugin.descriptor->default_input_key, method, path);
    const Json params = fields.contains("params") ? objectField(fields, "params", path) : Json::object();
    const Json outputs = fields.contains("outputs") ? objectField(fields, "outputs", path) : Json::object();

    PrepareCallbackBridge prepBridge(plugin, normalizedInputs, outputs, knownSlots, path);
    auto binding = std::make_shared<PluginStepBinding>(prepBridge.prepare(params));
    const std::string paramsText = params.dump();

    auto outsCopy = binding->scalarOutputs;
    auto executeFn = [plugin, binding, stepName, paramsText](AnalysisRunContext &runCtx) {
        HostCallbackBridge hostBridge(plugin, *binding, stepName, runCtx);
        hostBridge.execute(paramsText);
    };

    return {
        std::move(ref),
        stepName,
        method,
        1,
        std::move(outsCopy),
        {{}, std::move(executeFn)}
    };
}

} // namespace ir::plugins
