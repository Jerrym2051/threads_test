# Threading in Zephyr

This guide covers the fundamentals of creating and managing threads in the
Zephyr RTOS. It builds directly on the patterns used in the `threads_test`
application's `src/main.c`.

---

## What Is a Thread?

A **thread** in Zephyr is a unit of execution with its own stack, priority, and
execution context. When multiple threads exist, the kernel's scheduler decides
which thread runs at any given time.

Each thread has:

- **A stack** – private memory for function calls and local variables.
- **A priority** – determines scheduling order.
- **An entry function** – the C function the thread starts executing.
- **A state** – running, ready, suspended, etc.

---

## Thread Priorities

Zephyr uses two priority ranges:

| Range | Macro | Description |
|-------|-------|-------------|
| **Cooperative** | `K_PRIO_COOP(x)` | Negative values (`-CONFIG_NUM_COOP_PRIORITIES` to `-1`). Highest urgency. |
| **Preemptible** | `K_PRIO_PREEMPT(x)` | Non-negative values (`0` to `CONFIG_NUM_PREEMPT_PRIORITIES - 1`). Lower numbers = higher priority. |

### Key Macros (from `kernel.h`)

```c
K_PRIO_COOP(x)          // Cooperative priority, lower x = higher urgency
K_PRIO_PREEMPT(x)       // Preemptible priority, lower x = higher urgency
K_HIGHEST_THREAD_PRIO   // Most urgent priority (-CONFIG_NUM_COOP_PRIORITIES)
K_LOWEST_THREAD_PRIO    // Least urgent (CONFIG_NUM_PREEMPT_PRIORITIES)
K_IDLE_PRIO             // Priority of the idle thread
```

The defaults are `CONFIG_NUM_COOP_PRIORITIES=16` and
`CONFIG_NUM_PREEMPT_PRIORITIES=15`.

### Cooperative vs Preemptible

- **Cooperative threads** run until they voluntarily yield (sleep, wait on an
  object). They are never preempted by another thread.
- **Preemptible threads** can be interrupted by any higher-priority thread that
  becomes ready.

---

## Creating a Thread

Zephyr offers two ways to create threads: statically (at compile time) and
dynamically (at runtime). The `threads_test` project uses the **static**
approach via `K_THREAD_DEFINE`.

### Static Thread Definition (K_THREAD_DEFINE)

The `K_THREAD_DEFINE` macro is the simplest and most common way to create a
thread. It allocates the stack and thread control block at compile time.

```c
#define STACKSIZE  500    // Stack size in bytes
#define PRIORITY   5       // Preemptible priority (0 = highest)

void my_thread_entry(void *arg1, void *arg2, void *arg3)
{
    /* Thread body — typically a loop */
    while (1) {
        /* Do work */
        k_msleep(1000);    // Sleep for 1000 ms (yields to other threads)
    }
}

/* Statically define and start the thread */
K_THREAD_DEFINE(my_tid, STACKSIZE, my_thread_entry, NULL, NULL, NULL,
                PRIORITY, 0, 0);
```

#### Macro Parameters

```c
K_THREAD_DEFINE(name, stack_size, entry, p1, p2, p3, prio, options, delay)
```

| Parameter | Description |
|-----------|-------------|
| `name` | Symbolic name (also used as the thread ID variable) |
| `stack_size` | Stack size in bytes |
| `entry` | Entry function (must match `k_thread_entry_t` signature) |
| `p1, p2, p3` | Three `void *` arguments passed to the entry function |
| `prio` | Thread priority (`K_PRIO_COOP(x)` or `K_PRIO_PREEMPT(x)`) |
| `options` | Thread options (e.g., `K_ESSENTIAL`, `K_FP_REGS`); use `0` for defaults |
| `delay` | Startup delay in milliseconds (`0` = start immediately) |

#### Thread ID Reference

After `K_THREAD_DEFINE(blink_id, ...)`, a `const k_tid_t blink_id` variable is
created. You can use this ID with the management APIs below:

```c
extern const k_tid_t blink_id;   /* auto-declared by the macro */
```

### Dynamic Thread Creation (k_thread_create)

For threads created at runtime, use `k_thread_create()`:

```c
#include <zephyr/kernel.h>

#define STACKSIZE  1024

/* Allocate a stack in memory */
K_THREAD_STACK_DEFINE(my_stack, STACKSIZE);

/* Thread control block */
struct k_thread my_tcb;

void my_thread_entry(void *a, void *b, void *c)
{
    while (1) {
        k_msleep(500);
    }
}

void create_thread(void)
{
    k_tid_t tid = k_thread_create(&my_tcb, my_stack,
                                  K_THREAD_STACK_SIZEOF(my_stack),
                                  my_thread_entry,
                                  NULL, NULL, NULL,
                                  5,       /* priority */
                                  0,       /* options  */
                                  K_NO_WAIT);  /* start immediately */

    /* tid can be used for suspend, resume, abort, etc. */
}
```

---

## Entry Function Signature

Every thread's entry function must match the `k_thread_entry_t` type:

```c
typedef void (*k_thread_entry_t)(void *a, void *b, void *c);
```

The three `void *` parameters are typically `NULL` (as in the example project)
but can be used to pass configuration data to the thread.

---

## Thread Sleep

The most common way to yield execution is to **sleep**:

```c
k_msleep(milliseconds);          // Sleep for N milliseconds
k_sleep(K_MSEC(milliseconds));   // Same, using k_timeout_t
k_sleep(K_SECONDS(2));           // Sleep for 2 seconds
k_sleep(K_FOREVER);             // Sleep indefinitely (until woken)
```

`k_msleep()` is used in the `threads_test` project's `blink_thread` and
`print_thread`.

---

## Thread States

A thread can be in one of several states:

| State | Description |
|-------|-------------|
| **Running** | Currently executing on a CPU |
| **Ready** | Ready to run; waiting for the scheduler to pick it |
| **Pending** | Blocked waiting on a kernel object (semaphore, mutex, etc.) |
| **Suspended** | Blocked by `k_thread_suspend()`; must be resumed externally |
| **Sleeping** | Blocked waiting for a timeout (includes `k_msleep`) |
| **Dead** | Has been aborted or exited |

You can query the current thread and its state:

```c
struct k_thread *current = k_current_get();   /* pointer to the running thread */
```

---

## Thread Management API

### Suspend and Resume

```c
k_thread_suspend(k_tid_t thread);    // Suspend a thread (externally resumed)
k_thread_resume(k_tid_t thread);     // Resume a suspended thread
```

### Abort

```c
k_thread_abort(k_tid_t thread);      // Terminate a thread immediately
```

### Priority

```c
int prio = k_thread_priority_get(k_tid_t thread);
k_thread_priority_set(k_tid_t thread, int prio);
```

### Join (wait for completion)

```c
int ret = k_thread_join(k_tid_t thread, K_FOREVER);
if (ret == 0) {
    /* Thread has exited */
}
```

---

## Example: An Improved Two-Thread Application

The following example extends the pattern from `src/main.c`. It shows how to
pass arguments between threads and use both cooperative and preemptible
priorities:

```c
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#define STACKSIZE  500
#define BLINK_PRIO K_PRIO_PREEMPT(5)
#define PRINT_PRIO K_PRIO_PREEMPT(0)   // Higher urgency

/* GPIO setup (same as the existing project) */
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

/* Entry function that accepts a parameter */
void blink_thread(void *led_ptr, void *unused2, void *unused3)
{
    const struct gpio_dt_spec *led_dev = (const struct gpio_dt_spec *)led_ptr;

    if (!gpio_is_ready_dt(led_dev)) {
        return;
    }
    gpio_pin_configure_dt(led_dev, GPIO_OUTPUT_ACTIVE);

    while (1) {
        gpio_pin_toggle_dt(led_dev);
        k_msleep(500);
    }
}

void print_thread(void *unused1, void *unused2, void *unused3)
{
    int count = 0;
    while (1) {
        printf("Hello from print thread! Board: %s, count: %d\n",
               CONFIG_BOARD_TARGET, count++);
        k_msleep(1000);
    }
}

/* Pass the LED spec as the first argument to blink_thread */
K_THREAD_DEFINE(blink_id, STACKSIZE, blink_thread, (void *)&led, NULL, NULL,
                BLINK_PRIO, 0, 0);

K_THREAD_DEFINE(print_id, STACKSIZE, print_thread, NULL, NULL, NULL,
                PRINT_PRIO, 0, 0);
```

---

## Stack Size Considerations

- The `threads_test` project uses `STACKSIZE 500`, which is sufficient for
  simple threads that call `printf` and GPIO functions.
- Threads using floating-point or deep call stacks may need 1–4 KB or more.
- Always enable `CONFIG_DEBUG_STACK_OVERFLOW=y` during development to catch
  stack overflows early.
- You can inspect runtime stack usage:

```c
#include <zephyr/sys/printk.h>

void my_thread(void *a, void *b, void *c)
{
    while (1) {
        printk("Stack free: %u\n", k_thread_get_stackspace(k_current_get()));
        k_msleep(1000);
    }
}
```

---

## Scheduling Overview

```
High Priority ──────────────────────────────
  K_PRIO_COOP(0)   ← Highest cooperative
  ...
  K_PRIO_COOP(15)
  K_PRIO_PREEMPT(0) ← Highest preemptible
  ...
  K_PRIO_PREEMPT(14)
  K_IDLE_PRIO        ← Idle thread (lowest)
Low Priority ───────────────────────────────
```

Key scheduling rules:

1. The scheduler always picks the **highest-priority** ready thread.
2. **Cooperative threads** are never preempted except by an interrupt.
3. **Preemptible threads** are preempted by any higher-priority thread that
   becomes ready.
4. Equal-priority threads round-robin (if `CONFIG_TIMESLICING` is enabled).

---

## Thread Options

The `options` parameter in `K_THREAD_DEFINE` supports platform-specific flags:

| Option | Description |
|--------|-------------|
| `K_ESSENTIAL` | Mark thread as essential (system crash if it exits abnormally) |
| `K_FP_REGS` | Preserve floating-point registers across context switches |
| `K_SSE_REGS` | Preserve SSE registers (x86 only) |
| `0` | Default options (most common) |

---

## Further Reading

- [Zephyr Threads Documentation](https://docs.zephyrproject.org/latest/kernel/services/threads/index.html)
- [IPC Guide](./ipc.md) — how threads communicate with each other
- [Getting Started Guide](./getting-started.md)
