// Test the firmware API directly, before platform enumeration can filter nodes.
static void
__PlatformCell(
    _Out_ uint8_t* bytes,
    _In_  uint32_t value)
{
    bytes[0] = value >> 24;
    bytes[1] = value >> 16;
    bytes[2] = value >> 8;
    bytes[3] = value;
}

static void
__ExpectPlatformDecodeFailure(
    _In_ const struct FdtTree*      tree,
    _In_ const struct FdtResources* nodes,
    _In_ int                       depth,
    _In_ oserr_t                   expected)
{
    PlatformDevice_t device;
    PlatformDevice_t saved;

    memset(&device, 0xa5, sizeof(device));
    saved = device;
    CHECK(FdtDescribePlatformDevice(tree, nodes, depth, &device) == expected);
    CHECK(!memcmp(&device, &saved, sizeof(device)));
}

static void
__CheckPlatformDecoder(
    _In_    const struct FdtResources* nodes,
    _In_    int                       depth,
    _InOut_ void*                     context)
{
    struct FdtTree* tree = context;
    struct FdtResources* path;
    struct FdtResources* node;
    PlatformDevice_t device;
    uint8_t cells[16 * (PLATFORM_DEVICE_MAX_INTERRUPTS + 1)] = { 0 };
    unsigned int i;

    if (strcmp(nodes[depth].Name, "device@120")) {
        return;
    }
    CHECK(FdtDescribePlatformDevice(tree, nodes, depth, &device) == OS_EOK);
    CHECK(PlatformDeviceValidate(&device));
    CHECK(device.Pending == 0 && device.Base.Identification.Description == NULL);
    CHECK(device.RegisterCount == 2 && device.InterruptCount == 2);
    CHECK(device.Registers[0].Base == 0x100000120ULL && device.Registers[0].Length == 0x20);
    CHECK(device.Registers[1].Base == 0x100000200ULL && device.Registers[1].Length == 0x40);
    CHECK(device.Interrupts[0].Controller == 1 && device.Interrupts[0].Number == 72);
    CHECK(device.Interrupts[0].Type == 4 && device.Interrupts[1].Type == 1);

    path = malloc((depth + 1) * sizeof(*path));
    CHECK(path != NULL);
    memcpy(path, nodes, (depth + 1) * sizeof(*path));
    node = &path[depth];

    // Reject incomplete entries, overflow and extents crossing a bus window.
    node->RegLength--;
    __ExpectPlatformDecodeFailure(tree, path, depth, OS_EINVALPARAMS);
    *node = nodes[depth];
    node->Reg = cells;
    node->RegLength = 8;
    __ExpectPlatformDecodeFailure(tree, path, depth, OS_EINVALPARAMS);
    __PlatformCell(cells, 0xfff0);
    __PlatformCell(cells + 4, 0x20);
    __ExpectPlatformDecodeFailure(tree, path, depth, OS_ENOTSUPPORTED);
    node->ParentAddressCells = 2;
    node->ParentSizeCells = 2;
    node->RegLength = 16;
    __PlatformCell(cells, UINT32_MAX);
    __PlatformCell(cells + 4, 0xfffffff0);
    __PlatformCell(cells + 8, 0);
    __PlatformCell(cells + 12, 0x20);
    __ExpectPlatformDecodeFailure(tree, path, depth, OS_EINVALPARAMS);
    *node = nodes[depth];
    node->RegLength = (PLATFORM_DEVICE_MAX_REGISTERS + 1) * 8;
    __ExpectPlatformDecodeFailure(tree, path, depth, OS_ENOTSUPPORTED);

    // Extended references take precedence and validate the provider's tuple size.
    *node = nodes[depth];
    node->InterruptsExtended = cells;
    node->InterruptsExtendedLength = 16;
    __PlatformCell(cells, 1);
    __PlatformCell(cells + 4, 0);
    __PlatformCell(cells + 8, 50);
    __PlatformCell(cells + 12, 4);
    CHECK(FdtDescribePlatformDevice(tree, path, depth, &device) == OS_EOK);
    CHECK(device.InterruptCount == 1 && device.Interrupts[0].Number == 82);
    node->InterruptsExtendedLength = 12;
    __ExpectPlatformDecodeFailure(tree, path, depth, OS_EINVALPARAMS);
    node->InterruptsExtendedLength = 16;
    __PlatformCell(cells, 999);
    __ExpectPlatformDecodeFailure(tree, path, depth, OS_ENOENT);
    __PlatformCell(cells, 1);
    __PlatformCell(cells + 4, 1);
    __ExpectPlatformDecodeFailure(tree, path, depth, OS_ENOTSUPPORTED);
    __PlatformCell(cells + 4, 0);
    __PlatformCell(cells + 12, 2);
    __ExpectPlatformDecodeFailure(tree, path, depth, OS_ENOTSUPPORTED);
    __PlatformCell(cells + 12, 4);
    for (i = 1; i <= PLATFORM_DEVICE_MAX_INTERRUPTS; i++) {
        memcpy(cells + i * 16, cells, 16);
    }
    node->InterruptsExtendedLength = sizeof(cells);
    __ExpectPlatformDecodeFailure(tree, path, depth, OS_ENOTSUPPORTED);

    *node = nodes[depth];
    node->InterruptParent = 0;
    __ExpectPlatformDecodeFailure(tree, path, depth, OS_ENOTSUPPORTED);
    *node = nodes[depth];
    node->CompatibleLength = PLATFORM_DEVICE_MAX_COMPATIBLES + 1;
    __ExpectPlatformDecodeFailure(tree, path, depth, OS_ENOTSUPPORTED);
    *node = nodes[depth];
    node->AncestorMalformed = 1;
    __ExpectPlatformDecodeFailure(tree, path, depth, OS_EINVALPARAMS);
    *node = nodes[depth];
    node->Disabled = 1;
    __ExpectPlatformDecodeFailure(tree, path, depth, OS_ENOENT);
    __ExpectPlatformDecodeFailure(tree, path, 0, OS_EINVALPARAMS);

    // Devices may have no registers or interrupts. This grants no readiness.
    *node = nodes[depth];
    node->Reg = NULL;
    node->Interrupts = NULL;
    CHECK(FdtDescribePlatformDevice(tree, path, depth, &device) == OS_EOK);
    CHECK(device.RegisterCount == 0 && device.InterruptCount == 0 && device.Pending == 0);
    free(path);
}
