// The numbers every tpu unit's values use, as the Spike these units came from did them (riscv-isa-sim branch
// spike-fp8, 7259e73, riscv/insns/torchsim_*.h): element conversions and rounding, and an element of a register group
// or of memory. Each component's handlers compute with them; a component keeps its values in its own blocks.
#ifndef TPU_VALUES_HPP
#define TPU_VALUES_HPP

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "vcix_cp_base.hpp"

namespace tpu {

// Ends the run: an instruction the model cannot carry out has no trap to take through the interface.
[[noreturn]] inline void fail(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  std::fprintf(stderr, "tpu: ");
  std::vfprintf(stderr, format, arguments);
  std::fprintf(stderr, "\n");
  va_end(arguments);
  std::exit(1);
}

// IEEE half precision as SoftFloat for RISC-V converts it: round to nearest even, and every NaN is the default NaN.
inline float half_to_float(uint16_t half) {
  const uint32_t exponent = (half >> 10) & 0x1f, fraction = half & 0x3ff;
  float value;
  if (exponent == 0x1f) {
    const uint32_t bits = fraction ? 0x7fc00000u : (half & 0x8000 ? 0xff800000u : 0x7f800000u);
    std::memcpy(&value, &bits, sizeof value);
    return value;
  }
  value = std::ldexp(static_cast<float>(exponent ? fraction | 0x400 : fraction), static_cast<int>(exponent ? exponent : 1) - 25);
  return half & 0x8000 ? -value : value;
}
inline uint16_t double_to_half(double value) {
  if (std::isnan(value)) return 0x7e00;
  const uint16_t sign = std::signbit(value) ? 0x8000 : 0;
  const double magnitude = std::fabs(value);
  if (std::isinf(magnitude)) return sign | 0x7c00;
  if (magnitude == 0) return sign;
  int exponent;
  std::frexp(magnitude, &exponent);
  exponent = exponent - 1 < -14 ? -14 : exponent - 1;
  if (exponent > 15) return sign | 0x7c00;
  const double scaled = std::ldexp(magnitude, 10 - exponent);
  double rounded = std::floor(scaled);
  const double rest = scaled - rounded;
  if (rest > 0.5 || (rest == 0.5 && std::fmod(rounded, 2) == 1)) rounded += 1;
  if (rounded == 2048) {
    rounded = 1024;
    exponent++;
  }
  if (exponent > 15) return sign | 0x7c00;
  const uint16_t significand = static_cast<uint16_t>(rounded);
  if (significand < 1024) return sign | significand;
  return sign | static_cast<uint16_t>((exponent + 15) << 10) | (significand - 1024);
}
// A float of fewer bits than a single: a half, or one of the two of 8 bits. E4M3 has no infinity; what is too
// large for it becomes S.1111.111, which is also its NaN.
struct Narrow {
  int fraction_bits, exponent_bits;
  bool infinity;
};
constexpr Narrow HALF{10, 5, true}, E4M3{3, 4, false}, E5M2{2, 5, true};
// A bfloat16 as a single: its bits are the single's upper half. Every NaN is the default NaN.
inline float bf16_to_float(uint16_t bits) {
  const uint32_t wide = (bits & 0x7f80) == 0x7f80 && (bits & 0x7f) ? 0x7fc00000u : static_cast<uint32_t>(bits) << 16;
  float value;
  std::memcpy(&value, &wide, sizeof value);
  return value;
}

// `value` as `to`, rounded as SoftFloat for RISC-V rounds under the mode `frm` holds: 0 to nearest even,
// 1 toward zero, 2 down, 3 up, 4 to nearest away, 5 to odd (toward zero, the last bit set when anything was
// lost); 6 and 7 toward zero. Every NaN is the default NaN.
inline uint32_t narrow(float value, const Narrow &to, uint64_t mode) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof bits);
  const bool negative = bits >> 31;
  const uint32_t exponent = (bits >> 23) & 0xff, fraction = bits & 0x7fffff;
  const int bias = (1 << (to.exponent_bits - 1)) - 1, largest = to.infinity ? bias : bias + 1;
  const uint32_t sign = static_cast<uint32_t>(negative) << (to.exponent_bits + to.fraction_bits);
  const uint32_t ones = ((1u << to.exponent_bits) - 1) << to.fraction_bits, hidden = 1u << to.fraction_bits;
  const uint32_t too_large = to.infinity ? ones : ones | (hidden - 1);
  if (exponent == 0xff) return fraction ? (to.infinity ? ones | hidden >> 1 : too_large) : sign | too_large;
  if (!exponent && !fraction) return sign;

  const uint64_t significand = exponent ? fraction | 0x800000 : fraction;
  const int scale = static_cast<int>(exponent ? exponent : 1) - 127 - 23;
  int top = scale + 23;
  for (uint64_t rest = significand; rest < 0x800000; rest <<= 1) top--;
  int at = top < 1 - bias ? 1 - bias : top;
  const int shift = at - to.fraction_bits - scale;
  const uint64_t kept = shift < 40 ? significand >> shift : 0;
  const uint64_t below = shift < 40 ? significand & ((uint64_t{1} << shift) - 1) : significand;
  const uint64_t half = uint64_t{1} << (shift < 40 ? shift - 1 : 40);
  const bool away = mode == 0 || mode == 4 || (mode == 2 && negative) || (mode == 3 && !negative);
  bool up = false;
  if (mode == 0) up = below > half || (below == half && (kept & 1));
  if (mode == 4) up = below >= half;
  if (mode == 2 || mode == 3) up = away && below != 0;
  uint32_t rounded = static_cast<uint32_t>(kept) + up;
  if (mode == 5 && below != 0) rounded |= 1;
  if (rounded == hidden << 1) {
    rounded = hidden;
    at++;
  }
  if (at > largest || (!to.infinity && at == largest && rounded == (hidden << 1) - 1)) return sign | (too_large - !away);
  if (rounded < hidden) return sign | rounded;
  return sign | static_cast<uint32_t>(at + bias) << to.fraction_bits | (rounded - hidden);
}
// A float of 8 bits as a single, which holds every one of them exactly.
inline float fp8_to_float(uint8_t byte, const Narrow &from) {
  const uint32_t hidden = 1u << from.fraction_bits;
  const uint32_t exponent = (byte & 0x7f) >> from.fraction_bits, fraction = byte & (hidden - 1);
  const bool all_ones = exponent == (1u << from.exponent_bits) - 1;
  float value;
  if (from.infinity ? all_ones : (byte & 0x7f) == 0x7f) {
    const uint32_t bits = !from.infinity || fraction ? 0x7fc00000u : (byte & 0x80 ? 0xff800000u : 0x7f800000u);
    std::memcpy(&value, &bits, sizeof value);
    return value;
  }
  const int bias = (1 << (from.exponent_bits - 1)) - 1;
  value = std::ldexp(static_cast<float>(exponent ? fraction | hidden : fraction),
                     static_cast<int>(exponent ? exponent : 1) - bias - from.fraction_bits);
  return byte & 0x80 ? -value : value;
}

// An operation's NaN as x86's SSE gives it, which golden's values came from: a NaN `first` operand quieted, else a NaN
// `second`, else the result. The units name the operand golden's build put first, so a NaN is the same bit for bit.
template <class F>
inline F sse_nan(F first, F second, F result) {
  F nan = std::isnan(first) ? first : std::isnan(second) ? second : result;
  if (!std::isnan(nan)) return nan;
  unsigned char bytes[sizeof(F)];
  std::memcpy(bytes, &nan, sizeof(F));
  bytes[sizeof(F) - 2] |= sizeof(F) == 4 ? 0x40 : 0x08;
  std::memcpy(&nan, bytes, sizeof(F));
  return nan;
}
// acc + a x b, the product's NaN a's first, the sum's acc's.
inline float multiply_add(float acc, float a, float b) {
  const float product = sse_nan(a, b, a * b);
  return sse_nan(acc, product, acc + product);
}
// The sum of two halves is exact in a double, so this rounds once.
inline uint16_t half_add(uint16_t a, uint16_t b) {
  return double_to_half(static_cast<double>(half_to_float(a)) + static_cast<double>(half_to_float(b)));
}

// Element `index` of the group that starts at vector register `reg`, as the type T.
template <class T>
T get(const cp::Host &host, uint32_t lane, uint32_t reg, uint64_t index) {
  const uint64_t per_register = host.vlen_bits() / 8 / sizeof(T);
  T value;
  std::memcpy(&value, host.vreg<unsigned char>(lane, reg + index / per_register) + index % per_register * sizeof(T),
              sizeof(T));
  return value;
}
template <class T>
void put(const cp::Host &host, uint32_t lane, uint32_t reg, uint64_t index, T value) {
  const uint64_t per_register = host.vlen_bits() / 8 / sizeof(T);
  std::memcpy(host.vreg_mut<unsigned char>(lane, reg + index / per_register) + index % per_register * sizeof(T),
              &value, sizeof(T));
}
template <class T>
T load(const cp::Host &host, uint64_t address) {
  T value;
  host.mem_read(address, &value, sizeof(T));
  return value;
}
template <class T>
void store(const cp::Host &host, uint64_t address, T value) {
  host.mem_write(address, &value, sizeof(T));
}

}

#endif
