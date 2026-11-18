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

#include <arch/mmu.h>
#include <machine.h>
#include <debug.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static SystemMachine_t g_machine;
static unsigned int    g_errors;

SystemMachine_t*
GetMachine(void)
{
    return &g_machine;
}

void
LogAppendMessage(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format,
    ...)
{
    assert(level == OSSYSLOGLEVEL_ERROR);
    assert(strstr(format, "device pool unavailable") != NULL);
    g_errors++;
}

int
main(int argc, char** argv)
{
    struct VBootMemoryEntry entry = {
        .Type = VBootMemoryType_Available,
        .PhysicalBase = 0x100000,
        .Length = 0x100000000ULL
    };
    PlatformDevicePool_t pool = { .Base = 0xfeed, .Length = 0xfeed, .Cached = false };
    bool fail = argc == 2 && strcmp(argv[1], "failure") == 0;
    oserr_t status;

    g_machine.BootInformation.Memory.EntrySize = sizeof(entry);
    g_machine.BootInformation.Memory.NumberOfEntries = 1;
    g_machine.BootInformation.Memory.Entries = (uintptr_t)&entry;
    if (fail) {
        entry.Type = VBootMemoryType_Reserved;
    }

    assert(MmuGetDevicePool(NULL) == OS_EINVALPARAMS);
    status = MmuGetDevicePool(&pool);
    assert(status == (fail ? OS_ENOENT : OS_EOK));
    if (fail) {
        assert(pool.Base == 0xfeed && pool.Length == 0xfeed && !pool.Cached);
        assert(g_errors == 1);
    } else {
        assert(pool.Base == 0xffe00000 && pool.Length == 0x200000 && pool.Cached);
        assert(g_errors == 0);
    }

    // Change the boot map after selection. Success and failure remain saved.
    entry.Type = fail ? VBootMemoryType_Available : VBootMemoryType_Reserved;
    assert(MmuGetDevicePool(&pool) == status);
    assert(pool.Base == (fail ? 0xfeed : 0xffe00000));
    assert(pool.Length == (fail ? 0xfeed : 0x200000));
    assert(pool.Cached == !fail);
    assert(g_errors == (fail ? 2U : 0U));
    puts("x86 device pool: saved selection and failure passed");
    return 0;
}
