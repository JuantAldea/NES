# NES Emulator (Just a 6502 emulator for now)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)

A Nintendo Entertainment System (NES) emulator written in C++20. This project aims to emulate the NES hardware, including the CPU and PPU, and provides a graphical debugger for inspection.

## Features

### Core Components
*   **CPU (Ricoh 2A03 / 6502):**
    *   Full implementation of all official 6502 opcodes.
    *   Includes support for common unofficial opcodes.
    *   Passes the [Klaus2m5 functional and interrupt test suites](https://github.com/Klaus2m5/6502_65C02_functional_tests).
*   **PPU (Picture Processing Unit):**
    *   OAM DMA transfer is implemented and tested.
    *   Scanline and cycle-level timing is in progress.
*   **Bus:**
    *   Manages communication between the CPU, PPU, APU, and RAM.
*   **APU (Audio Processing Unit):**
    *   Currently a stub.

### GUI Debugger (`qhex`)
The project includes a graphical debugger built with Qt and QHexView, providing:
*   A live hexadecimal view of the system memory.
*   Real-time display of CPU registers and status flags.
*   Controls for step-by-step execution, running, stopping, and resetting the emulation.

## Building the Project

### Prerequisites
*   A C++20 compatible compiler (e.g., GCC, Clang)
*   CMake (version 3.15 or later)
*   Qt5 (Core, Widgets, Gui)
*   Ninja (optional, for faster builds)

### Build Steps

```sh
# Clone the repository
git clone https://github.com/JuantAldea/NES.git
cd NES

# Configure the build using CMake
mkdir build && cd build
cmake ..
# Or, if you want to use Ninja
cmake .. -G Ninja

# Build the project
cmake --build .
```

## Usage

The build process generates two executables in the `build/` directory.

### Command-Line (`NES`)
Not production ready.

### GUI Debugger (`qhex`)
To use the graphical debugger:
```sh
./build/qhex
```
Use the "Load File" button to load a ROM into the emulator's memory at a specified address.

![qhex Screenshot](https://github.com/JuantAldea/NES/blob/master/.github/docs/qhex.png)

## Running Tests

The project uses Google Test for unit testing. The tests are built automatically with the project. To run them, execute the following command from the `build` directory:

```sh
ctest
# Or, if you used Ninja:
ninja test
# Or
make test
```
Results shall be
```
[0/1] Running tests...
Test project /tmp/NES/build
    Start 1: testCPU.6502_Klaus2m5_funtional_test
1/3 Test #1: testCPU.6502_Klaus2m5_funtional_test ...   Passed    3.60 sec
    Start 2: testCPU.6502_Klaus2m5_interrupt_test
2/3 Test #2: testCPU.6502_Klaus2m5_interrupt_test ...   Passed    0.00 sec
    Start 3: testDMA.dma_test
3/3 Test #3: testDMA.dma_test .......................   Passed    0.01 sec

100% tests passed, 0 tests failed out of 3

Total Test time (real) =   3.61 sec
```

## License
This project is licensed under the GNU General Public License v2.0. See the [LICENSE](LICENSE) file for details.
