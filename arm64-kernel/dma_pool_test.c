/**
 * Copyright 2026, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "../../kernel/arch/aarch64/common/device-pool.h"
#include "../../kernel/arch/aarch64/common/boot-memory.h"
#include <arch/aarch64/arch.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/types/syslog.h>

static _Alignas(4096) unsigned char g_blob[0x100000];
static bool g_forbidAllocation;

void* __real_malloc(size_t size);
void* __real_calloc(size_t count, size_t size);
void* __real_realloc(void* pointer, size_t size);
void* __wrap_malloc(size_t size)
{
    assert(!g_forbidAllocation);
    return __real_malloc(size);
}
void* __wrap_calloc(size_t count, size_t size)
{
    assert(!g_forbidAllocation);
    return __real_calloc(count, size);
}
void* __wrap_realloc(void* pointer, size_t size)
{
    assert(!g_forbidAllocation);
    return __real_realloc(pointer, size);
}

void LogAppendMessage(enum OSSysLogLevel level, const char* format, ...)
{
    (void)level;
    (void)format;
}

static void
__ExpectUnchanged(
    _In_ const PlatformDevicePool_t* pool)
{
    assert(pool->Base == 0xfeed && pool->Length == 0xfeed && pool->Cached);
}

int
main(int argc, char** argv)
{
    static struct VBootMemoryEntry entries[] = {
        { VBootMemoryType_Firmware, 0, 0, 0x1000000, 0 },
        { VBootMemoryType_Available, 0x1000000, 0, 0x1ff000000ULL, 0 },
        { VBootMemoryType_Reserved, 0x300000000ULL, 0, 0x1000, 0 },
        { VBootMemoryType_Reserved, 0x300001000ULL, 0, 0x1000, 0 }
    };
    struct VBoot boot = {
        .Magic = VBOOT_MAGIC,
        .Version = VBOOT_VERSION,
        .Memory = {
            .Entries = (uintptr_t)entries,
            .EntrySize = sizeof(entries[0]),
            .NumberOfEntries = 4
        }
    };
    PlatformDevicePool_t pool = { .Base = 0xfeed, .Length = 0xfeed, .Cached = true };
    PlatformDevicePool_t saved;
    uint64_t            expectedBase;
    oserr_t             expectedStatus;
    unsigned int        mode;
    unsigned char       savedMagic;
    FILE*               file;
    size_t              length;
    oserr_t             status;

    assert(argc == 5);
    expectedBase = strtoull(argv[2], NULL, 0);
    expectedStatus = (oserr_t)strtoul(argv[3], NULL, 0);
    mode = (unsigned int)strtoul(argv[4], NULL, 0);
    file = fopen(argv[1], "rb");
    assert(file != NULL);
    length = fread(g_blob, 1, sizeof(g_blob), file);
    assert(length > 0 && !ferror(file) && feof(file));
    assert(fclose(file) == 0);
    boot.DeviceTree.PhysicalBase = (uintptr_t)g_blob;
    boot.DeviceTree.Length = (uint32_t)length;

    if (mode == 1) {
        // A marked pool competes with available RAM and must obey the route.
        entries[1].Length = 0x8f000000;
        entries[2] = (struct VBootMemoryEntry) { VBootMemoryType_DevicePool, 0x90000000, 0, 0x400000, 0 };
        entries[3] = (struct VBootMemoryEntry) { VBootMemoryType_Available, 0x90400000, 0, 0x16fc00000ULL, 0 };
    } else if (mode == 2) {
        // The whole DTB must be covered by known reserved RAM before parsing.
        entries[0].Type = VBootMemoryType_Reserved;
    } else if (mode == 3) {
        entries[0].Attributes = VBOOT_MEMORY_NO_MAP;
    } else if (mode == 4) {
        boot.DeviceTree.PhysicalBase = UINT64_MAX - 4;
    } else if (mode == 5) {
        // Adjacent firmware entries may jointly cover the tree.
        entries[0].Length = (uintptr_t)g_blob + 32;
        entries[2] = (struct VBootMemoryEntry) {
            VBootMemoryType_Firmware, entries[0].Length, 0, 0x1000000 - entries[0].Length, 0
        };
    } else if (mode == 6) {
        // No available memory inside a low firmware window.
        entries[1].PhysicalBase = 0x80000000;
        entries[1].Length = 0x180000000ULL;
    } else if (mode == 7) {
        // The marked pool starts inside the route but its last 2 MiB do not.
        entries[1].Length = 0x3ee00000;
        entries[2] = (struct VBootMemoryEntry) { VBootMemoryType_DevicePool, 0x3fe00000, 0, 0x400000, 0 };
        entries[3] = (struct VBootMemoryEntry) { VBootMemoryType_Available, 0x40200000, 0, 0x1bfe00000ULL, 0 };
    }
    assert(Arm64ValidateBootMemory(&boot, MEMORY_LOCATION_RING3_CODE) == OS_EOK);
    g_forbidAllocation = true;
    status = Arm64PrepareDevicePool(&boot, &pool);
    if (status != expectedStatus) {
        (void)MmuGetDevicePool(&boot, &pool);
        fflush(stdout);
        fprintf(stderr, "unexpected pool status %u (expected %u)\n", status, expectedStatus);
    }
    assert(status == expectedStatus);
    if (status != OS_EOK) {
        __ExpectUnchanged(&pool);
        assert(MmuGetDevicePool(&boot, &pool) == status);
        __ExpectUnchanged(&pool);
        g_forbidAllocation = false;
        return 0;
    }
    assert(pool.Base == expectedBase && pool.Length == 0x400000 && !pool.Cached);
    saved = pool;

    // Later reservation must consume the saved early choice without parsing
    // firmware or selecting again, even if the inputs have changed.
    savedMagic = g_blob[0];
    g_blob[0] = 0;
    entries[1].Type = VBootMemoryType_Reserved;
    pool = (PlatformDevicePool_t) { .Base = 0xfeed, .Length = 0xfeed, .Cached = true };
    assert(MmuGetDevicePool(&boot, &pool) == OS_EOK);
    assert(pool.Base == saved.Base && pool.Length == saved.Length && pool.Cached == saved.Cached);
#if defined(__OSCONFIG_DEVICETREE_SUPPORT)
    // A failed preparation must erase a previous selection, not expose it later.
    pool = (PlatformDevicePool_t) { .Base = 0xfeed, .Length = 0xfeed, .Cached = true };
    assert(Arm64PrepareDevicePool(&boot, &pool) != OS_EOK);
    __ExpectUnchanged(&pool);
    assert(MmuGetDevicePool(&boot, &pool) != OS_EOK);
    __ExpectUnchanged(&pool);
#endif
    g_blob[0] = savedMagic;
    g_forbidAllocation = false;
    return 0;
}
