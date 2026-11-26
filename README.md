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

**The specified flash is 32 Mbit = 4 MiB, not 31 MB.** The compact 100 Hz implementation stores about 21 minutes before file overhead, or roughly 1 MiB per protected 20-second pre-trigger + five-minute post-trigger capture.

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

Every `build.py` build also prints the linker memory breakdown, including when
Ninja reports no work. The region table shows origin, used/capacity/free bytes,
and usage percentage from the evaluated linker map and ELF. A second table
lists every ELF section's byte size, runtime address (VMA), load address (LMA),
and memory regions. Initialized `.data` counts toward both RAM and FLASH;
`.bss` and heap/stack reservations count toward RAM. Debug sections are listed
separately in the same table and consume no device memory. Region usage includes
alignment gaps and reflects link-time reservations, not runtime stack peaks.
The report uses GNU `arm-none-eabi-objdump` from the ARM toolchain; set `OBJDUMP`
to override its executable path.

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

## USB and sensor bring-up

Sensor and flash initialization report through `printf`. The USB CDC
adapter now accepts the SDD's command lines instead of sending unsolicited
`alive` lines. Send `STATUS` with DTR asserted for firmware, uptime, identity,
and transport health. Flash recovery runs at boot, followed by automatic sensor
recording once identities and a recent pressure reference are available.
STATUS reports LOGGING/CAPTURING/READY/FULL/FAULT and durable/drop counters; initial
recovery can briefly report NOT_READY. Unknown media is never auto-erased.

The [USB client](client/README.md) provides a Tk desktop app and CLI, binary-safe
CRC-checked exports/resume, explicit deletion confirmation, and saved print
viewing. `printf`, stdout/stderr, and `__io_putchar` feed an 8 KiB RAM history;
the client retrieves and saves it without mixing prints into binary downloads.
RAM history survives a disconnected host but is cleared by board reset. Saved
host captures remain available later.

BMI088/BMP390 drivers and sensor/processing tasks are wired up. In the GUI,
enable **Live sensors (bypass flash)** for current readings and saved JSON
snapshots without a storage backend. CLI: `datalogger_client.py --port <port>
live --follow`. The flash/data worker stores compact raw data and reconstructs processed values
for downloads. Normal history rolls automatically; acceleration spikes, freefall
and barometric shifts protect 20 seconds before and five minutes after a trigger.
The GUI lists these separately as protected launch/drop files, with trigger-relative
CSV exports. Protected files require explicit deletion. Downloading stops normal
recording; active captures finish first. Click Start to resume after a stop.
USB identifies as **UBSEDS CrashNBurn Datalogger**. [Storage integration](docs/storage-integration.md) documents
the durable format, boot behavior, recovery and hardware qualification limits.

```sh
python3 -m pip install -r client/requirements.txt
python3 client/datalogger_gui.py
python3 client/datalogger_client.py --port /dev/cu.usbmodemXXXX logs --follow
```

Thread declarations live in `Core/Inc/T-Threads.h`, with one source file per
thread, matching RFBoard's layout. The USB adapter uses priority 20 and a 2048 B
static stack; a priority-21 receiver with a 1024 B stack handles blocking USBX
reads. [USB integration](docs/usb-integration.md) documents queues, buffer
ownership, exact replies, stack checking, and flash-worker integration.

The supplied schematic shows the 3.3 V regulator powered from the XT30 battery
input through SW1; USB VBUS only serves the USB protection circuit. Power the
board through its normal supply and verify the 3.3 V rail. USB alone is not a
board power source in this schematic revision.

Flash the Debug build using the methods above, then connect the board's native
USB data connector to the computer. On macOS it should create a
`/dev/cu.usbmodem...` device (VID:PID `0483:5710`). Open it with, for example,
`screen /dev/cu.usbmodem<actual-suffix> 115200`. The USB CDC baud setting does not
change the MCU clock. Exit screen using Ctrl-A, then backslash.
An FT232R `/dev/cu.usbserial...` device is a separate UART adapter and does not
confirm that the STM32 is executing this application.

The adapter in `Middlewares/ST/usbx/common/usbx_stm32_device_controllers` is
copied unchanged from ST's X-CUBE-AZRTOS-G4 2.0.0 pack. The root CMake file
builds it. USB bring-up now includes PMA buffer allocation, HAL interrupt
routing, DCD initialization, controller start, USB SOF clock recovery, distinct
CDC bulk/notification endpoint addresses, and sufficient memory pools.
The debugger symbols `diagnostic_usb_start_status` and
`diagnostic_usb_lines_sent` expose startup status and completed transmissions.

The IOC records the USB memory sizes and CDC notification endpoint (`0x82`).
After CubeMX regeneration, review the USB IRQ hookup in the `USER CODE` sections
and the application controller startup. The IOC alone does not encode all of
these application additions.

If the new image does not enumerate, connect ST-LINK to SWD and check whether
execution reaches `main`, the sensor task, or `Error_Handler`. ROM DFU
(BOOT0/reset with the required boot options) is another independent MCU/USB
check; it does not require the application to run. Lack of enumeration alone
cannot distinguish bad firmware from cabling, power, boot selection, or board
assembly. Advancing sensor samples and USB uptime confirm MCU execution and
USB communication; sensor and flash health still need their own checks.

## Hardware configuration and regeneration

Open `datalogger.ioc` in CubeMX. Keep the project name `datalogger`, CMake toolchain, and generation location at this checkout's root. The generated baseline uses STM32CubeG4 1.6.3 and X-CUBE-AZRTOS-G4 2.0.0. Commit IOC changes together with their generated sources. Put application additions in preserved `USER CODE` sections and separate modules registered in the root CMake file. Review the generated diff and rebuild both modes before a PR; see [BRANCHES.md](BRANCHES.md).

SPI1 is full-duplex master, mode 0, 8-bit, MSB first, software NSS, currently 8 MHz from a 16 MHz peripheral clock. PA5 is SCK, PA6 MISO, and PA7 MOSI. PB11/PB12/PB13 select the accelerometer/gyroscope/barometer; **PB14 (`flash_CS`) selects the W25Q32JV flash**. All four active-low selects initialize high. PA13/PA14 preserve SWD.

The sensor, processing, durable flash logger, recovery, USB storage commands and client are implemented on this branch. Simulated-device and power-loss tests exercise their behavior; hardware throughput, timing, power-loss and endurance qualification remain open. Preserve the shared 1 kHz timing configuration in `app_timing.h`, `tx_user.h` and the SysTick assembly across regeneration. The guide's example budgets are not measured hardware guarantees.

To compile the filled SDD from this repository root:

```sh
latexmk -xelatex -interaction=nonstopmode -halt-on-error -cd \
  -outdir=../../build/sdd docs/datalogger-sdd/main.tex
```

Review the guide's SWD wiring and USB hardware checks before bring-up. USB export uses a computer as host; a thumb drive requires additional host hardware. CubeMX screenshots are attributed reference examples, with the actual CrashNBurn settings specified beside them.

Have fun with the project!
