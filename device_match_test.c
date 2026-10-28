#include "../services/deviced/core/match.c"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

void SystemDebug(enum OSSysLogLevel level, const char* format, ...) { (void)level; (void)format; }
int main(void)
{
    struct DriverConfiguration driver = { .Class = 0xc0003, .Subclass = 0x300000 };
    struct DriverCompatible compatible = { .Name = "generic-xhci" };
    struct DriverIdentification device = { .Class = 0xc0003, .Subclass = 0x300000 };
    ELEMENT_INIT(&compatible.ListHeader, 0, &compatible);
    driver.Compatibles.head = &compatible.ListHeader;
    driver.Compatibles.count = 1;
    CHECK(DmDriverMatchScore(&driver, &device) == 2);
    device.IsPlatform = 1;
    device.Compatibles = "generic-xhci";
    device.CompatibleLength = sizeof("generic-xhci");
    CHECK(DmDriverMatchScore(&driver, &device) == 1);
    device.CompatibleLength--;
    CHECK(DmDriverMatchScore(&driver, &device) == 0);
    compatible.Name = "snps,dwc3";
    device.Compatibles = "test,specific-usb\0snps,dwc3";
    device.CompatibleLength = sizeof("test,specific-usb\0snps,dwc3");
    CHECK(DmDriverMatchScore(&driver, &device) == 2);
    device.IsPlatform = 0;
    device.Class = device.Subclass = 0;
    CHECK(DmDriverMatchScore(&driver, &device) == 0);
    driver.Class = driver.Subclass = 0;
    CHECK(DmDriverMatchScore(&driver, &device) == 0);
    puts("Device matching: one driver supports explicit PCI and firmware identities without zero-field fallback");
    return 0;
}
