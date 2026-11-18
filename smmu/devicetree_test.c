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
 */

#include "../../kernel/arch/aarch64/smmu/discovery.h"
#include <assert.h>
#include <log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int g_allocations;
static bool         g_failAllocation;

void*
kmalloc(size_t size)
{
    void* allocation;

    if (g_failAllocation) {
        return NULL;
    }
    allocation = malloc(size);
    assert(allocation != NULL);
    g_allocations++;
    return allocation;
}

void
kfree(void* allocation)
{
    if (allocation != NULL) {
        assert(g_allocations != 0);
        g_allocations--;
        free(allocation);
    }
}

void
LogAppendMessage(enum OSSysLogLevel level, const char* format, ...)
{
    (void)level;
    (void)format;
}

static void
__CheckRoutes(
    FILE*                expected,
    DeviceTree_t*        tree,
    ArmSMMUDiscovery_t*  discovery)
{
    char                   path[256];
    ArmSMMUDeviceRoute_t    route;
    ArmSMMUDeviceRoute_t    unchanged;
    ArmSMMUDescription_t    description;
    const DeviceTreeNode_t* node;
    DeviceTreeNode_t        foreign = {0};
    unsigned int           segment;
    unsigned int           rid;
    unsigned int           error;
    unsigned int           unit;
    unsigned int           cache;
    unsigned int           count;
    unsigned int           requester;
    unsigned long long     limit;
    oserr_t                status;

    memset(&unchanged, 0xa5, sizeof(unchanged));
    route = unchanged;
    assert(ArmSMMUResolveDeviceTreeDevice(discovery, &foreign, &route) == OS_EINVALPARAMS);
    assert(memcmp(&route, &unchanged, sizeof(route)) == 0);
    assert(ArmSMMUResolveDeviceTreeDevice(discovery, NULL, &route) == OS_EINVALPARAMS);
    assert(ArmSMMUResolveDeviceTreeDevice(discovery, tree->Root, NULL) == OS_EINVALPARAMS);
    assert(ArmSMMUGetUnitDescription(discovery, UINT32_MAX, &description) == OS_ENOENT);

    // Expected values are generated independently of the production decoder.
    // '-' identifies a PCI query. Every failing lookup must preserve all bytes.
    while (fscanf(expected, "%255s %u %u %u %u %u %llu %u",
        path, &segment, &rid, &error, &unit, &cache, &limit, &count) == 8) {
        route = unchanged;
        if (!strcmp(path, "-")) {
            status = ArmSMMUResolvePci(discovery, segment, (uint16_t)rid, &route);
        } else {
            node = DeviceTreeFindPath(tree, path);
            assert(node != NULL);
            status = ArmSMMUResolveDeviceTreeDevice(discovery, node, &route);
        }
        assert(status == (oserr_t)error);
        if (status != OS_EOK) {
            assert(memcmp(&route, &unchanged, sizeof(route)) == 0);
            continue;
        }
        assert(route.UnitIndex == unit && route.CachePolicy == (enum SHMDeviceCachePolicy)cache);
        assert(route.AddressLimit == limit && route.RequesterCount == count);
        for (unsigned int i = 0; i < count; i++) {
            assert(fscanf(expected, "%u", &requester) == 1);
            assert(route.Requesters[i] == requester);
        }
    }
    assert(feof(expected));
}

int
main(int argc, char** argv)
{
    uint8_t*               blob;
    DeviceTree_t*          tree = NULL;
    ArmSMMUDiscovery_t*    discovery = NULL;
    ArmSMMUDiscovery_t*    occupied = (void*)1;
    ArmSMMUDescription_t   description;
    FILE*                 file;
    FILE*                 expected;
    size_t                size;
    long                  fileSize;
    unsigned int          error;
    unsigned int          units;
    unsigned int          kind;
    unsigned int          coherent;
    unsigned int          irq;
    unsigned int          flags;
    unsigned int          secondIrq;
    unsigned int          secondFlags;
    unsigned int          contexts;
    unsigned long long    base;
    unsigned long long    length;
    unsigned int          treeAllocations;
    oserr_t               status;

    assert(argc == 3);
    file = fopen(argv[1], "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    fileSize = ftell(file);
    assert(fileSize > 0 && fileSize <= 4 * 1024 * 1024);
    assert(fseek(file, 0, SEEK_SET) == 0);
    blob = malloc((size_t)fileSize);
    assert(blob != NULL);
    size = fread(blob, 1, (size_t)fileSize, file);
    assert(size == (size_t)fileSize);
    assert(!ferror(file) && fclose(file) == 0);
    assert(DeviceTreeCreate(blob, size, &tree) == OS_EOK);
    free(blob);
    treeAllocations = g_allocations;
    assert(ArmSMMUDiscoverDeviceTree(tree, &occupied) == OS_EBUSY && occupied == (void*)1);
    assert(ArmSMMUDiscoverDeviceTree(NULL, &discovery) == OS_EINVALPARAMS && discovery == NULL);
    assert(ArmSMMUDiscoverDeviceTree(tree, NULL) == OS_EINVALPARAMS);
    g_failAllocation = true;
    assert(ArmSMMUDiscoverDeviceTree(tree, &discovery) == OS_EOOM && discovery == NULL);
    g_failAllocation = false;
    expected = fopen(argv[2], "r");
    assert(expected != NULL && fscanf(expected, "%u %u", &error, &units) == 2);
    status = ArmSMMUDiscoverDeviceTree(tree, &discovery);
    assert(status == (oserr_t)error);
    if (status == OS_EOK) {
        assert(ArmSMMUGetUnitCount(discovery) == units);
        for (unsigned int i = 0; i < units; i++) {
            assert(fscanf(expected, "%u %llu %llu %u %u %u %u %u %u",
                &kind, &base, &length, &coherent, &irq, &flags, &secondIrq, &secondFlags, &contexts) == 9);
            assert(ArmSMMUGetUnitDescription(discovery, i, &description) == OS_EOK);
            assert(description.Kind == (enum ArmSMMUKind)kind);
            assert(description.RegisterBase == base && description.RegisterLength == length);
            assert(description.TableCoherent == (bool)coherent);
            if (kind == ArmSMMUv3) {
                assert(description.V3.EventQueue.Line == irq && description.V3.EventQueue.Flags == flags);
                assert(description.V3.GlobalError.Line == secondIrq);
                assert(description.V3.GlobalError.Flags == secondFlags);
            } else {
                assert(description.Mmu500.GlobalFault.Line == irq && description.Mmu500.GlobalFault.Flags == flags);
                assert(description.Mmu500.ConfigurationFault.Line == secondIrq);
                assert(description.Mmu500.ConfigurationFault.Flags == secondFlags);
                assert(description.Mmu500.ContextInterruptCount == contexts);
                for (unsigned int j = 0; j < contexts; j++) {
                    assert(fscanf(expected, "%u %u", &irq, &flags) == 2);
                    assert(description.Mmu500.ContextInterrupts[j].Line == irq);
                    assert(description.Mmu500.ContextInterrupts[j].Flags == flags);
                }
            }
        }
        __CheckRoutes(expected, tree, discovery);
        ArmSMMUDiscoveryDestroy(&discovery);
    }
    assert(discovery == NULL && g_allocations == treeAllocations);
    assert(fclose(expected) == 0);
    DeviceTreeDestroy(tree);
    assert(g_allocations == 0);
    return 0;
}

