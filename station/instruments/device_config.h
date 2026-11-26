#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <string_view>

namespace ir::instruments {

// Validate the connection fields without resolving or opening the endpoint.
void validateDeviceConnection(std::int32_t kind, const nlohmann::json &connection,
                              std::string_view path);

} // namespace ir::instruments
