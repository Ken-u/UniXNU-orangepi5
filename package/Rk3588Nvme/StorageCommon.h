// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// StorageCommon.h -- what Gpt.h, RootManifest.h and the driver share and what has nothing to do with a controller:
// the logical block size the windows are published in and the CRC-32 the GPT and the Data header use.
// From EmmcHost.h of Rk3588Emmc (Ken-u/Rk3588Emmc).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace storage {

// The block size of everything above the driver: the GPT and the root manifest count 512-byte sectors, and so does the
// IOBlockStorageDevice the windows are published as. The NVMe namespace has to be formatted with 512-byte LBAs (nvme.h).
constexpr uint32_t kBlockBytes = 512;

inline uint32_t crc32(const void *data, size_t length, uint32_t seed = 0) {
  const uint8_t *bytes = static_cast<const uint8_t *>(data);
  uint32_t crc = ~seed;
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

}  // namespace storage
