# threads_test

A Zephyr RTOS application demonstrating **producer-consumer threading** with a
**CAN bus** transport on STM32 Nucleo boards. Built and tested on the
**Nucleo F303K8** (STM32F303K8, bxCAN CAN1).

## What this demo does

The producer thread encodes computation jobs into classic CAN frames and
transmits them with `can_send()`. The bxCAN controller runs in **loopback
mode** (`can_set_mode(dev, CAN_MODE_LOOPBACK)`), so every transmitted frame is
routed back into the controller's own receiver — **no CAN transceiver, no bus,
no second node needed**.

Each received frame lands in the driver's RX filter callback
(`can_add_rx_filter()`), which decodes the payload into a computation job and
enqueues it on a `k_msgq`:

```
payload byte 0     job type (CRC32 / Fibonacci / XOR)
payload byte 1     sequence number (makes drops visible)
payload bytes 2-5  operands
```

The dispatcher thread takes jobs from the queue: light jobs (XOR) are computed
inline and toggle the on-board LED; heavy jobs (CRC32, Fibonacci) are forwarded
to a worker thread that executes the computation and prints the result with its
duration. This keeps the original "producer generates work, consumer executes
work" architecture meaningful over a real CAN controller.

## Quick Start

```bash
# From this application directory (inside a Zephyr workspace)
west build -b nucleo_f303k8

# Flash the board
west flash
```

> The board must be plugged in via USB. Serial output appears on the
> ST-Link virtual COM port (e.g., `/dev/ttyACM0` at 115200 baud).

### Example output

```
[IFACE] CAN loopback up: id 0x100 @ 500 kbit/s, filter 7
[RX] frame received, seq 0
[JOB] seq 0 -> worker
[WORK] processing seq 0...
[WORK] seq 0 fib(20) = 6765
[WORK] done seq 0 in 2 us
[RX] frame received, seq 1
[JOB] seq 1 xor = 0xfe (led toggled)
[RX] frame received, seq 2
[JOB] seq 2 -> worker
[WORK] seq 2 crc32(4 operands) = 0xf09fdded
[WORK] done seq 2 in 1 us
```

## Project Structure

```
threads_test/
├── CMakeLists.txt        # Build configuration (board, project name, sources)
├── app.overlay           # Enables can1 (bxCAN) + zephyr,canbus chosen node
├── prj.conf              # Kconfig options (CAN, GPIO, printk, console)
├── src/
│   └── main.c            # Producer/dispatcher/worker threads over CAN loopback
├── .vscode/
│   └── c_cpp_properties.json
├── docs/
│   ├── getting-started.md  # Workspace setup, build, flash, VS Code
│   ├── threading.md        # Thread creation, priorities, states, management
│   ├── ipc.md              # Semaphores, mutexes, FIFO, message queues
│   ├── can.md              # CAN hardware, loopback mode, wiring, troubleshooting
│   └── device-tree.md      # DTS -> driver trace for the hardware this demo uses
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
- **[CAN Bus](./docs/can.md)** — Setting up and testing CAN communication on the
  Nucleo-F303K8, including loopback mode, external transceiver wiring, and
  troubleshooting.
- **[Device Tree](./docs/device-tree.md)** — How the DTS files for CAN1, pins,
  clocks and GPIO end up in the driver and in `src/main.c`.

## Cloning

Clone the repo into the zephyr workspace, such as this:

```
zephyrproject/
├── zephyr/
├── modules/
├── bootloader/
└── threads_test/   <- clone this repo here
```
