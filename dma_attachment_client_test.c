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

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ddk/dma/attachment.h>

static oserr_t g_status;
static uint32_t g_calls;
static uint32_t g_capacity;
static uint32_t g_count;

// Each mock writes outputs even on error, as a raw syscall may do before
// publication fails. The real DDK functions must isolate their caller's buffers.
static oserr_t
__Create(
    _Out_ uuid_t* handle)
{
    g_calls++;
    *handle = 42;
    return g_status;
}

static oserr_t
__Register(
    _In_  uuid_t                                  manager,
    _In_  const OSDmaAttachmentParameters_t* parameters,
    _Out_ OSDmaAttachmentId_t*               attachment)
{
    assert(manager == 42 && parameters->Version == DMA_ATTACHMENT_VERSION);
    g_calls++;
    attachment->Value = 0x100000002ULL;
    return g_status;
}

static oserr_t
__Events(
    _In_  uuid_t                       manager,
    _Out_ OSDmaAttachmentEvent_t* entries,
    _In_  uint32_t                     capacity,
    _Out_ uint32_t*                    count)
{
    uint32_t i;

    assert(manager == 42 && capacity > 0 && capacity <= DMA_MANAGER_MAX_EVENTS);
    assert(g_count <= capacity);
    g_calls++;
    g_capacity = capacity;
    for (i = 0; i < g_count; i++) {
        entries[i] = (OSDmaAttachmentEvent_t) {
            .Attachment = i + 1, .Generation = 2, .Reasons = DMA_ATTACHMENT_IDLE
        };
    }
    *count = g_count;
    return g_status;
}

static oserr_t
__Remove(
    _In_ uuid_t                          manager,
    _In_ const OSDmaAttachmentId_t* attachment)
{
    assert(manager == 42 && attachment->Value == 0x100000002ULL);
    g_calls++;
    return g_status;
}

static oserr_t
__Bind(
    _In_  uuid_t                            manager,
    _In_  const OSDmaBindParameters_t* parameters,
    _Out_ OSDmaBindingResult_t*        result)
{
    assert(manager == 42 && parameters->Attachment == 0x100000001ULL);
    assert(parameters->Principal == 0x200000002ULL && parameters->RequestId == 0x300000003ULL);
    g_calls++;
    *result = (OSDmaBindingResult_t) { .Handle = 84, .Generation = 0x400000004ULL };
    return g_status;
}

static oserr_t
__PrepareStop(uuid_t manager, const OSDmaAttachmentId_t* attachment, OSDmaStopTicket_t* ticket)
{
    assert(manager == 42 && attachment->Value == 0x100000002ULL);
    g_calls++;
    *ticket = (OSDmaStopTicket_t) { 0x500000005ULL, 0x600000006ULL };
    return g_status;
}

static oserr_t
__CompleteStop(uuid_t manager, const OSDmaStopTicket_t* ticket)
{
    assert(manager == 42 && ticket->RecoveryGroup == 0x500000005ULL && ticket->Epoch == 0x600000006ULL);
    g_calls++;
    return g_status;
}

#define Syscall_DmaAttachmentPrepareStop __PrepareStop
#define Syscall_DmaAttachmentCompleteStop __CompleteStop
#define Syscall_DmaAttachmentBind __Bind
#define Syscall_DmaManagerCreate __Create
#define Syscall_DmaAttachmentRegister __Register
#define Syscall_DmaManagerReadEvents __Events
#define Syscall_DmaAttachmentRemove __Remove
#include "../librt/libddk/dma/attachment.c"

int
main(void)
{
    OSDmaAttachmentParameters_t parameters = { .Version = DMA_ATTACHMENT_VERSION };
    OSDmaAttachmentId_t id = { 123 };
    OSDmaAttachmentEvent_t events[33];
    OSDmaAttachmentEvent_t before[33];
    OSDmaStopTicket_t ticket = { 123, 456 };
    uuid_t handle = 123;
    uint32_t count = 123;
    OSDmaBindParameters_t bindingParameters = {
        .Attachment = 0x100000001ULL, .Principal = 0x200000002ULL, .RequestId = 0x300000003ULL
    };
    OSDmaBindingResult_t binding = { .Handle = 123, .Reserved = 456, .Generation = 789 };

    memset(events, 0x5a, sizeof(events));
    memcpy(before, events, sizeof(events));
    assert(DMAManagerCreate(NULL) == OS_EINVALPARAMS);
    assert(DMAAttachmentRegister(42, NULL, &id) == OS_EINVALPARAMS);
    assert(DMAAttachmentRegister(42, &parameters, NULL) == OS_EINVALPARAMS);
    assert(DMAManagerReadEvents(42, NULL, 1, &count) == OS_EINVALPARAMS);
    assert(DMAManagerReadEvents(42, events, 1, NULL) == OS_EINVALPARAMS);
    assert(DMAManagerReadEvents(42, events, 0, &count) == OS_EINVALPARAMS);
    assert(DMAAttachmentRemove(42, NULL) == OS_EINVALPARAMS);
    assert(DMAAttachmentBind(42, NULL, &binding) == OS_EINVALPARAMS);
    assert(DMAAttachmentBind(42, &bindingParameters, NULL) == OS_EINVALPARAMS);
    assert(g_calls == 0);

    g_status = OS_ECANCELLED;
    g_count = 32;
    assert(DMAManagerCreate(&handle) == OS_ECANCELLED && handle == 123);
    assert(DMAAttachmentRegister(42, &parameters, &id) == OS_ECANCELLED && id.Value == 123);
    assert(DMAManagerReadEvents(42, events, UINT32_MAX, &count) == OS_ECANCELLED);
    assert(count == 123 && memcmp(events, before, sizeof(events)) == 0);
    assert(g_capacity == DMA_MANAGER_MAX_EVENTS);

    assert(DMAAttachmentBind(42, &bindingParameters, &binding) == OS_ECANCELLED);
    assert(binding.Handle == 123 && binding.Reserved == 456 && binding.Generation == 789);

    g_status = OS_EOK;
    assert(DMAAttachmentBind(42, &bindingParameters, &binding) == OS_EOK);
    assert(binding.Handle == 84 && binding.Reserved == 0 && binding.Generation == 0x400000004ULL);
    assert(DMAManagerCreate(&handle) == OS_EOK && handle == 42);
    assert(DMAAttachmentRegister(handle, &parameters, &id) == OS_EOK && id.Value == 0x100000002ULL);
    assert(DMAManagerReadEvents(handle, events, 33, &count) == OS_EOK && count == 32);
    assert(events[0].Attachment == 1 && events[31].Attachment == 32);
    assert(memcmp(&events[32], &before[32], sizeof(*events)) == 0);
    memcpy(before, events, sizeof(events));
    g_count = 0;
    assert(DMAManagerReadEvents(handle, events, 1, &count) == OS_EOK && count == 0);
    assert(memcmp(events, before, sizeof(events)) == 0);
    assert(DMAAttachmentRemove(handle, &id) == OS_EOK);
    g_status = OS_EBUSY;
    assert(DMAAttachmentRemove(handle, &id) == OS_EBUSY);
    assert(DMAAttachmentPrepareStop(42, NULL, &ticket) == OS_EINVALPARAMS);
    assert(DMAAttachmentPrepareStop(42, &id, NULL) == OS_EINVALPARAMS);
    assert(DMAAttachmentCompleteStop(42, NULL) == OS_EINVALPARAMS);
    g_status = OS_ECANCELLED;
    assert(DMAAttachmentPrepareStop(42, &id, &ticket) == OS_ECANCELLED);
    assert(ticket.RecoveryGroup == 123 && ticket.Epoch == 456);
    g_status = OS_EOK;
    assert(DMAAttachmentPrepareStop(42, &id, &ticket) == OS_EOK);
    assert(DMAAttachmentCompleteStop(42, &ticket) == OS_EOK);
    g_status = OS_EBUSY;
    assert(DMAAttachmentCompleteStop(42, &ticket) == OS_EBUSY);
    puts("DMA attachment DDK: bounded reads and output preservation passed");
    return 0;
}
