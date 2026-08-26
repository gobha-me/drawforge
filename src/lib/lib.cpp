#include <drawforge/drawforge.hpp>

#include <version.hpp>

namespace drawforge {

auto project_info() noexcept -> ProjectInfo {
  return {
      .name = PROGRAM_NAME,
      .version =
          {
              .major = VERSION_MAJOR,
              .minor = VERSION_MINOR,
              .patch = VERSION_PATCH,
              .tweak = VERSION_TWEAK,
              .dirty = VERSION_DIRTY,
          },
      .stage = DevelopmentStage::experimental,
  };
}

auto stage_name(const DevelopmentStage stage) noexcept -> std::string_view {
  switch (stage) {
    case DevelopmentStage::experimental:
      return "experimental";
  }

  return "unknown";
}

}  // namespace drawforge
