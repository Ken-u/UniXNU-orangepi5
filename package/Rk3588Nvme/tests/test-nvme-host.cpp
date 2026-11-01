// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Host-side test of ../NvmeHost.h and ../Gpt.h: the same headers the kernel driver compiles, run against a model of an NVMe
// controller.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -O1 -I . tests/test-nvme-host.cpp -o /tmp/test-nvme-host && /tmp/test-nvme-host
//
// The model enforces the facts the host depends on (see NvmeHost.h) and fails the test when the host breaks one of them:
// a controller the firmware left enabled has to be reset before its queue registers are written; a doorbell is rung only for
// a queue that exists; every address the controller is given has to lie inside the DMA window the host was given and PRP entries
// have to be page aligned; a completion is only valid with the phase bit the host expects; the host may not write anywhere
// but the range it declared. The model completes a command when the doorbell is rung, in order, one at a time.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "Gpt.h"
#include "NvmeHost.h"

namespace {

int failures = 0;
#define CHECK(condition)                                                          \
  do {                                                                            \
    if (!(condition)) {                                                           \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                 \
      ++failures;                                                                 \
    }                                                                             \
  } while (0)

constexpr uint32_t kBlock = storage::kBlockBytes;

struct Model {
  // configuration of the fake controller
  uint64_t diskBlocks = 4096;           // namespace size in 512-byte blocks
  uint32_t lbaShift = 9;
  uint32_t metadata = 0;
  uint32_t mdts = 5;                    // 2^5 pages = 128 KiB
  bool volatileCache = true;
  bool neverReady = false;              // CC.EN = 1 never sets RDY
  bool leftEnabled = false;             // the firmware's driver left the controller running
  unsigned dropCompletions = 0;         // the next N I/O commands are swallowed (timeout)
  unsigned failIo = 0;                  // the next N I/O commands complete with a media error
  bool noDevice = false;                // registers read all ones
  uint32_t doorbellStride = 0;          // CAP.DSTRD

  // registers
  uint32_t cc = 0, csts = 0, intms = 0, aqa = 0;
  uint64_t asq = 0, acq = 0;
  bool admin = false;                   // admin queue configured since the last enable

  struct Queue {
    uint64_t sq = 0, cq = 0;
    uint32_t sqEntries = 0, cqEntries = 0;
    uint32_t sqHead = 0, cqTail = 0, cqHead = 0, phase = 1;   // cqHead: the last head doorbell the host rang
    bool sqCreated = false, cqCreated = false;
  };
  Queue adminQ, ioQ;

  std::vector<uint8_t> disk;
  std::vector<std::pair<uint64_t, uint32_t>> writes;  // (lba, count) of every write command
  uint64_t windowLo = 0, windowHi = 0;
  unsigned violations = 0;
  unsigned commands = 0, ioCommands = 0, flushes = 0, resets = 0;
  uint64_t clock = 0;

  void violation(const char *what) {
    printf("MODEL VIOLATION: %s\n", what);
    ++violations;
  }

  void init() {
    disk.assign(static_cast<size_t>(diskBlocks) * kBlock, 0);
    for (size_t i = 0; i < disk.size(); ++i) disk[i] = static_cast<uint8_t>((i * 2654435761u) >> 13);
    if (leftEnabled) {
      cc = nvme::kCcEnable | (6u << 16) | (4u << 20);
      csts = nvme::kCstsReady;
      asq = 0xdead0000;
      acq = 0xbeef0000;
      aqa = 0x003f003f;
      ioQ.sqCreated = ioQ.cqCreated = true;
      admin = true;
    }
  }

  uint64_t cap() const {
    const uint64_t mqes = 1023, to = 20, css = 1;  // 10 s ready timeout
    return mqes | (to << 24) | (static_cast<uint64_t>(doorbellStride) << 32) | (css << 37);
  }

  uint32_t read32(uint32_t offset) {
    ++clock;
    if (noDevice) return ~0u;
    switch (offset) {
      case nvme::kCap: return static_cast<uint32_t>(cap());
      case nvme::kCap + 4: return static_cast<uint32_t>(cap() >> 32);
      case nvme::kVs: return 0x00010400;  // 1.4.0
      case nvme::kIntms: return intms;
      case nvme::kCc: return cc;
      case nvme::kCsts: return csts;
      case nvme::kAqa: return aqa;
      case nvme::kAsq: return static_cast<uint32_t>(asq);
      case nvme::kAsq + 4: return static_cast<uint32_t>(asq >> 32);
      case nvme::kAcq: return static_cast<uint32_t>(acq);
      case nvme::kAcq + 4: return static_cast<uint32_t>(acq >> 32);
      default: violation("read of an undefined register"); return 0;
    }
  }

  void write32(uint32_t offset, uint32_t value) {
    ++clock;
    if (offset >= nvme::kDoorbells) return doorbell(offset, value);
    const bool enabled = cc & nvme::kCcEnable;
    switch (offset) {
      case nvme::kIntms: intms |= value; break;
      case nvme::kIntmc: intms &= ~value; break;
      case nvme::kCc: {
        const bool wasEnabled = enabled, willEnable = value & nvme::kCcEnable;
        if (wasEnabled && willEnable && value != cc) violation("CC changed while enabled");
        if (wasEnabled && !willEnable) {
          csts = 0;
          admin = false;
          adminQ = Queue();
          ioQ = Queue();
          ++resets;
        } else if (!wasEnabled && willEnable) {
          if (!aqa || !asq || !acq) violation("enabled without an admin queue");
          if (((value >> 16) & 15) != 6 || ((value >> 20) & 15) != 4) violation("entry sizes");
          if ((value >> 7) & 15) violation("page size");
          adminQ = Queue();
          adminQ.sq = asq;
          adminQ.cq = acq;
          adminQ.sqEntries = (aqa & 0xfff) + 1;
          adminQ.cqEntries = ((aqa >> 16) & 0xfff) + 1;
          adminQ.sqCreated = adminQ.cqCreated = true;
          admin = true;
          if (!neverReady) csts = nvme::kCstsReady;
        }
        cc = value;
        break;
      }
      case nvme::kAqa: if (enabled) violation("AQA written while enabled"); aqa = value; break;
      case nvme::kAsq: if (enabled) violation("ASQ written while enabled"); asq = (asq & ~0xffffffffull) | value; break;
      case nvme::kAsq + 4: if (enabled) violation("ASQ written while enabled"); asq = (asq & 0xffffffffull) | (static_cast<uint64_t>(value) << 32); break;
      case nvme::kAcq: if (enabled) violation("ACQ written while enabled"); acq = (acq & ~0xffffffffull) | value; break;
      case nvme::kAcq + 4: if (enabled) violation("ACQ written while enabled"); acq = (acq & 0xffffffffull) | (static_cast<uint64_t>(value) << 32); break;
      default: violation("write of an undefined register"); break;
    }
  }

  // ------------------------------------------------------------- doorbells --
  void doorbell(uint32_t offset, uint32_t value) {
    if (!(csts & nvme::kCstsReady)) {
      violation("doorbell while not ready");
      return;
    }
    const uint32_t stride = 4u << doorbellStride;
    const uint32_t index = (offset - nvme::kDoorbells) / stride;
    if ((offset - nvme::kDoorbells) % stride) violation("doorbell offset not on the stride");
    const unsigned qid = index / 2;
    const bool completion = index & 1;
    Queue *queue = qid == 0 ? &adminQ : qid == 1 ? &ioQ : nullptr;
    if (!queue) {
      violation("doorbell of a queue that cannot exist");
      return;
    }
    if (completion) {
      if (!queue->cqCreated || value >= queue->cqEntries) violation("bad CQ head doorbell");
      else queue->cqHead = value;
      return;
    }
    if (!queue->sqCreated || value >= queue->sqEntries) {
      violation("bad SQ tail doorbell");
      return;
    }
    while (queue->sqHead != value) {
      process(qid, *queue, queue->sqHead);
      queue->sqHead = (queue->sqHead + 1) % queue->sqEntries;
    }
  }

  bool inWindow(uint64_t address, uint64_t bytes) const { return address >= windowLo && address + bytes <= windowHi; }

  uint8_t *dma(uint64_t address, uint64_t bytes) {
    if (!inWindow(address, bytes)) {
      violation("DMA outside the window");
      return nullptr;
    }
    return reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(address));
  }

  static uint32_t dw(const uint8_t *entry, unsigned index) {
    uint32_t value;
    memcpy(&value, entry + 4 * index, 4);
    return value;
  }

  void complete(Queue &queue, uint32_t commandId, uint32_t result, uint32_t status) {
    if ((queue.cqTail + 1) % queue.cqEntries == queue.cqHead) {
      // A full queue: the controller cannot post, the command never completes. It happens when the host does not ring the head doorbell.
      violation("completion queue full: the host does not ring the CQ head doorbell");
      return;
    }
    uint8_t *entry = dma(queue.cq + queue.cqTail * 16ull, 16);
    if (!entry) return;
    uint32_t words[4] = {result, 0, queue.sqHead, commandId | (queue.phase << 16) | (status << 17)};
    memcpy(entry, words, 16);
    queue.cqTail = (queue.cqTail + 1) % queue.cqEntries;
    if (!queue.cqTail) queue.phase ^= 1;
  }

  // The pages of a PRP transfer, in order. Every address must be page aligned (the host never uses an offset).
  bool pages(uint64_t prp1, uint64_t prp2, uint32_t bytes, std::vector<uint64_t> &out) {
    const uint32_t count = (bytes + 4095) / 4096;
    if (prp1 & 4095) {
      violation("PRP1 not page aligned");
      return false;
    }
    out.push_back(prp1);
    if (count == 2) {
      if (prp2 & 4095) violation("PRP2 not page aligned");
      out.push_back(prp2);
    } else if (count > 2) {
      if (prp2 & 4095) violation("PRP list not page aligned");
      const uint8_t *list = dma(prp2, 8ull * (count - 1));
      if (!list) return false;
      for (uint32_t i = 0; i + 1 < count; ++i) {
        uint64_t entry;
        memcpy(&entry, list + 8 * i, 8);
        if (entry & 4095) violation("PRP list entry not page aligned");
        out.push_back(entry);
      }
    } else if (prp2) {
      violation("PRP2 set for a one-page transfer");
    }
    return true;
  }

  void process(unsigned qid, Queue &queue, uint32_t slot) {
    const uint8_t *entry = dma(queue.sq + slot * 64ull, 64);
    if (!entry) return;
    ++commands;
    const uint32_t d0 = dw(entry, 0);
    const uint8_t opcode = d0 & 0xff;
    const uint32_t id = d0 >> 16;
    const uint32_t nsid = dw(entry, 1);
    uint64_t prp1, prp2;
    memcpy(&prp1, entry + 24, 8);
    memcpy(&prp2, entry + 32, 8);
    const uint32_t d10 = dw(entry, 10), d11 = dw(entry, 11), d12 = dw(entry, 12);
    if ((d0 >> 8) & 0xff) violation("fuse or PSDT set");
    if (qid == 0) return processAdmin(queue, opcode, id, nsid, prp1, d10, d11);
    ++ioCommands;
    if (!ioQ.cqCreated || !ioQ.sqCreated) {
      violation("I/O command before the queue pair exists");
      return;
    }
    if (dropCompletions) {
      --dropCompletions;
      return;
    }
    if (nsid != 1) {
      complete(ioQ, id, 0, (0u << 8) | 0x0b);  // invalid namespace
      return;
    }
    if (failIo) {
      --failIo;
      complete(ioQ, id, 0, (2u << 8) | 0x81);  // media error: unrecovered read error
      return;
    }
    switch (opcode) {
      case nvme::kNvmFlush:
        ++flushes;
        complete(ioQ, id, 0, 0);
        return;
      case nvme::kNvmRead:
      case nvme::kNvmWrite: {
        const uint64_t lba = d10 | (static_cast<uint64_t>(d11) << 32);
        const uint32_t count = (d12 & 0xffff) + 1;
        if (d12 >> 16) violation("flags in CDW12");
        if (count * kBlock > (4096u << mdts)) violation("transfer larger than MDTS");
        if (lba + count > diskBlocks) {
          complete(ioQ, id, 0, (0u << 8) | 0x80);  // LBA out of range
          return;
        }
        std::vector<uint64_t> list;
        if (!pages(prp1, prp2, count * kBlock, list)) return;
        for (uint32_t i = 0; i < list.size(); ++i) {
          const uint32_t chunk = count * kBlock - i * 4096 < 4096 ? count * kBlock - i * 4096 : 4096;
          uint8_t *memory = dma(list[i], chunk);
          if (!memory) return;
          uint8_t *media = disk.data() + lba * kBlock + i * 4096ull;
          if (opcode == nvme::kNvmRead) memcpy(memory, media, chunk);
          else memcpy(media, memory, chunk);
        }
        if (opcode == nvme::kNvmWrite) writes.push_back({lba, count});
        complete(ioQ, id, 0, 0);
        return;
      }
      default: complete(ioQ, id, 0, 0x01);  // invalid opcode
    }
  }

  void put(uint8_t *page, unsigned at, const char *text, unsigned width) {
    memset(page + at, ' ', width);
    memcpy(page + at, text, strlen(text) < width ? strlen(text) : width);
  }

  void processAdmin(Queue &queue, uint8_t opcode, uint32_t id, uint32_t nsid, uint64_t prp1, uint32_t d10, uint32_t d11) {
    switch (opcode) {
      case nvme::kAdminIdentify: {
        uint8_t *page = dma(prp1, 4096);
        if (!page) return;
        memset(page, 0, 4096);
        const uint32_t cns = d10 & 0xff;
        if (cns == 1) {
          put(page, 4, "MODEL0001", 20);
          put(page, 24, "Model NVMe 128GB", 40);
          put(page, 64, "1.0", 8);
          page[77] = static_cast<uint8_t>(mdts);
          const uint32_t namespaces = 1;
          memcpy(page + 516, &namespaces, 4);
          const uint16_t oncs = 0x1f;
          memcpy(page + 520, &oncs, 2);
          page[525] = volatileCache ? 1 : 0;
        } else if (cns == 2) {
          const uint32_t first = nsid == 0 ? 1 : 0;
          memcpy(page, &first, 4);
        } else if (cns == 0) {
          if (nsid != 1) {
            complete(queue, id, 0, 0x0b);
            return;
          }
          const uint64_t size = (diskBlocks * kBlock) >> lbaShift;
          memcpy(page, &size, 8);
          memcpy(page + 8, &size, 8);
          page[25] = 1;   // two formats
          page[26] = 0;   // format 0 in use
          // format 0: the configured one; format 1: the other size
          page[128 + 0] = static_cast<uint8_t>(metadata);
          page[128 + 2] = static_cast<uint8_t>(lbaShift);
          page[128 + 4 + 2] = lbaShift == 9 ? 12 : 9;
        } else {
          complete(queue, id, 0, 0x02);
          return;
        }
        complete(queue, id, 0, 0);
        return;
      }
      case nvme::kAdminSetFeatures: {
        if ((d10 & 0xff) != 0x07) {
          complete(queue, id, 0, 0x02);
          return;
        }
        if (d11 != 0) violation("asked for more than one queue pair");
        complete(queue, id, 0, 0);  // dword 0: one of each
        return;
      }
      case nvme::kAdminCreateCq: {
        const uint32_t qid = d10 & 0xffff, entries = (d10 >> 16) + 1;
        if (qid != 1) violation("created a CQ other than 1");
        if (!(d11 & 1)) violation("CQ not physically contiguous");
        if (d11 & 2) violation("CQ with interrupts enabled");
        if (prp1 & 4095) violation("CQ not page aligned");
        dma(prp1, entries * 16ull);
        ioQ.cq = prp1;
        ioQ.cqEntries = entries;
        ioQ.cqTail = 0;
        ioQ.cqHead = 0;
        ioQ.phase = 1;
        ioQ.cqCreated = true;
        complete(queue, id, 0, 0);
        return;
      }
      case nvme::kAdminCreateSq: {
        const uint32_t qid = d10 & 0xffff, entries = (d10 >> 16) + 1;
        if (qid != 1) violation("created an SQ other than 1");
        if (!ioQ.cqCreated || (d11 >> 16) != 1) violation("SQ before its CQ or bound to the wrong one");
        if (!(d11 & 1)) violation("SQ not physically contiguous");
        if (prp1 & 4095) violation("SQ not page aligned");
        dma(prp1, entries * 64ull);
        ioQ.sq = prp1;
        ioQ.sqEntries = entries;
        ioQ.sqHead = 0;
        ioQ.sqCreated = true;
        complete(queue, id, 0, 0);
        return;
      }
      default:
        complete(queue, id, 0, 0x01);
    }
  }
};

Model *gModel = nullptr;

uint32_t platRead(void *, uint32_t offset) { return gModel->read32(offset); }
void platWrite(void *, uint32_t offset, uint32_t value) { gModel->write32(offset, value); }
uint64_t platNow() { return ++gModel->clock; }
void platDelay(uint32_t us) { gModel->clock += us; }
bool gQuiet = !getenv("NVME_TEST_VERBOSE");
void platLog(const char *format, ...) __attribute__((format(printf, 1, 2)));
void platLog(const char *format, ...) {
  if (gQuiet) return;
  va_list args;
  va_start(args, format);
  vprintf(format, args);
  va_end(args);
}
// The cache of a CPU that is not coherent with the controller: the host works on `cpu`, the controller (the model) on `dram`,
// and the two meet only in flush(), which is clean and invalidate per 64-byte line: a line the CPU changed since it last synced
// (cpu differs from `shadow`) is written to DRAM, then the CPU view of every line in the range is reloaded from DRAM. A host
// that forgets a flush sees stale completions, or sends stale commands and data, or lets the model read a queue that is still
// only in the CPU's cache.
constexpr size_t kLine = 64;
uint8_t *gCpu = nullptr;
uint8_t *gDram = nullptr;
uint8_t *gShadow = nullptr;
unsigned gFlushes = 0;
void platFlush(void *virt, size_t bytes) {
  ++gFlushes;
  uint8_t *at = static_cast<uint8_t *>(virt);
  if (at < gCpu || at + bytes > gCpu + nvme::kWindowBytes) {
    printf("MODEL VIOLATION: flush outside the window\n");
    ++gModel->violations;
    return;
  }
  const size_t first = static_cast<size_t>(at - gCpu) / kLine * kLine;
  const size_t end = (static_cast<size_t>(at - gCpu) + bytes + kLine - 1) / kLine * kLine;
  for (size_t line = first; line < end; line += kLine) {
    if (memcmp(gCpu + line, gShadow + line, kLine)) memcpy(gDram + line, gCpu + line, kLine);  // dirty: write back
    memcpy(gCpu + line, gDram + line, kLine);                                                // invalidate: reload
    memcpy(gShadow + line, gCpu + line, kLine);
  }
}

const nvme::Platform kPlatform = {platRead, platWrite, platNow, platDelay, platLog};

struct Fixture {
  Model model;
  nvme::Host host;
  nvme::DmaWindow window;
  uint8_t *cpu = nullptr;   // what the host sees of the window
  uint8_t *dram = nullptr;  // what the controller sees
  uint8_t *shadow = nullptr;

  explicit Fixture(void (*configure)(Model &) = nullptr) {
    if (configure) configure(model);
    model.init();
    cpu = static_cast<uint8_t *>(aligned_alloc(4096, nvme::kWindowBytes));
    dram = static_cast<uint8_t *>(aligned_alloc(4096, nvme::kWindowBytes));
    memset(cpu, 0xa5, nvme::kWindowBytes);  // junk, and different junk: init() must not depend on a clean window
    memset(dram, 0x5a, nvme::kWindowBytes);
    shadow = static_cast<uint8_t *>(malloc(nvme::kWindowBytes));
    memcpy(shadow, cpu, nvme::kWindowBytes);
    gCpu = cpu;
    gDram = dram;
    gShadow = shadow;
    window = {cpu, reinterpret_cast<uintptr_t>(dram), platFlush};
    model.windowLo = window.bus;
    model.windowHi = window.bus + nvme::kWindowBytes;
    gModel = &model;
    host.attach(&kPlatform, &model);  // the base pointer is opaque to the host; the model ignores it
    host.setDma(&window);
  }
  ~Fixture() {
    free(cpu);
    free(dram);
    free(shadow);
  }
};

void testInit() {
  Fixture f;
  CHECK(f.host.init() == nvme::kOk);
  CHECK(f.model.violations == 0);
  CHECK(f.host.ready());
  CHECK(f.host.blocks() == 4096);
  CHECK(!strcmp(f.host.id.model, "Model NVMe 128GB"));
  CHECK(!strcmp(f.host.id.serial, "MODEL0001"));
  CHECK(!strcmp(f.host.id.firmware, "1.0"));
  CHECK(f.host.id.nsid == 1);
  CHECK(f.host.id.lbaBytes == 512);
  CHECK(f.host.id.mdtsBlocks == 256);  // 2^5 pages of 4 KiB
  CHECK(f.host.id.volatileWriteCache);
  CHECK(f.model.intms == 0xffffffffu);
  CHECK(f.model.ioQ.sqCreated && f.model.ioQ.cqCreated);
}

void testLeftEnabled() {
  // The firmware's driver left the controller running with queues the kernel may have reused.
  Fixture f([](Model &m) { m.leftEnabled = true; });
  CHECK(f.host.init() == nvme::kOk);
  CHECK(f.model.violations == 0);
  CHECK(f.model.resets == 1);
}

void testNoDevice() {
  Fixture f([](Model &m) { m.noDevice = true; });
  CHECK(f.host.init() == nvme::kNoDevice);
  CHECK(!f.host.ready());
  char buffer[512];
  CHECK(f.host.readBlocks(0, 1, buffer) == nvme::kBadState);
}

void testNeverReady() {
  Fixture f([](Model &m) { m.neverReady = true; });
  CHECK(f.host.init() == nvme::kTimeout);
  CHECK(!f.host.ready());
}

void testFourK() {
  Fixture f([](Model &m) { m.lbaShift = 12; m.diskBlocks = 4096; });
  CHECK(f.host.init() == nvme::kUnsupported);
  CHECK(!f.host.ready());
}

void testMetadata() {
  Fixture f([](Model &m) { m.metadata = 8; });
  CHECK(f.host.init() == nvme::kUnsupported);
}

void testReads() {
  Fixture f;
  CHECK(f.host.init() == nvme::kOk);
  std::vector<uint8_t> out(static_cast<size_t>(1000) * kBlock);
  // one block
  CHECK(f.host.readBlocks(7, 1, out.data()) == nvme::kOk);
  CHECK(!memcmp(out.data(), f.model.disk.data() + 7 * kBlock, kBlock));
  // two pages, an odd count, several runs with a PRP list; crosses the queue wrap and the phase flip many times
  for (uint32_t start : {0u, 1u, 15u, 255u, 256u, 257u, 1000u, 2000u}) {
    for (uint32_t count : {1u, 3u, 8u, 9u, 16u, 255u, 256u, 257u, 700u}) {
      if (start + count > f.model.diskBlocks || count > 1000) continue;
      memset(out.data(), 0, out.size());
      CHECK(f.host.readBlocks(start, count, out.data()) == nvme::kOk);
      CHECK(!memcmp(out.data(), f.model.disk.data() + static_cast<size_t>(start) * kBlock, static_cast<size_t>(count) * kBlock));
    }
  }
  // the whole disk, in an odd chunk size, compared by CRC
  std::vector<uint8_t> all(static_cast<size_t>(f.model.diskBlocks) * kBlock);
  for (uint32_t at = 0; at < f.model.diskBlocks; at += 333) {
    uint32_t n = f.model.diskBlocks - at < 333 ? f.model.diskBlocks - at : 333;
    CHECK(f.host.readBlocks(at, n, all.data() + static_cast<size_t>(at) * kBlock) == nvme::kOk);
  }
  CHECK(storage::crc32(all.data(), all.size()) == storage::crc32(f.model.disk.data(), f.model.disk.size()));
  CHECK(f.model.violations == 0);
  CHECK(f.host.stats.failures == 0);
  CHECK(f.host.stats.commands > 100);
  CHECK(f.model.ioCommands > 4 * nvme::kQueueEntries);  // the queues wrapped, and the phase bit flipped, several times
  // ... and nothing was papered over by a retry or a reset (a host that mis-tracks the phase bit times out, recovers and "works")
  CHECK(f.host.stats.timeouts == 0 && f.host.stats.retries == 0 && f.host.stats.recoveries == 0);
  CHECK(f.model.resets == 0);
  CHECK(f.model.writes.empty());
}

void testReadOutside() {
  Fixture f;
  CHECK(f.host.init() == nvme::kOk);
  uint8_t buffer[kBlock * 2];
  CHECK(f.host.readBlocks(4095, 2, buffer) == nvme::kRefused);
  CHECK(f.host.readBlocks(4096, 1, buffer) == nvme::kRefused);
  CHECK(f.host.readBlocks(~0ull, 1, buffer) == nvme::kRefused);
  CHECK(f.host.readBlocks(4095, 1, buffer) == nvme::kOk);
  CHECK(f.host.readBlocks(0, 0, buffer) == nvme::kOk);
  CHECK(f.model.violations == 0);
}

void testSmallMdts() {
  // A controller that takes 8 KiB per command: the host has to split.
  Fixture f([](Model &m) { m.mdts = 1; });
  CHECK(f.host.init() == nvme::kOk);
  CHECK(f.host.id.mdtsBlocks == 16);
  std::vector<uint8_t> out(static_cast<size_t>(100) * kBlock);
  CHECK(f.host.readBlocks(33, 100, out.data()) == nvme::kOk);
  CHECK(!memcmp(out.data(), f.model.disk.data() + 33 * kBlock, out.size()));
  CHECK(f.model.violations == 0);
}

void testWrites() {
  Fixture f;
  CHECK(f.host.init() == nvme::kOk);
  std::vector<uint8_t> data(static_cast<size_t>(600) * kBlock);
  for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 7 + 1);
  // no range declared: every write is refused, and nothing reaches the media
  const std::vector<uint8_t> before = f.model.disk;
  CHECK(!f.host.writable());
  CHECK(f.host.writeBlocks(100, 1, data.data()) == nvme::kRefused);
  CHECK(f.model.disk == before);
  f.host.setWriteRange(1000, 2999);
  CHECK(f.host.writable());
  CHECK(f.host.writeBlocks(999, 1, data.data()) == nvme::kRefused);
  CHECK(f.host.writeBlocks(3000, 1, data.data()) == nvme::kRefused);
  CHECK(f.host.writeBlocks(2999, 2, data.data()) == nvme::kRefused);
  CHECK(f.host.writeBlocks(900, 200, data.data()) == nvme::kRefused);  // straddles the start
  CHECK(f.host.writeBlocks(~0ull, 1, data.data()) == nvme::kRefused);
  CHECK(f.model.disk == before);
  CHECK(f.model.writes.empty());
  // inside: several runs, then read back
  CHECK(f.host.writeBlocks(1000, 600, data.data()) == nvme::kOk);
  CHECK(!memcmp(f.model.disk.data() + 1000 * kBlock, data.data(), data.size()));
  CHECK(f.host.flushCache() == nvme::kOk);
  CHECK(f.model.flushes == 1);
  std::vector<uint8_t> back(data.size());
  CHECK(f.host.readBlocks(1000, 600, back.data()) == nvme::kOk);
  CHECK(back == data);
  // the last block of the range
  CHECK(f.host.writeBlocks(2999, 1, data.data()) == nvme::kOk);
  // nothing outside the range changed
  for (uint64_t lba = 0; lba < f.model.diskBlocks; ++lba) {
    if (lba >= 1000 && lba <= 2999) continue;
    CHECK(!memcmp(f.model.disk.data() + lba * kBlock, before.data() + lba * kBlock, kBlock));
  }
  for (auto &write : f.model.writes) CHECK(write.first >= 1000 && write.first + write.second - 1 <= 2999);
  CHECK(f.host.stats.timeouts == 0 && f.host.stats.retries == 0 && f.host.stats.recoveries == 0 && f.model.resets == 0);
  f.host.clearWriteRange();
  CHECK(!f.host.writable());
  CHECK(f.host.writeBlocks(1000, 1, data.data()) == nvme::kRefused);
  CHECK(f.model.violations == 0);
}

void testFlushWithoutCache() {
  Fixture f([](Model &m) { m.volatileCache = false; });
  CHECK(f.host.init() == nvme::kOk);
  CHECK(f.host.flushCache() == nvme::kOk);
  CHECK(f.model.flushes == 0);  // no volatile cache: nothing to send
}

void testErrorStatus() {
  Fixture f([](Model &m) { m.failIo = 1; });
  CHECK(f.host.init() == nvme::kOk);
  std::vector<uint8_t> out(static_cast<size_t>(8) * kBlock);
  // one error, then the retry succeeds
  CHECK(f.host.readBlocks(0, 8, out.data()) == nvme::kOk);
  CHECK(f.host.stats.retries == 1);
  CHECK(!memcmp(out.data(), f.model.disk.data(), out.size()));
  f.model.failIo = 100;
  CHECK(f.host.readBlocks(0, 8, out.data()) == nvme::kIoError);
  CHECK(f.host.stats.failures == 1);
  CHECK(f.host.ready());
  f.model.failIo = 0;
  CHECK(f.host.readBlocks(0, 8, out.data()) == nvme::kOk);
  CHECK(f.host.stats.timeouts == 0 && f.model.resets == 0);  // an error status is not a reason to reset the controller
}

void testTimeoutRecovery() {
  Fixture f;
  CHECK(f.host.init() == nvme::kOk);
  std::vector<uint8_t> out(static_cast<size_t>(8) * kBlock);
  f.model.dropCompletions = 1;
  CHECK(f.host.readBlocks(40, 8, out.data()) == nvme::kOk);  // the first try times out, the controller is reset, the second works
  CHECK(!memcmp(out.data(), f.model.disk.data() + 40 * kBlock, out.size()));
  CHECK(f.host.stats.timeouts == 1);
  CHECK(f.host.stats.recoveries == 1);
  CHECK(f.model.resets >= 1);
  CHECK(f.model.violations == 0);
  CHECK(f.host.ready());
  // a controller that never answers: the read fails, nothing hangs
  f.model.dropCompletions = 1000;
  CHECK(f.host.readBlocks(40, 8, out.data()) == nvme::kTimeout);
  CHECK(f.host.stats.failures == 1);
}

void testWriteRecovery() {
  Fixture f;
  CHECK(f.host.init() == nvme::kOk);
  f.host.setWriteRange(0, 4095);
  std::vector<uint8_t> data(static_cast<size_t>(64) * kBlock, 0x5a);
  f.model.dropCompletions = 1;
  CHECK(f.host.writeBlocks(10, 64, data.data()) == nvme::kOk);
  CHECK(!memcmp(f.model.disk.data() + 10 * kBlock, data.data(), data.size()));
  CHECK(f.host.stats.recoveries == 1);
}

// ------------------------------------------------------------------- GPT ----
void put32(uint8_t *at, uint32_t value) { memcpy(at, &value, 4); }
void put64(uint8_t *at, uint64_t value) { memcpy(at, &value, 8); }

void makeGpt(Model &m) {
  uint8_t *disk = m.disk.data();
  const uint64_t total = m.diskBlocks;
  memset(disk, 0, 34 * kBlock);
  // protective MBR
  disk[510] = 0x55;
  disk[511] = 0xaa;
  // entries at LBA 2: ESP and userdata
  uint8_t *entries = disk + 2 * kBlock;
  static const uint8_t espType[16] = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
  static const uint8_t dataType[16] = {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47, 0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4};
  struct Part { const uint8_t *type; uint64_t first, last; const char *name; };
  const Part parts[] = {{espType, 2048, 2048 + 512 - 1, "ESP"}, {dataType, 2048 + 512, total - 64, "userdata"}};
  for (unsigned i = 0; i < 2; ++i) {
    uint8_t *e = entries + i * 128;
    memcpy(e, parts[i].type, 16);
    e[16] = static_cast<uint8_t>(0x10 + i);
    put64(e + 32, parts[i].first);
    put64(e + 40, parts[i].last);
    for (unsigned c = 0; parts[i].name[c]; ++c) e[56 + 2 * c] = static_cast<uint8_t>(parts[i].name[c]);
  }
  const uint32_t entriesCrc = storage::crc32(entries, 128 * 128);
  uint8_t *header = disk + kBlock;
  memcpy(header, "EFI PART", 8);
  put32(header + 8, 0x00010000);
  put32(header + 12, 92);
  put64(header + 24, 1);
  put64(header + 32, total - 1);
  put64(header + 40, 34);
  put64(header + 48, total - 34);
  header[56] = 0x42;
  put64(header + 72, 2);
  put32(header + 80, 128);
  put32(header + 84, 128);
  put32(header + 88, entriesCrc);
  put32(header + 16, 0);
  put32(header + 16, storage::crc32(header, 92));
}

void testGptThroughTheHost() {
  Fixture f;
  makeGpt(f.model);
  CHECK(f.host.init() == nvme::kOk);
  uint8_t scratch[storage::kGptScratchBytes];
  storage::GptEntry entries[storage::kGptMaxEntries];
  storage::GptInfo info;
  const int status = storage::readGpt(
      [&](uint64_t lba, uint32_t count, void *out) { return f.host.readBlocks(lba, count, out) == nvme::kOk; }, scratch, info, entries,
      storage::kGptMaxEntries);
  CHECK(status == storage::kGptOk);
  CHECK(info.headerCrcOk && info.entriesCrcOk);
  CHECK(info.used == 2);
  CHECK(info.altLba + 1 == f.model.diskBlocks);
  CHECK(storage::sameName(entries[0].name, "ESP") && entries[0].first == 2048);
  CHECK(storage::sameName(entries[1].name, "userdata") && entries[1].last == f.model.diskBlocks - 64);
}

}  // namespace

int main() {
  testInit();
  testLeftEnabled();
  testNoDevice();
  testNeverReady();
  testFourK();
  testMetadata();
  testReads();
  testReadOutside();
  testSmallMdts();
  testWrites();
  testFlushWithoutCache();
  testErrorStatus();
  testTimeoutRecovery();
  testWriteRecovery();
  testGptThroughTheHost();
  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("all NVMe host checks passed\n");
  return 0;
}
