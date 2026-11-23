/*
 * The firmware node request (OSIOCTLREQUEST_FIRMWARE_NODE): deviced's packed
 * node description through the real request path, the libddk wrapper that
 * reads it in chunks, the checks of DeviceFirmwareNodeParse against broken
 * descriptions, and the accessors. Uses described@6000 and huge@6100 of
 * platform_fixtures.py.
 */

#include <ddk/devices/firmware-node.h>

// Counted by the allocation fakes in platform_bus_test.c.
static int          g_liveAllocations;
static unsigned int g_nodeAnswerChunk;
static int          g_nodeAnswerMode;

enum __NodeAnswer {
    NodeAnswerUnchanged,
    NodeAnswerLengthChanges,
    NodeAnswerEmptyChunk,
    NodeAnswerOverlongChunk,
    NodeAnswerBadVersion,
    NodeAnswerLaterRequestRefused
};

// Damages the second chunk of an answer, or the header of the first, on its
// way back to the libddk wrapper.
static void
__AnswerNode(enum OSIOCtlRequest request, void* buffer, size_t length)
{
    struct DeviceFirmwareNodeRequest* body = buffer;

    CHECK(request == OSIOCTLREQUEST_FIRMWARE_NODE && length == sizeof(*body));
    g_nodeAnswerChunk++;

    // The next request comes from another principal, so deviced refuses it
    // after the description was allocated.
    if (g_nodeAnswerMode == NodeAnswerLaterRequestRefused && g_nodeAnswerChunk == 1) {
        g_ioctlPrincipal = 2;
    }
    if (g_nodeAnswerMode == NodeAnswerBadVersion && body->Offset == 0) {
        body->Data[0] ^= 0xff;
    }
    if (g_nodeAnswerChunk != 2) {
        return;
    }
    if (g_nodeAnswerMode == NodeAnswerLengthChanges) {
        body->Length += 4;
    } else if (g_nodeAnswerMode == NodeAnswerEmptyChunk) {
        body->Count = 0;
    } else if (g_nodeAnswerMode == NodeAnswerOverlongChunk) {
        body->Count = body->Length;
    }
}

static oserr_t
__NodeRaw(struct PlatformEntry* entry, struct DeviceFirmwareNodeRequest* request, size_t length)
{
    return DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_FIRMWARE_NODE, request, length);
}

// What deviced accepts and refuses in a request, before any wrapper.
static void
__FirmwareNodeRequests(struct PlatformEntry* described, struct PlatformEntry* huge)
{
    struct DeviceFirmwareNodeRequest request = { 0 };
    uint32_t                         length;

    CHECK(__NodeRaw(described, &request, sizeof(request)) == OS_EOK);
    CHECK(request.Length > 3000 && request.Length <= DEVICE_FIRMWARE_NODE_MAX_LENGTH);
    CHECK(request.Count == DEVICE_FIRMWARE_NODE_CHUNK_SIZE);
    length = request.Length;

    // The last chunk is short, the end itself is empty, and past it is refused.
    request = (struct DeviceFirmwareNodeRequest) { .Offset = length - 5 };
    CHECK(__NodeRaw(described, &request, sizeof(request)) == OS_EOK && request.Count == 5);
    CHECK(request.Data[5] == 0 && request.Data[DEVICE_FIRMWARE_NODE_CHUNK_SIZE - 1] == 0);
    request = (struct DeviceFirmwareNodeRequest) { .Offset = length };
    CHECK(__NodeRaw(described, &request, sizeof(request)) == OS_EOK && request.Count == 0);
    request = (struct DeviceFirmwareNodeRequest) { .Offset = length + 1 };
    CHECK(__NodeRaw(described, &request, sizeof(request)) == OS_EINVALPARAMS);

    // A wrong size or a reserved value is refused.
    request = (struct DeviceFirmwareNodeRequest) { .Reserved = 1 };
    CHECK(__NodeRaw(described, &request, sizeof(request)) == OS_EINVALPARAMS);
    request = (struct DeviceFirmwareNodeRequest) { 0 };
    CHECK(__NodeRaw(described, &request, sizeof(request) - 1) == OS_EINVALPARAMS);
    CHECK(DmHandleIoctl(1, described->DeviceId, OSIOCTLREQUEST_FIRMWARE_NODE, NULL, 0) == OS_EINVALPARAMS);

    // Only the assigned driver may ask, before preparation like any driver.
    CHECK(DmHandleIoctl(2, described->DeviceId, OSIOCTLREQUEST_FIRMWARE_NODE,
        &request, sizeof(request)) == OS_EPERMISSIONS);

    // A node whose description is larger than the limit is never handed out.
    CHECK(__NodeRaw(huge, &request, sizeof(request)) == OS_ENOTSUPPORTED);
}

// The wrapper reads all chunks, and the accessors find what the node holds.
static void
__FirmwareNodeRead(struct PlatformEntry* described, struct PlatformEntry* huge)
{
    DeviceFirmwareNode_t      node;
    DeviceFirmwareNode_t      saved;
    DeviceFirmwareNodeChild_t child;
    const uint8_t*            big;
    unsigned int              ioctls;
    uint32_t                  length;
    uint32_t                  value;
    uint32_t                  index;
    uint32_t                  count;
    int                       allocations;

    CHECK(DeviceFirmwareNodeRead(described->DeviceId, NULL) == OS_EINVALPARAMS);
    ioctls = g_ioctls;
    CHECK(DeviceFirmwareNodeRead(described->DeviceId, &node) == OS_EOK);
    CHECK(g_ioctls - ioctls == (node.Length + DEVICE_FIRMWARE_NODE_CHUNK_SIZE - 1) / DEVICE_FIRMWARE_NODE_CHUNK_SIZE);
    CHECK(g_ioctls - ioctls >= 4);
    CHECK(node.ParentAddressCells == 1 && node.ParentSizeCells == 1);
    CHECK(node.PropertyCount == 6 && node.ChildCount == 3);

    big = DeviceFirmwareNodeProperty(&node, "big", &length);
    CHECK(big != NULL && length == 3000);
    for (uint32_t i = 0; i < length; i++) {
        CHECK(big[i] == ((i * 7) & 0xff));
    }
    CHECK(DeviceFirmwareNodeProperty(&node, "compatible", NULL) != NULL);
    CHECK(DeviceFirmwareNodeProperty(&node, "missing", &length) == NULL);

    CHECK(DeviceFirmwareNodeFlag(&node, "flag", &value) == OS_EOK && value == 1);
    CHECK(DeviceFirmwareNodeFlag(&node, "missing", &value) == OS_EOK && value == 0);
    CHECK(DeviceFirmwareNodeFlag(&node, "number", &value) == OS_EINVALPARAMS && value == 0);
    CHECK(DeviceFirmwareNodeU32(&node, "number", &value) == OS_EOK && value == 7);
    CHECK(DeviceFirmwareNodeU32(&node, "missing", &value) == OS_ENOENT && value == 7);
    CHECK(DeviceFirmwareNodeU32(&node, "reg", &value) == OS_EINVALPARAMS && value == 7);
    CHECK(DeviceFirmwareNodeStringIndex(&node, "names", "two", &index, &count) == OS_EOK);
    CHECK(index == 1 && count == 2);
    CHECK(DeviceFirmwareNodeStringIndex(&node, "names", "three", &index, &count) == OS_ENOENT);
    CHECK(index == UINT32_MAX && count == 2);
    CHECK(DeviceFirmwareNodeStringIndex(&node, "big", "x", &index, &count) == OS_EINVALPARAMS);
    CHECK(DeviceFirmwareNodeStringIndex(&node, "missing", "x", &index, &count) == OS_ENOENT);

    // Children in firmware order, with their state.
    CHECK(DeviceFirmwareNodeChild(&node, 0, &child) == OS_EOK && !strcmp(child.Name, "fn@1"));
    CHECK(child.Flags == (DEVICE_FIRMWARE_CHILD_ENABLED | DEVICE_FIRMWARE_CHILD_HAS_REG));
    CHECK(DeviceFirmwareNodeChild(&node, 1, &child) == OS_EOK && !strcmp(child.Name, "fn@2"));
    CHECK(child.Flags == DEVICE_FIRMWARE_CHILD_HAS_REG);
    CHECK(DeviceFirmwareNodeChild(&node, 2, &child) == OS_EOK && !strcmp(child.Name, "plain"));
    CHECK(child.Flags == DEVICE_FIRMWARE_CHILD_ENABLED);
    CHECK(DeviceFirmwareNodeChild(&node, 3, &child) == OS_ENOENT);
    DeviceFirmwareNodeDestroy(&node);
    CHECK(node.Data == NULL && node.Length == 0);
    DeviceFirmwareNodeDestroy(&node);
    DeviceFirmwareNodeDestroy(NULL);

    // Broken answers never reach the driver, and its output stays unchanged.
    // A request refused after the first chunk frees what was read so far.
    memset(&node, 0xa5, sizeof(node));
    saved = node;
    g_ioctlAnswer = __AnswerNode;
    for (int mode = NodeAnswerLengthChanges; mode <= NodeAnswerBadVersion; mode++) {
        g_nodeAnswerMode = mode;
        g_nodeAnswerChunk = 0;
        CHECK(DeviceFirmwareNodeRead(described->DeviceId, &node) == OS_EPROTOCOL);
        CHECK(!memcmp(&node, &saved, sizeof(node)));
    }
    g_nodeAnswerMode = NodeAnswerLaterRequestRefused;
    g_nodeAnswerChunk = 0;
    allocations = g_liveAllocations;
    CHECK(DeviceFirmwareNodeRead(described->DeviceId, &node) == OS_EPERMISSIONS);
    CHECK(g_nodeAnswerChunk == 1 && !memcmp(&node, &saved, sizeof(node)));
    CHECK(g_liveAllocations == allocations);
    g_ioctlPrincipal = 1;
    g_ioctlAnswer = NULL;

    // deviced's refusals pass through unchanged.
    CHECK(DeviceFirmwareNodeRead(huge->DeviceId, &node) == OS_ENOTSUPPORTED);
    g_ioctlPrincipal = 2;
    CHECK(DeviceFirmwareNodeRead(described->DeviceId, &node) == OS_EPERMISSIONS);
    g_ioctlPrincipal = 1;
    CHECK(!memcmp(&node, &saved, sizeof(node)));
}

/**
 * @brief Builds descriptions by hand, so the parser can be fed broken ones.
 */
struct __NodeBuilder {
    uint8_t  Data[256];
    uint32_t Length;
};

static void
__BuildU32(struct __NodeBuilder* builder, uint32_t value)
{
    memcpy(&builder->Data[builder->Length], &value, sizeof(value));
    builder->Length += sizeof(value);
}

static void
__BuildBytes(struct __NodeBuilder* builder, const void* data, uint32_t length, uint32_t padded)
{
    memset(&builder->Data[builder->Length], 0, padded);
    memcpy(&builder->Data[builder->Length], data, length);
    builder->Length += padded;
}

// A valid description: header, property "a" = 0x01020304, child "c".
static void
__BuildValid(struct __NodeBuilder* builder, uint32_t version, uint32_t properties, uint32_t children)
{
    static const uint8_t value[] = { 1, 2, 3, 4 };

    memset(builder, 0, sizeof(*builder));
    __BuildU32(builder, version);
    __BuildU32(builder, 0);
    __BuildU32(builder, 1);
    __BuildU32(builder, 1);
    __BuildU32(builder, properties);
    __BuildU32(builder, children);
    __BuildU32(builder, 2);
    __BuildU32(builder, sizeof(value));
    __BuildBytes(builder, "a", 2, 4);
    __BuildBytes(builder, value, sizeof(value), 4);
    __BuildU32(builder, 2);
    __BuildU32(builder, DEVICE_FIRMWARE_CHILD_ENABLED);
    __BuildBytes(builder, "c", 2, 4);
    memcpy(&builder->Data[4], &builder->Length, sizeof(builder->Length));
}

static void
__FirmwareNodeParse(void)
{
    struct __NodeBuilder builder;
    DeviceFirmwareNode_t node;
    DeviceFirmwareNode_t saved;
    uint32_t             value;
    uint32_t             offset;

    __BuildValid(&builder, DEVICE_FIRMWARE_NODE_VERSION, 1, 1);
    CHECK(DeviceFirmwareNodeParse(builder.Data, builder.Length, &node) == OS_EOK);
    CHECK(DeviceFirmwareNodeU32(&node, "a", &value) == OS_EOK && value == 0x01020304);
    CHECK(DeviceFirmwareNodeParse(NULL, builder.Length, &node) == OS_EINVALPARAMS);
    CHECK(DeviceFirmwareNodeParse(builder.Data, builder.Length, NULL) == OS_EINVALPARAMS);

    memset(&node, 0xa5, sizeof(node));
    saved = node;

    // Version, length and counts.
    __BuildValid(&builder, DEVICE_FIRMWARE_NODE_VERSION + 1, 1, 1);
    CHECK(DeviceFirmwareNodeParse(builder.Data, builder.Length, &node) == OS_EPROTOCOL);
    __BuildValid(&builder, DEVICE_FIRMWARE_NODE_VERSION, 1, 1);
    CHECK(DeviceFirmwareNodeParse(builder.Data, builder.Length - 4, &node) == OS_EPROTOCOL);
    CHECK(DeviceFirmwareNodeParse(builder.Data, 8, &node) == OS_EPROTOCOL);
    CHECK(DeviceFirmwareNodeParse(builder.Data, DEVICE_FIRMWARE_NODE_MAX_LENGTH + 1, &node) == OS_EPROTOCOL);
    __BuildValid(&builder, DEVICE_FIRMWARE_NODE_VERSION, 2, 1);
    CHECK(DeviceFirmwareNodeParse(builder.Data, builder.Length, &node) == OS_EPROTOCOL);
    __BuildValid(&builder, DEVICE_FIRMWARE_NODE_VERSION, 1, 0);
    CHECK(DeviceFirmwareNodeParse(builder.Data, builder.Length, &node) == OS_EPROTOCOL);
    __BuildValid(&builder, DEVICE_FIRMWARE_NODE_VERSION, 0x40000000, 1);
    CHECK(DeviceFirmwareNodeParse(builder.Data, builder.Length, &node) == OS_EPROTOCOL);

    // A property name without its terminator, an empty name, and lengths
    // that point past the end.
    offset = sizeof(struct DeviceFirmwareNodeHeader);
    __BuildValid(&builder, DEVICE_FIRMWARE_NODE_VERSION, 1, 1);
    builder.Data[offset + 9] = 'x';
    CHECK(DeviceFirmwareNodeParse(builder.Data, builder.Length, &node) == OS_EPROTOCOL);
    __BuildValid(&builder, DEVICE_FIRMWARE_NODE_VERSION, 1, 1);
    builder.Data[offset + 8] = 0;
    CHECK(DeviceFirmwareNodeParse(builder.Data, builder.Length, &node) == OS_EPROTOCOL);
    __BuildValid(&builder, DEVICE_FIRMWARE_NODE_VERSION, 1, 1);
    value = 0xFFFFFFFFU;
    memcpy(&builder.Data[offset + 4], &value, sizeof(value));
    CHECK(DeviceFirmwareNodeParse(builder.Data, builder.Length, &node) == OS_EPROTOCOL);
    __BuildValid(&builder, DEVICE_FIRMWARE_NODE_VERSION, 1, 1);
    memcpy(&builder.Data[offset], &value, sizeof(value));
    CHECK(DeviceFirmwareNodeParse(builder.Data, builder.Length, &node) == OS_EPROTOCOL);
    value = 1;
    memcpy(&builder.Data[offset], &value, sizeof(value));
    CHECK(DeviceFirmwareNodeParse(builder.Data, builder.Length, &node) == OS_EPROTOCOL);

    // Bytes after the last record.
    __BuildValid(&builder, DEVICE_FIRMWARE_NODE_VERSION, 1, 1);
    __BuildU32(&builder, 0);
    memcpy(&builder.Data[4], &builder.Length, sizeof(builder.Length));
    CHECK(DeviceFirmwareNodeParse(builder.Data, builder.Length, &node) == OS_EPROTOCOL);

    // A property name that appears twice: the child record is rewritten as a
    // second property "a" with an empty value.
    __BuildValid(&builder, DEVICE_FIRMWARE_NODE_VERSION, 2, 0);
    value = 0;
    memcpy(&builder.Data[offset + 16 + 4], &value, sizeof(value));
    builder.Data[offset + 16 + 8] = 'a';
    CHECK(DeviceFirmwareNodeParse(builder.Data, builder.Length, &node) == OS_EPROTOCOL);
    builder.Data[offset + 16 + 8] = 'b';
    CHECK(DeviceFirmwareNodeParse(builder.Data, builder.Length, &node) == OS_EOK);
    CHECK(DeviceFirmwareNodeFlag(&node, "b", &value) == OS_EOK && value == 1);
    memset(&node, 0xa5, sizeof(node));
    CHECK(!memcmp(&node, &saved, sizeof(node)));
}

static void
__FirmwareNodeSynthetic(struct PlatformBus* bus)
{
    struct PlatformEntry* described = __FindCompatible(bus, "test,described");
    struct PlatformEntry* huge = __FindCompatible(bus, "test,huge");

    CHECK(described != NULL && huge != NULL && described->Activation == NULL);
    __FirmwareNodeRequests(described, huge);
    __FirmwareNodeRead(described, huge);
    __FirmwareNodeParse();
}
