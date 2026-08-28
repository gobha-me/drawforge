#pragma once

#include <cstdint>

namespace drawforge::detail {

enum class TransactionFaultPoint : std::uint8_t {
  none,
  stage_allocation,
  receipt_allocation,
};

auto set_transaction_fault(TransactionFaultPoint fault) noexcept -> void;

} // namespace drawforge::detail
