#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ds/streambuffer.h>
#include <os/futex.h>

#define CAPACITY 4056
#define WRITERS 4
#define PACKETS 5000

static streambuffer_rw_options_t g_options = {.flags = STREAMBUFFER_NO_BLOCK};
static streambuffer_t* g_stream;
static atomic_uint g_seen[WRITERS * PACKETS];

void* dsalloc(size_t size) { return malloc(size); }
void dsfree(void* pointer) { free(pointer); }
void dswait(OSFutexParameters_t* parameters, OSAsyncContext_t* context)
{
    (void)parameters; (void)context;
    assert(!"Tests must never enter blocking stream helpers");
}
void dswake(OSFutexParameters_t* parameters) { (void)parameters; }

static void
__Reset(unsigned int index)
{
    streambuffer_construct(g_stream, CAPACITY,
        STREAMBUFFER_MULTIPLE_READERS | STREAMBUFFER_MULTIPLE_WRITERS);
    atomic_store(&g_stream->producer_index, index);
    atomic_store(&g_stream->producer_comitted_index, index);
    atomic_store(&g_stream->consumer_index, index);
    atomic_store(&g_stream->consumer_comitted_index, index);
}

static void
__CheckRollover(void)
{
    unsigned char input[CAPACITY];
    unsigned char output[CAPACITY];
    streambuffer_packet_ctx_t packet;
    unsigned int limit = (UINT_MAX / CAPACITY) * CAPACITY;
    size_t available;
    size_t index;
    size_t length = CAPACITY - sizeof(size_t);

    for (index = 0; index < sizeof(input); index++) {
        input[index] = (unsigned char)(index * 37);
    }
    // Split the size prefix itself across rollover, then fill every ring byte.
    __Reset(limit - 3);
    assert(streambuffer_write_packet_start(g_stream, length, &g_options, &packet) == length);
    streambuffer_write_packet_data(input, 19, &packet);
    streambuffer_write_packet_data(input + 19, length - 19, &packet);
    streambuffer_write_packet_end(&packet);
    streambuffer_get_bytes_available_out(g_stream, &available);
    assert(available == 0);
    assert(streambuffer_write_packet_start(g_stream, 1, &g_options, &packet) == 0);
    assert(streambuffer_read_packet_start(g_stream, &g_options, &packet) == length);
    streambuffer_read_packet_data(output, 31, &packet);
    streambuffer_read_packet_data(output + 31, length - 31, &packet);
    streambuffer_read_packet_end(&packet);
    assert(!memcmp(input, output, length));
    streambuffer_get_bytes_available_in(g_stream, &available);
    assert(available == 0);
    streambuffer_get_bytes_available_out(g_stream, &available);
    assert(available == CAPACITY);

    __Reset(limit - 11);
    assert(streambuffer_stream_out(g_stream, input, CAPACITY, &g_options) == CAPACITY);
    assert(streambuffer_stream_in(g_stream, output, CAPACITY, &g_options) == CAPACITY);
    assert(!memcmp(input, output, CAPACITY));

    // Overwrite truncates old bytes; both its reservation and commit roll over.
    __Reset(limit - 3);
    streambuffer_set_option(g_stream, STREAMBUFFER_OVERWRITE_ENABLED);
    assert(streambuffer_stream_out(g_stream, input, CAPACITY, &g_options) == CAPACITY);
    assert(streambuffer_stream_out(g_stream, input, 17, &g_options) == 17);
    assert(streambuffer_stream_in(g_stream, output, CAPACITY, &g_options) == CAPACITY);
    assert(!memcmp(input + 17, output, CAPACITY - 17));
    assert(!memcmp(input, output + CAPACITY - 17, 17));
}

static void*
__WritePackets(void* argument)
{
    unsigned int writer = (unsigned int)(uintptr_t)argument;
    unsigned int index;
    unsigned int payload[9];
    streambuffer_packet_ctx_t packet;

    for (index = 0; index < PACKETS; index++) {
        payload[0] = writer * PACKETS + index;
        for (size_t word = 1; word < 9; word++) {
            payload[word] = payload[0] ^ (unsigned int)(word * 0x1234567);
        }
        while (!streambuffer_write_packet_start(g_stream, sizeof(payload), &g_options, &packet)) {
            sched_yield();
        }
        streambuffer_write_packet_data(payload, sizeof(payload), &packet);
        streambuffer_write_packet_end(&packet);
    }
    return NULL;
}

static void*
__ReadPackets(void* argument)
{
    unsigned int index;
    unsigned int payload[9];
    streambuffer_packet_ctx_t packet;
    size_t length;
    (void)argument;

    for (index = 0; index < PACKETS; index++) {
        do {
            length = streambuffer_read_packet_start(g_stream, &g_options, &packet);
            if (length == 0) {
                sched_yield();
            }
        } while (length == 0);
        assert(length == sizeof(payload));
        streambuffer_read_packet_data(payload, sizeof(payload), &packet);
        streambuffer_read_packet_end(&packet);
        assert(payload[0] < WRITERS * PACKETS);
        for (size_t word = 1; word < 9; word++) {
            assert(payload[word] == (payload[0] ^ (unsigned int)(word * 0x1234567)));
        }
        assert(atomic_fetch_add(&g_seen[payload[0]], 1) == 0);
    }
    return NULL;
}

int
main(void)
{
    pthread_t writers[WRITERS];
    pthread_t readers[WRITERS];
    streambuffer_t invalid;
    size_t available;
    unsigned int index;

    assert(streambuffer_create(0, 0, &g_stream) == OS_EINVALPARAMS);
    assert(streambuffer_create((size_t)UINT_MAX / 2 + 1, 0, &g_stream) == OS_EINVALPARAMS);
    streambuffer_construct(&invalid, SIZE_MAX, 0);
    streambuffer_get_bytes_available_out(&invalid, &available);
    assert(available == 0 && (invalid.options & STREAMBUFFER_DISABLED));
    streambuffer_construct(&invalid, UINT_MAX / 2, 0);
    atomic_store(&invalid.consumer_index, UINT_MAX - 3);
    atomic_store(&invalid.producer_comitted_index, 1);
    streambuffer_get_bytes_available_in(&invalid, &available);
    assert(available == 3);
    streambuffer_get_bytes_available_out(&invalid, &available);
    assert(available == UINT_MAX / 2);
    assert(streambuffer_create(CAPACITY, 0, &g_stream) == OS_EOK);
    __CheckRollover();
    __Reset((UINT_MAX / CAPACITY) * CAPACITY - 71);
    for (index = 0; index < WRITERS; index++) {
        assert(pthread_create(&writers[index], NULL, __WritePackets, (void*)(uintptr_t)index) == 0);
        assert(pthread_create(&readers[index], NULL, __ReadPackets, NULL) == 0);
    }
    for (index = 0; index < WRITERS; index++) {
        assert(pthread_join(writers[index], NULL) == 0);
        assert(pthread_join(readers[index], NULL) == 0);
    }
    for (index = 0; index < WRITERS * PACKETS; index++) {
        assert(atomic_load(&g_seen[index]) == 1);
    }
    streambuffer_get_bytes_available_in(g_stream, &available);
    assert(available == 0);
    free(g_stream);
    puts("Streambuffers: rollover, framing, full/empty, truncation and 20000 concurrent packets passed");
    return 0;
}
