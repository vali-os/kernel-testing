// Selected EP0 diagnostics must work with the general bring-up dumps disabled.
#define XHCI_VERBOSE_DIAGNOSTICS 0
#define main __OriginalDmaMain
#include "xhci_dma_test.c"
#undef main

int
main(void)
{
    __TestEp0Diagnostics();
    __TestPortPowerDiagnostics();
    return 0;
}
