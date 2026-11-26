#include "station/acquisition/frame_source.h"
#include "api_common.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cwctype>
#include <cstring>
#include <deque>
#include <memory>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ir::acquisition {
namespace {
constexpr size_t DirectShowQueueCapacity = 64;
using Microsoft::WRL::ComPtr;
using ir::api::Error;
using ir::api::require;

constexpr CLSID SampleGrabberClassId =
    {0xc1f400a0, 0x3f08, 0x11d3, {0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37}};
constexpr CLSID NullRendererClassId =
    {0xc1f400a4, 0x3f08, 0x11d3, {0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37}};
constexpr IID SampleGrabberCallbackId =
    {0x0579154a, 0x2b53, 0x4994, {0xb0, 0xd0, 0xe7, 0x73, 0x14, 0x8e, 0xff, 0x85}};
constexpr IID SampleGrabberInterfaceId =
    {0x6b652fff, 0x11fe, 0x4fce, {0x92, 0xad, 0x02, 0x66, 0xb5, 0xd7, 0xc7, 0x8f}};

struct ISampleGrabberCallback : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double time, IMediaSample *sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double time, BYTE *buffer, long length) = 0;
};

struct ISampleGrabber : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL value) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE *type) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE *type) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long *size, long *buffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample **sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCallback *callback, long method) = 0;
};

std::string hresultText(HRESULT result)
{
    wchar_t systemMessage[512]{};
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(result), 0, systemMessage, 512, nullptr);
    std::ostringstream text;
    text << "HRESULT=0x" << std::hex << std::uppercase
         << static_cast<unsigned long>(result);
    if (length != 0) {
        char utf8[sizeof(systemMessage) * 2]{};
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, systemMessage, static_cast<int>(length),
                                             utf8, sizeof utf8, nullptr, nullptr);
        if (bytes > 0)
            text << ": " << std::string_view(utf8, static_cast<size_t>(bytes));
    }
    return text.str();
}

void checkHr(HRESULT result, const char *operation)
{
    if (FAILED(result))
        throw Error(IR_IO_ERROR, std::string(operation) + ": " + hresultText(result));
}

void freeMediaType(AM_MEDIA_TYPE &type) noexcept
{
    if (type.cbFormat) CoTaskMemFree(type.pbFormat);
    if (type.pUnk) type.pUnk->Release();
    type = {};
}

struct OwnedMediaType {
    AM_MEDIA_TYPE type{};
    ~OwnedMediaType() { freeMediaType(type); }
};

void deleteMediaType(AM_MEDIA_TYPE *type) noexcept
{
    if (!type) return;
    freeMediaType(*type);
    CoTaskMemFree(type);
}

std::wstring lower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

std::wstring fromUtf8(const char *value)
{
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, nullptr, 0);
    require(length > 0, IR_BAD_ARGUMENT, "Camera name must be valid UTF-8");
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, result.data(), length);
    result.pop_back();
    return result;
}

std::wstring propertyText(IPropertyBag *bag, const wchar_t *name)
{
    VARIANT value;
    VariantInit(&value);
    std::wstring result;
    if (SUCCEEDED(bag->Read(name, &value, nullptr)) && value.vt == VT_BSTR)
        result = value.bstrVal;
    VariantClear(&value);
    return result;
}

struct FormatInfo {
    LONG width{};
    LONG height{};
    WORD bitCount{};
    size_t rowStride{};
};

bool readFormat(const AM_MEDIA_TYPE &type, FormatInfo &format)
{
    if (!type.pbFormat) return false;
    const BITMAPINFOHEADER *bitmap = nullptr;
    if (type.formattype == FORMAT_VideoInfo && type.cbFormat >= sizeof(VIDEOINFOHEADER)) {
        bitmap = &reinterpret_cast<const VIDEOINFOHEADER *>(type.pbFormat)->bmiHeader;
    } else if (type.formattype == FORMAT_VideoInfo2 && type.cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        bitmap = &reinterpret_cast<const VIDEOINFOHEADER2 *>(type.pbFormat)->bmiHeader;
    } else {
        return false;
    }
    format.width = bitmap->biWidth;
    const auto height = bitmap->biHeight < 0 ? -int64_t(bitmap->biHeight) : int64_t(bitmap->biHeight);
    if (height > std::numeric_limits<LONG>::max()) return false;
    format.height = LONG(height);
    format.bitCount = bitmap->biBitCount;
    if (format.width <= 0 || format.height <= 0 || format.bitCount == 0) return false;
    const auto rowBits = uint64_t(format.width) * format.bitCount;
    const auto rowBytes = ((rowBits + 31) / 32) * 4;
    if (rowBytes > std::numeric_limits<size_t>::max()) return false;
    format.rowStride = size_t(rowBytes);
    return true;
}

bool isRaw16Subtype(const GUID &subtype)
{
    return IsEqualGUID(subtype, MEDIASUBTYPE_RGB565) || IsEqualGUID(subtype, MEDIASUBTYPE_YUY2);
}

struct QueuedFrame {
    std::vector<BYTE> bytes;
    uint64_t sequence{};
    int64_t timestampNs{};
    bool hasTimestamp{};
};

class SampleCallback final : public ISampleGrabberCallback {
public:
    STDMETHODIMP QueryInterface(REFIID id, void **object) override
    {
        if (!object) return E_POINTER;
        if (IsEqualIID(id, IID_IUnknown) || IsEqualIID(id, SampleGrabberCallbackId)) {
            *object = static_cast<ISampleGrabberCallback *>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }

    STDMETHODIMP_(ULONG) Release() override
    {
        const auto count = --references_;
        if (count == 0) delete this;
        return count;
    }

    STDMETHODIMP SampleCB(double, IMediaSample *) override { return E_NOTIMPL; }

    STDMETHODIMP BufferCB(double sampleTime, BYTE *buffer, long length) override
    {
        if (!buffer || length <= 0) return S_OK;
        try {
            uint64_t generation = 0;
            {
                std::lock_guard lock(mutex_);
                if (interrupted_) return S_OK;
                generation = generation_;
            }
            QueuedFrame frame;
            frame.bytes.assign(buffer, buffer + length);
            if (std::isfinite(sampleTime) && sampleTime >= 0) {
                frame.timestampNs = static_cast<int64_t>(sampleTime * 1000000000.0);
                frame.hasTimestamp = true;
            }
            {
                std::lock_guard lock(mutex_);
                if (interrupted_ || generation != generation_) return S_OK;
                frame.sequence = ++sequence_;
                if (frames_.size() == DirectShowQueueCapacity) frames_.pop_front();
                frames_.push_back(std::move(frame));
            }
            ready_.notify_one();
            return S_OK;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }

    void reset()
    {
        std::lock_guard lock(mutex_);
        ++generation_;
        frames_.clear();
        sequence_ = 0;
        interrupted_ = false;
    }

    void clearQueue()
    {
        std::lock_guard lock(mutex_);
        ++generation_;
        frames_.clear();
    }

    void wake()
    {
        {
            std::lock_guard lock(mutex_);
            interrupted_ = true;
        }
        ready_.notify_all();
    }

    bool next(QueuedFrame &frame, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        if (!ready_.wait_for(lock, timeout, [&] { return interrupted_ || !frames_.empty(); }) ||
            interrupted_ || frames_.empty()) return false;

        frame = std::move(frames_.front());
        frames_.pop_front();
        return true;
    }

private:
    std::atomic<ULONG> references_{1};
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<QueuedFrame> frames_;
    uint64_t sequence_{};
    uint64_t generation_{};
    bool interrupted_{};
};
}

class DirectShowSource final : public FrameSource {
public:
    DirectShowSource(const char *cameraName, int validBits) : validBits_(validBits)
    {
        try {
            open(cameraName);
        } catch (...) {
            close();
            throw;
        }
    }

    ~DirectShowSource() override { close(); }

    IRGeometry geometry() const override { return geometry_; }
    uint64_t bufferedBytes() const noexcept override
    {
        return static_cast<uint64_t>(rowStride_) * static_cast<uint64_t>(geometry_.height) *
               DirectShowQueueCapacity;
    }

    void start() override
    {
        require(!running_, IR_BUSY, "DirectShow capture is already running");
        callback_->reset();
        previousSequence_ = 0;
        hasPreviousSequence_ = false;
        checkHr(control_->Run(), "Start DirectShow capture");
        running_ = true;
    }

    void read(void *pixels, IRFrameStamp &stamp, int timeoutMs) override
    {
        require(running_, IR_BAD_ARGUMENT, "Start DirectShow capture before reading frames");
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs);
        const size_t rowBytes = size_t(geometry_.width) * sizeof(uint16_t);
        const size_t frameBytes = rowStride_ * size_t(geometry_.height);
        bool receivedFrame = false;

        for (;;) {
            if (receivedFrame && std::chrono::steady_clock::now() >= deadline)
                throw Error(IR_TIMEOUT, "DirectShow frame wait timed out");
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            QueuedFrame frame;
            if (!callback_->next(frame, std::chrono::milliseconds(std::max<int64_t>(0, remaining))))
                throw Error(IR_TIMEOUT, "DirectShow frame wait timed out");
            receivedFrame = true;
            if (frame.bytes.size() < frameBytes) continue;

            const auto *source = frame.bytes.data();
            auto *destination = reinterpret_cast<BYTE *>(pixels);
            for (LONG row = 0; row < geometry_.height; ++row)
                std::memcpy(destination + size_t(row) * rowBytes,
                            source + size_t(row) * rowStride_, rowBytes);
            stamp = {};
            stamp.sequence = frame.sequence;
            if (hasPreviousSequence_ && frame.sequence > previousSequence_ + 1) {
                const auto missing = frame.sequence - previousSequence_ - 1;
                stamp.lost_frames = missing > UINT32_MAX ? UINT32_MAX : uint32_t(missing);
            }
            stamp.fields = IR_HAS_SEQUENCE | IR_SEQUENCE_IS_CALLBACK | IR_HAS_LOSS;
            if (frame.hasTimestamp) {
                stamp.timestamp_ns = frame.timestampNs;
                stamp.time_origin = IR_TIME_SOURCE;
            }
            previousSequence_ = frame.sequence;
            hasPreviousSequence_ = true;
            return;
        }
    }

    void flush() override
    {
        require(running_, IR_BAD_ARGUMENT, "Start DirectShow capture before flushing frames");
        callback_->clearQueue();
        previousSequence_ = 0;
        hasPreviousSequence_ = false;
    }

    void stop() override
    {
        if (!running_) return;
        const HRESULT result = control_->Stop();
        running_ = false;
        callback_->wake();
        checkHr(result, "Stop DirectShow capture");
    }

private:
    void open(const char *cameraName)
    {
        const HRESULT initResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(initResult)) comInitialized_ = true;
        else if (initResult != RPC_E_CHANGED_MODE)
            checkHr(initResult, "Initialize DirectShow COM");

        ComPtr<ICreateDevEnum> deviceEnumerator;
        checkHr(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&deviceEnumerator)), "Create DirectShow device enumerator");
        ComPtr<IEnumMoniker> devices;
        const HRESULT enumerateResult = deviceEnumerator->CreateClassEnumerator(
            CLSID_VideoInputDeviceCategory, &devices, 0);
        require(enumerateResult == S_OK && devices, IR_IO_ERROR,
                "No DirectShow video capture devices are available");

        const auto wanted = lower(fromUtf8(cameraName));
        HRESULT bindResult = E_FAIL;
        ComPtr<IMoniker> moniker;
        while (devices->Next(1, moniker.GetAddressOf(), nullptr) == S_OK) {
            ComPtr<IPropertyBag> bag;
            if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag)))) {
                const auto friendlyName = lower(propertyText(bag.Get(), L"FriendlyName"));
                const auto devicePath = lower(propertyText(bag.Get(), L"DevicePath"));
                if (friendlyName.find(wanted) != std::wstring::npos ||
                    devicePath.find(wanted) != std::wstring::npos) {
                    bindResult = moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&camera_));
                    if (SUCCEEDED(bindResult)) break;
                }
            }
            moniker.Reset();
        }
        if (!camera_)
            throw Error(IR_IO_ERROR, "DirectShow camera not found: " + std::string(cameraName));

        checkHr(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&graph_)), "Create DirectShow filter graph");
        checkHr(CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&builder_)), "Create DirectShow capture graph builder");
        checkHr(builder_->SetFiltergraph(graph_.Get()), "Connect DirectShow graph builder");
        checkHr(graph_->AddFilter(camera_.Get(), L"FX3 camera"), "Add DirectShow camera filter");

        ComPtr<IAMStreamConfig> streamConfig;
        checkHr(builder_->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, camera_.Get(),
                                        IID_PPV_ARGS(&streamConfig)),
                "Find DirectShow capture stream configuration");
        configureFormat(streamConfig.Get());

        checkHr(CoCreateInstance(SampleGrabberClassId, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&grabberFilter_)), "Create DirectShow sample grabber");
        checkHr(grabberFilter_->QueryInterface(SampleGrabberInterfaceId,
                                                reinterpret_cast<void **>(grabber_.GetAddressOf())),
                "Query DirectShow sample grabber interface");
        checkHr(CoCreateInstance(NullRendererClassId, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&nullRenderer_)), "Create DirectShow null renderer");
        checkHr(graph_->AddFilter(grabberFilter_.Get(), L"Sample Grabber"),
                "Add DirectShow sample grabber");
        checkHr(graph_->AddFilter(nullRenderer_.Get(), L"Null Renderer"),
                "Add DirectShow null renderer");

        AM_MEDIA_TYPE accepted{};
        accepted.majortype = MEDIATYPE_Video;
        accepted.subtype = selectedSubtype_;
        accepted.formattype = GUID_NULL;
        checkHr(grabber_->SetMediaType(&accepted), "Set DirectShow sample grabber media type");
        checkHr(grabber_->SetOneShot(FALSE), "Disable DirectShow one-shot capture");
        checkHr(grabber_->SetBufferSamples(FALSE), "Disable DirectShow sample copy buffer");

        callback_ = new SampleCallback;
        checkHr(grabber_->SetCallback(callback_, 1), "Set DirectShow sample callback");
        checkHr(builder_->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, camera_.Get(),
                                       grabberFilter_.Get(), nullRenderer_.Get()),
                "Connect DirectShow capture graph");
        OwnedMediaType connectedType;
        checkHr(grabber_->GetConnectedMediaType(&connectedType.type),
                "Read DirectShow connected sample format");
        FormatInfo connectedFormat{};
        require(connectedType.type.majortype == MEDIATYPE_Video &&
                IsEqualGUID(connectedType.type.subtype, selectedSubtype_) &&
                readFormat(connectedType.type, connectedFormat) && connectedFormat.bitCount == 16 &&
                connectedFormat.width == width_ && connectedFormat.height == height_,
                IR_UNSUPPORTED, "DirectShow connected format differs from the selected 16-bit raw format");
        rowStride_ = connectedFormat.rowStride;
        checkHr(graph_.As(&control_), "Get DirectShow media control");

        ComPtr<IMediaFilter> mediaFilter;
        if (SUCCEEDED(graph_.As(&mediaFilter))) mediaFilter->SetSyncSource(nullptr);
        geometry_ = {width_, height_, validBits_};
    }

    void configureFormat(IAMStreamConfig *streamConfig)
    {
        int count = 0;
        int capabilitySize = 0;
        checkHr(streamConfig->GetNumberOfCapabilities(&count, &capabilitySize),
                "Read DirectShow video format count");
        require(count > 0 && capabilitySize > 0, IR_UNSUPPORTED,
                "DirectShow camera exposes no video formats");

        std::vector<BYTE> capability(static_cast<size_t>(capabilitySize));
        std::unique_ptr<AM_MEDIA_TYPE, decltype(&deleteMediaType)> selected(nullptr, deleteMediaType);
        int selectedPriority = 2;
        for (int index = 0; index < count; ++index) {
            AM_MEDIA_TYPE *candidate = nullptr;
            if (FAILED(streamConfig->GetStreamCaps(index, &candidate, capability.data())) || !candidate)
                continue;
            std::unique_ptr<AM_MEDIA_TYPE, decltype(&deleteMediaType)> current(candidate, deleteMediaType);
            FormatInfo format{};
            if (candidate->majortype != MEDIATYPE_Video || !readFormat(*candidate, format) ||
                format.bitCount != 16 || !isRaw16Subtype(candidate->subtype)) continue;

            const int priority = IsEqualGUID(candidate->subtype, MEDIASUBTYPE_RGB565) ? 0 : 1;
            if (priority >= selectedPriority) continue;
            width_ = format.width;
            height_ = format.height;
            selectedSubtype_ = candidate->subtype;
            selected = std::move(current);
            selectedPriority = priority;
            if (selectedPriority == 0) break;
        }
        require(bool(selected), IR_UNSUPPORTED,
                "DirectShow camera does not expose a 16-bit RGB565 or YUY2 raw transport format");
        checkHr(streamConfig->SetFormat(selected.get()), "Select DirectShow 16-bit capture format");

        AM_MEDIA_TYPE *actualType = nullptr;
        checkHr(streamConfig->GetFormat(&actualType), "Read selected DirectShow capture format");
        std::unique_ptr<AM_MEDIA_TYPE, decltype(&deleteMediaType)> actual(actualType, deleteMediaType);
        FormatInfo actualFormat{};
        require(actualType && actualType->majortype == MEDIATYPE_Video &&
                IsEqualGUID(actualType->subtype, selectedSubtype_) &&
                readFormat(*actualType, actualFormat) &&
                actualFormat.bitCount == 16,
                IR_UNSUPPORTED, "DirectShow did not apply the selected 16-bit raw format");
        width_ = actualFormat.width;
        height_ = actualFormat.height;
    }

    void close() noexcept
    {
        if (control_) control_->Stop();
        running_ = false;
        if (callback_) callback_->wake();
        if (grabber_) grabber_->SetCallback(nullptr, 1);
        if (callback_) {
            callback_->Release();
            callback_ = nullptr;
        }
        control_.Reset();
        nullRenderer_.Reset();
        grabber_.Reset();
        grabberFilter_.Reset();
        camera_.Reset();
        builder_.Reset();
        graph_.Reset();
        if (comInitialized_) {
            CoUninitialize();
            comInitialized_ = false;
        }
    }

    bool comInitialized_{};
    bool running_{};
    LONG width_{};
    LONG height_{};
    int validBits_{};
    uint64_t previousSequence_{};
    bool hasPreviousSequence_{};
    GUID selectedSubtype_{};
    IRGeometry geometry_{};
    size_t rowStride_{};
    ComPtr<IBaseFilter> camera_;
    ComPtr<IGraphBuilder> graph_;
    ComPtr<ICaptureGraphBuilder2> builder_;
    ComPtr<IBaseFilter> grabberFilter_;
    ComPtr<ISampleGrabber> grabber_;
    ComPtr<IBaseFilter> nullRenderer_;
    ComPtr<IMediaControl> control_;
    SampleCallback *callback_{};
};

std::unique_ptr<FrameSource> openDirectShow(const char *cameraName, int validBits)
{
    return std::make_unique<DirectShowSource>(cameraName, validBits);
}
}
