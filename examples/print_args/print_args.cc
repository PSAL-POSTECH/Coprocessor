// Example model: owns custom-2 and custom-1, prints what each face is given, computes nothing.
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "vcix_cp_base.hpp"

namespace {

using namespace cp;
using Insn = vcix_insn;

constexpr uint32_t MATCH_CUSTOM_2 = 0x5B;
constexpr uint32_t MATCH_CUSTOM_1 = 0x2B;
constexpr uint32_t MASK_OPCODE = 0x7F;
constexpr Cycle RECOVERY = 3;

bool is_custom_1(const Insn &insn) { return (insn.bits & MASK_OPCODE) == MATCH_CUSTOM_1; }

// Which fields of a custom-2 instruction are registers, by the VCIX operand rules.
struct VcixOperands {
  explicit VcixOperands(const Insn &insn)
      : shape(insn.bits >> 28), vd_written(((insn.bits >> 25) & 1) == 0), rs1_kind(funct3(insn)) {}
  uint32_t shape;
  bool vd_written;
  uint32_t rs1_kind;
  bool reads_vs2() const { return shape != 0x0; }
  bool reads_vd() const { return shape == 0xa || shape == 0xf; }
  bool reads_vs1() const { return rs1_kind == 0; }
  bool any_vector() const { return vd_written || reads_vd() || reads_vs2() || reads_vs1(); }
};

void print_fields(const char *who, const Insn &insn) {
  if (is_custom_1(insn))
    printf("[%s] insn=%08" PRIx32 " funct7=%02x rs2=%-2u rs1=%-2u funct3=%u rd=%-2u", who, insn.bits, insn.bits >> 25,
           rs2(insn), rs1(insn), funct3(insn), rd(insn));
  else
    printf("[%s] insn=%08" PRIx32 " funct6=%02x vm=%u f24_20=%-2u f19_15=%-2u funct3=%u f11_7=%-2u", who, insn.bits,
           insn.bits >> 26, (insn.bits >> 25) & 1, rs2(insn), rs1(insn), funct3(insn), rd(insn));
  printf(" | vl=%u sew=%u lmul=2^%d", insn.vl, insn.sew_bits, insn.lmul_log2);
}

// At most the first two elements of the register in lane 0.
void print_vreg(const Host &host, const Insn &insn, uint32_t reg) {
  const uint32_t bytes = insn.sew_bits / 8;
  const uint32_t count = std::min<uint32_t>(insn.vl, 2);
  if (count == 0 || bytes == 0 || bytes > sizeof(uint64_t)) return;
  const uint8_t *data = host.vreg<uint8_t>(0, reg);
  printf(count == 1 ? " v%u[0]=" : " v%u[0..1]=", reg);
  for (uint32_t i = 0; i < count; i++) {
    uint64_t element = 0;
    memcpy(&element, data + i * bytes, bytes);
    printf("%s0x%0*" PRIx64, i ? "," : "", static_cast<int>(2 * bytes), element);
  }
}

class PrintArgs : public Model {
 public:
  const char *name() const override { return "print_args"; }
  std::vector<Encoding> owns() const override { return {{MATCH_CUSTOM_2, MASK_OPCODE, "vcix"}, {MATCH_CUSTOM_1, MASK_OPCODE, "custom1"}}; }

  void configure(const Config &config) override {
    latency_ = config.uint("print_args_latency_cycles", 5);
    const char *lanes = config.get("vpu_num_lanes");
    printf("[config ] print_args_latency_cycles=%" PRIu64 " vpu_num_lanes=%s\n", latency_, lanes ? lanes : "(unset)");
    fflush(stdout);
  }

  void execute(const Host &host, const Insn &insn) override {
    print_fields("execute", insn);
    printf(" lanes=%u", host.lanes());
    if (is_custom_1(insn)) {
      printf(" x[%u]=0x%" PRIx64 " x[%u]=0x%" PRIx64 "\n", rs1(insn), host.xreg(rs1(insn)), rs2(insn), host.xreg(rs2(insn)));
      fflush(stdout);
      return;
    }
    const VcixOperands operands(insn);
    if (operands.rs1_kind == 4) printf(" x[%u]=0x%" PRIx64, rs1(insn), host.xreg(rs1(insn)));
    if (operands.rs1_kind == 5) printf(" f[%u]=0x%016" PRIx64, rs1(insn), host.freg_bits(rs1(insn)));
    if (operands.reads_vd()) print_vreg(host, insn, rd(insn));
    if (operands.reads_vs2()) print_vreg(host, insn, rs2(insn));
    if (operands.reads_vs1()) print_vreg(host, insn, rs1(insn));
    printf("\n");
    fflush(stdout);
  }

  bool can_accept(const Insn &, Cycle now) const override { return !in_flight_ && now >= busy_until_; }
  Cycle issue(const Insn &insn, Id, Cycle now) override {
    const bool vector = !is_custom_1(insn) && VcixOperands(insn).any_vector();
    Cycle cycles = latency_ * (vector ? group_size(insn) : 1);
    print_fields("issue  ", insn);
    printf(" | cycle=%" PRIu64 " ready=%" PRIu64 "\n", now, now + cycles);
    fflush(stdout);
    in_flight_ = true;
    return cycles;
  }
  void commit(const Insn &insn, Id, Cycle now) override {
    in_flight_ = false;
    busy_until_ = now + RECOVERY;
    if (replaying()) return;
    print_fields("commit ", insn);
    printf(" | cycle=%" PRIu64 "\n", now);
    fflush(stdout);
  }
  void reset() override {
    in_flight_ = false;
    busy_until_ = 0;
  }

 private:
  Cycle latency_ = 5;
  bool in_flight_ = false;
  Cycle busy_until_ = 0;
};

}  // namespace

VCIX_CP_REGISTER(PrintArgs)
