#pragma once

#include <drawforge/cancellation.hpp>
#include <drawforge/foundation.hpp>
#include <drawforge/query.hpp>
#include <drawforge/render.hpp>
#include <drawforge/scene.hpp>
#include <drawforge/transaction.hpp>

#include <cstdint>
#include <string_view>

namespace drawforge {

enum class DevelopmentStage {
  experimental,
};

struct Version {
  std::uint32_t major{};
  std::uint32_t minor{};
  std::uint32_t patch{};
  std::uint32_t tweak{};
  bool dirty{};
  auto operator==(const Version &) const -> bool = default;
};

struct ProjectInfo {
  std::string_view name;
  Version version;
  DevelopmentStage stage{DevelopmentStage::experimental};
  auto operator==(const ProjectInfo &) const -> bool = default;
};

[[nodiscard]] auto project_info() noexcept -> ProjectInfo;
[[nodiscard]] auto stage_name(DevelopmentStage stage) noexcept
    -> std::string_view;

} // namespace drawforge
