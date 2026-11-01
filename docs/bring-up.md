# Orange Pi 5 bring-up

Status: **host side complete for the first round (console kernel, NVMe storage), nothing has run on the board.** The firmware builds
(`uefi-orangepi5`), the kernel with the NVMe driver builds and links, the payload and the NVMe image are assembled and checked on the
host. Every "expected" below is a plan, not an observation. The procedure for the board is `uefi-orangepi5/docs/first-boot.md`.

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

1. The firmware is the SPI NOR image of `uefi-orangepi5`: TF-A (BL31) at EL3 with PSCI over SMC, OP-TEE, and the UEFI
   volume with the UniXNU modules (`DarwinBoot`, the UniXNU menu and the boot watchdog `UniXNUBootWdtDxe`).
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

What is assumed about the hardware (from the sources of edk2-rk3588, **not measured**):

| | |
| --- | --- |
| Controller | PCIe segment 4 (`pcie2x1l2`): APB `0xfe190000`, DBI `0xa41000000`, configuration space of the drive through the iATU CFG0 window at `0xa36500000` |
| BAR | assigned by the firmware in the 32-bit window (bus `0xf0000000`, CPU `0xa38000000`, 128 MiB) or the 64-bit window (`0xa00000000`, bus = CPU) |
| State | the firmware's `NvmExpressDxe` leaves the controller enabled; the driver resets it (`CC.EN = 0`) before it programs the queues |
| DMA | bus address = CPU physical address, no IOMMU; not cache coherent, hence `dc civac` around every transfer. The window is allocated below 1 TiB |
| Blocks | 512-byte logical blocks without metadata (anything else is refused with a message) |

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
| M0 | UEFI runs on the board: serial log, HDMI picture, NVMe drive visible | Board, serial adapter, a way to write the SPI NOR | bundle ready |
| M1 | The kernel starts from the NVMe ESP payload; the driver finds the drive (`nvme=1` shows PCIe, NVMe identify, GPT, probe) | M0 | kernel and payload built |
| M2 | Root and Data mount, `launchd` runs | A written userdata partition | image and installer ready |
| M3 | The boot screen on the HDMI monitor through the GOP frame buffer | M1 | not started |
| M4 | VOP2 scanout on video port 0, Mali composition, frame rate | M2, M3 | not started |
| later | USB input (xHCI + HID), audio, Ethernet and Wi-Fi, microSD | | |

## Placeholders and open inputs

- `dts/orangepi5.dts` memory node and `payload.dram_size`: 4 GiB at `0x100000000`, which exists on this 8 GiB board in full. Replace
  with the memory map of the first run if it disagrees.
- `payload.adt.chip_id`, `payload.adt.owned_memory_size` (2 GiB): copied from the EVB10 port, unchecked here. Where DarwinBoot puts the
  kernel's pages is known only from the first log; the NVMe window accepts any address below 1 TiB.
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
