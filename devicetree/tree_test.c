#include <devicetree.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int allocations;
static unsigned int failAt;
static unsigned int live;

void*
kmalloc(size_t size)
{
    void* result;

    if (++allocations == failAt) {
        return NULL;
    }
    result = malloc(size);
    if (result) {
        live++;
    }
    return result;
}

void
kfree(void* pointer)
{
    if (pointer) {
        live--;
        free(pointer);
    }
}

#define CHECK(condition) do { if (!(condition)) { \
    printf("line %d: %s\n", __LINE__, #condition); return 1; } } while (0)

int
main(int argc, char** argv)
{
    unsigned char blob[262144];
    FILE* file;
    size_t size;
    DeviceTree_t* tree;
    const DeviceTreeNode_t* device;
    DeviceTreeReference_t reference;
    uint64_t address;
    uint64_t length;
    unsigned int total;
    oserr_t status;

    CHECK(argc == 3);
    file = fopen(argv[1], "rb");
    CHECK(file);
    size = fread(blob, 1, sizeof(blob), file);
    fclose(file);
    status = DeviceTreeCreate(blob, size, &tree);
    CHECK(status == (oserr_t)atoi(argv[2]));
    if (status != OS_EOK) {
        CHECK(!tree && !live);
        return 0;
    }
    total = allocations;
    if (strstr(argv[1], "fixture")) {
        device = DeviceTreeFindPath(tree, "serial0:115200n8");
        CHECK(device && DeviceTreeIsCompatible(device, "arm,pl011"));
        CHECK(DeviceTreeIsEnabled(device));
        CHECK(DeviceTreeReadRegister(device, 0, &address, &length) == OS_EOK);
        CHECK(address == 0x107d001000ULL && length == 0x1000);
        CHECK(DeviceTreeReadRegister(device, 1, &address, &length) == OS_ENOENT);
        CHECK(DeviceTreeReadReference(tree, device, "clocks", "#clock-cells", 0, &reference) == OS_EOK);
        CHECK(reference.Provider->Phandle == 2 && reference.CellCount == 1 && reference.Cells[0] == 7);
        CHECK(DeviceTreeReadInterrupt(tree, device, 0, &reference) == OS_EOK);
        CHECK(reference.Provider->Phandle == 1 && reference.CellCount == 3);
        CHECK(reference.Cells[0] == 0 && reference.Cells[1] == 91 && reference.Cells[2] == 4);
        device = DeviceTreeFindPath(tree, "/disabled/device@0");
        CHECK(device && !DeviceTreeIsEnabled(device));
    }
    DeviceTreeDestroy(tree);
    CHECK(live == 0);

    // Fail every individual allocation, including a copied blob and partially
    // constructed sibling/property lists. Each failure must release ownership.
    for (failAt = 1; failAt <= total; failAt++) {
        allocations = 0;
        CHECK(DeviceTreeCreate(blob, size, &tree) == OS_EOOM);
        CHECK(!tree && !live);
    }
    return 0;
}
