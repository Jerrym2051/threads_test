/*
 * threads_test - CAN loopback producer-consumer demo
 *
 * Board: STM32 Nucleo-F303K8 (STM32F303K8, bxCAN CAN1 on PA11/PA12)
 *
 * Architecture (unchanged from the original job-processing demo):
 *
 *   producer thread          dispatcher thread        worker thread
 *   (IFACE_PRIO)             (JOB_PRIO)               (WORKER_PRIO)
 *        |                        |                        |
 *   build CAN frame         take job from job_q       take job from
 *   encoding a compute      light job -> compute      worker_q
 *   job, can_send()         inline (XOR + LED)        heavy job ->
 *        |                        |                    CRC32 / Fibonacci
 *   bxCAN in LOOPBACK mode       |                        |
 *   (CAN_MODE_LOOPBACK):    k_msgq (job_q) <--- RX filter callback
 *   TX is routed back to         decodes frame into job
 *   the receiver, no bus,
 *   no transceiver needed
 *
 * The CAN controller runs in loopback mode (LBKM), so every transmitted
 * frame is fed back into the receiver filters. Receiving a frame is what
 * turns it into a unit of work: the RX callback decodes the payload
 * (byte 0 = computation type, byte 1 = sequence number, bytes 2..5 =
 * operands) and enqueues a job. The dispatcher executes light jobs itself
 * and hands heavy jobs to the worker. This keeps the "producer generates
 * work, consumer executes work" model meaningful over CAN.
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/can.h>
#include <zephyr/sys/crc.h>

#define STACKSIZE   1024
#define IFACE_PRIO  4   /* producer: highest of ours, must never stall */
#define JOB_PRIO    5   /* dispatcher                                  */
#define WORKER_PRIO 6   /* heavy lifting: lowest, gets what's left     */

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

/*
 * CAN controller from <zephyr,canbus> chosen node (see app.overlay:
 * can1 = bxCAN at 0x40006400, pins PA11/PA12 AF9, 500 kbit/s).
 */
static const struct device *const can_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

/* All job frames are sent with this ID; the RX filter matches it exactly. */
#define JOB_CAN_ID       0x100

/* ---- Job processing demo: shared message vocabulary ---- */

enum job_type {
	JOB_CRC32,     /* heavy: worker computes CRC32 over operands   */
	JOB_FIBONACCI, /* heavy: worker computes Fibonacci(operand)    */
	JOB_XOR,       /* light: dispatcher handles it inline + LED    */
};

/*
 * CAN payload layout (classic CAN frame, dlc = JOB_PAYLOAD_LEN):
 *   byte 0:   job type (one of enum job_type)
 *   byte 1:   sequence number (8-bit, wraps at 256)
 *   bytes 2..5: operands (meaning depends on job type)
 */
#define JOB_PAYLOAD_LEN  6

struct job_msg {
	uint8_t  type;     /* one of enum job_type                 */
	uint16_t seq;      /* sequence number: makes drops visible */
	uint8_t  data[4];  /* operands                             */
};

/* Bounded buffers: depth is the latency budget. */
K_MSGQ_DEFINE(job_q,    sizeof(struct job_msg), 8, 4);
K_MSGQ_DEFINE(worker_q, sizeof(struct job_msg), 4, 4);

/* Frame received -> job conversion happens in the driver RX callback, which
 * runs in interrupt context. It must stay minimal: decode, enqueue, count.
 * All printing is done by the consumer threads. */
static volatile uint32_t rx_drops;

static void can_rx_callback(const struct device *dev, struct can_frame *frame,
			    void *user_data)
{
	struct job_msg msg;

	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	if (frame->dlc < JOB_PAYLOAD_LEN || frame->data[0] > JOB_XOR) {
		rx_drops++;
		return;
	}

	msg.type = frame->data[0];
	msg.seq  = frame->data[1];
	memcpy(msg.data, &frame->data[2], sizeof(msg.data));

	if (k_msgq_put(&job_q, &msg, K_NO_WAIT) != 0) {
		rx_drops++;   /* queue full: consumer cannot keep up */
	}
}

static void can_tx_callback(const struct device *dev, int status,
			    void *user_data)
{
	uint16_t seq = (uint16_t)(uintptr_t)user_data;

	ARG_UNUSED(dev);

	if (status != 0) {
		printf("[TX] seq %u failed (%d)\n", seq, status);
	}
}

/* Configure the controller for loopback and register the RX filter.
 * Called once by the producer thread before its send loop. */
static int can_loopback_start(void)
{
	const struct can_filter rx_filter = {
		.id    = JOB_CAN_ID,
		.mask  = CAN_STD_ID_MASK,   /* exact match on standard ID */
		.flags = 0,                 /* standard (11-bit) ID frames */
	};
	int err;

	if (!device_is_ready(can_dev)) {
		printf("[IFACE] CAN device not ready\n");
		return -ENODEV;
	}

	/* CAN_MODE_LOOPBACK makes the bxCAN driver set the LBKM bit:
	 * transmitted frames are routed back to the receiver internally.
	 * No external transceiver or bus is involved. */
	err = can_set_mode(can_dev, CAN_MODE_LOOPBACK);
	if (err != 0) {
		printf("[IFACE] can_set_mode failed (%d)\n", err);
		return err;
	}

	/* Start with bit timing from devicetree (bitrate = <500000>). */
	err = can_start(can_dev);
	if (err != 0) {
		printf("[IFACE] can_start failed (%d)\n", err);
		return err;
	}

	err = can_add_rx_filter(can_dev, can_rx_callback, NULL, &rx_filter);
	if (err < 0) {
		printf("[IFACE] can_add_rx_filter failed (%d)\n", err);
		return err;
	}

	printf("[IFACE] CAN loopback up: id 0x%03x @ 500 kbit/s, filter %d\n",
	       JOB_CAN_ID, err);
	return 0;
}

void interface_thread(void *a, void *b, void *c)
{
	uint16_t seq = 0;
	uint32_t last_drops = 0;

	if (can_loopback_start() != 0) {
		return;   /* producer has nothing to do without CAN */
	}

	while (1) {
		struct can_frame frame = {
			.id    = JOB_CAN_ID,
			.flags = 0,               /* standard data frame */
			.dlc   = JOB_PAYLOAD_LEN,
		};

		frame.data[1] = seq & 0xFF;

		if (seq % 5 == 0) {            /* every 5th job is heavy */
			frame.data[0] = JOB_FIBONACCI;
			frame.data[2] = 20 + (seq % 20);  /* n in 20..39 */
		} else if (seq % 5 == 2) {     /* every 5th other job    */
			frame.data[0] = JOB_CRC32;
			frame.data[2] = (uint8_t)seq;
			frame.data[3] = (uint8_t)(seq * 7U);
			frame.data[4] = (uint8_t)(seq * 13U);
			frame.data[5] = (uint8_t)(seq * 31U);
		} else {
			frame.data[0] = JOB_XOR;
			frame.data[2] = (uint8_t)seq;
			frame.data[3] = (uint8_t)(seq >> 8);
			frame.data[4] = 0xA5;
			frame.data[5] = 0x5A;
		}

		int err = can_send(can_dev, &frame, K_MSEC(200),
				   can_tx_callback, (void *)(uintptr_t)seq);
		if (err != 0) {
			printf("[IFACE] can_send failed (%d), seq %u dropped\n",
			       err, seq);
		}

		if (rx_drops != last_drops) {
			last_drops = rx_drops;
			printf("[IFACE] rx drops so far: %u\n", last_drops);
		}

		seq++;
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
		k_msgq_get(&job_q, &msg, K_FOREVER);  /* sleep until work exists */

		printf("[RX] frame received, seq %u\n", msg.seq);

		switch (msg.type) {
		case JOB_XOR: {
			uint8_t x = msg.data[0] ^ msg.data[1] ^
				    msg.data[2] ^ msg.data[3];
			gpio_pin_toggle_dt(&led);
			printf("[JOB] seq %u xor = 0x%02x (led toggled)\n",
			       msg.seq, x);
			break;
		}
		case JOB_CRC32:
		case JOB_FIBONACCI:
			if (k_msgq_put(&worker_q, &msg, K_MSEC(10)) != 0) {
				printf("[JOB] worker busy, dropped seq %u\n",
				       msg.seq);
			} else {
				printf("[JOB] seq %u -> worker\n", msg.seq);
			}
			break;
		}
	}
}

/* Iterative Fibonacci, u32 (n capped at 47 to avoid overflow). */
static uint32_t fib_u32(uint32_t n)
{
	uint32_t a = 0, b = 1;

	for (uint32_t i = 0; i < n; i++) {
		uint32_t t = a + b;
		a = b;
		b = t;
	}
	return a;
}

void worker_thread(void *a, void *b, void *c)
{
	while (1) {
		struct job_msg msg;
		k_msgq_get(&worker_q, &msg, K_FOREVER);

		printf("[WORK] processing seq %u...\n", msg.seq);
		uint32_t t0 = k_cycle_get_32();

		if (msg.type == JOB_FIBONACCI) {
			uint32_t n = MIN((uint32_t)msg.data[0], 47U);
			printf("[WORK] seq %u fib(%u) = %u\n",
			       msg.seq, n, fib_u32(n));
		} else {
			printf("[WORK] seq %u crc32(4 operands) = 0x%08x\n",
			       msg.seq, crc32_ieee(msg.data, sizeof(msg.data)));
		}

		printf("[WORK] done seq %u in %u us\n", msg.seq,
		       k_cyc_to_us_floor32(k_cycle_get_32() - t0));
	}
}

int main(void)
{
	/* Nothing to do here: all work happens in the K_THREAD_DEFINE
	 * threads below, started automatically at POST_KERNEL. */
	return 0;
}

K_THREAD_DEFINE(iface_id,  STACKSIZE, interface_thread, NULL, NULL, NULL,
                IFACE_PRIO, 0, 0);
K_THREAD_DEFINE(job_id,    STACKSIZE, job_thread,       NULL, NULL, NULL,
                JOB_PRIO, 0, 0);
K_THREAD_DEFINE(worker_id, STACKSIZE, worker_thread,    NULL, NULL, NULL,
                WORKER_PRIO, 0, 0);
