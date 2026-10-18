# Device Tree: from DTS files to running CAN code

This document traces **every devicetree stage the CAN loopback demo touches**,
file by file, ending at the C code that actually talks to the hardware. It is
written against **Zephyr v4.1** (`v4.1-branch`) and the **nucleo_f303k8**
board; every file, node and macro named here exists in that tree.

The hardware in play:

| Block | Node | Physical |
|-------|------|----------|
| CAN controller | `can1` (bxCAN) | 0x40006400, pins PA11 (RX) / PA12 (TX), AF9 |
| Peripheral clock gate | `rcc` | APB1 bit 25 (CAN1EN) |
| Pin multiplexing | `pinctrl` | GPIOA MODER/AFR/PUPDR |
| Interrupts | `nvic` | IRQ 19 (TX), 20 (RX0), 22 (SCE) |
| LED (consumer output) | `gpiob` | PB3 (`led0`) |

---

## 0. The pipeline at a glance

```
app.overlay ─┐
boards/st/nucleo_f303k8/nucleo_f303k8.dts
   └─ dts/arm/st/f3/stm32f303X8.dtsi
        └─ dts/arm/st/f3/stm32f303.dtsi
             └─ dts/arm/st/f3/stm32f3.dtsi
                  └─ dts/arm/armv7-m.dtsi
   └─ (hal_stm32) dts/st/f3/stm32f303k(6-8)tx-pinctrl.dtsi
              │
              ▼  C preprocessor + dtc (cmake/modules/dts.cmake)
build/zephyr/zephyr.dts                      ← single merged tree
              │
              ▼  scripts/dts/gen_edt.py (edtlib) + bindings/*.yaml
build/zephyr/edt.pickle                      ← validated, typed tree
              │
              ▼  scripts/dts/gen_defines.py
build/zephyr/include/generated/zephyr/
    devicetree_generated.h                ← DT_* C macros
    (in Zephyr ≥ 4.2 this is split into devicetree_unfixed.h + devicetree_fixed.h)
              │
              ▼  drivers/can/can_stm32_bxcan.c  +  src/main.c
device objects, IRQ_CONNECT, register writes
```

`west build -b nucleo_f303k8` runs all of this before the first compiler
invocation. You can inspect the artifacts at any point — see §9.

---

## 1. `app.overlay` — what the application adds

File: `app.overlay` (this repository, next to `prj.conf`).

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

Four things happen here:

1. `status = "okay"` flips the `can1` node from `disabled` (as shipped by the
   SoC dtsi) to enabled. **Only nodes with `status = "okay"` generate driver
   instances** — this is the single most important switch in the whole chain.
2. `pinctrl-0`/`pinctrl-names` point the node at two pin groups that are
   defined *not* in the board files but in the `hal_stm32` module (§4). The
   binding `st,stm32-bxcan.yaml` declares both properties `required: true`,
   so omitting them is a build error, not a runtime surprise.
3. `bitrate = <500000>` is consumed later by the driver config macro
   `CAN_DT_DRIVER_CONFIG_INST_GET()` (§7).
4. `chosen { zephyr,canbus = &can1; }` gives the application a stable, board-
   independent handle: `DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus))` in
   `src/main.c` (§8) resolves to the CAN1 device object whatever board you
   cross-compile for.

**What the next stage does with it:** the overlay is concatenated with the
board DTS (after all its `#include`s) and fed to the device tree compiler.

---

## 2. `boards/st/nucleo_f303k8/nucleo_f303k8.dts` — the board file

```dts
#include <st/f3/stm32f303X8.dtsi>
#include <st/f3/stm32f303k(6-8)tx-pinctrl.dtsi>
```

The board file pulls in the SoC tree (§3) **and** the package-specific pin
table (§4), then describes the board:

* `chosen`: `zephyr,console = &usart2` (ST-LINK VCP on PA2), `zephyr,sram`,
  `zephyr,flash` — the demo's `printf` output goes through this one.
* `leds`: `green_led_3: green_led_3 { gpios = <&gpiob 3 GPIO_ACTIVE_HIGH>; }`
  under a `gpio-leds` compatible node, plus `aliases { led0 = &green_led_3; }`
  — used by `GPIO_DT_SPEC_GET(LED0_NODE, gpios)` in `main.c` (§8.4).
* Clock tree overrides — this is what CAN's bit timing math ultimately reads:

```dts
&rcc {
    clocks = <&pll>;
    clock-frequency = <DT_FREQ_M(36)>;
    ahb-prescaler = <1>;
    apb1-prescaler = <2>;      /* → APB1 = 18 MHz, CAN1's kernel clock */
    apb2-prescaler = <1>;
};
```

* Note what is **absent**: the board DTS does *not* enable `can1` and does
  *not* set `zephyr,canbus`. (Older revisions of `docs/can.md` claimed it
  did — it does not, in v4.1 or today.) That is exactly why this application
  ships an `app.overlay`.

**What the next stage does with it:** `#include` resolution continues down the
SoC chain; `/omit-if-no-ref/` nodes (§4) are dropped unless referenced — and
the only thing referencing them is the `pinctrl-0` in `app.overlay`.

---

## 3. The SoC include chain: `stm32f303X8.dtsi → stm32f303.dtsi → stm32f3.dtsi → armv7-m.dtsi`

All under `dts/arm/st/f3/`.

### 3.1 `stm32f303X8.dtsi` (die size for the K8 package)

`#include <st/f3/stm32f303.dtsi>`, then sets `sram0` to 12 KB, `flash0` to
64 KB, adds CCM RAM, and `/delete-node/` removes peripherals the K8 package
doesn't bond out (spi2, spi3). Nothing CAN-specific — but if you ever wonder
why a link error says "region `SRAM` overflowed", this is the file that sized
it.

### 3.2 `stm32f303.dtsi` (series: F303)

Declares the SoC `compatible = "st,stm32f303", "st,stm32f3", "simple-bus";`
and adds/overrides F303 peripherals. One line matters enormously for CAN on
this chip:

```dts
usb: usb@40005c00 {
    /* Remap USB_LP IRQ to enable use with CAN_1 */
    interrupts = <75 0>;
};
```

On the base part (stm32f3.dtsi) USB_LP and CAN_RX0 share NVIC IRQ 20. Here
USB_LP is moved to its alternate position 75, leaving IRQ 20 unambiguously to
CAN1's `rx0` interrupt — which the driver's `IRQ_CONNECT` then uses (§7).

### 3.3 `stm32f3.dtsi` (family: all F3)

Defines the peripherals shared by every F3, including the four nodes the demo
depends on.

**CAN1 (disabled, awaiting §1):**

```dts
can1: can@40006400 {
    compatible = "st,stm32-bxcan";
    reg = <0x40006400 0x400>;
    interrupts = <19 0>, <20 0>, <21 0>, <22 0>;
    interrupt-names = "TX", "RX0", "RX1", "SCE";
    clocks = <&rcc STM32_CLOCK(APB1, 25U)>;
    status = "disabled";
};
```

* `compatible = "st,stm32-bxcan"` is the string that later matches the driver
  (§6/§7) — the *only* link between a DTS node and a C driver.
* `clocks` uses `STM32_CLOCK(bus, bit)` from
  `include/zephyr/dt-bindings/clock/stm32_common_clocks.h`:

  ```c
  #define STM32_CLOCK(bus, bit) (STM32_CLOCK_BUS_##bus) (1 << bit)
  ```

  with `STM32_CLOCK_BUS_APB1 = 0x01c` (`stm32f3_clock.h`). The cell pair
  `(bus, bit)` = `(0x01c, 1<<25)` is the RCC APB1 enable-register offset and
  the CAN1EN bit position — exactly what the clock driver will write to
  `RCC->APB1ENR` (§7.3).
* `interrupt-names` lets the driver ask for "the rx0 interrupt" by name
  instead of hardcoding index 20 (§7.2).

**RCC (clock controller):**

```dts
rcc: rcc@40021000 {
    compatible = "st,stm32f3-rcc";
    #clock-cells = <2>;
    ...
};
```

`#clock-cells = <2>` is what makes the `<&rcc bus bit>` phandle legal; the
matching driver is `drivers/clock_control/clock_stm32f0_f3.c`.

**PINCTRL + GPIOA/B (pins):**

```dts
pinctrl: pin-controller@48000000 {
    compatible = "st,stm32-pinctrl";
    ...
    gpioa: gpio@48000000 {
        compatible = "st,stm32-gpio";
        gpio-controller; #gpio-cells = <2>;
        reg = <0x48000000 0x400>;
        clocks = <&rcc STM32_CLOCK(AHB1, 17U)>;
    };
    gpiob: gpio@48000400 { ... clocks = <&rcc STM32_CLOCK(AHB1, 18U)>; };
};
```

`pinctrl` is the node the package pin table (§4) *appends children to*.
`gpiob` drives the user LED; its driver is `drivers/gpio/gpio_stm32.c`.

### 3.4 `dts/arm/armv7-m.dtsi` (architecture common)

`stm32f3.dtsi` starts with `#include <arm/armv7-m.dtsi>`, which defines
`cpu0` (Cortex-M4), the `soc` and `chosen` skeletons, and the interrupt parent
every `interrupts = <...>` property refers to:

```dts
nvic: interrupt-controller@e000e100 {
    compatible = "arm,v7m-nvic";
    interrupt-controller; #interrupt-cells = <2>;
};
```

`stm32f3.dtsi` later does `&nvic { arm,num-irq-priority-bits = <4>; }`, which
feeds `CONFIG_NUM_IRQ_PRIO_BITS` — used by `IRQ_CONNECT`'s priority argument.

**What the next stage does with it:** after preprocessing, the merged tree
contains a complete but *disabled* `can1` node; only `app.overlay` (§1) makes
it live.

---

## 4. `modules/hal/stm32/dts/st/f3/stm32f303k(6-8)tx-pinctrl.dtsi` — where PA11/PA12 get defined

This file lives in the **`hal_stm32` module**, not in the Zephyr repo — it is
generated per-package by ST's `genpinctrl.py` and is auto-included by the
board DTS (§2). It adds children to the SoC's `pinctrl` node:

```dts
/ {
    soc {
        pinctrl: pin-controller@48000000 {

            /omit-if-no-ref/ can_rx_pa11: can_rx_pa11 {
                pinmux = <STM32_PINMUX('A', 11, AF9)>;
                bias-pull-up;
            };

            /omit-if-no-ref/ can_tx_pa12: can_tx_pa12 {
                pinmux = <STM32_PINMUX('A', 12, AF9)>;
            };
            ...
        };
    }
};
```

* `STM32_PINMUX` comes from
  `include/zephyr/dt-bindings/pinctrl/stm32-pinctrl.h` and packs the pin into
  one integer: `port` in bits 13:9, `line` in bits 8:5, mode/AF in bits 4:0.
  `AF9` is `STM32_AF9 = 0x9`, so `can_rx_pa11` encodes to
  `('A'-'A')<<9 | 11<<5 | 9` = **0x169**. The pinctrl driver unpacks it with
  `STM32_DT_PINMUX_PORT/LINE/FUNC()` — no lookup table needed.
* `/omit-if-no-ref/` means: keep these nodes only if something references
  them. `app.overlay`'s `pinctrl-0 = <&can_rx_pa11 &can_tx_pa12>` is that
  reference. (This is also why enabling `can1` without `pinctrl-0` fails at
  the binding check rather than silently misconfiguring pins.)
* `bias-pull-up` is not cosmetic: a CAN controller only leaves initialization
  mode after 11 consecutive recessive bits on RX. Without an onboard
  transceiver, PA11 would float — the internal pull-up holds it recessive so
  loopback mode can actually start (§8.2). This property is consumed by
  `st,stm32-pinctrl.yaml` → `struct stm32_pinctrl_config` → `PUPDR` writes.

---

## 5. Bindings — the contract between DTS and C

Bindings are YAML schema files in `dts/bindings/`, matched by `compatible`.
`gen_edt.py` (build stage 2) uses them to type-check and parse every property.

| File | Matches | What it enforces for this demo |
|------|---------|-------------------------------|
| `dts/bindings/can/st,stm32-bxcan.yaml` | `can1` | `include: [can-controller.yaml, pinctrl-device.yaml]`; `reg`, `interrupts`, `clocks`, `pinctrl-0`, `pinctrl-names` all `required: true` |
| `dts/bindings/can/can-controller.yaml` | (included above) | defines `bitrate` (int, bit/s), `sample-point` (per-mille), `phys`, child `can-transceiver` |
| `dts/bindings/pinctrl/st,stm32-pinctrl.yaml` | `pinctrl` children | `pinmux` (required), `bias-pull-up`, `bias-pull-down`, `drive-open-drain`, ... |
| `dts/bindings/gpio/st,stm32-gpio.yaml` | `gpioa..g` | `gpio-controller`, `#gpio-cells = <2>` |
| `dts/bindings/clock/st,stm32f3-rcc.yaml` | `rcc` | `#clock-cells = <2>`, PLL child nodes |
| `dts/bindings/interrupt-controller/arm,v7m-nvic.yaml` | `nvic` | `arm,num-irq-priority-bits` |

If `app.overlay` had said `status = "okay"` but forgotten `pinctrl-0`,
the build would stop right here with a binding validation error — before
any C code is compiled.

---

## 6. Kconfig ↔ devicetree glue

`prj.conf` sets only `CONFIG_CAN=y` (plus GPIO/console). The rest is derived
from the *enabled* devicetree nodes, via
`drivers/can/Kconfig.stm32`:

```kconfig
config CAN_STM32_BXCAN
	bool "STM32 bxCAN driver"
	default y
	depends on DT_HAS_ST_STM32_BXCAN_ENABLED
	select PINCTRL
```

`DT_HAS_ST_STM32_BXCAN_ENABLED` is itself a generated macro — it expands to 1
iff at least one node with `compatible = "st,stm32-bxcan"` has
`status = "okay"`. In other words: **the `status = "okay"` in `app.overlay`
turns the driver's Kconfig on by itself.** `select PINCTRL` also pulls in the
pinctrl driver (`drivers/pinctrl/pinctrl_stm32.c`), because the CAN driver
calls `pinctrl_apply_state()`.

`CAN=y` additionally sets `CONFIG_CAN_INIT_PRIORITY` (default 80), which is
the boot phase at which the device instance is registered (§7.1).

---

## 7. Generated macros → driver instantiation

### 7.1 What `gen_defines.py` emits

For every node it emits path/label macros into
`build/zephyr/include/generated/zephyr/devicetree_generated.h`. These are the
**actual lines from this demo's build** (v4.1 emits one file; Zephyr ≥ 4.2
splits it into `devicetree_unfixed.h` + `devicetree_fixed.h`):

```c
#define DT_N_S_soc_S_can_40006400_P_bitrate   500000      /* from app.overlay */
#define DT_N_S_soc_S_can_40006400_P_status    "okay"     /* from app.overlay */
#define DT_N_S_soc_S_can_40006400_P_compatible {"st,stm32-bxcan"}
#define DT_N_S_soc_S_can_40006400_STATUS_okay 1
#define DT_N_NODELABEL_can1      DT_N_S_soc_S_can_40006400
#define DT_CHOSEN_zephyr_canbus  DT_N_S_soc_S_can_40006400
#define DT_N_S_soc_S_can_40006400_ORD 40
#define DT_FOREACH_OKAY_INST_st_stm32_bxcan(fn) fn(0)
```

`include/zephyr/devicetree.h` wraps these into the friendly API:
`DT_NODELABEL(can1)` → `DT_N_NODELABEL_can1` → the path macro, and likewise
`DT_PROP(node, bitrate)`, `DT_CHOSEN(zephyr_canbus)`,
`DT_REG_ADDR(node)`, `DT_IRQ_BY_NAME(node, rx0, irq)`. The `ORD 40` line is
the node's device ordinal — the number that ties the DTS node to a `struct
device` (§8.1). And `DT_FOREACH_OKAY_INST_st_stm32_bxcan` tells the driver
exactly which instances to instantiate — note it passes `fn(0)`: instance 0
exists *because* `app.overlay` set the status; a `disabled` node simply never
appears in this list.

### 7.2 The driver binds to `compatible` via `DT_DRV_COMPAT`

`drivers/can/can_stm32_bxcan.c` (v4.1) begins with:

```c
#define DT_DRV_COMPAT st_stm32_bxcan
```

and ends with:

```c
DT_INST_FOREACH_STATUS_OKAY(CAN_STM32_INST)
```

`DT_INST_FOREACH_STATUS_OKAY` expands **only for okay-status instances** —
one expansion, `inst = 0`, for our `can1`. `CAN_STM32_INST(0)` expands to
four blocks:

```c
CAN_STM32_IRQ_INST(0)      /* config_can_0_irq(): IRQ_CONNECT from DT      */
CAN_STM32_CONFIG_INST(0)   /* const struct can_stm32_config                */
CAN_STM32_DATA_INST(0)     /* driver RAM                                   */
CAN_STM32_DEFINE_INST(0)   /* the device object                            */
```

The config struct is pure devicetree read-out:

```c
#define CAN_STM32_CONFIG_INST(inst)                                      \
PINCTRL_DT_INST_DEFINE(inst);                                            \
static const struct can_stm32_config can_stm32_cfg_##inst = {            \
	.common = CAN_DT_DRIVER_CONFIG_INST_GET(inst, 0, 1000000),       \
	.can = (CAN_TypeDef *)DT_INST_REG_ADDR(inst),                    \
	.master_can = (CAN_TypeDef *)DT_INST_PROP_OR(inst,               \
		master_can_reg, DT_INST_REG_ADDR(inst)),                 \
	.pclken = {                                                      \
		.enr = DT_INST_CLOCKS_CELL(inst, bits),                  \
		.bus = DT_INST_CLOCKS_CELL(inst, bus),                   \
	},                                                               \
	.config_irq = config_can_##inst##_irq,                           \
	.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst),                    \
};
```

Connecting this to our DTS values, one by one:

| Driver field | Macro | Resolves to (from §1–§4) |
|---|---|---|
| `can` | `DT_INST_REG_ADDR(0)` | `0x40006400` (from `stm32f3.dtsi`) |
| `common.bitrate` | `CAN_DT_DRIVER_CONFIG_GET` → `DT_PROP_OR(node, bitrate, CONFIG_CAN_DEFAULT_BITRATE)` | `500000` (from `app.overlay`) |
| `pclken.bus` / `.enr` | `DT_INST_CLOCKS_CELL(0, bus/bits)` | `0x01c` / `1<<25` (from `STM32_CLOCK(APB1, 25U)`) |
| `pcfg` | `PINCTRL_DT_INST_DEV_CONFIG_GET(0)` | two-pin array: `{0x169, pull-up cfg}`, `{0x189, 0}` |
| filter banks | `CONFIG_CAN_MAX_STD_ID_FILTER` / `CAN_MAX_EXT_ID_FILTER` (Kconfig) | 7 ext + 14 std filter ids over bxCAN's 14 banks (2 std filters per bank) |

The device registration:

```c
CAN_DEVICE_DT_INST_DEFINE(inst, can_stm32_init, NULL,
			  &can_stm32_dev_data_##inst, &can_stm32_cfg_##inst,
			  POST_KERNEL, CONFIG_CAN_INIT_PRIORITY,
			  &can_api_funcs);
```

`CAN_DEVICE_DT_INST_DEFINE` wraps `CAN_DEVICE_DT_DEFINE(DT_DRV_INST(inst),
...)`, which wraps `Z_DEVICE_DEFINE(...)` — this emits the `struct device`
entry tagged with `can1`'s **ordinal** (40, §7.1) as `__device_dts_ord_40`.
That ordinal is the bridge to §8.

Interrupts (non-F0 path of `CAN_STM32_IRQ_INST`):

```c
IRQ_CONNECT(DT_INST_IRQ_BY_NAME(inst, rx0, irq),
	    DT_INST_IRQ_BY_NAME(inst, rx0, priority),
	    can_stm32_rx_isr, DEVICE_DT_INST_GET(inst), 0);
irq_enable(DT_INST_IRQ_BY_NAME(inst, rx0, irq));
/* ... same for tx (IRQ 19) and sce (IRQ 22) */
```

`DT_INST_IRQ_BY_NAME(0, rx0, irq)` looks up the *name* in
`interrupt-names = "TX", "RX0", "RX1", "SCE"` and returns **20** — the number
that `stm32f303.dtsi`'s USB remap (§3.2) made available. No file in the chain
hardcodes "CAN RX interrupt is 20" in C; the name lookup does.

### 7.3 What runs at boot (POST_KERNEL, prio 80)

`can_stm32_init()` is where the DTS values become register writes, in order:

1. `DEVICE_DT_GET(STM32_CLOCK_CONTROL_NODE)` — `STM32_CLOCK_CONTROL_NODE` is
   literally `DT_NODELABEL(rcc)` (`stm32_clock_control.h`); gets the clock
   driver device for the `st,stm32f3-rcc` node.
2. `clock_control_on(clock, &cfg->pclken)` → `clock_stm32f0_f3.c` writes
   `RCC->APB1ENR |= (1 << 25)` (bus offset `0x01c` decoded from the DT cell).
   CAN1's 18 MHz APB1 clock is now live.
3. `pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT)` →
   `pinctrl_stm32.c` unpacks each `pinmux` integer and calls
   `gpio_stm32_configure()` (`drivers/gpio/gpio_stm32.c`), which writes the
   pad registers through ST's LL API: `LL_GPIO_SetPinMode` (MODER =
   alternate function), `LL_GPIO_SetAFPin_8_15` (AFR[1] = 9 for pins 11/12),
   and `LL_GPIO_SetPinPull` (PUPDR = pull-up on PA11 from `bias-pull-up`).
4. Enter init mode (`MCR.INRQ`), leave sleep, configure filter banks
   (`FMR.FINIT`/`FS1R`) for 32-bit mask filters.
5. `can_stm32_get_core_clock()` → `clock_control_get_rate()` returns
   **18 000 000** (PLL 36 MHz ÷ `apb1-prescaler = <2>`, from the board DTS
   §2); `can_calc_timing(dev, &timing, 500000, 0)` derives BTR fields
   (BRP/TS1/TS2/SJW) for 500 kbit/s; `can_set_timing()` writes `CAN_BTR`.
6. `can_stm32_set_mode(dev, CAN_MODE_NORMAL)` — clears LBKM/SILM for now.
7. `cfg->config_irq(can)` — the `IRQ_CONNECT`s above, plus enabling
   `CAN_IER` TX/RX0/RX1/ERR/BOF interrupts.

The controller is now **parked in initialization mode with everything
programmed** — waiting for the application to choose its mode.

---

## 8. Application code: `src/main.c`

### 8.1 Getting the device

```c
static const struct device *const can_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
```

Expansion: `DT_CHOSEN(zephyr_canbus)` → `DT_CHOSEN_zephyr_canbus` (emitted
because `app.overlay` set the chosen node) → `DT_N_S_soc_S_can_40006400` →
ordinal 40 (`DT_..._ORD 40`, §7.1) → `&__device_dts_ord_40` — the exact
`struct device` that `CAN_DEVICE_DT_INST_DEFINE` registered in §7.2. You can
see it in the linked image:

```
$ arm-zephyr-eabi-nm build/zephyr/zephyr.elf | grep dts_ord_40
0800491c R __device_dts_ord_40
08004d48 r can_stm32_cfg_0
```

This is a compile-time pointer — if `can1` were `status = "disabled"`, the
build would fail here with an "undefined device reference" error, not boot
with a surprise.

### 8.2 Loopback + start

```c
can_set_mode(can_dev, CAN_MODE_LOOPBACK);
can_start(can_dev);
```

`can_set_mode` is a syscall wrapper → `z_impl_can_set_mode` → api table entry
`can_stm32_set_mode`, which does (because the DT node enabled this driver):

```c
if ((mode & CAN_MODE_LOOPBACK) != 0) {
	can->BTR |= CAN_BTR_LBKM;   /* bit 30, stm32f303xc.h CMSIS defs */
}
```

The controller is still in init mode (from §7.3 step 7 — `can_start` hasn't
run yet), so the `BTR` write is legal. `can_start` →
`can_stm32_leave_init_mode()` waits for `MSR.INAK` to clear, which requires
11 recessive bits on CAN_RX — provided by the `bias-pull-up` on PA11 (§4)
because there is no transceiver. With LBKM set, bxCAN internally connects its
own TX to its RX: every frame we send comes back through the acceptance
filters, and no ACK is needed from a bus.

### 8.3 Filters, send, receive

```c
can_add_rx_filter(can_dev, can_rx_callback, NULL,
		  &(struct can_filter){ .id = 0x100,
					.mask = CAN_STD_ID_MASK,
					.flags = 0 });
```

The driver allocates one of the 14 bxCAN filter banks — standard-ID filters
start at filter id `CONFIG_CAN_MAX_EXT_ID_FILTER` (7), since ids 0–6 are
reserved for extended-ID filters, so this returns **7** (printed as
`filter 7` in the demo output), and the ISR path becomes:

```
can_send(frame id=0x100)            → TX mailbox TIR/TDTR/TDR loaded, TMEIE set
bxCAN loopback (LBKM)               → frame appears in RX FIFO 0, bank 7 matched
CAN_RX0 IRQ 20                      → can_stm32_rx_isr() [DT-connected §7.2]
 → can_stm32_rx_isr_handler(): RDTR filter-id → data->rx_cb_std[0]
 → can_rx_callback()                [app: frame → job_msg]
 → k_msgq_put(&job_q)               → dispatcher/worker threads [app]
 → RF0R |= RFOM0                    [release FIFO]
```

### 8.4 The LED path (same machinery, smaller scale)

```c
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
```

`DT_ALIAS(led0)` → `DT_ALIAS_led0` → `green_led_3` (board DTS §2) →
`gpios = <&gpiob 3 GPIO_ACTIVE_HIGH>`; `GPIO_DT_SPEC_GET` extracts port =
`DEVICE_DT_GET(DT_GPIO_CTLR(...))` = the `gpiob` device (driver
`drivers/gpio/gpio_stm32.c`, compatible `st,stm32-gpio`, clock `AHB1 bit 18`)
and pin 3. `gpio_pin_toggle_dt(&led)` writes GPIOB_ODR bit 3. Identical
DT→macro→device-object pattern as CAN, one level smaller.

---

## 9. Inspecting the chain yourself

```bash
# Merged, preprocessed tree (what dtc saw):
$ less build/zephyr/zephyr.dts

# Generated C macros (the exact lines quoted in §7.1):
grep -r "DT_CHOSEN_zephyr_canbus\|DT_N_S_soc_S_can_40006400_P_bitrate" \
     build/zephyr/include/generated/zephyr/

# Effective Kconfig showing DT-driven symbols:
grep "CAN_STM32_BXCAN\|CAN_INIT_PRIORITY\|DT_HAS_ST_STM32_BXCAN" build/zephyr/.config

# Which files feed the build (board dts, SoC chain, overlay, bindings):
grep -o "[^ ]*\.dts[i]*" build/build.ninja | sort -u
```

---

## 10. Summary — one sentence per stage

| Stage | File | Role for this demo |
|---|---|---|
| app | `app.overlay` | enables `can1`, sets `bitrate`, picks PA11/PA12 groups, publishes `zephyr,canbus` |
| board | `nucleo_f303k8.dts` | clock tree (36 MHz PLL, APB1 ÷2), LEDs, console; includes SoC + pin table |
| SoC die | `stm32f303X8.dtsi` | memory sizes for the K8 package |
| series | `stm32f303.dtsi` | frees IRQ 20 for CAN by remapping USB_LP → 75 |
| family | `stm32f3.dtsi` | `can1` node: `st,stm32-bxcan`, reg, named IRQs, `STM32_CLOCK(APB1, 25U)`; `rcc`, `pinctrl`, `gpioa/b` |
| arch | `armv7-m.dtsi` | Cortex-M4 + `nvic` interrupt parent |
| HAL | `stm32f303k(6-8)tx-pinctrl.dtsi` | `can_rx_pa11` (AF9, pull-up) / `can_tx_pa12` (AF9), `/omit-if-no-ref/` |
| bindings | `st,stm32-bxcan.yaml` etc. | schema: required pinctrl, `bitrate` type, clock cells |
| kconfig | `Kconfig.stm32` | `CAN_STM32_BXCAN` auto-`y` from enabled DT node |
| generated | `devicetree_unfixed.h` | `DT_NODELABEL_can1`, `DT_CHOSEN_zephyr_canbus`, property macros |
| driver | `can_stm32_bxcan.c` | `DT_DRV_COMPAT` → device object, config, `IRQ_CONNECT`, init sequence |
| app code | `src/main.c` | `DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus))`, `CAN_MODE_LOOPBACK`, filters, msgq |
