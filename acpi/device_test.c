// Reuse the page-map backend to verify the explicit device path separately.
#define main __MappingTestMain
#include "mapping_test.c"
#undef main
#include "../../kernel/arch/aarch64/common/device.h"

int main(void)
{
    unsigned int before;
    g_pages[0] = MAPPING_COMMIT;
    g_pages[1] = MAPPING_COMMIT | MAPPING_NOCACHE;
    assert(Arm64MapDevice(0x9000018, 4) == 0x40000018);
    assert(g_options.PhysicalStart == 0x9000000 && g_options.Length == 4096);
    assert(g_options.Flags == (MAPPING_COMMIT | MAPPING_PERSISTENT | MAPPING_DEVICE | MAPPING_NOCACHE));
    before = g_maps;
    assert(!Arm64MapDevice(0x10000, 4)); // Cached RAM.
    assert(!Arm64MapDevice(0x11000, 4)); // Uncached RAM is still Normal, not Device.
    assert(!Arm64MapDevice(0xfffc, 8)); // Last byte crosses into RAM.
    assert(!Arm64MapDevice(UINT64_MAX - 8, 16));
    assert(!Arm64MapDevice(1ULL << 48, 4));
    assert(!Arm64MapDevice(0x9000000, 0));
    g_failLookup = 1;
    assert(!Arm64MapDevice(0x9000000, 4));
    g_failLookup = 0;
    assert(g_maps == before);
    assert(Arm64MapDevice(1ULL << 40, 4)); // MMIO beyond the RAM identity window.
    g_failMap = 1;
    assert(!Arm64MapDevice(0x9000000, 4));
    puts("ARM64 Device mapping and RAM alias rejection: PASS");
    return 0;
}
