// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// NativeNvme: the NVMe drive in the M.2 slot of the Orange Pi 5 (RK3588S, PCIe 2.0 x1 controller "pcie2x1l2", PCI segment 4
// of edk2-rk3588) as an IOKit service, and the block devices it publishes.
//
// Derived from NativeEmmc.cpp of Rk3588Emmc (Ken-u/Rk3588Emmc, UniXNU-evb10 package/Rk3588Emmc): everything above the
// controller (the GPT scan, the SHNGROOT root image and its windows, the Data volume, the kernel hooks native_emmc_*, the watchdog
// feeder and the power-manager nub) is the same code with the host core swapped for NvmeHost.h. The C symbols keep their
// native_emmc_ names on purpose: the kernel patches of the EVB10 (evb10-userland-mounts) and of the SoC (rk3588-root-trust) call them.
//
// Nothing in the firmware is repeated here. edk2-rk3588 brought the PCIe link up, assigned BAR0 of the drive and read the ESP
// (the payload) from it with its own NvmExpressDxe; this driver finds the same things in the same places, takes the controller over
// (NvmeHost.h resets it first, because the firmware's driver leaves it enabled) and reads the drive by polling. It maps the PCIe
// controller's registers at fixed addresses (the firmware's own, Rk3588Pcie.h of edk2-rk3588, see kApb/kDbi/kCfg0 below) because
// the device tree node it attaches to, "pcie-nvme", only exists to be matched: it carries no `reg` (the arm-io window spans every
// exported `reg`, and the controller's DBI and config space sit above 4 GiB).
//
// The drive is read-only except for one window the owner asks for (nvme-data-persist=1, below): the core refuses to write a block
// outside the range it was given, and the range is never anything but that window.
//
// NativeNvmeBlock is a window of the drive (the whole disk, or one partition) as a subclass of the original IOBlockStorageDevice:
// 512-byte sectors (the namespace has to be formatted that way), read-only (except the Data window), synchronous. The original
// IOBlockStorageDriver, the partition schemes and APFS sit on top unchanged. How the subclass is bound to the original class is
// explained in scripts/bind-nvme-storage.py of UniXNU-orangepi5.
//
// Boot-args:  nvme=0           do not attach
//             nvme=1           probe only (default): bring the link and the controller up, read the GPT, check that reads are
//                              stable, publish nothing
//             nvme=2           also publish the whole disk as one read-only block device
//             nvme=3           also publish the partition named "userdata" as one device
//             nvme=4           root image: read the SHNGROOT manifest at the start of `userdata` (RootManifest.h), and publish its
//                              windows, "SystemOS" first and "OS" last, each as one read-only device of its own. The volume
//                              inside "OS" is what /chosen/root-matching selects as the root.
//             nvme-probe=N     blocks of `userdata` to double-read (default 1024, 0 = none)
//             nvme-boot-manifest=0  do not publish /chosen/boot-manifest-hash (default 1, see publishBootManifestHash): the
//                              original `mount -P 1` exits 66 without it
//             nvme-root-lba=N  drive block of the manifest (default: first block of `userdata`)
//             nvme-root-trust=1  let the kernel accept the root volume without Apple's root authentication, if it is a read-only
//                              volume inside the "OS" window this driver published (native_emmc_root_trusted, called from bsd_init
//                              by the SoC patch rk3588-root-trust.patch). Without it the kernel panics with "rootvp not
//                              authenticated after mounting", because the OS volume of an IPSW is not sealed.
//             wdt-kick=N       keep the DW watchdog at 0xFEAF0000 fed for N seconds. The firmware arms it (about 89 s) and stops
//                              feeding it at ExitBootServices, and nothing in the kernel does, so without this every run ends there.
//                              The feeding stops after N seconds on purpose: a hung board still resets by itself. Default 0 =
//                              do not feed.
//             pmgr-nub=0       do not publish the ApplePMGRNub that the original AMFI waits for (default 1, see pmgrNubPublisher).
//                              Independent of the drive.
//             nvme-data-size=N make the Data RAM disk N MiB (default: as big as the payload's seed). The seed on the payload only
//                              needs to carry the part of the APFS container that is not zero (TRIM=1 of the data seed tool); the
//                              rest is zero-filled kernel memory made here (growDataDisk).
//             nvme-data-ram=0  do not publish the Data RAM disk (default 1, see dataDiskPublisher): the payload's
//                              /chosen/memory-map/RAMDisk as a WRITABLE block device of its own, in front of which Apple's APFS
//                              publishes the Data volume as /dev/diskNsM. Independent of the drive.
//             nvme-data-persist=1  keep the Data volume on the drive instead of in RAM (default 0; needs nvme=4 and the payload's
//                              seed, see persistentData). The volume is a window at the END of `userdata`, a 1 MiB header and then
//                              the APFS container the seed describes (its size comes from the seed's own superblock, not from
//                              nvme-data-size), clear of the root image by 64 MiB. The first boot formats it (zeros, the seed, a
//                              read-back, the header's "ready"), later boots find the header and publish the window as it is; the
//                              kernel then copies the OS volume's /private/var into it only once (evb10-userland-mounts.patch).
//                              This is the only place the driver writes the drive. If anything about it is wrong the Data RAM disk
//                              is published instead, as without the argument.
//             nvme-data-reset=1    format that window again, whatever it holds (default 0)
//             nvme-data-wait=N     seconds native_emmc_data_device waits for the Data volume to appear (default 30)
//
// The kernel reaches this driver through five C symbols. Three of them refuse unless nvme-root-trust=1 and are about read-only
// volumes inside a window published here:
//   native_emmc_root_trusted(dev)     the root volume is the one of the "OS" window
//   native_emmc_volume_device(uuid, &dev, path, size)
//                                     the BSD device (and its /dev path) of the volume with this UUID inside the "SystemOS" window,
//                                     for the cryptex mount
//   native_emmc_cache_trusted(dev)    that device is the "SystemOS" window's volume, still read-only
// the fourth is about the writable one:
//   native_emmc_data_device(uuid, &dev, path, size)
//                                     the BSD device of the volume with this UUID inside the Data RAM disk (or the Data window of
//                                     the drive), for the kernel's mount of it on /private/var. Why not /dev/md0: Apple's APFS takes
//                                     the volume index from the device NAME ("diskNsM" is volume M-1, anything else is -1 and
//                                     nx_is_mount_valid refuses it with ENOENT).
#include <IOKit/IOBSD.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOService.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/crypto/sha2.h>
#include <pexpert/pexpert.h>

#include "Gpt.h"
#include "NvmeHost.h"
#include "RootManifest.h"

// xnu's own device-memory mapper (osfmk/arm/machine_routines.h), as the platform code uses it
// for the GIC and the UART. Declared here because the driver does not include that header.
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
// Boot memory (the RAM disk of the payload) is reached through the kernel's static mapping, like
// the original code does; gPhysBase/gPhysSize bound the physical addresses the loader may hand over.
extern "C" vm_offset_t ml_static_ptovirt(vm_offset_t paddr);
extern "C" unsigned long gPhysBase, gPhysSize;

// The sizes of the original classes (scripts/data/evb10-storage-interface-audit.json of UniXNU-evb10). The public
// headers this file compiles against must describe the same objects as the original kernelcache,
// or a subclass would overlay its members on the base class's.
static_assert(sizeof(IOService) == 0x88, "IOService layout differs from the original kernelcache");
static_assert(sizeof(IOBlockStorageDevice) == 0x90, "IOBlockStorageDevice layout differs from the original kernelcache");

extern "C" {
volatile uint32_t native_emmc_ready = 0;          // 1 once the drive answered and the GPT parsed
volatile uint64_t native_emmc_userdata_first = 0;
volatile uint64_t native_emmc_userdata_last = 0;
volatile uint32_t native_emmc_blocks_published = 0;
volatile uint64_t native_emmc_block_requests = 0;
volatile uint64_t native_emmc_block_failures = 0;
volatile uint64_t native_emmc_read_calls = 0;     // host reads (one per bounce-buffer chunk, 64 KiB at most)
volatile uint64_t native_emmc_read_blocks = 0;    // blocks the drive delivered
volatile uint64_t native_emmc_read_us = 0;        // time spent inside those reads
volatile uint32_t native_emmc_multi_block = 0;    // 1 once reads are runs of blocks (always, on NVMe: one command per run)
volatile uint32_t native_emmc_dma = 0;            // 1 once the controller has read a probe by DMA (always, on NVMe)
volatile uint64_t native_emmc_dma_blocks = 0;     // blocks read
volatile int32_t native_emmc_root_status = -1;    // storage::RootStatus of the manifest, -1 = not looked at
volatile uint32_t native_emmc_root_windows = 0;   // root windows started by the publisher
volatile uint32_t native_emmc_data_ready = 0;     // 1 once the Data RAM disk is published
volatile uint64_t native_emmc_ram_requests = 0;   // block requests served from the Data RAM disk
volatile uint64_t native_emmc_ram_writes = 0;     // ... of which writes
volatile uint64_t native_emmc_ram_failures = 0;
volatile uint32_t native_emmc_data_persistent = 0; // 1 once the Data volume is a window of the drive (not RAM)
volatile uint32_t native_emmc_data_seeded = 0;     // ... and it was already formatted by an earlier boot
volatile uint64_t native_emmc_write_calls = 0;     // host writes (one per bounce-buffer chunk, 64 KiB at most)
volatile uint64_t native_emmc_write_blocks = 0;    // blocks the drive acknowledged
volatile uint64_t native_emmc_write_us = 0;
volatile uint64_t native_emmc_write_failures = 0;  // write calls that failed
volatile uint64_t native_emmc_flush_calls = 0;

// Two hooks that the kernel patch evb10-userland-mounts calls and that the Mali driver of the EVB10 defines: the GPU addon the loader
// placed after the trust caches, and the test programs the display archive carries. This board has no display archive yet; these weak
// definitions answer "none", which the patch handles (it prints that the loader handed over no addon / the kernel carries no test
// program, and starts launchd as usual). A linked Mali archive defines the same names and replaces them.
__attribute__((weak)) int mali_gpu_addon(const uint8_t **data, uint64_t *bytes) {
  if (data) *data = nullptr;
  if (bytes) *bytes = 0;
  return 0;
}

__attribute__((weak)) int mali_test_file(unsigned index, const char **name, const uint8_t **data, size_t *size, size_t *rawSize) {
  (void)index;
  if (name) *name = nullptr;
  if (data) *data = nullptr;
  if (size) *size = 0;
  if (rawSize) *rawSize = 0;
  return -1;
}
}

namespace {

constexpr uint32_t kChunkBlocks = nvme::kMaxRunBlocks;        // one bounce buffer: 128 KiB, one NVMe command
constexpr uint32_t kChunkBytes = kChunkBlocks * storage::kBlockBytes;
constexpr unsigned kRequestLogLines = 48;                     // first block requests that are logged

// 32-bit accesses only: the NVMe registers are 32 or 64 bits wide and the host core reads a 64-bit register as two halves; the
// PCIe configuration window of the controller is accessed the same way.
uint32_t mmioRead(void *base, uint32_t offset) {
  return *reinterpret_cast<volatile uint32_t *>(static_cast<volatile uint8_t *>(base) + offset);
}

void mmioWrite(void *base, uint32_t offset, uint32_t value) {
  *reinterpret_cast<volatile uint32_t *>(static_cast<volatile uint8_t *>(base) + offset) = value;
  __asm__ volatile("dsb sy" ::: "memory");
}

uint64_t nowUs() {
  uint64_t nanoseconds = 0;
  absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
  return nanoseconds / 1000;
}

// Short waits spin. IODelay() puts the calling thread to sleep on a timer whenever the platform does
// not report a CPU idle latency, and the RK3588 does not: on the EVB10 every 1 us poll of the eMMC became a
// context switch and a timer interrupt (72,000 of each per second under a read load). A poll that long is a
// register read and a spin; only a real wait sleeps.
void delayUs(uint32_t microseconds) {
  if (microseconds >= 1000) {
    IOSleep((microseconds + 999) / 1000);
    return;
  }
  const uint64_t until = nowUs() + microseconds;
  while (nowUs() < until) {
  }
}

const nvme::Platform kPlatform = {mmioRead, mmioWrite, nowUs, delayUs, IOLog};

// The controller does not look into the CPU's caches (the RK3588's PCIe DMA is not cache-coherent and this
// kernel does no maintenance of its own for it): before a command the window's lines are cleaned out to memory and
// dropped, so no dirty line is written over the data later; afterwards they are dropped again, so the CPU reads what
// the controller wrote and not a line it fetched on the way. Clean+invalidate (dc civac) does both, and is harmless
// on a line that is not dirty. NvmeHost.h calls this around every command, and tests/test-nvme-host.cpp checks that it does.
void dmaFlush(void *virt, size_t bytes) {
  uint64_t ctr;
  __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
  const uintptr_t line = static_cast<uintptr_t>(4) << ((ctr >> 16) & 15);
  const uintptr_t end = reinterpret_cast<uintptr_t>(virt) + bytes;
  __asm__ volatile("dsb sy" ::: "memory");
  for (uintptr_t address = reinterpret_cast<uintptr_t>(virt) & ~(line - 1); address < end; address += line)
    __asm__ volatile("dc civac, %0" ::"r"(address) : "memory");
  __asm__ volatile("dsb sy" ::: "memory");
}

nvme::DmaWindow gDmaWindow = {};
IOBufferMemoryDescriptor *gDmaMemory = nullptr;  // for the life of the system

// One physically contiguous, cacheable run of memory: the queues, the PRP list, the identify page and 128 KiB of data (nvme::kOff*).
// The controller addresses it by physical address: the PCIe controller of the RK3588 has no IOMMU in front of it and the
// firmware's own driver does the same (edk2-rk3588 sets DmaAbove4G for it), so the window may sit anywhere below 2^40, which is
// where the kernel's memory is on a board with more than 4 GiB. IOKit places it by the mask (bits that may be set in the address).
const nvme::DmaWindow *dmaWindow() {
  if (gDmaWindow.virt) return &gDmaWindow;
  const size_t bytes = nvme::kWindowBytes;
  IOBufferMemoryDescriptor *memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
      kernel_task, static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryMapperNone) | kIOMemoryPhysicallyContiguous,
      bytes, 0x000000fffffff000ULL);
  if (!memory) {
    IOLog("NativeNvme: dma: no contiguous memory for the NVMe window\n");
    return nullptr;
  }
  if (memory->prepare() != kIOReturnSuccess) {
    IOLog("NativeNvme: dma: the NVMe window could not be wired\n");
    memory->release();
    return nullptr;
  }
  IOByteCount length = 0;
  const uint64_t bus = memory->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
  uint8_t *virt = static_cast<uint8_t *>(memory->getBytesNoCopy());
  if (!virt || !bus || length < bytes || bus + bytes > (1ull << 40) || (bus & 0xfff)) {
    IOLog("NativeNvme: dma: window at bus %#llx, %llu contiguous bytes of %zu: not usable\n", static_cast<unsigned long long>(bus),
          static_cast<unsigned long long>(length), bytes);
    (void)memory->complete();
    memory->release();
    return nullptr;
  }
  bzero(virt, bytes);
  dmaFlush(virt, bytes);
  gDmaMemory = memory;
  gDmaWindow.virt = virt;
  gDmaWindow.bus = bus;
  gDmaWindow.flush = dmaFlush;
  IOLog("NativeNvme: dma: window at bus %#llx, kernel %p, %zu bytes (queues, PRP list and identify page, then %u bytes of data)\n",
        static_cast<unsigned long long>(bus), static_cast<void *>(virt), bytes, nvme::kMaxRunBytes);
  return &gDmaWindow;
}

uint32_t bootArg(const char *name, uint32_t fallback) {
  uint32_t value = fallback;
  if (!PE_parse_boot_argn(name, &value, sizeof(value))) return fallback;
  return value;
}

uint64_t bootArg64(const char *name, uint64_t fallback) {
  uint64_t value = fallback;
  if (!PE_parse_boot_argn(name, &value, sizeof(value))) return fallback;
  return value;
}

void sha256(const uint8_t *data, size_t length, uint8_t out[32]) {
  SHA256_CTX context;
  SHA256_Init(&context);
  SHA256_Update(&context, data, length);
  SHA256_Final(out, &context);
}

// Hex of the first bytes of a block, for the log.
void hexLine(const char *label, uint64_t lba, const uint8_t *bytes, unsigned count) {
  static const char digits[] = "0123456789abcdef";
  char text[3 * 32 + 1];
  unsigned at = 0;
  for (unsigned i = 0; i < count && i < 32; ++i) {
    text[at++] = digits[bytes[i] >> 4];
    text[at++] = digits[bytes[i] & 15];
    text[at++] = ' ';
  }
  text[at] = 0;
  IOLog("NativeNvme: %s lba %llu: %s\n", label, static_cast<unsigned long long>(lba), text);
}

unsigned long long ull(uint64_t value) { return static_cast<unsigned long long>(value); }

}  // namespace

class NativeNvmeBlock;

class NativeNvme : public IOService {
  OSDeclareDefaultStructors(NativeNvme)

  IOLock *gate = nullptr;
  nvme::Host host;
  void *bar0 = nullptr;               // the drive's registers, mapped by bringUpPcie()
  uint64_t diskBlocks = 0;
  uint64_t userdataFirst = 0, userdataLast = 0;
  // The root image (nvme=4): where its manifest is, what it says, and the publication order.
  uint64_t rootLba = 0;
  storage::RootManifest root = {};
  storage::RootPlan rootPlan = {};

  bool rootReady = false;             // findRoot() found a usable image
  bool bringUpPcie();
  bool scanGpt();
  void probeUserdata();
  bool findRoot();
  // The Data volume on the drive (nvme-data-persist=1), see persistentData(). Runs on a thread of its own.
  bool persistentData();
  bool formatData(uint64_t windowFirst, uint64_t dataBlocks, const uint8_t *seed, uint64_t seedBytes, uint32_t seedCrc, uint32_t generation);
  static void dataPublisher(void *self, wait_result_t);
  // `role` is what the kernel will later ask about the window: 0 none, 1 the "OS" window (the root
  // volume, native_emmc_root_trusted), 2 the "SystemOS" window (the cryptex volume,
  // native_emmc_volume_device and native_emmc_cache_trusted).
  NativeNvmeBlock *publishWindow(const char *label, uint64_t first, uint64_t blocks, unsigned role = 0, bool writable = false);
  void publishRoot();
  static void rootPublisher(void *self, wait_result_t);

 public:
  bool start(IOService *provider) override;
  void stop(IOService *provider) override;
  void free() override;

  // Serialised read of whole blocks; the block devices call this.
  IOReturn readBlocks(uint64_t lba, uint32_t count, void *out) {
    if (!gate) return kIOReturnNotReady;
    IOLockLock(gate);
    int result = host.readBlocks(lba, count, out);
    native_emmc_read_calls = native_emmc_read_calls + 1;
    native_emmc_read_blocks = host.stats.blocks;
    native_emmc_read_us = host.stats.readUs;
    native_emmc_dma_blocks = host.stats.blocks;
    IOLockUnlock(gate);
    if (result == nvme::kOk) return kIOReturnSuccess;
    return result == nvme::kTimeout ? kIOReturnTimeout : kIOReturnIOError;
  }

  // Serialised write of whole blocks, inside the range the drive was opened for (enableWrites) and nowhere else:
  // the host core refuses the rest. Returns once the drive reports the blocks written.
  IOReturn writeBlocks(uint64_t lba, uint32_t count, const void *in) {
    if (!gate) return kIOReturnNotReady;
    IOLockLock(gate);
    const int result = host.writeBlocks(lba, count, in);
    native_emmc_write_calls = native_emmc_write_calls + 1;
    native_emmc_write_blocks = host.stats.writeBlocks;
    native_emmc_write_us = host.stats.writeUs;
    if (result != nvme::kOk) native_emmc_write_failures = native_emmc_write_failures + 1;
    IOLockUnlock(gate);
    if (result == nvme::kOk) return kIOReturnSuccess;
    return result == nvme::kTimeout ? kIOReturnTimeout : kIOReturnIOError;
  }

  // What the drive has in its volatile write cache goes to the media (a no-op on a drive without one).
  IOReturn flush() {
    if (!gate) return kIOReturnNotReady;
    IOLockLock(gate);
    const int result = host.flushCache();
    native_emmc_flush_calls = native_emmc_flush_calls + 1;
    IOLockUnlock(gate);
    if (result == nvme::kOk) return kIOReturnSuccess;
    return result == nvme::kTimeout ? kIOReturnTimeout : kIOReturnIOError;
  }

  // The one place the driver lets itself write: [first, last]. DMA has to be on.
  bool enableWrites(uint64_t first, uint64_t last) {
    if (!gate) return false;
    IOLockLock(gate);
    host.setWriteRange(first, last);
    const bool ok = host.writable();
    if (!ok) host.clearWriteRange();
    IOLockUnlock(gate);
    return ok;
  }
  void disableWrites() {
    if (!gate) return;
    IOLockLock(gate);
    host.clearWriteRange();
    IOLockUnlock(gate);
  }
};

OSDefineMetaClassAndStructors(NativeNvme, IOService)

// One window of the drive as a block device, read-only unless it is the Data window (configure with writable).
// Sectors are 512 bytes, as the drive's own GPT and the disk images of the root volume expect.
//
// The same class also serves the Data RAM disk (configureRam):
// a part of boot memory the payload hands over, read and written by plain copies. Same bindings to
// the original IOBlockStorageDevice, no second subclass to audit.
class NativeNvmeBlock : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(NativeNvmeBlock)

  NativeNvme *owner = nullptr;      // our provider; it outlives us
  uint64_t first = 0;               // first drive block of the window
  uint64_t blocks = 0;              // length of the window in blocks
  char label[32] = {};
  uint8_t *bounce = nullptr;
  IOLock *lock = nullptr;           // the bounce buffer is shared by all requests of this window
  uint8_t *ram = nullptr;           // RAM disk mode: the memory the blocks are, never freed (boot memory)
  bool online = false;
  bool writable = false;            // drive window the host core may write (the Data window of nvme-data-persist=1)

 public:
  bool configure(NativeNvme *controller, const char *name, uint64_t firstBlock, uint64_t blockCount, bool canWrite = false);
  bool configureRam(const char *name, uint8_t *memory, uint64_t bytes);
  const char *windowLabel() const { return label; }
  bool start(IOService *provider) override;
  void free() override;
  IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) override { return kIOReturnNotWritable; }
  UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
  char *getVendorString() override { return const_cast<char *>("UniXNU"); }
  char *getProductString() override { return label; }
  char *getRevisionString() override { return const_cast<char *>("1.0"); }
  char *getAdditionalDeviceInfoString() override {
    return const_cast<char *>(ram ? "RAM disk, volatile" : writable ? "NVMe, polled writes" : "NVMe, read-only");
  }
  IOReturn reportBlockSize(UInt64 *value) override { *value = storage::kBlockBytes; return kIOReturnSuccess; }
  IOReturn reportEjectability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock(UInt64 *value) override { *value = blocks - 1; return kIOReturnSuccess; }
  IOReturn reportMediaState(bool *present, bool *changed) override {
    *present = online;
    if (changed) *changed = false;
    return kIOReturnSuccess;
  }
  IOReturn reportRemovability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportWriteProtection(bool *value) override { *value = !(ram || writable); return kIOReturnSuccess; }
  IOReturn doSynchronizeCache() override {
    if (!online) return kIOReturnNotReady;
    return writable && owner ? owner->flush() : kIOReturnSuccess;
  }
  IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache(); }
  IOReturn getWriteCacheState(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *, IOStorageCompletion *) override;
};

OSDefineMetaClassAndStructors(NativeNvmeBlock, IOBlockStorageDevice)

bool NativeNvmeBlock::configure(NativeNvme *controller, const char *name, uint64_t firstBlock, uint64_t blockCount, bool canWrite) {
  if (!controller || !name || !blockCount || firstBlock > UINT64_MAX - blockCount ||
      blockCount > UINT64_MAX / storage::kBlockBytes) return false;
  owner = controller;
  first = firstBlock;
  blocks = blockCount;
  writable = canWrite;
  strlcpy(label, name, sizeof(label));
  bounce = static_cast<uint8_t *>(IOMallocData(kChunkBytes));
  lock = IOLockAlloc();
  return bounce && lock;
}

// The Data RAM disk: `memory` is the kernel's static mapping of `bytes` of boot memory (a whole
// number of pages, checked by the caller); the blocks of the device are those bytes.
bool NativeNvmeBlock::configureRam(const char *name, uint8_t *memory, uint64_t bytes) {
  if (!name || !memory || !bytes || (bytes % storage::kBlockBytes)) return false;
  owner = nullptr;
  first = 0;
  blocks = bytes / storage::kBlockBytes;
  strlcpy(label, name, sizeof(label));
  ram = memory;
  return true;
}

bool NativeNvmeBlock::start(IOService *provider) {
  if (!(ram || (bounce && lock)) || !IOBlockStorageDevice::start(provider)) return false;
  online = true;
  registerService();
  return true;
}

IOReturn NativeNvmeBlock::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                           IOStorageAttributes *attributes, IOStorageCompletion *completion) {
  if (!buffer || !completion || !completion->action || !count || block >= blocks || count > blocks - block ||
      count > UINT64_MAX / storage::kBlockBytes || buffer->getLength() < count * storage::kBlockBytes) {
    return kIOReturnBadArgument;
  }
  if (ram) {
    // kIODirectionIn: the data goes into the buffer (a read of the device); kIODirectionOut: it comes out of it.
    const IODirection direction = buffer->getDirection();
    if (direction != kIODirectionIn && direction != kIODirectionOut) return kIOReturnBadArgument;
    if (!online) return kIOReturnNotReady;
    const uint64_t bytes = count * storage::kBlockBytes;
    const bool write = direction == kIODirectionOut;
    uint8_t *at = ram + block * storage::kBlockBytes;
    uint64_t done = write ? buffer->readBytes(0, at, bytes) : buffer->writeBytes(0, at, bytes);
    IOReturn result = done == bytes ? kIOReturnSuccess : kIOReturnIOError;
    uint64_t serial = __atomic_fetch_add(&native_emmc_ram_requests, 1, __ATOMIC_RELAXED);
    if (write) __atomic_fetch_add(&native_emmc_ram_writes, 1, __ATOMIC_RELAXED);
    if (result != kIOReturnSuccess) __atomic_fetch_add(&native_emmc_ram_failures, 1, __ATOMIC_RELAXED);
    if (serial < 16 || result != kIOReturnSuccess) {
      IOLog("NativeNvmeBlock[%s]: ram %s #%llu lba %llu count %llu: %llu bytes, 0x%x\n", label, write ? "write" : "read", ull(serial),
            ull(block), ull(count), ull(done), static_cast<unsigned>(result));
    }
    completion->action(completion->target, completion->parameter, result, done);
    return kIOReturnSuccess;
  }
  const IODirection cardDirection = buffer->getDirection();
  const bool write = cardDirection == kIODirectionOut;
  if (cardDirection != kIODirectionIn && !(writable && write)) return kIOReturnNotWritable;
  if (!online) return kIOReturnNotReady;

  uint64_t serial = __atomic_fetch_add(&native_emmc_block_requests, 1, __ATOMIC_RELAXED);
  if (serial < kRequestLogLines) {
    IOLog("NativeNvmeBlock[%s]: %s #%llu lba %llu count %llu\n", label, write ? "write" : "read", ull(serial), ull(block), ull(count));
  }
  const uint64_t total = count * storage::kBlockBytes;
  uint64_t done = 0;
  IOReturn result = kIOReturnSuccess;
  IOLockLock(lock);
  while (done < total) {
    uint32_t chunk = static_cast<uint32_t>(total - done > kChunkBytes ? kChunkBlocks : (total - done) / storage::kBlockBytes);
    const uint64_t at = first + block + done / storage::kBlockBytes;
    if (write) {
      if (buffer->readBytes(done, bounce, chunk * storage::kBlockBytes) != chunk * storage::kBlockBytes) {
        result = kIOReturnIOError;
        break;
      }
      result = owner->writeBlocks(at, chunk, bounce);
      if (result != kIOReturnSuccess) break;
    } else {
      result = owner->readBlocks(at, chunk, bounce);
      if (result != kIOReturnSuccess) break;
      if (buffer->writeBytes(done, bounce, chunk * storage::kBlockBytes) != chunk * storage::kBlockBytes) {
        result = kIOReturnIOError;
        break;
      }
    }
    done += chunk * storage::kBlockBytes;
  }
  // Force unit access: the data has to be on the media, not only in the drive's cache, before the request completes.
  if (write && result == kIOReturnSuccess && attributes && (attributes->options & kIOStorageOptionForceUnitAccess)) {
    result = owner->flush();
  }
  IOLockUnlock(lock);
  if (result != kIOReturnSuccess) {
    uint64_t failures = __atomic_fetch_add(&native_emmc_block_failures, 1, __ATOMIC_RELAXED);
    if (failures < 16) {
      IOLog("NativeNvmeBlock[%s]: %s lba %llu count %llu failed after %llu bytes: 0x%x\n", label, write ? "write" : "read", ull(block),
            ull(count), ull(done), static_cast<unsigned>(result));
    }
  }
  completion->action(completion->target, completion->parameter, result, done);
  return kIOReturnSuccess;
}

void NativeNvmeBlock::free() {
  if (bounce) { IOFreeData(bounce, kChunkBytes); bounce = nullptr; }
  if (lock) { IOLockFree(lock); lock = nullptr; }
  IOBlockStorageDevice::free();
}

namespace {

// One IOMedia as the log shows it. The volumes of a filesystem add their own properties (the
// APFS ones carry FullName, Status and Sealed), shown when present.
void logMedia(const char *prefix, IOService *media) {
  auto text = [media](const char *key) -> const char * {
    OSString *value = OSDynamicCast(OSString, media->getProperty(key));
    return value ? value->getCStringNoCopy() : "-";
  };
  OSNumber *size = OSDynamicCast(OSNumber, media->getProperty("Size"));
  OSBoolean *whole = OSDynamicCast(OSBoolean, media->getProperty("Whole"));
  OSBoolean *writable = OSDynamicCast(OSBoolean, media->getProperty("Writable"));
  IOLog("%s: media %s: content %s, %llu bytes, %s%s uuid %s\n", prefix, text(kIOBSDNameKey), text("Content"),
        size ? ull(size->unsigned64BitValue()) : 0ull, whole && whole->isTrue() ? "whole" : "slice",
        writable && writable->isTrue() ? ", writable" : ", read-only", text("UUID"));
  if (media->getProperty("FullName") || media->getProperty("Sealed") || media->getProperty("Status")) {
    IOLog("%s:   volume \"%s\" status %s sealed %s\n", prefix, text("FullName"), text("Status"), text("Sealed"));
  }
}

// The IOMedia below `window` in the service plane: the one the block driver publishes for it,
// the partitions the partition scheme makes of that, and the volumes the filesystem makes of
// those. With `log` set each one goes to the log.
unsigned mediaBelow(IORegistryEntry *window, bool log) {
  IORegistryIterator *iterator = IORegistryIterator::iterateOver(window, gIOServicePlane, kIORegistryIterateRecursively);
  if (!iterator) return 0;
  unsigned count = 0;
  for (unsigned visited = 0; visited < 512; ++visited) {  // every entry below the window, media or not
    IORegistryEntry *entry = iterator->getNextObject();
    if (!entry) break;
    IOService *media = entry->metaCast("IOMedia") ? OSDynamicCast(IOService, entry) : nullptr;
    if (!media) continue;
    ++count;
    if (log) logMedia("NativeNvme", media);
  }
  iterator->release();
  return count;
}

// After a window is registered the partition scheme and the filesystem publish their media on
// threads of their own. Wait until the window is quiet and the number of IOMedia below it has
// stopped changing, for at most `seconds`. Returns that number.
unsigned settleMedia(IOService *window, unsigned seconds) {
  window->waitQuiet(static_cast<uint64_t>(seconds) * 1000000000ull);
  unsigned previous = ~0u, stable = 0, count = 0;
  for (unsigned tick = 0; tick < seconds * 10 && stable < 15; ++tick) {
    count = mediaBelow(window, false);
    stable = count && count == previous ? stable + 1 : 0;
    previous = count;
    IOSleep(100);
  }
  return count;
}

// What IOStorageFamily made of the windows: every IOMedia in the registry, once, after the
// count stops growing. Observation only.
void mediaReport(void *, wait_result_t) {
  unsigned previous = ~0u, stable = 0, count = 0;
  for (unsigned tick = 0; tick < 200 && stable < 15; ++tick) {
    OSDictionary *matching = IOService::serviceMatching("IOMedia");
    OSIterator *iterator = matching ? IOService::getMatchingServices(matching) : nullptr;
    count = 0;
    while (iterator && iterator->getNextObject()) ++count;
    if (iterator) iterator->release();
    if (matching) matching->release();
    stable = count && count == previous ? stable + 1 : 0;
    previous = count;
    IOSleep(100);
  }
  IOLog("NativeNvme: media report: %u IOMedia, %llu block requests, %llu failures\n", count,
        ull(native_emmc_block_requests), ull(native_emmc_block_failures));
  OSDictionary *matching = IOService::serviceMatching("IOMedia");
  OSIterator *iterator = matching ? IOService::getMatchingServices(matching) : nullptr;
  for (unsigned shown = 0; iterator && shown < 64; ++shown) {
    IOService *media = OSDynamicCast(IOService, iterator->getNextObject());
    if (!media) break;
    logMedia("NativeNvme", media);
  }
  if (iterator) iterator->release();
  if (matching) matching->release();
}

// The "OS" window of the root image, set before that window is started so that the root volume
// cannot be mounted before the trust decision can find it. Never cleared once the window runs.
NativeNvmeBlock *gOsWindow = nullptr;
// The "SystemOS" window (the cryptex volume with the dyld shared cache), set the same way.
NativeNvmeBlock *gSystemOsWindow = nullptr;
// The Data RAM disk (the one writable device), set the same way.
NativeNvmeBlock *gDataWindow = nullptr;

// The IOMedia below `window` whose BSD device number is major,minor (the number the kernel got
// from IOFindBSDRoot). Gives its BSD name, and whether it can be written (unknown counts as yes).
bool mediaByDevice(IORegistryEntry *window, uint32_t major, uint32_t minor, char (&name)[32], bool &writable) {
  IORegistryIterator *iterator = IORegistryIterator::iterateOver(window, gIOServicePlane, kIORegistryIterateRecursively);
  if (!iterator) return false;
  bool found = false;
  for (unsigned visited = 0; visited < 512 && !found; ++visited) {
    IORegistryEntry *entry = iterator->getNextObject();
    if (!entry) break;
    if (!entry->metaCast("IOMedia")) continue;
    OSNumber *entryMajor = OSDynamicCast(OSNumber, entry->getProperty(kIOBSDMajorKey));
    OSNumber *entryMinor = OSDynamicCast(OSNumber, entry->getProperty(kIOBSDMinorKey));
    if (!entryMajor || !entryMinor || entryMajor->unsigned32BitValue() != major || entryMinor->unsigned32BitValue() != minor) continue;
    OSString *bsdName = OSDynamicCast(OSString, entry->getProperty(kIOBSDNameKey));
    OSBoolean *flag = OSDynamicCast(OSBoolean, entry->getProperty("Writable"));
    strlcpy(name, bsdName ? bsdName->getCStringNoCopy() : "-", sizeof(name));
    writable = !flag || flag->isTrue();
    found = true;
  }
  iterator->release();
  return found;
}

// ASCII case-insensitive equality, for UUID strings (the kernel compares them in upper case, the
// property of an IOMedia may be either).
bool sameUuid(const char *a, const char *b) {
  if (!a || !b) return false;
  for (;; ++a, ++b) {
    char x = *a, y = *b;
    if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 'a' + 'A');
    if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 'a' + 'A');
    if (x != y) return false;
    if (!x) return true;
  }
}

// The IOMedia below `window` whose UUID property is `uuid`: its BSD name and device number, and
// whether it can be written (unknown counts as yes). The volumes of a filesystem carry the UUID
// of the volume itself, the one the original kernel code looks the root and the cryptex up by.
bool mediaByUuid(IORegistryEntry *window, const char *uuid, uint32_t &major, uint32_t &minor, char (&name)[32], bool &writable) {
  IORegistryIterator *iterator = IORegistryIterator::iterateOver(window, gIOServicePlane, kIORegistryIterateRecursively);
  if (!iterator) return false;
  bool found = false;
  for (unsigned visited = 0; visited < 512 && !found; ++visited) {
    IORegistryEntry *entry = iterator->getNextObject();
    if (!entry) break;
    if (!entry->metaCast("IOMedia")) continue;
    OSString *entryUuid = OSDynamicCast(OSString, entry->getProperty("UUID"));
    OSNumber *entryMajor = OSDynamicCast(OSNumber, entry->getProperty(kIOBSDMajorKey));
    OSNumber *entryMinor = OSDynamicCast(OSNumber, entry->getProperty(kIOBSDMinorKey));
    OSString *bsdName = OSDynamicCast(OSString, entry->getProperty(kIOBSDNameKey));
    if (!entryUuid || !sameUuid(entryUuid->getCStringNoCopy(), uuid) || !entryMajor || !entryMinor || !bsdName) continue;
    OSBoolean *flag = OSDynamicCast(OSBoolean, entry->getProperty("Writable"));
    major = entryMajor->unsigned32BitValue();
    minor = entryMinor->unsigned32BitValue();
    strlcpy(name, bsdName->getCStringNoCopy(), sizeof(name));
    writable = !flag || flag->isTrue();
    found = true;
  }
  iterator->release();
  return found;
}

// The DW watchdog of the SoC. The firmware sets its period (about 89 s) and feeds it until it
// leaves boot services; restarting the count is writing 0x76 to CRR. The registers other than
// CRR are only read, to log what the hardware did: CCVR is the current count (it counts down
// from 2^(16+TOP)-1, so right after a restart it is close to 0x7FFFFFFF for TOP = 0xF).
constexpr uint64_t kWdtPhys = 0xFEAF0000ull;
constexpr uint32_t kWdtCr = 0x00;
constexpr uint32_t kWdtTorr = 0x04;
constexpr uint32_t kWdtCcvr = 0x08;
constexpr uint32_t kWdtCrr = 0x0C;
constexpr uint32_t kWdtStat = 0x10;
constexpr uint32_t kWdtRestart = 0x76;
constexpr uint32_t kWdtFeedMs = 5000;

void wdtKicker(void *argument, wait_result_t) {
  const uint32_t seconds = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(argument));
  vm_offset_t base = ml_io_map(static_cast<vm_offset_t>(kWdtPhys), 0x1000);
  if (!base) {
    IOLog("NativeNvme: wdt-kick: cannot map the watchdog at 0x%llx\n", ull(kWdtPhys));
    return;
  }
  volatile uint32_t *regs = reinterpret_cast<volatile uint32_t *>(base);
  IOLog("NativeNvme: wdt-kick: feeding the watchdog every %u ms for %u s (CR 0x%x TORR 0x%x CCVR 0x%08x STAT 0x%x)\n",
        kWdtFeedMs, seconds, regs[kWdtCr / 4], regs[kWdtTorr / 4], regs[kWdtCcvr / 4], regs[kWdtStat / 4]);
  const uint64_t end = nowUs() + static_cast<uint64_t>(seconds) * 1000000ull;
  for (uint32_t feed = 1; nowUs() < end; ++feed) {
    const uint32_t before = regs[kWdtCcvr / 4];
    regs[kWdtCrr / 4] = kWdtRestart;
    __asm__ volatile("dsb sy" ::: "memory");
    const uint32_t after = regs[kWdtCcvr / 4];
    // The first feeds, then once a minute: enough to see that the count restarts, and the last
    // line before an unexplained reset says how long ago the last feed was.
    if (feed <= 3 || feed % 12 == 0) {
      IOLog("NativeNvme: wdt-kick: feed %u: count 0x%08x -> 0x%08x (CR 0x%x STAT 0x%x)\n", feed, before, after,
            regs[kWdtCr / 4], regs[kWdtStat / 4]);
    }
    IOSleep(kWdtFeedMs);
  }
  IOLog("NativeNvme: wdt-kick: %u s are over, not feeding the watchdog any more\n", seconds);
}

// The ApplePMGRNub the original AMFI waits for.
//
// AppleMobileFileIntegrity's configuration (ConfigurationSettings.cpp) finds the power manager with
// IOService::waitForMatchingService(serviceMatching("ApplePMGRNub")), 30 s after it starts, and panics
// "AMFI: No PMGR?" when nothing answers (a board run: launchd had been running for 27 s).
// What it then does with the nub is fixed by the original code (the PMGR audit script of
// UniXNU pins the instructions): read the nub's always-on word through the provider stored at nub+0x88,
// OR bit 2 into it, write it back. The helpers behind that return 0xffffffff and do nothing when the
// provider is NULL, so a REAL nub of the original class without any provider satisfies it: this board
// has no Apple power manager, and nothing here pretends to have one. UniXNU's sirius port found the
// same thing (its docs: "publishing a real but provider-less ApplePMGRNub satisfies the latch").
// Refused, with a log line, unless the class is exactly the original one.

constexpr unsigned kPmgrNubClassSize = 0x740;      // ApplePMGRNub of the pinned kernelcache
constexpr unsigned kPmgrNubProviderOffset = 0x88;  // where the original code keeps the nub's provider

void pmgrNubPublisher(void *, wait_result_t) {
  // The kext is a dependency of AMFI and normally started already; loading it again is a no-op then.
  kern_return_t loaded = OSKextLoadKextWithIdentifier("com.apple.driver.ApplePMGR");
  const OSMetaClass *meta = nullptr;
  for (unsigned tries = 0; tries < 100 && !meta; ++tries) {
    const OSSymbol *name = OSSymbol::withCString("ApplePMGRNub");
    meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    if (name) name->release();
    if (!meta) IOSleep(100);
  }
  if (!meta) {
    IOLog("NativeNvme: pmgr-nub: no class ApplePMGRNub (loading com.apple.driver.ApplePMGR returned 0x%x)\n", loaded);
    return;
  }
  if (meta->getClassSize() != kPmgrNubClassSize) {
    IOLog("NativeNvme: pmgr-nub: ApplePMGRNub is 0x%x bytes, expected 0x%x: not the original of the pinned kernelcache, not publishing\n",
          meta->getClassSize(), kPmgrNubClassSize);
    return;
  }
  OSObject *object = meta->alloc();
  IOService *nub = OSDynamicCast(IOService, object);
  IOService *platform = IOService::getPlatform();
  if (!nub || !platform || !nub->init(static_cast<OSDictionary *>(nullptr)) || !nub->attach(platform)) {
    IOLog("NativeNvme: pmgr-nub: cannot create the nub (object %p, platform %p)\n", object, platform);
    if (object) object->release();
    return;
  }
  uintptr_t provider = 0;
  memcpy(&provider, reinterpret_cast<const char *>(nub) + kPmgrNubProviderOffset, sizeof(provider));
  if (provider) {
    // Not provider-less after all: the layout is not the one this was written against.
    IOLog("NativeNvme: pmgr-nub: the new nub already has a provider pointer 0x%llx, not publishing\n", ull(provider));
    nub->detach(platform);
    nub->release();
    return;
  }
  nub->registerService(kIOServiceSynchronous);
  IOLog("NativeNvme: pmgr-nub: published an original ApplePMGRNub without provider (AMFI reads 0xffffffff from it)\n");
}

// The original IOBlockStorageDevice must be the class this file was compiled against, or a
// subclass would overlay its members on the base class's. The audit proves it for the pinned
// kernelcache; this proves it for the kernel that is running.
bool originalStorageClassMatches() {
  const OSMetaClass *base = &IOBlockStorageDevice::gMetaClass;
  const OSMetaClass *service = &IOService::gMetaClass;
  unsigned baseSize = base->getClassSize(), serviceSize = service->getClassSize();
  bool linked = base->getSuperClass() == service;
  IOLog("NativeNvme: original IOBlockStorageDevice: class size 0x%x (expect 0x%x), IOService 0x%x (expect 0x%x), parent link %s\n",
        baseSize, static_cast<unsigned>(sizeof(IOBlockStorageDevice)), serviceSize, static_cast<unsigned>(sizeof(IOService)),
        linked ? "ok" : "WRONG");
  return baseSize == sizeof(IOBlockStorageDevice) && serviceSize == sizeof(IOService) && linked;
}

}  // namespace

NativeNvmeBlock *NativeNvme::publishWindow(const char *label, uint64_t first, uint64_t blocks, unsigned role, bool writable) {
  if (!originalStorageClassMatches()) {
    IOLog("NativeNvme: refusing to publish %s: the original storage classes are not the ones the driver was built for\n", label);
    return nullptr;
  }
  NativeNvmeBlock *device = OSTypeAlloc(NativeNvmeBlock);
  if (!device || !device->init(static_cast<OSDictionary *>(nullptr)) || !device->configure(this, label, first, blocks, writable) ||
      !device->attach(this)) {
    IOLog("NativeNvme: cannot prepare block device %s\n", label);
    if (device) device->release();
    return nullptr;
  }
  NativeNvmeBlock **slot = role == 1 ? &gOsWindow : role == 2 ? &gSystemOsWindow : role == 3 ? &gDataWindow : nullptr;
  if (slot) __atomic_store_n(slot, device, __ATOMIC_RELEASE);
  if (!device->start(this)) {
    IOLog("NativeNvme: block device %s did not start\n", label);
    if (slot) __atomic_store_n(slot, static_cast<NativeNvmeBlock *>(nullptr), __ATOMIC_RELEASE);
    device->detach(this);
    device->release();
    return nullptr;
  }
  // Started objects keep their reference: IOMedia clients hold the device for the life of the system.
  __atomic_fetch_add(&native_emmc_blocks_published, 1, __ATOMIC_RELAXED);
  IOLog("NativeNvme: published %s: lba %llu + %llu blocks (%llu MiB), %s\n", device->windowLabel(), ull(first), ull(blocks),
        ull(blocks >> 11), writable ? "WRITABLE" : "read-only");
  return device;
}

// The Data RAM disk as a writable block device of its own. It hangs off the platform expert, not
// off the drive: nothing about it depends on the controller, and a run without a drive can still look
// at it. Like the root windows, a started device keeps its reference for the life of the system.
static NativeNvmeBlock *publishRamDisk(IOService *provider, const char *label, uint8_t *memory, uint64_t bytes) {
  if (!originalStorageClassMatches()) {
    IOLog("NativeNvme: refusing to publish %s: the original storage classes are not the ones the driver was built for\n", label);
    return nullptr;
  }
  NativeNvmeBlock *device = OSTypeAlloc(NativeNvmeBlock);
  if (!device || !device->init(static_cast<OSDictionary *>(nullptr)) || !device->configureRam(label, memory, bytes) ||
      !device->attach(provider)) {
    IOLog("NativeNvme: cannot prepare RAM block device %s\n", label);
    if (device) device->release();
    return nullptr;
  }
  __atomic_store_n(&gDataWindow, device, __ATOMIC_RELEASE);
  if (!device->start(provider)) {
    IOLog("NativeNvme: RAM block device %s did not start\n", label);
    __atomic_store_n(&gDataWindow, static_cast<NativeNvmeBlock *>(nullptr), __ATOMIC_RELEASE);
    device->detach(provider);
    device->release();
    return nullptr;
  }
  __atomic_fetch_add(&native_emmc_blocks_published, 1, __ATOMIC_RELAXED);
  native_emmc_data_ready = 1;
  IOLog("NativeNvme: published %s: RAM %p + %llu bytes (%llu MiB), writable\n", device->windowLabel(), static_cast<void *>(memory), ull(bytes),
        ull(bytes >> 20));
  return device;
}

// The range of the Data seed the payload hands over: /chosen/memory-map/RAMDisk is {physical base,
// size}, both u64, written by the loader (UniXNU's --data-ramdisk). The same bounds check the original
// RamBlock driver makes: page aligned, inside the memory the kernel manages, nowhere else.
static uint8_t *dataDiskRange(uint64_t &bytes) {
  bytes = 0;
  IORegistryEntry *map = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
  if (!map) {
    IOLog("NativeNvme: data-ram: no /chosen/memory-map\n");
    return nullptr;
  }
  OSData *range = OSDynamicCast(OSData, map->getProperty("RAMDisk"));
  uint64_t physical = 0, size = 0;
  if (range && range->getLength() == 16) {
    memcpy(&physical, range->getBytesNoCopy(), 8);
    memcpy(&size, static_cast<const uint8_t *>(range->getBytesNoCopy()) + 8, 8);
  }
  map->release();
  if (!size) {
    IOLog("NativeNvme: data-ram: the payload hands over no RAMDisk\n");
    return nullptr;
  }
  if (size > 0x80000000ull || (physical & 4095) || (size & 4095) || physical < gPhysBase || size > gPhysSize ||
      physical - gPhysBase > gPhysSize - size) {
    IOLog("NativeNvme: data-ram: RAMDisk %#llx + %#llx is not boot memory the kernel manages (%#lx + %#lx)\n", ull(physical), ull(size),
          gPhysBase, gPhysSize);
    return nullptr;
  }
  uint8_t *memory = reinterpret_cast<uint8_t *>(ml_static_ptovirt(static_cast<vm_offset_t>(physical)));
  if (!memory) {
    IOLog("NativeNvme: data-ram: no static mapping for %#llx\n", ull(physical));
    return nullptr;
  }
  bytes = size;
  IOLog("NativeNvme: data-ram: RAMDisk physical %#llx, %llu bytes, at %p; header %02x%02x%02x%02x %02x%02x%02x%02x (NXSB at +32: %02x%02x%02x%02x)\n",
        ull(physical), ull(size), static_cast<void *>(memory), memory[0], memory[1], memory[2], memory[3], memory[4], memory[5], memory[6], memory[7],
        memory[32], memory[33], memory[34], memory[35]);
  return memory;
}

// The seed on the payload is only the front of the Data container: an empty APFS container of 512 MiB has
// 18 blocks that are not zero, all of them in the first 1.7 MiB, and the payload (and the fastboot buffer it
// rides in) has no room for the other 510 MiB of zeros. With nvme-data-size=N the block device is N MiB:
// the seed is copied to the front of a fresh allocation and the rest is zero. The container inside says how
// large it is itself (its superblock), so N has to be the size it was made for.
static uint8_t *growDataDisk(uint8_t *seed, uint64_t &bytes) {
  const uint64_t wanted = static_cast<uint64_t>(bootArg("nvme-data-size", 0)) << 20;
  if (!wanted || wanted == bytes) return seed;
  if (wanted < bytes || wanted > (4ull << 30)) {
    IOLog("NativeNvme: data-ram: nvme-data-size %llu MiB is not usable with a seed of %llu MiB\n", ull(wanted >> 20), ull(bytes >> 20));
    return seed;
  }
  IOBufferMemoryDescriptor *memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut, wanted, page_size);
  uint8_t *grown = memory ? static_cast<uint8_t *>(memory->getBytesNoCopy()) : nullptr;
  if (!grown) {
    IOLog("NativeNvme: data-ram: no memory for %llu MiB\n", ull(wanted >> 20));
    if (memory) memory->release();
    return seed;
  }
  memcpy(grown, seed, bytes);
  bzero(grown + bytes, wanted - bytes);
  IOLog("NativeNvme: data-ram: %llu MiB of kernel memory at %p: the %llu MiB seed in front, zeros behind\n", ull(wanted >> 20),
        static_cast<void *>(grown), ull(bytes >> 20));
  bytes = wanted;  // the descriptor is kept for the life of the system, like the disk
  return grown;
}

// Runs on a thread of its own: publishing is followed by waiting for the partition scheme and APFS,
// which the thread that starts the controller would be holding up.
static void dataDiskPublisher(void *, wait_result_t) {
  uint64_t bytes = 0;
  uint8_t *memory = dataDiskRange(bytes);
  if (!memory) return;
  memory = growDataDisk(memory, bytes);
  IOService *platform = IOService::getPlatform();
  if (!platform) {
    IOLog("NativeNvme: data-ram: no platform expert to attach to\n");
    return;
  }
  NativeNvmeBlock *device = publishRamDisk(platform, "Data", memory, bytes);
  if (!device) return;
  unsigned media = settleMedia(device, 20);
  IOLog("NativeNvme: Data settled with %u IOMedia below it, %llu ram requests (%llu writes), %llu failures\n", media,
        ull(native_emmc_ram_requests), ull(native_emmc_ram_writes), ull(native_emmc_ram_failures));
  mediaBelow(device, true);
}

// ---- the Data volume on the drive (nvme-data-persist=1) -----------------------------------------------------
//
// Where: the END of `userdata`, so that nothing else ever has to move. A window of
//     kDataHeaderBlocks (1 MiB) of header, then the APFS container of the payload's seed, whose size is the
//     seed's own (nx_block_size * nx_block_count at +36/+40 of its superblock). It must lie at least 64 MiB behind
//     the end of the root image, and inside `userdata`; otherwise nothing is written and the RAM disk is used.
// What is written, and when: only inside that window, only by this thread and the Data block device, and only
//     after the checks above (NativeNvme::enableWrites gives the host core that one range, the core refuses
//     the rest). A first boot formats: header "formatting", the first 64 MiB of the container zeroed (the old
//     contents of the window must not look like APFS objects), the seed written over its front, flushed, read
//     back and compared, header "ready". A header that says "formatting" (power loss) is formatted again.
// Header (one 512-byte block, little endian): +0 "EVB10DAT", +8 version, +12 state (1 formatting, 2 ready),
//     +16 container blocks (u64), +24 seed bytes (u64), +32 crc32 of the seed, +36 number of formats so far,
//     +508 crc32 of bytes 0..507.
namespace {

constexpr uint32_t kDataHeaderBlocks = 2048;        // 1 MiB
constexpr uint64_t kDataMarginBlocks = 131072;      // 64 MiB between the root image and the window
constexpr uint64_t kDataZeroBlocks = 131072;        // 64 MiB zeroed in front of the container when formatting
constexpr uint64_t kDataMaxBytes = 64ull << 30;
constexpr uint32_t kDataVersion = 1;
constexpr uint32_t kDataFormatting = 1, kDataReady = 2;
constexpr char kDataMagic[8] = {'E', 'V', 'B', '1', '0', 'D', 'A', 'T'};

struct DataHeader {
  uint32_t version = 0, state = 0;
  uint64_t dataBlocks = 0, seedBytes = 0;
  uint32_t seedCrc = 0, generation = 0;
};

void encodeDataHeader(uint8_t (&block)[storage::kBlockBytes], const DataHeader &header) {
  bzero(block, sizeof(block));
  memcpy(block, kDataMagic, 8);
  memcpy(block + 8, &header.version, 4);
  memcpy(block + 12, &header.state, 4);
  memcpy(block + 16, &header.dataBlocks, 8);
  memcpy(block + 24, &header.seedBytes, 8);
  memcpy(block + 32, &header.seedCrc, 4);
  memcpy(block + 36, &header.generation, 4);
  const uint32_t crc = storage::crc32(block, 508);
  memcpy(block + 508, &crc, 4);
}

bool decodeDataHeader(const uint8_t (&block)[storage::kBlockBytes], DataHeader &header) {
  if (memcmp(block, kDataMagic, 8)) return false;
  uint32_t crc = 0;
  memcpy(&crc, block + 508, 4);
  if (crc != storage::crc32(block, 508)) return false;
  memcpy(&header.version, block + 8, 4);
  memcpy(&header.state, block + 12, 4);
  memcpy(&header.dataBlocks, block + 16, 8);
  memcpy(&header.seedBytes, block + 24, 8);
  memcpy(&header.seedCrc, block + 32, 4);
  memcpy(&header.generation, block + 36, 4);
  return true;
}

}  // namespace

// Called with nothing held, from dataPublisher. `seed` is `seedBytes` of the payload's RAM disk.
bool NativeNvme::formatData(uint64_t windowFirst, uint64_t dataBlocks, const uint8_t *seed, uint64_t seedBytes, uint32_t seedCrc,
                            uint32_t generation) {
  const uint64_t begin = nowUs();
  const uint64_t dataFirst = windowFirst + kDataHeaderBlocks;
  alignas(8) uint8_t block[storage::kBlockBytes];
  uint8_t *buffer = static_cast<uint8_t *>(IOMallocData(kChunkBytes));
  if (!buffer) return false;
  bool ok = true;
  DataHeader header;
  header.version = kDataVersion;
  header.state = kDataFormatting;
  header.dataBlocks = dataBlocks;
  header.seedBytes = seedBytes;
  header.seedCrc = seedCrc;
  header.generation = generation;
  encodeDataHeader(block, header);
  if (writeBlocks(windowFirst, 1, block) != kIOReturnSuccess || flush() != kIOReturnSuccess) {
    IOLog("NativeNvme: data: cannot write the header of the Data window\n");
    ok = false;
  }
  const uint64_t zeroBlocks = dataBlocks < kDataZeroBlocks ? dataBlocks : kDataZeroBlocks;
  bzero(buffer, kChunkBytes);
  for (uint64_t at = 0; ok && at < zeroBlocks; at += kChunkBlocks) {
    const uint32_t count = static_cast<uint32_t>(zeroBlocks - at < kChunkBlocks ? zeroBlocks - at : kChunkBlocks);
    if (writeBlocks(dataFirst + at, count, buffer) != kIOReturnSuccess) {
      IOLog("NativeNvme: data: zeroing failed at block %llu of the window\n", ull(at));
      ok = false;
    }
  }
  const uint64_t zeroed = nowUs();
  for (uint64_t at = 0; ok && at < seedBytes; at += kChunkBytes) {
    const uint64_t bytes = seedBytes - at < kChunkBytes ? seedBytes - at : kChunkBytes;
    memcpy(buffer, seed + at, static_cast<size_t>(bytes));
    if (writeBlocks(dataFirst + at / storage::kBlockBytes, static_cast<uint32_t>(bytes / storage::kBlockBytes), buffer) != kIOReturnSuccess) {
      IOLog("NativeNvme: data: writing the seed failed at byte %llu\n", ull(at));
      ok = false;
    }
  }
  if (ok && flush() != kIOReturnSuccess) {
    IOLog("NativeNvme: data: the flush after the seed failed\n");
    ok = false;
  }
  // What the drive holds is what was meant: the seed read back, through the same path the kernel will read it by.
  if (ok) {
    uint32_t crc = 0;
    for (uint64_t at = 0; ok && at < seedBytes; at += kChunkBytes) {
      const uint64_t bytes = seedBytes - at < kChunkBytes ? seedBytes - at : kChunkBytes;
      if (readBlocks(dataFirst + at / storage::kBlockBytes, static_cast<uint32_t>(bytes / storage::kBlockBytes), buffer) != kIOReturnSuccess) {
        IOLog("NativeNvme: data: read-back failed at byte %llu\n", ull(at));
        ok = false;
        break;
      }
      crc = storage::crc32(buffer, static_cast<size_t>(bytes), crc);
    }
    if (ok && crc != seedCrc) {
      IOLog("NativeNvme: data: the seed read back from the drive differs from what was written (crc32 0x%08x, expected 0x%08x)\n", crc, seedCrc);
      ok = false;
    }
  }
  if (ok) {
    header.state = kDataReady;
    encodeDataHeader(block, header);
    if (writeBlocks(windowFirst, 1, block) != kIOReturnSuccess || flush() != kIOReturnSuccess) {
      IOLog("NativeNvme: data: cannot write the ready header\n");
      ok = false;
    }
  }
  IOFreeData(buffer, kChunkBytes);
  IOLog("NativeNvme: data: formatted the Data window at lba %llu (%llu MiB zeroed in %llu ms, seed of %llu KiB, all in %llu ms): %s\n",
        ull(windowFirst), ull(zeroBlocks >> 11), ull((zeroed - begin) / 1000), ull(seedBytes >> 10), ull((nowUs() - begin) / 1000),
        ok ? "ready" : "FAILED");
  return ok;
}

// Decides, formats if needed, and publishes. false = nothing was published (and nothing is writable any more).
bool NativeNvme::persistentData() {
  uint64_t seedBytes = 0;
  uint8_t *seed = dataDiskRange(seedBytes);
  if (!seed) return false;
  if (!rootReady || !userdataLast) {
    IOLog("NativeNvme: data: persistent Data needs nvme=4 and a usable root image (rootReady %d)\n", rootReady ? 1 : 0);
    return false;
  }
  if (!native_emmc_dma) {
    IOLog("NativeNvme: data: persistent Data needs DMA, which is off\n");
    return false;
  }
  // The container the seed describes: nx_superblock_t, nx_magic at +32, nx_block_size at +36, nx_block_count at +40.
  uint32_t apfsBlock = 0;
  uint64_t apfsCount = 0;
  if (seedBytes >= 4096 && !memcmp(seed + 32, "NXSB", 4)) {
    memcpy(&apfsBlock, seed + 36, 4);
    memcpy(&apfsCount, seed + 40, 8);
  }
  if (apfsBlock < 4096 || apfsBlock > 65536 || (apfsBlock & (apfsBlock - 1)) || !apfsCount || apfsCount > kDataMaxBytes / apfsBlock) {
    IOLog("NativeNvme: data: the seed is no APFS container (block size %u, %llu blocks)\n", apfsBlock, ull(apfsCount));
    return false;
  }
  const uint64_t containerBytes = static_cast<uint64_t>(apfsBlock) * apfsCount;
  if ((containerBytes & 0xfffff) || containerBytes < seedBytes || containerBytes < (64ull << 20)) {
    IOLog("NativeNvme: data: a container of %llu bytes does not fit a seed of %llu bytes, or is not whole MiB, or is under 64 MiB\n",
          ull(containerBytes), ull(seedBytes));
    return false;
  }
  const uint64_t dataBlocks = containerBytes / storage::kBlockBytes;
  const uint32_t sizeArg = bootArg("nvme-data-size", 0);
  if (sizeArg && (static_cast<uint64_t>(sizeArg) << 20) != containerBytes) {
    IOLog("NativeNvme: data: nvme-data-size=%u is not the seed's container (%llu MiB); the container decides\n", sizeArg, ull(containerBytes >> 20));
  }
  const uint64_t total = kDataHeaderBlocks + dataBlocks;
  const uint64_t userdataBlocks = userdataLast - userdataFirst + 1;
  if (total > userdataBlocks) {
    IOLog("NativeNvme: data: %llu MiB do not fit userdata (%llu MiB)\n", ull(total >> 11), ull(userdataBlocks >> 11));
    return false;
  }
  const uint64_t windowFirst = userdataLast + 1 - total;
  const uint64_t rootEnd = rootLba + (root.imageBytes + storage::kBlockBytes - 1) / storage::kBlockBytes;
  if (windowFirst < rootEnd + kDataMarginBlocks) {
    IOLog("NativeNvme: data: the window at %llu would come closer than 64 MiB to the end of the root image (%llu); not writing\n",
          ull(windowFirst), ull(rootEnd));
    return false;
  }
  IOLog("NativeNvme: data: window lba %llu..%llu (%llu MiB: 1 MiB header + %llu MiB container of %u-byte blocks), root image ends at %llu\n",
        ull(windowFirst), ull(userdataLast), ull(total >> 11), ull(containerBytes >> 20), apfsBlock, ull(rootEnd));

  const uint32_t seedCrc = storage::crc32(seed, static_cast<size_t>(seedBytes));
  alignas(8) uint8_t block[storage::kBlockBytes];
  DataHeader header;
  const bool haveHeader = readBlocks(windowFirst, 1, block) == kIOReturnSuccess && decodeDataHeader(block, header);
  const bool reset = bootArg("nvme-data-reset", 0) != 0;
  const bool keep = haveHeader && !reset && header.version == kDataVersion && header.state == kDataReady && header.dataBlocks == dataBlocks;
  if (haveHeader) {
    IOLog("NativeNvme: data: header: version %u state %u, %llu blocks, seed crc 0x%08x, formatted %u times%s\n", header.version, header.state,
          ull(header.dataBlocks), header.seedCrc, header.generation, reset ? " (nvme-data-reset=1)" : "");
  } else {
    IOLog("NativeNvme: data: no header in the window%s: formatting\n", reset ? " (nvme-data-reset=1)" : "");
  }
  if (!enableWrites(windowFirst, userdataLast)) {
    IOLog("NativeNvme: data: the host core would not open the window for writing\n");
    return false;
  }
  if (keep) {
    if (header.seedCrc != seedCrc) {
      IOLog("NativeNvme: data: it was formatted from another seed (crc32 0x%08x, this payload's is 0x%08x); the contents are kept\n", header.seedCrc,
            seedCrc);
    }
    native_emmc_data_seeded = 1;
  } else if (!formatData(windowFirst, dataBlocks, seed, seedBytes, seedCrc, haveHeader ? header.generation + 1 : 1)) {
    disableWrites();
    return false;
  }
  native_emmc_data_persistent = 1;
  NativeNvmeBlock *device = publishWindow("Data", windowFirst + kDataHeaderBlocks, dataBlocks, 3, true);
  if (!device) {
    native_emmc_data_persistent = 0;
    native_emmc_data_seeded = 0;
    disableWrites();
    return false;
  }
  native_emmc_data_ready = 1;
  unsigned media = settleMedia(device, 20);
  IOLog("NativeNvme: Data (on the drive) settled with %u IOMedia below it, %llu write calls (%llu blocks), %llu failures\n", media,
        ull(native_emmc_write_calls), ull(native_emmc_write_blocks), ull(native_emmc_write_failures));
  mediaBelow(device, true);
  return true;
}

void NativeNvme::dataPublisher(void *self, wait_result_t) {
  NativeNvme *controller = static_cast<NativeNvme *>(self);
  if (!controller->persistentData()) {
    IOLog("NativeNvme: data: no persistent Data volume (see above): the Data RAM disk instead\n");
    if (bootArg("nvme-data-ram", 1)) dataDiskPublisher(nullptr, 0);
  }
  controller->release();  // the reference start() took for this thread
}

// The boot-manifest-hash of /chosen. On a phone iBoot publishes it (the SHA-384 of the img4 manifest
// it booted from) and the original `mount -P 1` cannot go on without it: `kern.bootobjectspath` is the
// ENOENT behind "failed to get boot manifest hash", the exit status 66 of that boot task is a critical
// failure, and launchd panics ("mount[3] exited"). UniXNU's own offline runs hit exactly that and
// publish an owner hash from the verified root volumes (patches/xnu/offline-owner-boot-manifest.patch).
// There is no iBoot here either, so the hash names what was booted from the owner's record of it:
// the SHA-256 of the whole OS window and of the whole SystemOS window, as the root manifest lists
// them (the manifest itself is checked against its own SHA-256 before anything is used), role byte
// and digest per volume under a domain of our own, SHA-384 over all of it, the construction of the
// official patch. An identity that is already in /chosen is never replaced.
// boot-arg nvme-boot-manifest=0 leaves it out.
static bool publishBootManifestHash(const storage::RootManifest &manifest) {
  static const char domain[] = "EmmcRootImage.OwnerBootSet.v1";  // the NUL is part of it. Unchanged from the eMMC driver on purpose: the same root image gives the same identity
  const storage::RootWindow *os = nullptr, *system = nullptr;
  for (unsigned i = 0; i < manifest.count; ++i) {
    if (storage::sameName(manifest.window[i].name, "OS")) os = &manifest.window[i];
    if (storage::sameName(manifest.window[i].name, "SystemOS")) system = &manifest.window[i];
  }
  if (!os || !system) {
    IOLog("NativeNvme: boot-manifest-hash: the root image has no %s window, nothing to derive it from\n", os ? "SystemOS" : "OS");
    return false;
  }
  const uint8_t osRole = 1, systemRole = 2;
  uint8_t digest[SHA384_DIGEST_LENGTH];
  SHA384_CTX context;
  SHA384_Init(&context);
  SHA384_Update(&context, domain, sizeof(domain));
  SHA384_Update(&context, &osRole, sizeof(osRole));
  SHA384_Update(&context, os->sha256, sizeof(os->sha256));
  SHA384_Update(&context, &systemRole, sizeof(systemRole));
  SHA384_Update(&context, system->sha256, sizeof(system->sha256));
  SHA384_Final(digest, &context);

  IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
  if (!chosen) {
    IOLog("NativeNvme: boot-manifest-hash: no /chosen\n");
    return false;
  }
  bool published = false;
  if (chosen->getProperty("boot-manifest-hash") || chosen->getProperty("owner-boot-manifest-domain")) {
    IOLog("NativeNvme: boot-manifest-hash: /chosen already carries an identity, not replaced\n");
  } else {
    OSData *value = OSData::withBytes(digest, sizeof(digest));
    OSData *tag = OSData::withBytes(domain, sizeof(domain));
    published = value && tag && chosen->setProperty("owner-boot-manifest-domain", tag) && chosen->setProperty("boot-manifest-hash", value);
    OSSafeReleaseNULL(tag);
    OSSafeReleaseNULL(value);
  }
  chosen->release();
  if (published) {
    static const char digits[] = "0123456789abcdef";
    char text[2 * sizeof(digest) + 1];
    for (unsigned i = 0; i < sizeof(digest); ++i) {
      text[2 * i] = digits[digest[i] >> 4];
      text[2 * i + 1] = digits[digest[i] & 15];
    }
    text[2 * sizeof(digest)] = 0;
    IOLog("NativeNvme: boot-manifest-hash published in /chosen: %s (domain %s)\n", text, domain);
  }
  return published;
}

// The manifest of the root image: read at `rootLba` (the start of `userdata` unless a boot-arg
// says otherwise), checked by RootManifest.h, and turned into the order the windows go out in.
// Called with the gate held.
bool NativeNvme::findRoot() {
  rootLba = bootArg64("nvme-root-lba", userdataFirst);
  if (!userdataLast || rootLba < userdataFirst || rootLba > userdataLast) {
    IOLog("NativeNvme: no room for a root image (userdata %llu..%llu, manifest at %llu)\n", ull(userdataFirst), ull(userdataLast),
          ull(rootLba));
    return false;
  }
  uint8_t *page = static_cast<uint8_t *>(IOMallocData(storage::kRootManifestBytes));
  if (!page) return false;
  bool usable = false;
  if (host.readBlocks(rootLba, storage::kRootManifestBytes / storage::kBlockBytes, page) != nvme::kOk) {
    IOLog("NativeNvme: cannot read the root manifest at block %llu\n", ull(rootLba));
  } else {
    const uint64_t partitionBytes = (userdataLast - rootLba + 1) * storage::kBlockBytes;
    int status = storage::parseRootManifest(page, partitionBytes, sha256, root);
    native_emmc_root_status = status;
    if (status != storage::kRootOk) {
      IOLog("NativeNvme: block %llu holds no usable root image: %s\n", ull(rootLba), storage::rootStatusName(status));
    } else {
      IOLog("NativeNvme: root image at block %llu: %u windows, %llu bytes of %llu\n", ull(rootLba), root.count,
            ull(root.imageBytes), ull(partitionBytes));
      for (unsigned i = 0; i < root.count; ++i) {
        IOLog("NativeNvme:   window \"%s\" offset 0x%llx size %llu bytes (%llu MiB)\n", root.window[i].name, ull(root.window[i].offset),
              ull(root.window[i].size), ull(root.window[i].size >> 20));
      }
      usable = storage::buildRootPlan(root, rootPlan);
      if (!usable) {
        IOLog("NativeNvme: the root image has no \"OS\" window\n");
      } else {
        IOLog("NativeNvme: publication order:");
        for (unsigned i = 0; i < rootPlan.count; ++i) IOLog(" %s", root.window[rootPlan.order[i]].name);
        IOLog("\n");
        if (bootArg("nvme-boot-manifest", 1)) publishBootManifestHash(root);
      }
    }
  }
  IOFreeData(page, storage::kRootManifestBytes);
  return usable;
}

// Publishes the windows in plan order, each after the previous one has settled: the kernel
// goes looking for the SystemOS volume as soon as it has mounted the root, so by then that one
// must be there. Runs on a thread of its own because settling means waiting for matching
// that the thread starting the controller would be holding up.
void NativeNvme::publishRoot() {
  for (unsigned position = 0; position < rootPlan.count; ++position) {
    const storage::RootWindow &window = root.window[rootPlan.order[position]];
    NativeNvmeBlock *device = publishWindow(window.name, rootLba + window.offset / storage::kBlockBytes,
                                            window.size / storage::kBlockBytes,
                                            storage::sameName(window.name, "OS") ? 1u : storage::sameName(window.name, "SystemOS") ? 2u : 0u);
    if (!device) {
      IOLog("NativeNvme: root publisher stops: %s did not publish\n", window.name);
      return;
    }
    __atomic_fetch_add(&native_emmc_root_windows, 1, __ATOMIC_RELAXED);
    unsigned media = settleMedia(device, 20);
    IOLog("NativeNvme: %s settled with %u IOMedia below it, %llu block requests, %llu failures\n", window.name, media,
          ull(native_emmc_block_requests), ull(native_emmc_block_failures));
    mediaBelow(device, true);
  }
  IOLog("NativeNvme: all %u root windows published\n", rootPlan.count);
}

void NativeNvme::rootPublisher(void *self, wait_result_t) {
  NativeNvme *controller = static_cast<NativeNvme *>(self);
  controller->publishRoot();
  controller->release();  // the reference start() took for this thread
}

// bsd_init asks this after it mounted the root and Apple's FSIOC_KERNEL_ROOTAUTH said no. The
// answer is yes only for the volume the owner put in the "OS" window of the NVMe root image,
// and only if the owner said so on the command line: the trust is the owner's, not the kernel's.
extern "C" int native_emmc_root_trusted(int32_t dev) {
  const uint32_t major = (static_cast<uint32_t>(dev) >> 24) & 0xff, minor = static_cast<uint32_t>(dev) & 0xffffff;
  NativeNvmeBlock *window = __atomic_load_n(&gOsWindow, __ATOMIC_ACQUIRE);
  if (!window) {
    IOLog("NativeNvme: root device %u,%u is not trusted: this driver published no \"OS\" window\n", major, minor);
    return 0;
  }
  char name[32] = {};
  bool writable = true;
  if (!mediaByDevice(window, major, minor, name, writable)) {
    IOLog("NativeNvme: root device %u,%u is not trusted: it is no IOMedia below the \"OS\" window\n", major, minor);
    return 0;
  }
  if (writable) {
    IOLog("NativeNvme: root device %u,%u (%s) is not trusted: the media is writable\n", major, minor, name);
    return 0;
  }
  if (!bootArg("nvme-root-trust", 0)) {
    IOLog("NativeNvme: root device %u,%u (%s) is a read-only volume of the \"OS\" window, but boot-arg nvme-root-trust=1 is not set: "
          "not trusted\n", major, minor, name);
    return 0;
  }
  IOLog("NativeNvme: root device %u,%u (%s) is a read-only volume of the \"OS\" window: accepted without Apple's root authentication "
        "(boot-arg nvme-root-trust=1)\n", major, minor, name);
  return 1;
}

// The volume of the "SystemOS" window (the cryptex: dyld shared cache, system libraries) with
// this UUID: its device number and its /dev path, for the kernel's own mount of it over the root
// (UniXNU-evb10 patches/xnu/evb10-userland-mounts.patch, the counterpart of the original owner code's
// native_verified_block_volume_device). Returns 1 when the window has such a read-only volume,
// 0 otherwise. Only the owner's say-so (nvme-root-trust=1) makes it an answer at all.
extern "C" int native_emmc_volume_device(const char *uuid, int32_t *dev, char *path, size_t pathSize) {
  NativeNvmeBlock *window = __atomic_load_n(&gSystemOsWindow, __ATOMIC_ACQUIRE);
  if (!window || !uuid || !dev || !path || pathSize < 16) return 0;
  if (!bootArg("nvme-root-trust", 0)) {
    IOLog("NativeNvme: SystemOS volume %s not offered: boot-arg nvme-root-trust=1 is not set\n", uuid);
    return 0;
  }
  uint32_t major = 0, minor = 0;
  char name[32] = {};
  bool writable = true;
  if (!mediaByUuid(window, uuid, major, minor, name, writable)) {
    IOLog("NativeNvme: the \"SystemOS\" window has no volume %s\n", uuid);
    return 0;
  }
  if (writable) {
    IOLog("NativeNvme: SystemOS volume %s (%s) is writable: not offered\n", uuid, name);
    return 0;
  }
  *dev = static_cast<int32_t>((major << 24) | (minor & 0xffffff));
  snprintf(path, pathSize, "/dev/%s", name);
  IOLog("NativeNvme: SystemOS volume %s is %s (device %u,%u), read-only\n", uuid, path, major, minor);
  return 1;
}

// May the kernel treat `dev` as the SystemOS volume it mounts? Yes for a read-only IOMedia below the
// "SystemOS" window this driver published, with nvme-root-trust=1: the same decision, and the same
// owner, as native_emmc_root_trusted makes for the root.
extern "C" int native_emmc_cache_trusted(int32_t dev) {
  const uint32_t major = (static_cast<uint32_t>(dev) >> 24) & 0xff, minor = static_cast<uint32_t>(dev) & 0xffffff;
  NativeNvmeBlock *window = __atomic_load_n(&gSystemOsWindow, __ATOMIC_ACQUIRE);
  if (!window || !bootArg("nvme-root-trust", 0)) return 0;
  char name[32] = {};
  bool writable = true;
  if (!mediaByDevice(window, major, minor, name, writable) || writable) return 0;
  return 1;
}

// The volume of the Data RAM disk with this UUID: the BSD device and /dev path the kernel mounts on
// /private/var. Waits for it, for at most `nvme-data-wait` seconds (default 30): the partition scheme
// and APFS publish their media on threads of their own, a little after the RAM disk went out. No trust
// question here, the Data volume is the owner's by construction (it IS the payload's RAM disk); the
// answer is yes only for a WRITABLE volume inside the device this driver published.
extern "C" int native_emmc_data_device(const char *uuid, int32_t *dev, char *path, size_t pathSize) {
  if (!uuid || !dev || !path || pathSize < 16) return 0;
  const uint32_t seconds = bootArg("nvme-data-wait", 30);
  uint32_t major = 0, minor = 0;
  char name[32] = {};
  bool writable = false, found = false;
  NativeNvmeBlock *window = nullptr;
  for (uint32_t tick = 0; tick <= seconds * 10; ++tick) {
    window = __atomic_load_n(&gDataWindow, __ATOMIC_ACQUIRE);
    if (window && mediaByUuid(window, uuid, major, minor, name, writable)) {
      found = true;
      break;
    }
    if (tick < seconds * 10) IOSleep(100);
  }
  if (!window) {
    IOLog("NativeNvme: no Data RAM disk was published (waited %u s)\n", seconds);
    return 0;
  }
  if (!found) {
    IOLog("NativeNvme: the Data RAM disk has no volume %s (waited %u s)\n", uuid, seconds);
    mediaBelow(window, true);
    return 0;
  }
  if (!writable) {
    IOLog("NativeNvme: Data volume %s (%s) is not writable: not offered\n", uuid, name);
    return 0;
  }
  *dev = static_cast<int32_t>((major << 24) | (minor & 0xffffff));
  snprintf(path, pathSize, "/dev/%s", name);
  IOLog("NativeNvme: Data volume %s is %s (device %u,%u), writable\n", uuid, path, major, minor);
  return 1;
}

bool NativeNvme::scanGpt() {
  uint8_t *scratch = static_cast<uint8_t *>(IOMallocData(storage::kGptScratchBytes));
  storage::GptEntry *entries = static_cast<storage::GptEntry *>(IOMallocData(sizeof(storage::GptEntry) * storage::kGptMaxEntries));
  if (!scratch || !entries) {
    if (scratch) IOFreeData(scratch, storage::kGptScratchBytes);
    if (entries) IOFreeData(entries, sizeof(storage::GptEntry) * storage::kGptMaxEntries);
    return false;
  }
  storage::GptInfo info;
  int status = storage::readGpt(
      [this](uint64_t lba, uint32_t count, void *out) { return host.readBlocks(lba, count, out) == nvme::kOk; },
      scratch, info, entries, storage::kGptMaxEntries);
  if (status == storage::kGptOk) {
    char guid[37];
    storage::formatGuid(info.diskGuid, guid);
    diskBlocks = info.altLba + 1;
    IOLog("NativeNvme: GPT disk %s: my=%llu alt=%llu (%llu blocks, %llu MiB) usable %llu..%llu, %u x %u entries at %llu; "
          "header crc %s, entries crc %s\n",
          guid, ull(info.myLba), ull(info.altLba), ull(diskBlocks), ull(diskBlocks >> 11), ull(info.firstUsable),
          ull(info.lastUsable), info.count, info.entrySize, ull(info.entriesLba), info.headerCrcOk ? "ok" : "BAD",
          info.entriesCrcOk ? "ok" : "BAD");
    for (uint32_t i = 0; i < info.used; ++i) {
      const storage::GptEntry &entry = entries[i];
      char type[37];
      storage::formatGuid(entry.typeGuid, type);
      IOLog("NativeNvme: gpt[%u] \"%s\" lba %llu-%llu (%llu MiB) type %s\n", entry.slot, entry.name, ull(entry.first),
            ull(entry.last), ull(entry.last >= entry.first ? (entry.last - entry.first + 1) >> 11 : 0), type);
      if (storage::sameName(entry.name, "userdata") && entry.last >= entry.first) {
        userdataFirst = entry.first;
        userdataLast = entry.last;
      }
    }
    IOLog("NativeNvme: %u partitions\n", info.used);
  } else {
    IOLog("NativeNvme: no usable GPT (status %d)\n", status);
  }
  IOFreeData(scratch, storage::kGptScratchBytes);
  IOFreeData(entries, sizeof(storage::GptEntry) * storage::kGptMaxEntries);
  return status == storage::kGptOk;
}

void NativeNvme::probeUserdata() {
  if (!userdataLast) {
    IOLog("NativeNvme: no partition named \"userdata\"\n");
    return;
  }
  native_emmc_userdata_first = userdataFirst;
  native_emmc_userdata_last = userdataLast;
  alignas(8) uint8_t block[storage::kBlockBytes];
  if (host.readBlocks(userdataFirst, 1, block) != nvme::kOk) {
    IOLog("NativeNvme: cannot read the first block of userdata\n");
    return;
  }
  hexLine("userdata head", userdataFirst, block, 32);
  char magic[9];
  for (unsigned i = 0; i < 8; ++i) magic[i] = block[i] >= 0x20 && block[i] < 0x7f ? static_cast<char>(block[i]) : '.';
  magic[8] = 0;
  IOLog("NativeNvme: userdata starts with \"%s\" (an UniXNU root image starts with SHNGROOT)\n", magic);

  uint32_t want = bootArg("nvme-probe", 1024);
  if (want == 0) return;
  const uint32_t chunk = nvme::kMaxRunBlocks;
  uint8_t *buffer = static_cast<uint8_t *>(IOMallocData(chunk * storage::kBlockBytes));
  if (!buffer) return;
  // `want` blocks from `at`, read the way the kernel will read them: the crc of what came back and how long it took.
  auto pass = [&](uint64_t at, uint32_t &crc, uint64_t &elapsed) {
    crc = 0;
    uint64_t begin = nowUs();
    for (uint32_t done = 0; done < want; done += chunk) {
      uint32_t n = want - done < chunk ? want - done : chunk;
      if (host.readBlocks(at + done, n, buffer) != nvme::kOk) return false;
      crc = storage::crc32(buffer, n * storage::kBlockBytes, crc);
    }
    elapsed = nowUs() - begin;
    return true;
  };
  auto rate = [&](uint64_t us) {
    return us ? ull(static_cast<uint64_t>(want) * storage::kBlockBytes * 1000000ull / 1024 / us) : 0ull;
  };
  // The same blocks twice (the CRCs have to agree: a controller or a cache that is not what the host assumes shows up here), then the
  // next range, untouched by the first two passes. Only a stable probe lets the driver count itself as DMA-capable, which is what
  // the persistent Data volume asks before it writes anything.
  uint32_t crcA = 0, crcB = 0, crcC = 0;
  uint64_t usA = 0, usB = 0, usC = 0;
  if (!pass(userdataFirst, crcA, usA) || !pass(userdataFirst, crcB, usB)) {
    IOFreeData(buffer, chunk * storage::kBlockBytes);
    IOLog("NativeNvme: probe read of userdata failed\n");
    return;
  }
  const bool second = userdataFirst + 2ull * want <= userdataLast + 1 && pass(userdataFirst + want, crcC, usC);
  const bool stable = crcA == crcB;
  IOLog("NativeNvme: probe %u blocks of userdata twice, %u blocks per command: crc32 0x%08x / 0x%08x (%s), %llu us / %llu us = %llu KiB/s; "
        "next range %s %llu us = %llu KiB/s (crc32 0x%08x), %llu failed commands, %llu timeouts\n",
        want, chunk, crcA, crcB, stable ? "stable" : "MISMATCH", ull(usA), ull(usB), rate(usA), second ? "read" : "skipped", ull(usC),
        rate(usC), crcC, ull(host.stats.failures), ull(host.stats.timeouts));
  native_emmc_multi_block = 1;
  native_emmc_dma = stable && host.stats.failures == 0 ? 1 : 0;
  IOLog("NativeNvme: reads by DMA %s\n", native_emmc_dma ? "ON" : "OFF");
  IOFreeData(buffer, chunk * storage::kBlockBytes);
}

// The Data RAM disk on a thread of its own. Without nvme-data-persist it is started first thing; with it, the
// persistent publisher decides, and this is what runs instead whenever that one cannot.
static void startDataRamThread() {
  if (!bootArg("nvme-data-ram", 1)) return;
  thread_t thread = nullptr;
  if (kernel_thread_start(dataDiskPublisher, nullptr, &thread) == KERN_SUCCESS) {
    thread_deallocate(thread);
  } else {
    IOLog("NativeNvme: data-ram: cannot start the thread\n");
  }
}

// ---- the PCIe controller and the drive behind it ---------------------------------------------------------------
namespace {

// Where edk2-rk3588 put PCI segment 4, the PCIe 2.0 x1 controller "pcie2x1l2" behind the M.2 slot of the Orange Pi 5
// (Silicon/Rockchip/RK3588/Include/Library/Rk3588Pcie.h, PCIE_*(4), and PciHostBridgeInit.c for the iATU regions). The values are
// the firmware's, not measured on the board yet; the log of the first run shows every register they lead to.
constexpr uint64_t kPcieApb = 0xfe190000ull;       // client registers: LTSSM status at +0x300 (the low 5 bits read 0x11 in L0)
constexpr uint64_t kPcieDbi = 0xa41000000ull;      // the root port's own configuration space and the DesignWare registers; link status at +0x80
// The iATU CFG0 region onto bus 1 behind the root port: 00:00.0 is the drive. The region is 64 KiB wide (the granule); only the first
// 4 KiB are the function's configuration space, and +0x8000 (device 1) must never be touched, the firmware saw the bus hang on it.
constexpr uint64_t kPcieCfg0 = 0xa36500000ull;
constexpr uint64_t kPcieMem32Bus = 0xf0000000ull;  // PCIE_MEM32_BUS_BASE: the 128 MiB of 32-bit space BARs are assigned in...
constexpr uint64_t kPcieMem32Size = 0x08000000ull;
constexpr uint64_t kPcieMem32Cpu = 0xa38000000ull;  // ...and where the CPU reaches it (an iATU memory region with a translation)
constexpr uint64_t kPcieMem64Cpu = 0xa00000000ull;  // 64-bit BARs: the bus address is the CPU address
constexpr uint64_t kPcieMem64Size = 0x30000000ull;
constexpr uint32_t kApbLtssm = 0x300;
constexpr uint32_t kDbiLinkStatus = 0x80;
constexpr uint32_t kPciCommand = 0x04, kPciClass = 0x08, kPciBar0 = 0x10, kPciBar1 = 0x14, kPciCapabilities = 0x34;
constexpr uint32_t kPciCmdMemory = 1u << 1, kPciCmdMaster = 1u << 2, kPciCmdIntxOff = 1u << 10;
constexpr uint32_t kNvmeClass = 0x010802;           // mass storage, non-volatile memory, NVM Express
constexpr uint32_t kBar0Bytes = 0x2000;             // the registers and the doorbells of two queue pairs: all that is used

volatile uint32_t *mapRegisters(uint64_t physical, size_t bytes, const char *what) {
  vm_offset_t virt = ml_io_map(static_cast<vm_offset_t>(physical), bytes);
  if (!virt) IOLog("NativeNvme: pcie: cannot map %s at %#llx\n", what, ull(physical));
  return reinterpret_cast<volatile uint32_t *>(virt);
}

uint32_t reg(volatile uint32_t *base, uint32_t offset) { return base[offset / 4]; }

void regWrite(volatile uint32_t *base, uint32_t offset, uint32_t value) {
  base[offset / 4] = value;
  __asm__ volatile("dsb sy" ::: "memory");
}

}  // namespace

// Finds the drive where the firmware left it, enables memory space and bus mastering on it and on its root port (both are already on:
// the firmware's driver read the ESP with them; asserting them again costs nothing), and maps BAR0. The address the BAR holds is a
// PCI bus address; the CPU reaches it through the translation of the iATU memory region it falls in.
bool NativeNvme::bringUpPcie() {
  volatile uint32_t *apb = mapRegisters(kPcieApb, 0x1000, "the PCIe client registers");
  volatile uint32_t *dbi = mapRegisters(kPcieDbi, 0x1000, "the root port");
  volatile uint32_t *cfg = mapRegisters(kPcieCfg0, 0x1000, "the endpoint's configuration space");
  if (!apb || !dbi || !cfg) return false;
  const uint32_t ltssm = reg(apb, kApbLtssm), link = reg(dbi, kDbiLinkStatus);
  IOLog("NativeNvme: pcie: LTSSM %#x (%s), root port link status %#x: Gen%u x%u, root port command %#x\n", ltssm,
        (ltssm & 0x1f) == 0x11 ? "L0, link up" : "NOT L0", link, (link >> 16) & 15, (link >> 20) & 63, reg(dbi, kPciCommand));
  const uint32_t id = reg(cfg, 0);
  if (id == 0xffffffffu || id == 0) {
    IOLog("NativeNvme: pcie: nothing answers at 00:00.0 behind the root port (vendor/device %#x): no drive in the slot, the link is down, "
          "or the firmware did not configure this controller\n", id);
    return false;
  }
  const uint32_t classCode = reg(cfg, kPciClass) >> 8;
  IOLog("NativeNvme: pcie: 00:00.0 vendor %#x device %#x class %#x revision %#x header %#x subsystem %#x command/status %#x\n", id & 0xffff,
        id >> 16, classCode, reg(cfg, kPciClass) & 0xff, (reg(cfg, 0x0c) >> 16) & 0xff, reg(cfg, 0x2c), reg(cfg, kPciCommand));
  // The capability list, for the log: the PCIe capability says what the link negotiated, and whether MSI/MSI-X are on (they stay off).
  uint32_t at = reg(cfg, kPciCapabilities) & 0xfc;
  for (unsigned seen = 0; at >= 0x40 && at < 0x1000 && seen < 12; ++seen) {
    const uint32_t header = reg(cfg, at);
    const uint32_t capability = header & 0xff;
    if (capability == 0x10) {
      IOLog("NativeNvme: pcie:   capability %#x at %#x: device control %#x, link status %#x (Gen%u x%u)\n", capability, at,
            reg(cfg, at + 8) & 0xffff, reg(cfg, at + 0x10) >> 16, (reg(cfg, at + 0x10) >> 16) & 15, (reg(cfg, at + 0x10) >> 20) & 63);
    } else {
      IOLog("NativeNvme: pcie:   capability %#x at %#x: %#x\n", capability, at, header);
    }
    at = (header >> 8) & 0xfc;
  }
  if (classCode != kNvmeClass) {
    IOLog("NativeNvme: pcie: the device is not an NVMe controller (class %#x, expected %#x)\n", classCode, kNvmeClass);
    return false;
  }
  // The command register shares its dword with the status register, whose bits clear when written as 1: write the command half only.
  const uint32_t command = reg(cfg, kPciCommand) & 0xffff;
  regWrite(cfg, kPciCommand, command | kPciCmdMemory | kPciCmdMaster | kPciCmdIntxOff);
  const uint32_t rootCommand = reg(dbi, kPciCommand) & 0xffff;
  if ((rootCommand & (kPciCmdMemory | kPciCmdMaster)) != (kPciCmdMemory | kPciCmdMaster)) {
    regWrite(dbi, kPciCommand, rootCommand | kPciCmdMemory | kPciCmdMaster);
    IOLog("NativeNvme: pcie: the root port had memory space or bus mastering off (command %#x): turned on\n", rootCommand);
  }
  IOLog("NativeNvme: pcie: command %#x -> %#x, root port command %#x\n", command, reg(cfg, kPciCommand) & 0xffff, reg(dbi, kPciCommand) & 0xffff);

  const uint32_t low = reg(cfg, kPciBar0), high = reg(cfg, kPciBar1);
  if (low & 1) {
    IOLog("NativeNvme: pcie: BAR0 is an I/O BAR (%#x)\n", low);
    return false;
  }
  const bool wide = ((low >> 1) & 3) == 2;
  const uint64_t bus = (low & ~0xfull) | (wide ? static_cast<uint64_t>(high) << 32 : 0);
  uint64_t cpu = 0;
  if (!bus) {
    IOLog("NativeNvme: pcie: BAR0 is not assigned (%#x %#x)\n", low, high);
    return false;
  } else if (bus >= kPcieMem32Bus && bus < kPcieMem32Bus + kPcieMem32Size && !wide) {
    cpu = bus - kPcieMem32Bus + kPcieMem32Cpu;
  } else if (bus >= kPcieMem64Cpu && bus < kPcieMem64Cpu + kPcieMem64Size) {
    cpu = bus;
  } else {
    IOLog("NativeNvme: pcie: BAR0 %#llx (%s) lies in none of the windows the firmware set up (32-bit %#llx+%#llx, 64-bit %#llx+%#llx)\n",
          ull(bus), wide ? "64-bit" : "32-bit", ull(kPcieMem32Bus), ull(kPcieMem32Size), ull(kPcieMem64Cpu), ull(kPcieMem64Size));
    return false;
  }
  IOLog("NativeNvme: pcie: BAR0 %#llx (%s, %sprefetchable) is at CPU address %#llx\n", ull(bus), wide ? "64-bit" : "32-bit",
        (low & 8) ? "" : "not ", ull(cpu));
  volatile uint32_t *registers = mapRegisters(cpu, kBar0Bytes, "BAR0");
  if (!registers) return false;
  bar0 = const_cast<uint32_t *>(registers);
  return true;
}

bool NativeNvme::start(IOService *provider) {
  if (!IOService::start(provider)) return false;
  // The watchdog has nothing to do with the drive; it lives here because this is the one built-in
  // driver of the board, and it must run even when the drive does not come up.
  if (uint32_t kick = bootArg("wdt-kick", 0)) {
    thread_t thread = nullptr;
    if (kernel_thread_start(wdtKicker, reinterpret_cast<void *>(static_cast<uintptr_t>(kick)), &thread) == KERN_SUCCESS) {
      thread_deallocate(thread);
    } else {
      IOLog("NativeNvme: wdt-kick: cannot start the thread\n");
    }
  }
  // Same reason: the power-manager nub that AMFI waits for has nothing to do with the drive either.
  if (bootArg("pmgr-nub", 1)) {
    thread_t thread = nullptr;
    if (kernel_thread_start(pmgrNubPublisher, nullptr, &thread) == KERN_SUCCESS) {
      thread_deallocate(thread);
    } else {
      IOLog("NativeNvme: pmgr-nub: cannot start the thread\n");
    }
  }
  // And the Data RAM disk: the payload's RAMDisk as a writable block device, so that Apple's APFS can
  // publish the Data volume as /dev/diskNsM for the kernel's mount on /private/var. With nvme-data-persist=1
  // the volume is a window of the drive instead (persistentData), and this only runs if that cannot, from
  // wherever start() gives up as well.
  const uint32_t mode = bootArg("nvme", 1);
  const bool persist = mode == 4 && bootArg("nvme-data-persist", 0) != 0;
  struct DataFallback {
    bool armed;
    ~DataFallback() {
      if (armed) startDataRamThread();
    }
  } dataFallback = {persist};
  if (!persist) startDataRamThread();
  if (mode == 0) {
    IOLog("NativeNvme: disabled by boot-arg nvme=0\n");
    return false;
  }
  IOLog("NativeNvme: matched %s\n", provider->getName());
  gate = IOLockAlloc();
  if (!gate) return false;
  if (!bringUpPcie()) return false;
  const nvme::DmaWindow *window = dmaWindow();
  if (!window) return false;
  host.attach(&kPlatform, bar0);
  host.setDma(window);

  host.dump("firmware state");
  int result = host.init();
  if (result != nvme::kOk) {
    IOLog("NativeNvme: the drive is not usable (%s)\n", nvme::resultName(result));
    host.dump("after failed init");
    return false;
  }
  host.dump("initialised");
  IOLockLock(gate);
  bool gpt = scanGpt();
  rootReady = false;
  if (gpt) {
    probeUserdata();
    if (mode == 4) rootReady = findRoot();
  }
  IOLockUnlock(gate);
  const nvme::Stats &stats = host.stats;
  IOLog("NativeNvme: %llu blocks read, %llu commands, %llu retries, %llu recoveries, %llu failures, %llu timeouts, %llu us\n",
        ull(stats.blocks), ull(stats.commands), ull(stats.retries), ull(stats.recoveries), ull(stats.failures),
        ull(stats.timeouts), ull(stats.readUs));
  if (!gpt) return false;
  native_emmc_ready = 1;
  registerService();

  bool published = false;
  if (mode == 2) {
    published = publishWindow("NVMe", 0, diskBlocks) != nullptr;
  } else if (mode == 3) {
    published = userdataLast ? publishWindow("userdata", userdataFirst, userdataLast - userdataFirst + 1) != nullptr : false;
  } else if (mode == 4 && rootReady) {
    // The windows go out from a thread of their own (see publishRoot); it keeps us alive until it is done.
    thread_t thread = nullptr;
    retain();
    if (kernel_thread_start(rootPublisher, this, &thread) == KERN_SUCCESS) {
      thread_deallocate(thread);
    } else {
      IOLog("NativeNvme: cannot start the root publisher thread\n");
      release();
    }
    if (persist) {
      // The Data window on the drive, next to the root windows (it formats first, if it has to, and it only
      // starts once the root image is known to end well in front of it).
      thread_t dataThread = nullptr;
      retain();
      if (kernel_thread_start(dataPublisher, this, &dataThread) == KERN_SUCCESS) {
        thread_deallocate(dataThread);
        dataFallback.armed = false;
      } else {
        IOLog("NativeNvme: cannot start the Data publisher thread\n");
        release();
      }
    }
  }
  if (published) {
    thread_t thread = nullptr;
    if (kernel_thread_start(mediaReport, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
  }
  return true;
}

void NativeNvme::stop(IOService *provider) { IOService::stop(provider); }

void NativeNvme::free() {
  if (gate) { IOLockFree(gate); gate = nullptr; }
  IOService::free();
}
