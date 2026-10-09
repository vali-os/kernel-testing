#include <firmware/resources.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { printf("line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

/** A small tree containing an unknown consumer and a forward provider. */
struct __ReaderFixture {
    uint8_t Bytes[2048];
    char Strings[512];
    uint32_t Cursor;
    uint32_t StringsLength;
    uint32_t ProviderPhandle;
    uint32_t ProviderCells;
    uint32_t Tail;
};

/** Copied views deliberately survive the temporary traversal stack. */
struct __ReaderResult {
    unsigned int Count;
    struct FdtNode Consumer;
};

static void
__ReaderWord(
    _Out_ uint8_t* bytes,
    _In_ uint32_t value)
{
    bytes[0] = value >> 24;
    bytes[1] = value >> 16;
    bytes[2] = value >> 8;
    bytes[3] = value;
}

static void
__ReaderToken(
    _InOut_ struct __ReaderFixture* fixture,
    _In_ uint32_t token)
{
    __ReaderWord(fixture->Bytes + fixture->Cursor, token);
    fixture->Cursor += 4;
}

static void
__ReaderNode(
    _InOut_ struct __ReaderFixture* fixture,
    _In_ const char* name)
{
    size_t length = strlen(name) + 1;

    __ReaderToken(fixture, FDT_BEGIN_NODE);
    memcpy(fixture->Bytes + fixture->Cursor, name, length);
    fixture->Cursor += (length + 3) & ~3U;
}

static uint32_t
__ReaderField(
    _InOut_ struct __ReaderFixture* fixture,
    _In_ const char* name,
    _In_ const void* value,
    _In_ uint32_t length)
{
    uint32_t offset;

    __ReaderToken(fixture, FDT_PROP);
    __ReaderToken(fixture, length);
    __ReaderToken(fixture, fixture->StringsLength);
    strcpy(fixture->Strings + fixture->StringsLength, name);
    fixture->StringsLength += strlen(name) + 1;
    offset = fixture->Cursor;
    memcpy(fixture->Bytes + offset, value, length);
    fixture->Cursor += (length + 3) & ~3U;
    return offset;
}

static size_t
__ReaderFixtureBuild(
    _Out_ struct __ReaderFixture* fixture)
{
    uint8_t cell[4];
    uint8_t references[12];
    uint32_t structureLength;

    memset(fixture, 0, sizeof(*fixture));
    fixture->Cursor = 56;
    __ReaderNode(fixture, "");
    __ReaderNode(fixture, "consumer");
    __ReaderWord(cell, 1);
    __ReaderField(fixture, "phandle", cell, 4);
    __ReaderToken(fixture, FDT_NOP);
    __ReaderField(fixture, "compatible", "example,new-controller", 23);
    __ReaderField(fixture, "vendor,opaque", "data", 5);
    // Nonzero alignment padding is not part of the borrowed property value.
    fixture->Bytes[fixture->Cursor - 1] = 0xa5;
    __ReaderWord(references, 2);
    __ReaderWord(references + 4, 0x12345678);
    __ReaderWord(references + 8, 0xabcdef01);
    __ReaderField(fixture, "vendor,links", references, sizeof(references));
    __ReaderToken(fixture, FDT_END_NODE);
    __ReaderNode(fixture, "provider");
    __ReaderWord(cell, 2);
    fixture->ProviderPhandle = __ReaderField(fixture, "phandle", cell, 4);
    fixture->ProviderCells = __ReaderField(fixture, "#vendor-cells", cell, 4);
    __ReaderToken(fixture, FDT_END_NODE);
    __ReaderToken(fixture, FDT_END_NODE);
    fixture->Tail = fixture->Cursor;
    __ReaderToken(fixture, FDT_END);
    structureLength = fixture->Cursor - 56;
    memcpy(fixture->Bytes + fixture->Cursor, fixture->Strings, fixture->StringsLength);
    __ReaderWord(fixture->Bytes, 0xd00dfeed);
    __ReaderWord(fixture->Bytes + 4, fixture->Cursor + fixture->StringsLength);
    __ReaderWord(fixture->Bytes + 8, 56);
    __ReaderWord(fixture->Bytes + 12, fixture->Cursor);
    __ReaderWord(fixture->Bytes + 16, 40);
    __ReaderWord(fixture->Bytes + 20, 17);
    __ReaderWord(fixture->Bytes + 24, 16);
    __ReaderWord(fixture->Bytes + 32, fixture->StringsLength);
    __ReaderWord(fixture->Bytes + 36, structureLength);
    return fixture->Cursor + fixture->StringsLength;
}

static void
__ReaderCollect(
    _In_ const struct FdtNode* nodes,
    _In_ int depth,
    _InOut_ void* context)
{
    struct __ReaderResult* result = context;

    result->Count++;
    if (!strcmp(nodes[depth].Name, "consumer")) {
        result->Consumer = nodes[depth];
    }
}

int
main(void)
{
    struct __ReaderFixture fixture;
    struct __ReaderResult result = { 0 };
    struct FdtNode provider;
    struct FdtResources resources;
    const uint8_t* value;
    const uint8_t* arguments;
    uint32_t length;
    uint32_t offset;
    uint32_t scalar = 99;
    size_t size = __ReaderFixtureBuild(&fixture);
    size_t i;

    CHECK(FdtWalkNodes(fixture.Bytes, size, __ReaderCollect, &result) == OS_EOK);
    CHECK(result.Count == 3);
    CHECK(FdtCompatible(&result.Consumer, "example,new-controller"));
    value = FdtProperty(&result.Consumer, "vendor,opaque", &length);
    CHECK(length == 5 && !memcmp(value, "data", 5));
    CHECK(value >= fixture.Bytes && value + length <= fixture.Bytes + size);
    CHECK(FdtProperty(&result.Consumer, "absent", &length) == NULL && !length);
    CHECK(FdtScalar(&result.Consumer, "vendor,opaque", &scalar) == OS_EINVALPARAMS);
    CHECK(scalar == 99);
    CHECK(FdtFindNode(fixture.Bytes, size, 2, &provider) == OS_EOK);
    CHECK(!strcmp(provider.Name, "provider"));
    CHECK(FdtFindNode(fixture.Bytes, size, 3, &provider) == OS_ENOENT);
    CHECK(provider.Phandle == 2);

    value = FdtProperty(&result.Consumer, "vendor,links", &length);
    offset = 0;
    CHECK(FdtNextReference(fixture.Bytes, size, value, length, "#vendor-cells",
        0, &offset, &resources, &arguments) == OS_EOK);
    CHECK(offset == length && resources.Phandle == 2 && arguments == value + 4);
    CHECK(FdtReadBe32(arguments) == 0x12345678 && FdtReadBe32(arguments + 4) == 0xabcdef01);
    offset = 0;
    CHECK(FdtNextReference(fixture.Bytes, size, value, length - 4, "#vendor-cells",
        0, &offset, &resources, &arguments) == OS_EINVALPARAMS);
    CHECK(offset == 0 && resources.Phandle == 2 && arguments == value + 4);
    __ReaderWord(fixture.Bytes + fixture.ProviderCells, 17);
    CHECK(FdtNextReference(fixture.Bytes, size, value, length, "#vendor-cells",
        0, &offset, &resources, &arguments) == OS_EINVALPARAMS);
    __ReaderWord(fixture.Bytes + fixture.ProviderCells, 2);
    __ReaderWord(fixture.Bytes + fixture.ProviderPhandle, 1);
    CHECK(FdtFindNode(fixture.Bytes, size, 1, &provider) == OS_EINVALPARAMS);
    __ReaderWord(fixture.Bytes + fixture.ProviderPhandle, 2);

    for (i = 0; i < size; i++) {
        result.Count = 0;
        CHECK(FdtWalkNodes(fixture.Bytes, i, __ReaderCollect, &result) != OS_EOK);
        CHECK(result.Count == 0);
    }
    // Even a valid consumer/provider prefix cannot be published without FDT_END.
    __ReaderWord(fixture.Bytes + fixture.Tail, FDT_NOP);
    result.Count = 0;
    CHECK(FdtWalkNodes(fixture.Bytes, size, __ReaderCollect, &result) == OS_EINVALPARAMS);
    CHECK(result.Count == 0);
    CHECK(FdtFindNode(fixture.Bytes, size, 2, &provider) == OS_EINVALPARAMS);
    puts("Firmware reader: unknown bindings, borrowed views, forward providers and validation before callbacks passed");
    return 0;
}
