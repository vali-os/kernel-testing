// Exercise token operations with the real handle tables and deferred cleanup.
#include "../kernel/handle.c"
#include "handle_set_stubs.h"
#ifndef GRANTS_SOURCE
#define GRANTS_SOURCE "../kernel/handle_grants.c"
#endif
#include GRANTS_SOURCE
#include "../kernel/memory/shm/view.c"
#undef __MODULE
#include "../kernel/api/handles.c"
#include "../librt/libos/grants.c"
#include <stdio.h>
#include <stdlib.h>

_Static_assert(sizeof(GrantTokenOptions_t) == 28, "Typed options layout");
_Static_assert(sizeof(GrantToken_t) == 56, "Typed native token layout");
_Static_assert(offsetof(GrantToken_t, ControlHandleID) == 0, "Control ID offset");
_Static_assert(offsetof(GrantToken_t, Version) == 4, "Version offset");
_Static_assert(offsetof(GrantToken_t, ExportGlobalHandleID) == 8, "Export ID offset");
_Static_assert(offsetof(GrantToken_t, Options) == 12, "Options offset");
_Static_assert(offsetof(GrantToken_t, Options.Flags) == 36, "Flags offset");
_Static_assert(offsetof(GrantToken_t, Secret) == 40, "Secret offset");
_Static_assert(GrantTokenScopeRead == 0x1 && GrantTokenScopeWrite == 0x2, "Scope flags");
_Static_assert(GrantTokenTargetTypeAny == 0 && GrantTokenTargetTypeSpecific == 1, "Target values");

static MSContextSecurity_t* g_currentContext;
static int                 g_failAllocationAfter = -1;
static size_t              g_liveAllocations;
static unsigned int        g_bufferDeletes;
static bool                g_failOptionsCopy;
static bool                g_changeOptionsAfterCopy;
static size_t              g_tokenCopyLimit = SIZE_MAX;
static unsigned int        g_optionsCopies;
static unsigned int        g_tokenCopies;

enum __CopyAction {
    __CopyKeep,
    __CopyClose,
    __CopyRevoke
};

static enum __CopyAction g_copyAction;
static bool              g_keepCopyReference;
static uuid_t            g_copyReference;
static oserr_t (*g_copyOverride)(void*, void*, size_t, bool);

MSContextSecurity_t*
MemorySpaceGetSecurityContext(void)
{
    return g_currentContext;
}

void*
kmalloc(
    _In_ size_t size)
{
    void* memory;

    if (g_failAllocationAfter == 0) {
        return NULL;
    }
    if (g_failAllocationAfter > 0) {
        g_failAllocationAfter--;
    }
    memory = malloc(size);
    if (memory != NULL) {
        g_liveAllocations++;
    }
    return memory;
}

void
kfree(
    _In_ void* memory)
{
    if (memory != NULL) {
        assert(g_liveAllocations != 0);
        g_liveAllocations--;
        free(memory);
    }
}

void*
dsalloc(
    _In_ size_t size)
{
    return malloc(size);
}

void
dsfree(
    _In_ void* memory)
{
    free(memory);
}

static void
__DrainJanitor(void)
{
    element_t* entry;

    while ((entry = queue_pop(&g_cleanQueue)) != NULL) {
        __CleanupHandle((struct ResourceHandle*)entry);
    }
}

static void
__DeleteBuffer(
    _In_ void* resource)
{
    (void)resource;
    g_bufferDeletes++;
}

static void
__AssertInvalidToken(
    _In_ const GrantToken_t* token)
{
    uuid_t handleID = 0x1234;

    assert(GrantTokenImport(token, GrantTokenResourceTypeSHM, GrantTokenScopeRead, NULL, &handleID) == OS_EINVALPARAMS);
    assert(handleID == 0x1234);
}

static void
__TestCopiedFields(
    _In_ const GrantToken_t* token)
{
    GrantToken_t changed;
    size_t       i;

    changed = *token;
    changed.Options.ResourceType = GrantTokenResourceTypeInvalid;
    __AssertInvalidToken(&changed);
    changed = *token;
    changed.Options.ResourceType = (GrantTokenResourceType_t)42;
    __AssertInvalidToken(&changed);
    changed = *token;
    changed.Version = 1;
    __AssertInvalidToken(&changed);
    changed.Version = 2;
    __AssertInvalidToken(&changed);
    changed = *token;
    changed.Version++;
    __AssertInvalidToken(&changed);
    changed = *token;
    changed.ControlHandleID++;
    __AssertInvalidToken(&changed);
    changed = *token;
    changed.ExportGlobalHandleID = UUID_INVALID;
    __AssertInvalidToken(&changed);
    changed = *token;
    changed.Options.TargetType = GrantTokenTargetTypeSpecific;
    __AssertInvalidToken(&changed);
    changed = *token;
    changed.Options.TargetGlobalHandleID++;
    __AssertInvalidToken(&changed);
    changed = *token;
    changed.Options.Flags ^= GrantTokenFlagsSingleUse;
    __AssertInvalidToken(&changed);
    changed = *token;
    changed.Options.Scope = GrantTokenScopeWrite;
    __AssertInvalidToken(&changed);
    changed = *token;
    changed.Options.Data.SHM.Offset++;
    __AssertInvalidToken(&changed);
    changed = *token;
    changed.Options.Data.SHM.Length++;
    __AssertInvalidToken(&changed);
    for (i = 0; i < sizeof(changed.Secret); i++) {
        changed = *token;
        changed.Secret[i] ^= 1;
        __AssertInvalidToken(&changed);
    }
}

static oserr_t
__CopyGrantUser(
    _In_ void*  userBuffer,
    _In_ void*  kernelBuffer,
    _In_ size_t length,
    _In_ bool   toUser)
{
    const GrantToken_t* token;
    uuid_t              imported = 0x1234;

    if (g_copyOverride != NULL) {
        return g_copyOverride(userBuffer, kernelBuffer, length, toUser);
    }

    if (userBuffer == NULL) {
        return OS_EINVALPARAMS;
    }

    if (!toUser) {
        g_optionsCopies++;
        assert(length == sizeof(GrantTokenOptions_t));
        if (g_failOptionsCopy) {
            return OS_EINVALPARAMS;
        }
        memcpy(kernelBuffer, userBuffer, length);
        if (g_changeOptionsAfterCopy) {
            ((GrantTokenOptions_t*)userBuffer)->Data.SHM.Length = 0;
        }
        return OS_EOK;
    }

    g_tokenCopies++;
    assert(length == sizeof(GrantToken_t));
    token = kernelBuffer;
    // Creation makes the export usable before the result is copied out.
    assert(GrantTokenImport(token, GrantTokenResourceTypeSHM, GrantTokenScopeRead, NULL, &imported) == OS_EOK);
    if (g_keepCopyReference) {
        g_copyReference = imported;
    } else {
        assert(ReleaseGlobalHandle(imported) == OS_EINCOMPLETE);
    }

    if (g_copyAction == __CopyClose) {
        assert(ScDestroyHandle(token->ControlHandleID) == OS_EOK);
    } else if (g_copyAction == __CopyRevoke) {
        assert(ScGrantTokenRevoke(token->ControlHandleID) == OS_EOK);
    }
    // Allow cleanup to finish before the syscall handles the copy result.
    __DrainJanitor();
    if (g_tokenCopyLimit <= length) {
        memcpy(userBuffer, kernelBuffer, g_tokenCopyLimit);
        return OS_EINVALPARAMS;
    }
    memcpy(userBuffer, kernelBuffer, length);
    return OS_EOK;
}

oserr_t
MemorySpaceCopyFromUser(
    _Out_ void*       kernelBuffer,
    _In_  const void* userBuffer,
    _In_  size_t      length)
{
    return __CopyGrantUser((void*)userBuffer, kernelBuffer, length, false);
}

oserr_t
MemorySpaceCopyToUser(
    _Out_ void*       userBuffer,
    _In_  const void* kernelBuffer,
    _In_  size_t      length)
{
    return __CopyGrantUser(userBuffer, (void*)kernelBuffer, length, true);
}

SCTYPE
syscall3(
    SCTYPE function,
    SCTYPE arg0,
    SCTYPE arg1,
    SCTYPE arg2)
{
    assert(function == TEST_GRANT_CREATE_NUMBER);
    return ScGrantTokenCreate(
        (uuid_t)arg0,
        (const GrantTokenOptions_t*)(uintptr_t)arg1,
        (GrantToken_t*)(uintptr_t)arg2
    );
}

SCTYPE
syscall1(
    SCTYPE function,
    SCTYPE arg0)
{
    assert(function == TEST_GRANT_REVOKE_NUMBER);
    return ScGrantTokenRevoke((uuid_t)arg0);
}

static void
__TestScopeFlags(
    _In_ uuid_t source)
{
    const unsigned int scopes[] = { 0, 1, 2, 3, 4, 5, 6, 7, UINT32_MAX };
    GrantTokenOptions_t options = {
        .ResourceType = GrantTokenResourceTypeSHM,
        .Data.SHM.Length = 4096
    };
    GrantToken_t        token;
    GrantToken_t        changed;
    uuid_t              imported;
    oserr_t             status;
    size_t              allowed;
    size_t              requested;

    // Only the three nonempty combinations of known bits may be exported.
    for (allowed = 0; allowed < sizeof(scopes) / sizeof(scopes[0]); allowed++) {
        options.Scope = (GrantTokenScope_t)scopes[allowed];
        status = GrantTokenCreate(source, &options, &token);
        if (scopes[allowed] == 0 || scopes[allowed] > 3) {
            assert(status == OS_EINVALPARAMS);
            assert(__LookupGlobalSafe(source)->References == 1);
            continue;
        }
        assert(status == OS_EOK);

        // Check reads, writes, combined requests and invalid masks independently.
        for (requested = 0; requested < sizeof(scopes) / sizeof(scopes[0]); requested++) {
            imported = 0x1234;
            status = GrantTokenImport(&token, GrantTokenResourceTypeSHM, (GrantTokenScope_t)scopes[requested], NULL, &imported);
            if (scopes[requested] >= 1 && scopes[requested] <= 3 &&
                (scopes[requested] & scopes[allowed]) == scopes[requested]) {
                assert(status == OS_EOK);
                assert(imported == source);
                assert(ReleaseGlobalHandle(imported) == OS_EINCOMPLETE);
            } else {
                assert(status == OS_EPERMISSIONS);
                assert(imported == 0x1234);
            }
            assert(__LookupGlobalSafe(source)->References == 2);
        }

        // Requested access may be smaller; copied token policy must stay exact.
        changed = token;
        changed.Options.Scope = options.Scope == GrantTokenScopeRead ?
            GrantTokenScopeRead | GrantTokenScopeWrite : GrantTokenScopeRead;
        __AssertInvalidToken(&changed);
        assert(GrantTokenRevoke(token.ControlHandleID) == OS_EOK);
        __DrainJanitor();
        assert(__LookupGlobalSafe(source)->References == 1);
    }
}

static void
__TestSyscalls(
    _In_ uuid_t source)
{
    GrantTokenOptions_t options = {
        .ResourceType = GrantTokenResourceTypeSHM,
        .Scope = GrantTokenScopeRead,
        .Data.SHM.Length = 4096
    };
    GrantToken_t        token;
    GrantToken_t        unchanged;
    uuid_t              imported;
    uuid_t              nextID;
    unsigned int        copies;
    size_t              prefix;
    void*               resource;
    int                 failure;

    memset(&unchanged, 0xA5, sizeof(unchanged));
    token = unchanged;
    nextID = atomic_load(&g_nextHandleId);
    g_failOptionsCopy = true;
    assert(OSGrantTokenCreate(source, &options, &token) == OS_EINVALPARAMS);
    g_failOptionsCopy = false;
    assert(memcmp(&token, &unchanged, sizeof(token)) == 0);
    assert(atomic_load(&g_nextHandleId) == nextID);
    assert(OSGrantTokenCreate(source, NULL, &token) == OS_EINVALPARAMS);
    assert(OSGrantTokenCreate(source, &options, NULL) == OS_EINVALPARAMS);
    assert(__LookupGlobalSafe(source)->References == 1);
    __DrainJanitor();

    // Creation failures must not copy a result or leave an export reference.
    copies = g_tokenCopies;
    for (failure = 0; failure < 2; failure++) {
        token = unchanged;
        g_failAllocationAfter = failure;
        assert(OSGrantTokenCreate(source, &options, &token) == OS_EOOM);
        g_failAllocationAfter = -1;
        assert(memcmp(&token, &unchanged, sizeof(token)) == 0);
        assert(g_tokenCopies == copies);
        assert(__LookupGlobalSafe(source)->References == 1);
    }

    // Fault after every possible output prefix, including a complete copy.
    // The syscall must cancel the export even when all token bytes escaped.
    for (prefix = 0; prefix <= sizeof(token); prefix++) {
        token = unchanged;
        g_tokenCopyLimit = prefix;
        assert(OSGrantTokenCreate(source, &options, &token) == OS_EINVALPARAMS);
        assert(__LookupGlobalSafe(source)->References == 1);
        __AssertInvalidToken(&token);
        __DrainJanitor();
        assert(g_handles.element_count == 1);
    }
    g_tokenCopyLimit = SIZE_MAX;

    // A reference verified during copy-out survives cancellation on copy fault.
    g_keepCopyReference = true;
    g_tokenCopyLimit = sizeof(token);
    assert(OSGrantTokenCreate(source, &options, &token) == OS_EINVALPARAMS);
    g_keepCopyReference = false;
    g_tokenCopyLimit = SIZE_MAX;
    assert(g_copyReference == source);
    assert(__LookupGlobalSafe(source)->References == 2);
    __AssertInvalidToken(&token);
    assert(ReleaseGlobalHandle(g_copyReference) == OS_EINCOMPLETE);
    __DrainJanitor();
    assert(__LookupGlobalSafe(source)->References == 1);

    // Close or revoke during copy-out does not change the copy result. A copy
    // fault still reports its original error if cleanup finds no control grant.
    for (g_copyAction = __CopyClose; g_copyAction <= __CopyRevoke; g_copyAction++) {
        assert(OSGrantTokenCreate(source, &options, &token) == OS_EOK);
        assert(__LookupGlobalSafe(source)->References == 1);
        __AssertInvalidToken(&token);

        g_tokenCopyLimit = sizeof(token);
        assert(OSGrantTokenCreate(source, &options, &token) == OS_EINVALPARAMS);
        assert(__LookupGlobalSafe(source)->References == 1);
        __AssertInvalidToken(&token);
        g_tokenCopyLimit = SIZE_MAX;
    }
    g_copyAction = __CopyKeep;

    // Mutating userspace options after the initial copy cannot change policy.
    copies = g_optionsCopies;
    g_changeOptionsAfterCopy = true;
    assert(OSGrantTokenCreate(source, &options, &token) == OS_EOK);
    g_changeOptionsAfterCopy = false;
    assert(g_optionsCopies == copies + 1);
    assert(options.Data.SHM.Length == 0 && token.Options.Data.SHM.Length == 4096);
    assert(GrantTokenImport(&token, GrantTokenResourceTypeSHM, GrantTokenScopeRead, NULL, &imported) == OS_EOK);
    assert(OSGrantTokenRevoke(token.ControlHandleID) == OS_EOK);
    assert(OSGrantTokenRevoke(token.ControlHandleID) == OS_ENOENT);
    __AssertInvalidToken(&token);
    assert(ReleaseGlobalHandle(imported) == OS_EINCOMPLETE);
    __DrainJanitor();
    assert(__LookupGlobalSafe(source)->References == 1);

    // Revoke may disable a record before its control grant is removed.
    options.Data.SHM.Length = 4096;
    assert(GrantTokenCreate(source, &options, &token) == OS_EOK);
    resource = LookupHandle(token.ControlHandleID, HandleTypeGrantToken);
    __GrantTokenDisable(resource);
    assert(GrantTokenImport(&token, GrantTokenResourceTypeSHM, GrantTokenScopeRead, NULL, &imported) == OS_ECANCELLED);
    assert(OSGrantTokenRevoke(token.ControlHandleID) == OS_EOK);

    // A record held independently remains disabled after its control closes.
    assert(GrantTokenCreate(source, &options, &token) == OS_EOK);
    assert(AcquireGlobalHandle(token.ControlHandleID, HandleTypeGrantToken, &resource) == OS_EOK);
    assert(ScDestroyHandle(token.ControlHandleID) == OS_EOK);
    assert(((struct __GrantTokenRecord*)resource)->Status == GrantTokenStatusRevoked);
    assert(GrantTokenImport(&token, GrantTokenResourceTypeSHM, GrantTokenScopeRead, NULL, &imported) == OS_ECANCELLED);
    assert(ReleaseGlobalHandle(token.ControlHandleID) == OS_EOK);
    __DrainJanitor();
    assert(g_handles.element_count == 1);
    assert(__LookupGlobalSafe(source)->References == 1);
    assert(g_tokenCopies > sizeof(token));
}

static void
__TestSingleUse(
    _In_ uuid_t source)
{
    GrantTokenOptions_t options = {
        .ResourceType = GrantTokenResourceTypeSHM,
        .Scope = GrantTokenScopeRead,
        .Data.SHM.Length = 4096,
        .Flags = GrantTokenFlagsSingleUse
    };
    GrantToken_t token;
    GrantToken_t changed;
    uuid_t       first = 0x1234;
    uuid_t       second = 0x1234;
    void*        record;
    size_t       allocations = g_liveAllocations;

    assert(GrantTokenCreate(source, &options, &token) == OS_EOK);
    changed = token;
    changed.Options.Flags = 0;
    __AssertInvalidToken(&changed);

    // The first import uses up the token. The export's source reference is
    // handed to the caller, so the reference count does not change.
    assert(GrantTokenImport(&token, GrantTokenResourceTypeSHM, GrantTokenScopeRead, NULL, &first) == OS_EOK);
    assert(first == source);
    assert(__LookupGlobalSafe(source)->References == 2);

    // A second import must neither receive a source reference nor change the
    // caller's output.
    assert(GrantTokenImport(&token, GrantTokenResourceTypeSHM, GrantTokenScopeRead, NULL, &second) == OS_ECANCELLED);
    assert(second == 0x1234);
    assert(__LookupGlobalSafe(source)->References == 2);

    // Using up the token does not close its control handle. Revoking it must
    // not release the reference that was handed to the importer.
    assert(GrantTokenRevoke(token.ControlHandleID) == OS_EOK);
    __DrainJanitor();
    assert(__LookupGlobalSafe(source)->References == 2);
    assert(ReleaseGlobalHandle(first) == OS_EINCOMPLETE);
    __DrainJanitor();
    assert(__LookupGlobalSafe(source)->References == 1);
    assert(g_liveAllocations == allocations);

    // A revoke before the import has already released the export's source
    // reference, so there is nothing left to hand out. Hold the record so the
    // import reaches the status check instead of failing the lookup.
    assert(GrantTokenCreate(source, &options, &token) == OS_EOK);
    assert(AcquireGlobalHandle(token.ExportGlobalHandleID, HandleTypeGrantToken, &record) == OS_EOK);
    assert(GrantTokenRevoke(token.ControlHandleID) == OS_EOK);
    assert(__LookupGlobalSafe(source)->References == 1);
    assert(GrantTokenImport(&token, GrantTokenResourceTypeSHM, GrantTokenScopeRead, NULL, &second) == OS_ECANCELLED);
    assert(second == 0x1234);
    assert(__LookupGlobalSafe(source)->References == 1);
    assert(ReleaseGlobalHandle(token.ExportGlobalHandleID) == OS_EOK);
    __DrainJanitor();
    __AssertInvalidToken(&token);
    assert(g_liveAllocations == allocations);

    // Reusable tokens add a new source reference for each import and stay
    // active until revoked. The export keeps its own reference until then.
    options.Flags = 0;
    assert(GrantTokenCreate(source, &options, &token) == OS_EOK);
    assert(GrantTokenImport(&token, GrantTokenResourceTypeSHM, GrantTokenScopeRead, NULL, &first) == OS_EOK);
    assert(GrantTokenImport(&token, GrantTokenResourceTypeSHM, GrantTokenScopeRead, NULL, &second) == OS_EOK);
    assert(first == source && second == source);
    assert(__LookupGlobalSafe(source)->References == 4);
    ReleaseGlobalHandle(first);
    ReleaseGlobalHandle(second);
    assert(__LookupGlobalSafe(source)->References == 2);
    assert(GrantTokenRevoke(token.ControlHandleID) == OS_EOK);
    __DrainJanitor();
    assert(__LookupGlobalSafe(source)->References == 1);
    assert(g_liveAllocations == allocations);
}

int
main(void)
{
    MSContextSecurity_t first = { .GlobalHandleID = 101 };
    MSContextSecurity_t second = { .GlobalHandleID = 102 };
    struct SHMBuffer    buffer = { .Length = 4096 };
    GrantTokenOptions_t options = {
        .ResourceType = GrantTokenResourceTypeSHM,
        .Scope = GrantTokenScopeRead,
        .Data.SHM.Length = 4096
    };
    GrantTokenOptions_t changed;
    GrantToken_t        token;
    GrantToken_t        restricted;
    GrantToken_t        unchanged;
    uuid_t              source;
    uuid_t              imported;
    uuid_t              output;
    uuid_t              nextID;
    void*               resource;
    int                 failure;

    assert(InitializeHandles() == OS_EOK);
    first.HandleTable = CreateHandleGrantsTable();
    second.HandleTable = CreateHandleGrantsTable();
    assert(first.HandleTable != NULL && second.HandleTable != NULL);
    g_currentContext = &first;
    source = CreateHandle(HandleTypeSHM, NULL, __DeleteBuffer, &buffer);
    assert(source != UUID_INVALID);
    buffer.ID = source;
    output = 0x1234;
    __TestSyscalls(source);
    __TestScopeFlags(source);
    __TestSingleUse(source);

    // Fail record allocation, handle allocation and ID exhaustion without
    // leaking the export reference or changing the output token.
    memset(&unchanged, 0xA5, sizeof(unchanged));
    for (failure = 0; failure < 2; failure++) {
        token = unchanged;
        g_failAllocationAfter = failure;
        assert(GrantTokenCreate(source, &options, &token) == OS_EOOM);
        g_failAllocationAfter = -1;
        assert(memcmp(&token, &unchanged, sizeof(token)) == 0);
        assert(__LookupGlobalSafe(source)->References == 1);
    }
    nextID = atomic_load(&g_nextHandleId);
    atomic_store(&g_nextHandleId, UUID_INVALID);
    assert(GrantTokenCreate(source, &options, &token) == OS_EOOM);
    atomic_store(&g_nextHandleId, nextID);
    assert(__LookupGlobalSafe(source)->References == 1);

    // Unsupported categories must fail before creating an export or source reference.
    token = unchanged;
    changed = options;
    changed.ResourceType = GrantTokenResourceTypeInvalid;
    assert(GrantTokenCreate(source, &changed, &token) == OS_ENOTSUPPORTED);
    changed.ResourceType = (GrantTokenResourceType_t)42;
    assert(GrantTokenCreate(source, &changed, &token) == OS_ENOTSUPPORTED);
    assert(memcmp(&token, &unchanged, sizeof(token)) == 0);
    assert(__LookupGlobalSafe(source)->References == 1);
    assert(atomic_load(&g_nextHandleId) == nextID);

    assert(GrantTokenCreate(source, NULL, &token) == OS_EINVALPARAMS);
    assert(GrantTokenCreate(source, &options, NULL) == OS_EINVALPARAMS);
    changed = options;
    changed.Data.SHM.Length = 0;
    assert(GrantTokenCreate(source, &changed, &token) == OS_EINVALPARAMS);
    changed = options;
    changed.Data.SHM.Offset = UINT32_MAX;
    assert(GrantTokenCreate(source, &changed, &token) == OS_EINVALPARAMS);
    changed = options;
    changed.Data.SHM.Length = UINT32_MAX;
    assert(GrantTokenCreate(source, &changed, &token) == OS_EINVALPARAMS);
    changed = options;
    changed.TargetType = 42;
    assert(GrantTokenCreate(source, &changed, &token) == OS_EINVALPARAMS);
    changed = options;
    changed.Flags = 2;
    assert(GrantTokenCreate(source, &changed, &token) == OS_EINVALPARAMS);
    changed = options;
    changed.Scope = 42;
    assert(GrantTokenCreate(source, &changed, &token) == OS_EINVALPARAMS);
    changed = options;
    changed.TargetGlobalHandleID = second.GlobalHandleID;
    assert(GrantTokenCreate(source, &changed, &token) == OS_EINVALPARAMS);
    changed = options;
    changed.TargetType = GrantTokenTargetTypeSpecific;
    assert(GrantTokenCreate(source, &changed, &token) == OS_EINVALPARAMS);
    buffer.Flags = SHM_PRIVATE;
    assert(GrantTokenCreate(source, &options, &token) == OS_EPERMISSIONS);
    buffer.Flags = 0;

    assert(GrantTokenCreate(source, &options, &token) == OS_EOK);
    assert(token.ControlHandleID == token.ExportGlobalHandleID);
    assert(atomic_load(&g_nextHandleId) == token.ControlHandleID + 1);
    assert(GrantHandle(&second, token.ControlHandleID) == OS_EPERMISSIONS);
    assert(GrantTokenCreate(token.ControlHandleID, &options, &restricted) == OS_EUNKNOWN);
    restricted = token;
    restricted.ControlHandleID = source;
    restricted.ExportGlobalHandleID = source;
    __AssertInvalidToken(&restricted);

    g_currentContext = &second;
    assert(GrantTokenCreate(source, &options, &restricted) == OS_ENOENT);
    assert(OSGrantTokenRevoke(token.ControlHandleID) == OS_ENOENT);
    assert(GrantTokenImport(NULL, GrantTokenResourceTypeSHM, GrantTokenScopeRead, NULL, &output) == OS_EINVALPARAMS);
    assert(GrantTokenImport(&token, GrantTokenResourceTypeSHM, GrantTokenScopeRead, NULL, NULL) == OS_EINVALPARAMS);
    __TestCopiedFields(&token);
    assert(GrantTokenImport(&token, GrantTokenResourceTypeInvalid, GrantTokenScopeRead, NULL, &output) == OS_EINVALPARAMS);
    assert(GrantTokenImport(&token, (GrantTokenResourceType_t)42, GrantTokenScopeRead, NULL, &output) == OS_EINVALPARAMS);
    assert(GrantTokenImport(&token, GrantTokenResourceTypeSHM, GrantTokenScopeWrite, NULL, &output) == OS_EPERMISSIONS);
    assert(GrantTokenImport(&token, GrantTokenResourceTypeSHM, 42, NULL, &output) == OS_EPERMISSIONS);
    assert(output == 0x1234);
    __LookupGlobalSafe(source)->References = INT_MAX;
    assert(GrantTokenImport(&token, GrantTokenResourceTypeSHM, GrantTokenScopeRead, NULL, &output) == OS_EOVERFLOW);
    assert(output == 0x1234);
    __LookupGlobalSafe(source)->References = 2;
    assert(GrantTokenImport(&token, GrantTokenResourceTypeSHM, GrantTokenScopeRead, NULL, &imported) == OS_EOK);
    assert(imported == source);
    assert(LookupHandle(imported, HandleTypeSHM) == NULL);

    // Import-before-revoke keeps its independent resource reference. Hold the
    // record to test revoke-before-verify while its secret is still available.
    g_currentContext = &first;
    assert(AcquireGlobalHandle(token.ExportGlobalHandleID, HandleTypeGrantToken, &resource) == OS_EOK);
    assert(DestroyHandle(source) == OS_EOK);
    assert(GrantTokenRevoke(token.ControlHandleID) == OS_EOK);
    assert(GrantTokenImport(&token, GrantTokenResourceTypeSHM, GrantTokenScopeRead, NULL, &output) == OS_ECANCELLED);
    __TestCopiedFields(&token);
    assert(GrantTokenImport(&token, GrantTokenResourceTypeInvalid, GrantTokenScopeRead, NULL, &output) == OS_EINVALPARAMS);
    assert(GrantTokenImport(&token, (GrantTokenResourceType_t)42, GrantTokenScopeRead, NULL, &output) == OS_EINVALPARAMS);
    assert(ReleaseGlobalHandle(token.ExportGlobalHandleID) == OS_EOK);
    __DrainJanitor();
    assert(g_bufferDeletes == 0);
    __AssertInvalidToken(&token);
    assert(GrantTokenRevoke(token.ControlHandleID) == OS_ENOENT);
    assert(ReleaseGlobalHandle(imported) == OS_EOK);
    __DrainJanitor();
    assert(g_bufferDeletes == 1);

    // A group-bound token can pass through the wrong group without importing.
    source = CreateHandle(HandleTypeSHM, NULL, __DeleteBuffer, &buffer);
    buffer.ID = source;
    options.TargetType = GrantTokenTargetTypeSpecific;
    options.TargetGlobalHandleID = second.GlobalHandleID;
    options.Scope = GrantTokenScopeWrite;
    options.Data.SHM.Offset = 1024;
    options.Data.SHM.Length = 2048;
    assert(GrantTokenCreate(source, &options, &restricted) == OS_EOK);
    assert(GrantTokenImport(&restricted, GrantTokenResourceTypeSHM, GrantTokenScopeWrite, NULL, &output) == OS_EPERMISSIONS);
    g_currentContext = &second;
    assert(GrantTokenImport(&restricted, GrantTokenResourceTypeSHM, GrantTokenScopeWrite, NULL, &imported) == OS_EOK);
    g_currentContext = &first;
    assert(DestroyHandle(restricted.ControlHandleID) == OS_EOK);
    __AssertInvalidToken(&restricted);
    assert(ReleaseGlobalHandle(imported) == OS_EINCOMPLETE);

    // Security-group teardown disables exports without invalidating imports.
    assert(GrantTokenCreate(source, &options, &restricted) == OS_EOK);
    g_currentContext = &second;
    assert(GrantTokenImport(&restricted, GrantTokenResourceTypeSHM, GrantTokenScopeWrite, NULL, &imported) == OS_EOK);
    DestroyHandleGrantsTable(first.HandleTable);
    __AssertInvalidToken(&restricted);
    __DrainJanitor();
    assert(g_bufferDeletes == 1);
    assert(ReleaseGlobalHandle(imported) == OS_EOK);
    __DrainJanitor();
    assert(g_bufferDeletes == 2);
    DestroyHandleGrantsTable(second.HandleTable);
    g_currentContext = NULL;
    assert(GrantTokenCreate(source, &options, &token) == OS_ENOTSUPPORTED);
    assert(GrantTokenRevoke(token.ControlHandleID) == OS_ENOTSUPPORTED);
    assert(GrantTokenImport(&token, GrantTokenResourceTypeSHM, GrantTokenScopeRead, NULL, &output) == OS_ENOTSUPPORTED);
    assert(g_liveAllocations == 0);
    assert(g_handles.element_count == 0);
    hashtable_destroy(&g_handles);
    hashtable_destroy(&g_handlemappings);
    puts("Grant tokens: create/copy/revoke, single-use imports, copy faults and reference lifetime passed");
    return 0;
}

// Provider tokens have their own production-module suite.
oserr_t HandleProviderReferenceForExport(uuid_t id, const GrantTokenOptions_t* options, HandleType_t* type)
{ (void)id; (void)options; (void)type; return OS_ENOTSUPPORTED; }
