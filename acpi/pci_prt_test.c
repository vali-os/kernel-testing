#ifndef ACPI_PCI_IRQ_SOURCE
#define ACPI_PCI_IRQ_SOURCE "../../kernel/acpi/irq.c"
#endif
#include ACPI_PCI_IRQ_SOURCE
#if defined(__aarch64__)
#ifndef ARM64_PCI_IRQ_SOURCE
#define ARM64_PCI_IRQ_SOURCE "../../kernel/arch/aarch64/acpi/pci-irq.c"
#endif
#include ARM64_PCI_IRQ_SOURCE
#else
#include "../../kernel/arch/x86/components/acpi-pci.c"
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static uint8_t g_prt[256];
static size_t g_length;
static unsigned g_buffers;
void AcpiOsFree(void* p) { assert(g_buffers != 0); g_buffers--; free(p); }
ACPI_STATUS AcpiGetIrqRoutingTable(ACPI_HANDLE handle, ACPI_BUFFER* buffer)
{
    buffer->Length = g_length; buffer->Pointer = malloc(g_length);
    assert(buffer->Pointer != NULL); g_buffers++;
    memcpy(buffer->Pointer, g_prt, g_length); return AE_OK;
}
static AcpiInterruptResource_t g_irq = { .Irq = 45, .Trigger = ACPI_LEVEL_SENSITIVE,
    .Polarity = ACPI_ACTIVE_LOW, .Shareable = 1 };
static AcpiInterruptSource_t g_source = { .CurrentInterrupt = &g_irq };
static AcpiBusRoutings_t g_routings;
static AcpiDevice_t g_device = { .Routings = &g_routings };
static int g_link;
void* list_find_value(list_t* list, void* key) { return g_link ? &g_source : NULL; }
#if defined(__i386__) || defined(__amd64__)
AcpiDevice_t* AcpiDeviceLookupHandle(ACPI_HANDLE h) { return g_link ? &g_device : NULL; }
#endif
static ACPI_PCI_ROUTING_TABLE* add(unsigned int device, unsigned int function, unsigned int pin, unsigned int gsi)
{
    ACPI_PCI_ROUTING_TABLE* entry = (void*)(g_prt + g_length);
    memset(entry, 0, 32); entry->Length = 32; entry->Address = (device << 16) | function;
    entry->Pin = pin; entry->SourceIndex = gsi; g_length += 32; return entry;
}

void LogAppendMessage(enum OSSysLogLevel level, const char* format, ...)
{
    (void)level;
    (void)format;
}

#if defined(__aarch64__)
static ACPI_RESOURCE g_resources[3];
static size_t g_resourceCount;
static ACPI_STATUS g_handleStatus, g_walkStatus, g_finishStatus;
static unsigned g_lookups, g_walks, g_callbacks;

ACPI_STATUS AcpiGetHandle(ACPI_HANDLE parent, ACPI_STRING path, ACPI_HANDLE* out)
{
    // Link names must be resolved relative to the bus that supplied _PRT.
    assert(parent == (void*)1 && !strcmp(path, "L001"));
    g_lookups++;
    if (ACPI_FAILURE(g_handleStatus)) return g_handleStatus;
    *out = (void*)2;
    return AE_OK;
}

ACPI_STATUS AcpiWalkResources(ACPI_HANDLE handle, char* name,
                              ACPI_WALK_RESOURCE_CALLBACK callback, void* context)
{
    ACPI_STATUS status;

    assert(handle == (void*)2 && !strcmp(name, "_CRS"));
    g_walks++;
    if (ACPI_FAILURE(g_walkStatus)) return g_walkStatus;
    for (size_t i = 0; i < g_resourceCount; i++) {
        g_callbacks++;
        status = callback(&g_resources[i], context);
        if (ACPI_FAILURE(status)) return status;
        if (g_resources[i].Type == ACPI_RESOURCE_TYPE_END_TAG) break;
    }
    return g_finishStatus;
}

ACPI_STATUS AcpiSetCurrentResources(ACPI_HANDLE handle, ACPI_BUFFER* resources)
{
    assert(false && "Current-link query must not evaluate _SRS");
    return AE_ERROR;
}

static ACPI_PCI_ROUTING_TABLE* __ResetCurrentLink(void)
{
    ACPI_PCI_ROUTING_TABLE* entry;
    ACPI_RESOURCE_EXTENDED_IRQ* irq;

    memset(g_prt, 0, sizeof(g_prt)); g_length = 0;
    entry = add(3, 2, 3, 0); g_length += 4;
    strcpy(entry->Source, "L001");
    memset(g_resources, 0, sizeof(g_resources));
    g_resources[0].Type = ACPI_RESOURCE_TYPE_EXTENDED_IRQ;
    g_resources[0].Length = ACPI_RS_SIZE(ACPI_RESOURCE_EXTENDED_IRQ);
    irq = &g_resources[0].Data.ExtendedIrq;
    irq->ProducerConsumer = ACPI_CONSUMER;
    irq->Triggering = ACPI_LEVEL_SENSITIVE;
    irq->Polarity = ACPI_ACTIVE_HIGH;
    irq->Shareable = ACPI_EXCLUSIVE;
    irq->InterruptCount = 1;
    irq->Interrupts[0] = 36;
    g_resources[1].Type = ACPI_RESOURCE_TYPE_END_TAG;
    g_resources[1].Length = ACPI_RS_SIZE(ACPI_RESOURCE_END_TAG);
    g_resourceCount = 2;
    g_handleStatus = g_walkStatus = g_finishStatus = AE_OK;
    g_lookups = g_walks = g_callbacks = 0;
    return entry;
}

static void __CheckCurrentLink(oserr_t expected)
{
    struct AcpiPciQuery query = { .Address = {3,2}, .Pin = 4 };
    struct AcpiPciInterrupt result, before;
    ACPI_RESOURCE_EXTENDED_IRQ* irq = &g_resources[0].Data.ExtendedIrq;
    unsigned int conform = INTERRUPT_ACPICONFORM_PRESENT | INTERRUPT_ACPICONFORM_FIXED |
        INTERRUPT_ACPICONFORM_TRIGGERMODE;

    memset(&result, 0xa5, sizeof(result)); before = result;
    assert(AcpiPciRoute((void*)1, &query, &result) == expected);
    assert(g_buffers == 0);
    if (expected != OS_EOK) {
        assert(!memcmp(&result, &before, sizeof(result)));
        return;
    }
    if (irq->Shareable == ACPI_SHARED) conform |= INTERRUPT_ACPICONFORM_SHAREABLE;
    assert(result.Line == (int32_t)irq->Interrupts[0] && result.Conform == conform);
    assert(g_lookups == 1 && g_walks == 1 && g_callbacks == 2);
}

static void __CurrentLinks(void)
{
    const uint32_t lines[] = {32, 36, 1019, 0, 31, 1020, UINT32_MAX};
    const ACPI_STATUS errors[] = {AE_NOT_FOUND, AE_NO_MEMORY, AE_ERROR};
    const oserr_t expected[] = {OS_ENOTSUPPORTED, OS_EOOM, OS_EUNKNOWN};
    ACPI_RESOURCE_EXTENDED_IRQ* irq = &g_resources[0].Data.ExtendedIrq;
    ACPI_PCI_ROUTING_TABLE* entry;

    for (unsigned share = ACPI_EXCLUSIVE; share <= ACPI_SHARED; share++) {
        for (size_t i = 0; i < SIZEOF_ARRAY(lines); i++) {
            __ResetCurrentLink();
            irq->Shareable = share;
            irq->Interrupts[0] = lines[i];
            __CheckCurrentLink(i < 3 ? OS_EOK : OS_ENOTSUPPORTED);
        }
    }
    entry = __ResetCurrentLink(); entry->SourceIndex = 1;
    __CheckCurrentLink(OS_ENOTSUPPORTED);
    assert(g_lookups == 0 && g_walks == 0);

    // Every excluded attribute fails before publishing the local candidate.
    for (unsigned i = 0; i < 8; i++) {
        __ResetCurrentLink();
        switch (i) {
            case 0: irq->InterruptCount = 0; break;
            case 1: irq->InterruptCount = 2; break;
            case 2: irq->ProducerConsumer = ACPI_PRODUCER; break;
            case 3: irq->Triggering = ACPI_EDGE_SENSITIVE; break;
            case 4: irq->Polarity = ACPI_ACTIVE_LOW; break;
            case 5: irq->Shareable = 2; break;
            case 6: irq->ResourceSource.StringLength = 5; irq->ResourceSource.StringPtr = "GIC0"; break;
            case 7: g_resources[0].Type = ACPI_RESOURCE_TYPE_IRQ; break;
        }
        __CheckCurrentLink(OS_ENOTSUPPORTED);
    }
    __ResetCurrentLink(); g_resources[0].Length--;
    __CheckCurrentLink(OS_EINVALPARAMS);

    __ResetCurrentLink(); g_resourceCount = 0;
    __CheckCurrentLink(OS_ENOTSUPPORTED);
    __ResetCurrentLink(); g_resources[0] = g_resources[1]; g_resourceCount = 1;
    __CheckCurrentLink(OS_ENOTSUPPORTED);
    __ResetCurrentLink(); g_resourceCount = 1;
    __CheckCurrentLink(OS_ENOTSUPPORTED);

    // A valid first descriptor does not authorize an extra interrupt or memory
    // descriptor. The walk stops at the second descriptor in either case.
    for (unsigned i = 0; i < 2; i++) {
        __ResetCurrentLink();
        g_resources[2] = g_resources[1];
        g_resources[1] = g_resources[0];
        if (i != 0) g_resources[1].Type = ACPI_RESOURCE_TYPE_FIXED_MEMORY32;
        g_resourceCount = 3;
        __CheckCurrentLink(OS_ENOTSUPPORTED);
        assert(g_callbacks == 2);
    }
    for (size_t i = 0; i < SIZEOF_ARRAY(errors); i++) {
        __ResetCurrentLink(); g_handleStatus = errors[i];
        __CheckCurrentLink(expected[i]); assert(g_walks == 0);
        __ResetCurrentLink(); g_walkStatus = errors[i];
        __CheckCurrentLink(expected[i]); assert(g_callbacks == 0);
        __ResetCurrentLink(); g_finishStatus = errors[i];
        __CheckCurrentLink(expected[i]);
    }
    puts("ACPI PCI AArch64 current-link resources: PASS");
}
#endif

int main(void)
{
    struct AcpiPciQuery query = { .Address = {3,2}, .Pin = 4 };
    struct AcpiPciInterrupt result;
    ACPI_PCI_ROUTING_TABLE* entry;
    ACPI_BUFFER buffer;
    add(3, 0xffff, 3, 300); g_length += 4;
    assert(AcpiPciRoute((void*)1, &query, &result) == OS_EOK && result.Line == 300);
    assert((result.Conform & (INTERRUPT_ACPICONFORM_FIXED | INTERRUPT_ACPICONFORM_SHAREABLE)) != 0);
    query.Pin = 3; assert(AcpiPciRoute((void*)1, &query, &result) == OS_ENOENT); query.Pin = 4;
    g_length -= 4; add(3,2,3,301); g_length += 4;
    assert(AcpiPciRoute((void*)1, &query, &result) == OS_EOK && result.Line == 301);
    g_length -= 4; add(3,2,3,302); g_length += 4;
    assert(AcpiPciRoute((void*)1, &query, &result) == OS_EINVALPARAMS);
    memset(g_prt,0,sizeof(g_prt)); g_length = 0; entry = add(3,2,3,300); g_length += 4;
    entry->Length = 256; assert(AcpiPciRoute((void*)1, &query, &result) == OS_EINVALPARAMS);
    entry->Length = 32; entry->Pin = 4; assert(AcpiPciRoute((void*)1, &query, &result) == OS_EINVALPARAMS);
    entry->Pin = 3; entry->Address = (32 << 16); assert(AcpiPciRoute((void*)1, &query, &result) == OS_EINVALPARAMS);
    entry->Address = (3 << 16) | 8; assert(AcpiPciRoute((void*)1, &query, &result) == OS_EINVALPARAMS);
    entry->Address = (3 << 16) | 2; memset(entry->Source,'X',32 - offsetof(ACPI_PCI_ROUTING_TABLE,Source));
    assert(AcpiPciRoute((void*)1, &query, &result) == OS_EINVALPARAMS);
    entry->Source[1] = 0; assert(AcpiPciRoute((void*)1, &query, &result) == OS_ENOTSUPPORTED);
#if defined(__i386__) || defined(__amd64__)
    g_link = 1; entry->SourceIndex = 0;
    assert(AcpiPciRoute((void*)1, &query, &result) == OS_EOK && result.Line == 45);
    assert(result.Conform & INTERRUPT_ACPICONFORM_POLARITY);
    assert(!(result.Conform & INTERRUPT_ACPICONFORM_FIXED));
#endif
    buffer = (ACPI_BUFFER) { 2, g_prt }; assert(ACPI_FAILURE(__ValidatePrt(&buffer)));
#if defined(__aarch64__)
    __CurrentLinks();
#endif
    assert(g_buffers == 0);
    puts("ACPI PCI bounded _PRT and function routing: PASS"); return 0;
}
