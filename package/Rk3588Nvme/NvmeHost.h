// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// NvmeHost.h -- a polled NVMe host: one admin queue, one I/O queue pair, one command in flight, PRP transfers through a
// bounce window. Pure functions of a register file and a DMA window, no kernel dependency: tests/test-nvme-host.cpp runs
// this very header against a model of an NVMe controller.
//
// What the host assumes, and why (NVMe 1.4 base specification; none of it has been observed on an Orange Pi 5 yet):
//
//   * The PCIe link is up and the controller's BAR0 is assigned and mapped. The firmware (edk2-rk3588) did that for
//     its own NvmExpressDxe; the driver only enables memory space and bus mastering (NativeNvme.cpp).
//   * The firmware's driver left the controller enabled with its queues in memory the kernel may have reused, and it does not shut
//     the controller down at ExitBootServices. So init() starts from a controller reset (CC.EN 1 -> 0, wait for CSTS.RDY = 0)
//     and sets everything up again; it never trusts a queue pointer it did not write itself.
//   * Interrupts are not used: pin-based interrupts are masked (INTMS), the I/O completion queue is created without interrupt
//     enable, completions are found by their phase bit. No MSI/MSI-X is touched.
//   * 4 KiB controller pages (CC.MPS = 0, CAP.MPSMIN = 0), 64-byte submission and 16-byte completion entries, the NVM command set.
//   * The namespace has 512-byte logical blocks and no metadata: the GPT and the root image count 512-byte sectors. A drive
//     formatted with 4 KiB blocks is refused with a message that says how to fix it (`nvme format -l`).
//   * The PCIe controller does not snoop the CPU's caches (the RK3588's DMA is not cache coherent), so the window is cleaned and
//     dropped (DmaWindow::flush, dc civac) before the controller reads or writes it and again before the CPU reads what it wrote.
//   * One command at a time per queue (queue depth 1 in effect): a read of up to kMaxRunBlocks blocks goes into the window, the CPU
//     copies it out. Throughput is the point of nothing here; correctness of the root volume read is.
//
// Writes exist for exactly one reason, the persistent Data volume. writeBlocks() refuses every block outside the range
// the driver declared with setWriteRange(), whatever the caller asks for.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "StorageCommon.h"

namespace nvme {

// ------------------------------------------------------------------ results --
enum Result : int {
  kOk = 0,
  kTimeout = -1,      // the controller did not answer in time
  kIoError = -2,      // the controller completed the command with an error status
  kNoDevice = -3,     // the registers read all ones: no link, no endpoint
  kUnsupported = -4,  // the controller or namespace does something this host does not do
  kRefused = -5,      // a write outside the declared range, or a read outside the namespace
  kBadState = -6,     // used before init(), or after a failed one
};

inline const char *resultName(int result) {
  switch (result) {
    case kOk: return "ok";
    case kTimeout: return "timeout";
    case kIoError: return "error status";
    case kNoDevice: return "no device (registers read all ones)";
    case kUnsupported: return "unsupported";
    case kRefused: return "refused";
    case kBadState: return "not initialised";
    default: return "?";
  }
}

// ---------------------------------------------------------------- registers --
constexpr uint32_t kCap = 0x00;    // 64 bit
constexpr uint32_t kVs = 0x08;
constexpr uint32_t kIntms = 0x0c;
constexpr uint32_t kIntmc = 0x10;
constexpr uint32_t kCc = 0x14;
constexpr uint32_t kCsts = 0x1c;
constexpr uint32_t kAqa = 0x24;
constexpr uint32_t kAsq = 0x28;    // 64 bit
constexpr uint32_t kAcq = 0x30;    // 64 bit
constexpr uint32_t kDoorbells = 0x1000;

constexpr uint32_t kCcEnable = 1u << 0;
constexpr uint32_t kCstsReady = 1u << 0;
constexpr uint32_t kCstsFatal = 1u << 1;

// Admin opcodes
constexpr uint8_t kAdminCreateSq = 0x01;
constexpr uint8_t kAdminCreateCq = 0x05;
constexpr uint8_t kAdminIdentify = 0x06;
constexpr uint8_t kAdminSetFeatures = 0x09;
// NVM opcodes
constexpr uint8_t kNvmFlush = 0x00;
constexpr uint8_t kNvmWrite = 0x01;
constexpr uint8_t kNvmRead = 0x02;

// ------------------------------------------------------------- platform/DMA --
// Same shape as emmc::Platform: the kernel driver supplies the register accessors (volatile accesses and a dsb after a write),
// the clock and the log; the test supplies a model.
struct Platform {
  uint32_t (*read32)(void *base, uint32_t offset);
  void (*write32)(void *base, uint32_t offset, uint32_t value);
  uint64_t (*nowUs)();
  void (*delayUs)(uint32_t microseconds);
  void (*log)(const char *format, ...) __attribute__((format(printf, 1, 2)));
};

constexpr uint32_t kPage = 4096;
constexpr uint32_t kQueueEntries = 16;                       // both queues; queue depth 1 is used
constexpr uint32_t kMaxRunBlocks = 256;                      // one command: 128 KiB
constexpr uint32_t kMaxRunBytes = kMaxRunBlocks * storage::kBlockBytes;
constexpr uint32_t kMaxRunPages = kMaxRunBytes / kPage;

// One physically contiguous, 4 KiB aligned, cacheable run of memory, laid out as below. `bus` is the address the controller uses.
constexpr uint32_t kOffAdminSq = 0x0000;
constexpr uint32_t kOffAdminCq = 0x1000;
constexpr uint32_t kOffIoSq = 0x2000;
constexpr uint32_t kOffIoCq = 0x3000;
constexpr uint32_t kOffPrpList = 0x4000;
constexpr uint32_t kOffIdentify = 0x5000;
constexpr uint32_t kOffData = 0x6000;
constexpr uint32_t kWindowBytes = kOffData + kMaxRunBytes;

struct DmaWindow {
  uint8_t *virt;
  uint64_t bus;
  void (*flush)(void *virt, size_t bytes);  // clean and invalidate, the whole range, caches only
};

struct Stats {
  uint64_t blocks = 0;        // blocks read
  uint64_t commands = 0;      // commands completed, admin and I/O
  uint64_t retries = 0;
  uint64_t recoveries = 0;    // controller resets after a timeout
  uint64_t failures = 0;      // commands that failed for good
  uint64_t timeouts = 0;
  uint64_t readUs = 0;
  uint64_t writeBlocks = 0;
  uint64_t writeUs = 0;
};

// What identify said, for the log and for the driver's decisions.
struct Identity {
  char serial[21];
  char model[41];
  char firmware[9];
  uint32_t nsid = 0;
  uint64_t blocks = 0;        // namespace size in 512-byte blocks
  uint32_t lbaBytes = 0;
  uint32_t mdtsBlocks = 0;    // largest transfer the controller takes, in blocks (0 = no limit)
  uint32_t namespaces = 0;
  uint16_t oncs = 0;
  bool volatileWriteCache = false;
  uint32_t version = 0;
};

class Host {
 public:
  Stats stats;
  Identity id;

  void attach(const Platform *platform, void *registers) {
    plat_ = platform;
    base_ = registers;
  }
  void setDma(const DmaWindow *window) { dma_ = window; }
  bool ready() const { return ready_; }
  uint64_t blocks() const { return id.blocks; }

  // Writes are possible only inside [first, last].
  void setWriteRange(uint64_t first, uint64_t last) {
    writeFirst_ = first;
    writeLast_ = last;
    writeOpen_ = last >= first;
  }
  void clearWriteRange() { writeOpen_ = false; }
  bool writable() const { return writeOpen_ && ready_; }

  void dump(const char *label) const {
    if (!plat_ || !base_) return;
    plat_->log("NativeNvme: %s: CAP %#llx VS %#x CC %#x CSTS %#x AQA %#x ASQ %#llx ACQ %#llx INTMS %#x\n", label,
               static_cast<unsigned long long>(read64(kCap)), rd(kVs), rd(kCc), rd(kCsts), rd(kAqa),
               static_cast<unsigned long long>(read64(kAsq)), static_cast<unsigned long long>(read64(kAcq)), rd(kIntms));
  }

  // Reset the controller, set up the admin queue, identify the controller and namespace, create the I/O queue pair.
  int init() {
    ready_ = false;
    if (!plat_ || !base_ || !dma_ || !dma_->virt || (dma_->bus & (kPage - 1))) return kBadState;
    const uint64_t cap = read64(kCap);
    if (cap == ~0ull || rd(kVs) == ~0u) return kNoDevice;
    const uint32_t version = rd(kVs);
    id.version = version;
    const uint32_t mqes = static_cast<uint32_t>(cap & 0xffff);
    const uint32_t timeoutUnits = static_cast<uint32_t>((cap >> 24) & 0xff);
    stride_ = 4u << ((cap >> 32) & 0xf);
    const bool nvmCommandSet = (cap >> 37) & 1;
    const uint32_t mpsMin = static_cast<uint32_t>((cap >> 48) & 0xf), mpsMax = static_cast<uint32_t>((cap >> 52) & 0xf);
    // CAP.TO counts 500 ms; a controller that says 0 gets one unit anyway.
    readyTimeoutUs_ = static_cast<uint64_t>(timeoutUnits ? timeoutUnits : 1) * 500000ull;
    plat_->log("NativeNvme: NVMe %u.%u.%u, %u queue entries at most, doorbell stride %u, ready timeout %llu ms, pages %u..%u KiB, command sets %#x\n",
               version >> 16, (version >> 8) & 0xff, version & 0xff, mqes + 1, stride_, static_cast<unsigned long long>(readyTimeoutUs_ / 1000),
               4u << mpsMin, 4u << mpsMax, static_cast<unsigned>((cap >> 37) & 0xff));
    if (!nvmCommandSet || mpsMin != 0 || mqes + 1 < kQueueEntries) {
      plat_->log("NativeNvme: the controller lacks the NVM command set, 4 KiB pages or %u queue entries\n", kQueueEntries);
      return kUnsupported;
    }
    int result = resetController();
    if (result != kOk) return result;
    result = enableController();
    if (result != kOk) return result;
    result = identify();
    if (result != kOk) return result;
    result = createIoQueues();
    if (result != kOk) return result;
    ready_ = true;
    return kOk;
  }

  int readBlocks(uint64_t lba, uint32_t count, void *out) {
    if (!ready_) return kBadState;
    if (!count) return kOk;
    if (lba >= id.blocks || count > id.blocks - lba) return kRefused;
    uint8_t *bytes = static_cast<uint8_t *>(out);
    const uint64_t begin = plat_->nowUs();
    for (uint32_t done = 0; done < count;) {
      uint32_t run = count - done < maxRun() ? count - done : maxRun();
      int result = transfer(kNvmRead, lba + done, run);
      for (int attempt = 0; result != kOk && attempt < 2; ++attempt) {
        ++stats.retries;
        if (result == kTimeout && recover() != kOk) break;
        result = transfer(kNvmRead, lba + done, run);
      }
      if (result != kOk) {
        ++stats.failures;
        stats.readUs += plat_->nowUs() - begin;
        return result;
      }
      dma_->flush(data(), static_cast<size_t>(run) * storage::kBlockBytes);  // what the CPU read of the window before the DMA is stale
      __builtin_memcpy(bytes + static_cast<size_t>(done) * storage::kBlockBytes, data(), static_cast<size_t>(run) * storage::kBlockBytes);
      stats.blocks += run;
      done += run;
    }
    stats.readUs += plat_->nowUs() - begin;
    return kOk;
  }

  int writeBlocks(uint64_t lba, uint32_t count, const void *in) {
    if (!ready_) return kBadState;
    if (!count) return kOk;
    if (!writeOpen_ || lba < writeFirst_ || lba > writeLast_ || count > writeLast_ - lba + 1 || lba >= id.blocks || count > id.blocks - lba) {
      return kRefused;
    }
    const uint8_t *bytes = static_cast<const uint8_t *>(in);
    const uint64_t begin = plat_->nowUs();
    for (uint32_t done = 0; done < count;) {
      uint32_t run = count - done < maxRun() ? count - done : maxRun();
      __builtin_memcpy(data(), bytes + static_cast<size_t>(done) * storage::kBlockBytes, static_cast<size_t>(run) * storage::kBlockBytes);
      int result = transfer(kNvmWrite, lba + done, run);
      for (int attempt = 0; result != kOk && attempt < 2; ++attempt) {
        ++stats.retries;
        if (result == kTimeout && recover() != kOk) break;
        // The window still holds this run (recover() does not touch the data area), but clean it again anyway.
        result = transfer(kNvmWrite, lba + done, run);
      }
      if (result != kOk) {
        ++stats.failures;
        stats.writeUs += plat_->nowUs() - begin;
        return result;
      }
      stats.writeBlocks += run;
      done += run;
    }
    stats.writeUs += plat_->nowUs() - begin;
    return kOk;
  }

  // What the drive holds in a volatile write cache goes to the media (a no-op on a drive without one).
  int flushCache() {
    if (!ready_) return kBadState;
    if (!id.volatileWriteCache) return kOk;
    Command command = {};
    command.opcode = kNvmFlush;
    command.nsid = id.nsid;
    return submit(1, command, nullptr, 10000000);
  }

 private:
  struct Command {
    uint8_t opcode = 0;
    uint32_t nsid = 0;
    uint64_t prp1 = 0, prp2 = 0;
    uint32_t cdw10 = 0, cdw11 = 0, cdw12 = 0;
  };

  struct Queue {
    uint32_t sqTail = 0, cqHead = 0;
    uint32_t phase = 1;
    uint16_t nextId = 1;
  };

  const Platform *plat_ = nullptr;
  void *base_ = nullptr;
  const DmaWindow *dma_ = nullptr;
  bool ready_ = false;
  uint32_t stride_ = 4;
  uint64_t readyTimeoutUs_ = 500000;
  Queue queue_[2];  // 0 admin, 1 I/O
  bool writeOpen_ = false;
  uint64_t writeFirst_ = 0, writeLast_ = 0;

  uint32_t rd(uint32_t offset) const { return plat_->read32(base_, offset); }
  void wr(uint32_t offset, uint32_t value) const { plat_->write32(base_, offset, value); }
  uint64_t read64(uint32_t offset) const { return rd(offset) | (static_cast<uint64_t>(rd(offset + 4)) << 32); }
  void write64(uint32_t offset, uint64_t value) const {
    wr(offset, static_cast<uint32_t>(value));
    wr(offset + 4, static_cast<uint32_t>(value >> 32));
  }

  uint8_t *window(uint32_t offset) const { return dma_->virt + offset; }
  uint8_t *data() const { return window(kOffData); }
  uint32_t maxRun() const {
    uint32_t run = kMaxRunBlocks;
    if (id.mdtsBlocks && id.mdtsBlocks < run) run = id.mdtsBlocks;
    return run;
  }

  static void put32(uint8_t *at, uint32_t value) { __builtin_memcpy(at, &value, 4); }
  static void put64(uint8_t *at, uint64_t value) { __builtin_memcpy(at, &value, 8); }
  static uint32_t get32(const uint8_t *at) {
    uint32_t value;
    __builtin_memcpy(&value, at, 4);
    return value;
  }

  bool waitReady(bool wanted) const {
    const uint64_t until = plat_->nowUs() + readyTimeoutUs_;
    for (;;) {
      const uint32_t status = rd(kCsts);
      if (!!(status & kCstsReady) == wanted) return true;
      if (plat_->nowUs() >= until) return false;
      plat_->delayUs(100);
    }
  }

  int resetController() {
    const uint32_t cc = rd(kCc);
    const uint32_t csts = rd(kCsts);
    plat_->log("NativeNvme: controller as the firmware left it: CC %#x CSTS %#x\n", cc, csts);
    wr(kCc, 0);
    if (!waitReady(false)) {
      plat_->log("NativeNvme: the controller did not drop CSTS.RDY after CC.EN = 0 (CSTS %#x)\n", rd(kCsts));
      return kTimeout;
    }
    return kOk;
  }

  int enableController() {
    wr(kIntms, 0xffffffffu);  // no pin-based interrupts
    bzero_(window(kOffAdminSq), kPage);
    bzero_(window(kOffAdminCq), kPage);
    bzero_(window(kOffIoSq), kPage);
    bzero_(window(kOffIoCq), kPage);
    dma_->flush(dma_->virt, kOffData);
    queue_[0] = Queue();
    queue_[1] = Queue();
    wr(kAqa, ((kQueueEntries - 1) << 16) | (kQueueEntries - 1));
    write64(kAsq, dma_->bus + kOffAdminSq);
    write64(kAcq, dma_->bus + kOffAdminCq);
    // EN, NVM command set, 4 KiB pages, round robin, 64-byte SQ entries (2^6), 16-byte CQ entries (2^4)
    wr(kCc, kCcEnable | (0u << 4) | (0u << 7) | (0u << 11) | (6u << 16) | (4u << 20));
    if (!waitReady(true)) {
      plat_->log("NativeNvme: the controller did not become ready (CSTS %#x)\n", rd(kCsts));
      return kTimeout;
    }
    if (rd(kCsts) & kCstsFatal) {
      plat_->log("NativeNvme: the controller reports a fatal status (CSTS %#x)\n", rd(kCsts));
      return kIoError;
    }
    return kOk;
  }

  int identify() {
    Command command = {};
    command.opcode = kAdminIdentify;
    command.prp1 = dma_->bus + kOffIdentify;
    command.cdw10 = 1;  // CNS 1: the controller
    int result = submit(0, command, nullptr, 5000000);
    if (result != kOk) {
      plat_->log("NativeNvme: identify controller failed (%s)\n", resultName(result));
      return result;
    }
    dma_->flush(window(kOffIdentify), kPage);
    const uint8_t *page = window(kOffIdentify);
    copyText(id.serial, page + 4, 20);
    copyText(id.model, page + 24, 40);
    copyText(id.firmware, page + 64, 8);
    const uint32_t mdts = page[77];
    id.mdtsBlocks = mdts && mdts < 20 ? (1u << mdts) * (kPage / storage::kBlockBytes) : 0;  // minimum page size 4 KiB
    id.namespaces = get32(page + 516);
    id.oncs = static_cast<uint16_t>(page[520] | (page[521] << 8));
    id.volatileWriteCache = page[525] & 1;
    plat_->log("NativeNvme: controller \"%s\" serial \"%s\" firmware \"%s\": %u namespaces, MDTS %u (%u blocks), volatile write cache %s, ONCS %#x\n", id.model,
               id.serial, id.firmware, id.namespaces, mdts, id.mdtsBlocks, id.volatileWriteCache ? "yes" : "no", id.oncs);
    if (!id.namespaces) return kUnsupported;

    // The first active namespace; a controller that has no list (NVMe before 1.1) gets namespace 1.
    uint32_t nsid = 1;
    Command list = {};
    list.opcode = kAdminIdentify;
    list.prp1 = dma_->bus + kOffIdentify;
    list.cdw10 = 2;  // CNS 2: active namespace IDs greater than NSID 0
    if (submit(0, list, nullptr, 5000000) == kOk) {
      dma_->flush(window(kOffIdentify), kPage);
      if (get32(window(kOffIdentify))) nsid = get32(window(kOffIdentify));
    }
    Command ns = {};
    ns.opcode = kAdminIdentify;
    ns.nsid = nsid;
    ns.prp1 = dma_->bus + kOffIdentify;
    ns.cdw10 = 0;  // CNS 0: the namespace
    result = submit(0, ns, nullptr, 5000000);
    if (result != kOk) {
      plat_->log("NativeNvme: identify namespace %u failed (%s)\n", nsid, resultName(result));
      return result;
    }
    dma_->flush(window(kOffIdentify), kPage);
    page = window(kOffIdentify);
    uint64_t nsze = get32(page) | (static_cast<uint64_t>(get32(page + 4)) << 32);
    const uint8_t flbas = page[26];
    const uint32_t format = (flbas & 0xf) | (((flbas >> 5) & 3) << 4);
    const uint8_t *lbaf = page + 128 + 4 * format;
    const uint32_t metadata = lbaf[0] | (lbaf[1] << 8);
    const uint32_t lbads = lbaf[2];
    plat_->log("NativeNvme: namespace %u: %llu blocks of 2^%u bytes (format %u, %u metadata bytes) = %llu MiB\n", nsid,
               static_cast<unsigned long long>(nsze), lbads, format, metadata,
               static_cast<unsigned long long>(lbads < 63 ? (nsze << lbads) >> 20 : 0));
    if (!nsze) return kUnsupported;
    if (lbads != 9 || metadata) {
      plat_->log("NativeNvme: the namespace has %u-byte blocks and %u metadata bytes; the GPT and the root image are laid out in 512-byte "
                 "sectors: reformat it with `nvme format --lbaf=<the 512-byte format> /dev/nvmeXnY` (this destroys its data)\n",
                 lbads < 31 ? 1u << lbads : 0u, metadata);
      return kUnsupported;
    }
    id.nsid = nsid;
    id.lbaBytes = 1u << lbads;
    id.blocks = nsze;
    return kOk;
  }

  int createIoQueues() {
    // Ask for one queue pair; the answer says how many there are, one is all that is used.
    Command features = {};
    features.opcode = kAdminSetFeatures;
    features.cdw10 = 0x07;  // number of queues
    features.cdw11 = 0;     // one submission queue, one completion queue (zero based)
    uint32_t granted = 0;
    int result = submit(0, features, &granted, 5000000);
    if (result != kOk) {
      plat_->log("NativeNvme: set features (number of queues) failed (%s)\n", resultName(result));
      return result;
    }
    plat_->log("NativeNvme: queues granted: %u submission, %u completion\n", (granted & 0xffff) + 1, (granted >> 16) + 1);
    Command cq = {};
    cq.opcode = kAdminCreateCq;
    cq.prp1 = dma_->bus + kOffIoCq;
    cq.cdw10 = ((kQueueEntries - 1) << 16) | 1;
    cq.cdw11 = 1;  // physically contiguous, interrupts disabled
    result = submit(0, cq, nullptr, 5000000);
    if (result != kOk) {
      plat_->log("NativeNvme: create I/O completion queue failed (%s)\n", resultName(result));
      return result;
    }
    Command sq = {};
    sq.opcode = kAdminCreateSq;
    sq.prp1 = dma_->bus + kOffIoSq;
    sq.cdw10 = ((kQueueEntries - 1) << 16) | 1;
    sq.cdw11 = (1u << 16) | 1;  // completion queue 1, medium priority class ignored (round robin), physically contiguous
    result = submit(0, sq, nullptr, 5000000);
    if (result != kOk) {
      plat_->log("NativeNvme: create I/O submission queue failed (%s)\n", resultName(result));
      return result;
    }
    return kOk;
  }

  // After a timeout: reset, set the admin queue up again and recreate the I/O queues. The identity is kept.
  int recover() {
    ++stats.recoveries;
    plat_->log("NativeNvme: recovering the controller after a timeout (CSTS %#x)\n", rd(kCsts));
    ready_ = false;
    int result = resetController();
    if (result == kOk) result = enableController();
    if (result == kOk) result = createIoQueues();
    if (result != kOk) {
      plat_->log("NativeNvme: recovery failed (%s)\n", resultName(result));
      return result;
    }
    ready_ = true;
    return kOk;
  }

  // One read or write of `run` blocks (the data area of the window) at `lba`.
  int transfer(uint8_t opcode, uint64_t lba, uint32_t run) {
    const uint32_t bytes = run * storage::kBlockBytes;
    const uint32_t pages = (bytes + kPage - 1) / kPage;
    Command command = {};
    command.opcode = opcode;
    command.nsid = id.nsid;
    command.prp1 = dma_->bus + kOffData;
    if (pages == 2) {
      command.prp2 = dma_->bus + kOffData + kPage;
    } else if (pages > 2) {
      uint8_t *list = window(kOffPrpList);
      for (uint32_t i = 1; i < pages; ++i) put64(list + 8 * (i - 1), dma_->bus + kOffData + static_cast<uint64_t>(i) * kPage);
      dma_->flush(list, 8 * (pages - 1));
      command.prp2 = dma_->bus + kOffPrpList;
    }
    command.cdw10 = static_cast<uint32_t>(lba);
    command.cdw11 = static_cast<uint32_t>(lba >> 32);
    command.cdw12 = run - 1;
    // Reads: drop what the CPU may have cached of the window, so no dirty line lands on the DMA data. Writes: push the data out.
    dma_->flush(data(), bytes);
    return submit(1, command, nullptr, 8000000);
  }

  // Put `command` on queue `qid` (0 admin, 1 I/O), ring the doorbell, wait for its completion. `result` receives dword 0 of the entry.
  int submit(unsigned qid, const Command &command, uint32_t *result, uint64_t timeoutUs) {
    Queue &queue = queue_[qid];
    const uint32_t sqOffset = qid ? kOffIoSq : kOffAdminSq, cqOffset = qid ? kOffIoCq : kOffAdminCq;
    uint8_t *entry = window(sqOffset) + queue.sqTail * 64;
    const uint16_t commandId = queue.nextId++;
    if (!queue.nextId) queue.nextId = 1;
    bzero_(entry, 64);
    put32(entry + 0, command.opcode | (static_cast<uint32_t>(commandId) << 16));
    put32(entry + 4, command.nsid);
    put64(entry + 24, command.prp1);
    put64(entry + 32, command.prp2);
    put32(entry + 40, command.cdw10);
    put32(entry + 44, command.cdw11);
    put32(entry + 48, command.cdw12);
    dma_->flush(entry, 64);
    queue.sqTail = (queue.sqTail + 1) % kQueueEntries;
    wr(kDoorbells + (2 * qid) * stride_, queue.sqTail);

    const uint64_t until = plat_->nowUs() + timeoutUs;
    uint8_t *completion = window(cqOffset) + queue.cqHead * 16;
    uint32_t status;
    for (;;) {
      dma_->flush(completion, 16);
      status = get32(completion + 12);
      if (((status >> 16) & 1) == queue.phase) break;
      if (plat_->nowUs() >= until) {
        ++stats.timeouts;
        plat_->log("NativeNvme: command %#x (opcode %#x) timed out after %llu ms, CSTS %#x\n", commandId, command.opcode,
                   static_cast<unsigned long long>(timeoutUs / 1000), rd(kCsts));
        return kTimeout;
      }
    }
    if (result) *result = get32(completion);
    const uint32_t completedId = status & 0xffff;
    queue.cqHead = (queue.cqHead + 1) % kQueueEntries;
    if (!queue.cqHead) queue.phase ^= 1;
    wr(kDoorbells + (2 * qid + 1) * stride_, queue.cqHead);
    ++stats.commands;
    const uint32_t code = status >> 17;  // status code type and status code
    if (completedId != commandId) {
      plat_->log("NativeNvme: command %#x completed as %#x\n", commandId, completedId);
      return kIoError;
    }
    if (code) {
      plat_->log("NativeNvme: command %#x (opcode %#x, queue %u) failed: status type %u code %#x%s\n", commandId, command.opcode, qid,
                 (code >> 8) & 7, code & 0xff, (status >> 31) ? " (do not retry)" : "");
      return kIoError;
    }
    return kOk;
  }

  static void bzero_(void *at, size_t bytes) { __builtin_memset(at, 0, bytes); }

  static void copyText(char *out, const uint8_t *in, unsigned bytes) {
    unsigned end = 0;
    for (unsigned i = 0; i < bytes; ++i) {
      out[i] = in[i] >= 0x20 && in[i] < 0x7f ? static_cast<char>(in[i]) : ' ';
      if (out[i] != ' ') end = i + 1;
    }
    out[end] = 0;
  }
};

}  // namespace nvme
