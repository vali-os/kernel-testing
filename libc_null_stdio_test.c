// Exercise the standard streams of a process that inherits no stdio, which
// includes every service. StdioConfigureStandardHandles gives such a process
// NULL_SIGNATURE descriptors for stdin, stdout and stderr. The test uses the
// real descriptor table, close(), write(), read(), lseeki64(), the stream
// functions and vfprintf with streamout, and ends with a real exit(), which
// runs the __CleanupSTDIO handler that StdioInitialize registered.
//
// Expected results for a stream that goes nowhere: writes succeed and report
// every byte, reads return end of file, seeks report position 0, and close
// succeeds. These descriptors used to have no operations table, so the first
// write through stderr and the close in the exit handler read through a NULL
// pointer and ended the process with signal 8.
//
// Two more groups of scenarios use the same table:
// - "bad": every call that takes a descriptor number gets one that is not
//   open (77). Each must fail with the errno of its neighbours. write() used
//   to read handle->Ops through the NULL that stdio_handle_get returns.
// - "exit N", "count N" and "exit-nomem N": N more descriptors of mixed kinds
//   are open when the process exits. __close_io_descriptors used to close
//   them from inside hashtable_enumerate. Each close removes its entry, and
//   once the table shrank, the enumeration wrote to freed storage.
#ifndef STDIO_CORE_SOURCE
#define STDIO_CORE_SOURCE "../librt/libc/stdio/core/core.c"
#endif

// Allocations made by core.c go through __TestMalloc, so the "exit-nomem"
// scenario can fail the allocation of the list of descriptors to close.
// stdlib.h is included first, so its declaration of malloc keeps its name.
#include <stdlib.h>
static int   g_failCoreAllocations;
static void* __TestMalloc(size_t size);
#define malloc __TestMalloc
#include STDIO_CORE_SOURCE
#undef malloc

#include <ds/mstring.h>
#include <ioctl.h>
#include <os/spinlock.h>
#include <stdarg.h>
#include <wchar.h>

// Failures are reported with the host dprintf and end the test with _Exit,
// so no exit handler runs on a failed check. The target stdio.h maps _exit to
// exit, and the target stdout is the stream under test.
#define CHECK(x) do { if (!(x)) { __Fail(__LINE__, #x); } } while (0)

extern void StdioInitialize(void);
extern int  dprintf(int iod, const char* format, ...);
extern int  vdprintf(int iod, const char* format, va_list arguments);

_Noreturn static void
__Fail(int line, const char* text)
{
    dprintf(2, "line %d: %s\n", line, text);
    _Exit(1);
}

static int g_errno;

static void*
__TestMalloc(size_t size)
{
    if (g_failCoreAllocations) {
        return NULL;
    }
    return malloc(size);
}

// Operation tables of the other descriptor types. The test creates none of
// those descriptors except FILE_SIGNATURE ones, so the others are never used.
stdio_ops_t g_fmemOps;
stdio_ops_t g_memstreamOps;
stdio_ops_t g_evtOps;
stdio_ops_t g_iosetOps;
stdio_ops_t g_pipeOps;
stdio_ops_t g_ipcOps;
stdio_ops_t g_netOps;

// FILE_SIGNATURE descriptors of the exit scenarios. They have no OS handle.
// The close operation counts the closes. A descriptor marked __IO_PRIORITY
// must be closed after every other one, which is the order __CleanupSTDIO
// asks for with its two calls to __close_io_descriptors.
static int    g_fileCloses;
static int    g_priorityCloses;
static int    g_expectedFileCloses;
static int    g_expectedPriorityCloses;
static size_t g_fileBytes;
static size_t g_expectedFileBytes;

static oserr_t
__file_write(stdio_handle_t* handle, const void* buffer, size_t length, size_t* bytesWritten)
{
    (void)handle;
    (void)buffer;
    g_fileBytes += length;
    *bytesWritten = length;
    return OS_EOK;
}

static void
__file_close(stdio_handle_t* handle, int options)
{
    (void)options;
    if (handle->XTFlags & __IO_PRIORITY) {
        CHECK(g_fileCloses == g_expectedFileCloses);
        g_priorityCloses++;
    } else {
        CHECK(g_priorityCloses == 0);
        g_fileCloses++;
    }
}

stdio_ops_t g_fileOps = {
    .write = __file_write,
    .close = __file_close
};

int*
__errno(void)
{
    return &g_errno;
}

void
_assert_panic(const char* message)
{
    __Fail(__LINE__, message);
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

// Log lines from libc. The exit scenarios count them: only a failed
// allocation of the list of descriptors to close may log an error, and only a
// failed close a warning.
static int g_errorLines;
static int g_warningLines;

void
OSSysLogWrite(enum OSSysLogLevel level, const char* format, ...)
{
    va_list arguments;

    if (level == OSSYSLOGLEVEL_ERROR) {
        g_errorLines++;
    }
    if (level == OSSYSLOGLEVEL_WARNING) {
        g_warningLines++;
    }
    va_start(arguments, format);
    vdprintf(2, format, arguments);
    va_end(arguments);
    dprintf(2, "\n");
}

// The test runs on one thread, so locks have nothing to do.
void
spinlock_acquire(spinlock_t* lock)
{
    (void)lock;
}

void
spinlock_release(spinlock_t* lock)
{
    (void)lock;
}

void
usched_mtx_init(struct usched_mtx* mutex, int type)
{
    (void)mutex;
    (void)type;
}

void
usched_mtx_lock(struct usched_mtx* mutex)
{
    (void)mutex;
}

void
usched_mtx_unlock(struct usched_mtx* mutex)
{
    (void)mutex;
}

int
usched_mtx_trylock(struct usched_mtx* mutex)
{
    (void)mutex;
    return 0;
}

// NULL_SIGNATURE descriptors carry no OS handle, so none of these may run.
void
OSHandleDestroy(OSHandle_t* handle)
{
    (void)handle;
    __Fail(__LINE__, "OSHandleDestroy called for a descriptor without an OS handle");
}

// The "stuck" scenarios open one shared descriptor whose release always
// fails, so close() keeps it open. No other descriptor is shared.
static int g_stuckIod = -1;
static int g_stuckReleases;

oserr_t
OSHandleRelease(OSHandle_t* handle)
{
    (void)handle;
    if (g_stuckIod < 0) {
        __Fail(__LINE__, "OSHandleRelease called for a descriptor that is not shared");
    }
    g_stuckReleases++;
    return OS_EBUSY;
}

oserr_t
OSHandlesAcquire(OSHandle_t* handle)
{
    (void)handle;
    __Fail(__LINE__, "OSHandlesAcquire called for a descriptor without an OS handle");
}

oserr_t
OSNotificationQueuePost(OSHandle_t* handle, unsigned int flags)
{
    (void)handle;
    (void)flags;
    __Fail(__LINE__, "OSNotificationQueuePost called");
}

// streamout links these for float and mstring formats, which the test does
// not use.
int
__isfinitef(float value)
{
    (void)value;
    __Fail(__LINE__, "unexpected float format");
}

int
__isfinite(double value)
{
    (void)value;
    __Fail(__LINE__, "unexpected float format");
}

int
__isfinitel(long double value)
{
    (void)value;
    __Fail(__LINE__, "unexpected float format");
}

int
mstr_fromchar(mchar_t character, char* u8, size_t* length)
{
    (void)character;
    (void)u8;
    (void)length;
    __Fail(__LINE__, "unexpected mstring format");
}

// Formats through the target vfprintf, which runs streamout and fputc.
static int
__Print(FILE* stream, const char* format, ...)
{
    va_list arguments;
    int     result;

    va_start(arguments, format);
    result = vfprintf(stream, format, arguments);
    va_end(arguments);
    return result;
}

// Runs after __CleanupSTDIO, because atexit handlers run in reverse order of
// registration and this one is registered before StdioInitialize. Every
// descriptor must be gone, and each FILE_SIGNATURE descriptor of the exit
// scenarios closed once, in the right order.
static void
__CheckCleanup(void)
{
    int iod;

    for (iod = 0; iod < INTERNAL_MAXFILES; iod++) {
        if (iod != g_stuckIod) {
            CHECK(stdio_handle_get(iod) == NULL);
        }
    }

    // A descriptor whose close fails stays open. Each of the two passes of
    // __CleanupSTDIO tries it once, also when it closes without a list.
    if (g_stuckIod >= 0) {
        CHECK(stdio_handle_get(g_stuckIod) != NULL);
        CHECK(g_stdioObjects.element_count == 1);
        CHECK(g_stuckReleases == 2);
        CHECK(g_warningLines == 2);
    } else {
        CHECK(g_warningLines == 0);
        CHECK(g_stdioObjects.element_count == 0);
    }
    CHECK(g_fileCloses == g_expectedFileCloses);
    CHECK(g_priorityCloses == g_expectedPriorityCloses);
    CHECK(g_fileBytes == g_expectedFileBytes);
    // Without memory, each pass that finds descriptors left logs one error.
    // The second pass finds some only when priority descriptors or a stuck
    // descriptor are left.
    if (g_failCoreAllocations) {
        CHECK(g_errorLines == 1 + (g_expectedPriorityCloses > 0 || g_stuckIod >= 0));
    } else {
        CHECK(g_errorLines == 0);
    }
    dprintf(1, "null stdio: write, read, seek, close and exit() without a fault passed\n");
}

// Calls every function that takes a descriptor number with one that is not
// open. Each must fail the way its neighbours report a bad descriptor, and
// none may touch the missing descriptor.
static void
__CheckUnknownDescriptor(void)
{
    const int unknown = 77;
    char      buffer[8] = { 0 };
    FILE      stream;

    CHECK(stdio_handle_get(unknown) == NULL);

    g_errno = 0;
    CHECK(write(unknown, "lost", 4) == -1);
    CHECK(g_errno == EBADFD);

    g_errno = 0;
    CHECK(read(unknown, buffer, sizeof(buffer)) == -1);
    CHECK(g_errno == EBADFD);

    g_errno = 0;
    CHECK(close(unknown) == -1);
    CHECK(g_errno == EBADFD);

    g_errno = 0;
    CHECK(lseek(unknown, 0, SEEK_CUR) == -1);
    CHECK(g_errno == EBADFD);

    g_errno = 0;
    CHECK(lseeki64(unknown, 0, SEEK_SET) == -1);
    CHECK(g_errno == EBADFD);

    g_errno = 0;
    CHECK(dup(unknown) == -1);
    CHECK(g_errno == EBADFD);

    g_errno = 0;
    CHECK(chsize(unknown, 0) == -1);
    CHECK(g_errno == EBADFD);

    // isatty answers 0 for a descriptor that is not open, like marktty and
    // ioctl it reports EBADF.
    g_errno = 0;
    CHECK(isatty(unknown) == 0);
    CHECK(g_errno == EBADF);

    g_errno = 0;
    CHECK(marktty(unknown) == -1);
    CHECK(g_errno == EBADF);

    g_errno = 0;
    CHECK(ioctl(unknown, FIONREAD, buffer) == -1);
    CHECK(g_errno == EBADF);

    // A stream whose descriptor is gone, for example after a close of its
    // number. fgetwc and fputwc already report EBADFD for it.
    memset(&stream, 0, sizeof(stream));
    stream.IOD = unknown;
    g_errno = 0;
    CHECK(ungetwc(L'a', &stream) == WEOF);
    CHECK(g_errno == EBADFD);

    // Nothing above may have created the descriptor.
    CHECK(stdio_handle_get(unknown) == NULL);
}

// Opens count more descriptors, of five kinds in turn: a raw NULL_SIGNATURE
// descriptor, a raw FILE_SIGNATURE descriptor, a copy of stdout, a buffered
// FILE_SIGNATURE stream with data in its buffer, and a FILE_SIGNATURE
// descriptor marked __IO_PRIORITY. Returns how many are marked __IO_PRIORITY.
static int
__OpenDescriptors(int count)
{
    stdio_handle_t* handle;
    int             priority = 0;
    int             i;

    for (i = 0; i < count; i++) {
        switch (i % 5) {
            case 0:
                CHECK(stdio_handle_create(-1, 0, 0, NULL_SIGNATURE, NULL, &handle) == 0);
                break;
            case 1:
                CHECK(stdio_handle_create(-1, 0, 0, FILE_SIGNATURE, NULL, &handle) == 0);
                g_expectedFileCloses++;
                break;
            case 2:
                CHECK(stdio_handle_clone(stdio_handle_get(STDOUT_FILENO), &handle) == 0);
                break;
            case 3:
                CHECK(stdio_handle_create(-1, O_WRONLY, 0, FILE_SIGNATURE, NULL, &handle) == 0);
                CHECK(stdio_handle_set_buffered(handle, NULL, _IOWR, _IOFBF) == 0);
                CHECK(fputs("flushed at exit", handle->Stream) >= 0);
                g_expectedFileBytes += strlen("flushed at exit");
                g_expectedFileCloses++;
                break;
            default:
                CHECK(stdio_handle_create(-1, 0, __IO_PRIORITY, FILE_SIGNATURE, NULL, &handle) == 0);
                priority++;
                break;
        }
        CHECK(handle != NULL);
    }
    g_expectedPriorityCloses = priority;
    return priority;
}

// Scenarios:
//   (none)        Every stream operation on the standard streams, then exit().
//   exit          Only exit(), like a service that fails during setup before
//                 it writes anything. That was the signal 8 seen from served.
//   bad           Calls on a descriptor that is not open, then exit().
//   exit N        N more descriptors are open at exit().
//   count N       The same, but the test calls __close_io_descriptors itself
//                 the way __CleanupSTDIO does and checks the counts.
//   exit-nomem N  The same as exit N, but the allocation of the list of
//                 descriptors to close fails.
//   exit-stuck N, exit-nomem-stuck N
//                 The same as exit N and exit-nomem N, with one more shared
//                 descriptor whose close fails and leaves it open.
int
main(int argc, char** argv)
{
    stdio_handle_t* handle;
    stdio_handle_t* copy;
    char            buffer[8];
    int             iod;
    int             count;
    int             priority;

    CHECK(atexit(__CheckCleanup) == 0);
    StdioInitialize();
    StdioConfigureStandardHandles();
    if (argc > 1 && strcmp(argv[1], "exit") == 0 && argc == 2) {
        exit(0);
    }

    if (argc > 1 && strcmp(argv[1], "bad") == 0) {
        __CheckUnknownDescriptor();
        exit(0);
    }

    if (argc == 3) {
        count = atoi(argv[2]);
        CHECK(count >= 0);
        priority = __OpenDescriptors(count);
        if (strstr(argv[1], "-stuck") != NULL) {
            CHECK(stdio_handle_create(-1, 0, 0, NULL_SIGNATURE, NULL, &handle) == 0);
            stdio_handle_set_shared(handle);
            g_stuckIod = handle->IOD;
        }

        if (strcmp(argv[1], "exit") == 0 || strcmp(argv[1], "exit-stuck") == 0) {
            exit(0);
        } else if (strcmp(argv[1], "count") == 0) {
            // The first pass closes everything but the priority descriptors,
            // which includes the three standard streams. The second pass
            // closes the rest.
            io_buffer_flush_all(0xFFFFU);
            CHECK(__close_io_descriptors(__IO_PRIORITY) == count - priority + 3);
            CHECK(g_fileCloses == g_expectedFileCloses);
            CHECK(g_priorityCloses == 0);
            CHECK(__close_io_descriptors(0) == priority);
            CHECK(__close_io_descriptors(0) == 0);
            exit(0);
        } else if (strcmp(argv[1], "exit-nomem") == 0 || strcmp(argv[1], "exit-nomem-stuck") == 0) {
            g_failCoreAllocations = 1;
            exit(0);
        }
        __Fail(__LINE__, "unknown scenario");
    }

    // Every standard descriptor goes nowhere, and still has operations.
    for (iod = STDIN_FILENO; iod <= STDERR_FILENO; iod++) {
        handle = stdio_handle_get(iod);
        CHECK(handle != NULL);
        CHECK(handle->Signature == NULL_SIGNATURE);
        CHECK(handle->Stream != NULL);
    }

    // Raw writes report every byte as written.
    CHECK(write(STDOUT_FILENO, "out", 3) == 3);
    CHECK(write(STDERR_FILENO, "err", 3) == 3);

    // stderr is unbuffered, so fputc writes straight to the descriptor. This
    // is the path of the served crash, vfprintf to streamout to fputc to write.
    CHECK(fputc('x', stderr) == 'x');
    CHECK(__Print(stderr, "%s %d\n", "error", 42) == 9);
    CHECK(ferror(stderr) == 0);

    // stdout is line buffered. The newline and fflush write the buffer out.
    CHECK(__Print(stdout, "%s\n", "line") == 5);
    CHECK(fputs("buffered", stdout) >= 0);
    CHECK(fflush(stdout) == 0);
    CHECK(ferror(stdout) == 0);

    // stdin reads end of file, both raw and through the stream.
    CHECK(read(STDIN_FILENO, buffer, sizeof(buffer)) == 0);
    CHECK(fgetc(stdin) == EOF);
    CHECK(feof(stdin) != 0);
    CHECK(ferror(stdin) == 0);

    // Seeking the empty stream reports position 0.
    CHECK(lseeki64(STDOUT_FILENO, 0, SEEK_CUR) == 0);

    // A copy gets the same operations, and closing it leaves the original.
    CHECK(stdio_handle_clone(stdio_handle_get(STDOUT_FILENO), &copy) == 0);
    CHECK(copy->Ops == stdio_handle_get(STDOUT_FILENO)->Ops);
    CHECK(write(copy->IOD, "copy", 4) == 4);
    CHECK(close(copy->IOD) == 0);
    CHECK(stdio_handle_get(STDOUT_FILENO) != NULL);

    // Leave something in the stdout buffer for the exit handler to flush.
    CHECK(fputs("left in the buffer", stdout) >= 0);

    // exit() flushes and closes every descriptor through __CleanupSTDIO.
    exit(0);
}
