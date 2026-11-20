#include "T-Threads.h"
#include "main.h"
#include <stdio.h>

TX_THREAD heartbeat_thread;
_Alignas(8) static ULONG heartbeat_stack[512];
volatile ULONG diagnostic_led_transitions;
volatile ULONG diagnostic_led_levels_seen;

UINT create_heartbeat_thread(TX_BYTE_POOL *pool)
{
    TX_PARAMETER_NOT_USED(pool);
    return tx_thread_create(&heartbeat_thread, "LED heartbeat", heartbeat_thread_entry, 0,
                            heartbeat_stack, sizeof(heartbeat_stack), 25, 25,
                            TX_NO_TIME_SLICE, TX_AUTO_START);
}

VOID heartbeat_thread_entry(ULONG input)
{
    TX_PARAMETER_NOT_USED(input);
    for (;;) {
        HAL_GPIO_TogglePin(led_GPIO_Port, led_Pin);
        ++diagnostic_led_transitions;
        diagnostic_led_levels_seen |= 1UL <<
            (HAL_GPIO_ReadPin(led_GPIO_Port, led_Pin) == GPIO_PIN_SET);
        // printf("Heartbeat: %lu\n", (unsigned long)diagnostic_led_transitions);
        // fflush(stdout);
        tx_thread_sleep(TX_TIMER_TICKS_PER_SECOND / 2);
    }
}
