// Include the implementation to test private walker contracts without exporting
// early-boot internals as kernel APIs. Use the real Vali headers and error codes.
#include "../../kernel/components/devicetree.c"
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); return 1; } } while (0)

struct Fixture {
    uint8_t Bytes[4096];
    uint32_t Used;
};

static void word(struct Fixture* f, uint32_t value)
{
    f->Bytes[f->Used++] = value >> 24;
    f->Bytes[f->Used++] = value >> 16;
    f->Bytes[f->Used++] = value >> 8;
    f->Bytes[f->Used++] = value;
}

static void data(struct Fixture* f, const void* bytes, uint32_t length)
{
    memcpy(f->Bytes + f->Used, bytes, length);
    f->Used += length;
    while (f->Used & 3) f->Bytes[f->Used++] = 0;
}

static void node(struct Fixture* f, const char* name)
{
    word(f, FDT_BEGIN_NODE);
    data(f, name, (uint32_t)strlen(name) + 1);
}

static void property(struct Fixture* f, uint32_t name, const void* value, uint32_t length)
{
    word(f, FDT_PROP);
    word(f, length);
    word(f, name);
    data(f, value, length);
}

static oserr_t walk(struct Fixture* f, const char* strings, uint32_t size,
                    struct __MemoryMapBuilder* builder)
{
    *builder = (struct __MemoryMapBuilder){.FrameIndex = -1};
    struct FdtParser context = {
        __ParseMemoryMapBeginNode, __ParseMemoryMapProperty,
        __ParseMemoryMapEndNode, builder
    };
    return FdtParseStructure(f->Bytes, f->Used, strings, size, &context);
}

static oserr_t reject_begin(void* context, const char* name, uint32_t length)
{
    (void)context; (void)name; (void)length;
    return OS_EBUFFER;
}

int main(int argc, char** argv)
{
    struct Fixture f = {0};
    struct __MemoryMapBuilder b;
    const uint8_t two[] = {0, 0, 0, 2};
    const uint8_t one[] = {0, 0, 0, 1};
    const uint8_t reg[] = {0,0,0,1, 0,0,0,0, 0,0,0,0, 0x80,0,0,0};
    const char strings[] = "#address-cells\0#size-cells\0reg\0device_type\0status\0";
    node(&f, "");
    property(&f, 0, two, 4);
    property(&f, 15, two, 4);
    node(&f, "memory@100000000");
    property(&f, 27, reg, sizeof(reg));
    property(&f, 31, "memory", 7);
    property(&f, 43, "disabled", 9);
    word(&f, FDT_END_NODE); word(&f, FDT_END_NODE); word(&f, FDT_END);
    CHECK(walk(&f, strings, sizeof(strings), &b) == OS_EOK);
    CHECK(b.FrameIndex == -1 && b.Frames[1].Role == NodeMemory);
    CHECK(b.Frames[1].ParentAddressCells == 2 && b.Frames[1].ParentSizeCells == 2);
    CHECK(b.Frames[1].AddressCells == 2 && b.Frames[1].SizeCells == 1);
    CHECK(b.Frames[1].Reg.Length == 16 && b.Frames[1].IsMemory && !b.Frames[1].Enabled);
    CHECK(FdtReadBe32(b.Frames[1].Reg.Value) == 1);

    // Every truncation of a valid structure must fail, even after a closed root.
    uint32_t fullSize = f.Used;
    for (uint32_t i = 0; i < fullSize; i++) {
        f.Used = i;
        CHECK(walk(&f, strings, sizeof(strings), &b) != OS_EOK);
    }
    f.Used = fullSize;
    struct FdtParser rejected = {reject_begin, __ParseMemoryMapProperty,
                                      __ParseMemoryMapEndNode, &b};
    CHECK(FdtParseStructure(f.Bytes, f.Used, strings, sizeof(strings), &rejected) == OS_EBUFFER);

    // Unaligned input is legal for the byte-safe reader even before normal RAM
    // attributes permit unaligned native loads.
    uint8_t unaligned[4097];
    memcpy(unaligned + 1, f.Bytes, f.Used);
    struct FdtParser context = {__ParseMemoryMapBeginNode, __ParseMemoryMapProperty,
                                     __ParseMemoryMapEndNode, &b};
    b = (struct __MemoryMapBuilder){.FrameIndex = -1};
    CHECK(FdtParseStructure(unaligned + 1, f.Used, strings, sizeof(strings), &context) == OS_EOK);

    f = (struct Fixture){0};
    node(&f, ""); node(&f, "child"); word(&f, FDT_END_NODE);
    property(&f, 0, two, 4);
    word(&f, FDT_END_NODE); word(&f, FDT_END);
    CHECK(walk(&f, strings, sizeof(strings), &b) == OS_EINVALPARAMS);

    f = (struct Fixture){0};
    node(&f, ""); property(&f, 0, two, 4);
    word(&f, FDT_END_NODE); word(&f, FDT_END);
    CHECK(walk(&f, "unterminated", 12, &b) == OS_EINVALPARAMS);
    CHECK(walk(&f, "", 0, &b) == OS_EINVALPARAMS);
    f.Bytes[12] = 0xff; // Oversized property value must not wrap the cursor.
    CHECK(walk(&f, strings, sizeof(strings), &b) == OS_EINVALPARAMS);

    f = (struct Fixture){0};
    word(&f, FDT_NOP); node(&f, ""); word(&f, FDT_NOP);
    word(&f, FDT_END_NODE); word(&f, FDT_NOP); word(&f, FDT_END);
    CHECK(walk(&f, NULL, 0, &b) == OS_EOK);
    word(&f, FDT_NOP);
    CHECK(walk(&f, NULL, 0, &b) == OS_EINVALPARAMS);

    f = (struct Fixture){0};
    node(&f, ""); word(&f, FDT_END_NODE); node(&f, "");
    word(&f, FDT_END_NODE); word(&f, FDT_END);
    CHECK(walk(&f, NULL, 0, &b) == OS_EINVALPARAMS);
    f = (struct Fixture){0}; word(&f, FDT_END_NODE); word(&f, FDT_END);
    CHECK(walk(&f, NULL, 0, &b) == OS_EINVALPARAMS);
    f = (struct Fixture){0}; node(&f, ""); word(&f, 0xdeadbeef);
    CHECK(walk(&f, NULL, 0, &b) == OS_EINVALPARAMS);

    f = (struct Fixture){0};
    node(&f, "");
    for (int i = 1; i < FDT_MAX_DEPTH; i++) node(&f, "n");
    for (int i = 0; i < FDT_MAX_DEPTH; i++) word(&f, FDT_END_NODE);
    word(&f, FDT_END);
    CHECK(walk(&f, NULL, 0, &b) == OS_EOK);
    f = (struct Fixture){0}; node(&f, "");
    for (int i = 0; i < FDT_MAX_DEPTH; i++) node(&f, "n");
    CHECK(walk(&f, NULL, 0, &b) == OS_EOVERFLOW);

    // Independent callback checks make the accumulated state inspectable
    // before EndNode reuses frames for siblings.
    b = (struct __MemoryMapBuilder){.FrameIndex = -1};
    CHECK(__ParseMemoryMapBeginNode(&b, "", 0) == OS_EOK);
    CHECK(__ParseMemoryMapProperty(&b, "#address-cells", one, 4) == OS_EOK);
    CHECK(__ParseMemoryMapProperty(&b, "#size-cells", two, 4) == OS_EOK);
    CHECK(__ParseMemoryMapBeginNode(&b, "reserved-memory", 15) == OS_EOK);
    CHECK(b.Frames[1].Role == NodeReservedMemory);
    CHECK(b.Frames[1].ParentAddressCells == 1 && b.Frames[1].AddressCells == 2);
    CHECK(__ParseMemoryMapProperty(&b, "ranges", NULL, 0) == OS_EOK);
    CHECK(b.Frames[1].Ranges.Present && !b.Frames[1].Ranges.Length);
    CHECK(__ParseMemoryMapProperty(&b, "status", "disabled", 9) == OS_EOK);
    CHECK(__ParseMemoryMapBeginNode(&b, "pool@0", 6) == OS_EOK);
    CHECK(b.Frames[2].Role == NodeReservation && !b.Frames[2].Enabled);
    CHECK(__ParseMemoryMapProperty(&b, "status", "okay", 5) == OS_EOK);
    CHECK(!b.Frames[2].Enabled);
    CHECK(__ParseMemoryMapProperty(&b, "size", two, 4) == OS_EOK);
    CHECK(__ParseMemoryMapProperty(&b, "alignment", one, 4) == OS_EOK);
    CHECK(__ParseMemoryMapProperty(&b, "alloc-ranges", reg, sizeof(reg)) == OS_EOK);
    CHECK(__ParseMemoryMapProperty(&b, "reg", reg, sizeof(reg)) == OS_EOK);
    CHECK(b.Frames[2].Reg.Present && b.Frames[2].Size.Present);
    CHECK(b.Frames[2].Alignment.Present && b.Frames[2].AllocRanges.Present);
    CHECK(__ParseMemoryMapProperty(&b, "no-map", NULL, 0) == OS_EOK);
    CHECK(__ParseMemoryMapProperty(&b, "reusable", NULL, 0) == OS_EINVALPARAMS);
    CHECK(__ParseMemoryMapProperty(&b, "reg", reg, sizeof(reg)) == OS_EINVALPARAMS);
    CHECK(__ParseMemoryMapEndNode(&b) == OS_EOK);
    CHECK(__ParseMemoryMapBeginNode(&b, "pool@1", 6) == OS_EOK);
    CHECK(!b.Frames[2].Reg.Present && !b.Frames[2].NoMap);
    CHECK(__ParseMemoryMapProperty(&b, "no-map", one, 4) == OS_EINVALPARAMS);
    CHECK(__ParseMemoryMapProperty(&b, "status", "bad", 3) == OS_EINVALPARAMS);
    CHECK(__ParseMemoryMapProperty(&b, "#size-cells", one, 3) == OS_EINVALPARAMS);

    // The public skeleton must not claim a valid map until reservation and
    // interval emission exist, even when the entire tree is well formed.
    f = (struct Fixture){0};
    uint32_t header[] = {0xd00dfeed, 72, 56, 72, 40, 17, 16, 0, 0, 16};
    for (unsigned i = 0; i < 10; i++) word(&f, header[i]);
    for (unsigned i = 0; i < 4; i++) word(&f, 0);
    node(&f, ""); word(&f, FDT_END_NODE); word(&f, FDT_END);
    uint32_t count = 99;
    CHECK(DeviceTreeBuildMemoryMap(f.Bytes, f.Used, NULL, 0, &count) == OS_ENOTSUPPORTED);
    CHECK(count == 0);
    for (uint32_t i = 0; i < f.Used; i++) {
        CHECK(DeviceTreeBuildMemoryMap(f.Bytes, i, NULL, 0, &count) != OS_EOK);
    }
    for (int i = 1; i < argc; i++) {
        static uint8_t blob[262144];
        FILE* file = fopen(argv[i], "rb");
        CHECK(file != NULL);
        size_t length = fread(blob, 1, sizeof(blob), file);
        CHECK(!ferror(file) && length < sizeof(blob));
        fclose(file);
        CHECK(DeviceTreeBuildMemoryMap(blob, (uint32_t)length, NULL, 0, &count) == OS_ENOTSUPPORTED);
        CHECK(count == 0);
    }
    puts("DTB walker and property-state tests passed");
    return 0;
}
