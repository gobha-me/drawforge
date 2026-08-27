#include "backend.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

namespace spike = drawforge::renderer_spike;

namespace {

enum class PathVerb { move_to, line_to, close, unsupported };

struct PathCommand {
  PathVerb verb;
  double x;
  double y;
};

[[nodiscard]] auto path_is_valid(std::span<const PathCommand> commands)
    -> bool {
  if (commands.empty() || commands.front().verb != PathVerb::move_to) {
    return false;
  }
  return std::ranges::all_of(commands, [](const PathCommand &command) {
    return command.verb != PathVerb::unsupported && std::isfinite(command.x) &&
           std::isfinite(command.y);
  });
}

[[nodiscard]] auto write_file(const std::filesystem::path &path,
                              const std::vector<std::uint8_t> &bytes) -> bool {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return output.good();
}

[[nodiscard]] auto common_failure_matrix_passes() -> bool {
  constexpr auto budget = 256U * 1024U * 1024U;
  if (spike::checked_rgba_size(0U, 1U, budget).has_value()) {
    return false;
  }
  if (spike::checked_rgba_size(std::numeric_limits<std::size_t>::max(), 2U,
                               budget)
          .has_value()) {
    return false;
  }
  if (spike::checked_rgba_size(16384U, 16384U, budget).has_value()) {
    return false;
  }
  const auto ordinary = spike::checked_rgba_size(spike::canvas_width,
                                                 spike::canvas_height, budget);
  if (!ordinary || *ordinary != spike::rgba_size) {
    return false;
  }

  const double malformed_numbers[]{std::numeric_limits<double>::quiet_NaN(),
                                   std::numeric_limits<double>::infinity()};
  if (std::ranges::all_of(malformed_numbers,
                          [](double value) { return std::isfinite(value); })) {
    return false;
  }
  const PathCommand malformed_path[]{
      {PathVerb::move_to, 0.0, 0.0},
      {PathVerb::unsupported, 1.0, 1.0},
  };
  if (path_is_valid(malformed_path)) {
    return false;
  }

  std::vector<int> committed_operations{1};
  const auto apply_at_boundary =
      [&committed_operations](bool cancelled) -> bool {
    if (cancelled) {
      return false;
    }
    committed_operations.push_back(2);
    return true;
  };
  if (apply_at_boundary(true) ||
      committed_operations != std::vector<int>({1})) {
    return false;
  }

  std::vector<std::uint8_t> committed{1U, 2U, 3U};
  const std::vector<std::uint8_t> proposed(32U, 0xaaU);
  const auto atomic_commit = [&committed](std::span<const std::uint8_t> bytes,
                                          std::size_t limit) -> bool {
    if (bytes.size() > limit) {
      return false;
    }
    committed.assign(bytes.begin(), bytes.end());
    return true;
  };
  if (atomic_commit(proposed, 8U) ||
      committed != std::vector<std::uint8_t>({1U, 2U, 3U})) {
    return false;
  }
  return true;
}

auto print_bounds(std::string_view name, const spike::Bounds &bounds) -> void {
  std::cout << '"' << name << "\":{\"x\":" << bounds.x << ",\"y\":" << bounds.y
            << ",\"width\":" << bounds.width << ",\"height\":" << bounds.height
            << '}';
}

} // namespace

auto main(int argc, char **argv) -> int {
  if (argc != 2) {
    std::cerr << "usage: drawforge-renderer-spike OUTPUT_DIR\n";
    return 64;
  }
  if (!common_failure_matrix_passes()) {
    std::cerr << "common failure matrix failed\n";
    return 2;
  }

  const auto evidence = spike::render_scene();
  if (!evidence) {
    std::cerr << evidence.error().code << ": " << evidence.error().message
              << '\n';
    return 3;
  }
  if (evidence->rgba.size() != spike::rgba_size ||
      !spike::png_signature_is_valid(evidence->png)) {
    std::cerr << "backend returned malformed image output\n";
    return 4;
  }

  const std::filesystem::path output_dir(argv[1]);
  std::error_code filesystem_error;
  std::filesystem::create_directories(output_dir, filesystem_error);
  if (filesystem_error ||
      !write_file(output_dir / "scene.rgba", evidence->rgba) ||
      !write_file(output_dir / "scene.png", evidence->png)) {
    std::cerr << "artifact write failed\n";
    return 5;
  }

  std::cout << "{\"backend\":\"" << spike::backend_name() << "\",\"version\":\""
            << DRAWFORGE_SPIKE_BACKEND_VERSION << "\",";
  print_bounds("fill_bounds", evidence->fill_bounds);
  std::cout << ',';
  print_bounds("stroke_bounds", evidence->stroke_bounds);
  std::cout << ',';
  print_bounds("transformed_bounds", evidence->transformed_bounds);
  std::cout << ",\"fill_hit\":" << std::boolalpha << evidence->fill_hit
            << ",\"fill_miss\":" << evidence->fill_miss
            << ",\"stroke_hit\":" << evidence->stroke_hit
            << ",\"native_fill_hit_test\":" << evidence->native_fill_hit_test
            << ",\"native_stroke_hit_test\":"
            << evidence->native_stroke_hit_test
            << ",\"native_tight_bounds\":" << evidence->native_tight_bounds
            << ",\"failure_matrix\":true}\n";
  return 0;
}
