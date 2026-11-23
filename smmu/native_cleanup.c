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
 * Native SMMUv3 self-test: cleanup.
 * Compiled only with VALI_ARM64_SMMU_SELFTEST on generic AArch64.
 *
 * ArmSMMUSelfTestCleanup is the last stage of a run whose case passed. It
 * tears down in the reverse order of setup and prints "STEP cleanup complete"
 * when every step passed. A run that failed calls only
 * ArmSMMUSelfTestStopDevices from here and keeps everything else, because
 * hardware may still reference it. The after-unmap case also releases one
 * mapping early through ArmSMMUSelfTestReleaseMapping.
 */

#define __MODULE "SMMUTEST"

#include "native_coordinator.h"
#include <debug.h>

/*******************************************************************************
 * Cleanup
 ******************************************************************************/

// Releases one mapping, retrying only a timeout. A successful release has
// removed the stage-2 leaf entries and finished their TLB invalidation, so
// the IOVA no longer reaches the block. The block itself stays allocated.
// A mapping released earlier is NULL, which needs no work.
oserr_t
ArmSMMUSelfTestReleaseMapping(
    _InOut_ ArmSMMUTestMapping_t* mapping)
{
    oserr_t status = OS_ETIMEOUT;

    for (uint32_t attempt = 0; attempt < ARM_SMMU_SELFTEST_MAX_ATTEMPTS && status == OS_ETIMEOUT; attempt++) {
        status = DMAAddressMappingRelease(&mapping->Mapping);
    }

    if (status != OS_EOK) {
        ERROR("SMMU-SELFTEST: releasing mapping %s failed: %u", mapping->Name, status);
    }
    return status;
}

// Disarms both endpoints and clears memory decode and bus mastering.
oserr_t
ArmSMMUSelfTestStopDevices(void)
{
    oserr_t result = OS_EOK;
    oserr_t status;

    for (uint32_t i = 0; i < ARM_SMMU_TEST_DEVICE_COUNT; i++) {
        status = ArmSMMUTestDeviceDisable(&g_armSMMUSelfTest.Devices[i]);
        if (status != OS_EOK) {
            ERROR("SMMU-SELFTEST: stopping device %04x failed: %u", g_armSMMUSelfTest.Devices[i].RequesterId, status);
            result = status;
        }
    }
    return result;
}

// Ends every connection that still exists. A connected stream is disconnected
// first. A stream that a case already disconnected only needs its record
// destroyed. A domain that was never connected (unknown-stream) has nothing
// to clean up.
static oserr_t
__DisconnectDomains(void)
{
    oserr_t status;

    __ArmSMMUSelfTestStep("cleanup-disconnect");
    for (uint32_t i = 0; i < ARM_SMMU_TEST_DEVICE_COUNT; i++) {
        ArmSMMUTestDomain_t* domain = &g_armSMMUSelfTest.Domains[i];

        if (domain->ConnectionState == ArmSMMUTestConnectionConnected) {
            status = ArmSMMUSelfTestDisconnect(domain);
            if (status != OS_EOK) {
                return status;
            }
        }

        if (domain->ConnectionState == ArmSMMUTestConnectionPrepared ||
            domain->ConnectionState == ArmSMMUTestConnectionDisconnected) {
            ArmSMMUSelfTestDestroyConnection(domain);
        }
    }
    return OS_EOK;
}

// Releases every mapping that is still installed, so the data pages can be
// freed later.
static oserr_t
__ReleaseMappings(void)
{
    oserr_t status;

    __ArmSMMUSelfTestStep("cleanup-mappings");
    for (uint32_t i = 0; i < ArmSMMUTestMappingCount; i++) {
        status = ArmSMMUSelfTestReleaseMapping(&g_armSMMUSelfTest.Mappings[i]);
        if (status != OS_EOK) {
            return status;
        }
    }
    return OS_EOK;
}

static oserr_t
__DestroyDomains(void)
{
    oserr_t status;

    __ArmSMMUSelfTestStep("cleanup-spaces");
    for (uint32_t i = 0; i < ARM_SMMU_TEST_DEVICE_COUNT; i++) {
        status = DMAAddressSpaceDestroy(&g_armSMMUSelfTest.Domains[i].Space);
        if (status != OS_EOK) {
            ERROR("SMMU-SELFTEST: destroying %s failed: %u", g_armSMMUSelfTest.Domains[i].Name, status);
            return status;
        }
    }
    return OS_EOK;
}

static oserr_t
__FreePagesLocked(void)
{
    oserr_t status;

    for (uint32_t i = 0; i < ArmSMMUTestBlockCount; i++) {
        status = ArmSMMUMemoryFree(&g_armSMMUSelfTest.Memory, &g_armSMMUSelfTest.Blocks[i].Memory);
        if (status != OS_EOK) {
            ERROR("SMMU-SELFTEST: freeing %s failed: %u", g_armSMMUSelfTest.Blocks[i].Name, status);
            return status;
        }
    }

    status = ArmSMMUMemoryDestroy(&g_armSMMUSelfTest.Memory);
    if (status != OS_EOK) {
        ERROR("SMMU-SELFTEST: destroying the test arena failed: %u", status);
    }
    return status;
}

static oserr_t
__FreePages(void)
{
    oserr_t status;

    __ArmSMMUSelfTestStep("cleanup-pages");
    DMAIOMMUUnitLock(g_armSMMUSelfTest.Backend);
    status = __FreePagesLocked();
    DMAIOMMUUnitUnlock(g_armSMMUSelfTest.Backend);
    return status;
}

// Stops the SMMU and releases its control memory, retrying a timeout.
static oserr_t
__DestroyUnit(void)
{
    oserr_t status = OS_ETIMEOUT;

    __ArmSMMUSelfTestStep("cleanup-unit");
    for (uint32_t attempt = 0; attempt < ARM_SMMU_SELFTEST_MAX_ATTEMPTS && status == OS_ETIMEOUT; attempt++) {
        status = SMMUv3UnitDestroy(&g_armSMMUSelfTest.Unit);
    }

    if (status != OS_EOK) {
        ERROR("SMMU-SELFTEST: SMMUv3UnitDestroy failed: %u", status);
        return status;
    }
    g_armSMMUSelfTest.Backend = NULL;
    return OS_EOK;
}

// Tears everything down in the reverse order of setup. Each step must
// succeed before the next one frees memory that the previous step
// protected: devices stop before streams disconnect, streams disconnect
// before mappings are released, and pages are freed last.
oserr_t
ArmSMMUSelfTestCleanup(void)
{
    oserr_t status;

    __ArmSMMUSelfTestStep("cleanup-devices");
    status = ArmSMMUSelfTestStopDevices();
    if (status != OS_EOK) {
        return status;
    }

    status = __DisconnectDomains();
    if (status != OS_EOK) {
        return status;
    }

    status = __ReleaseMappings();
    if (status != OS_EOK) {
        return status;
    }

    status = __DestroyDomains();
    if (status != OS_EOK) {
        return status;
    }

    __ArmSMMUSelfTestStep("cleanup-poll");
    status = ArmSMMUSelfTestCheckNoFaults();
    if (status != OS_EOK) {
        return status;
    }

    status = __FreePages();
    if (status != OS_EOK) {
        return status;
    }

    status = __DestroyUnit();
    if (status != OS_EOK) {
        return status;
    }
    DEBUG("SMMU-SELFTEST STEP cleanup complete");
    return OS_EOK;
}
