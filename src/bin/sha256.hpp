#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace drawforge::cli {

[[nodiscard]] auto sha256_hex(std::span<const std::uint8_t> bytes)
    -> std::string;

} // namespace drawforge::cli
