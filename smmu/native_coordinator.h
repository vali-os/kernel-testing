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
 * Native SMMUv3 self-test: state and records shared by the coordinator files.
 * Compiled only with VALI_ARM64_SMMU_SELFTEST on generic AArch64.
 *
 * The coordinator is split by topic, and every file below includes this
 * header:
 * - native.c: the worker thread, the case table and the terminal PASS and
 *   FAIL records. Start reading there to follow one run from start to end.
 * - native_setup.c: clock check, IORT capture, route discovery, unit start,
 *   test pages, address spaces, mappings and connections.
 * - native_requests.c: transfers and refused-write probes, the page and Poll
 *   checks after each request, and the serial records for them.
 * - native_cases.c: the seven case functions and their request tables.
 * - native_cleanup.c: stopping the endpoints and tearing everything down.
 *
 * This header holds the test records, the one shared state record and the
 * few functions that one file calls in another. Every other function stays
 * static in its own file. The PCI adapter (native_pci.c) has its own header,
 * native_private.h, and knows nothing about these records.
 *
 * Locking:
 * Everything here runs on the single worker thread, so the state needs no
 * lock of its own. None of the functions below may be called with the unit
 * mutex held. Each one takes the mutex itself around the backend calls that
 * need it.
 */

#ifndef __ARM_SMMU_NATIVE_COORDINATOR_H__
#define __ARM_SMMU_NATIVE_COORDINATOR_H__

#include "native_private.h"
#include "../../kernel/arch/aarch64/smmu/discovery.h"
#include "../../kernel/arch/aarch64/smmu/v3/private.h"
#include "../../kernel/memory/dma/private.h"

// Cleanup retries for calls that document OS_ETIMEOUT as retryable.
#define ARM_SMMU_SELFTEST_MAX_ATTEMPTS   3U

// Fault records read by one Poll call.
#define ARM_SMMU_SELFTEST_FAULT_CAPACITY 8U

// Observed memory blocks. The guard covers the whole IOVA range used by
// both domains. Its physical address is the IOVA itself.
enum ArmSMMUTestBlockIndex {
    ArmSMMUTestBlockGuard,    // 4 pages. IOVA range of both domains. Must never change.
    ArmSMMUTestBlockDomainA,  // 1 page. Backs IOVA page 0 in domain A (write-only).
    ArmSMMUTestBlockBoundary, // 2 pages. Backs IOVA pages 1-2 in domain A (read/write).
    ArmSMMUTestBlockDomainB,  // 1 page. Backs IOVA page 0 in domain B (read/write).
    ArmSMMUTestBlockReadOnly, // 1 page. Backs IOVA page 1 in domain B (device reads only).
    ArmSMMUTestBlockCount
};

// Mappings installed for every case, in install order. Each space hands out
// IOVAs lowest first, so this order gives the IOVA layout below:
//
//   IOVA page   Domain A               Domain B
//   0           data-a (write-only)    data-b (read/write)
//   1           boundary (read/write)  read-only (device reads only)
//   2           boundary (read/write)  -
//   3           -                      -
enum ArmSMMUTestMappingIndex {
    ArmSMMUTestMappingWriteOnly, // Domain A, IOVA page 0 -> ArmSMMUTestBlockDomainA, DMAFromDevice.
    ArmSMMUTestMappingBoundary,  // Domain A, IOVA pages 1-2 -> ArmSMMUTestBlockBoundary, DMABidirectional.
    ArmSMMUTestMappingDomainB,   // Domain B, IOVA page 0 -> ArmSMMUTestBlockDomainB, DMABidirectional.
    ArmSMMUTestMappingReadOnly,  // Domain B, IOVA page 1 -> ArmSMMUTestBlockReadOnly, DMAToDevice.
    ArmSMMUTestMappingCount
};

// Connection progress of one domain's stream ID.
enum ArmSMMUTestConnectionState {
    ArmSMMUTestConnectionNone,        // No connection record exists.
    ArmSMMUTestConnectionPrepared,    // PrepareHardwareIDs reserved the stream ID.
    ArmSMMUTestConnectionConnected,   // ConnectHardwareIDs routed the stream ID to the space.
    ArmSMMUTestConnectionDisconnected // DisconnectHardwareIDs completed. Only Destroy remains.
};

// What the SMMU must do with a probe, and so what Poll must return.
enum ArmSMMUTestProbeOutcome {
    ArmSMMUTestProbeNoMapping,  // One NoMapping record (SMMUv3 event 0x10, F_TRANSLATION).
    ArmSMMUTestProbePermission, // One Permission record (SMMUv3 event 0x13, F_PERMISSION).
    ArmSMMUTestProbeAbort       // No record, or one NoRoute record (event 0x02, C_BAD_STREAMID).
};

/**
 * @brief One allocated block of test pages. Name is used in log records.
 */
typedef struct ArmSMMUTestBlock {
    const char*          Name;
    uint32_t             PageCount;
    ArmSMMUMemoryBlock_t Memory;
} ArmSMMUTestBlock_t;

/**
 * @brief One translated domain: a DMA address space and the single test endpoint
 * whose stream ID is connected to it. Connection is the backend record from
 * PrepareHardwareIDs. HardwareID stays in static storage because the backend
 * may keep a pointer to it until DestroyHardwareIDs.
 */
typedef struct ArmSMMUTestDomain {
    const char*                     Name;
    ArmSMMUTestDevice_t*            Device;
    uint32_t                        HardwareID;
    struct DMAAddressSpace*         Space;
    void*                           Connection;
    enum ArmSMMUTestConnectionState ConnectionState;
} ArmSMMUTestDomain_t;

/**
 * @brief One mapping. IovaOffset is the IOVA the space must return, relative
 * to the guard start. DeviceAddress is that IOVA once it was checked at
 * install time. Mapping is NULL before Prepare and after a successful
 * release, which DMAAddressMappingRelease treats as nothing to do.
 */
typedef struct ArmSMMUTestMapping {
    const char*                Name;
    uint32_t                   Domain;
    enum ArmSMMUTestBlockIndex Block;
    enum DMADirection          Direction;
    uint64_t                   IovaOffset;
    uint64_t                   DeviceAddress;
    struct DMAAddressMapping*  Mapping;
} ArmSMMUTestMapping_t;

/**
 * @brief One device request that must succeed, and the bytes it must change.
 * IOVA offsets are relative to the guard start. The target is a byte range in
 * one observed block.
 */
typedef struct ArmSMMUTestTransfer {
    const char*                Name;
    uint32_t                   Domain;
    uint64_t                   IovaOffset;
    enum ArmSMMUTestBlockIndex Block;
    uint64_t                   BlockOffset;
    uint32_t                   Length;
} ArmSMMUTestTransfer_t;

/**
 * @brief One device write that the SMMU must refuse. The IOVA offset is
 * relative to the guard start, and the write is 4 bytes long (__PROBE_LENGTH
 * in native_requests.c). Outcome says which fault record Poll must return for
 * it.
 */
typedef struct ArmSMMUTestProbe {
    const char*                  Name;
    uint32_t                     Domain;
    uint64_t                     IovaOffset;
    enum ArmSMMUTestProbeOutcome Outcome;
} ArmSMMUTestProbe_t;

/**
 * @brief All test state.
 *
 * Kept in static storage, because hardware or an unfinished cleanup may still
 * reference these records after the worker thread exits. Step names the step
 * that runs now, for the FAIL record. Description, UnitIndex and AddressLimit
 * come from discovery. AddressLimit is the lower device address limit of both
 * routes. Memory is the separate test arena that holds all Blocks.
 */
typedef struct ArmSMMUSelfTestState {
    const char*          Step;
    ArmSMMUDescription_t Description;
    uint32_t             UnitIndex;
    uint64_t             AddressLimit;
    SMMUv3Unit_t*        Unit;
    DMAIOMMUUnit_t*      Backend;
    ArmSMMUMemory_t      Memory;
    ArmSMMUTestDevice_t  Devices[ARM_SMMU_TEST_DEVICE_COUNT];
    ArmSMMUTestDomain_t  Domains[ARM_SMMU_TEST_DEVICE_COUNT];
    ArmSMMUTestBlock_t   Blocks[ArmSMMUTestBlockCount];
    ArmSMMUTestMapping_t Mappings[ArmSMMUTestMappingCount];
    DMAIOMMUFault_t      Faults[ARM_SMMU_SELFTEST_FAULT_CAPACITY];
} ArmSMMUSelfTestState_t;

// The one test state record, defined in native.c. Its initializer names the
// two domains, the five blocks and the four mappings.
extern ArmSMMUSelfTestState_t g_armSMMUSelfTest;

/**
 * @brief Records the step that runs next, so a FAIL record names it.
 *
 * @param name Step name. Must be a string that stays valid for the whole boot.
 */
static inline void
__ArmSMMUSelfTestStep(
    _In_ const char* name)
{
    g_armSMMUSelfTest.Step = name;
}

/**
 * @brief Returns the physical address of the guard block. Both spaces use the
 * same number as their lowest IOVA, so IOVA offsets are added to it.
 *
 * Valid once setup has allocated the test pages.
 *
 * @return Physical address of the first guard page.
 */
static inline uint64_t
__ArmSMMUSelfTestGuardBase(void)
{
    return g_armSMMUSelfTest.Blocks[ArmSMMUTestBlockGuard].Memory.PhysicalAddress;
}

/*******************************************************************************
 * native_setup.c
 ******************************************************************************/

/**
 * @brief Brings the test from boot to a started unit with mapped and
 * connected domains.
 *
 * Call once from the worker thread. The steps run in this order: clock check,
 * IORT capture, endpoint and route discovery, unit start, test pages, address
 * spaces, mappings and connections. The order matters, see the function in
 * native_setup.c. Each step sets the step name for the FAIL record. Setup
 * stops at the first failure and keeps everything it created, since hardware
 * may still reference it.
 *
 * @param connectedDomains Number of domains to connect, starting with domain
 *                         A. Either 1 or ARM_SMMU_TEST_DEVICE_COUNT.
 * @return OS_EOK when every step passed. Otherwise the status of the step
 *         that failed, for example OS_ENOTSUPPORTED for a fixture that does
 *         not match or OS_EDEVFAULT for an unexpected fault record.
 */
KERNELABI oserr_t KERNELAPI
ArmSMMUSelfTestSetup(
    _In_ uint32_t connectedDomains);

/**
 * @brief Disconnects the domain's stream ID from its address space.
 *
 * The domain must be Connected and its endpoint idle. Takes the unit mutex
 * around each SMMUv3HardwareIDsDisconnect call and retries only OS_ETIMEOUT,
 * at most ARM_SMMU_SELFTEST_MAX_ATTEMPTS times. On success the domain becomes
 * Disconnected and a "STEP disconnect" record is printed. The connection
 * record stays until ArmSMMUSelfTestDestroyConnection.
 *
 * @param domain Connected domain.
 * @return OS_EOK on success, or the last status of the Disconnect callback.
 *         The domain state is unchanged on failure.
 */
KERNELABI oserr_t KERNELAPI
ArmSMMUSelfTestDisconnect(
    _InOut_ ArmSMMUTestDomain_t* domain);

/**
 * @brief Releases the stream ID reservation and the connection record.
 *
 * Valid only for a Prepared or Disconnected domain. Takes the unit mutex
 * around SMMUv3HardwareIDsDestroy. Afterwards the domain has no connection
 * record and is in the None state.
 *
 * @param domain Prepared or Disconnected domain.
 */
KERNELABI void KERNELAPI
ArmSMMUSelfTestDestroyConnection(
    _InOut_ ArmSMMUTestDomain_t* domain);

/**
 * @brief Disconnects the domain and connects it again with a new connection
 * record.
 *
 * The domain must be Connected and its endpoint idle. Sets the step to
 * "reconnect", then disconnects, destroys the old record, prepares a new one
 * and connects it. A disconnected record is never connected again.
 *
 * @param domain Connected domain.
 * @return OS_EOK when the domain is Connected again. Otherwise the status of
 *         the call that failed. The domain keeps the state it reached.
 */
KERNELABI oserr_t KERNELAPI
ArmSMMUSelfTestReconnect(
    _InOut_ ArmSMMUTestDomain_t* domain);

/*******************************************************************************
 * native_requests.c
 ******************************************************************************/

/**
 * @brief Runs one device write that must succeed and checks what it changed.
 *
 * The domain of the transfer must be Connected. Sets the step to the transfer
 * name, fills every observed page with the fill byte, sends the write and
 * prints its request, "poll" and "memory" records. The write passes when the
 * device result is 0, Poll returns OS_EOK with no fault record, the target
 * range holds the device pattern and every other observed byte still holds
 * the fill byte.
 *
 * @param transfer Transfer to run.
 * @return OS_EOK when the write passed. OS_EDEVFAULT for a wrong device
 *         result, a fault record or a wrong byte. Otherwise the status of the
 *         device protocol or of Poll.
 */
KERNELABI oserr_t KERNELAPI
ArmSMMUSelfTestTransfer(
    _In_ const ArmSMMUTestTransfer_t* transfer);

/**
 * @brief Runs one 4-byte device write that the SMMU must refuse, and checks
 * the fault record and the memory afterwards.
 *
 * Sets the step to the probe name, fills every observed page with the fill
 * byte and sends the write with its own IOVA as the readback address. Prints
 * the request and "poll" records, then the "FAULT expected" record when Poll
 * returned the right one, then the "memory" record. The probe passes when the
 * device result is ARM_SMMU_TEST_RESULT_TX_FAIL, Poll returns OS_EOK with the
 * record that the probe outcome asks for, and no observed byte changed.
 *
 * @param probe Probe to run.
 * @return OS_EOK when the write was refused as expected. OS_EDEVFAULT for a
 *         wrong device result, a missing or wrong fault record or a changed
 *         byte. Otherwise the status of the device protocol or of Poll.
 */
KERNELABI oserr_t KERNELAPI
ArmSMMUSelfTestProbe(
    _In_ const ArmSMMUTestProbe_t* probe);

/**
 * @brief Reads every pending fault record at a point where none is expected.
 *
 * The unit must exist. Each record found is printed as "FAULT unexpected".
 *
 * @return OS_EOK when Poll returned OS_EOK and no record. OS_EDEVFAULT when
 *         Poll returned records. Otherwise the Poll status.
 */
KERNELABI oserr_t KERNELAPI
ArmSMMUSelfTestCheckNoFaults(void);

/*******************************************************************************
 * native_cases.c
 *
 * The worker calls exactly one case function, after ArmSMMUSelfTestSetup
 * passed and before ArmSMMUSelfTestCleanup. Each one runs the requests of its
 * row in the T6 table, and stops at the first request that fails. The comment
 * on each function in native_cases.c says what the case proves.
 ******************************************************************************/

/**
 * @brief Runs the positive case. Setup must have connected both domains.
 *
 * @return OS_EOK when every request passed, or the status of the first
 *         request that failed.
 */
KERNELABI oserr_t KERNELAPI
ArmSMMUSelfTestRunPositive(void);

/**
 * @brief Runs the unmapped case. Setup must have connected both domains.
 *
 * @return OS_EOK when every request passed, or the status of the first
 *         request that failed.
 */
KERNELABI oserr_t KERNELAPI
ArmSMMUSelfTestRunUnmapped(void);

/**
 * @brief Runs the write-denied case. Setup must have connected both domains.
 *
 * @return OS_EOK when every request passed, or the status of the first
 *         request that failed.
 */
KERNELABI oserr_t KERNELAPI
ArmSMMUSelfTestRunWriteDenied(void);

/**
 * @brief Runs the unknown-stream case. Setup must have connected domain A
 * only.
 *
 * @return OS_EOK when every request passed, or the status of the first
 *         request that failed.
 */
KERNELABI oserr_t KERNELAPI
ArmSMMUSelfTestRunUnknownStream(void);

/**
 * @brief Runs the other-domain case. Setup must have connected both domains.
 *
 * @return OS_EOK when every request passed, or the status of the first
 *         request that failed.
 */
KERNELABI oserr_t KERNELAPI
ArmSMMUSelfTestRunOtherDomain(void);

/**
 * @brief Runs the after-unmap case. Setup must have connected both domains.
 *
 * @return OS_EOK when every request passed, or the status of the first
 *         request or release that failed.
 */
KERNELABI oserr_t KERNELAPI
ArmSMMUSelfTestRunAfterUnmap(void);

/**
 * @brief Runs the after-disconnect case. Setup must have connected both
 * domains.
 *
 * @return OS_EOK when every request passed, or the status of the first
 *         request or disconnect that failed.
 */
KERNELABI oserr_t KERNELAPI
ArmSMMUSelfTestRunAfterDisconnect(void);

/*******************************************************************************
 * native_cleanup.c
 ******************************************************************************/

/**
 * @brief Tears down everything setup created, in the reverse order of setup.
 *
 * Call once from the worker thread, after the case passed. Stops the
 * endpoints, ends every connection, releases the mappings, destroys the
 * spaces, polls once more, frees the pages and the arena and destroys the
 * unit. Prints "STEP cleanup complete" when every step passed.
 *
 * @return OS_EOK when everything was released. Otherwise the status of the
 *         step that failed. The later steps are skipped, so memory that
 *         hardware may still reference is kept.
 */
KERNELABI oserr_t KERNELAPI
ArmSMMUSelfTestCleanup(void);

/**
 * @brief Disarms both endpoints and clears memory decode and bus mastering.
 *
 * Safe at any point of the run, also before the endpoints were prepared,
 * because ArmSMMUTestDeviceDisable does nothing for an Empty record. Both
 * endpoints are tried even when the first one fails, and each failure is
 * logged.
 *
 * @return OS_EOK when both endpoints stopped, or the status of the last
 *         endpoint that failed.
 */
KERNELABI oserr_t KERNELAPI
ArmSMMUSelfTestStopDevices(void);

/**
 * @brief Releases one mapping, so its IOVA no longer reaches the block.
 *
 * Retries only OS_ETIMEOUT, at most ARM_SMMU_SELFTEST_MAX_ATTEMPTS times. A
 * mapping that was released earlier is NULL and needs no work. The block
 * behind the mapping stays allocated.
 *
 * @param mapping Mapping to release.
 * @return OS_EOK when the mapping is released, or the last status of
 *         DMAAddressMappingRelease.
 */
KERNELABI oserr_t KERNELAPI
ArmSMMUSelfTestReleaseMapping(
    _InOut_ ArmSMMUTestMapping_t* mapping);

#endif //!__ARM_SMMU_NATIVE_COORDINATOR_H__
