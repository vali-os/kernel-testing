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
 * Native SMMUv3 self-test: the seven cases.
 * Compiled only with VALI_ARM64_SMMU_SELFTEST on generic AArch64.
 *
 * The worker runs one case function after setup and before cleanup. The case
 * table in native.c picks which one, from the case ID that CMake sets. Each
 * function follows its row in the T6 table. The tables at the top of this
 * file give every request its domain, its IOVA and the bytes it must change.
 * The requests themselves run through native_requests.c.
 */

#define __MODULE "SMMUTEST"

#include "native_coordinator.h"
#include <debug.h>

/*******************************************************************************
 * Request tables
 ******************************************************************************/

// Requests of the positive case, in the order of the T6 table row.
static const ArmSMMUTestTransfer_t g_writeOnly = {
    "write-only", 0, 0x100, ArmSMMUTestBlockDomainA, 0x100, 64
};

// Starts 128 bytes before the end of IOVA page 1 and ends 128 bytes into
// page 2, so the write uses two stage-2 leaf entries.
static const ArmSMMUTestTransfer_t g_pageBoundary = {
    "page-boundary", 0, 0x1F80, ArmSMMUTestBlockBoundary, 0xF80, 256
};

// The same IOVA in both domains, backed by different pages.
static const ArmSMMUTestTransfer_t g_sameIovaDomainB = {
    "same-iova-b", 1, 0x200, ArmSMMUTestBlockDomainB, 0x200, 128
};
static const ArmSMMUTestTransfer_t g_sameIovaDomainA = {
    "same-iova-a", 0, 0x200, ArmSMMUTestBlockDomainA, 0x200, 128
};

static const ArmSMMUTestTransfer_t g_afterReconnect = {
    "after-reconnect", 0, 0x300, ArmSMMUTestBlockDomainA, 0x300, 64
};

// Control transfers. Each negative case runs one of these on the endpoint
// it probes (or on the connected peer in unknown-stream) before the probe.
static const ArmSMMUTestTransfer_t g_controlA = {
    "control-a", 0, 0x100, ArmSMMUTestBlockDomainA, 0x100, 64
};
static const ArmSMMUTestTransfer_t g_controlB = {
    "control-b", 1, 0x100, ArmSMMUTestBlockDomainB, 0x100, 64
};

// Domain A writes the IOVA that other-domain probes from domain B, so the
// SMMU holds domain A's translation for it when domain B tries it.
static const ArmSMMUTestTransfer_t g_domainAPage2 = {
    "domain-a-page2", 0, 0x2100, ArmSMMUTestBlockBoundary, 0x1100, 64
};

// Follow-up transfers after a probe, one per domain. Both use mappings that
// no case removes: domain A's boundary mapping and domain B's data page.
static const ArmSMMUTestTransfer_t g_followUps[ARM_SMMU_TEST_DEVICE_COUNT] = {
    { "follow-up-a", 0, 0x1400, ArmSMMUTestBlockBoundary, 0x400, 64 },
    { "follow-up-b", 1, 0x0400, ArmSMMUTestBlockDomainB,  0x400, 64 }
};

// Probes, one per negative case. See the case functions for why each one
// must be refused and why its record is the expected one.
static const ArmSMMUTestProbe_t g_unmappedWrite = {
    "unmapped-write", 0, 0x3100, ArmSMMUTestProbeNoMapping
};
static const ArmSMMUTestProbe_t g_deniedWrite = {
    "denied-write", 1, 0x1100, ArmSMMUTestProbePermission
};
static const ArmSMMUTestProbe_t g_unconnectedWrite = {
    "unconnected-write", 1, 0x0100, ArmSMMUTestProbeAbort
};
static const ArmSMMUTestProbe_t g_otherDomainWrite = {
    "other-domain-write", 1, 0x2100, ArmSMMUTestProbeNoMapping
};
static const ArmSMMUTestProbe_t g_afterUnmapWrite = {
    "after-unmap-write", 0, 0x0100, ArmSMMUTestProbeNoMapping
};
static const ArmSMMUTestProbe_t g_afterDisconnectWrite = {
    "after-disconnect-write", 0, 0x0100, ArmSMMUTestProbeAbort
};

// Runs a follow-up transfer on every domain that is still connected. After a
// fault this is the endpoint that faulted and its peer. After an abort it is
// only the peer, because the probing endpoint has no connection. Both show
// that the unit kept translating after the refused write.
static oserr_t
__TransferOnConnectedDomains(void)
{
    oserr_t status;

    for (uint32_t i = 0; i < ARM_SMMU_TEST_DEVICE_COUNT; i++) {
        if (g_armSMMUSelfTest.Domains[i].ConnectionState != ArmSMMUTestConnectionConnected) {
            continue;
        }

        status = ArmSMMUSelfTestTransfer(&g_followUps[i]);
        if (status != OS_EOK) {
            return status;
        }
    }
    return OS_EOK;
}

/*******************************************************************************
 * Cases
 *
 * Each function follows its row in the T6 table. Setup has already connected
 * the domains named in the case table (native.c), and cleanup runs afterwards.
 ******************************************************************************/

// positive: translated writes succeed through a write-only and a read/write
// mapping, across a page boundary, from two domains that use the same IOVA
// with different backing, and again after a disconnect and reconnect.
oserr_t
ArmSMMUSelfTestRunPositive(void)
{
    oserr_t status;

    // A write-only (DMAFromDevice) mapping accepts a device write.
    status = ArmSMMUSelfTestTransfer(&g_writeOnly);
    if (status != OS_EOK) {
        return status;
    }

    // A read/write mapping accepts a write that crosses a page boundary.
    status = ArmSMMUSelfTestTransfer(&g_pageBoundary);
    if (status != OS_EOK) {
        return status;
    }

    // Two domains map the same IOVA to different pages. Each endpoint
    // reaches only its own page.
    status = ArmSMMUSelfTestTransfer(&g_sameIovaDomainB);
    if (status != OS_EOK) {
        return status;
    }

    status = ArmSMMUSelfTestTransfer(&g_sameIovaDomainA);
    if (status != OS_EOK) {
        return status;
    }

    // After a completed disconnect and a reconnect with a new record, the
    // endpoint transfers again.
    status = ArmSMMUSelfTestReconnect(&g_armSMMUSelfTest.Domains[0]);
    if (status != OS_EOK) {
        return status;
    }
    return ArmSMMUSelfTestTransfer(&g_afterReconnect);
}

// unmapped: domain A writes IOVA page 3, which no domain maps. Domain A's
// stage-2 table has no leaf entry for that page, so the SMMU refuses the
// write with a translation fault (event 0x10, F_TRANSLATION). Poll returns
// it as one NoMapping record with domain A's stream ID and the IOVA.
oserr_t
ArmSMMUSelfTestRunUnmapped(void)
{
    oserr_t status;

    status = ArmSMMUSelfTestTransfer(&g_controlA);
    if (status != OS_EOK) {
        return status;
    }

    status = ArmSMMUSelfTestProbe(&g_unmappedWrite);
    if (status != OS_EOK) {
        return status;
    }
    return __TransferOnConnectedDomains();
}

// write-denied: domain B writes IOVA page 1, which its DMAToDevice mapping
// lets the device read but not write. The leaf entry exists but does not
// allow writes, so the SMMU raises a permission fault (event 0x13,
// F_PERMISSION). Poll returns it as one Permission record, and the
// read-only data page and the guard keep the fill byte.
oserr_t
ArmSMMUSelfTestRunWriteDenied(void)
{
    oserr_t status;

    status = ArmSMMUSelfTestTransfer(&g_controlB);
    if (status != OS_EOK) {
        return status;
    }

    status = ArmSMMUSelfTestProbe(&g_deniedWrite);
    if (status != OS_EOK) {
        return status;
    }
    return __TransferOnConnectedDomains();
}

// unknown-stream: only domain A is connected. Domain B's stream ID lies in
// the same stream table page, which holds a valid abort entry for every
// stream without a connection. The SMMU drops domain B's write at that entry.
// QEMU records no event for it, and real hardware may record C_BAD_STREAMID
// (a NoRoute record). The control transfer runs on the connected peer, since
// the probing endpoint has no connection to test.
oserr_t
ArmSMMUSelfTestRunUnknownStream(void)
{
    oserr_t status;

    status = ArmSMMUSelfTestTransfer(&g_controlA);
    if (status != OS_EOK) {
        return status;
    }

    status = ArmSMMUSelfTestProbe(&g_unconnectedWrite);
    if (status != OS_EOK) {
        return status;
    }
    return __TransferOnConnectedDomains();
}

// other-domain: domain B writes IOVA page 2, which only domain A maps. Domain
// B first proves that its own mapping works. Domain A then writes the same
// IOVA, so the SMMU holds domain A's translation when domain B tries it. Each
// domain has its own stage-2 table and VMID, so domain B's write finds no
// leaf entry in its own table and faults with F_TRANSLATION (a NoMapping
// record for domain B's stream ID). Domain A's page keeps the fill byte.
oserr_t
ArmSMMUSelfTestRunOtherDomain(void)
{
    oserr_t status;

    status = ArmSMMUSelfTestTransfer(&g_controlB);
    if (status != OS_EOK) {
        return status;
    }

    status = ArmSMMUSelfTestTransfer(&g_domainAPage2);
    if (status != OS_EOK) {
        return status;
    }

    status = ArmSMMUSelfTestProbe(&g_otherDomainWrite);
    if (status != OS_EOK) {
        return status;
    }
    return __TransferOnConnectedDomains();
}

// after-unmap: domain A writes through its write-only mapping, which puts the
// translation in the SMMU's TLB. The endpoint is disarmed after every
// request. The mapping is then released while its page stays allocated, and
// the same IOVA is written again. The release removed the leaf entry and
// invalidated the TLB, so the write faults with F_TRANSLATION (a NoMapping
// record) and the old page keeps the fill byte.
oserr_t
ArmSMMUSelfTestRunAfterUnmap(void)
{
    ArmSMMUTestMapping_t* mapping = &g_armSMMUSelfTest.Mappings[ArmSMMUTestMappingWriteOnly];
    oserr_t               status;

    status = ArmSMMUSelfTestTransfer(&g_controlA);
    if (status != OS_EOK) {
        return status;
    }

    __ArmSMMUSelfTestStep("release");
    status = ArmSMMUSelfTestReleaseMapping(mapping);
    if (status != OS_EOK) {
        return status;
    }
    DEBUG("SMMU-SELFTEST STEP release %s iova=0x%llx", mapping->Name, mapping->DeviceAddress);

    status = ArmSMMUSelfTestProbe(&g_afterUnmapWrite);
    if (status != OS_EOK) {
        return status;
    }
    return __TransferOnConnectedDomains();
}

// after-disconnect: domain A writes through its mapping, which puts its
// stream table entry and translation in the SMMU's caches. Its connection is
// then completed as a disconnect while the mapping and pages stay, and the
// endpoint writes the same IOVA again. Disconnect wrote a valid abort entry
// and invalidated the cached entry, so the SMMU drops the write. QEMU records
// no event for it. Only domain B is still connected and transfers again.
oserr_t
ArmSMMUSelfTestRunAfterDisconnect(void)
{
    oserr_t status;

    status = ArmSMMUSelfTestTransfer(&g_controlA);
    if (status != OS_EOK) {
        return status;
    }

    __ArmSMMUSelfTestStep("disconnect");
    status = ArmSMMUSelfTestDisconnect(&g_armSMMUSelfTest.Domains[0]);
    if (status != OS_EOK) {
        return status;
    }

    status = ArmSMMUSelfTestProbe(&g_afterDisconnectWrite);
    if (status != OS_EOK) {
        return status;
    }
    return __TransferOnConnectedDomains();
}
