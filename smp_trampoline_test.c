/* Exercise the production trampoline setup without privileged instructions. */
#include "../kernel/arch/x86/components/smp.c"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
const int __GlbTramplineCode_length = 64;
const char __GlbTramplineCode[64] = { 0xFA, 0xFC };
static SystemMachine_t g_machine;
static MemorySpace_t g_space;
static _Alignas(4096) char g_pageCopy[4096];
static uintptr_t g_page = 0x9f000;
static void* g_stacks;
static int g_failPhysical, g_failStack, g_failMap, g_freed, g_maps;
static int g_ipis, g_sipis, g_stalls, g_runningAfter, g_failSipi;
void LogAppendMessage(enum OSSysLogLevel level, const char* format, ...) { (void)level; (void)format; }
SystemMachine_t* GetMachine(void) { return &g_machine; }
MemorySpace_t* GetCurrentMemorySpace(void) { return &g_space; }
void* kmalloc(size_t size) {
    CHECK(size == 3 * THREADING_KERNEL_STACK_SIZE);
    if (g_failStack) return NULL;
    CHECK(g_stacks == NULL); g_stacks = malloc(size); return g_stacks;
}
void kfree(void* p) { CHECK(p == g_stacks); free(p); g_stacks = NULL; }
oserr_t AllocatePhysicalMemory(size_t mask, int count, uintptr_t* pages) {
    CHECK(mask == MEMORY_MASK_BIOS && count == 1);
    if (g_failPhysical) return OS_EOOM;
    *pages = g_page; return OS_EOK;
}
void FreePhysicalMemory(int count, const uintptr_t* pages) { CHECK(count == 1 && *pages == g_page); g_freed++; }
oserr_t MemorySpaceMap(MemorySpace_t* space, struct MemorySpaceMapOptions* options, vaddr_t* mapping) {
    CHECK(space == &g_space && options->VirtualStart == g_page);
    CHECK(options->Pages != NULL && *options->Pages == g_page && options->Length == 4096);
    CHECK(options->Flags == (MAPPING_COMMIT | MAPPING_EXECUTABLE | MAPPING_PERSISTENT));
    CHECK(options->PlacementFlags == (MAPPING_PHYSICAL_FIXED | MAPPING_VIRTUAL_FIXED));
    g_maps++;
    if (g_failMap) return OS_EOOM;
    *mapping = (vaddr_t)g_pageCopy; return OS_EOK;
}
irqstate_t InterruptDisable(void) { return 0; }
void GdtInstall(void) { abort(); }
void IdtInstall(void) { abort(); }
void CpuInitializeFeatures(void) { abort(); }
void ApicInitializeForApplicationCore(void) { abort(); }
void CpuCoreStart(void) { abort(); }
uuid_t CpuCoreId(SystemCpuCore_t* core) { (void)core; return 3; }
SystemCpuState_t CpuCoreState(SystemCpuCore_t* core) {
    (void)core;
    return g_sipis && g_stalls >= g_runningAfter ? CpuStateRunning : CpuStateUnavailable;
}
oserr_t ApicPerformIPI(uuid_t id, int assert) { CHECK(id == 3 && assert == 1); g_ipis++; return OS_EOK; }
oserr_t ApicPerformSIPI(uuid_t id, uintptr_t address) {
    CHECK(id == 3 && address == g_page); g_sipis++; return g_failSipi ? OS_EUNKNOWN : OS_EOK;
}
void SystemTimerGetWallClockTime(OSTimestamp_t* time) { memset(time, 0, sizeof(*time)); }
void SystemTimerStall(OSTimestamp_t* deadline) { (void)deadline; g_stalls++; }
static void Reset(void) {
    if (g_stacks) kfree(g_stacks);
    g_jumpSpace = 0; g_page = 0x9f000; g_space.PlatformData.Cr3PhysicalAddress = 0x1000000;
    atomic_store(&g_machine.NumberOfCores, 4);
    g_failPhysical = g_failStack = g_failMap = g_freed = g_maps = 0;
    g_ipis = g_sipis = g_stalls = g_failSipi = 0; g_runningAfter = 1;
}
int main(void) {
    Reset();
    g_space.PlatformData.Cr3PhysicalAddress = 0x100000000ULL;
    CHECK(InitializeApplicationJumpSpace() == OS_ENOTSUPPORTED && g_maps == 0);
    Reset(); g_failPhysical = 1;
    CHECK(InitializeApplicationJumpSpace() == OS_EOOM && g_freed == 0);
    Reset(); g_page = 0;
    CHECK(InitializeApplicationJumpSpace() == OS_ENOTSUPPORTED && g_freed == 1);
    Reset(); g_failStack = 1;
    CHECK(InitializeApplicationJumpSpace() == OS_EOOM && g_freed == 1);
    Reset(); g_failMap = 1;
    StartApplicationCore((SystemCpuCore_t*)1);
    CHECK(g_jumpSpace == 0 && g_freed == 1 && g_stacks == NULL && g_sipis == 0);
    g_failMap = 0;
    StartApplicationCore((SystemCpuCore_t*)1);
    CHECK(g_jumpSpace == g_page && g_sipis == 1 && g_ipis == 1);
    uint64_t* parameters = (uint64_t*)(g_pageCopy + __GlbTramplineCode_length);
    CHECK(parameters[-1] == (uintptr_t)SmpApplicationCoreEntry);
    CHECK(parameters[-2] == 0x1000000 && parameters[-4] == THREADING_KERNEL_STACK_SIZE);
    CHECK(parameters[-3] == (uintptr_t)g_stacks + THREADING_KERNEL_STACK_SIZE);
    CHECK(g_pageCopy[0] == (char)0xFA && __GlbTramplineCode[63] == 0);
    Reset(); g_runningAfter = 15;
    StartApplicationCore((SystemCpuCore_t*)1);
    CHECK(g_sipis == 2 && g_stalls == 15);
    Reset(); g_runningAfter = 2000;
    StartApplicationCore((SystemCpuCore_t*)1);
    CHECK(g_sipis == 2 && g_stalls == 1011 && g_jumpSpace == g_page && g_stacks != NULL);
    Reset(); g_failSipi = 1;
    StartApplicationCore((SystemCpuCore_t*)1);
    CHECK(g_sipis == 1 && g_stalls == 1 && g_stacks != NULL);
    Reset();
    puts("SMP trampoline: low-page mapping, trailer, allocation failures, retry and bounded waits passed");
    return 0;
}
