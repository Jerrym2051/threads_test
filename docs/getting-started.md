# Getting Started with `threads_test`

This guide walks you through setting up a Zephyr RTOS workspace, building the
`threads_test` application, flashing it to a board, and monitoring serial output.

---

## Prerequisites

The following tools must be installed on your host machine (Linux assumed):

| Tool | Minimum Version | Purpose |
|------|----------------|---------|
| **CMake** | 3.28.0 | Build system generator |
| **Ninja** | 1.11+ | Build backend (faster than Make) |
| **Python** | 3.10+ | West, flashing tools |
| **Git** | 2.30+ | Repository cloning |
| **Zephyr SDK** | 1.0.1 | Cross-compilation toolchain (GCC ARM) |
| **PyOCD** | 0.45+ | On-board debugger / flash runner |
| **West** | 1.5.0 | Zephyr meta-tool for multi-repo management |

> **Note:** The Zephyr SDK is typically installed at `/opt/zephyr-sdk-1.0.1`
> (or wherever you installed it) and the Python virtual environment is at
> the workspace root in `.venv`. Activate it before proceeding:

```bash
source .venv/bin/activate
```

---

## 1. Set up the Zephyr Workspace

The `threads_test` project is an **application** that lives *inside* a Zephyr
workspace. The workspace must already contain the `zephyr/` tree, `modules/`,
and `bootloader/` directories (set up via `west`).

### Folder Layout

```
zephyrproject/
├── zephyr/              # Main Zephyr RTOS repository
├── modules/             # Zephyr modules (HAL, etc.)
├── bootloader/          # MCUboot (if used)
├── .west/               # West workspace metadata
├── .venv/               # Python virtual environment
└── threads_test/        # <-- This application
    ├── CMakeLists.txt
    ├── prj.conf
    ├── README.md
    ├── docs/
    │   ├── getting-started.md
    │   ├── threading.md
    │   └── ipc.md
    ├── src/
    │   └── main.c
    └── .vscode/
        └── c_cpp_properties.json
```

### If Starting from Scratch

```bash
# 1. Create and enter the workspace directory
mkdir zephyrproject && cd zephyrproject

# 2. Initialize the West workspace
west init .

# 3. Update all Zephyr repositories
west update

# 4. Export Zephyr environment scripts
west zephyr-export

# 5. Install Python dependencies
pip install -r zephyr/scripts/requirements.txt
```

### Clone This Project

Clone `threads_test` into the workspace root:

```bash
cd zephyrproject
git clone https://github.com/your-github-username/threads_test.git
```

## 2. Project Structure

Every Zephyr application has the same three essential files:

### `CMakeLists.txt`

The top-level build configuration file. It tells CMake which board to target,
sets the project name, and lists source files.

```cmake
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.28.0)

# Set the flash runner (used by `west flash`)
set(BOARD_FLASH_RUNNER pyocd)

# Load the Zephyr build system
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

# Application name
project(threads_test)

# Source files
target_sources(app PRIVATE src/main.c)
```

### `prj.conf`

The Kconfig configuration file. Each `CONFIG_*` line enables or configures a
Zephyr subsystem. These settings are merged with board defaults at build time.

```kconfig
CONFIG_GPIO=y       # Enable GPIO driver API
CONFIG_PRINTK=y     # Enable low-level print output
CONFIG_CONSOLE=y    # Enable console driver
CONFIG_SERIAL=y     # Enable serial driver (needed for console output)
```

### `src/main.c`

The main application source file. It defines threads and their entry-point
functions, and wires them up using `K_THREAD_DEFINE`.

---

## 3. Building the Project

### Select a Board

The board is specified with the `-b` (or `--board`) flag. This project was
developed and tested on the **STM32 Nucleo F303K8**:

```bash
west build -b nucleo_f303k8 --spec threads_test/
```

> `--spec threads_test/` tells West to use the application directory
> `threads_test/` relative to the current working directory.

Build output is placed in `zephyrproject/build/` by default. The final binary
is `build/zephyr/zephyr.hex` (for flashing) and `build/zephyr/zephyr.elf`
(for debugging).

### Clean Rebuild

```bash
# Wipe the build directory and start fresh
rm -rf build
west build -b nucleo_f303k8 --spec threads_test/
```

### Verbose Build

```bash
west build -b nucleo_f303k8 --spec threads_test/ -- -v
```

---

## 4. Flashing

### Connect the Board

Plug the Nucleo board into your computer via USB. The ST-LINK debugger is
exposed as a USB device. On Linux, ensure your user has permission to access
it (typically via the `dialout` group or udev rules).

### Flash the Binary

```bash
west flash
```

This builds (if needed) and flashes the hex file to the target using the
configured flash runner (`pyocd`).

### Board-Specific Flash Runners

The `BOARD_FLASH_RUNNER` in `CMakeLists.txt` can be overridden on the command
line:

```bash
west flash --flash-runner openocd
west flash --flash-runner dfu-util
```

---

## 5. Serial Output / Monitoring

The `print_thread` in `main.c` writes to the console via `printf()`. To see
the output:

### Using `west` with an External Terminal

```bash
# Find the serial device
ls /dev/ttyACM* /dev/ttyUSB*

# Open a terminal (e.g., with screen)
screen /dev/ttyACM0 115200
```

Press `Ctrl+A` then `K` to exit `screen`.

### Using PyOCD (if Supported by the Board)

```bash
pyocd gdb
```

### Baud Rate

The default Zephyr console baud rate is typically **115200** unless the board
defconfig overrides it. Check:

```bash
grep "CONFIG_SERIAL_Baud" build/zephyr/.config
```

---

## 6. VS Code Setup

The project includes a `.vscode/c_cpp_properties.json` for IntelliSense support.
The configuration points to the build directory's `compile_commands.json`, which
is generated automatically by CMake:

```json
{
    "configurations": [
        {
            "name": "Zephyr",
            "compileCommands": "${workspaceFolder}/../build/compile_commands.json",
            "cStandard": "c11",
            "intelliSenseMode": "gcc-arm"
        }
    ],
    "version": 4
}
```

### Recommended Extensions

Install these VS Code extensions in addition to the C/C++ extension:

- **CMake Tools** – provides CMake integration and IntelliSense
- **Cortex-Debug** – for debugging STM32 targets
- **Zephyr** – provides Kconfig highlighting and navigation

### Regenerating `compile_commands.json`

If the file is missing or stale, rebuild:

```bash
cd build
cmake --build . --target configure  # regenerates compile_commands.json
```

---

## 7. Configuration Reference

### Essential Kconfig Options

| Option | Default | Description |
|--------|---------|-------------|
| `CONFIG_GPIO` | `y` | Enable GPIO driver API (used by `blink_thread`) |
| `CONFIG_PRINTK` | `y` | Enable `printk()` low-level output |
| `CONFIG_CONSOLE` | `y` | Enable the console driver subsystem |
| `CONFIG_SERIAL` | *(implied)* | Enable serial UART driver (required for console) |
| `CONFIG_MULTITHREADING` | `y` | Enable multi-threading (needed for threads) |

### Common Debugging Options

Add these to `prj.conf` for development:

```kconfig
CONFIG_DEBUG=y
CONFIG_LOG=y
CONFIG_LOG_DEFAULT_LEVEL=3
CONFIG_THREAD_MONITOR=y
CONFIG_THREAD_STACK_INFO=y
CONFIG_DEBUG_STACK_OVERFLOW=y
```

---

## Quick Reference Cheat Sheet

```bash
# Activate venv
source .venv/bin/activate

# Build for nucleo_f303k8
west build -b nucleo_f303k8 --spec threads_test/

# Flash
west flash

# Clean build
rm -rf build && west build -b nucleo_f303k8 --spec threads_test/

# Open serial terminal
screen /dev/ttyACM0 115200
```

---

> **Next:** Read [`threading.md`](./threading.md) for an in-depth guide to
> Zephyr threads, or [`ipc.md`](./ipc.md) for inter-thread communication
> primitives.
