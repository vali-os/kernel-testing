// Exercise anonymous mmap with the target ABI and ordinary SHM allocation.
#define mmap __TestMmap
#define munmap __TestMunmap
#define msync __TestMsync
#include "../librt/libc/compat/mman.c"
#undef mmap
#undef munmap
#undef msync

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static unsigned int g_creates;
static SHM_t g_options;
static char g_buffer[4096];

int*
__errno(void)
{
    static int error;

    return &error;
}

oserr_t
SHMCreate(
    _In_  SHM_t*      options,
    _Out_ OSHandle_t* handle)
{
    g_creates++;
    g_options = *options;
    *handle = (OSHandle_t){ .ID = 1 };
    return OS_EOK;
}

void*
SHMBuffer(
    _In_ OSHandle_t* handle)
{
    CHECK(handle->ID == 1);
    return g_buffer;
}

int
OsErrToErrNo(
    _In_ oserr_t status)
{
    CHECK(status == OS_EOK);
    return 0;
}

void
usched_mtx_lock(
    _In_ struct usched_mtx* mutex)
{
    (void)mutex;
}

void
usched_mtx_unlock(
    _In_ struct usched_mtx* mutex)
{
    (void)mutex;
}

void
spinlock_acquire(
    _In_ spinlock_t* lock)
{
    (void)lock;
}

void
spinlock_release(
    _In_ spinlock_t* lock)
{
    (void)lock;
}

int
main(void)
{
    struct __mmap_item* item;
    int flags = MAP_ANONYMOUS | MAP_SHARED;

    CHECK(__mmap_anonymous(NULL, sizeof(g_buffer), PROT_READ, flags | MAP_32BIT) == NULL);
    CHECK(errno == ENOTSUP && g_creates == 0 && g_mmaps.count == 0);
    CHECK(__mmap_anonymous(NULL, sizeof(g_buffer), PROT_READ | PROT_WRITE, flags) == g_buffer);
    CHECK(g_creates == 1 && g_options.Flags == SHM_CLEAN);
    CHECK(g_options.Access == (SHM_ACCESS_READ | SHM_ACCESS_WRITE));
    item = __get_mmap_from_addr(g_buffer);
    CHECK(item != NULL);
    list_remove(&g_mmaps, &item->Header);
    free(item);

    // MAP_FIXED ignores MAP_32BIT. Fixed placement itself is still unsupported.
    CHECK(__mmap_anonymous(NULL, sizeof(g_buffer), PROT_READ,
        flags | MAP_FIXED | MAP_32BIT | MAP_POPULATE) == g_buffer);
    CHECK(g_creates == 2 && g_options.Flags == (SHM_CLEAN | SHM_COMMIT));
    item = __get_mmap_from_addr(g_buffer);
    CHECK(item != NULL);
    list_remove(&g_mmaps, &item->Header);
    free(item);
    puts("anonymous mmap: ordinary SHM, unsupported low virtual placement and MAP_FIXED precedence passed");
    return 0;
}
