#pragma once

#include <drawforge/foundation.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <limits>
#include <new>
#include <span>
#include <vector>

namespace drawforge::detail {

inline constexpr ValueError arithmetic_overflow_error{
    ValueErrorCode::arithmetic_overflow,
    "size arithmetic is not representable"};
inline constexpr ValueError resource_limit_error{
    ValueErrorCode::resource_limit, "value exceeds the configured limit"};
inline constexpr ValueError duplicate_identity_error{
    ValueErrorCode::duplicate_identity, "identity is duplicated"};
inline constexpr ValueError allocation_failure_error{
    ValueErrorCode::allocation_failure,
    "identity validation allocation failed"};

[[nodiscard]] constexpr auto checked_add(std::uint64_t left,
                                         std::uint64_t right) noexcept
    -> std::expected<std::uint64_t, ValueError> {
  if (left > std::numeric_limits<std::uint64_t>::max() - right) {
    return std::unexpected{arithmetic_overflow_error};
  }
  return left + right;
}

[[nodiscard]] constexpr auto checked_multiply(std::uint64_t left,
                                              std::uint64_t right) noexcept
    -> std::expected<std::uint64_t, ValueError> {
  if (right != 0 && left > std::numeric_limits<std::uint64_t>::max() / right) {
    return std::unexpected{arithmetic_overflow_error};
  }
  return left * right;
}

template <typename Id>
[[nodiscard]] auto validate_unique_ids(std::span<const Id> ids,
                                       std::uint64_t max_count) noexcept
    -> std::expected<void, ValueError> {
  if (ids.size() > max_count) {
    return std::unexpected{resource_limit_error};
  }

  try {
    std::vector<const Id *> ordered;
    ordered.reserve(ids.size());
    for (const auto &id : ids) {
      ordered.push_back(&id);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const Id *left, const Id *right) { return *left < *right; });
    for (std::size_t index = 1; index < ordered.size(); ++index) {
      if (*ordered[index - 1] == *ordered[index]) {
        return std::unexpected{duplicate_identity_error};
      }
    }
  } catch (const std::bad_alloc &) {
    return std::unexpected{allocation_failure_error};
  } catch (...) {
    return std::unexpected{allocation_failure_error};
  }
  return {};
}

} // namespace drawforge::detail
