#pragma once

#include <drawforge/cancellation.hpp>

#include <filesystem>
#include <iosfwd>

namespace drawforge::cli {

inline constexpr int exit_success = 0;
inline constexpr int exit_invocation = 2;
inline constexpr int exit_encoding = 3;
inline constexpr int exit_domain = 4;
inline constexpr int exit_adapter = 5;
inline constexpr int exit_cancelled = 130;

[[nodiscard]] auto run_jsonl(std::istream &input, std::ostream &output,
                             std::ostream &diagnostics,
                             const std::filesystem::path &artifact_directory,
                             CancellationToken cancellation = {}) -> int;

} // namespace drawforge::cli
