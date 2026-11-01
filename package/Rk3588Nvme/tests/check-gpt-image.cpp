// Reads the GPT of a disk image (or a device) with the driver's own reader (Gpt.h) and prints what the driver would see.
// Used to check the tables tools/board/nvme-install.py of uefi-orangepi5 writes:
//
//   g++ -std=c++17 -Wall -Wextra -Werror -O1 -I . tests/check-gpt-image.cpp -o /tmp/check-gpt-image && /tmp/check-gpt-image DISK
//
// Exit status 0 when both CRCs are good and a partition named "userdata" exists.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Gpt.h"

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: check-gpt-image DISK\n");
    return 2;
  }
  FILE *file = std::fopen(argv[1], "rb");
  if (!file) {
    std::perror(argv[1]);
    return 2;
  }
  auto read = [&](uint64_t lba, uint32_t count, uint8_t *out) {
    return ::fseeko(file, static_cast<off_t>(lba * storage::kBlockBytes), SEEK_SET) == 0 &&
           std::fread(out, storage::kBlockBytes, count, file) == count;
  };
  static uint8_t scratch[storage::kGptScratchBytes];
  storage::GptInfo info;
  storage::GptEntry entries[storage::kGptMaxEntries];
  int status = storage::readGpt(read, scratch, info, entries, storage::kGptMaxEntries);
  if (status != storage::kGptOk) {
    std::printf("readGpt failed: %d\n", status);
    return 1;
  }
  std::printf("GPT: my %llu alt %llu usable %llu..%llu entries at %llu (%u x %u), header CRC %s, entry CRC %s\n",
              static_cast<unsigned long long>(info.myLba), static_cast<unsigned long long>(info.altLba),
              static_cast<unsigned long long>(info.firstUsable), static_cast<unsigned long long>(info.lastUsable),
              static_cast<unsigned long long>(info.entriesLba), info.count, info.entrySize, info.headerCrcOk ? "ok" : "BAD",
              info.entriesCrcOk ? "ok" : "BAD");
  bool userdata = false;
  for (unsigned i = 0; i < info.used; ++i) {
    char guid[37];
    storage::formatGuid(entries[i].typeGuid, guid);
    std::printf("  slot %u %-10s %s LBA %llu..%llu\n", entries[i].slot, entries[i].name, guid,
                static_cast<unsigned long long>(entries[i].first), static_cast<unsigned long long>(entries[i].last));
    if (storage::sameName(entries[i].name, "userdata") && entries[i].last >= entries[i].first) userdata = true;
  }
  std::fclose(file);
  return info.headerCrcOk && info.entriesCrcOk && userdata ? 0 : 1;
}
