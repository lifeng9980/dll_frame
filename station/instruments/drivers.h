#pragma once

#include "IR_device.h"
#include "IR_power_driver_abi.h"
#include "asio_link.h"
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace ir::instruments {

struct BlackbodyConfig {
    using Endpoint = transport::Endpoint;
    Endpoint endpoint;
    std::chrono::milliseconds timeout{3000};
    std::chrono::milliseconds commandInterval{};
    int head{-1};
    std::string expectedHead;
    // Software setpoint window, overridable with min_setpoint_c / max_setpoint_c.
    // Defaults match the DOC058 controller families: PCN7-LT -60..100, DCN1000 -5..100.
    double minSetpointC{-60.0};
    double maxSetpointC{100.0};
};

class Doc058Blackbody {
public:
    explicit Doc058Blackbody(BlackbodyConfig config);
    IRBlackbodyReading read();
    double set(double target);

private:
    void open();
    template <class Fn> auto withReconnect(Fn read);
    IRBlackbodyReading readOnce();
    std::string request(uint8_t function, std::string argument = {}, bool write = false);
    double number(uint8_t function);
    double numberWithReconnect(uint8_t function);
    bool tcp() const;

    BlackbodyConfig config_;
    std::unique_ptr<transport::AsioLink> link_;
    transport::Deadline nextCommand_{};
    bool reconnectable_{};
};

struct CoolerModbusConfig {
    transport::Endpoint endpoint;
    uint8_t slaveAddress{1};
    uint16_t registerAddress{0x7A52};
    std::chrono::milliseconds timeout{2000};
};

class CoolerModbus {
public:
    explicit CoolerModbus(CoolerModbusConfig config);
    IRCoolerReading read();

private:
    using Clock = std::chrono::steady_clock;
    void connect();
    IRCoolerReading readOnce();
    template <class Fn> auto withReconnect(Fn fn);

    CoolerModbusConfig config_;
    std::unique_ptr<transport::AsioLink> link_;
};

struct DetectorSerialConfig {
    transport::Endpoint endpoint;
    std::chrono::milliseconds timeout{1200};
    std::chrono::milliseconds interCommandDelay{0};
};

class DetectorSerial {
public:
    explicit DetectorSerial(DetectorSerialConfig config);
    void setTiming(int32_t integrationUs, int32_t frameHz, int32_t emptyUs);
    void setGpol(double gpolV);
    void setRegister(uint8_t commandCode, const uint8_t *payload, int32_t payloadBytes);
    IRDetectorCurrentReading measureCurrent(int32_t gainCode, uint16_t channelCode);

private:
    using Clock = std::chrono::steady_clock;
    void connect();
    void sendCommandAndVerifyAck(uint8_t commandCode, const uint8_t *payload, size_t payloadLen);

    DetectorSerialConfig config_;
    std::unique_ptr<transport::AsioLink> link_;
};

struct Dmm6500Config {
    std::string visaResource;
    transport::Endpoint endpoint;
    std::chrono::milliseconds timeout{2000};
};

class Dmm6500 {
public:
    explicit Dmm6500(Dmm6500Config config);
    void setMode(int32_t mode);
    IRDmmReading read(int32_t expectedMode);

private:
    using Clock = std::chrono::steady_clock;
    void connect();
    std::string query(std::string_view command);
    void write(std::string_view command);
    IRDmmReading readOnce(int32_t expectedMode);

    Dmm6500Config config_;
    std::unique_ptr<transport::ScpiLink> link_;
};

struct GwsStenConfig {
    transport::Endpoint endpoint;
    int address{1};
    std::string delimiter{"\r\n"};
    double minSetpointC{-45.0};
    double maxSetpointC{70.0};
    std::chrono::milliseconds timeout{2000};
};

class GwsStenOven {
public:
    explicit GwsStenOven(GwsStenConfig config);
    IROvenReading read();
    double set(double targetC);
    void stop();

private:
    using Clock = std::chrono::steady_clock;
    void connect();
    std::string query(std::string_view body);
    IROvenReading readOnce();

    template <class Fn>
    auto withReconnect(Fn fn);

    GwsStenConfig config_;
    std::unique_ptr<transport::AsioLink> link_;
};

class PowerSupply {
public:
    virtual ~PowerSupply() = default;
    virtual uint32_t capabilities() const noexcept = 0;
    virtual int minChannel() const noexcept = 0;
    virtual int maxChannel() const noexcept = 0;
    virtual IRPowerReading readPower(int channel) = 0;
    virtual IRPowerReading setPowerOutput(int channel, bool enabled) = 0;
    virtual IRPowerReading setPowerChannel(int channel, double voltageV, double currentLimitA) = 0;
    virtual void close() {}
};

struct It6332aConfig {
    std::string visaResource;
    transport::Endpoint endpoint;
    std::chrono::milliseconds timeout{1200};
};

struct It6332aReading {
    double setpointVoltageV{};
    double currentLimitA{};
    double measuredVoltageV{};
    double measuredCurrentA{};
    bool outputEnabled{};
};

class It6332aPower final : public PowerSupply {
public:
    explicit It6332aPower(It6332aConfig config);
    uint32_t capabilities() const noexcept override
    {
        return IR_POWER_CAP_READ | IR_POWER_CAP_OUTPUT | IR_POWER_CAP_SET;
    }
    int minChannel() const noexcept override { return 1; }
    int maxChannel() const noexcept override { return 3; }
    IRPowerReading readPower(int channel) override
    {
        const auto v = read(channel);
        return {v.setpointVoltageV, v.currentLimitA, v.measuredVoltageV, v.measuredCurrentA,
                v.outputEnabled ? 1 : 0, 0};
    }
    IRPowerReading setPowerOutput(int channel, bool enabled) override
    {
        const auto v = setOutput(channel, enabled);
        return {v.setpointVoltageV, v.currentLimitA, v.measuredVoltageV, v.measuredCurrentA,
                v.outputEnabled ? 1 : 0, 0};
    }
    IRPowerReading setPowerChannel(int channel, double voltageV, double currentLimitA) override
    {
        const auto v = setChannel(channel, voltageV, currentLimitA);
        return {v.setpointVoltageV, v.currentLimitA, v.measuredVoltageV, v.measuredCurrentA,
                v.outputEnabled ? 1 : 0, 0};
    }

    It6332aReading read(int channel);
    It6332aReading setChannel(int channel, double voltageV, double currentLimitA);
    It6332aReading setOutput(int channel, bool enabled);

private:
    using Clock = std::chrono::steady_clock;
    void connect();
    void ensureRemote();
    void selectChannel(int channel);
    std::string query(const std::string &command);
    void write(const std::string &command);
    double queryNumber(const std::string &command);
    It6332aReading readOnce(int channel);
    void checkChannel(int channel) const;

    It6332aConfig config_;
    std::unique_ptr<transport::ScpiLink> link_;
    bool remote_{};
};

struct Udp6730Config {
    transport::Endpoint endpoint;
    std::chrono::milliseconds timeout{1500};
};

class Udp6730Power final : public PowerSupply {
public:
    explicit Udp6730Power(Udp6730Config config);
    uint32_t capabilities() const noexcept override
    {
        return IR_POWER_CAP_READ | IR_POWER_CAP_OUTPUT;
    }
    int minChannel() const noexcept override { return 1; }
    int maxChannel() const noexcept override { return 1; }
    IRPowerReading readPower(int channel) override;
    IRPowerReading setPowerOutput(int channel, bool enabled) override;
    IRPowerReading setPowerChannel(int channel, double voltageV, double currentLimitA) override;

private:
    using Clock = std::chrono::steady_clock;
    void connect();
    void checkChannel(int channel) const;
    std::string query(const std::string &command);
    void write(const std::string &command);
    double queryNumber(const std::string &command);
    IRPowerReading readOnce();

    Udp6730Config config_;
    std::unique_ptr<transport::AsioLink> link_;
};

struct ExternalPowerInspection {
    uint32_t capabilities{0};
    int32_t minChannel{0};
    int32_t maxChannel{0};
};

struct ExternalPowerConfig {
    std::string driverPath;
    std::string expectedDriverId;
    std::string driverConfigJson;
    ExternalPowerInspection inspected{};
};

ExternalPowerInspection inspectExternalPowerDriver(const ExternalPowerConfig &config,
                                                   std::string_view path);

class ExternalPowerSupply final : public PowerSupply {
public:
    explicit ExternalPowerSupply(ExternalPowerConfig config);
    ~ExternalPowerSupply() override;

    uint32_t capabilities() const noexcept override { return capabilities_; }
    int minChannel() const noexcept override { return minChannel_; }
    int maxChannel() const noexcept override { return maxChannel_; }
    IRPowerReading readPower(int channel) override;
    IRPowerReading setPowerOutput(int channel, bool enabled) override;
    IRPowerReading setPowerChannel(int channel, double voltageV, double currentLimitA) override;
    void close() override;

private:
    void checkChannel(int channel) const;

    ExternalPowerConfig config_;
    const IRPowerDriverDescriptor *desc_{nullptr};
    IRPowerDriverHandle handle_{nullptr};
    uint32_t capabilities_{0};
    int32_t minChannel_{1};
    int32_t maxChannel_{1};
    bool closed_{false};
};

} // namespace ir::instruments
