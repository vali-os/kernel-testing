#include "../../services/deviced/bus/pci/interrupts.c"
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>

static unsigned int g_queries;
static oserr_t g_status;
void SystemDebug(enum OSSysLogLevel level, const char* format, ...) { (void)level; (void)format; }
oserr_t AcpiQueryPci(const struct AcpiPciQuery* query, union AcpiPciResult* result)
{
    assert(query->Type == AcpiPciQueryInterrupt && query->RootId == 42);
    assert(query->BridgeCount == 2 - g_queries);
    if (g_queries == 0) {
        assert(query->Address.Device == 5 && query->Address.Function == 1 && query->Pin == 1);
        assert(query->Bridges[0].Device == 2 && query->Bridges[1].Device == 3);
    } else if (g_queries == 1) {
        assert(query->Address.Device == 3 && query->Pin == 2 && query->Bridges[0].Device == 2);
    } else {
        assert(query->Address.Device == 2 && query->Pin == 1);
    }
    g_queries++;
    if (g_status != OS_EOK) return g_status;
    if (query->BridgeCount) return OS_ENOENT;
    result->Interrupt.Line = 300; result->Interrupt.Conform = INTERRUPT_ACPICONFORM_FIXED;
    return OS_EOK;
}
int main(void)
{
    struct PciHostOperations operations = {0};
    PciHost_t host = { .AcpiRootId = 42, .Operations = &operations };
    PciDevice_t root = { .Host = &host };
    PciDevice_t first = { .Host = &host, .Parent = &root, .Slot = 2 };
    PciDevice_t second = { .Host = &host, .Parent = &first, .Bus = 32, .Slot = 3 };
    PciDevice_t endpoint = { .Host = &host, .Parent = &second, .Bus = 63, .Slot = 5, .Function = 1 };
    PciNativeHeader_t header = { .InterruptLine = 0xaa, .InterruptPin = 1 };
    host.RootDevice = &root; endpoint.Header = &header;
    PciResolveInterruptLineAndPin(&second,63,5,1,&endpoint);
    assert(g_queries == 3 && endpoint.InterruptLine == 300 && header.InterruptLine == 0xaa);
    g_queries = 0; g_status = OS_ENOENT;
    PciResolveInterruptLineAndPin(&second,63,5,1,&endpoint);
    assert(g_queries == 3 && endpoint.InterruptLine == INTERRUPT_NONE);
    g_queries = 0; g_status = OS_ENOTSUPPORTED;
    PciResolveInterruptLineAndPin(&second,63,5,1,&endpoint);
    assert(g_queries == 1 && endpoint.InterruptLine == INTERRUPT_NONE);
    g_queries = 0; header.InterruptPin = 5;
    PciResolveInterruptLineAndPin(&second,63,5,1,&endpoint);
    assert(!g_queries && endpoint.InterruptLine == INTERRUPT_NONE);
    puts("deviced: root IDs, two-bridge swizzling, full GSI and no stale-line fallback: PASS"); return 0;
}
