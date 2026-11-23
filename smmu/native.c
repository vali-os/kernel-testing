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
 * Native SMMUv3 self-test coordinator. Compiled only with
 * VALI_ARM64_SMMU_SELFTEST on generic AArch64.
 *
 * One kernel thread runs one scenario against QEMU's SMMUv3 and two
 * iommu-testdev endpoints, then ends the test. It uses the real firmware
 * discovery, the native SMMUv3 unit functions, the generic DMA address space
 * and mapping calls, and (under the unit mutex) the four SMMUv3 hardware ID
 * callbacks. No DMA manager, deviced or userspace is involved.
 *
 * How a transfer proves translation:
 * The test allocates a guard range and uses its physical address as the
 * device address (IOVA). That IOVA is mapped to other data pages. If the SMMU
 * translates, the device write lands in the data page. If the SMMU were
 * bypassed, the write would land in the guard range, which is checked to stay
 * unchanged. Before every request all observed pages are filled with a byte
 * that differs from every byte of the device pattern, so an old write cannot
 * hide a missing new one. After the request every byte of every observed
 * page is compared with what it should hold.
 *
 * How a refused write is judged (the six negative cases):
 * Each negative case first runs a control transfer that must succeed, so a
 * broken fixture cannot pass as "refused". The write that must fail (the
 * probe) is 4 bytes long, because QEMU reports one event record for each
 * 4-byte access it refuses. The probe gives the device its own IOVA as the
 * physical readback address. A bypassed SMMU would make the readback match
 * (result 0), and a write translated to the wrong page gives a mismatch. Only
 * the transaction failure result counts as a refused write. Poll must then
 * return the exact record the case expects, every observed byte must still
 * hold the fill byte, and the endpoints that are still connected must
 * transfer again before cleanup.
 *
 * Serial records (one log line each, kept below 120 characters):
 * - "SMMU-SELFTEST START case=<name>": the worker began this scenario.
 * - "SMMU-SELFTEST IORT ...": the raw IORT, as length and hex chunks.
 * - "SMMU-SELFTEST STEP <name> ...": one finished step with its details. For
 *   each request: the request and device result, then "<name> poll" with the
 *   Poll status and record count, then "<name> memory" with the outcome of
 *   the memory checks.
 * - "SMMU-SELFTEST FAULT expected <request> ...": the one fault record a
 *   negative case expects, printed after its poll record.
 * - "SMMU-SELFTEST FAULT unexpected ...": any other fault record.
 * - "SMMU-SELFTEST PASS case=<name>": everything passed and cleanup finished.
 * - "SMMU-SELFTEST FAIL case=<name> step=<step> status=<n>": the test
 *   stopped. Resources are kept as they are, since the hardware may still
 *   reference them.
 * A run that finishes ends with exactly one PASS or FAIL record. A panic or
 * a hang prints neither, and the host runner reports that as FAIL.
 *
 * Files:
 * This file holds the worker thread, the case table and the terminal PASS and
 * FAIL records, so it shows one run from start to end. The steps live in
 * native_setup.c, native_requests.c, native_cases.c and native_cleanup.c. They
 * share the test state and records through native_coordinator.h.
 */

#define __MODULE "SMMUTEST"

#include "native.h"
#include "native_coordinator.h"
#include <debug.h>
#include <threading.h>

// Numeric scenario IDs set by CMake from VALI_ARM64_SMMU_SELFTEST_CASE, in
// the order of the case list in the root CMakeLists.txt.
#define __CASE_POSITIVE         0
#define __CASE_UNMAPPED         1
#define __CASE_WRITE_DENIED     2
#define __CASE_UNKNOWN_STREAM   3
#define __CASE_OTHER_DOMAIN     4
#define __CASE_AFTER_UNMAP      5
#define __CASE_AFTER_DISCONNECT 6

#if !defined(VALI_ARM64_SMMU_SELFTEST_CASE_ID) || VALI_ARM64_SMMU_SELFTEST_CASE_ID < __CASE_POSITIVE || \
    VALI_ARM64_SMMU_SELFTEST_CASE_ID > __CASE_AFTER_DISCONNECT
#error "VALI_ARM64_SMMU_SELFTEST_CASE_ID must name one of the seven native SMMUv3 cases"
#endif

/**
 * @brief One scenario. ConnectedDomains is how many domains setup connects,
 * starting with domain A. Run performs the requests of the case after setup
 * and before cleanup.
 */
typedef struct __TestCase {
    const char* Name;
    uint32_t    ConnectedDomains;
    oserr_t     (*Run)(void);
} __TestCase_t;

// The one test state record. See native_coordinator.h for its fields. The
// mapping table gives the IOVA layout shown at ArmSMMUTestMappingIndex.
ArmSMMUSelfTestState_t g_armSMMUSelfTest = {
    .Step = "start",
    .Domains = {
        { .Name = "domain-a", .Device = &g_armSMMUSelfTest.Devices[0] },
        { .Name = "domain-b", .Device = &g_armSMMUSelfTest.Devices[1] }
    },
    .Blocks = {
        [ArmSMMUTestBlockGuard]    = { .Name = "guard",     .PageCount = 4 },
        [ArmSMMUTestBlockDomainA]  = { .Name = "data-a",    .PageCount = 1 },
        [ArmSMMUTestBlockBoundary] = { .Name = "boundary",  .PageCount = 2 },
        [ArmSMMUTestBlockDomainB]  = { .Name = "data-b",    .PageCount = 1 },
        [ArmSMMUTestBlockReadOnly] = { .Name = "read-only", .PageCount = 1 }
    },
    .Mappings = {
        [ArmSMMUTestMappingWriteOnly] = { "write-only", 0, ArmSMMUTestBlockDomainA,  DMAFromDevice,    0x0000, 0, NULL },
        [ArmSMMUTestMappingBoundary]  = { "boundary",   0, ArmSMMUTestBlockBoundary, DMABidirectional, 0x1000, 0, NULL },
        [ArmSMMUTestMappingDomainB]   = { "domain-b",   1, ArmSMMUTestBlockDomainB,  DMABidirectional, 0x0000, 0, NULL },
        [ArmSMMUTestMappingReadOnly]  = { "read-only",  1, ArmSMMUTestBlockReadOnly, DMAToDevice,      0x1000, 0, NULL }
    }
};

// Case table, indexed by the numeric case ID from CMake.
static const __TestCase_t g_testCases[] = {
    [__CASE_POSITIVE]         = { "positive",         2, ArmSMMUSelfTestRunPositive },
    [__CASE_UNMAPPED]         = { "unmapped",         2, ArmSMMUSelfTestRunUnmapped },
    [__CASE_WRITE_DENIED]     = { "write-denied",     2, ArmSMMUSelfTestRunWriteDenied },
    [__CASE_UNKNOWN_STREAM]   = { "unknown-stream",   1, ArmSMMUSelfTestRunUnknownStream },
    [__CASE_OTHER_DOMAIN]     = { "other-domain",     2, ArmSMMUSelfTestRunOtherDomain },
    [__CASE_AFTER_UNMAP]      = { "after-unmap",      2, ArmSMMUSelfTestRunAfterUnmap },
    [__CASE_AFTER_DISCONNECT] = { "after-disconnect", 2, ArmSMMUSelfTestRunAfterDisconnect }
};

// The case this kernel was built for.
static const __TestCase_t* const g_testCase = &g_testCases[VALI_ARM64_SMMU_SELFTEST_CASE_ID];

/*******************************************************************************
 * Worker
 ******************************************************************************/

// Ends a failed test. Stops both endpoints so no request can still run, then
// prints the terminal FAIL record with the step that failed. Every other
// resource is kept, because hardware may still reference it.
static void
__Fail(
    _In_ oserr_t status)
{
    oserr_t stopStatus;

    stopStatus = ArmSMMUSelfTestStopDevices();
    if (stopStatus != OS_EOK) {
        ERROR("SMMU-SELFTEST: the endpoints could not be stopped after the failure: %u", stopStatus);
    }
    ERROR("SMMU-SELFTEST FAIL case=%s step=%s status=%u", g_testCase->Name, g_armSMMUSelfTest.Step, status);
}

// Worker thread. Runs setup, the selected case and cleanup, then prints the
// terminal record and returns, which ends the thread. The first failing
// stage ends the test through __Fail.
static void
__Run(
    _In_ void* context)
{
    oserr_t status;

    (void)context;
    DEBUG("SMMU-SELFTEST START case=%s", g_testCase->Name);

    status = ArmSMMUSelfTestSetup(g_testCase->ConnectedDomains);
    if (status != OS_EOK) {
        __Fail(status);
        return;
    }

    status = g_testCase->Run();
    if (status != OS_EOK) {
        __Fail(status);
        return;
    }

    status = ArmSMMUSelfTestCleanup();
    if (status != OS_EOK) {
        __Fail(status);
        return;
    }
    DEBUG("SMMU-SELFTEST PASS case=%s", g_testCase->Name);
}

oserr_t
ArmSMMUSelfTestStart(void)
{
    uuid_t  thread;
    oserr_t status;

    status = ThreadCreate(
        "smmu-selftest",
        __Run,
        NULL,
        THREADING_KERNELMODE,
        UUID_INVALID,
        0,
        0,
        &thread
    );
    if (status != OS_EOK) {
        ERROR("ArmSMMUSelfTestStart: creating the worker thread failed: %u", status);
    }
    return status;
}
