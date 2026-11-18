/**
 * Copyright, Philip Meulengracht
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
 * Standalone host fixture using real DMA, SystemMemory and MemoryStack.
 * Register behavior lives in register_model.c so another fixture can supply
 * memory and locking without linking these single-unit kernel mocks.
 */

#include "model.h"
#include <component/timer.h>
#include <debug.h>
#include <machine.h>
#include <memoryspace.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int           g_failHeapAfter;
bool          g_noHeap;
bool          g_failMap;
bool          g_failUnmap;
bool          g_failRelease;
bool          g_failRegisters;
size_t        g_heapCount;
size_t        g_unmaps;
size_t        g_releases;

static SystemMachine_t g_machine;
static void*           g_stack;
static void*           g_mapping;
static paddr_t         g_mappingPhysical;
static size_t          g_mappingLength;
static Mutex_t*        g_unitMutex;
static Mutex_t*        g_lockedMutex;
static Spinlock_t*     g_lockedSpin;
static unsigned int    g_mutexCount;

SystemMachine_t* GetMachine(void) { return &g_machine; }
MemorySpace_t* GetDomainMemorySpace(void) { return &g_machine.SystemSpace; }
size_t GetMemorySpacePageSize(void) { return 4096; }

void* kmalloc(size_t length)
{
    void* data;

    assert(!g_noHeap);
    if (g_failHeapAfter == 0) return NULL;
    if (g_failHeapAfter > 0) g_failHeapAfter--;
    data = malloc(length);
    assert(data != NULL);
    g_heapCount++;
    return data;
}

void kfree(void* data)
{
    if (data == NULL) return;
    assert(g_heapCount != 0);
    g_heapCount--;
    free(data);
}

void LogAppendMessage(enum OSSysLogLevel level, const char* format, ...)
{
    (void)level;
    (void)format;
}

void MutexConstruct(Mutex_t* mutex, unsigned int flags)
{
    assert(flags == MUTEX_FLAG_PLAIN);
    assert(pthread_mutex_init(&mutex->Native, NULL) == 0);
    // The unit is created before its IOVA spaces. Remember that mutex so
    // holding an unrelated space mutex cannot satisfy an MMIO assertion.
    if (g_mutexCount == 0) g_unitMutex = mutex;
    g_mutexCount++;
}

void MutexDestruct(Mutex_t* mutex)
{
    assert(g_lockedMutex == NULL && g_mutexCount != 0);
    if (mutex == g_unitMutex) {
        assert(g_mutexCount == 1);
        g_unitMutex = NULL;
    }
    assert(pthread_mutex_destroy(&mutex->Native) == 0);
    g_mutexCount--;
}

void MutexLock(Mutex_t* mutex)
{
    assert(g_lockedMutex == NULL && g_lockedSpin == NULL);
    assert(pthread_mutex_lock(&mutex->Native) == 0);
    g_lockedMutex = mutex;
}

void MutexUnlock(Mutex_t* mutex)
{
    assert(g_lockedMutex == mutex && g_lockedSpin == NULL);
    g_lockedMutex = NULL;
    assert(pthread_mutex_unlock(&mutex->Native) == 0);
}

void
SMMUTestAssertUnitLocked(
    _In_ uintptr_t registers)
{
    assert(registers == (uintptr_t)&g_smmu500Model);
    assert(g_unitMutex != NULL && g_lockedMutex == g_unitMutex);
}

void SpinlockConstruct(Spinlock_t* lock) { memset(lock, 0, sizeof(*lock)); }
void SpinlockAcquireIrq(Spinlock_t* lock)
{
    assert(g_lockedSpin == NULL && atomic_exchange(&lock->Current, 1) == 0);
    g_lockedSpin = lock;
}
void SpinlockReleaseIrq(Spinlock_t* lock)
{
    assert(g_lockedSpin == lock && atomic_exchange(&lock->Current, 0) == 1);
    g_lockedSpin = NULL;
}

void SystemTimerGetClockFrequency(UInteger64_t* frequency)
{
    frequency->QuadPart = g_smmu500Model.Clock ? 1000000000ULL : 0;
}
void SystemTimerGetTimestamp(tick_t* tick)
{
    g_smmu500Model.Now += g_smmu500Model.TimeStep;
    *tick = (tick_t)g_smmu500Model.Now;
}

uintptr_t Arm64MapDevice(uint64_t address, uint64_t length)
{
    assert(address == 0x100000 && length <= SMMU500_MODEL_REGISTER_SIZE);
    g_smmu500Model.RegisterLength = length;
    return g_failRegisters ? 0 : (uintptr_t)&g_smmu500Model;
}

oserr_t MemorySpaceMap(MemorySpace_t* space, struct MemorySpaceMapOptions* options, vaddr_t* output)
{
    unsigned int expected = MAPPING_COMMIT | MAPPING_PERSISTENT;

    assert(g_lockedMutex != NULL && g_mapping == NULL && space == &g_machine.SystemSpace);
    if (!g_machine.PhysicalMemory.DeviceMemory.Cached) expected |= MAPPING_NOCACHE;
    assert(options->Flags == expected);
    assert(options->PlacementFlags == (MAPPING_PHYSICAL_CONTIGUOUS | MAPPING_VIRTUAL_GLOBAL));
    if (g_failMap) return OS_EOOM;
    g_mapping = aligned_alloc(4096, options->Length);
    assert(g_mapping != NULL);
    memset(g_mapping, 0xA5, options->Length);
    g_mappingPhysical = options->PhysicalStart;
    g_mappingLength = options->Length;
    *output = (vaddr_t)g_mapping;
    return OS_EOK;
}

oserr_t MemorySpaceUnmap(MemorySpace_t* space, vaddr_t address, size_t length)
{
    assert(g_lockedMutex != NULL && space == &g_machine.SystemSpace);
    assert(g_mapping != NULL && address == (vaddr_t)g_mapping && length == g_mappingLength);
    // No CPU/physical release while this arena is still reachable by hardware.
    if (g_smmu500Model.Writes != 0) {
        unsigned int index;

        assert((g_smmu500Model.Registers[0] & 1U) == 0);
        assert(!g_smmu500Model.PendingSync && g_smmu500Model.PostedCount == 0 && g_smmu500Model.HeldWalks == 0);
        for (index = 0; index < (g_smmu500Model.Registers[0x24 / 4] & 255); index++) {
            assert((g_smmu500Model.Registers[SMMU500ModelBank(index) / 4] & 1U) == 0);
        }
        for (index = 0; index < SMMU500_MODEL_CACHE_ENTRIES; index++) {
            assert(!g_smmu500Model.Translations[index].Present && !g_smmu500Model.Routes[index].Present);
        }
    }
    g_unmaps++;
    if (g_failUnmap) return OS_ETIMEOUT;
    free(g_mapping);
    g_mapping = NULL;
    return OS_EOK;
}

oserr_t __real_SystemMemoryReleaseAllocation(struct SystemMemoryAllocation* allocation);
oserr_t __wrap_SystemMemoryReleaseAllocation(struct SystemMemoryAllocation* allocation)
{
    if (allocation->Owner != NULL) {
        assert(g_mapping == NULL);
        g_releases++;
        if (g_failRelease) return OS_EBUSY;
    }
    return __real_SystemMemoryReleaseAllocation(allocation);
}

void*
SMMUTestPhysical(
    _In_ uint64_t physicalAddress,
    _In_ size_t   length)
{
    assert(g_mapping != NULL && physicalAddress >= g_mappingPhysical);
    assert(physicalAddress - g_mappingPhysical <= g_mappingLength);
    assert(length <= g_mappingLength - (physicalAddress - g_mappingPhysical));
    return (char*)g_mapping + (physicalAddress - g_mappingPhysical);
}

void SMMU500ModelBegin(bool cached)
{
    size_t bytes = MemoryStackCalculateSize(SMMU500_MODEL_POOL_SIZE / 4096);

    assert(g_heapCount == 0 && g_mutexCount == 0 && g_mapping == NULL);
    memset(&g_machine, 0, sizeof(g_machine));
    SMMU500ModelReset();
    g_smmu500Model.TableCached = cached;
    g_failHeapAfter = -1;
    g_noHeap = g_failMap = g_failUnmap = g_failRelease = g_failRegisters = false;
    g_unmaps = g_releases = 0;
    g_stack = malloc(bytes);
    assert(g_stack != NULL);
    SystemMemoryConstruct(&g_machine.PhysicalMemory, SMMU500_MODEL_POOL_BASE, SMMU500_MODEL_POOL_SIZE, 4096, 0);
    assert(SystemMemoryInitializeDeviceRegion(&g_machine.PhysicalMemory,
        SMMU500_MODEL_POOL_BASE, SMMU500_MODEL_POOL_SIZE, cached, g_stack, bytes) == OS_EOK);
}

void SMMU500ModelEnd(void)
{
    assert(g_heapCount == 0 && g_mutexCount == 0 && g_mapping == NULL);
    assert(MemoryStackFreeBlockCount(&g_machine.PhysicalMemory.DeviceMemory.Stack) == SMMU500_MODEL_POOL_SIZE / 4096);
    free(g_stack);
    g_stack = NULL;
}

SMMU500Unit_t* SMMU500ModelStart(void)
{
    ArmSMMUDescription_t description = SMMU500ModelDescription();
    SMMU500Unit_t* unit = NULL;

    assert(SMMU500UnitCreate(&description, &unit) == OS_EOK);
    assert(g_smmu500Model.Writes == 0);
    assert(SMMU500UnitStart(unit) == OS_EOK);
    assert((g_smmu500Model.Registers[0] & 0x8000FC3F) == 0x1C12);
    assert(!g_smmu500Model.PendingSync && g_smmu500Model.Syncs == 1);
    return unit;
}

void SMMU500ModelPinned(void (*test)(void))
{
    pid_t child = fork();
    int status;

    if (child == -1) { perror("fork"); exit(EXIT_FAILURE); }
    if (child == 0) { test(); _exit(0); }
    if (waitpid(child, &status, 0) != child) { perror("waitpid"); exit(EXIT_FAILURE); }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return;
    fprintf(stderr, "pinned scenario child failed: %d\n", status);
    // Preserve assertion classification. Exit failures and unrelated signals
    // must never be converted into an assertion by this parent process.
    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT) {
        if (raise(SIGABRT) != 0) { perror("raise"); exit(EXIT_FAILURE); }
    }
    exit(EXIT_FAILURE);
}
