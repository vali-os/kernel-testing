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

#include "dma_iommu_sim.h"
#include "../kernel/memory/dma/private.h"
#include <assert.h>
#include <heap.h>
#include <pthread.h>
#include <string.h>

// This deliberately small table format belongs only to the model. Dividing
// the granule index avoids multiplying a potentially large granule by 16.
#define __SIM_LEAF_ENTRIES 16U

struct __SimEntry {
    bool              Present;
    uint64_t          Physical;
    enum DMADirection Direction;
};

/** A shared table path whose cache survives removal of its table entries. */
struct __SimLeaf {
    struct __SimLeaf* Next;
    uint64_t         Index;
    struct __SimEntry Table[__SIM_LEAF_ENTRIES];
    struct __SimEntry Cache[__SIM_LEAF_ENTRIES];
};

/** A domain's private table/cache namespace, even when another uses the same IOVA. */
struct __SimSpace {
    DMAIOMMUSim_t*    Sim;
    struct __SimLeaf* Leaves;
};

struct __SimMapping;
struct __SimConnection;

enum __SimCommandState {
    __SimCommandFresh,
    __SimCommandPending,
    __SimCommandComplete
};

enum __SimCommandType {
    __SimMappingSync,
    __SimMappingInvalidate,
    __SimHardwareIDSync,
    __SimHardwareIDInvalidate
};

/** Embedded in its mapping or connection until it has left the queue. */
struct __SimCommand {
    enum __SimCommandState State;
    enum __SimCommandType  Type;
    union {
        struct __SimMapping*    Mapping;
        struct __SimConnection* Connection;
    };
    uint64_t               ReadyAt;
};

/** Entry progress and both command tokens survive timeout/error returns. */
struct __SimMapping {
    struct __SimSpace*             Space;
    const DMAIOMMUMappingConfig_t* Config;
    struct __SimLeaf*              Candidates;
    uint64_t                      Installed;
    uint64_t                      Removed;
    struct __SimCommand           Install;
    struct __SimCommand           Removal;
};

/** Reservations, programmed routes and cached routes have separate lifetimes. */
struct __SimHardwareID {
    uint32_t ID;
    bool     Installed;
    bool     Cached;
};

/** One complete hardware ID set, reserved on the unit until software cleanup. */
struct __SimConnection {
    struct __SimConnection* Next;
    struct __SimSpace*      Space;
    uint32_t               Count;
    uint32_t               Installed;
    uint32_t               Removed;
    bool                   ConnectStarted;
    struct __SimCommand    Connect;
    struct __SimCommand    Disconnect;
    struct __SimHardwareID  HardwareIDs[];
};

/** All fields except the immutable native unit pointer are guarded by Lock. */
struct DMAIOMMUSim {
    pthread_mutex_t        Lock;
    DMAIOMMUUnit_t*         Unit;
    DMAIOMMUSimConfig_t     Config;
    DMAIOMMUSimControl_t    Control;
    DMAIOMMUSimStats_t      Stats;
    struct __SimConnection* Connections;
    uint64_t                Now;
    struct __SimCommand**  Queue;
    uint32_t                Head;
    uint32_t                Count;
};

static void __Lock(DMAIOMMUSim_t* sim)
{
    assert(pthread_mutex_lock(&sim->Lock) == 0);
}

static void __Unlock(DMAIOMMUSim_t* sim)
{
    assert(pthread_mutex_unlock(&sim->Lock) == 0);
}

static oserr_t
__Step(DMAIOMMUSim_t* sim, enum DMAIOMMUSimFailurePoint point)
{
    oserr_t status;

    if (sim->Control.FailurePoint != point) {
        return OS_EOK;
    }
    if (sim->Control.AfterSteps != 0) {
        sim->Control.AfterSteps--;
        return OS_EOK;
    }
    status = sim->Control.FailureStatus;
    sim->Control.FailurePoint = DMAIOMMUSimFailureNone;
    if (sim->Control.FailUnit) {
        DMAIOMMUUnitFail(sim->Unit, status);
    }
    return status;
}

static struct __SimLeaf*
__FindLeaf(struct __SimLeaf* leaves, uint64_t index)
{
    while (leaves != NULL && leaves->Index != index) {
        leaves = leaves->Next;
    }
    return leaves;
}

static void
__FreeLeaves(DMAIOMMUSim_t* sim, struct __SimLeaf* leaves)
{
    while (leaves != NULL) {
        struct __SimLeaf* next = leaves->Next;

        for (unsigned int i = 0; i < __SIM_LEAF_ENTRIES; i++) {
            assert(!leaves->Table[i].Present && !leaves->Cache[i].Present);
        }
        assert(sim->Stats.TablePages != 0);
        sim->Stats.TablePages--;
        kfree(leaves);
        leaves = next;
    }
}

static oserr_t
__CreateSpace(void* context, void** spaceOut)
{
    DMAIOMMUSim_t*    sim = context;
    struct __SimSpace* space;

    __Lock(sim);
    space = kmalloc(sizeof(*space));
    if (space == NULL) {
        __Unlock(sim);
        return OS_EOOM;
    }
    space->Sim = sim;
    space->Leaves = NULL;
    sim->Stats.Spaces++;
    *spaceOut = space;
    __Unlock(sim);
    return OS_EOK;
}

static void
__DestroySpace(void* context, void* backendSpace)
{
    DMAIOMMUSim_t*     sim = context;
    struct __SimSpace* space = backendSpace;

    __Lock(sim);
    assert(space->Sim == sim && sim->Stats.Spaces != 0);
    for (struct __SimConnection* connection = sim->Connections; connection != NULL; connection = connection->Next) {
        assert(connection->Space != space);
    }
    __FreeLeaves(sim, space->Leaves);
    sim->Stats.Spaces--;
    kfree(space);
    __Unlock(sim);
}

static oserr_t
__PrepareMapping(void* backendSpace, const DMAIOMMUMappingConfig_t* config, void** mappingOut)
{
    struct __SimSpace*   space = backendSpace;
    DMAIOMMUSim_t*       sim = space->Sim;
    struct __SimMapping* mapping;
    uint64_t            first = config->DeviceAddress / sim->Config.Unit.Granularity;
    uint64_t            last = first + config->Length / sim->Config.Unit.Granularity - 1;

    __Lock(sim);
    mapping = kmalloc(sizeof(*mapping));
    if (mapping == NULL) {
        __Unlock(sim);
        return OS_EOOM;
    }
    memset(mapping, 0, sizeof(*mapping));
    mapping->Space = space;
    mapping->Config = config;
    mapping->Install.Mapping = mapping;
    mapping->Removal.Mapping = mapping;
    mapping->Removal.Type = __SimMappingInvalidate;

    // A missing path can also be prepared by another mapping. Keep private
    // candidates and resolve that race at Install under the native unit lock.
    for (uint64_t i = first / __SIM_LEAF_ENTRIES; i <= last / __SIM_LEAF_ENTRIES; i++) {
        struct __SimLeaf* leaf;

        if (__FindLeaf(space->Leaves, i) != NULL) {
            continue;
        }
        leaf = kmalloc(sizeof(*leaf));
        if (leaf == NULL) {
            __FreeLeaves(sim, mapping->Candidates);
            kfree(mapping);
            __Unlock(sim);
            return OS_EOOM;
        }
        memset(leaf, 0, sizeof(*leaf));
        leaf->Index = i;
        leaf->Next = mapping->Candidates;
        mapping->Candidates = leaf;
        sim->Stats.TablePages++;
    }
    sim->Stats.Mappings++;
    *mappingOut = mapping;
    __Unlock(sim);
    return OS_EOK;
}

static struct __SimLeaf*
__PublishLeaf(struct __SimMapping* mapping, uint64_t index)
{
    struct __SimLeaf*  leaf = __FindLeaf(mapping->Space->Leaves, index);
    struct __SimLeaf** link;

    if (leaf != NULL) {
        return leaf;
    }
    link = &mapping->Candidates;
    while (*link != NULL && (*link)->Index != index) {
        link = &(*link)->Next;
    }
    assert(*link != NULL);
    leaf = *link;
    *link = leaf->Next;
    leaf->Next = mapping->Space->Leaves;
    mapping->Space->Leaves = leaf;
    return leaf;
}

static oserr_t
__Submit(DMAIOMMUSim_t* sim, struct __SimCommand* command)
{
    uint32_t tail;
    oserr_t  status;

    if (command->State != __SimCommandFresh) {
        return OS_EOK;
    }
    if (sim->Count == sim->Config.QueueCapacity) {
        return OS_EBUSY;
    }
    if (sim->Control.CompletionDelayNs > UINT64_MAX - sim->Now) {
        return OS_EOVERFLOW;
    }
    status = __Step(sim, DMAIOMMUSimFailureSubmit);
    if (status != OS_EOK) {
        return status;
    }
    tail = (uint32_t)(((uint64_t)sim->Head + sim->Count) % sim->Config.QueueCapacity);
    command->ReadyAt = sim->Now + sim->Control.CompletionDelayNs;
    command->State = __SimCommandPending;
    sim->Queue[tail] = command;
    sim->Count++;
    sim->Stats.PendingCommands++;
    sim->Stats.SubmittedCommands++;
    return OS_EOK;
}

static void
__InvalidateMapping(DMAIOMMUSim_t* sim, struct __SimMapping* mapping)
{
    uint64_t            granule = sim->Config.Unit.Granularity;
    uint64_t            first = mapping->Config->DeviceAddress / granule;
    uint64_t            count = mapping->Config->Length / granule;

    for (struct __SimLeaf* leaf = mapping->Space->Leaves; leaf != NULL; leaf = leaf->Next) {
        for (unsigned int i = 0; i < __SIM_LEAF_ENTRIES; i++) {
            uint64_t index = leaf->Index * __SIM_LEAF_ENTRIES + i;

            if (index >= first && index - first < count && leaf->Cache[i].Present) {
                leaf->Cache[i].Present = false;
                assert(sim->Stats.CachedTranslations != 0);
                sim->Stats.CachedTranslations--;
            }
        }
    }
}

/** Process one ready command. Submission alone has no effect on either cache. */
static oserr_t
__CompleteHead(DMAIOMMUSim_t* sim)
{
    struct __SimCommand* command = sim->Queue[sim->Head];
    oserr_t             status;

    assert(command->State == __SimCommandPending && command->ReadyAt <= sim->Now);
    status = __Step(sim, DMAIOMMUSimFailureComplete);
    if (status != OS_EOK) {
        return status;
    }
    if (command->Type == __SimMappingInvalidate) {
        __InvalidateMapping(sim, command->Mapping);
    } else if (command->Type == __SimHardwareIDInvalidate) {
        struct __SimConnection* connection = command->Connection;

        assert(connection->Installed == connection->Removed);
        for (uint32_t i = 0; i < connection->Count; i++) {
            struct __SimHardwareID* hardwareID = &connection->HardwareIDs[i];

            assert(!hardwareID->Installed);
            if (hardwareID->Cached) {
                hardwareID->Cached = false;
                assert(sim->Stats.CachedHardwareIDs != 0);
                sim->Stats.CachedHardwareIDs--;
            }
        }
    }
    command->State = __SimCommandComplete;
    sim->Queue[sim->Head] = NULL;
    sim->Head = (uint32_t)(((uint64_t)sim->Head + 1) % sim->Config.QueueCapacity);
    sim->Count--;
    sim->Stats.PendingCommands--;
    sim->Stats.CompletedCommands++;
    return OS_EOK;
}

static oserr_t
__Deadline(DMAIOMMUSim_t* sim, uint64_t timeoutNs, uint64_t* deadlineOut)
{
    if (timeoutNs > UINT64_MAX - sim->Now) {
        return OS_EOVERFLOW;
    }
    *deadlineOut = sim->Now + timeoutNs;
    return OS_EOK;
}

/** Wait with one call budget, processing earlier commands before our token. */
static oserr_t
__Wait(DMAIOMMUSim_t* sim, struct __SimCommand* command, uint64_t deadline)
{
    while (command->State == __SimCommandPending) {
        struct __SimCommand* head = sim->Queue[sim->Head];
        oserr_t              status;

        assert(sim->Count != 0 && sim->Now <= deadline);
        if (sim->Control.HoldCompletions || head->ReadyAt > deadline) {
            sim->Now = deadline;
            return OS_ETIMEOUT;
        }
        if (head->ReadyAt > sim->Now) {
            sim->Now = head->ReadyAt;
        }
        status = __CompleteHead(sim);
        if (status != OS_EOK) {
            return status;
        }
    }
    return OS_EOK;
}

static oserr_t
__InstallMapping(void* backendSpace, void* backendMapping, uint64_t timeoutNs)
{
    struct __SimSpace*   space = backendSpace;
    struct __SimMapping* mapping = backendMapping;
    DMAIOMMUSim_t*       sim = space->Sim;
    uint64_t            granule = sim->Config.Unit.Granularity;
    uint64_t            first = mapping->Config->DeviceAddress / granule;
    uint64_t            deadline;
    oserr_t             status;

    __Lock(sim);
    assert(mapping->Space == space && mapping->Installed == 0);
    status = __Deadline(sim, timeoutNs, &deadline);
    if (status != OS_EOK) {
        goto done;
    }
    for (int i = 0; i < mapping->Config->ExtentCount; i++) {
        const SHMSG_t* extent = &mapping->Config->Extents[i];

        for (uint64_t offset = 0; offset < extent->Length; offset += granule) {
            uint64_t          index = first + mapping->Installed;
            struct __SimLeaf* leaf;
            struct __SimEntry* entry;

            status = __Step(sim, DMAIOMMUSimFailureInstallEntry);
            if (status != OS_EOK) {
                goto done;
            }
            leaf = __PublishLeaf(mapping, index / __SIM_LEAF_ENTRIES);
            entry = &leaf->Table[index % __SIM_LEAF_ENTRIES];
            assert(!entry->Present);
            *entry = (struct __SimEntry){ true, (uint64_t)extent->Address + offset,
                                          mapping->Config->Direction };
            mapping->Installed++;
        }
    }
    status = __Submit(sim, &mapping->Install);
    if (status != OS_EOK) {
        goto done;
    }
    status = __Wait(sim, &mapping->Install, deadline);

done:
    __Unlock(sim);
    return status;
}

static oserr_t
__RemoveMapping(void* backendSpace, void* backendMapping, uint64_t timeoutNs)
{
    struct __SimSpace*   space = backendSpace;
    struct __SimMapping* mapping = backendMapping;
    DMAIOMMUSim_t*       sim = space->Sim;
    uint64_t            first = mapping->Config->DeviceAddress / sim->Config.Unit.Granularity;
    uint64_t            deadline;
    oserr_t             status;

    __Lock(sim);
    assert(mapping->Space == space);
    status = __Deadline(sim, timeoutNs, &deadline);
    if (status != OS_EOK) {
        goto done;
    }

    // A timed-out installation may still have a descriptor referring to this
    // record. Drain it before removal, using the same deadline as invalidation.
    status = __Wait(sim, &mapping->Install, deadline);
    if (status != OS_EOK) {
        goto done;
    }
    while (mapping->Removed < mapping->Installed) {
        uint64_t          index = first + mapping->Removed;
        struct __SimLeaf* leaf = __FindLeaf(space->Leaves, index / __SIM_LEAF_ENTRIES);

        status = __Step(sim, DMAIOMMUSimFailureRemoveEntry);
        if (status != OS_EOK) {
            goto done;
        }
        assert(leaf != NULL && leaf->Table[index % __SIM_LEAF_ENTRIES].Present);
        leaf->Table[index % __SIM_LEAF_ENTRIES].Present = false;
        mapping->Removed++;
    }
    status = __Submit(sim, &mapping->Removal);
    if (status != OS_EOK) {
        goto done;
    }
    status = __Wait(sim, &mapping->Removal, deadline);

done:
    __Unlock(sim);
    return status;
}

static void
__DestroyMapping(void* backendSpace, void* backendMapping)
{
    struct __SimSpace*   space = backendSpace;
    struct __SimMapping* mapping = backendMapping;
    DMAIOMMUSim_t*       sim = space->Sim;

    __Lock(sim);
    assert(mapping->Space == space && sim->Stats.Mappings != 0);
    assert(mapping->Installed == mapping->Removed);
    assert(mapping->Install.State != __SimCommandPending);
    assert(mapping->Removal.State != __SimCommandPending);
    __FreeLeaves(sim, mapping->Candidates);
    sim->Stats.Mappings--;
    kfree(mapping);
    __Unlock(sim);
}

/** Includes prepared and disconnected records until DestroyHardwareIDs runs. */
static struct __SimConnection*
__FindHardwareID(DMAIOMMUSim_t* sim, uint32_t id, uint32_t* indexOut)
{
    for (struct __SimConnection* connection = sim->Connections; connection != NULL; connection = connection->Next) {
        for (uint32_t i = 0; i < connection->Count; i++) {
            if (connection->HardwareIDs[i].ID == id) {
                *indexOut = i;
                return connection;
            }
        }
    }
    return NULL;
}

static oserr_t
__PrepareHardwareIDs(void* backendSpace, const uint32_t* hardwareIDs,
                     uint32_t count, void** connectionOut)
{
    struct __SimSpace*      space = backendSpace;
    DMAIOMMUSim_t*          sim = space->Sim;
    struct __SimConnection* connection;
    uint32_t               index;
    size_t                 bytes;

    assert(count != 0 && hardwareIDs != NULL && *connectionOut == NULL);
    if ((uint64_t)count > (SIZE_MAX - sizeof(*connection)) / sizeof(*connection->HardwareIDs)) {
        return OS_EOVERFLOW;
    }
    __Lock(sim);
    for (uint32_t i = 0; i < count; i++) {
        if (__FindHardwareID(sim, hardwareIDs[i], &index) != NULL) {
            __Unlock(sim);
            return OS_EBUSY;
        }
        for (uint32_t j = 0; j < i; j++) {
            if (hardwareIDs[i] == hardwareIDs[j]) {
                __Unlock(sim);
                return OS_EINVALPARAMS;
            }
        }
    }
    bytes = sizeof(*connection) + count * sizeof(*connection->HardwareIDs);
    connection = kmalloc(bytes);
    if (connection == NULL) {
        __Unlock(sim);
        return OS_EOOM;
    }
    memset(connection, 0, bytes);
    connection->Space = space;
    connection->Count = count;
    connection->Connect.Type = __SimHardwareIDSync;
    connection->Connect.Connection = connection;
    connection->Disconnect.Type = __SimHardwareIDInvalidate;
    connection->Disconnect.Connection = connection;
    for (uint32_t i = 0; i < count; i++) {
        connection->HardwareIDs[i].ID = hardwareIDs[i];
    }
    connection->Next = sim->Connections;
    sim->Connections = connection;
    sim->Stats.Connections++;
    sim->Stats.ReservedHardwareIDs += count;
    *connectionOut = connection;
    __Unlock(sim);
    return OS_EOK;
}

static oserr_t
__ConnectHardwareIDs(void* backendSpace, void* backendConnection, uint64_t timeoutNs)
{
    struct __SimSpace*      space = backendSpace;
    struct __SimConnection* connection = backendConnection;
    DMAIOMMUSim_t*          sim = space->Sim;
    uint64_t               deadline;
    oserr_t                status;

    __Lock(sim);
    assert(connection->Space == space && !connection->ConnectStarted);
    connection->ConnectStarted = true;
    status = __Deadline(sim, timeoutNs, &deadline);
    if (status != OS_EOK) {
        goto done;
    }
    while (connection->Installed < connection->Count) {
        status = __Step(sim, DMAIOMMUSimFailureConnectEntry);
        if (status != OS_EOK) {
            goto done;
        }
        connection->HardwareIDs[connection->Installed].Installed = true;
        connection->Installed++;
        sim->Stats.InstalledHardwareIDs++;
    }
    status = __Submit(sim, &connection->Connect);
    if (status != OS_EOK) {
        goto done;
    }
    status = __Wait(sim, &connection->Connect, deadline);

done:
    __Unlock(sim);
    return status;
}

static oserr_t
__DisconnectHardwareIDs(void* backendSpace, void* backendConnection, uint64_t timeoutNs)
{
    struct __SimSpace*      space = backendSpace;
    struct __SimConnection* connection = backendConnection;
    DMAIOMMUSim_t*          sim = space->Sim;
    uint64_t               deadline;
    oserr_t                status;

    __Lock(sim);
    assert(connection->Space == space && connection->ConnectStarted);
    status = __Deadline(sim, timeoutNs, &deadline);
    if (status != OS_EOK) {
        goto done;
    }
    // Pending connect commands still refer to this record. Complete them before
    // changing routes, using the same deadline as the disconnect command.
    status = __Wait(sim, &connection->Connect, deadline);
    if (status != OS_EOK) {
        goto done;
    }
    while (connection->Removed < connection->Installed) {
        status = __Step(sim, DMAIOMMUSimFailureDisconnectEntry);
        if (status != OS_EOK) {
            goto done;
        }
        connection->HardwareIDs[connection->Removed].Installed = false;
        connection->Removed++;
        assert(sim->Stats.InstalledHardwareIDs != 0);
        sim->Stats.InstalledHardwareIDs--;
    }
    // Cached configurations can still select the old root. Only completion
    // clears those routes. Clearing the installed entries alone is insufficient.
    status = __Submit(sim, &connection->Disconnect);
    if (status != OS_EOK) {
        goto done;
    }
    status = __Wait(sim, &connection->Disconnect, deadline);

done:
    __Unlock(sim);
    return status;
}

static void
__DestroyHardwareIDs(void* backendSpace, void* backendConnection)
{
    struct __SimSpace*       space = backendSpace;
    struct __SimConnection*  connection = backendConnection;
    DMAIOMMUSim_t*           sim = space->Sim;
    struct __SimConnection** link;

    __Lock(sim);
    assert(connection->Space == space && sim->Stats.Connections != 0);
    assert(!connection->ConnectStarted || connection->Disconnect.State == __SimCommandComplete);
    assert(connection->Installed == connection->Removed);
    assert(connection->Connect.State != __SimCommandPending);
    assert(connection->Disconnect.State != __SimCommandPending);
    for (uint32_t i = 0; i < connection->Count; i++) {
        assert(!connection->HardwareIDs[i].Installed && !connection->HardwareIDs[i].Cached);
    }
    link = &sim->Connections;
    while (*link != connection) {
        assert(*link != NULL);
        link = &(*link)->Next;
    }
    *link = connection->Next;
    assert(sim->Stats.ReservedHardwareIDs >= connection->Count);
    sim->Stats.ReservedHardwareIDs -= connection->Count;
    sim->Stats.Connections--;
    kfree(connection);
    __Unlock(sim);
}

oserr_t
DMAIOMMUSimCreate(
    _In_    const DMAIOMMUSimConfig_t* config,
    _InOut_ DMAIOMMUSim_t**            simOut)
{
    DMAIOMMUSim_t* sim;
    oserr_t       status;
    const DMAIOMMUBackendOps_t ops = {
        __CreateSpace, __DestroySpace, __PrepareMapping,
        __InstallMapping, __RemoveMapping, __DestroyMapping,
        __PrepareHardwareIDs, __ConnectHardwareIDs, __DisconnectHardwareIDs, __DestroyHardwareIDs
    };

    if (simOut == NULL || config == NULL || config->QueueCapacity == 0) {
        return OS_EINVALPARAMS;
    }
    if (*simOut != NULL) {
        return OS_EBUSY;
    }
    if ((uint64_t)config->QueueCapacity > SIZE_MAX / sizeof(*sim->Queue)) {
        return OS_EOVERFLOW;
    }
    sim = kmalloc(sizeof(*sim));
    if (sim == NULL) {
        return OS_EOOM;
    }
    memset(sim, 0, sizeof(*sim));
    sim->Queue = kmalloc((size_t)config->QueueCapacity * sizeof(*sim->Queue));
    if (sim->Queue == NULL) {
        kfree(sim);
        return OS_EOOM;
    }
    memset(sim->Queue, 0, (size_t)config->QueueCapacity * sizeof(*sim->Queue));
    sim->Config = *config;
    assert(pthread_mutex_init(&sim->Lock, NULL) == 0);
    status = DMAIOMMUUnitCreate(&config->Unit, &ops, sim, &sim->Unit);
    if (status != OS_EOK) {
        assert(pthread_mutex_destroy(&sim->Lock) == 0);
        kfree(sim->Queue);
        kfree(sim);
        return status;
    }
    *simOut = sim;
    return OS_EOK;
}

DMAIOMMUUnit_t*
DMAIOMMUSimUnit(
    _In_ DMAIOMMUSim_t* sim)
{
    assert(sim != NULL);
    return sim->Unit;
}

oserr_t
DMAIOMMUSimDestroy(
    _InOut_ DMAIOMMUSim_t** sim)
{
    DMAIOMMUSim_t* released;
    oserr_t       status;

    if (sim == NULL || *sim == NULL) {
        return OS_EOK;
    }
    released = *sim;
    status = DMAIOMMUUnitDestroy(&released->Unit);
    if (status != OS_EOK) {
        return status;
    }
    assert(released->Stats.Spaces == 0 && released->Stats.Mappings == 0);
    assert(released->Stats.TablePages == 0 && released->Stats.CachedTranslations == 0);
    assert(released->Count == 0 && released->Stats.PendingCommands == 0);
    assert(released->Connections == NULL && released->Stats.Connections == 0);
    assert(released->Stats.ReservedHardwareIDs == 0 && released->Stats.InstalledHardwareIDs == 0);
    assert(released->Stats.CachedHardwareIDs == 0);
    assert(pthread_mutex_destroy(&released->Lock) == 0);
    kfree(released->Queue);
    kfree(released);
    *sim = NULL;
    return OS_EOK;
}

oserr_t
DMAIOMMUSimSetControl(
    _In_ DMAIOMMUSim_t*              sim,
    _In_ const DMAIOMMUSimControl_t* control)
{
    if (sim == NULL || control == NULL ||
        control->FailurePoint < DMAIOMMUSimFailureNone ||
        control->FailurePoint > DMAIOMMUSimFailureComplete ||
        (control->FailurePoint != DMAIOMMUSimFailureNone && control->FailureStatus == OS_EOK)) {
        return OS_EINVALPARAMS;
    }
    __Lock(sim);
    sim->Control = *control;
    __Unlock(sim);
    return OS_EOK;
}

oserr_t
DMAIOMMUSimAdvance(
    _In_ DMAIOMMUSim_t* sim,
    _In_ uint64_t       nanoseconds)
{
    oserr_t status;

    if (sim == NULL) {
        return OS_EINVALPARAMS;
    }
    __Lock(sim);
    if (nanoseconds > UINT64_MAX - sim->Now) {
        __Unlock(sim);
        return OS_EOVERFLOW;
    }
    sim->Now += nanoseconds;
    while (sim->Count != 0 && !sim->Control.HoldCompletions &&
           sim->Queue[sim->Head]->ReadyAt <= sim->Now) {
        status = __CompleteHead(sim);
        if (status != OS_EOK) {
            __Unlock(sim);
            return status;
        }
    }
    __Unlock(sim);
    return OS_EOK;
}

void
DMAIOMMUSimSnapshot(
    _In_  DMAIOMMUSim_t*      sim,
    _Out_ DMAIOMMUSimStats_t* statsOut)
{
    assert(sim != NULL && statsOut != NULL);
    __Lock(sim);
    *statsOut = sim->Stats;
    __Unlock(sim);
}

/** A device lookup can still hit the cache after the table entry was removed. */
static oserr_t
__Translate(struct __SimSpace* space, uint64_t address, enum DMADirection access, uint64_t* physicalOut)
{
    DMAIOMMUSim_t*    sim = space->Sim;
    uint64_t         index = address / sim->Config.Unit.Granularity;
    struct __SimLeaf* leaf = __FindLeaf(space->Leaves, index / __SIM_LEAF_ENTRIES);
    unsigned int     slot = (unsigned int)(index % __SIM_LEAF_ENTRIES);

    if (leaf == NULL) {
        return OS_ENOENT;
    }
    if (!leaf->Cache[slot].Present) {
        if (!leaf->Table[slot].Present) {
            return OS_ENOENT;
        }
        leaf->Cache[slot] = leaf->Table[slot];
        sim->Stats.CachedTranslations++;
    }
    if (leaf->Cache[slot].Direction != DMABidirectional && leaf->Cache[slot].Direction != access) {
        return OS_EPERMISSIONS;
    }
    *physicalOut = leaf->Cache[slot].Physical + (address & (sim->Config.Unit.Granularity - 1));
    return OS_EOK;
}

/** Two passes under the same lock keep caller output unchanged on errors. */
static oserr_t
__AccessSpans(struct __SimSpace* space, uint64_t address, uint64_t length,
              enum DMADirection access, SHMSG_t* spans, uint32_t* countOut)
{
    uint64_t granule = space->Sim->Config.Unit.Granularity;
    SHMSG_t  last = { 0 };
    uint32_t count = 0;

    while (length != 0) {
        uint64_t physical;
        uint64_t piece = granule - (address & (granule - 1));
        oserr_t  status;

        if (piece > length) {
            piece = length;
        }
        status = __Translate(space, address, access, &physical);
        if (status != OS_EOK) {
            return status;
        }
        if (physical > UINTPTR_MAX || piece > SIZE_MAX) {
            return OS_EOVERFLOW;
        }
        if (count != 0 && last.Length <= UINTPTR_MAX - last.Address &&
            last.Address + last.Length == physical) {
            if (piece > SIZE_MAX - last.Length) {
                return OS_EOVERFLOW;
            }
            last.Length += (size_t)piece;
        } else {
            if (count == UINT32_MAX) {
                return OS_EOVERFLOW;
            }
            last = (SHMSG_t){ (uintptr_t)physical, (size_t)piece };
            count++;
        }
        if (spans != NULL) {
            spans[count - 1] = last;
        }
        address += piece;
        length -= piece;
    }
    *countOut = count;
    return OS_EOK;
}

/** Read one root under the model lock, publishing spans only after validation. */
static oserr_t
__AccessRoot(struct __SimSpace* space, uint64_t address, uint64_t length,
              enum DMADirection access, SHMSG_t* spansOut, uint32_t capacity, uint32_t* countOut)
{
    uint32_t count;
    oserr_t  status;

    status = __AccessSpans(space, address, length, access, NULL, &count);
    if (status != OS_EOK) {
        return status;
    }
    if (capacity < count) {
        *countOut = count;
        return OS_EBUFFER;
    }
    status = __AccessSpans(space, address, length, access, spansOut, &count);
    if (status != OS_EOK) {
        return status;
    }
    *countOut = count;
    return OS_EOK;
}

oserr_t
DMAIOMMUSimAccess(
    _In_  DMAIOMMUSim_t*                sim,
    _In_  const struct DMAAddressSpace* space,
    _In_  uint64_t                      address,
    _In_  uint64_t                      length,
    _In_  enum DMADirection             access,
    _Out_ SHMSG_t*                      spansOut,
    _In_  uint32_t                      capacity,
    _Out_ uint32_t*                     countOut)
{
    struct __SimSpace* backend;
    oserr_t           status;

    if (sim == NULL || space == NULL || countOut == NULL ||
        space->Mode != DMAAddressSpaceTranslated || space->Translated.Unit != sim->Unit ||
        (spansOut == NULL && capacity != 0) || length == 0 || length - 1 > UINT64_MAX - address ||
        (access != DMAToDevice && access != DMAFromDevice && access != DMABidirectional)) {
        return OS_EINVALPARAMS;
    }
    // This is the only adapter to core-private space layout. From here on the
    // model consults its own table/cache records, never native mapping state.
    backend = space->Translated.BackendSpace;
    __Lock(sim);
    status = __AccessRoot(backend, address, length, access, spansOut, capacity, countOut);
    __Unlock(sim);
    return status;
}

oserr_t
DMAIOMMUSimAccessHardwareID(
    _In_  DMAIOMMUSim_t*    sim,
    _In_  uint32_t          hardwareID,
    _In_  uint64_t          address,
    _In_  uint64_t          length,
    _In_  enum DMADirection access,
    _Out_ SHMSG_t*          spansOut,
    _In_  uint32_t          capacity,
    _Out_ uint32_t*         countOut)
{
    struct __SimConnection* connection;
    struct __SimHardwareID* route;
    uint32_t               index;
    oserr_t                status;

    if (sim == NULL || countOut == NULL || (spansOut == NULL && capacity != 0) ||
        length == 0 || length - 1 > UINT64_MAX - address ||
        (access != DMAToDevice && access != DMAFromDevice && access != DMABidirectional)) {
        return OS_EINVALPARAMS;
    }
    __Lock(sim);
    connection = __FindHardwareID(sim, hardwareID, &index);
    if (connection == NULL) {
        __Unlock(sim);
        return OS_ENOENT;
    }
    route = &connection->HardwareIDs[index];
    if (!route->Cached) {
        if (!route->Installed) {
            __Unlock(sim);
            return OS_ENOENT;
        }
        route->Cached = true;
        sim->Stats.CachedHardwareIDs++;
    }
    // The connection keeps the root alive until disconnect completion. Software
    // Closing, manager loss and unit failure cannot remove this cached route.
    status = __AccessRoot(connection->Space, address, length, access, spansOut, capacity, countOut);
    __Unlock(sim);
    return status;
}
