#include <drawforge/drawforge.hpp>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

auto print_version() -> void {
  const auto info = drawforge::project_info();
  std::cout << info.name << ' ' << info.version.major << '.'
            << info.version.minor << '.' << info.version.patch;
  if (info.version.tweak != 0U) std::cout << '+' << info.version.tweak;
  if (info.version.dirty) std::cout << "-dirty";
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
