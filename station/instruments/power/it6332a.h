#pragma once
#include "station/instruments/transport/asio_link.h"
#include <chrono>
#include <memory>
#include <string>

namespace ir::instruments {

struct It6332aConfig {
    std::string port;
    unsigned baud{115200};
    std::chrono::milliseconds timeout{1200};
};

struct It6332aReading {
    double setpointVoltageV{};
    double currentLimitA{};
    double measuredVoltageV{};
    double measuredCurrentA{};
    bool outputEnabled{};
};

class It6332aPower {
public:
    explicit It6332aPower(It6332aConfig config);
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
    std::unique_ptr<transport::AsioLink> link_;
    bool remote_{};
};

}
