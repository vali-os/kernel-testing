/**
 * Copyright, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * Host test for DMAMapSHMChunk. The real librt/libddk/dma/chunk.c is linked
 * against scripted fakes of DMAMapSHMDirect and DMAMapSHM. Each fake records
 * its arguments and returns the next scripted status, so the test can check
 * the exact sequence of map calls and the values the caller gets back.
 */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ddk/dma.h>
#include <ddk/utils.h>

#define BLOCK      512U
#define MAX_CALLS  32
#define NO_LENGTH  ((size_t)0xDEAD)

enum __MapKind {
    __MapDirect,
    __MapBounce
};

struct __MapCall {
    enum __MapKind                   Kind;
    struct DMAContext*               Context;
    const OSHandle_t*                Buffer;
    size_t                           Offset;
    size_t                           Length;
    enum DMADirection                Direction;
    const struct DMAMapRequirements* Requirements;
};

// Fake objects. Only their addresses are compared, the contents are never read.
static int g_contextStorage;
static int g_mappingStorage;
static int g_otherMappingStorage;
static OSHandle_t g_buffer = { .ID = 33, .Type = OSHANDLE_SHM };

static struct __MapCall g_calls[MAX_CALLS];
static int              g_callCount;
static oserr_t          g_directStatus;
static oserr_t          g_bounceStatus[MAX_CALLS];
static int              g_bounceStatusCount;
static int              g_bounceCalls;
static int              g_warnings;

#define TEST_CONTEXT ((struct DMAContext*)&g_contextStorage)
#define TEST_MAPPING ((struct DMAMapping*)&g_mappingStorage)

void
SystemDebug(enum OSSysLogLevel level, const char* format, ...)
{
    va_list arguments;

    // Count warnings so the test can check that the final pool shortage is
    // reported. The text is printed to help when a case fails.
    if (level == OSSYSLOGLEVEL_WARNING) {
        g_warnings++;
    }
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
}

static void
__Record(
    _In_    enum __MapKind                   kind,
    _In_    struct DMAContext*               context,
    _In_    const OSHandle_t*                buffer,
    _In_    size_t                           offset,
    _In_    size_t                           length,
    _In_    enum DMADirection                direction,
    _In_    const struct DMAMapRequirements* requirements,
    _InOut_ struct DMAMapping**              mappingOut)
{
    // The helper must never pass a filled slot to a map call.
    assert(g_callCount < MAX_CALLS);
    assert(mappingOut != NULL && *mappingOut == NULL);
    g_calls[g_callCount++] = (struct __MapCall) {
        .Kind = kind,
        .Context = context,
        .Buffer = buffer,
        .Offset = offset,
        .Length = length,
        .Direction = direction,
        .Requirements = requirements
    };
}

oserr_t
DMAMapSHMDirect(
    _In_    struct DMAContext*               context,
    _In_    const OSHandle_t*                buffer,
    _In_    size_t                           offset,
    _In_    size_t                           length,
    _In_    enum DMADirection                direction,
    _In_    const struct DMAMapRequirements* requirements,
    _InOut_ struct DMAMapping**              mappingOut)
{
    __Record(__MapDirect, context, buffer, offset, length, direction, requirements, mappingOut);
    if (g_directStatus == OS_EOK) {
        *mappingOut = TEST_MAPPING;
    }
    return g_directStatus;
}

oserr_t
DMAMapSHM(
    _In_    struct DMAContext*               context,
    _In_    const OSHandle_t*                buffer,
    _In_    size_t                           offset,
    _In_    size_t                           length,
    _In_    enum DMADirection                direction,
    _In_    const struct DMAMapRequirements* requirements,
    _InOut_ struct DMAMapping**              mappingOut)
{
    oserr_t status;

    __Record(__MapBounce, context, buffer, offset, length, direction, requirements, mappingOut);
    assert(g_bounceCalls < g_bounceStatusCount);
    status = g_bounceStatus[g_bounceCalls++];
    if (status == OS_EOK) {
        *mappingOut = TEST_MAPPING;
    }
    return status;
}

static void
__Script(
    _In_ oserr_t        directStatus,
    _In_ int            bounceCount,
    _In_ const oserr_t* bounceStatus)
{
    assert(bounceCount <= MAX_CALLS);
    memset(g_calls, 0, sizeof(g_calls));
    g_callCount = 0;
    g_bounceCalls = 0;
    g_warnings = 0;
    g_directStatus = directStatus;
    g_bounceStatusCount = bounceCount;
    if (bounceCount != 0) {
        memcpy(g_bounceStatus, bounceStatus, (size_t)bounceCount * sizeof(oserr_t));
    }
}

static oserr_t
__Map(
    _In_    size_t                           offset,
    _In_    size_t                           length,
    _In_    const struct DMAMapRequirements* requirements,
    _In_    const struct DMAChunkLimits*     limits,
    _InOut_ struct DMAMapping**              mapping,
    _Out_   size_t*                          mappedLength)
{
    return DMAMapSHMChunk(
        TEST_CONTEXT,
        &g_buffer,
        offset,
        length,
        DMAFromDevice,
        requirements,
        limits,
        mapping,
        mappedLength
    );
}

static void
__ExpectCall(
    _In_ int            index,
    _In_ enum __MapKind kind,
    _In_ size_t         offset,
    _In_ size_t         length)
{
    assert(index < g_callCount);
    assert(g_calls[index].Kind == kind);
    assert(g_calls[index].Context == TEST_CONTEXT);
    assert(g_calls[index].Buffer == &g_buffer);
    assert(g_calls[index].Offset == offset);
    assert(g_calls[index].Length == length);
}

static void
__TestDirectSuccess(void)
{
    struct DMAChunkLimits limits = { 8 * BLOCK, 2 * BLOCK, BLOCK };
    static const size_t   lengths[] = { 3 * BLOCK, 8 * BLOCK, 20 * BLOCK };
    static const size_t   expected[] = { 3 * BLOCK, 8 * BLOCK, 8 * BLOCK };
    struct DMAMapping*    mapping;
    size_t                mappedLength;
    size_t                i;

    // Below, equal to and above DirectLength. The direct path is clipped to the
    // limit and never touches the bounce path.
    for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        mapping = NULL;
        mappedLength = NO_LENGTH;
        __Script(OS_EOK, 0, NULL);
        assert(__Map(4096, lengths[i], NULL, &limits, &mapping, &mappedLength) == OS_EOK);
        assert(mapping == TEST_MAPPING);
        assert(mappedLength == expected[i]);
        assert(g_callCount == 1);
        __ExpectCall(0, __MapDirect, 4096, expected[i]);
    }
}

static void
__TestDirectFallback(void)
{
    struct DMAChunkLimits limits = { 8 * BLOCK, 2 * BLOCK, BLOCK };
    static const oserr_t  fallbackErrors[] = { OS_ENOENT, OS_ENOTSUPPORTED };
    static const oserr_t  bounceOk[] = { OS_EOK };
    struct DMAMapping*    mapping;
    size_t                mappedLength;
    size_t                i;

    // Both errors mean the pages need a bounce buffer. The bounce attempt is
    // clipped to BounceLength.
    for (i = 0; i < sizeof(fallbackErrors) / sizeof(fallbackErrors[0]); i++) {
        mapping = NULL;
        mappedLength = NO_LENGTH;
        __Script(fallbackErrors[i], 1, bounceOk);
        assert(__Map(0, 20 * BLOCK, NULL, &limits, &mapping, &mappedLength) == OS_EOK);
        assert(mapping == TEST_MAPPING);
        assert(mappedLength == 2 * BLOCK);
        assert(g_callCount == 2);
        __ExpectCall(0, __MapDirect, 0, 8 * BLOCK);
        __ExpectCall(1, __MapBounce, 0, 2 * BLOCK);
    }

    // A transfer shorter than BounceLength is mapped in full.
    mapping = NULL;
    mappedLength = NO_LENGTH;
    __Script(OS_ENOENT, 1, bounceOk);
    assert(__Map(0, BLOCK, NULL, &limits, &mapping, &mappedLength) == OS_EOK);
    assert(mappedLength == BLOCK);
    __ExpectCall(1, __MapBounce, 0, BLOCK);
}

static void
__TestDirectOtherError(void)
{
    struct DMAChunkLimits limits = { 8 * BLOCK, 2 * BLOCK, BLOCK };
    static const oserr_t  errors[] = { OS_EINVALPARAMS, OS_EBUSY, OS_EOOM, OS_EBUFFER, OS_EUNKNOWN };
    struct DMAMapping*    mapping;
    size_t                mappedLength;
    size_t                i;

    // Any other direct error is returned unchanged, with no bounce attempt.
    // OS_EOOM and OS_EBUFFER only shrink on the bounce path.
    for (i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        mapping = NULL;
        mappedLength = NO_LENGTH;
        __Script(errors[i], 0, NULL);
        assert(__Map(0, 20 * BLOCK, NULL, &limits, &mapping, &mappedLength) == errors[i]);
        assert(mapping == NULL);
        assert(mappedLength == NO_LENGTH);
        assert(g_callCount == 1);
    }
}

static void
__TestShrink(
    _In_ oserr_t       shrinkError,
    _In_ size_t        bounceBlocks,
    _In_ const size_t* expectedBlocks,
    _In_ int           expectedCount)
{
    struct DMAChunkLimits limits = { 64 * BLOCK, bounceBlocks * BLOCK, BLOCK };
    oserr_t               script[MAX_CALLS];
    struct DMAMapping*    mapping;
    size_t                mappedLength;
    int                   i;

    // Every attempt but the last fails with the shrinking error. Lengths must
    // halve, stay whole blocks and end at one block.
    for (i = 0; i < expectedCount - 1; i++) {
        script[i] = shrinkError;
    }
    script[expectedCount - 1] = OS_EOK;

    mapping = NULL;
    mappedLength = NO_LENGTH;
    __Script(OS_ENOTSUPPORTED, expectedCount, script);
    assert(__Map(BLOCK, 64 * BLOCK, NULL, &limits, &mapping, &mappedLength) == OS_EOK);
    assert(mapping == TEST_MAPPING);
    assert(mappedLength == expectedBlocks[expectedCount - 1] * BLOCK);
    assert(g_callCount == expectedCount + 1);
    __ExpectCall(0, __MapDirect, BLOCK, 64 * BLOCK);
    for (i = 0; i < expectedCount; i++) {
        __ExpectCall(i + 1, __MapBounce, BLOCK, expectedBlocks[i] * BLOCK);
    }
    assert(g_warnings == 0);
}

static void
__TestShrinkSequences(void)
{
    static const size_t eight[] = { 8, 4, 2, 1 };
    static const size_t five[] = { 5, 2, 1 };
    static const size_t three[] = { 3, 1 };
    static const size_t seven[] = { 7, 3, 1 };
    static const oserr_t errors[] = { OS_EOOM, OS_EBUFFER };
    size_t              i;

    for (i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        __TestShrink(errors[i], 8, eight, 4);
        __TestShrink(errors[i], 5, five, 3);
        __TestShrink(errors[i], 3, three, 2);
        __TestShrink(errors[i], 7, seven, 3);
        // Success part way down the sequence stops shrinking.
        __TestShrink(errors[i], 8, eight, 2);
    }
}

static void
__TestMixedShrinkErrors(void)
{
    struct DMAChunkLimits limits = { 64 * BLOCK, 8 * BLOCK, BLOCK };
    static const oserr_t  script[] = { OS_EBUFFER, OS_EOOM, OS_EOK };
    struct DMAMapping*    mapping = NULL;
    size_t                mappedLength = NO_LENGTH;

    // The two shrinking errors can follow each other in one call.
    __Script(OS_ENOENT, 3, script);
    assert(__Map(0, 64 * BLOCK, NULL, &limits, &mapping, &mappedLength) == OS_EOK);
    assert(mappedLength == 2 * BLOCK);
    assert(g_callCount == 4);
    __ExpectCall(1, __MapBounce, 0, 8 * BLOCK);
    __ExpectCall(2, __MapBounce, 0, 4 * BLOCK);
    __ExpectCall(3, __MapBounce, 0, 2 * BLOCK);
}

static void
__TestOneBlockFailure(void)
{
    struct DMAChunkLimits limits = { 64 * BLOCK, 4 * BLOCK, BLOCK };
    static const oserr_t  errors[] = { OS_EOOM, OS_EBUFFER };
    oserr_t               script[3];
    struct DMAMapping*    mapping;
    size_t                mappedLength;
    size_t                i;

    // When even one block fails, that error is returned unchanged and both
    // outputs are left untouched. No attempt is made below one block.
    for (i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        script[0] = errors[i];
        script[1] = errors[i];
        script[2] = errors[i];
        mapping = NULL;
        mappedLength = NO_LENGTH;
        __Script(OS_ENOENT, 3, script);
        assert(__Map(0, 64 * BLOCK, NULL, &limits, &mapping, &mappedLength) == errors[i]);
        assert(mapping == NULL);
        assert(mappedLength == NO_LENGTH);
        assert(g_callCount == 4);
        __ExpectCall(1, __MapBounce, 0, 4 * BLOCK);
        __ExpectCall(2, __MapBounce, 0, 2 * BLOCK);
        __ExpectCall(3, __MapBounce, 0, BLOCK);
        assert(g_warnings == 1);
    }

    // A one-block transfer that fails is not retried.
    script[0] = OS_EOOM;
    mapping = NULL;
    mappedLength = NO_LENGTH;
    __Script(OS_ENOENT, 1, script);
    assert(__Map(0, BLOCK, NULL, &limits, &mapping, &mappedLength) == OS_EOOM);
    assert(mapping == NULL && mappedLength == NO_LENGTH);
    assert(g_callCount == 2);
}

static void
__TestBounceOtherError(void)
{
    struct DMAChunkLimits limits = { 64 * BLOCK, 8 * BLOCK, BLOCK };
    static const oserr_t  first[] = { OS_EINVALPARAMS };
    static const oserr_t  later[] = { OS_EOOM, OS_EBUSY };
    struct DMAMapping*    mapping;
    size_t                mappedLength;

    // A non-shrinking bounce error is returned at once.
    mapping = NULL;
    mappedLength = NO_LENGTH;
    __Script(OS_ENOENT, 1, first);
    assert(__Map(0, 64 * BLOCK, NULL, &limits, &mapping, &mappedLength) == OS_EINVALPARAMS);
    assert(mapping == NULL && mappedLength == NO_LENGTH);
    assert(g_callCount == 2);

    // The same holds after a shrink has already happened.
    __Script(OS_ENOENT, 2, later);
    assert(__Map(0, 64 * BLOCK, NULL, &limits, &mapping, &mappedLength) == OS_EBUSY);
    assert(mapping == NULL && mappedLength == NO_LENGTH);
    assert(g_callCount == 3);
    __ExpectCall(2, __MapBounce, 0, 4 * BLOCK);
}

static void
__TestInvalidArguments(void)
{
    struct DMAChunkLimits limits = { 8 * BLOCK, 2 * BLOCK, BLOCK };
    struct DMAChunkLimits bad;
    struct DMAMapping*    mapping = NULL;
    struct DMAMapping*    filled = (struct DMAMapping*)&g_otherMappingStorage;
    size_t                mappedLength = NO_LENGTH;

    __Script(OS_EOK, 0, NULL);

    // Required pointers.
    assert(DMAMapSHMChunk(NULL, &g_buffer, 0, BLOCK, DMAFromDevice, NULL, &limits,
                          &mapping, &mappedLength) == OS_EINVALPARAMS);
    assert(DMAMapSHMChunk(TEST_CONTEXT, NULL, 0, BLOCK, DMAFromDevice, NULL, &limits,
                          &mapping, &mappedLength) == OS_EINVALPARAMS);
    assert(DMAMapSHMChunk(TEST_CONTEXT, &g_buffer, 0, BLOCK, DMAFromDevice, NULL, NULL,
                          &mapping, &mappedLength) == OS_EINVALPARAMS);
    assert(DMAMapSHMChunk(TEST_CONTEXT, &g_buffer, 0, BLOCK, DMAFromDevice, NULL, &limits,
                          NULL, &mappedLength) == OS_EINVALPARAMS);
    assert(DMAMapSHMChunk(TEST_CONTEXT, &g_buffer, 0, BLOCK, DMAFromDevice, NULL, &limits,
                          &mapping, NULL) == OS_EINVALPARAMS);

    // The mapping slot must be empty, and stays as it was.
    assert(__Map(0, BLOCK, NULL, &limits, &filled, &mappedLength) == OS_EINVALPARAMS);
    assert(filled == (struct DMAMapping*)&g_otherMappingStorage);

    // Zero length and lengths that are not whole blocks.
    assert(__Map(0, 0, NULL, &limits, &mapping, &mappedLength) == OS_EINVALPARAMS);
    assert(__Map(0, BLOCK + 1, NULL, &limits, &mapping, &mappedLength) == OS_EINVALPARAMS);
    assert(__Map(0, BLOCK - 1, NULL, &limits, &mapping, &mappedLength) == OS_EINVALPARAMS);

    // Zero block length.
    bad = limits;
    bad.BlockLength = 0;
    assert(__Map(0, BLOCK, NULL, &bad, &mapping, &mappedLength) == OS_EINVALPARAMS);

    // Limits smaller than one block.
    bad = limits;
    bad.DirectLength = BLOCK - 1;
    assert(__Map(0, BLOCK, NULL, &bad, &mapping, &mappedLength) == OS_EINVALPARAMS);
    bad = limits;
    bad.BounceLength = BLOCK - 1;
    assert(__Map(0, BLOCK, NULL, &bad, &mapping, &mappedLength) == OS_EINVALPARAMS);
    bad = limits;
    bad.DirectLength = 0;
    assert(__Map(0, BLOCK, NULL, &bad, &mapping, &mappedLength) == OS_EINVALPARAMS);

    // No map call was made, and the outputs were never written.
    assert(g_callCount == 0);
    assert(mapping == NULL);
    assert(mappedLength == NO_LENGTH);
}

static void
__TestLimitRounding(void)
{
    struct DMAChunkLimits limits = { 3 * BLOCK + 100, 2 * BLOCK + BLOCK - 1, BLOCK };
    static const oserr_t  bounceOk[] = { OS_EOK };
    struct DMAMapping*    mapping = NULL;
    size_t                mappedLength = NO_LENGTH;

    // Limits that are not whole blocks are rounded down before use.
    __Script(OS_EOK, 0, NULL);
    assert(__Map(0, 10 * BLOCK, NULL, &limits, &mapping, &mappedLength) == OS_EOK);
    assert(mappedLength == 3 * BLOCK);
    __ExpectCall(0, __MapDirect, 0, 3 * BLOCK);

    mapping = NULL;
    mappedLength = NO_LENGTH;
    __Script(OS_ENOENT, 1, bounceOk);
    assert(__Map(0, 10 * BLOCK, NULL, &limits, &mapping, &mappedLength) == OS_EOK);
    assert(mappedLength == 2 * BLOCK);
    __ExpectCall(1, __MapBounce, 0, 2 * BLOCK);

    // A limit just above one block rounds to exactly one block.
    limits.DirectLength = BLOCK + 1;
    limits.BounceLength = BLOCK + 1;
    mapping = NULL;
    mappedLength = NO_LENGTH;
    __Script(OS_ENOENT, 1, bounceOk);
    assert(__Map(0, 10 * BLOCK, NULL, &limits, &mapping, &mappedLength) == OS_EOK);
    assert(mappedLength == BLOCK);
    __ExpectCall(0, __MapDirect, 0, BLOCK);
    __ExpectCall(1, __MapBounce, 0, BLOCK);
}

static void
__TestPassThrough(void)
{
    struct DMAChunkLimits     limits = { 8 * BLOCK, 4 * BLOCK, BLOCK };
    struct DMAMapRequirements requirements = { 64, BLOCK };
    static const oserr_t      script[] = { OS_EOOM, OS_EOK };
    static const enum DMADirection directions[] = { DMAToDevice, DMAFromDevice, DMABidirectional };
    struct DMAMapping*        mapping;
    size_t                    mappedLength;
    size_t                    i;
    int                       call;

    // The requirements pointer and the direction reach every map call
    // unchanged, including the retries.
    for (i = 0; i < sizeof(directions) / sizeof(directions[0]); i++) {
        mapping = NULL;
        mappedLength = NO_LENGTH;
        __Script(OS_ENOENT, 2, script);
        assert(DMAMapSHMChunk(TEST_CONTEXT, &g_buffer, 7 * BLOCK, 16 * BLOCK, directions[i],
                              &requirements, &limits, &mapping, &mappedLength) == OS_EOK);
        assert(mappedLength == 2 * BLOCK);
        assert(g_callCount == 3);
        for (call = 0; call < g_callCount; call++) {
            assert(g_calls[call].Requirements == &requirements);
            assert(g_calls[call].Direction == directions[i]);
            assert(g_calls[call].Offset == 7 * BLOCK);
        }
    }
    assert(requirements.AddressAlignment == 64 && requirements.LengthAlignment == BLOCK);

    // NULL requirements stay NULL.
    mapping = NULL;
    __Script(OS_EOK, 0, NULL);
    assert(__Map(0, BLOCK, NULL, &limits, &mapping, &mappedLength) == OS_EOK);
    assert(g_calls[0].Requirements == NULL);
}

static void
__TestChunkLoop(void)
{
    struct DMAChunkLimits limits = { 8 * BLOCK, 2 * BLOCK, BLOCK };
    static const oserr_t  bounceOk[] = { OS_EOK };
    struct DMAMapping*    mapping;
    size_t                offset = 0;
    size_t                remaining = 5 * BLOCK;
    size_t                mappedLength;
    int                   chunks = 0;

    // A caller walks the transfer by adding the mapped length to the offset.
    // With every chunk bouncing, five blocks take three calls: 2, 2 and 1.
    while (remaining != 0) {
        mapping = NULL;
        __Script(OS_ENOENT, 1, bounceOk);
        assert(__Map(offset, remaining, NULL, &limits, &mapping, &mappedLength) == OS_EOK);
        assert(mappedLength != 0 && mappedLength <= remaining && (mappedLength % BLOCK) == 0);
        __ExpectCall(1, __MapBounce, offset, mappedLength);
        offset += mappedLength;
        remaining -= mappedLength;
        chunks++;
    }
    assert(chunks == 3 && offset == 5 * BLOCK);
}

int main(void)
{
    __TestDirectSuccess();
    __TestDirectFallback();
    __TestDirectOtherError();
    __TestShrinkSequences();
    __TestMixedShrinkErrors();
    __TestOneBlockFailure();
    __TestBounceOtherError();
    __TestInvalidArguments();
    __TestLimitRounding();
    __TestPassThrough();
    __TestChunkLoop();
    printf("DMAMapSHMChunk host tests passed\n");
    return 0;
}
