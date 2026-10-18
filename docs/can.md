# CAN Bus on Nucleo-F303K8

This document describes how to set up and test CAN (Controller Area Network)
communication on the **STM32 Nucleo-F303K8** board using Zephyr RTOS.
It is the hardware reference for the CAN loopback producer-consumer demo in
`src/main.c`.

---

## Hardware Overview

### SoC Peripheral

The STM32F303K8T6 MCU integrates a **bxCAN** controller (CAN1) that is fully
supported by Zephyr via the `st,stm32-bxcan` driver
(`drivers/can/can_stm32_bxcan.c`).

| Peripheral | Instance | RX Pin | TX Pin | AF |
|------------|----------|--------|--------|----|
| CAN1 (bxCAN) | `can1` | PA11   | PA12   | AF9 |

These pins are also used for USB DM/DP, but the Nucleo-F303K8 routes USB
through the on-board ST-LINK/V2-1 debugger, so the target MCU's USB is
**disabled by default** and PA11/PA12 are free for CAN.

> On the STM32F303, the USB_LP and CAN_RX0 interrupts share NVIC IRQ 20.
> `dts/arm/st/f3/stm32f303.dtsi` remaps USB_LP to IRQ 75 so CAN_RX0 owns
> IRQ 20 exclusively. No user action needed.

### Arduino Nano V3 Connector

The CAN signals are available on the Arduino Nano V3 connector:

| Arduino Pin | MCU Pin | Function  |
|-------------|---------|-----------|
| D8          | PA11    | CAN_RX    |
| D9          | PA12    | CAN_TX    |

### No Onboard CAN Transceiver

The Nucleo-F303K8 does **not** include an onboard CAN transceiver. The bxCAN
controller outputs **digital TX/RX** signals only. To communicate on a physical
CAN bus you **must** add an external transceiver. For hardware-free testing,
use **loopback mode** (below).

---

## Loopback Mode (No External Hardware Required)

bxCAN has a hardware loopback bit: **LBKM** (loop back mode) in the
`CAN_BTR` register (bit 30, per ST's CMSIS definitions in
`stm32f303xc.h`). When set, the controller feeds every transmitted frame
back into its own receive FIFOs (subject to acceptance filtering) without
driving the pins and without requiring an ACK from the bus. This is exactly
what the producer-consumer demo in `src/main.c` uses.

### How Zephyr enables it

There is no `CONFIG_CAN_LOOPBACK_MODE` Kconfig for the bxCAN driver. Loopback
is selected **at runtime** through the CAN controller API:

```c
err = can_set_mode(can_dev, CAN_MODE_LOOPBACK);
err = can_start(can_dev);
```

`can_stm32_set_mode()` in `drivers/can/can_stm32_bxcan.c` then does:

```c
if ((mode & CAN_MODE_LOOPBACK) != 0) {
    /* Loopback mode */
    can->BTR |= CAN_BTR_LBKM;
}
```

Bit timing is taken from the devicetree (`bitrate = <500000>` in
`app.overlay`); `can_start()` programs `CAN_BTR` and leaves initialization
mode.

### Why it works without a transceiver

A CAN controller only leaves initialization mode after seeing 11 consecutive
recessive bits on its RX pin. With no transceiver, PA11 (CAN_RX) would float.
The pinctrl node `can_rx_pa11` in
`modules/hal/stm32/dts/st/f3/stm32f303k(6-8)tx-pinctrl.dtsi` already includes
`bias-pull-up`, so the internal pull-up holds CAN_RX recessive and the
controller starts cleanly.

### Running the demo

```bash
# From this application directory
west build -b nucleo_f303k8
west flash
```

Expected output (serial console, 115200 baud):

```
[IFACE] CAN loopback up: id 0x100 @ 500 kbit/s, filter 7
[RX] frame received, seq 0
[JOB] seq 0 -> worker
[WORK] seq 0 fib(20) = 6765
[WORK] done seq 0 in 2 us
[RX] frame received, seq 1
[JOB] seq 1 xor = 0xfe (led toggled)
```

Every frame the producer sends comes back through the RX filter callback:
TX and RX counters advance together, and dropping a frame (unknown job type,
full queue) is visible as an `rx drops` message.

### Reference: ISO-TP sample in loopback mode

The upstream ISO-TP sample exposes the same mechanism through its own Kconfig:

```bash
west build -b nucleo_f303k8 \
  ${ZEPHYR_BASE}/samples/subsys/canbus/isotp \
  -- -DCONFIG_SAMPLE_LOOPBACK_MODE=y
```

`CONFIG_SAMPLE_LOOPBACK_MODE` simply ORs `CAN_MODE_LOOPBACK` into
`can_set_mode()` — identical to what this demo does directly.

---

## Real CAN Bus (Requires External Hardware)

### Required Hardware

1. **CAN transceiver** — e.g., MCP2551, TJA1051T/3, SN65HVD250, or
   equivalent.
2. **120 ohm termination resistors** — one at each physical end of the CAN bus.
3. **A remote CAN node** (second board or ECU).

### Wiring

```
Nucleo-F303K8         CAN Transceiver         CAN Bus
---------------        --------------         --------
PA12 (D9, CAN_TX) --> TXD
PA11 (D8, CAN_RX) <-- RXD
VDD (3.3 V)       --> VCC
GND               --> GND
                       CAN_H  <-->  CAN_H
                       CAN_L  <-->  CAN_L
                              |120 ohm|  (at each bus end)
```

### Build (Normal Mode)

For real bus traffic, replace the loopback call in `can_loopback_start()` with:

```c
err = can_set_mode(can_dev, CAN_MODE_NORMAL);
```

and use `CONFIG_CAN_DEFAULT_BITRATE` (or the `bitrate` property on `can1`) to
match the remote node:

```ini
# prj.conf
CONFIG_CAN_DEFAULT_BITRATE=500000
```

---

## Devicetree Configuration

The board DTS (`boards/st/nucleo_f303k8/nucleo_f303k8.dts`) does **not**
enable CAN — the `can1` node ships `status = "disabled"` from
`dts/arm/st/f3/stm32f3.dtsi`. The application enables it in `app.overlay`:

```dts
/ {
    chosen {
        zephyr,canbus = &can1;
    };
};

&can1 {
    pinctrl-0 = <&can_rx_pa11 &can_tx_pa12>;
    pinctrl-names = "default";
    bitrate = <500000>;
    status = "okay";
};
```

and pulls in the driver via Kconfig in `prj.conf`:

```ini
CONFIG_CAN=y   # CAN_STM32_BXCAN is default-y once the can1 node is okay
```

For the full file-by-file trace (bindings, generated macros, driver code),
see [device-tree.md](./device-tree.md).

---

## Troubleshooting

### Problem: `can_start()` fails / never leaves init mode

**Cause:** CAN_RX (PA11) is not recessive long enough. Without a transceiver
this happens if the pull-up is missing.

**Fix:** make sure `pinctrl-0` on `can1` references `can_rx_pa11` (it carries
`bias-pull-up`), or add an external pull-up on PA11.

### Problem: ISO-TP Errors -- FC Timeout, EPIPE, TX Error

**Symptoms:**
```
<err> isotp: Reception of next FC has timed out
TX complete cb [-2]
Error while sending data to ID 384 [-2]
Receiving error [-14]
Got 0 bytes in total
```

**Cause:** The sample is running in **normal CAN mode** (not loopback) but
there is no external CAN transceiver or remote node on the bus. Without a
transceiver:

- CAN frames are sent but never acknowledged (no ACK) -> TX fails
- No remote node sends Flow Control frames -> ISO-TP timeout (-2)
- No data arrives on the receive IDs -> receive pipe error (-14)

| Error Code | Name               | Meaning                                  |
|------------|--------------------|------------------------------------------|
| -2         | ISOTP_N_TIMEOUT_BS | Flow Control frame not received          |
| -14        | -EPIPE             | No data available on receive             |

**Solutions:**
1. Use loopback mode for testing without hardware (see above).
2. Add an external CAN transceiver and 120 ohm termination, then connect a
   real remote node (see above).

### Problem: Build Fails -- pinctrl-0 missing

**Cause:** The `can1` device tree node is enabled but lacks a `pinctrl-0`
property. The `st,stm32-bxcan` binding **requires** `pinctrl-0`/`pinctrl-names`.

**Fix:** Keep the `&can1 { pinctrl-0 = <&can_rx_pa11 &can_tx_pa12>; ... };`
block in `app.overlay` (or a board overlay).

### Problem: CAN driver not compiling

**Cause:** `CONFIG_CAN=y` is not set, or `can1` is `status = "disabled"`.

**Fix:** Add to `prj.conf`:
```ini
CONFIG_CAN=y
```
and enable the node in `app.overlay`. `CONFIG_CAN_STM32_BXCAN` is
`default y` whenever a `st,stm32-bxcan` node is enabled, so it does not need
to be set manually.

---

## Useful References

- This app: `src/main.c` (loopback producer-consumer), `app.overlay`
- Sample: `samples/subsys/canbus/isotp/` -- ISO-TP (ISO 15765-2) demo
- Driver: `drivers/can/can_stm32_bxcan.c`
- API header: `include/zephyr/drivers/can.h` (`CAN_MODE_LOOPBACK`,
  `can_set_mode()`, `can_send()`, `can_add_rx_filter()`)
- Binding: `dts/bindings/can/st,stm32-bxcan.yaml`
- Pinctrl: `modules/hal/stm32/dts/st/f3/stm32f303k(6-8)tx-pinctrl.dtsi`
- Board docs: `boards/st/nucleo_f303k8/doc/index.rst`
- Device tree trace for this demo: [docs/device-tree.md](./device-tree.md)
- Nucleo-F303K8 product page on ST.com
- STM32F303K8 datasheet (PM0223)
- STM32F3 reference manual (RM0316)
