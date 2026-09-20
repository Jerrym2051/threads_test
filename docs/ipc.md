# Inter-Process Communication (IPC) in Zephyr

This guide covers the kernel synchronization and data-passing primitives used
for **inter-thread communication** (IPC) in Zephyr. These objects allow threads
to safely coordinate and exchange data.

Zephyr IPC primitives fall into two broad categories:

| Category | Primitives | Use Case |
|----------|-----------|----------|
| **Synchronization** | Semaphores, Mutexes | Signal events, protect shared resources |
| **Data Passing** | FIFO, LIFO, Message Queues, Pipes, Memory Slabs | Exchange data between threads / ISRs |

---

## Prerequisites (Kconfig)

Most IPC primitives rely on multi-threading being enabled. Add to your
`prj.conf`:

```kconfig
CONFIG_MULTITHREADING=y   # Required for all IPC primitives
```

For debugging and stack tracking:

```kconfig
CONFIG_DEBUG=y
CONFIG_THREAD_MONITOR=y
CONFIG_THREAD_STACK_INFO=y
```

---

## 1. Semaphores

A **semaphore** is an integer counter protected by the kernel. Threads can
increment (`give`) and decrement (`take`) it atomically. It is ideal for
signaling and resource counting.

### Definition (compile-time)

```c
#include <zephyr/kernel.h>

/* Define a semaphore with initial count 0 and max count 1 (binary semaphore) */
K_SEM_DEFINE(my_sem, 0, 1);

/* Define a counting semaphore (initial 3, max 3) */
K_SEM_DEFINE(resource_sem, 3, 3);
```

### Runtime Initialization

```c
struct k_sem my_sem;

void init(void)
{
    k_sem_init(&my_sem, 0, 1);   /* initial_count=0, limit=1 */
}
```

### Taking (acquiring)

```c
/* Block indefinitely until the semaphore is available */
int ret = k_sem_take(&my_sem, K_FOREVER);

/* Block for up to 1000 ms */
ret = k_sem_take(&my_sem, K_MSEC(1000));

/* Non-blocking — return immediately if unavailable */
ret = k_sem_take(&my_sem, K_NO_WAIT);
if (ret == -EBUSY) {
    /* Semaphore was not available */
}
```

### Giving (releasing)

```c
k_sem_give(&my_sem);   /* Increment the count by 1 */
```

> **Important:** `k_sem_give()` can be called from **both thread and interrupt
> contexts**. This makes semaphores ideal for ISR-to-thread signaling.

### Other Operations

```c
unsigned int count = k_sem_count(&my_sem);  /* Read current count */
k_sem_reset(&my_sem);                        /* Reset count to 0 */
```

### Example: ISR-to-Thread Signaling

```c
K_SEM_DEFINE(data_ready_sem, 0, 1);

/* Called from an ISR when new data arrives */
void data_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    /* Signal the thread that data is ready */
    k_sem_give(&data_ready_sem);
}

/* Consumer thread */
void consumer_thread(void *a, void *b, void *c)
{
    while (1) {
        /* Wait for ISR to signal */
        k_sem_take(&data_ready_sem, K_FOREVER);

        /* Process the data */
        process_data();
    }
}
```

---

## 2. Mutexes

A **mutex** (mutual exclusion object) ensures that only one thread can access
a shared resource at a time. Unlike semaphores, mutexes support **priority
inheritance** to prevent priority inversion.

### Definition (compile-time)

```c
K_MUTEX_DEFINE(my_mutex);
```

### Runtime Initialization

```c
struct k_mutex my_mutex;

void init(void)
{
    k_mutex_init(&my_mutex);
}
```

### Locking and Unlocking

```c
/* Lock with infinite timeout */
int ret = k_mutex_lock(&my_mutex, K_FOREVER);

/* Lock with a timeout of 500 ms */
ret = k_mutex_lock(&my_mutex, K_MSEC(500));
if (ret == -EAGAIN) {
    /* Timeout — mutex was not acquired */
}

/* Always unlock after you are done */
k_mutex_unlock(&my_mutex);
```

### Priority Inheritance

When a higher-priority thread tries to lock a mutex held by a lower-priority
thread, the kernel temporarily **boosts the lower-priority thread's priority**
to the higher thread's level. This prevents priority inversion and is
enabled by default.

### Example: Protecting a Shared Counter

```c
K_MUTEX_DEFINE(counter_mutex);
static int shared_counter = 0;

void thread_a(void *a, void *b, void *c)
{
    while (1) {
        k_mutex_lock(&counter_mutex, K_FOREVER);
        shared_counter++;
        k_mutex_unlock(&counter_mutex);
        k_msleep(100);
    }
}

void thread_b(void *a, void *b, void *c)
{
    while (1) {
        k_mutex_lock(&counter_mutex, K_FOREVER);
        printf("Counter: %d\n", shared_counter);
        k_mutex_unlock(&counter_mutex);
        k_msleep(500);
    }
}

K_THREAD_DEFINE(thread_a_id, 500, thread_a, NULL, NULL, NULL, 5, 0, 0);
K_THREAD_DEFINE(thread_b_id, 500, thread_b, NULL, NULL, NULL, 5, 0, 0);
```

---

## 3. FIFO (First-In, First-Out Queue)

A **FIFO** queue passes data between threads in first-in-first-out order. It
stores pointers to data structures, not the data itself.

### Definition (compile-time)

```c
K_FIFO_DEFINE(my_fifo);
```

### Putting Data

```c
/* Data must be a pointer to a structure (not a literal value) */
struct msg_data {
    void *fifo_hdr;       /* Required: reserved for FIFO internal use */
    int value;
    char text[32];
};

struct msg_data msg;
msg.value = 42;
strcpy(msg.text, "hello");

/* Send to the FIFO */
k_fifo_put(&my_fifo, &msg);    /* Can be called from thread or ISR context */
```

> **Note:** The first member of your data structure must be `void *fifo_hdr`
> (or `struct _thread_monitor *` / reserved bytes). This is used internally by
> the FIFO.

### Getting Data

```c
/* Block indefinitely until data is available */
struct msg_data *received = k_fifo_get(&my_fifo, K_FOREVER);

/* Non-blocking */
received = k_fifo_get(&my_fifo, K_NO_WAIT);
if (received == NULL) {
    /* No data available */
}
```

### Example: Producer / Consumer

```c
K_FIFO_DEFINE(msg_fifo);

struct msg {
    void *fifo_hdr;
    int id;
    int data;
};

void producer_thread(void *a, void *b, void *c)
{
    static int counter = 0;
    while (1) {
        struct msg *m = k_malloc(sizeof(struct msg));
        m->id = counter;
        m->data = counter * 10;
        k_fifo_put(&msg_fifo, m);
        printf("Produced msg %d\n", counter++);
        k_msleep(500);
    }
}

void consumer_thread(void *a, void *b, void *c)
{
    while (1) {
        struct msg *m = k_fifo_get(&msg_fifo, K_FOREVER);
        printf("Consumed msg %d, data=%d\n", m->id, m->data);
        k_free(m);
        k_msleep(100);
    }
}

K_THREAD_DEFINE(producer_id, 1024, producer_thread, NULL, NULL, NULL, 5, 0, 0);
K_THREAD_DEFINE(consumer_id, 1024, consumer_thread, NULL, NULL, NULL, 5, 0, 0);
```

> **Memory note:** When using `k_malloc()` in the producer, the consumer must
> call `k_free()` to release the memory. Alternatively, use a fixed pool of
> pre-allocated message buffers.

---

## 4. Message Queues

A **message queue** stores fixed-size messages in a circular buffer. Unlike
FIFO, it copies the message data (no dynamic allocation needed).

### Definition (compile-time)

```c
/* q_name, message_size, max_messages, alignment */
K_MSGQ_DEFINE(my_msgq, sizeof(struct msg), 10, 4);
```

The macro allocates the buffer automatically. For more control:

```c
K_MSGQ_DEFINE_STATIC(my_msgq, sizeof(struct msg), 10, 4);
```

### Runtime Initialization

```c
struct k_msgq my_msgq;
char msgq_buffer[10 * sizeof(struct msg)];

void init(void)
{
    k_msgq_init(&my_msgq, msgq_buffer, sizeof(struct msg), 10);
}
```

### Putting and Getting

```c
struct msg {
    int id;
    int value;
};

/* Send a message (copies data into the queue) */
struct msg tx_msg = { .id = 1, .value = 100 };
int ret = k_msgq_put(&my_msgq, &tx_msg, K_MSEC(100));
if (ret == -EAGAIN) {
    /* Queue is full */
}

/* Receive a message (copies data out of the queue) */
struct msg rx_msg;
ret = k_msgq_get(&my_msgq, &rx_msg, K_FOREVER);
if (ret == 0) {
    printf("Got: id=%d, value=%d\n", rx_msg.id, rx_msg.value);
}
```

### Peeking

```c
/* Look at the next message without removing it */
struct msg peek_msg;
int ret = k_msgq_peek(&my_msgq, &peek_msg);
```

### Example: Message Queue with Typed Macro

Zephyr provides `K_MSGQ_DEFINE_TYPE` for type-safe queues:

```c
struct sensor_data {
    uint32_t timestamp;
    int32_t temperature;
    int32_t humidity;
};

/* Type-safe message queue for sensor_data */
K_MSGQ_DEFINE_TYPE(sensor_q, struct sensor_data, 5, 4);

void sensor_thread(void *a, void *b, void *c)
{
    struct sensor_data reading = {
        .timestamp = k_uptime_get_32(),
        .temperature = 25,
        .humidity = 50,
    };
    k_msgq_put(&sensor_q, &reading, K_NO_WAIT);
}

void display_thread(void *a, void *b, void *c)
{
    struct sensor_data reading;
    while (1) {
        k_msgq_get(&sensor_q, &reading, K_FOREVER);
        printf("Temp: %d C, Humidity: %d%%\n",
               reading.temperature, reading.humidity);
    }
}
```

---

## 5. LIFO (Last-In, First-Out Stack)

A **LIFO** is a stack-like queue: the last item put is the first one retrieved.

### Definition and Usage

```c
K_LIFO_DEFINE(my_lifo);

struct lifo_data {
    void *lifo_hdr;    /* Required header */
    int value;
};

/* Push */
struct lifo_data item;
item.value = 42;
k_lifo_put(&my_lifo, &item);

/* Pop (from thread context only — not safe from ISRs) */
struct lifo_data *got = k_lifo_get(&my_lifo, K_MSEC(100));
```

> **LIFO** is less commonly used than FIFO. Use `K_LIFO_DEFINE` only when
> stack-like ordering is explicitly required.

---

## 6. Pipes

A **pipe** transfers chunks of byte data between threads. Unlike FIFO/LIFO
(which pass structured messages), pipes handle raw byte streams.

### Definition

```c
/* pipe_buffer_size in bytes, pipe_align for alignment */
K_PIPE_DEFINE(my_pipe, 128, 4);
```

### Writing and Reading

```c
/* Write data to the pipe */
const char *msg = "Hello, pipe!";
size_t written;
int ret = k_pipe_write(&my_pipe, msg, strlen(msg), &written,
                       0,         /* min_xfer: write all or nothing */
                       K_MSEC(100));

/* Read data from the pipe */
char buffer[64];
size_t read_bytes;
ret = k_pipe_read(&my_pipe, buffer, sizeof(buffer), &read_bytes,
                   0,             /* min_xfer */
                   K_FOREVER);
```

---

## 7. Memory Slabs

A **memory slab** pre-allocates a pool of fixed-size memory blocks. Threads
can allocate and free blocks without dynamic memory management.

### Definition

```c
/* block_size, num_blocks, alignment */
K_MEM_SLAB_DEFINE(my_slab, 64, 16, 4);
```

### Allocate and Free

```c
void *block;
int ret = k_mem_slab_alloc(&my_slab, &block, K_NO_WAIT);
if (ret == 0) {
    /* Use block — it is 64 bytes */
    memset(block, 0, 64);
    /* ... */
    k_mem_slab_free(&my_slab, block);
}
```

> **Tip:** Memory slabs are useful inside ISRs where `k_malloc()` /
> `k_free()` may not be suitable.

---

## Choosing the Right IPC Primitive

| Scenario | Recommended Primitive |
|----------|----------------------|
| Signal an event (1-bit) | Semaphore (binary, initial=0) |
| Control access to a shared resource | Mutex |
| Pass structured messages (pointers) | FIFO |
| Pass fixed-size messages (by value) | Message Queue |
| Stack-like ordering (LIFO) | LIFO |
| Stream of bytes | Pipe |
| Fixed-size memory allocation | Memory Slab |
| Count available resources | Counting Semaphore |

---

## Quick Start Template

Here is a minimal `main.c` skeleton that combines threads and IPC:

```c
#include <stdio.h>
#include <zephyr/kernel.h>

#define STACKSIZE  1024
#define PRIORITY   5

/* IPC objects */
K_SEM_DEFINE(data_ready, 0, 1);
K_MUTEX_DEFINE(resource_lock);
K_FIFO_DEFINE(msg_fifo);

struct msg {
    void *fifo_hdr;
    int value;
};

void producer_thread(void *a, void *b, void *c)
{
    while (1) {
        struct msg *m = k_malloc(sizeof(struct msg));
        m->value = k_uptime_get_32();
        k_fifo_put(&msg_fifo, m);
        k_sem_give(&data_ready);   /* Signal consumer */
        k_msleep(500);
    }
}

void consumer_thread(void *a, void *b, void *c)
{
    while (1) {
        k_sem_take(&data_ready, K_FOREVER);
        struct msg *m = k_fifo_get(&msg_fifo, K_NO_WAIT);
        if (m) {
            printf("Got value: %d\n", m->value);
            k_free(m);
        }
    }
}

K_THREAD_DEFINE(producer_id, STACKSIZE, producer_thread, NULL, NULL, NULL,
                PRIORITY, 0, 0);
K_THREAD_DEFINE(consumer_id, STACKSIZE, consumer_thread, NULL, NULL, NULL,
                PRIORITY, 0, 0);
```

Add to `prj.conf`:

```kconfig
CONFIG_MULTITHREADING=y
CONFIG_PRINTK=y
CONFIG_CONSOLE=y
CONFIG_SERIAL=y
CONFIG_HEAP_MEM_POOL_SIZE=4096   # Required for k_malloc / k_free
```

---

## Further Reading

- [Zephyr Kernel Services — Threads](https://docs.zephyrproject.org/latest/kernel/services/threads/index.html)
- [Zephyr Kernel Services — IPC](https://docs.zephyrproject.org/latest/kernel/services/ipc/index.html)
- [Getting Started Guide](./getting-started.md)
- [Threading Guide](./threading.md)
