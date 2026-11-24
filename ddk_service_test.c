// Tests the service lookup in libddk. Each getter must look up its own handle
// path, a found handle must be cached, and a service that is missing must be
// looked up again on the next call. The wait functions must sleep 10 ms
// between lookups and give up once their timeout has been slept, also when the
// timeout is not a multiple of 10 ms. A timeout of zero waits forever.
#ifndef DDK_SERVICE_SOURCE
#define DDK_SERVICE_SOURCE "../librt/libos/services/lookup.c"
#endif
#include DDK_SERVICE_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

// A wait that sleeps more often than this is treated as one that never ends.
#define SLEEP_LIMIT 10000

struct __FakeService {
    const char*  Path;
    uuid_t       Handle;
    // Number of lookups that still fail before the service appears. A negative
    // value means the service never appears.
    int          MissesLeft;
    unsigned int Lookups;
};

static struct __FakeService g_fakes[] = {
    { "/service/device",  12, 0, 0 },
    { "/service/usb",     13, 0, 0 },
    { "/service/process", 14, 0, 0 },
    { "/service/file",    15, 0, 0 },
    { "/service/net",     16, 0, 0 },
    { "/service/syslog",  17, 0, 0 },
};
enum { FAKE_DEVICE, FAKE_USB, FAKE_PROCESS, FAKE_FILE, FAKE_NET, FAKE_SYSLOG, FAKE_COUNT };

static unsigned int g_sleeps;
static unsigned int g_warnings;

oserr_t
Syscall_HandleLookup(
    _In_  const char* path,
    _Out_ uuid_t*     handleOut)
{
    int i;

    for (i = 0; i < FAKE_COUNT; i++) {
        if (strcmp(g_fakes[i].Path, path) == 0) {
            g_fakes[i].Lookups++;
            if (g_fakes[i].MissesLeft != 0) {
                if (g_fakes[i].MissesLeft > 0) {
                    g_fakes[i].MissesLeft--;
                }
                return OS_ENOENT;
            }
            *handleOut = g_fakes[i].Handle;
            return OS_EOK;
        }
    }
    printf("unexpected lookup of %s\n", path);
    exit(1);
}

int
thrd_sleep(
    _In_ const struct timespec* duration,
    _In_ struct timespec*       remaining)
{
    CHECK(duration->tv_sec == 0 && duration->tv_nsec == 10 * NSEC_PER_MSEC);
    CHECK(remaining == NULL);
    CHECK(++g_sleeps <= SLEEP_LIMIT);
    return 0;
}

void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format, ...)
{
    (void)format;
    if (level == OSSYSLOGLEVEL_WARNING) {
        g_warnings++;
    }
}

static void
__TestLookupAndCache(void)
{
    struct __FakeService* device = &g_fakes[FAKE_DEVICE];

    // A missing service is not remembered. Every call looks it up again.
    device->MissesLeft = 2;
    CHECK(OSServiceGetDevice() == UUID_INVALID && device->Lookups == 1);
    CHECK(OSServiceGetDevice() == UUID_INVALID && device->Lookups == 2);

    // Once found, the handle is reused without another lookup.
    CHECK(OSServiceGetDevice() == 12 && device->Lookups == 3);
    CHECK(OSServiceGetDevice() == 12 && device->Lookups == 3);
}

static void
__TestWait(void)
{
    struct __FakeService* usb = &g_fakes[FAKE_USB];
    struct __FakeService* process = &g_fakes[FAKE_PROCESS];
    struct __FakeService* file = &g_fakes[FAKE_FILE];
    struct __FakeService* net = &g_fakes[FAKE_NET];
    struct __FakeService* syslog = &g_fakes[FAKE_SYSLOG];

    // A service that is already known returns at once, without a lookup.
    g_sleeps = 0;
    CHECK(OSServiceWaitForDevice(1000) == OS_EOK && g_sleeps == 0);
    CHECK(OSServiceWaitForDevice(0) == OS_EOK && g_sleeps == 0);
    CHECK(g_fakes[FAKE_DEVICE].Lookups == 3);

    // Every service uses its own path. With a wrong path the USB lookup below
    // would return the handle of another service.
    // The service appears on the fourth lookup, after three sleeps.
    usb->MissesLeft = 3;
    CHECK(OSServiceWaitForUsb(1000) == OS_EOK);
    CHECK(g_sleeps == 3 && usb->Lookups == 4 && g_warnings == 0);
    CHECK(OSServiceGetUsb() == 13 && usb->Lookups == 4);

    // 30 ms allows three sleeps, and a last lookup after the third one.
    g_sleeps = 0;
    process->MissesLeft = -1;
    CHECK(OSServiceWaitForProcess(30) == OS_ETIMEOUT);
    CHECK(g_sleeps == 3 && process->Lookups == 4 && g_warnings == 1);

    // 15 ms is not a multiple of the 10 ms between lookups. The wait must
    // still end, after the second sleep has passed the timeout.
    g_sleeps = 0;
    file->MissesLeft = -1;
    CHECK(OSServiceWaitForFile(15) == OS_ETIMEOUT);
    CHECK(g_sleeps == 2 && file->Lookups == 3 && g_warnings == 2);

    // A timeout shorter than one sleep still sleeps once.
    g_sleeps = 0;
    syslog->MissesLeft = -1;
    CHECK(OSServiceWaitForSysLog(5) == OS_ETIMEOUT);
    CHECK(g_sleeps == 1 && syslog->Lookups == 2 && g_warnings == 3);

    // A timeout does not stop a later call from finding the service.
    g_sleeps = 0;
    syslog->MissesLeft = 0;
    CHECK(OSServiceWaitForSysLog(5) == OS_EOK && g_sleeps == 0);
    CHECK(OSServiceGetSysLog() == 17 && syslog->Lookups == 3);
    file->MissesLeft = 1;
    CHECK(OSServiceWaitForFile(15) == OS_EOK && g_sleeps == 1);
    CHECK(OSServiceGetFile() == 15);
    process->MissesLeft = 0;
    CHECK(OSServiceGetProcess() == 14);

    // Zero waits for as long as it takes, far past any other timeout here.
    g_sleeps = 0;
    net->MissesLeft = 500;
    CHECK(OSServiceWaitForNet(0) == OS_EOK);
    CHECK(g_sleeps == 500 && net->Lookups == 501);
    CHECK(OSServiceGetNet() == 16 && net->Lookups == 501);
    CHECK(g_warnings == 3);
}

int
main(void)
{
    __TestLookupAndCache();
    __TestWait();
    puts("DDK service lookup: paths, caching, retry of missing services and wait timeouts passed");
    return 0;
}
