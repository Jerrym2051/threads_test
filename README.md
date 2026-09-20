# threads_test

A Zephyr RTOS application demonstrating **basic threading** and **inter-thread
communication (IPC)** patterns on STM32 Nucleo boards. Built and tested on the
**Nucleo F303K8**.

## Quick Start

```bash
# Activate the Python virtual environment
source .venv/bin/activate

# Build for nucleo_f303k8
west build -b nucleo_f303k8 --spec threads_test/

# Flash the board
west flash
```

> The board must be plugged in via USB. Serial output appears on the
> ST-Link virtual COM port (e.g., `/dev/ttyACM0` at 115200 baud).

## Project Structure

```
threads_test/
├── CMakeLists.txt        # Build configuration (board, project name, sources)
├── prj.conf              # Kconfig options (GPIO, printk, console, etc.)
├── src/
│   └── main.c            # Application entry: blink_thread + print_thread
├── .vscode/
│   └── c_cpp_properties.json
├── docs/
│   ├── getting-started.md  # Workspace setup, build, flash, VS Code
│   ├── threading.md        # Thread creation, priorities, states, management
│   └── ipc.md              # Semaphores, mutexes, FIFO, message queues
└── README.md
```

## Documentation

- **[Getting Started](./docs/getting-started.md)** — Prerequisites, workspace
  setup, building, flashing, and VS Code integration.
- **[Threading](./docs/threading.md)** — How to create and manage threads in
  Zephyr, including priorities, scheduling, and the `K_THREAD_DEFINE` macro.
- **[IPC (Inter-Process Communication)](./docs/ipc.md)** — Semaphores, mutexes,
  FIFO queues, message queues, pipes, and memory slabs for inter-thread
  coordination.

## Cloning

Clone the repo into the zephyr workspace, such as this:

```
zephyrproject/
├── zephyr/
├── modules/
├── bootloader/
└── threads_test/   <- clone this repo here
```
