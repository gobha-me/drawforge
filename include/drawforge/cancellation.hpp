#pragma once

namespace drawforge {

class CancellationToken {
public:
  using Poll = bool (*)(const void *) noexcept;

  constexpr CancellationToken() noexcept = default;
  // The context is non-owning and must outlive the fallible operation. Each
  // operation documents the deterministic boundaries at which it polls.
  constexpr CancellationToken(const void *context, Poll poll) noexcept
      : m_context{context}, m_poll{poll} {}

  [[nodiscard]] auto stop_requested() const noexcept -> bool {
    return m_poll != nullptr && m_poll(m_context);
  }

private:
  const void *m_context{};
  Poll m_poll{};
};

} // namespace drawforge
