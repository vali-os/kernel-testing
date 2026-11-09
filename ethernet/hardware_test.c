#include <assert.h>
#include <stdio.h>
#include "hardware.c"

int
main(void)
{
    uint8_t frame[1514] = {0};
    uint32_t length;
    uint32_t sequence;
    uint32_t i;

    atomic_store(&g_device, 42);
    atomic_store(&g_ready, true);
    g_mac[0] = 2;
    memcpy(frame, g_mac, 6);
    frame[12] = 0x88;
    frame[13] = 0xb5;
    memcpy(frame + 14, "VALIGEN1", 8);
    frame[22] = 2;
    for (sequence = 1; sequence <= 2; sequence++) {
        length = sequence & 1 ? 60 : 1514;
        frame[26] = sequence;
        for (i = 27; i < length; i++) {
            frame[i] = (uint8_t)(i ^ sequence);
        }
        atomic_store(&g_expected, sequence);
        atomic_store(&g_received, 0);
        __Receive(42, 0, frame, length - 1);
        assert(atomic_load(&g_received) == 0);
        __Receive(41, 0, frame, length);
        assert(atomic_load(&g_received) == 0);
        frame[length - 1] ^= 1;
        __Receive(42, 0, frame, length);
        assert(atomic_load(&g_received) == 0);
        frame[length - 1] ^= 1;
        frame[22] = 1;
        __Receive(42, 0, frame, length);
        assert(atomic_load(&g_received) == 0);
        frame[22] = 2;
        __Receive(42, 0, frame, length);
        assert(atomic_load(&g_received) == sequence);
    }
    __Transmitted(42, 0, 2, OS_ETIMEOUT);
    assert(atomic_load(&g_completed) == 0);
    __Transmitted(42, 0, 2, OS_EOK);
    assert(atomic_load(&g_completed) == 2);
    puts("Ethernet hardware reply validation tests passed");
    return 0;
}
