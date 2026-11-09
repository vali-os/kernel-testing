/** Host DMA model: translated segments, independent bounce storage and failures. */
#ifndef TEST_VIRTIO_DMA_MODEL_H
#define TEST_VIRTIO_DMA_MODEL_H
#include <ddk/dma.h>
#include <os/shm.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define DMA_CHECK(x)                                                                               \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            printf("DMA model:%d: %s\n", __LINE__, #x);                                            \
            fflush(NULL);                                                                          \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

struct DMAContext {
    unsigned Children;
};
/** Continuously shared storage with device addresses distinct from CPU pointers. */
struct DMABuffer {
    struct DMABuffer* Next;
    struct DMAContext* Context;
    void* Data;
    size_t Length;
    uint32_t Count;
    struct DMASegment Segments[64];
};
/** Independent device view makes missing handoffs and excess copyback observable. */
struct DMAMapping {
    struct DMAMapping* Next;
    struct DMAContext* Context;
    OSHandle_t Attachment;
    void* DeviceData;
    size_t Offset, Length, ActiveOffset, ActiveLength;
    enum DMADirection Direction;
    bool Active;
    uint32_t Count;
    struct DMASegment Segments[64];
};
static struct DMABuffer* g_dmaBuffers;
static struct DMAMapping* g_dmaMappings;
static uint64_t g_dmaNext = 0x1000000000ULL;
static unsigned g_dmaAllocations, g_dmaMaps, g_dmaBegins, g_dmaEnds;
static unsigned g_dmaLiveBuffers, g_dmaLiveMappings;
static int g_dmaFailAllocate, g_dmaFailMap, g_dmaFailBegin, g_dmaFailEnd;
static int g_dmaFailFree, g_dmaFailUnmap, g_dmaFailClose;
static size_t g_dmaStride = 4096;

static uint32_t
__TestDmaSegments(
    _InOut_ struct DMASegment* segments,
    _In_ size_t length,
    _In_ size_t stride)
{
    uint32_t count = 0;
    size_t bytes;

    while (length) {
        bytes = length < stride ? length : stride;
        DMA_CHECK(count < 64);
        segments[count++] = (struct DMASegment){g_dmaNext, bytes};
        g_dmaNext += 0x10000;
        length -= bytes;
    }
    return count;
}

oserr_t
DMAOpen(
    _In_ uuid_t id,
    _In_ uint64_t limit,
    _Out_ struct DMAContext** out)
{
    (void)id;
    (void)limit;
    DMA_CHECK(!*out);
    *out = calloc(1, sizeof(**out));
    return *out ? OS_EOK : OS_EOOM;
}
oserr_t
DMAClose(
    _InOut_ struct DMAContext** context)
{
    if (!*context) {
        return OS_EOK;
    }
    if ((*context)->Children) {
        return OS_EBUSY;
    }
    if (g_dmaFailClose) {
        return OS_EUNKNOWN;
    }
    free(*context);
    *context = NULL;
    return OS_EOK;
}
oserr_t
DMAAllocate(
    _InOut_ struct DMAContext* context,
    _In_ const struct DMARequirements* requirements,
    _Out_ struct DMABuffer** out)
{
    struct DMABuffer* buffer;
    DMA_CHECK(context && !*out);
    g_dmaAllocations++;
    if (g_dmaFailAllocate && --g_dmaFailAllocate == 0) {
        return OS_EOOM;
    }
    // The allocator may return one large segment when adjacent pages coalesce.
    // Boundary constrains that segment, not each cell a caller puts inside it.
    if (requirements->Boundary && requirements->Length > requirements->Boundary &&
        (requirements->Contiguous || g_dmaStride > requirements->Boundary)) {
        return OS_ENOTSUPPORTED;
    }
    buffer = calloc(1, sizeof(*buffer));
    DMA_CHECK(buffer);
    buffer->Length = (requirements->Length + 4095) & ~(size_t)4095;
    buffer->Data = calloc(1, buffer->Length);
    DMA_CHECK(buffer->Data);
    buffer->Context = context;
    context->Children++;
    buffer->Count = __TestDmaSegments(
        buffer->Segments, buffer->Length, requirements->Contiguous ? buffer->Length : g_dmaStride);
    buffer->Next = g_dmaBuffers;
    g_dmaBuffers = buffer;
    g_dmaLiveBuffers++;
    *out = buffer;
    return OS_EOK;
}
void*
DMABufferData(
    _In_ const struct DMABuffer* buffer)
{
    return buffer->Data;
}
size_t
DMABufferSize(
    _In_ const struct DMABuffer* buffer)
{
    return buffer->Length;
}
const struct DMASegment*
DMABufferSegments(
    _In_ const struct DMABuffer* buffer,
    _Out_ uint32_t* count)
{
    *count = buffer->Count;
    return buffer->Segments;
}
oserr_t
DMAFree(
    _InOut_ struct DMABuffer** owner)
{
    struct DMABuffer* buffer = *owner;
    struct DMABuffer** link = &g_dmaBuffers;
    if (!buffer) {
        return OS_EOK;
    }
    if (g_dmaFailFree) {
        return OS_EUNKNOWN;
    }
    while (*link != buffer) {
        DMA_CHECK(*link);
        link = &(*link)->Next;
    }
    *link = buffer->Next;
    buffer->Context->Children--;
    g_dmaLiveBuffers--;
    free(buffer->Data);
    free(buffer);
    *owner = NULL;
    return OS_EOK;
}
oserr_t
DMAMapSHM(
    _InOut_ struct DMAContext*               context,
    _In_    const OSHandle_t*                memory,
    _In_    size_t                           offset,
    _In_    size_t                           length,
    _In_    enum DMADirection                direction,
    _In_    const struct DMAMapRequirements* requirements,
    _Out_   struct DMAMapping**              out)
{
    struct DMAMapping* mapping;
    DMA_CHECK(context && !*out && requirements == NULL);
    g_dmaMaps++;
    if (g_dmaFailMap && --g_dmaFailMap == 0) {
        return OS_EOOM;
    }
    if (!length || offset > SHMBufferCapacity((OSHandle_t*)memory) ||
        length > SHMBufferCapacity((OSHandle_t*)memory) - offset) {
        return OS_EINVALPARAMS;
    }
    mapping = calloc(1, sizeof(*mapping));
    DMA_CHECK(mapping);
    DMA_CHECK(SHMAttach(memory->ID, &mapping->Attachment) == OS_EOK);
    mapping->Context = context;
    context->Children++;
    mapping->Offset = offset;
    mapping->Length = length;
    mapping->Direction = direction;
    mapping->DeviceData = calloc(1, length);
    DMA_CHECK(mapping->DeviceData);
    mapping->Count = __TestDmaSegments(mapping->Segments, length, 4096);
    mapping->Next = g_dmaMappings;
    g_dmaMappings = mapping;
    g_dmaLiveMappings++;
    *out = mapping;
    return OS_EOK;
}
const struct DMASegment*
DMAMappingSegments(
    _In_ const struct DMAMapping* mapping,
    _Out_ uint32_t* count)
{
    DMA_CHECK(mapping);
    *count = mapping->Count;
    return mapping->Segments;
}
oserr_t
DMABeginTransfer(
    _InOut_ struct DMAMapping* mapping,
    _In_ size_t offset,
    _In_ size_t length,
    _In_ enum SHMWaitMode     mode,
    _In_ const OSTimestamp_t* deadline)
{
    DMA_CHECK(mode == SHMWaitNonBlocking && deadline == NULL);
    g_dmaBegins++;
    DMA_CHECK(mapping && !mapping->Active);
    DMA_CHECK(offset <= mapping->Length && length <= mapping->Length - offset);
    if (g_dmaFailBegin) {
        return OS_EUNKNOWN;
    }
    if (mapping->Direction != DMAFromDevice) {
        memcpy((char*)mapping->DeviceData + offset,
               (char*)SHMBuffer(&mapping->Attachment) + mapping->Offset + offset,
               length);
    }
    mapping->ActiveOffset = offset;
    mapping->ActiveLength = length;
    mapping->Active = true;
    return OS_EOK;
}
oserr_t
DMAEndTransfer(
    _InOut_ struct DMAMapping* mapping,
    _In_ size_t completed)
{
    g_dmaEnds++;
    DMA_CHECK(mapping && mapping->Active && completed <= mapping->ActiveLength);
    if (g_dmaFailEnd) {
        return OS_EUNKNOWN;
    }
    if (mapping->Direction != DMAToDevice) {
        memcpy((char*)SHMBuffer(&mapping->Attachment) + mapping->Offset + mapping->ActiveOffset,
               (char*)mapping->DeviceData + mapping->ActiveOffset,
               completed);
    }
    mapping->Active = false;
    return OS_EOK;
}
oserr_t
DMAUnmap(
    _InOut_ struct DMAMapping** owner)
{
    struct DMAMapping* mapping = *owner;
    struct DMAMapping** link = &g_dmaMappings;
    if (!mapping) {
        return OS_EOK;
    }
    DMA_CHECK(!mapping->Active);
    if (g_dmaFailUnmap) {
        return OS_EUNKNOWN;
    }
    while (*link != mapping) {
        DMA_CHECK(*link);
        link = &(*link)->Next;
    }
    *link = mapping->Next;
    DMA_CHECK(SHMRelease(&mapping->Attachment) == OS_EOK);
    mapping->Context->Children--;
    g_dmaLiveMappings--;
    free(mapping->DeviceData);
    free(mapping);
    *owner = NULL;
    return OS_EOK;
}
static void*
__TestDmaPointer(
    _In_ uint64_t address)
{
    struct DMABuffer* buffer;
    struct DMAMapping* mapping;
    uint32_t i;
    size_t offset;
    for (buffer = g_dmaBuffers; buffer; buffer = buffer->Next) {
        offset = 0;
        for (i = 0; i < buffer->Count; i++) {
            if (address >= buffer->Segments[i].Address &&
                address - buffer->Segments[i].Address < buffer->Segments[i].Length) {
                return (char*)buffer->Data + offset +
                       (size_t)(address - buffer->Segments[i].Address);
            }
            offset += buffer->Segments[i].Length;
        }
    }
    for (mapping = g_dmaMappings; mapping; mapping = mapping->Next) {
        offset = 0;
        for (i = 0; i < mapping->Count; i++) {
            if (address >= mapping->Segments[i].Address &&
                address - mapping->Segments[i].Address < mapping->Segments[i].Length) {
                DMA_CHECK(mapping->Active);
                return (char*)mapping->DeviceData + offset +
                       (size_t)(address - mapping->Segments[i].Address);
            }
            offset += mapping->Segments[i].Length;
        }
    }
    DMA_CHECK(0);
    return NULL;
}
#endif
