#include "IR_device.h"
#include "IR_station.h"
#include <nlohmann/json.hpp>

#include <cstdio>
#include <string>
#include <vector>

using Json = nlohmann::json;

namespace {

int gFailures = 0;

void checkTrue(bool cond, const char *expr, int line)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expr);
        ++gFailures;
    }
}

#define CHECK(cond) checkTrue((cond), #cond, __LINE__)

void testSerialPortDiscovery()
{
    char err[512]{};
    int32_t required = 0;
    CHECK(IR_ListSerialPorts(nullptr, 0, &required, err, sizeof err) == IR_BUFFER_TOO_SMALL);
    CHECK(required >= 3); // at least "[]\0"

    char tiny[2] = {'x', 'y'};
    int32_t req2 = 0;
    CHECK(IR_ListSerialPorts(tiny, 1, &req2, err, sizeof err) == IR_BUFFER_TOO_SMALL);
    CHECK(tiny[0] == '\0');
    CHECK(req2 == required);

    std::vector<char> buf(static_cast<size_t>(required), '\0');
    int32_t reqDone = 0;
    CHECK(IR_ListSerialPorts(buf.data(), required, &reqDone, err, sizeof err) == IR_OK);
    CHECK(reqDone == required);

    const auto arr = Json::parse(buf.data());
    CHECK(arr.is_array());
    for (const auto &item : arr) {
        CHECK(item.is_object());
        CHECK(item.contains("port") && item.at("port").is_string());
        CHECK(!item.at("port").get<std::string>().empty());
        CHECK(item.contains("device") && item.at("device").is_string());
    }
}

void testCaptureDeviceDiscovery()
{
    char err[512]{};
#ifdef _WIN32
    const int32_t okBackend = IR_DIRECTSHOW;
    const int32_t unsupportedBackend = IR_V4L2;
#else
    const int32_t okBackend = IR_V4L2;
    const int32_t unsupportedBackend = IR_DIRECTSHOW;
#endif

    int32_t required = 0;
    CHECK(IR_ListCaptureDevices(okBackend, nullptr, 0, &required, err, sizeof err) == IR_BUFFER_TOO_SMALL);
    CHECK(required >= 3);

    std::vector<char> buf(static_cast<size_t>(required), '\0');
    int32_t reqDone = 0;
    CHECK(IR_ListCaptureDevices(okBackend, buf.data(), required, &reqDone, err, sizeof err) == IR_OK);
    CHECK(reqDone == required);

    const auto arr = Json::parse(buf.data());
    CHECK(arr.is_array());
    for (const auto &item : arr) {
        CHECK(item.is_object());
        CHECK(item.contains("name") && item.at("name").is_string());
        CHECK(item.contains("device_path") && item.at("device_path").is_string());
    }

    CHECK(IR_ListCaptureDevices(IR_NI_IMAQ, buf.data(), required, &reqDone, err, sizeof err) == IR_UNSUPPORTED);
    CHECK(IR_ListCaptureDevices(IR_RAW_REPLAY, buf.data(), required, &reqDone, err, sizeof err) == IR_UNSUPPORTED);
    CHECK(IR_ListCaptureDevices(unsupportedBackend, buf.data(), required, &reqDone, err, sizeof err) == IR_UNSUPPORTED);
    CHECK(IR_ListCaptureDevices(99, buf.data(), required, &reqDone, err, sizeof err) == IR_BAD_ARGUMENT);
    CHECK(IR_ListSerialPorts(nullptr, 4, &reqDone, err, sizeof err) == IR_NULL_POINTER);
    CHECK(IR_ListSerialPorts(buf.data(), -1, &reqDone, err, sizeof err) == IR_BAD_ARGUMENT);
}

} // namespace

int main()
{
    testSerialPortDiscovery();
    testCaptureDeviceDiscovery();
    if (gFailures == 0)
        std::printf("discovery_test: ALL PASSED\n");
    return gFailures == 0 ? 0 : 1;
}
