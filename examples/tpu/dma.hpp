// The DMA, tpu::Misc's untimed side path (TOGSim counts its time): a tensor of up to four dimensions between memory
// and the scratchpad, one slice of the split axis per lane, as the Spike these units came from did it (9f555b4).
// dma_config_desc names the descriptor; mvin and mvout read it when they run.
#ifndef TPU_DMA_HPP
#define TPU_DMA_HPP

#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "vcix_cp_base.hpp"

#include "values.hpp"

namespace tpu {

class Dma {
  using Host = cp::Host;
  using Insn = vcix_insn;

 public:
  void configure(const cp::Config &config) {
    if (config.get(SPAD_LANE_KEY)) spad_lane_bytes_ = config.uint(SPAD_LANE_KEY, 0) * 1024;
    spad_base_ = config.hex(SPAD_BASE_KEY, 0xD0000000);
  }
  static constexpr const char *SPAD_LANE_KEY = "vpu_spad_size_kb_per_lane";
  bool scratchpad() const { return spad_lane_bytes_ != 0; }
  // The exit code of a transfer that leaves a lane's scratchpad; SKIP, an address another transfer owns.
  static constexpr int INVALID_SPAD_ACCESS = 200;
  static constexpr uint64_t SKIP = ~uint64_t{0};

  void dma_config_desc(const Host &host, const Insn &insn) { dma_descriptor_ = host.xreg(cp::rs1(insn)); }
  // x[rs1] once named the indirect transfers' index dumps; nothing reads it now, and programs that issue it still run.
  void dma_index_key(const Host &, const Insn &) {}

  struct Transfer {
    uint64_t memory, scratchpad;
    uint64_t dim_size[4], dim_low[4], dim_high[4], mm_stride[4], spad_stride[4];
    uint64_t element_size, vlane_stride;
    int split_axis;
    bool indirect, masked, accumulate, accumulate_float;
    unsigned skip_axes;
    uint64_t indirect_address, indirect_stride, indirect_element_size, indirect_lanes, fill, memory_base, memory_bytes;
    int indirect_dim;

    uint64_t lanes, outer_loops, used_lanes;
    uint64_t block_dim[4], block_stride[4], buffer_stride[4];
    uint64_t lane_stride, outer_stride, spad_outer_stride;
    std::vector<uint64_t> address;
  };

  Transfer dma_transfer(const Host &host, const Insn &insn, const char *name, bool skips) const {
    Transfer t;
    t.memory = host.xreg(cp::rs1(insn));
    t.scratchpad = host.xreg(cp::rs2(insn));
    const uint64_t d = dma_descriptor_;
    for (int i = 0; i < 4; i++) {
      t.dim_size[i] = static_cast<uint64_t>(static_cast<int64_t>(load<int32_t>(host, d + 0 + 4 * i)));
      t.dim_low[i] = static_cast<uint64_t>(static_cast<int64_t>(load<int32_t>(host, d + 16 + 4 * i)));
      t.dim_high[i] = static_cast<uint64_t>(static_cast<int64_t>(load<int32_t>(host, d + 32 + 4 * i)));
      t.mm_stride[i] = load<uint64_t>(host, d + 48 + 8 * i);
      t.spad_stride[i] = load<uint64_t>(host, d + 80 + 8 * i);
    }
    t.element_size = load<uint16_t>(host, d + 112);
    t.vlane_stride = load<uint16_t>(host, d + 114);
    t.split_axis = load<uint8_t>(host, d + 116);
    const uint16_t flags = load<uint16_t>(host, d + 118);
    t.indirect = flags & 0x1;
    t.masked = flags & 0x2;
    t.accumulate = flags & 0x4;
    t.accumulate_float = flags & 0x8;
    t.skip_axes = skips ? (flags >> 4) & 0xF : 0;
    t.indirect_address = load<uint64_t>(host, d + 120);
    t.indirect_stride = load<uint16_t>(host, d + 128);
    t.indirect_element_size = load<uint16_t>(host, d + 130);
    t.indirect_dim = load<uint8_t>(host, d + 132);
    t.indirect_lanes = load<uint16_t>(host, d + 134);
    t.fill = load<uint64_t>(host, d + 136);
    t.memory_base = load<uint64_t>(host, d + 144);
    t.memory_bytes = load<uint64_t>(host, d + 152);

    if (t.split_axis > 3) fail("%s: the descriptor splits axis %d of 4", name, t.split_axis);
    if (t.element_size == 0 || t.vlane_stride == 0) fail("%s: the descriptor has an element size or a lane stride of 0", name);
    for (int i = 0; i < 4; i++)
      if (t.dim_size[i] == 0) fail("%s: the descriptor has a dimension of 0", name);

    t.lanes = host.lanes();
    const uint64_t *dim = t.dim_size;
    t.outer_loops = (dim[t.split_axis] + t.vlane_stride * t.lanes - 1) / (t.vlane_stride * t.lanes);
    t.used_lanes = t.outer_loops > 1 ? t.lanes : (dim[t.split_axis] + t.vlane_stride - 1) / t.vlane_stride;
    for (int i = 0; i < 4; i++) {
      t.block_dim[i] = dim[i];
      t.block_stride[i] = t.spad_stride[i];
    }
    t.block_dim[t.split_axis] = t.vlane_stride;
    for (int i = 0; i < 4; i++)
      if (t.block_stride[i] > t.spad_stride[t.split_axis])
        t.block_stride[i] = t.block_stride[i] / dim[t.split_axis] * t.vlane_stride * t.outer_loops;

    const uint64_t elements = dim[0] * dim[1] * dim[2] * dim[3];
    const uint64_t round = t.used_lanes * t.block_dim[0] * t.block_dim[1] * t.block_dim[2] * t.block_dim[3];
    const uint64_t buffer_size = (elements + round - 1) / round * round;
    t.buffer_stride[0] = dim[1] * dim[2] * dim[3];
    t.buffer_stride[1] = dim[2] * dim[3];
    t.buffer_stride[2] = dim[3];
    t.buffer_stride[3] = 1;
    t.lane_stride = t.buffer_stride[t.split_axis] * t.vlane_stride;
    t.outer_stride = t.lane_stride * t.used_lanes;
    t.spad_outer_stride = t.block_stride[t.split_axis] * t.vlane_stride;

    try {
      t.address.assign(buffer_size, 0);
    } catch (const std::bad_alloc &) {
      fail("%s: no memory for a transfer of %llu elements", name, static_cast<unsigned long long>(buffer_size));
    }
    for (uint64_t n = 0; n < dim[0]; n++)
      for (uint64_t c = 0; c < dim[1]; c++)
        for (uint64_t h = 0; h < dim[2]; h++)
          for (uint64_t w = 0; w < dim[3]; w++) {
            const uint64_t offset =
                (n * t.mm_stride[0] + c * t.mm_stride[1] + h * t.mm_stride[2] + w * t.mm_stride[3]) * t.element_size;
            const uint64_t coordinate[4] = {n, c, h, w};
            bool in_box = true;
            uint64_t outside = 0;
            for (int i = 0; i < 4; i++) {
              const bool inside = coordinate[i] >= t.dim_low[i] && coordinate[i] < t.dim_high[i];
              in_box = in_box && inside;
              if ((t.skip_axes >> i & 1) && !inside) outside = SKIP;
            }
            t.address[n * t.buffer_stride[0] + c * t.buffer_stride[1] + h * t.buffer_stride[2] + w] =
                !t.masked || in_box ? t.memory + offset : outside;
          }
    return t;
  }

  // The address the index of an indirect transfer adds to.
  uint64_t dma_indirect(const Host &host, const Transfer &t, const char *name, uint64_t outer, uint64_t lane,
                        const uint64_t at[4], uint64_t spad_index) const {
    uint64_t address;
    if (t.indirect_dim) {
      const int axis = t.indirect_dim - 1;
      uint64_t coordinate = axis == 0 ? at[0] : axis == 1 ? at[1] : axis == 2 ? at[2] : at[3];
      if (axis == t.split_axis) coordinate += (outer * t.used_lanes + lane) * t.vlane_stride;
      const uint64_t lanes = t.indirect_lanes ? t.indirect_lanes : 1;
      address = t.indirect_address + coordinate / lanes * t.indirect_element_size + coordinate % lanes * spad_lane_bytes_;
    } else {
      address = t.indirect_address + spad_index * t.indirect_element_size + lane * spad_lane_bytes_;
    }
    uint64_t index = 0;
    switch (t.indirect_element_size) {
      case 1: index = static_cast<uint64_t>(static_cast<int64_t>(load<int8_t>(host, address))); break;
      case 2: index = static_cast<uint64_t>(static_cast<int64_t>(load<int16_t>(host, address))); break;
      case 4: index = static_cast<uint64_t>(static_cast<int64_t>(load<int32_t>(host, address))); break;
      case 8: index = load<uint64_t>(host, address); break;
      default: fail("%s: an index of %llu bytes is not supported", name, static_cast<unsigned long long>(t.indirect_element_size));
    }
    return index * t.indirect_stride * t.element_size;
  }

  template <class T>
  static void mvin_element(const Host &host, uint64_t from, uint64_t to, bool used, uint64_t fill) {
    store<T>(host, to, used ? load<T>(host, from) : static_cast<T>(fill));
  }
  void mvin(const Host &host, const Insn &insn) {
    const Transfer t = dma_transfer(host, insn, "mvin", true);
    for (uint64_t outer = 0; outer < t.outer_loops; outer++)
      for (uint64_t lane = 0; lane < t.lanes; lane++)
        for (uint64_t n = 0; n < t.block_dim[0]; n++)
          for (uint64_t c = 0; c < t.block_dim[1]; c++)
            for (uint64_t h = 0; h < t.block_dim[2]; h++)
              for (uint64_t w = 0; w < t.block_dim[3]; w++) {
                const uint64_t at[4] = {n, c, h, w};
                const uint64_t position = t.outer_stride * outer + t.lane_stride * lane + t.buffer_stride[0] * n +
                                          t.buffer_stride[1] * c + t.buffer_stride[2] * h + t.buffer_stride[3] * w;
                const uint64_t spad_index = t.spad_outer_stride * outer + t.block_stride[0] * n + t.block_stride[1] * c +
                                            t.block_stride[2] * h + t.block_stride[3] * w;
                bool used = lane < t.used_lanes;
                const uint64_t to = t.scratchpad + spad_index * t.element_size + lane * spad_lane_bytes_;
                uint64_t from = used ? dma_address(t, "mvin", position) : 0;
                if (from == SKIP) continue;
                used = used && from != 0;
                if (t.scratchpad + spad_index * t.element_size >= spad_base_ + spad_lane_bytes_) {
                  std::fprintf(stderr, "MVIN ERROR: Scratchpad address overflow: 0x%llx\n", static_cast<unsigned long long>(to));
                  std::exit(INVALID_SPAD_ACCESS);
                }
                if (t.indirect && used) from += dma_indirect(host, t, "mvin", outer, lane, at, spad_index);
                if (t.memory_bytes && (from < t.memory_base || from + t.element_size > t.memory_base + t.memory_bytes))
                  used = false;
                switch (t.element_size) {
                  case 1: mvin_element<uint8_t>(host, from, to, used, t.fill); break;
                  case 2: mvin_element<uint16_t>(host, from, to, used, t.fill); break;
                  case 4: mvin_element<uint32_t>(host, from, to, used, t.fill); break;
                  case 8: mvin_element<uint64_t>(host, from, to, used, t.fill); break;
                  default: break;
                }
              }
  }

  void mvout(const Host &host, const Insn &insn) {
    const Transfer t = dma_transfer(host, insn, "mvout", false);
    for (uint64_t outer = 0; outer < t.outer_loops; outer++)
      for (uint64_t lane = 0; lane < t.used_lanes; lane++)
        for (uint64_t n = 0; n < t.block_dim[0]; n++)
          for (uint64_t c = 0; c < t.block_dim[1]; c++)
            for (uint64_t h = 0; h < t.block_dim[2]; h++)
              for (uint64_t w = 0; w < t.block_dim[3]; w++) {
                const uint64_t at[4] = {n, c, h, w};
                const uint64_t position = t.outer_stride * outer + t.lane_stride * lane + t.buffer_stride[0] * n +
                                          t.buffer_stride[1] * c + t.buffer_stride[2] * h + t.buffer_stride[3] * w;
                const uint64_t spad_index = t.spad_outer_stride * outer + t.block_stride[0] * n + t.block_stride[1] * c +
                                            t.block_stride[2] * h + t.block_stride[3] * w;
                const uint64_t from = t.scratchpad + spad_index * t.element_size + lane * spad_lane_bytes_;
                uint64_t to = dma_address(t, "mvout", position);
                if (to == 0) continue;
                if (t.scratchpad + spad_index * t.element_size >= spad_base_ + spad_lane_bytes_) {
                  std::fprintf(stderr, "MVOUT ERROR: Scratchpad address overflow: 0x%llx\n", static_cast<unsigned long long>(from));
                  std::exit(INVALID_SPAD_ACCESS);
                }
                if (t.indirect) to += dma_indirect(host, t, "mvout", outer, lane, at, spad_index);
                if (t.memory_bytes && (to < t.memory_base || to + t.element_size > t.memory_base + t.memory_bytes))
                  continue;
                if (t.element_size == 1) {
                  uint8_t value = load<uint8_t>(host, from);
                  if (t.accumulate) value = static_cast<uint8_t>(load<uint8_t>(host, to) + value);
                  store<uint8_t>(host, to, value);
                } else if (t.element_size == 2) {
                  uint16_t value = load<uint16_t>(host, from);
                  if (t.accumulate) {
                    const uint16_t current = load<uint16_t>(host, to);
                    value = t.accumulate_float ? half_add(current, value) : static_cast<uint16_t>(current + value);
                  }
                  store<uint16_t>(host, to, value);
                } else if (t.element_size == 4) {
                  if (t.accumulate && t.accumulate_float)
                    store<float>(host, to, accumulated<float>(host, to, from));
                  else
                    store<uint32_t>(host, to, (t.accumulate ? load<uint32_t>(host, to) : 0) + load<uint32_t>(host, from));
                } else if (t.element_size == 8) {
                  if (t.accumulate && t.accumulate_float)
                    store<double>(host, to, accumulated<double>(host, to, from));
                  else
                    store<uint64_t>(host, to, (t.accumulate ? load<uint64_t>(host, to) : 0) + load<uint64_t>(host, from));
                }
              }
  }
  // What memory holds at `to` plus what the scratchpad holds at `from`, read in that order; a NaN is from's first.
  template <class F>
  static F accumulated(const Host &host, uint64_t to, uint64_t from) {
    const F current = load<F>(host, to), added = load<F>(host, from);
    return sse_nan(added, current, current + added);
  }
  static uint64_t dma_address(const Transfer &t, const char *name, uint64_t position) {
    if (position >= t.address.size())
      fail("%s: the descriptor reaches position %llu of a tensor of %zu", name, static_cast<unsigned long long>(position),
           t.address.size());
    return t.address[position];
  }

 private:
  static constexpr const char *SPAD_BASE_KEY = "vpu_spad_base_vaddr";

  uint64_t spad_lane_bytes_ = 0;
  uint64_t spad_base_ = 0xD0000000;

  uint64_t dma_descriptor_ = 0;
};

}  // namespace tpu

#endif
