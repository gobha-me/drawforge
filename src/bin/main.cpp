#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <version.hpp>

#if defined(DRAWFORGE_HAS_LIB)
#include <drawforge/drawforge.hpp>
#endif

namespace {

struct CliVersion {
  std::string_view name;
  std::uint32_t major{};
  std::uint32_t minor{};
  std::uint32_t patch{};
  std::uint32_t tweak{};
  bool dirty{};
};

auto current_version() -> CliVersion {
#if defined(DRAWFORGE_HAS_LIB)
  const auto info = drawforge::project_info();
  return {
      .name = info.name,
      .major = info.version.major,
      .minor = info.version.minor,
      .patch = info.version.patch,
      .tweak = info.version.tweak,
      .dirty = info.version.dirty,
  };
#else
  return {
      .name = PROGRAM_NAME,
      .major = VERSION_MAJOR,
      .minor = VERSION_MINOR,
      .patch = VERSION_PATCH,
      .tweak = VERSION_TWEAK,
      .dirty = VERSION_DIRTY,
  };
#endif
}

auto print_version() -> void {
  const auto version = current_version();
  std::cout << version.name << ' ' << version.major << '.' << version.minor
            << '.' << version.patch;
  if (version.tweak != 0U) std::cout << '+' << version.tweak;
  if (version.dirty) std::cout << "-dirty";
  std::cout << '\n';
}

auto print_help() -> void {
  std::cout
      << "DrawForge -- experimental semantic graphics tools for LLMs\n\n"
      << "Usage:\n"
      << "  drawforge --help\n"
      << "  drawforge --version\n\n"
      << "Document, scripting, and rendering commands are not implemented yet.\n"
      << "See DESIGN.md and the GitHub roadmap before depending on an API.\n";
}

}  // namespace

auto main(const int argc, char** argv) -> int {
  if (argc == 1) {
    print_help();
    return EXIT_SUCCESS;
  }

  if (argc == 2) {
    const std::string_view argument{argv[1]};
    if (argument == "--help" || argument == "-h") {
      print_help();
      return EXIT_SUCCESS;
    }
    if (argument == "--version") {
      print_version();
      return EXIT_SUCCESS;
    }
  }

  std::cerr << "drawforge: unsupported arguments; use --help\n";
  return 2;
}
