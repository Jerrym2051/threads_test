#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#define STACKSIZE   1024
#define IFACE_PRIO  4   /* producer: highest of ours, must never stall */
#define JOB_PRIO    5   /* dispatcher                                  */
#define WORKER_PRIO 6   /* heavy lifting: lowest, gets what's left     */


#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

/* ---- Job processing demo: shared message vocabulary ---- */

enum job_type {
    JOB_PING,     /* light: dispatcher handles it immediately    */
    JOB_LED,      /* light: dispatcher handles it immediately    */
    JOB_PROCESS,  /* heavy: handed to the worker thread          */
};

struct job_msg {
    uint8_t  type;     /* one of enum job_type                 */
    uint16_t seq;      /* sequence number: makes drops visible */
    uint32_t payload;  /* PROCESS: simulated work time in ms   */
};

/* Bounded buffers: depth is the latency budget. */
K_MSGQ_DEFINE(job_q,    sizeof(struct job_msg), 8, 4);
K_MSGQ_DEFINE(worker_q, sizeof(struct job_msg), 4, 4);


void interface_thread(void *a, void *b, void *c)
{
    uint16_t seq = 0;

    while (1) {
        struct job_msg msg;
        msg.seq = ++seq;

        if (seq % 5 == 0) {          /* every 5th job is heavy */
            msg.type    = JOB_PROCESS;
            msg.payload = 700;       /* 700 ms of "work"       */
        } else if (seq % 5 == 2) {
            msg.type    = JOB_LED;
            msg.payload = 1;
        } else {
            msg.type    = JOB_PING;
            msg.payload = 0;
        }

        if (k_msgq_put(&job_q, &msg, K_NO_WAIT) != 0) {
            printf("[IFACE] queue full, dropped seq %u\n", msg.seq);
        }
        k_msleep(250);
    }
}

void job_thread(void *a, void *b, void *c)
{
    if (gpio_is_ready_dt(&led)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    }

    while (1) {
        struct job_msg msg;
        k_msgq_get(&job_q, &msg, K_FOREVER);   /* sleep until work exists */

        switch (msg.type) {
        case JOB_PING:
            printf("[JOB] ping seq %u handled\n", msg.seq);
            break;
        case JOB_LED:
            gpio_pin_toggle_dt(&led);
            printf("[JOB] led seq %u handled\n", msg.seq);
            break;
        case JOB_PROCESS:
            if (k_msgq_put(&worker_q, &msg, K_MSEC(10)) != 0) {
                printf("[JOB] worker busy, dropped seq %u\n", msg.seq);
            } else {
                printf("[JOB] seq %u -> worker\n", msg.seq);
            }
            break;
        }
    }
}

void worker_thread(void *a, void *b, void *c)
{
    while (1) {
        struct job_msg msg;
        k_msgq_get(&worker_q, &msg, K_FOREVER);

        printf("[WORK] processing seq %u...\n", msg.seq);
        k_msleep(msg.payload);   /* simulated heavy processing */
        printf("[WORK] done seq %u\n", msg.seq);
    }
}


K_THREAD_DEFINE(iface_id,  STACKSIZE, interface_thread, NULL, NULL, NULL,
                IFACE_PRIO, 0, 0);
K_THREAD_DEFINE(job_id,    STACKSIZE, job_thread,       NULL, NULL, NULL,
                JOB_PRIO, 0, 0);
K_THREAD_DEFINE(worker_id, STACKSIZE, worker_thread,    NULL, NULL, NULL,
                WORKER_PRIO, 0, 0);
