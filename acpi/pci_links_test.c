// Build the real x86 link cache, free its _PRT buffer, then query the copied key.
#include "../../kernel/acpi/irq.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned int g_allocations;
static int g_failAfter = -1;
static int g_failResources;
static AcpiDevice_t g_device = { .Handle = (void*)1 };
void* kmalloc(size_t length)
{
    if (g_failAfter == 0) return NULL;
    if (g_failAfter > 0) g_failAfter--;
    g_allocations++; return calloc(1, length);
}
void kfree(void* pointer) { if (pointer) { assert(g_allocations); g_allocations--; free(pointer); } }
void AcpiOsFree(void* pointer) { free(pointer); }
void SpinlockConstruct(Spinlock_t* lock) { memset(lock,0,sizeof(*lock)); }
void SpinlockAcquireIrq(Spinlock_t* lock) { (void)lock; }
void SpinlockReleaseIrq(Spinlock_t* lock) { (void)lock; }
void LogAppendMessage(enum OSSysLogLevel level, const char* format, ...) { (void)level; (void)format; }
oserr_t InterruptIncreasePenalty(int irq) { assert(irq == 11); return OS_EOK; }
int InterruptGetLeastLoaded(int* irqs, int count) { return count ? irqs[0] : INTERRUPT_NONE; }
AcpiDevice_t* AcpiDeviceLookupHandle(ACPI_HANDLE handle) { return &g_device; }
ACPI_STATUS AcpiDeviceQueryStatus(ACPI_HANDLE handle, unsigned int* out) { *out = 15; return AE_OK; }
ACPI_STATUS AcpiGetHandle(ACPI_HANDLE parent, ACPI_STRING path, ACPI_HANDLE* out)
{
    assert(!strcmp(path,"LNKA")); *out = (void*)2; return AE_OK;
}
ACPI_STATUS AcpiSetCurrentResources(ACPI_HANDLE handle, ACPI_BUFFER* buffer) { return AE_OK; }
ACPI_STATUS AcpiWalkResources(ACPI_HANDLE handle, ACPI_STRING name, ACPI_WALK_RESOURCE_CALLBACK callback, void* context)
{
    ACPI_RESOURCE resource = { .Type = ACPI_RESOURCE_TYPE_IRQ };
    if (g_failResources && !strcmp(name,"_CRS")) return AE_ERROR;
    resource.Data.Irq.InterruptCount = 1; resource.Data.Irq.Interrupts[0] = 11;
    resource.Data.Irq.Triggering = ACPI_LEVEL_SENSITIVE;
    resource.Data.Irq.Polarity = ACPI_ACTIVE_LOW; resource.Data.Irq.Shareable = 1;
    return callback(&resource,context);
}
ACPI_STATUS AcpiGetIrqRoutingTable(ACPI_HANDLE handle, ACPI_BUFFER* buffer)
{
    ACPI_PCI_ROUTING_TABLE* entry;
    buffer->Pointer = calloc(1,40); buffer->Length = 40;
    entry = buffer->Pointer; entry->Length = 32;
    entry->Address = (3 << 16) | 0xffff; entry->Pin = 0;
    strcpy(entry->Source,"LNKA"); return AE_OK;
}
int main(void)
{
    struct AcpiPciQuery query = { .Address = {3,1}, .Pin = 1 };
    struct AcpiPciInterrupt route;
    assert(AcpiDeviceGetIrqRoutings(&g_device) == AE_OK);
    assert(g_allocations == 3);
    assert(AcpiPciRoute((void*)1,&query,&route) == OS_EOK && route.Line == 11);
    __DestroyRoutings(g_device.Routings); g_device.Routings = NULL;
    assert(g_allocations == 0);
    for (g_failAfter = 0; g_failAfter < 3; ) {
        int failure = g_failAfter;
        assert(ACPI_FAILURE(AcpiDeviceGetIrqRoutings(&g_device)));
        assert(!g_device.Routings && !g_allocations);
        g_failAfter = failure + 1;
    }
    g_failAfter = -1; g_failResources = 1;
    assert(ACPI_FAILURE(AcpiDeviceGetIrqRoutings(&g_device)));
    assert(!g_device.Routings && !g_allocations);
    puts("x86 _PRT links: copied name lifetime and allocation/resource failure cleanup: PASS");
    return 0;
}
