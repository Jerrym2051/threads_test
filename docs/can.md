# CAN Bus on Nucleo-F303K8

This document describes how to set up and test CAN (Controller Area Network)
communication on the **STM32 Nucleo-F303K8** board using Zephyr RTOS.

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

### Arduino Nano V3 Connector

The CAN signals are available on the Arduino Nano V3 connector:

| Arduino Pin | MCU Pin | Function  |
|-------------|---------|-----------|
| D8          | PA11    | CAN_RX    |
| D9          | PA12    | CAN_TX    |

### No Onboard CAN Transceiver

The Nucleo-F303K8 does **not** include an onboard CAN transceiver. The bxCAN
controller outputs **digital TX/RX** signals only. To communicate on a physical
CAN bus you **must** add an external transceiver.


---

## Building and Running

### Option A: Loopback Mode (No External Hardware Required)

Loopback mode causes the CAN controller to internally route transmitted
frames back to the receiver. This is ideal for validating the Zephyr CAN and
ISO-TP stack without any external hardware.

The ISO-TP sample is used as a reference application:

```bash
# From the zephyr workspace root
west build -b nucleo_f303k8 \
  /path/to/zephyr/samples/subsys/canbus/isotp \
  -- -DCONFIG_SAMPLE_LOOPBACK_MODE=y

west flash
```

**Expected output (loopback):**
```
Start sending data
Got 247 bytes in total    <-- large payload echoed back successfully
This is the sample test for the short payload   <-- small payload echoed back
```

### Option B: Real CAN Bus (Requires External Hardware)

#### Required Hardware

1. **CAN transceiver** — e.g., MCP2551, TJA1051T/3, SN65HVD250, or
   equivalent.
2. **120 ohm termination resistors** — one at each physical end of the CAN bus.
3. **A remote CAN node** (second board or ECU) that responds to the ISO-TP
   message IDs.

#### Wiring

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

#### Build (Normal Mode)

```bash
west build -b nucleo_f303k8 \
  /path/to/zephyr/samples/subsys/canbus/isotp

west flash
```

#### CAN Bitrate

If your remote node uses a non-default bitrate, set it in your prj.conf:

```ini
# prj.conf
CONFIG_CAN_DEFAULT_BITRATE=500000
```


---

## Devicetree Configuration

The `nucleo_f303k8` board DTS enables CAN automatically. No board overlay is
required. The relevant configuration in
`boards/st/nucleo_f303k8/nucleo_f303k8.dts`:

```dts
chosen {
    zephyr,canbus = &can1;
};

&can1 {
    pinctrl-0 = <&can_rx_pa11 &can_tx_pa12>;
    pinctrl-names = "default";
    status = "okay";
};
```

And in `boards/st/nucleo_f303k8/nucleo_f303k8_defconfig`:

```ini
CONFIG_CAN=y
CONFIG_CAN_STM32_BXCAN=y
```

---

## Interrupt Remapping (Internal — No User Action Needed)

On the STM32F303, the USB low-priority interrupt (USB_LP) and CAN_RX0 share
the same NVIC interrupt position (IRQ 20) in the base SoC device tree
(`dts/arm/st/f3/stm32f3.dtsi`). The `stm32f303.dtsi` file remaps USB_LP to
IRQ 75, freeing IRQ 20 for exclusive use by CAN_RX0:

```dts
/* In dts/arm/st/f3/stm32f303.dtsi */
usb: usb@40005c00 {
    /* Remap USB_LP IRQ to enable use with CAN_1 */
    interrupts = <75 0>;
};
```

This is applied automatically to all STM32F303-based boards and requires no
changes from the user.

---

## Troubleshooting

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
1. Use loopback mode for testing without hardware (see Option A above).
2. Add an external CAN transceiver and 120 ohm termination, then connect a
   real remote node (see Option B above).

### Problem: Build Fails -- pinctrl-0 missing

**Cause:** The `can1` device tree node is enabled but lacks a `pinctrl-0`
property. This happens if `can1` is enabled directly in the SoC dtsi
(`stm32f3.dtsi`) without a board overlay providing pin configuration.

**Fix:** Ensure the board DTS has a `&can1` overlay with pinctrl-0 and
pinctrl-names.

### Problem: CAN driver not compiling

**Cause:** CONFIG_CAN=y or CONFIG_CAN_STM32_BXCAN=y is not enabled.

**Fix:** Add to prj.conf or defconfig:
```ini
CONFIG_CAN=y
CONFIG_CAN_STM32_BXCAN=y
```

---

## Useful References

- Sample: samples/subsys/canbus/isotp/ -- ISO-TP (ISO 15765-2) demo
- Driver: drivers/can/can_stm32_bxcan.c
- Binding: dts/bindings/can/st,stm32-bxcan.yaml
- Pinctrl: modules/hal/stm32/dts/st/f3/stm32f303k(6-8)tx-pinctrl.dtsi
- Board docs: boards/st/nucleo_f303k8/doc/index.rst
- Nucleo-F303K8 product page on ST.com
- STM32F303K8 datasheet (PM0223)
- STM32F3 reference manual (RM0316)
