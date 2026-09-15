# CrashNBurn firmware training

Start with the [CrashNBurn Project Guide](CrashNBurn-Project-Guide.pdf). It walks through the project in order:

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
| [RFBoard26_Software_Design_Document.pdf](RFBoard26_Software_Design_Document.pdf) | Example software design document. |
| [RFBoard26_Software_Design_Document.zip](RFBoard26_Software_Design_Document.zip) | Complete editable LaTeX project for the example PDF. |
| [UBSEDS_SDD_Template_Overleaf.zip](UBSEDS_SDD_Template_Overleaf.zip) | Clean LaTeX template for your own SDD. |
| [datasheets/](datasheets/README.md) | Full flash, barometer, IMU and STM32 datasheets, with revisions and source links. |
| [build.py](build.py) | Firmware build/flash helper adapted from the sibling `analog_tears` project. |

Upload the template ZIP to Overleaf as a new project and select `main.tex` with **XeLaTeX**. Use RFBoard for documentation examples; CrashNBurn uses HAL and ThreadX without SEDSNet.

Follow the guide to generate CMake/GCC firmware in this directory, with `CMakeLists.txt` beside `build.py`, then implement your design:

```sh
python3 build.py build --debug
python3 build.py build --release
python3 build.py --help
```

On Windows, use `py -3` or your working Python 3 launcher instead of `python3`. No generated firmware is included. The helper reports a missing `CMakeLists.txt` until you generate the project. Proposed settings require review and measurement; the guide does not claim completed hardware tests.

To compile either LaTeX project locally, extract its ZIP into its own directory and run from the folder containing `main.tex`:

```sh
latexmk -xelatex -interaction=nonstopmode -halt-on-error \
  -outdir=build main.tex
```

Review the guide's SWD wiring and USB hardware checks before bring-up. USB export uses a computer as host; a thumb drive requires additional host hardware. CubeMX screenshots are attributed reference examples, with the actual CrashNBurn settings specified beside them.

Have fun with the project!
