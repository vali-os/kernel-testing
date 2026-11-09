#include "../../kernel/acpi/irq.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static uint8_t g_prt[256];
static size_t g_length;
void AcpiOsFree(void* p) { free(p); }
ACPI_STATUS AcpiGetIrqRoutingTable(ACPI_HANDLE handle, ACPI_BUFFER* buffer)
{
    buffer->Length = g_length; buffer->Pointer = malloc(g_length);
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
    puts("ACPI PCI bounded _PRT and function routing: PASS"); return 0;
}
