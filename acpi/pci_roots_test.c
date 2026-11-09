// Exercise production root parsing and query validation with bounded ACPICA resources.
#include "../../kernel/acpi/pci-roots.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

static ACPI_RESOURCE g_resources[4];
static unsigned int g_resourceCount;
static UINT64 g_segment;
static UINT64 g_bbn;
static UINT64 g_sta = 15;
static int g_bbnMissing;
static int g_failAllocation;
static unsigned int g_allocations;
static unsigned int g_routeCalls;

void* kmalloc(size_t size) { if (g_failAllocation) return NULL; g_allocations++; return calloc(1, size); }
void kfree(void* p) { if (p) { assert(g_allocations); g_allocations--; free(p); } }
void AcpiOsFree(void* p) { free(p); }
void LogAppendMessage(enum OSSysLogLevel level, const char* format, ...) { (void)level; (void)format; }
ACPI_STATUS AcpiGetObjectInfo(ACPI_HANDLE h, ACPI_DEVICE_INFO** out)
{
    *out = calloc(1, sizeof(**out)); (*out)->Flags = ACPI_PCI_ROOT_BRIDGE; return AE_OK;
}
ACPI_STATUS AcpiEvaluateObject(ACPI_HANDLE handle, ACPI_STRING name, ACPI_OBJECT_LIST* args, ACPI_BUFFER* out)
{
    ACPI_OBJECT* object = out->Pointer;
    object->Type = ACPI_TYPE_INTEGER;
    if (!strcmp(name, "_STA")) object->Integer.Value = g_sta;
    else if (!strcmp(name, "_SEG")) object->Integer.Value = g_segment;
    else if (!strcmp(name, "_BBN")) { if (g_bbnMissing) return AE_NOT_FOUND; object->Integer.Value = g_bbn; }
    else if (!strcmp(name, "_ADR")) object->Integer.Value = (UINT64)(uintptr_t)handle << 16;
    else return AE_NOT_FOUND;
    return AE_OK;
}
ACPI_STATUS AcpiGetCurrentResources(ACPI_HANDLE h, ACPI_BUFFER* b) { b->Pointer = malloc(1); return AE_OK; }
ACPI_STATUS AcpiWalkResourceBuffer(ACPI_BUFFER* b, ACPI_WALK_RESOURCE_CALLBACK callback, void* context)
{
    for (unsigned int i = 0; i < g_resourceCount; i++) {
        ACPI_STATUS status = callback(&g_resources[i], context);
        if (ACPI_FAILURE(status)) return status;
    }
    return AE_OK;
}
ACPI_STATUS AcpiResourceToAddress64(ACPI_RESOURCE* r, ACPI_RESOURCE_ADDRESS64* out)
{
    assert(r->Type == ACPI_RESOURCE_TYPE_ADDRESS64); *out = r->Data.Address64; return AE_OK;
}
ACPI_STATUS AcpiGetDevices(char* hid, ACPI_WALK_CALLBACK cb, void* context, void** ret)
{
    return cb((void*)1, 0, context, ret);
}
ACPI_STATUS AcpiGetNextObject(ACPI_OBJECT_TYPE type, ACPI_HANDLE parent, ACPI_HANDLE previous, ACPI_HANDLE* next)
{
    if (previous) return AE_NOT_FOUND;
    *next = (void*)2; return AE_OK;
}
oserr_t AcpiPciRoute(ACPI_HANDLE bus, const struct AcpiPciQuery* query, struct AcpiPciInterrupt* result)
{
    assert(bus == (void*)2); g_routeCalls++; result->Line = 300; return OS_EOK;
}
static void clear(void)
{
    while (g_pciRoots) { struct __PciRoot* root = g_pciRoots; g_pciRoots = root->Next; __FreeRoot(root); }
    g_pciRootCount = 0; assert(!g_allocations);
}
static void reset(void)
{
    clear(); memset(g_resources, 0, sizeof(g_resources)); g_resourceCount = 2;
    g_segment = 7; g_bbn = 32; g_sta = 15; g_bbnMissing = g_failAllocation = 0;
    for (unsigned int i = 0; i < 2; i++) {
        g_resources[i].Type = ACPI_RESOURCE_TYPE_ADDRESS64;
        g_resources[i].Data.Address64.MinAddressFixed = 1;
        g_resources[i].Data.Address64.MaxAddressFixed = 1;
    }
    g_resources[0].Data.Address64.ResourceType = ACPI_BUS_NUMBER_RANGE;
    g_resources[0].Data.Address64.Address = (ACPI_ADDRESS64_ATTRIBUTE) { .Minimum = 32, .Maximum = 63, .AddressLength = 32 };
    g_resources[1].Data.Address64.ResourceType = ACPI_MEMORY_RANGE;
    g_resources[1].Data.Address64.Info.Mem.Caching = ACPI_PREFETCHABLE_MEMORY;
    g_resources[1].Data.Address64.Address = (ACPI_ADDRESS64_ATTRIBUTE) { .Minimum = 0x80000000, .Maximum = 0x800fffff,
        .AddressLength = 0x100000, .TranslationOffset = 0x100000000ULL };
}
static void rejected(void) { AcpiPciScan(); assert(g_pciRootStatus != OS_EOK); assert(!g_pciRoots); assert(!g_allocations); }
int main(void)
{
    struct AcpiPciQuery query = { .Type = AcpiPciQueryRoot };
    union AcpiPciResult result;
    reset(); AcpiPciScan(); assert(g_pciRootStatus == OS_EOK);
    assert(AcpiPciQuery(&query, &result) == OS_EOK);
    assert(result.Root.Id == 1 && result.Root.Segment == 7 && result.Root.BusStart == 32 && result.Root.BusEnd == 63);
    query.Index = 1; assert(AcpiPciQuery(&query, &result) == OS_ENOENT);
    query = (struct AcpiPciQuery) { .Type = AcpiPciQueryWindow, .RootId = 1 };
    assert(AcpiPciQuery(&query, &result) == OS_EOK);
    assert(result.Window.BusBase == 0x80000000 && result.Window.PhysicalBase == 0x180000000ULL);
    assert(result.Window.Attributes == PCI_WINDOW_PREFETCHABLE);
    query = (struct AcpiPciQuery) { .Type = AcpiPciQueryInterrupt, .RootId = 1,
        .BridgeCount = 1, .Bridges = {{2,0}}, .Address = {3,1}, .Pin = 4 };
    assert(AcpiPciQuery(&query, &result) == OS_EOK && result.Interrupt.Line == 300 && g_routeCalls == 1);
    query.Bridges[0].Function = 8; assert(AcpiPciQuery(&query, &result) == OS_EINVALPARAMS);
    query.BridgeCount = 256; assert(AcpiPciQuery(&query, &result) == OS_EINVALPARAMS);
    query.BridgeCount = 0; query.Pin = 0; assert(AcpiPciQuery(&query, &result) == OS_EINVALPARAMS);
    reset(); g_segment = 65536; rejected();
    reset(); g_bbn = 31; rejected();
    reset(); g_resources[0].Data.Address64.Address.Maximum = 256; rejected();
    reset(); g_resources[0].Data.Address64.Address.TranslationOffset = 1; rejected();
    reset(); g_resources[1].Data.Address64.Address.AddressLength++; rejected();
    reset(); g_resources[1].Data.Address64.Address.TranslationOffset = UINT64_MAX; rejected();
    reset(); g_resources[1].Data.Address64.Decode = ACPI_SUB_DECODE; rejected();
    reset(); g_resources[2] = g_resources[1]; g_resourceCount = 3; rejected();
    reset(); g_failAllocation = 1; rejected();
    reset(); g_resources[1].Data.Address64.ResourceType = ACPI_IO_RANGE;
    g_resources[1].Data.Address64.Info.Io.TranslationType = ACPI_SPARSE_TRANSLATION; rejected();
    reset(); g_bbnMissing = 1; AcpiPciScan(); assert(g_pciRootStatus == OS_EOK);
    reset(); g_sta = 0; AcpiPciScan(); assert(g_pciRootStatus == OS_EOK && !g_pciRoots);
    clear(); puts("ACPI PCI roots: PASS"); return 0;
}
