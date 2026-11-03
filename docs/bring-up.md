# Orange Pi 5 bring-up

Status (2026-11-03): **the console kernel boots on the board from the NVMe drive: `launchd` runs.** Observed on hardware (serial log,
`evidence/device-first-boot-20261103.txt`): U-Boot starts the launcher from `boot.scr`, the firmware shows 8 GiB, finds the NVMe ESP and
starts DarwinBoot, the kernel identifies the drive, reads the GPT, publishes the root windows and a persistent Data window, and `launchd`
reaches "Early boot complete" 30 s after the hand-off; the kernel then runs the 400 s until the capture ends without a panic. Not done:
a real display (the `xnu-hdmi` entry mirrors the kernel log as text on the HDMI monitor and the log scrolled there, nothing is drawn by a display driver), GPU, input. The kernel owns 2.5 GiB of the 8 GiB (the largest run the firmware leaves; see "Placeholders and open inputs"). The
procedure for the board is `uefi-orangepi5/docs/first-boot.md`.

## Board

| Part | Orange Pi 5 | Same as the EVB10? |
| --- | --- | --- |
| SoC | RK3588S (8 cores, 4 x A76 + 4 x A55, Mali-G610) | Yes, minus some interfaces |
| Memory | 8 GiB LPDDR4x (the board of this port) | Same SoC memory map; the EVB10 uses the bank above 4 GiB for its device tree too |
| Debug console | UART2, 3-pin header, 1.5 Mbaud 8N1 | Yes (UART2, `feb50000`) |
| Root and Data storage | NVMe in the M.2 M-key slot, PCIe 2.0 x1 (`pcie2x1l2`, combphy0, reset GPIO3_D1, 3.3 V enable GPIO0_PC5); 128 GB drive | No: the EVB10 uses eMMC |
| eMMC socket, microSD | dwcmshc (as the EVB10 eMMC), dw_mmc | not used |
| Display | HDMI0 from VOP2 video port 0 | No: the EVB10 drives a panel from video port 2 |
| GPU rail | RK806 BUCK1 (`vdd_gpu_s0`) | Yes |
| Big-core rails | RK8602 / RK8603 at 0x42 / 0x43 on I2C0 | Yes |
| Audio | ES8388 on I2C6 0x10 + I2S1, HDMI audio | No |
| RTC | HYM8563 on I2C6 0x51 | No |
| Reset / power | none remote: the user pulls power; the hardware watchdog is the only reset path | No: the EVB10 has a reset controller |

## Boot flow

1. The firmware is the UEFI volume of `uefi-orangepi5` with the UniXNU modules (`DarwinBoot`, the UniXNU menu and the boot watchdog
   `UniXNUBootWdtDxe`). It starts in one of two ways:
   - **From U-Boot (the plan for this board).** The SPI NOR holds a U-Boot that the board's owner supplies (it has `nvme`, `ums`, `booti`
     and distro boot; TF-A's BL31 comes with it). `boot.scr` on the ESP loads `uefi-launcher.img`, a stub with the volume inside, and
     `booti`s it. The stub moves the volume to 0x200000, arms the hardware watchdog and jumps into it at EL2, the state TF-A leaves for
     BL33 in the normal flash flow. The launcher volume has no SPI NOR variable store, so nothing writes the SPI.
   - **From the SPI NOR.** `nor.img` of `uefi-orangepi5`: TF-A (BL31) at EL3 with PSCI over SMC, OP-TEE and the volume.
2. UEFI initialises DRAM, PCIe, the NVMe controller and HDMI (GOP). `NvmExpressDxe` exposes the NVMe drive and the
   firmware finds the ESP on it.
3. The ESP carries the payload: `kernel.macho`, the trust cache, the Data seed and `EFI/UniXNU/unixnu.toml`. `DarwinBoot` builds
   the ADT and the boot arguments, takes `owned_memory_size` from the UEFI memory map and jumps to the kernel at EL1; the watchdog driver
   of the firmware armed itself earlier.
4. The kernel keeps the watchdog fed with `wdt-kick=1800`; a hung kernel ends in a reset about 89 seconds after the feeding stops.
5. `userdata` behind the ESP holds the root image (OS and SystemOS windows, manifest `SHNGROOT` at its start) and, at its end,
   the persistent Data window. The NVMe driver (`package/Rk3588Nvme`) takes the controller over from the firmware and publishes the windows to IOKit.

## Storage (`package/Rk3588Nvme`)

Derived from `Rk3588Emmc` of the EVB10 port: the same GPT scan, root manifest, Data window, kernel hooks, watchdog feeder and PMGR nub;
the host core is an NVMe one (`NvmeHost.h`: reset, admin queue, identify, one I/O queue pair, polled completions, PRP transfers through a
contiguous bounce window, writes only inside the Data window).

The hardware as the first run measured it (the plan was derived from the sources of edk2-rk3588):

| | |
| --- | --- |
| Controller | PCIe segment 4 (`pcie2x1l2`): APB `0xfe190000`, DBI `0xa41000000`, configuration space of the drive through the iATU CFG0 window at `0xa36500000` |
| BAR | BAR0 is a 64-bit non-prefetchable BAR that edk2 put in the 32-bit window (bus `0xf0000000`, CPU `0xa38000000`, 128 MiB window); the 64-bit window (`0xa00000000`, bus = CPU) stays as the other case. The first driver version refused it (it took every 64-bit BAR for the 64-bit window) |
| State | the firmware's `NvmExpressDxe` leaves the controller enabled (`CC 0x460001`); the driver resets it (`CC.EN = 0`) before it programs the queues; works. U-Boot reports Gen2 x1 for its own link-up, the firmware (2.5 GT/s) and the kernel see Gen1 x1 |
| DMA | bus address = CPU physical address, no IOMMU; not cache coherent, hence `dc civac` around every transfer. The window is allocated below 1 TiB. Reads agree (two CRCs of the same 4096 blocks, 72 MiB/s, 0 failures); writes are exercised by the Data window only |
| Blocks | 512-byte logical blocks without metadata (anything else is refused with a message); the drive is one |

The ADT carries `/arm-io/pcie-nvme` (compatible `unixnu,rk3588-pcie-nvme`, no `reg`, no `interrupts`); the controller is addressed by the
fixed physical addresses above, as the Mali driver of the EVB10 does. `patches/xnu/orangepi5-nvme-personality.patch` adds the IOKit personality.

Boot arguments (header of `NativeNvme.cpp`): `nvme=4` publish the root windows, `nvme=1` probe only, `nvme-probe=N`, `nvme-root-lba=N`,
`nvme-root-trust=1`, `nvme-data-size=MiB`, `nvme-data-persist=0|1`, `nvme-data-reset=1`, `nvme-data-wait=S`. The names that the kernel
patches of the EVB10 read are unchanged (`emmc-systemos-union`, `emmc-data-mount`, `emmc-data-copy`; the C symbols `native_emmc_*`).

Host checks (`package/Rk3588Nvme/README.md`): `tests/test-nvme-host.cpp` (a controller model behind a non-coherent cache; the
reset order, doorbells, DMA window, PRP alignment, phase bits and the write range are checked, and each was broken on purpose to prove the
test notices), `tests/test-root-manifest.cpp`, `tests/check-gpt-image.cpp` (the tables of the installer through the driver's own reader).

## Milestones

| | Goal | Needs | State |
| --- | --- | --- | --- |
| M0 | UEFI runs on the board: serial log, HDMI picture, NVMe drive visible | Board, serial adapter, U-Boot in the SPI NOR with `ums` and a Mac to write the drive (or a way to write the SPI NOR) | done: serial log, NVMe ESP and drive seen, HDMI shows the firmware screens and (xnu-hdmi) the scrolling kernel log |
| M1 | The kernel starts from the NVMe ESP payload; the driver finds the drive (`nvme=1` shows PCIe, NVMe identify, GPT, probe) | M0 | done (2026-11-03) |
| M2 | Root and Data mount, `launchd` runs | A written userdata partition | done (2026-11-03): root windows, Data formatted at the first boot, `launchd` up; a second boot over the formatted Data window is still to be seen |
| M3 | The boot screen on the HDMI monitor through the GOP frame buffer | M1 | text mirror done (`xnu-hdmi`, `vc_mirror=1`); the iPadOS boot screen needs the display driver |
| M4 | VOP2 scanout on video port 0, Mali composition, frame rate | M2, M3 | not started |
| later | USB input (xHCI + HID), audio, Ethernet and Wi-Fi, microSD | | |

## Placeholders and open inputs

- Memory is measured, not a placeholder any more (`evidence/device-memory-20261103.txt`). The firmware prints its UEFI memory map
  (`MEMMAP:` lines, `UniXNUMemMapDxe`): the lower bank has free runs of 124 MiB, 849 MiB and 2674 MiB (`0x40000000` to `0xE72DA000`),
  the 4 GiB above `0x100000000` two runs of about 2 GiB, because the prebuilt Realtek UNDI driver sits at `0x180000000` (DxeCore loads
  an image at its linked address when that is free, `PcdImageLargeAddressLoad`). DarwinBoot takes the kernel's window from one run, first
  fit, so the window is at most about 2.6 GiB: `payload.adt.owned_memory_size = 0xA0000000` (2.5 GiB at `0x4007C000`, about 100 MiB of margin);
  3.5, 3 and 2.75 GiB were refused (`DARWINBOOT: boot refused: Out of Resources`), 2.5 GiB boots to `launchd`. The memory node of
  `dts/orangepi5.dts` and `payload.dram_size` are that size at `0x40000000`; the kernel does not read the node, and the ADT cannot carry
  two ranges, so the other bank is not usable by the kernel. The 256 MiB at `0x2f0000000` that U-Boot reports are mapped by edk2 nowhere.
  More than 2.6 GiB needs the upper bank in one piece. Moving the Realtek driver (`PcdImageLargeAddressLoad=FALSE`) or building without the
  network stack (`NETWORK_ENABLE=FALSE`) both stop the firmware in `AcpiTableDxe` (data abort, freed-pool pattern `0xAFAFAFAF` in the
  registers) on this board; that crash comes first, and the kernel has not run above 4 GiB on this port.
  To change the window: edit `owned_memory_size` (2 MiB steps; keep `dram_size` and the memory node equal to the size), run
  `REBUILD=1 tools/board/make-bundle.sh` in `uefi-orangepi5`, and write `kernel.macho.adt` to the ESP.
- `payload.adt.chip_id`: copied from the EVB10 port; the kernel boots with it. The Data RAM
  disk fallback prints `data-ram: no memory for 2048 MiB` (it keeps a 9 MiB one); with `nvme-data-persist=1` Data is on the drive and
  this does not matter.
- The drive's real sector count: the installer builds the backup GPT for whatever the drive has (a 128 GB drive has about 250 million sectors).
- No display, GPU, input, audio or network driver is in the kernel: HDMI shows the firmware's screens only; the kernel speaks on the serial port.

## Build

```sh
source ../host/env.sh        # or the equivalent environment of the machine that builds
scripts/build-orangepi5-kernel.sh                  # kernel (20 to 40 minutes from scratch, less when incremental)
cd ../uefi-orangepi5 && REBUILD=1 tools/board/make-bundle.sh      # payload + the bundle for the board
```

The script needs `.env` (see `.env.example`) and CMake 3.19.6 or newer. `scripts/check-vendored-patches.sh` compares the vendored
EVB10 patches (`patches/xnu/VENDORED.md`) with the sibling repository.
