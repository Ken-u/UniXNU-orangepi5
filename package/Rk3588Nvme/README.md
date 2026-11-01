# Rk3588Nvme

NVMe drive in the M.2 slot of the Orange Pi 5 (RK3588S, PCIe 2.0 x1 controller `pcie2x1l2`) as block storage for UniXNU: a polled NVMe host
core, GPT parsing, and a block device that publishes the root volumes and a persistent Data window.

It is the NVMe counterpart of `Rk3588Emmc` (`UniXNU-evb10/package/Rk3588Emmc`): everything above the controller (GPT scan, `SHNGROOT` root image
and its windows, the Data volume, the kernel hooks, the watchdog feeder, the power-manager nub) is the same code, with the host core swapped for
`NvmeHost.h`. The kernel-facing C symbols keep their `native_emmc_*` names because the kernel patches of the EVB10 and the SoC call them.

| Path | Contents |
| --- | --- |
| `NativeNvme.cpp` | IOKit service, PCIe bring-up, block device, root and Data publication |
| `NvmeHost.h` | Controller core: reset, admin queue, identify, one I/O queue pair, PRP transfers through a bounce window, all polled |
| `Gpt.h`, `RootManifest.h`, `StorageCommon.h` | Copies of the Rk3588Emmc headers with the namespace `emmc` renamed to `storage` (keep them in step) |
| `Info.plist` | Reference personality (the kernel patch `orangepi5-nvme-personality.patch` registers the real one) |
| `tests/` | Host tests: a model of an NVMe controller behind a non-coherent cache, the root manifest reader |

## Host checks

```sh
g++ -std=c++17 -Wall -Wextra -Werror -O1 -I . tests/test-nvme-host.cpp -o /tmp/test-nvme-host && /tmp/test-nvme-host
g++ -std=c++17 -Wall -Wextra -Werror -O1 -I . tests/test-root-manifest.cpp -o /tmp/test-root-manifest && /tmp/test-root-manifest
g++ -std=c++17 -Wall -Wextra -Werror -O1 -I . tests/check-gpt-image.cpp -o /tmp/check-gpt-image && /tmp/check-gpt-image DISK_IMAGE_OR_DEVICE
```

The last one reads a disk (image) with the driver's own GPT reader; it checks what `tools/board/nvme-install.py` of `uefi-orangepi5` writes.

`test-nvme-host` runs `NvmeHost.h` and `Gpt.h` against a model that enforces what the host depends on (reset before the queue registers are
written, doorbells only for queues that exist, every address inside the DMA window, page-aligned PRPs, phase-bit completions, writes only inside
the declared range) and a cache model in which the CPU and the controller meet only in `flush()`. Breaking any of those in `NvmeHost.h` makes the
test fail (each one was checked by mutating the header).

## What is assumed about the hardware, and not observed yet

Nothing in this directory has run on an Orange Pi 5. The assumptions, all from the sources of edk2-rk3588 and the NVMe 1.4 specification:

- Segment 4 of the firmware (`PCIE_*(4)` of `Rk3588Pcie.h`): APB `0xfe190000`, DBI `0xa41000000`, the endpoint's configuration space through the
  iATU CFG0 region at `0xa36500000`, BARs assigned in the 32-bit window (bus `0xf0000000`, CPU `0xa38000000`) or the 64-bit window
  (`0xa00000000`, bus = CPU).
- The firmware's `NvmExpressDxe` leaves the controller enabled; `NvmeHost::init()` resets it first.
- DMA addresses are CPU physical addresses (no IOMMU), the PCIe controller is not cache coherent.
- The namespace has 512-byte logical blocks. A drive formatted with 4 KiB blocks is refused with a message that says how to fix it.

The first run's log (`NativeNvme: pcie: ...`, `NativeNvme: NVMe ...`) shows each of these.

## Use

`UniXNU-orangepi5` selects this directory as `package/Rk3588Nvme`. The board supplies the `/arm-io/pcie-nvme` node (no `reg`), the GPT layout and
the boot arguments (`nvme=4`, `nvme-root-trust=1`, `nvme-data-persist=1`, `nvme-data-size`, ...; see the header of `NativeNvme.cpp`).
`scripts/build-orangepi5-drivers.py` compiles it against the kernel's compile record, `scripts/bind-nvme-storage.py` binds the block device
class to the original `IOBlockStorageDevice`.

Writes are limited to the Data window at the tail of `userdata`; the driver never writes elsewhere.
