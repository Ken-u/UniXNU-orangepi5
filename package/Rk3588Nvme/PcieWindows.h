// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// PcieWindows.h -- where the firmware (edk2-rk3588) put the memory windows of PCIe segment 4, the controller behind the M.2 slot
// of the Orange Pi 5, and how a BAR value maps to a CPU address. A pure function of the BAR, no kernel dependency:
// tests/test-pcie-windows.cpp runs it.
//
// Two windows (PciHostBridgeInit.c of edk2-rk3588):
//   32-bit: bus 0xf0000000, 128 MiB, reached by the CPU at 0xa38000000 through an iATU region with a translation;
//   64-bit: bus address = CPU address, 0xa00000000, 768 MiB.
//
// Which window a BAR sits in depends on the ADDRESS the firmware assigned, not on whether the BAR can be 64 bits wide: a 64-bit
// non-prefetchable BAR (the Silicon Motion controller of the drive on the first board that ran this: BAR0 low dword 0xf0000004)
// is placed in the 32-bit window, because non-prefetchable memory cannot go above 4 GiB behind a bridge without a 64-bit
// non-prefetchable window. The first version of the driver demanded a 32-bit BAR there and refused this one; the log of the first run
// (BAR0 0xf0000000 (64-bit) lies in none of the windows) is what showed it.
#pragma once

#include <stdint.h>

namespace pcie {

constexpr uint64_t kMem32Bus = 0xf0000000ull;
constexpr uint64_t kMem32Size = 0x08000000ull;
constexpr uint64_t kMem32Cpu = 0xa38000000ull;
constexpr uint64_t kMem64Cpu = 0xa00000000ull;
constexpr uint64_t kMem64Size = 0x30000000ull;

// The CPU address of the BAR that starts at the bus address `bus` and is `bytes` long, or false when the whole BAR is not inside one of the
// windows (or the arguments are empty/wrap around).
inline bool barToCpu(uint64_t bus, uint64_t bytes, uint64_t *cpu) {
  if (!bus || !bytes || bus + bytes < bus) return false;
  if (bus >= kMem32Bus && bus + bytes <= kMem32Bus + kMem32Size) {
    *cpu = bus - kMem32Bus + kMem32Cpu;
    return true;
  }
  if (bus >= kMem64Cpu && bus + bytes <= kMem64Cpu + kMem64Size) {
    *cpu = bus;
    return true;
  }
  return false;
}

}  // namespace pcie
