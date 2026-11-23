// Exercise the descriptor checks of libc's socket and IPC calls on a POSIX
// host. The test uses the real descriptor table (core.c), the real socket
// calls under librt/libc/inet, ipsend and iprecv from librt/libc/os/ipc.c and
// the real close(). Only the OS layer below them is replaced.
//
// Each call is made twice:
// - with a descriptor number that is not open (77). It must fail with EBADF,
//   the errno POSIX gives the socket calls for a bad descriptor and the one
//   ioctl, ioset and the IPC calls already use.
// - with an open descriptor that is not a socket (a FILE_SIGNATURE one). It
//   must fail with ENOTSOCK.
// Neither may reach the OS layer.
//
// socketpair is run with each of its later steps failing: the second socket,
// and the pairing of the two. No descriptor and no OS socket may be left open
// afterwards. The old code closed descriptor -1 when the second socket failed
// and left the first one open.
#ifndef STDIO_CORE_SOURCE
#define STDIO_CORE_SOURCE "../librt/libc/stdio/core/core.c"
#endif
#include STDIO_CORE_SOURCE

#include <internal/_tls.h>
#include <inet/socket.h>
#include <ipcontext.h>
#include <os/ipc.h>
#include <os/services/net.h>
#include <os/spinlock.h>
#include <stdarg.h>

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

static int                   g_errno;
static struct thread_storage g_tls;

// OS sockets the doubles have handed out and not yet released.
static int     g_openSockets;
static int     g_socketOpens;
static int     g_failSocketOpen;
static oserr_t g_pairStatus;
static int     g_pairCalls;

// The next g_failReleases releases fail with OS_EBUSY, which makes close()
// fail and keep the descriptor open. Error lines from libc are counted.
static int     g_failReleases;
static int     g_errorLines;

// Operation tables of descriptor types the test does not use. FILE_SIGNATURE
// descriptors are the "open but not a socket" case, and are never read from.
stdio_ops_t g_fmemOps;
stdio_ops_t g_memstreamOps;
stdio_ops_t g_evtOps;
stdio_ops_t g_iosetOps;
stdio_ops_t g_pipeOps;
stdio_ops_t g_fileOps;

int*
__errno(void)
{
    return &g_errno;
}

struct thread_storage*
__tls_current(void)
{
    return &g_tls;
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

void
OSSysLogWrite(enum OSSysLogLevel level, const char* format, ...)
{
    va_list arguments;

    if (level == OSSYSLOGLEVEL_ERROR) {
        g_errorLines++;
    }
    va_start(arguments, format);
    vdprintf(2, format, arguments);
    va_end(arguments);
    dprintf(2, "\n");
}

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

void
io_buffer_flush_all(uint16_t flags)
{
    (void)flags;
}

oserr_t
io_buffer_flush(FILE* file)
{
    (void)file;
    return OS_EOK;
}

oserr_t
OSNotificationQueuePost(OSHandle_t* handle, unsigned int flags)
{
    (void)handle;
    (void)flags;
    __Fail(__LINE__, "OSNotificationQueuePost called");
}

oserr_t
OSHandlesAcquire(OSHandle_t* handle)
{
    (void)handle;
    __Fail(__LINE__, "OSHandlesAcquire called");
}

// Sockets are shared descriptors, so close() releases them here.
oserr_t
OSHandleRelease(OSHandle_t* handle)
{
    CHECK(handle->Type == OSHANDLE_SOCKET);
    CHECK(g_openSockets > 0);
    if (g_failReleases > 0) {
        g_failReleases--;
        return OS_EBUSY;
    }
    g_openSockets--;
    return OS_EOK;
}

void
OSHandleDestroy(OSHandle_t* handle)
{
    CHECK(handle->Type == OSHANDLE_SOCKET);
    CHECK(g_openSockets > 0);
    g_openSockets--;
}

// The OS socket layer. Opening and pairing can be made to fail. Every other
// call must never be reached, because each test descriptor is either not
// open or not a socket.
oserr_t
OSSocketOpen(int domain, int type, int protocol, OSHandle_t* handleOut)
{
    (void)domain;
    (void)type;
    (void)protocol;
    g_socketOpens++;
    if (g_socketOpens == g_failSocketOpen) {
        return OS_EOOM;
    }
    memset(handleOut, 0, sizeof(*handleOut));
    handleOut->ID = 100 + g_socketOpens;
    handleOut->Type = OSHANDLE_SOCKET;
    g_openSockets++;
    return OS_EOK;
}

oserr_t
OSSocketPair(OSHandle_t* sock0, OSHandle_t* sock1)
{
    CHECK(sock0->Type == OSHANDLE_SOCKET && sock1->Type == OSHANDLE_SOCKET);
    g_pairCalls++;
    return g_pairStatus;
}

#define __UNREACHED(name) __Fail(__LINE__, name " reached the OS layer")

oserr_t
OSSocketAccept(OSHandle_t* handle, struct sockaddr* address, socklen_t* length, OSHandle_t* out)
{
    (void)handle; (void)address; (void)length; (void)out;
    __UNREACHED("accept");
}

oserr_t
OSSocketBind(OSHandle_t* handle, const struct sockaddr* address, socklen_t length)
{
    (void)handle; (void)address; (void)length;
    __UNREACHED("bind");
}

oserr_t
OSSocketConnect(OSHandle_t* handle, const struct sockaddr* address, socklen_t length)
{
    (void)handle; (void)address; (void)length;
    __UNREACHED("connect");
}

oserr_t
OSSocketListen(OSHandle_t* handle, int queueSize)
{
    (void)handle; (void)queueSize;
    __UNREACHED("listen");
}

oserr_t
OSSocketAddress(OSHandle_t* handle, int type, struct sockaddr* address, socklen_t length)
{
    (void)handle; (void)type; (void)address; (void)length;
    __UNREACHED("getpeername or getsockname");
}

oserr_t
OSSocketSetOption(OSHandle_t* handle, int protocol, int option, const void* data, socklen_t length)
{
    (void)handle; (void)protocol; (void)option; (void)data; (void)length;
    __UNREACHED("setsockopt");
}

oserr_t
OSSocketOption(OSHandle_t* handle, int protocol, int option, void* data, socklen_t* length)
{
    (void)handle; (void)protocol; (void)option; (void)data; (void)length;
    __UNREACHED("getsockopt");
}

oserr_t
OSSocketShutdown(OSHandle_t* handle, enum OSocketShutdownType type)
{
    (void)handle; (void)type;
    __UNREACHED("shutdown");
}

oserr_t
OSSocketSend(OSHandle_t* handle, const struct msghdr* message, int flags, size_t* count)
{
    (void)handle; (void)message; (void)flags; (void)count;
    __UNREACHED("send");
}

oserr_t
OSSocketRecv(OSHandle_t* handle, struct msghdr* message, int flags, size_t* count)
{
    (void)handle; (void)message; (void)flags; (void)count;
    __UNREACHED("recv");
}

oserr_t
OSSocketRecvPipe(OSHandle_t* handle, streambuffer_t** pipeOut)
{
    (void)handle; (void)pipeOut;
    __UNREACHED("socket ioctl");
}

oserr_t
IPCContextSend(OSHandle_t* handle, IPCAddress_t* address, const void* data, unsigned int length,
               OSTimestamp_t* deadline, OSAsyncContext_t* asyncContext)
{
    (void)handle; (void)address; (void)data; (void)length; (void)deadline; (void)asyncContext;
    __UNREACHED("ipsend");
}

oserr_t
IPCContextRecv(OSHandle_t* handle, void* buffer, unsigned int length, int flags,
               OSAsyncContext_t* asyncContext, IPCMessageContext_t* context, size_t* count)
{
    (void)handle; (void)buffer; (void)length; (void)flags; (void)asyncContext;
    (void)context; (void)count;
    __UNREACHED("iprecv");
}

oserr_t
IPCContextCreate(size_t length, IPCAddress_t* address, OSHandle_t* handleOut)
{
    (void)length; (void)address; (void)handleOut;
    __UNREACHED("ipcontext");
}

uuid_t
usched_notification_handle(void)
{
    return UUID_INVALID;
}

void*
SHMBuffer(OSHandle_t* handle)
{
    (void)handle;
    __UNREACHED("IPC read");
}

size_t
streambuffer_read_packet_start(streambuffer_t* stream, streambuffer_rw_options_t* options,
                               streambuffer_packet_ctx_t* context)
{
    (void)stream; (void)options; (void)context;
    __UNREACHED("IPC read");
}

void
streambuffer_read_packet_data(void* buffer, size_t length, streambuffer_packet_ctx_t* context)
{
    (void)buffer; (void)length; (void)context;
    __UNREACHED("IPC read");
}

void
streambuffer_read_packet_end(streambuffer_packet_ctx_t* context)
{
    (void)context;
    __UNREACHED("IPC read");
}

void
streambuffer_get_bytes_available_in(streambuffer_t* stream, size_t* bytesAvailable)
{
    (void)stream; (void)bytesAvailable;
    __UNREACHED("FIONREAD");
}

// Counts the descriptors in the table. The test opens no standard streams, so
// every descriptor in it was opened by the test.
static int
__OpenDescriptors(void)
{
    int count = 0;
    int iod;

    for (iod = 0; iod < INTERNAL_MAXFILES; iod++) {
        if (stdio_handle_get(iod) != NULL) {
            count++;
        }
    }
    return count;
}

// Runs every socket and IPC call on iod, and checks that each fails with
// expectedErrno.
static void
__CheckCalls(int iod, int expectedErrno)
{
    struct sockaddr_storage address = { 0 };
    socklen_t               length = sizeof(address);
    struct iovec            iov = { .iov_base = &address, .iov_len = 1 };
    struct msghdr           message = { .msg_iov = &iov, .msg_iovlen = 1 };
    IPCMessageContext_t     context;
    IPCAddress_t            target = { 0 };
    char                    buffer[4] = { 0 };
    int                     option = 0;

#define EXPECT_FAIL(call) do {               \
        g_errno = 0;                         \
        CHECK((call) == -1);                 \
        CHECK(g_errno == expectedErrno);     \
    } while (0)

    EXPECT_FAIL(accept(iod, (struct sockaddr*)&address, &length));
    EXPECT_FAIL(bind(iod, (struct sockaddr*)&address, sizeof(address)));
    EXPECT_FAIL(connect(iod, (struct sockaddr*)&address, sizeof(address)));
    EXPECT_FAIL(listen(iod, 1));
    EXPECT_FAIL(getpeername(iod, (struct sockaddr*)&address, &length));
    EXPECT_FAIL(getsockname(iod, (struct sockaddr*)&address, &length));
    EXPECT_FAIL(getsockopt(iod, SOL_SOCKET, SO_TYPE, &option, &length));
    EXPECT_FAIL(setsockopt(iod, SOL_SOCKET, SO_TYPE, &option, sizeof(option)));
    EXPECT_FAIL(shutdown(iod, SHUT_RDWR));
    EXPECT_FAIL(sockatmark(iod));
    EXPECT_FAIL(send(iod, buffer, sizeof(buffer), 0));
    EXPECT_FAIL(sendto(iod, buffer, sizeof(buffer), 0, (struct sockaddr*)&address, length));
    EXPECT_FAIL(sendmsg(iod, &message, 0));
    EXPECT_FAIL(recv(iod, buffer, sizeof(buffer), 0));
    EXPECT_FAIL(recvfrom(iod, buffer, sizeof(buffer), 0, (struct sockaddr*)&address, &length));
    EXPECT_FAIL(recvmsg(iod, &message, 0));
    EXPECT_FAIL(ipsend(iod, &target, buffer, sizeof(buffer), NULL));
    EXPECT_FAIL(iprecv(iod, buffer, sizeof(buffer), 0, &context));
#undef EXPECT_FAIL
}

static void
__CheckUnknownDescriptor(void)
{
    const int unknown = 77;

    CHECK(stdio_handle_get(unknown) == NULL);
    __CheckCalls(unknown, EBADF);
    CHECK(stdio_handle_get(unknown) == NULL);
}

static void
__CheckNotSocket(void)
{
    stdio_handle_t* handle;
    int             iod;

    CHECK(stdio_handle_create(-1, 0, 0, FILE_SIGNATURE, NULL, &handle) == 0);
    iod = handle->IOD;
    __CheckCalls(iod, ENOTSOCK);

    // The calls leave the descriptor as it was.
    CHECK(stdio_handle_get(iod) == handle);
    CHECK(handle->Signature == FILE_SIGNATURE);
    stdio_handle_delete(handle);
    CHECK(__OpenDescriptors() == 0);
}

// Runs socketpair with socket open number failSocketOpen failing (0 for none)
// and pairStatus as the answer to the pairing. Checks that it returns -1 with
// the expected errno and leaves nothing open.
static void
__CheckFailedPair(int failSocketOpen, oserr_t pairStatus, int expectedErrno)
{
    int iods[2] = { 55, 56 };

    g_socketOpens = 0;
    g_failSocketOpen = failSocketOpen;
    g_pairStatus = pairStatus;
    g_errno = 0;
    CHECK(socketpair(AF_LOCAL, SOCK_STREAM, 0, iods) == -1);
    CHECK(g_errno == expectedErrno);
    CHECK(iods[0] == -1 && iods[1] == -1);
    CHECK(g_openSockets == 0);
    CHECK(__OpenDescriptors() == 0);
}

static void
__CheckSocketPair(void)
{
    int iods[2] = { -1, -1 };
    int pairCalls;
    int iod;

    // A NULL array is refused before anything is opened.
    g_errno = 0;
    CHECK(socketpair(AF_LOCAL, SOCK_STREAM, 0, NULL) == -1);
    CHECK(g_errno == EINVAL);
    CHECK(g_socketOpens == 0);

    // The first socket fails: nothing was opened.
    __CheckFailedPair(1, OS_EOK, ENOMEM);

    // The second socket fails: the first one must be closed again.
    pairCalls = g_pairCalls;
    __CheckFailedPair(2, OS_EOK, ENOMEM);
    CHECK(g_pairCalls == pairCalls);

    // Both sockets open but netd refuses to pair them: both must be closed,
    // and errno is the one of the refusal.
    __CheckFailedPair(0, OS_ENOTSUPPORTED, ENOTSUP);
    CHECK(g_pairCalls == pairCalls + 1);
    CHECK(g_errorLines == 0);

    // The same, but the close of the second socket fails. The socket stays
    // open and an error is logged, and errno must still be the refusal and
    // not the EBUSY of the failed close.
    g_socketOpens = 0;
    g_failSocketOpen = 0;
    g_pairStatus = OS_ENOTSUPPORTED;
    g_failReleases = 1;
    g_errno = 0;
    CHECK(socketpair(AF_LOCAL, SOCK_STREAM, 0, iods) == -1);
    CHECK(g_errno == ENOTSUP);
    CHECK(g_errorLines == 1);
    CHECK(g_openSockets == 1 && __OpenDescriptors() == 1);
    for (iod = 0; iod < INTERNAL_MAXFILES; iod++) {
        if (stdio_handle_get(iod) != NULL) {
            CHECK(close(iod) == 0);
        }
    }
    CHECK(g_openSockets == 0 && __OpenDescriptors() == 0);
    g_errorLines = 0;

    // A pair that works leaves two open sockets, which close() releases.
    g_socketOpens = 0;
    g_failSocketOpen = 0;
    g_pairStatus = OS_EOK;
    CHECK(socketpair(AF_LOCAL, SOCK_STREAM, 0, iods) == 0);
    CHECK(iods[0] >= 0 && iods[1] >= 0 && iods[0] != iods[1]);
    CHECK(g_openSockets == 2);
    CHECK(__OpenDescriptors() == 2);
    CHECK(close(iods[0]) == 0);
    CHECK(close(iods[1]) == 0);
    CHECK(g_openSockets == 0);
    CHECK(__OpenDescriptors() == 0);
}

int
main(void)
{
    StdioInitialize();
    __CheckUnknownDescriptor();
    __CheckNotSocket();
    __CheckSocketPair();
    CHECK(g_openSockets == 0);
    dprintf(1, "libc sockets: bad descriptors, non-sockets and socketpair failures passed\n");
    return 0;
}
