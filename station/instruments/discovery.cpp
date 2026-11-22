#include "IR_device.h"
#include "api_common.h"
#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <dshow.h>
#include <objbase.h>
#include <wrl/client.h>
#else
#include <filesystem>
#include <fstream>
#include <iterator>
#endif

#include <algorithm>
#include <climits>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {
using Json = nlohmann::json;
using namespace ir::api;

void writeJsonBuffer(const std::string &payload, char *jsonOut, int32_t capacity, int32_t *requiredBytes)
{
    require(capacity >= 0, IR_BAD_ARGUMENT, "capacity must be >= 0");
    require(capacity == 0 || jsonOut != nullptr, IR_NULL_POINTER, "json_out is NULL");

    const int64_t req64 = static_cast<int64_t>(payload.size()) + 1;
    require(req64 <= INT32_MAX, IR_COMPUTATION_FAILED, "JSON output exceeds INT32_MAX bytes");
    if (requiredBytes != nullptr)
        *requiredBytes = static_cast<int32_t>(req64);

    if (capacity < req64) {
        if (jsonOut != nullptr && capacity > 0)
            jsonOut[0] = '\0';
        throw Error(IR_BUFFER_TOO_SMALL, "Output JSON buffer is too small");
    }

    if (!payload.empty())
        std::memcpy(jsonOut, payload.data(), payload.size());
    jsonOut[payload.size()] = '\0';
}

#ifdef _WIN32
std::string wideToUtf8(const wchar_t *text, size_t wcharCount)
{
    if (!text || wcharCount == 0)
        return {};
    require(wcharCount <= static_cast<size_t>(INT_MAX), IR_COMPUTATION_FAILED, "Wide string is too long");
    const int neededBytes = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(wcharCount),
                                                nullptr, 0, nullptr, nullptr);
    require(neededBytes > 0, IR_COMPUTATION_FAILED, "Failed to convert UTF-16 string to UTF-8");
    std::string out(static_cast<size_t>(neededBytes), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(wcharCount),
                                            out.data(), neededBytes, nullptr, nullptr);
    require(written == neededBytes, IR_COMPUTATION_FAILED, "Failed to convert UTF-16 string to UTF-8");
    return out;
}

std::string bstrToUtf8(BSTR value)
{
    if (!value)
        return {};
    return wideToUtf8(value, SysStringLen(value));
}

struct RegKeyGuard {
    HKEY key = nullptr;
    ~RegKeyGuard()
    {
        if (key != nullptr)
            RegCloseKey(key);
    }
};

struct ComInitGuard {
    bool shouldUninit = false;
    ComInitGuard()
    {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (hr == S_OK || hr == S_FALSE) {
            shouldUninit = true;
        } else if (hr != RPC_E_CHANGED_MODE) {
            throw Error(IR_IO_ERROR, "CoInitializeEx failed");
        }
    }
    ~ComInitGuard()
    {
        if (shouldUninit)
            CoUninitialize();
    }
};

int parseComNumber(const std::string &port)
{
    if (port.size() <= 3)
        return std::numeric_limits<int>::max();
    if (!((port[0] == 'C' || port[0] == 'c') &&
          (port[1] == 'O' || port[1] == 'o') &&
          (port[2] == 'M' || port[2] == 'm'))) {
        return std::numeric_limits<int>::max();
    }
    int num = 0;
    for (size_t i = 3; i < port.size(); ++i) {
        if (port[i] < '0' || port[i] > '9')
            return std::numeric_limits<int>::max();
        if (num > (std::numeric_limits<int>::max() - 10) / 10)
            return std::numeric_limits<int>::max();
        num = num * 10 + (port[i] - '0');
    }
    return num;
}

std::vector<std::pair<std::string, std::string>> enumerateSerialPortsImpl()
{
    RegKeyGuard guard;
    const LONG openRc = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &guard.key);
    if (openRc == ERROR_FILE_NOT_FOUND || openRc == ERROR_PATH_NOT_FOUND)
        return {};
    require(openRc == ERROR_SUCCESS, IR_IO_ERROR, "Failed to query Windows SERIALCOMM registry key");

    std::vector<std::pair<std::string, std::string>> items;
    DWORD index = 0;
    std::vector<wchar_t> nameBuf(256, L'\0');
    std::vector<BYTE> dataBuf(512, 0);

    for (;;) {
        DWORD nameChars = static_cast<DWORD>(nameBuf.size());
        DWORD type = 0;
        DWORD dataBytes = static_cast<DWORD>(dataBuf.size());
        const LONG rc = RegEnumValueW(guard.key, index, nameBuf.data(), &nameChars,
                                      nullptr, &type, dataBuf.data(), &dataBytes);
        if (rc == ERROR_NO_MORE_ITEMS)
            break;
        if (rc == ERROR_MORE_DATA) {
            if (nameChars + 1 > nameBuf.size())
                nameBuf.resize(static_cast<size_t>(nameChars) + 2, L'\0');
            else
                nameBuf.resize(nameBuf.size() * 2, L'\0');
            if (dataBytes > dataBuf.size())
                dataBuf.resize(static_cast<size_t>(dataBytes) + sizeof(wchar_t), 0);
            continue;
        }
        require(rc == ERROR_SUCCESS, IR_IO_ERROR, "Failed enumerating Windows serial ports");
        ++index;

        if (type != REG_SZ && type != REG_EXPAND_SZ)
            continue;

        const auto *wData = reinterpret_cast<const wchar_t *>(dataBuf.data());
        size_t wLen = dataBytes / sizeof(wchar_t);
        while (wLen > 0 && wData[wLen - 1] == L'\0')
            --wLen;

        std::string portName = wideToUtf8(wData, wLen);
        std::string deviceName = wideToUtf8(nameBuf.data(), nameChars);
        if (!portName.empty())
            items.emplace_back(std::move(portName), std::move(deviceName));
    }

    std::sort(items.begin(), items.end(), [](const auto &a, const auto &b) {
        const int na = parseComNumber(a.first);
        const int nb = parseComNumber(b.first);
        if (na != nb)
            return na < nb;
        return a.first < b.first;
    });
    return items;
}

std::string readBagText(IPropertyBag *bag, const wchar_t *propName)
{
    VARIANT var;
    VariantInit(&var);
    const HRESULT hr = bag->Read(propName, &var, nullptr);
    std::string result;
    if (SUCCEEDED(hr) && var.vt == VT_BSTR)
        result = bstrToUtf8(var.bstrVal);
    VariantClear(&var);
    return result;
}

std::vector<std::pair<std::string, std::string>> enumerateDirectShowDevicesImpl()
{
    using Microsoft::WRL::ComPtr;
    ComInitGuard com;

    ComPtr<ICreateDevEnum> devEnum;
    const HRESULT createHr = CoCreateInstance(
        CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devEnum));
    require(SUCCEEDED(createHr) && devEnum, IR_IO_ERROR, "Failed to create DirectShow device enumerator");

    ComPtr<IEnumMoniker> enumMoniker;
    const HRESULT enumHr = devEnum->CreateClassEnumerator(
        CLSID_VideoInputDeviceCategory, &enumMoniker, 0);
    if (enumHr == S_FALSE || !enumMoniker)
        return {};
    require(SUCCEEDED(enumHr), IR_IO_ERROR, "Failed to enumerate DirectShow video input devices");

    std::vector<std::pair<std::string, std::string>> devices;
    ComPtr<IMoniker> moniker;
    while (enumMoniker->Next(1, &moniker, nullptr) == S_OK) {
        ComPtr<IPropertyBag> bag;
        const HRESULT bagHr = moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag));
        if (SUCCEEDED(bagHr) && bag) {
            std::string friendly = readBagText(bag.Get(), L"FriendlyName");
            std::string devPath = readBagText(bag.Get(), L"DevicePath");
            if (friendly.empty())
                friendly = devPath;
            devices.emplace_back(std::move(friendly), std::move(devPath));
        }
        moniker.Reset();
    }
    return devices;
}
#else
bool startsWithAny(const std::string &name, std::initializer_list<const char *> prefixes)
{
    for (const char *prefix : prefixes) {
        const size_t len = std::strlen(prefix);
        if (name.compare(0, len, prefix) == 0)
            return true;
    }
    return false;
}

std::vector<std::pair<std::string, std::string>> enumerateSerialPortsImpl()
{
    namespace fs = std::filesystem;
    std::vector<std::pair<std::string, std::string>> items;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator("/dev", ec)) {
        const std::string name = entry.path().filename().string();
        if (startsWithAny(name, {"ttyUSB", "ttyACM", "ttyS", "ttyAMA", "rfcomm"}))
            items.emplace_back("/dev/" + name, name);
    }
    std::sort(items.begin(), items.end(), [](const auto &a, const auto &b) {
        return a.first < b.first;
    });
    return items;
}

std::vector<std::pair<std::string, std::string>> enumerateV4l2DevicesImpl()
{
    namespace fs = std::filesystem;
    std::vector<std::pair<int, std::pair<std::string, std::string>>> indexed;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator("/dev", ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("video", 0) != 0 || name.size() <= 5)
            continue;
        bool allDigits = true;
        for (size_t i = 5; i < name.size(); ++i) {
            if (name[i] < '0' || name[i] > '9') {
                allDigits = false;
                break;
            }
        }
        if (!allDigits)
            continue;
        int num = std::stoi(name.substr(5));
        std::string friendly = name;
        const fs::path sysName = fs::path("/sys/class/video4linux") / name / "name";
        std::ifstream f(sysName);
        if (f) {
            std::string line;
            std::getline(f, line);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            if (!line.empty())
                friendly = line;
        }
        indexed.push_back({num, {std::move(friendly), "/dev/" + name}});
    }
    std::sort(indexed.begin(), indexed.end(), [](const auto &a, const auto &b) {
        if (a.first != b.first)
            return a.first < b.first;
        return a.second.second < b.second.second;
    });
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(indexed.size());
    for (auto &item : indexed)
        out.push_back(std::move(item.second));
    return out;
}
#endif

} // namespace

extern "C" int32_t IR_ListSerialPorts(char *json_out, int32_t capacity,
    int32_t *required_bytes, char *err, int32_t err_bytes)
{
    if (required_bytes)
        *required_bytes = 0;
    return guarded(err, err_bytes, [&] {
        const auto ports = enumerateSerialPortsImpl();
        Json arr = Json::array();
        for (const auto &[port, dev] : ports) {
            arr.push_back({
                {"port", port},
                {"device", dev}
            });
        }
        writeJsonBuffer(arr.dump(), json_out, capacity, required_bytes);
    });
}

extern "C" int32_t IR_ListCaptureDevices(int32_t backend, char *json_out, int32_t capacity,
    int32_t *required_bytes, char *err, int32_t err_bytes)
{
    if (required_bytes)
        *required_bytes = 0;
    return guarded(err, err_bytes, [&] {
        require(backend == IR_DIRECTSHOW || backend == IR_RAW_REPLAY ||
                backend == IR_V4L2 || backend == IR_NI_IMAQ,
                IR_BAD_ARGUMENT, "Unknown capture backend");

#ifdef _WIN32
        require(backend == IR_DIRECTSHOW, IR_UNSUPPORTED,
                "Device enumeration on Windows is only supported for IR_DIRECTSHOW");
        const auto devices = enumerateDirectShowDevicesImpl();
#else
        require(backend == IR_V4L2, IR_UNSUPPORTED,
                "Device enumeration on Linux is only supported for IR_V4L2");
        const auto devices = enumerateV4l2DevicesImpl();
#endif

        Json arr = Json::array();
        for (const auto &[name, devPath] : devices) {
            arr.push_back({
                {"name", name},
                {"device_path", devPath}
            });
        }
        writeJsonBuffer(arr.dump(), json_out, capacity, required_bytes);
    });
}
