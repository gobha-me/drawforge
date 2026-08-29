#include "sha256.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>

namespace drawforge::cli {
namespace {

constexpr std::array<std::uint32_t, 64> round_constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
    0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
    0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
    0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
    0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
    0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
    0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
    0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

constexpr std::array<std::uint32_t, 8> initial_state{
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
};

auto load_be32(const std::uint8_t *bytes) noexcept -> std::uint32_t {
  return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
         (static_cast<std::uint32_t>(bytes[1]) << 16U) |
         (static_cast<std::uint32_t>(bytes[2]) << 8U) |
         static_cast<std::uint32_t>(bytes[3]);
}

auto transform(std::array<std::uint32_t, 8> &state,
               const std::uint8_t *block) noexcept -> void {
  std::array<std::uint32_t, 64> words{};
  for (std::size_t index = 0; index < 16; ++index)
    words[index] = load_be32(block + index * 4U);
  for (std::size_t index = 16; index < words.size(); ++index) {
    const auto s0 = std::rotr(words[index - 15], 7) ^
                    std::rotr(words[index - 15], 18) ^
                    (words[index - 15] >> 3U);
    const auto s1 = std::rotr(words[index - 2], 17) ^
                    std::rotr(words[index - 2], 19) ^ (words[index - 2] >> 10U);
    words[index] = words[index - 16] + s0 + words[index - 7] + s1;
  }

  auto a = state[0];
  auto b = state[1];
  auto c = state[2];
  auto d = state[3];
  auto e = state[4];
  auto f = state[5];
  auto g = state[6];
  auto h = state[7];

  for (std::size_t index = 0; index < words.size(); ++index) {
    const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
    const auto choose = (e & f) ^ (~e & g);
    const auto temporary1 =
        h + sum1 + choose + round_constants[index] + words[index];
    const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
    const auto majority = (a & b) ^ (a & c) ^ (b & c);
    const auto temporary2 = sum0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temporary1;
    d = c;
    c = b;
    b = a;
    a = temporary1 + temporary2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

} // namespace

auto sha256_hex(const std::span<const std::uint8_t> bytes) -> std::string {
  static_assert(std::numeric_limits<std::uint64_t>::digits == 64);

  auto state = initial_state;
  std::size_t offset{};
  while (bytes.size() - offset >= 64U) {
    transform(state, bytes.data() + offset);
    offset += 64U;
  }

  std::array<std::uint8_t, 128> tail{};
  const auto remainder = bytes.size() - offset;
  for (std::size_t index = 0; index < remainder; ++index)
    tail[index] = bytes[offset + index];
  tail[remainder] = 0x80U;

  const auto tail_size = remainder < 56U ? 64U : 128U;
  const auto bit_length = static_cast<std::uint64_t>(bytes.size()) * 8U;
  for (std::size_t index = 0; index < 8U; ++index) {
    tail[tail_size - 1U - index] =
        static_cast<std::uint8_t>(bit_length >> (index * 8U));
  }
  transform(state, tail.data());
  if (tail_size == 128U)
    transform(state, tail.data() + 64U);

  constexpr char hex[] = "0123456789abcdef";
  std::string result(64U, '0');
  std::size_t output{};
  for (const auto word : state) {
    for (int shift = 28; shift >= 0; shift -= 4)
      result[output++] = hex[(word >> static_cast<unsigned>(shift)) & 0xfU];
  }
  return result;
}

} // namespace drawforge::cli
