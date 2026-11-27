#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <string_view>

namespace ir::instruments {

struct PowerCapabilitiesInfo {
    std::uint32_t capabilities{0};
    std::int32_t minChannel{0};
    std::int32_t maxChannel{0};
};

// Validate the connection fields without resolving or opening the endpoint.
void validateDeviceConnection(std::int32_t kind, const nlohmann::json &connection,
                              std::string_view path);

// Return static power capabilities and valid channel range for a configured device.
PowerCapabilitiesInfo queryPowerDeviceCapabilities(std::int32_t kind,
                                                   const nlohmann::json &connection,
                                                   std::string_view path);

} // namespace ir::instruments
