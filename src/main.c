#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#define STACKSIZE 500
#define PRIORITY  5

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

void blink_thread(void *unused1, void *unused2, void *unused3)
{
    if (!gpio_is_ready_dt(&led)) {
        return;
    }
    gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);

    while (1) {
        gpio_pin_toggle_dt(&led);
        k_msleep(500);
    }
}

void print_thread(void *unused1, void *unused2, void *unused3)
{
    while (1) {
        printf("Hello from print thread! Board: %s\n", CONFIG_BOARD_TARGET);
        k_msleep(1000);
    }
}

K_THREAD_DEFINE(blink_id, STACKSIZE, blink_thread, NULL, NULL, NULL,
                PRIORITY, 0, 0);

K_THREAD_DEFINE(print_id, STACKSIZE, print_thread, NULL, NULL, NULL,
                PRIORITY, 0, 0);