// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PcieWindows.h: g++ -std=c++17 -Wall -Wextra -Werror -O1 -I . tests/test-pcie-windows.cpp -o /tmp/test-pcie-windows && /tmp/test-pcie-windows
#include <stdint.h>
#include <stdio.h>

#include "PcieWindows.h"

static int failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      printf("FAIL line %d: %s\n", __LINE__, #cond);                   \
      failures++;                                                      \
    }                                                                  \
  } while (0)

static bool map(uint64_t bus, uint64_t bytes, uint64_t *cpu) {
  *cpu = 0xdeadbeef;
  return pcie::barToCpu(bus, bytes, cpu);
}

int main() {
  uint64_t cpu;
  // The BAR of the first drive: 64-bit non-prefetchable, assigned at the start of the 32-bit window. Refused by the first version.
  CHECK(map(0xf0000000ull, 0x2000, &cpu) && cpu == 0xa38000000ull);
  // Anywhere inside the 32-bit window, up to its last byte.
  CHECK(map(0xf0004000ull, 0x4000, &cpu) && cpu == 0xa38004000ull);
  CHECK(map(0xf8000000ull - 0x2000, 0x2000, &cpu) && cpu == 0xa38000000ull + 0x08000000ull - 0x2000);
  // One byte over the end of the window, one byte below it, a BAR that straddles the end: refused, *cpu untouched.
  CHECK(!map(0xf8000000ull - 0x2000 + 1, 0x2000, &cpu) && cpu == 0xdeadbeef);
  CHECK(!map(0xefffe000ull, 0x2000, &cpu) && cpu == 0xdeadbeef);
  CHECK(!map(0xf8000000ull, 0x2000, &cpu));
  // The 64-bit window: the bus address is the CPU address.
  CHECK(map(0xa00000000ull, 0x2000, &cpu) && cpu == 0xa00000000ull);
  CHECK(map(0xa30000000ull - 0x2000, 0x2000, &cpu) && cpu == 0xa30000000ull - 0x2000);
  CHECK(!map(0xa30000000ull - 0x2000 + 1, 0x2000, &cpu));
  CHECK(!map(0x9ffffe000ull, 0x2000, &cpu));
  // Not assigned, empty, wrapping.
  CHECK(!map(0, 0x2000, &cpu));
  CHECK(!map(0xf0000000ull, 0, &cpu));
  CHECK(!map(~0ull - 0xfff, 0x2000, &cpu));
  // Addresses the firmware never used: 4 GiB boundary, low memory.
  CHECK(!map(0x100000000ull, 0x2000, &cpu));
  CHECK(!map(0x80000000ull, 0x2000, &cpu));

  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("test-pcie-windows: ok\n");
  return 0;
}
