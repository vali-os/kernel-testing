// Exercise io_buffer_flush_all with the target headers. exit() and fflush(NULL)
// flush every descriptor in the stdio table. Only descriptors opened as a
// FILE have a stream. I/O sets, events, sockets and IPC links do not, and
// must be skipped instead of dereferenced.
#ifndef FILE_STREAM_SOURCE
#define FILE_STREAM_SOURCE "../librt/libc/stdio/core/file_stream.c"
#endif
#include FILE_STREAM_SOURCE
#include "../librt/libds/hashtable.c"
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

#define MAX_FLUSHES 4

static hashtable_t    g_handles;
static stdio_handle_t g_streamHandle;
static stdio_handle_t g_cleanHandle;
static stdio_handle_t g_eventHandle;
static FILE           g_stream;
static FILE           g_cleanStream;
static FILE*          g_flushed[MAX_FLUSHES];
static unsigned int   g_flushCount;

int*
__errno(void)
{
    static int error;

    return &error;
}

void
_assert_panic(const char* message)
{
    puts(message);
    abort();
}

void*
dsalloc(size_t size)
{
    return malloc(size);
}

void
dsfree(void* memory)
{
    free(memory);
}

hashtable_t*
stdio_get_handles(void)
{
    return &g_handles;
}

int
fflush(FILE* stream)
{
    CHECK(g_flushCount < MAX_FLUSHES);
    g_flushed[g_flushCount++] = stream;
    return 0;
}

// Only the buffer helpers that are not tested here use these.
int
write(int iod, const void* buffer, unsigned int length)
{
    (void)iod;
    (void)buffer;
    (void)length;
    abort();
}

void
usched_mtx_lock(struct usched_mtx* mutex)
{
    (void)mutex;
    abort();
}

void
usched_mtx_unlock(struct usched_mtx* mutex)
{
    (void)mutex;
    abort();
}

int
usched_mtx_trylock(struct usched_mtx* mutex)
{
    (void)mutex;
    abort();
}

static uint64_t
__Hash(const void* element)
{
    return (uint64_t)((const struct stdio_object_entry*)element)->id;
}

static int
__Compare(const void* element1, const void* element2)
{
    return ((const struct stdio_object_entry*)element1)->id ==
           ((const struct stdio_object_entry*)element2)->id ? 0 : -1;
}

int
main(void)
{
    CHECK(hashtable_construct(&g_handles, 0, sizeof(struct stdio_object_entry), __Hash, __Compare) == 0);

    // A modified stream, a stream without changes, and an event descriptor
    // without a stream.
    g_stream.Flags = _IOMOD;
    g_streamHandle.Stream = &g_stream;
    g_cleanHandle.Stream = &g_cleanStream;
    g_eventHandle.Stream = NULL;
    hashtable_set(&g_handles, &(struct stdio_object_entry) { .id = 3, .handle = &g_streamHandle });
    hashtable_set(&g_handles, &(struct stdio_object_entry) { .id = 4, .handle = &g_cleanHandle });
    hashtable_set(&g_handles, &(struct stdio_object_entry) { .id = 5, .handle = &g_eventHandle });

    // fflush(NULL) flushes modified streams only.
    io_buffer_flush_all(_IOMOD);
    CHECK(g_flushCount == 1 && g_flushed[0] == &g_stream);

    // exit() flushes every stream with any flag set, and skips the event
    // descriptor. It used to read the flags of its NULL stream and crash the
    // exiting process.
    g_flushCount = 0;
    g_cleanStream.Flags = _IORD;
    io_buffer_flush_all(0xFFFFU);
    CHECK(g_flushCount == 2);
    CHECK((g_flushed[0] == &g_stream && g_flushed[1] == &g_cleanStream) ||
          (g_flushed[0] == &g_cleanStream && g_flushed[1] == &g_stream));
    hashtable_destroy(&g_handles);
    puts("io_buffer_flush_all: streams flushed, descriptors without a stream skipped passed");
    return 0;
}
