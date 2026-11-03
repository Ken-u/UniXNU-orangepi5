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
| `PcieWindows.h` | Which PCIe window a BAR lies in and its CPU address |
| `tests/` | Host tests: a model of an NVMe controller behind a non-coherent cache, the root manifest reader, the PCIe windows |

## Host checks

```sh
g++ -std=c++17 -Wall -Wextra -Werror -O1 -I . tests/test-nvme-host.cpp -o /tmp/test-nvme-host && /tmp/test-nvme-host
g++ -std=c++17 -Wall -Wextra -Werror -O1 -I . tests/test-root-manifest.cpp -o /tmp/test-root-manifest && /tmp/test-root-manifest
g++ -std=c++17 -Wall -Wextra -Werror -O1 -I . tests/test-pcie-windows.cpp -o /tmp/test-pcie-windows && /tmp/test-pcie-windows
g++ -std=c++17 -Wall -Wextra -Werror -O1 -I . tests/check-gpt-image.cpp -o /tmp/check-gpt-image && /tmp/check-gpt-image DISK_IMAGE_OR_DEVICE
```

The last one reads a disk (image) with the driver's own GPT reader; it checks what `tools/board/nvme-install.py` of `uefi-orangepi5` writes.

`test-nvme-host` runs `NvmeHost.h` and `Gpt.h` against a model that enforces what the host depends on (reset before the queue registers are
written, doorbells only for queues that exist, every address inside the DMA window, page-aligned PRPs, phase-bit completions, writes only inside
the declared range) and a cache model in which the CPU and the controller meet only in `flush()`. Breaking any of those in `NvmeHost.h` makes the
test fail (each one was checked by mutating the header).

## What the first run on the board showed

The first run (2026-11-03, `evidence/device-first-boot-20261103.txt`) settled the assumptions that came from the sources of edk2-rk3588 and the
NVMe 1.4 specification, except those listed last:

- Segment 4 of the firmware (`PCIE_*(4)` of `Rk3588Pcie.h`): APB `0xfe190000`, DBI `0xa41000000` and the endpoint's configuration space through the
  iATU CFG0 region at `0xa36500000` work: the log reads vendor `0x126f`, device `0x2263`, class `0x10802` from `00:00.0`, and the link is
  up (LTSSM L0). The firmware brings the link up again after U-Boot used the drive. U-Boot reports Gen2 x1 for its own link-up; the
  firmware ("x1, 2.5 GT/s") and the kernel (link status Gen1 x1) see Gen1, and the driver does not retrain the link.
- The BAR is a 64-bit **non-prefetchable** BAR at bus `0xf0000000`, which edk2 places in the 32-bit window; the driver maps it at CPU
  `0xa38000000` (`PcieWindows.h`). The first version of the driver took every 64-bit BAR for the 64-bit window and refused this one
  (`tests/test-pcie-windows.cpp` now covers the classification).
- The firmware's `NvmExpressDxe` leaves the controller enabled (`CC 0x460001 CSTS 0x1`); `NvmeHost::init()` resets it and initialises it: the
  controller grants 8 queue pairs (the driver uses one), identify, GPT, DMA reads (72 MiB/s from a 4096-block probe, identical CRC twice, 0 retries, 0 failures).
- The namespace has 512-byte logical blocks (250069680 blocks).
- Not settled: writes are only exercised by the Data window (650 write calls at the first boot, 0 failures, the window was formatted); a second
  boot over the formatted window, the cache handling under load (the probe CRCs were stable), and why the link stays at Gen1 (the probe reads 72 MiB/s, below what a Gen1 x1 link carries; whether the link or the polled bounce-window reads limit it was not measured).

## Use

`UniXNU-orangepi5` selects this directory as `package/Rk3588Nvme`. The board supplies the `/arm-io/pcie-nvme` node (no `reg`), the GPT layout and
the boot arguments (`nvme=4`, `nvme-root-trust=1`, `nvme-data-persist=1`, `nvme-data-size`, ...; see the header of `NativeNvme.cpp`).
`scripts/build-orangepi5-drivers.py` compiles it against the kernel's compile record, `scripts/bind-nvme-storage.py` binds the block device
class to the original `IOBlockStorageDevice`.

Writes are limited to the Data window at the tail of `userdata`; the driver never writes elsewhere.
