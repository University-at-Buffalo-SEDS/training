# Datalogger firmware training

The STM32CubeMX project and CMake target are **`datalogger`**, using the CrashNBurn board. Generated HAL, ThreadX, and USBX sources are included, so a clone builds without sibling repositories. Start with the [CrashNBurn Project Guide](CrashNBurn-Project-Guide.pdf). It walks through the project in order:

1. ThreadX features we use and why.
2. Drivers and the board hardware.
3. Opening the SDD template in Overleaf.
4. Firmware requirements.
5. Writing the SDD using RFBoard as an example, including LaTeX basics.
6. Creating the HAL/ThreadX/USBX CMake project in CubeMX, with screenshots.
7. ST STM32 extension setup with screenshots, Git integration, a full dependency checklist, and building/flashing with `build.py`.

## Hardware references and flash logging

Read the [included datasheets](datasheets/README.md) and cite their revision and relevant sections in your SDD. The guide shows how queued raw/processed records become SPI flash bytes and how the data task reads them back for USB download, including write-enable, page boundaries, CRC/commit, address mapping and confirmed sector erasure.

**The specified flash is 32 Mbit = 4 MiB, not 31 MB.** At the proposed rates, storage lasts about four minutes before additional overhead; validate the required recording duration.

## Development dependencies

Use **STM32CubeIDE for Visual Studio Code by STMicroelectronics**, with its STM32Cube clangd and managed companion extensions. Do not separately install the Microsoft C/C++ extension or C/C++ Extension Pack for this workflow; disable them for this workspace if already present. Let the ST pack manage its required dependencies.

Install/check **Git, VS Code, STM32CubeMX, STM32CubeCLT, STM32CubeProgrammer CLI, Python 3, CMake, Ninja, GNU ARM tools, STM32CubeG4 firmware, and X-CUBE-AZRTOS-G4**. The guide explains bundled versus standalone tools, PATH checks, ST-LINK drivers/hardware, and optional Overleaf/local LaTeX dependencies. A compatible programmer CLI already supplied by CubeCLT does not require a duplicate standalone installation.

## Included files

| File | Purpose |
|---|---|
| [CrashNBurn-Project-Guide.pdf](CrashNBurn-Project-Guide.pdf) | The complete training guide. |
| [Schematics.pdf](Schematics.pdf) | Board hardware and pin connections. |
| [UBSEDS_SDD_Template_Overleaf.zip](UBSEDS_SDD_Template_Overleaf.zip) | Clean LaTeX template for your own SDD. |
| [datasheets/](datasheets/README.md) | Full flash, barometer, IMU and STM32 datasheets, with revisions and source links. |
| [build.py](build.py) | Executable build/direct-flash helper adapted from DAQ-Board, without networking or application bootloader paths. |
| [datalogger.ioc](datalogger.ioc) | CubeMX CMake/GCC configuration, including the SPI flash chip select. |
| [BRANCHES.md](BRANCHES.md) | Branch, review, and PR workflow for contributors. |
| [Datalogger_SDD_Overleaf.zip](Datalogger_SDD_Overleaf.zip) | Filled project-specific SDD ready to upload to Overleaf. |
| [SDD source](docs/datalogger-sdd/main.tex) | Editable LaTeX design, task layout, queues, interfaces, and verification plan. |
| [Datalogger SDD PDF](output/pdf/Datalogger_Software_Design_Document.pdf) | Rendered project-specific design document. |

Upload `Datalogger_SDD_Overleaf.zip` to Overleaf as a new project and select `main.tex` with **XeLaTeX**. The original template ZIP is retained as a clean reference. Datalogger uses HAL and ThreadX without SEDSNet.

## Build and flash

Install Python 3, CMake 3.22+, Ninja, and the GNU Arm Embedded tools (including `arm-none-eabi-gcc`, `g++`, and `objcopy`) on PATH. CubeCLT supplies these tools. Then run from the checkout:

```sh
./build.py build --debug
./build.py build --release
./build.py --help
./build.py flash --debug --method stm32prog-cli
```

The script is checked in with executable permissions. On Windows use `py -3 build.py` (or your Python 3 launcher); `python3 build.py` also works on macOS/Linux. Debug and Release artifacts are `build/Debug_Script/datalogger.{elf,bin}` and `build/Release_Script/datalogger.{elf,bin}`. Flashing builds first and writes the STM32 internal flash at `0x08000000`. The helper also supports `st-flash`, `st-util`, ST's GDB server, and ROM DFU when the device is put into the appropriate mode. The GDB host/port options are debugger transport settings. There is no SEDSNet, telemetry, OTA, factory-image, or application bootloader build path. The script does not download external flash logs.

CMake presets are also available: `cmake --preset Debug` then `cmake --build --preset Debug` (or `Release`). Presets use `build/Debug` and `build/Release`; the helper additionally converts ELF to BIN.

### Install dfu-util for USB flashing

`dfu-util` is optional if you flash through ST-LINK. Check first with `dfu-util --version`.

- **macOS:** Install [Homebrew](https://brew.sh/) if needed, then run `brew install dfu-util` ([formula](https://formulae.brew.sh/formula/dfu-util)).
- **Ubuntu/Debian:** Run `sudo apt update` then `sudo apt install dfu-util` ([Ubuntu package](https://packages.ubuntu.com/dfu-util)). Other Linux distributions generally provide a package with the same name.
- **Windows:** Get the Windows binary archive from the [official dfu-util releases](https://dfu-util.sourceforge.net/releases/), linked by the [project homepage](https://dfu-util.sourceforge.net/). Extract it, keep its supplied DLLs alongside the executable, and add the folder containing `dfu-util.exe` to your user PATH. Open a new terminal and run `dfu-util --version`. The DFU device needs a libusb-compatible driver, normally WinUSB; follow the project's Windows driver guidance. If using [Zadig](https://zadig.akeo.ie/), select the board's **STM32 BOOTLOADER/DFU** device specifically. Do not replace an unrelated device's driver or the normal CDC serial interface.

Put the board into the STM32 ROM USB DFU boot mode using the board's BOOT0/reset procedure and applicable boot option settings, connect a USB data cable, then check:

```sh
dfu-util -l
./build.py flash --debug --method dfu
```

The list should show the intended STM32 DFU device and an internal-flash alternate setting. Keep only the intended DFU target connected: the helper uses alternate setting 0 and does not select by USB serial number. The generated application exposes USB CDC, so a normal serial connection does not mean ROM DFU is active. If no device appears, check boot mode, cable, power, and drivers. A Linux `LIBUSB_ERROR_ACCESS` means USB permissions need attention; install an appropriate udev rule for the actual enumerated device rather than running the entire build as root. After flashing, restore normal boot selection and reset if needed. This uses the MCU's built-in ROM loader and requires no application bootloader in this project.

## Hardware configuration and regeneration

Open `datalogger.ioc` in CubeMX. Keep the project name `datalogger`, CMake toolchain, and generation location at this checkout's root. The generated baseline uses STM32CubeG4 1.6.3 and X-CUBE-AZRTOS-G4 2.0.0. Commit IOC changes together with their generated sources. Put application additions in preserved `USER CODE` sections and separate modules registered in the root CMake file. Review the generated diff and rebuild both modes before a PR; see [BRANCHES.md](BRANCHES.md).

SPI1 is full-duplex master, mode 0, 8-bit, MSB first, software NSS, currently 8 MHz from a 16 MHz peripheral clock. PA5 is SCK, PA6 MISO, and PA7 MOSI. PB11/PB12/PB13 select the accelerometer/gyroscope/barometer; **PB14 (`flash_CS`) selects the W25Q32JV flash**. All four active-low selects initialize high. PA13/PA14 preserve SWD.

This is a buildable generated starter, not completed logger firmware. The filled SDD defines the intended sensor, processing, and data workers. Driver logic, logging, the USB command adapter/client, tick/pool sizing, timing validation, and hardware tests remain implementation work. The guide's example timing values are proposals; the IOC and reviewed generated code describe the current configuration.

To compile the filled SDD from this repository root:

```sh
latexmk -xelatex -interaction=nonstopmode -halt-on-error -cd \
  -outdir=../../build/sdd docs/datalogger-sdd/main.tex
```

Review the guide's SWD wiring and USB hardware checks before bring-up. USB export uses a computer as host; a thumb drive requires additional host hardware. CubeMX screenshots are attributed reference examples, with the actual CrashNBurn settings specified beside them.

Have fun with the project!
