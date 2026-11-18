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
 */

// Exercise the namespace reader and architecture policy through their
// production code. The ACPICA model exposes named objects and counts reads.
#ifndef ACPI_PCI_DMA_SOURCE
#define ACPI_PCI_DMA_SOURCE "../../kernel/acpi/pci-dma.c"
#endif
#ifndef ARM64_PCI_DMA_SOURCE
#define ARM64_PCI_DMA_SOURCE "../../kernel/arch/aarch64/acpi/pci-dma.c"
#endif
#include ACPI_PCI_DMA_SOURCE
#include ARM64_PCI_DMA_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

struct __Node {
    char Name[5];
    ACPI_OBJECT_TYPE Type;
    struct __Node* Parent;
    struct __Node* Children;
    struct __Node* Next;
    UINT64 Value;
};

struct ArmIortCatalogue { unsigned Magic; };
static struct __Node g_nodes[5000];
static unsigned g_count;
static struct __Node* g_systemBus;
static struct __Node* g_root;
static ACPI_STATUS g_nameStatus, g_parentStatus, g_typeStatus, g_handleStatus;
static ACPI_STATUS g_nextStatus, g_evaluateStatus, g_walkStatus, g_viotStatus;
static oserr_t g_catalogueStatus, g_directStatus;
static unsigned g_tables, g_queries, g_evaluations;
static ACPI_TABLE_HEADER g_viot;
static const struct AcpiPciRoot g_identity = {.Id = 9, .Segment = 3};

void
LogAppendMessage(enum OSSysLogLevel level, const char* format, ...)
{
    (void)level;
    (void)format;
}

static struct __Node*
__Add(struct __Node* parent, const char* name, ACPI_OBJECT_TYPE type)
{
    struct __Node* node;

    assert(g_count < SIZEOF_ARRAY(g_nodes));
    node = &g_nodes[g_count++];
    memcpy(node->Name, name, sizeof(node->Name));
    node->Type = type;
    node->Parent = parent;
    if (parent != NULL) {
        node->Next = parent->Children;
        parent->Children = node;
    }
    return node;
}

static void
__Reset(void)
{
    assert(g_tables == 0);
    memset(g_nodes, 0, sizeof(g_nodes));
    g_count = g_queries = g_evaluations = 0;
    g_nameStatus = g_parentStatus = g_typeStatus = g_handleStatus = AE_OK;
    g_nextStatus = g_evaluateStatus = g_walkStatus = AE_OK;
    g_viotStatus = AE_NOT_FOUND;
    g_catalogueStatus = g_directStatus = OS_EOK;
    g_systemBus = __Add(NULL, "_SB_", ACPI_TYPE_LOCAL_SCOPE);
    g_root = __Add(g_systemBus, "PCI0", ACPI_TYPE_DEVICE);
}

ACPI_STATUS
AcpiGetHandle(ACPI_HANDLE parent, ACPI_STRING name, ACPI_HANDLE* out)
{
    assert(parent == NULL && !strcmp(name, "\\_SB"));
    if (ACPI_FAILURE(g_handleStatus)) return g_handleStatus;
    *out = g_systemBus;
    return AE_OK;
}

ACPI_STATUS
AcpiGetParent(ACPI_HANDLE handle, ACPI_HANDLE* out)
{
    struct __Node* node = handle;

    if (ACPI_FAILURE(g_parentStatus)) return g_parentStatus;
    if (node->Parent == NULL) return AE_NOT_FOUND;
    *out = node->Parent;
    return AE_OK;
}

ACPI_STATUS
AcpiGetType(ACPI_HANDLE handle, ACPI_OBJECT_TYPE* out)
{
    if (ACPI_FAILURE(g_typeStatus)) return g_typeStatus;
    *out = ((struct __Node*)handle)->Type;
    return AE_OK;
}

ACPI_STATUS
AcpiGetName(ACPI_HANDLE handle, UINT32 type, ACPI_BUFFER* out)
{
    assert(type == ACPI_SINGLE_NAME && out->Length >= 5);
    if (ACPI_FAILURE(g_nameStatus)) return g_nameStatus;
    memcpy(out->Pointer, ((struct __Node*)handle)->Name, 5);
    return AE_OK;
}

ACPI_STATUS
AcpiGetNextObject(ACPI_OBJECT_TYPE type, ACPI_HANDLE parent, ACPI_HANDLE previous, ACPI_HANDLE* out)
{
    struct __Node* next = previous != NULL ? ((struct __Node*)previous)->Next : ((struct __Node*)parent)->Children;

    assert(type == ACPI_TYPE_ANY);
    if (ACPI_FAILURE(g_nextStatus)) return g_nextStatus;
    if (next == NULL) return AE_NOT_FOUND;
    *out = next;
    return AE_OK;
}

ACPI_STATUS
AcpiEvaluateObject(ACPI_HANDLE handle, ACPI_STRING name, ACPI_OBJECT_LIST* args, ACPI_BUFFER* out)
{
    struct __Node* node = handle;
    ACPI_OBJECT* value = out->Pointer;

    assert(name == NULL && args == NULL && !strcmp(node->Name, "_CCA"));
    g_evaluations++;
    if (ACPI_FAILURE(g_evaluateStatus)) return g_evaluateStatus;
    value->Type = node->Type == ACPI_TYPE_METHOD ? ACPI_TYPE_INTEGER : node->Type;
    value->Integer.Value = node->Value;
    return AE_OK;
}

static ACPI_STATUS
__Walk(struct __Node* parent, UINT32 level, UINT32 maximum, ACPI_WALK_CALLBACK callback, void* context)
{
    for (struct __Node* node = parent->Children; node != NULL; node = node->Next) {
        ACPI_STATUS status = callback(node, level, context, NULL);

        if (status != AE_OK) return status;
        if (level < maximum) {
            status = __Walk(node, level + 1, maximum, callback, context);
            if (status != AE_OK) return status;
        }
    }
    return AE_OK;
}

ACPI_STATUS
AcpiWalkNamespace(ACPI_OBJECT_TYPE type, ACPI_HANDLE root, UINT32 depth,
                 ACPI_WALK_CALLBACK descending, ACPI_WALK_CALLBACK ascending, void* context, void** out)
{
    ACPI_STATUS status;

    assert(type == ACPI_TYPE_ANY && root == g_root && ascending == NULL);
    if (ACPI_FAILURE(g_walkStatus)) return g_walkStatus;
    status = __Walk(root, 1, depth, descending, context);
    return status == AE_CTRL_TERMINATE ? AE_OK : status;
}

ACPI_STATUS
AcpiGetTable(char* name, UINT32 instance, ACPI_TABLE_HEADER** out)
{
    assert(!strcmp(name, ACPI_SIG_VIOT) && instance == 1);
    if (ACPI_FAILURE(g_viotStatus)) return g_viotStatus;
    g_tables++;
    *out = &g_viot;
    return AE_OK;
}

void
AcpiPutTable(ACPI_TABLE_HEADER* table)
{
    assert(table == &g_viot && g_tables == 1);
    g_tables--;
}

oserr_t
ArmIortGetCatalogue(const ArmIortCatalogue_t** out)
{
    static const ArmIortCatalogue_t catalogue = {0xcafef00d};

    if (g_catalogueStatus != OS_EOK) return g_catalogueStatus;
    *out = &catalogue;
    return OS_EOK;
}

oserr_t
ArmIortGetPciDirectLimit(const ArmIortCatalogue_t* discovery, uint32_t segment, uint64_t* out)
{
    assert(discovery->Magic == 0xcafef00d && segment == g_identity.Segment);
    g_queries++;
    if (g_directStatus != OS_EOK) return g_directStatus;
    *out = 0xffffffffffULL;
    return OS_EOK;
}

static void
__Check(oserr_t expected)
{
    AcpiPciDma_t result, before;

    memset(&result, 0xa5, sizeof(result));
    before = result;
    assert(ArchAcpiGetPciDma(g_root, &g_identity, &result) == expected);
    if (expected == OS_EOK) {
        assert(result.AddressLimit == 0xffffffffffULL);
        assert(result.CachePolicy == SHMDeviceCacheCoherent && result.Flags == ACPI_PCI_DMA_DIRECT);
    } else {
        assert(!memcmp(&result, &before, sizeof(result)));
    }
    assert(g_tables == 0);
}

static void
__Coherency(void)
{
    struct __Node* parent;
    struct __Node* coherency;

    __Reset();
    __Check(OS_ENOTSUPPORTED);
    assert(g_queries == 0);
    __Add(g_root, "_CCA", ACPI_TYPE_INTEGER)->Value = 1;
    __Check(OS_EOK);
    assert(g_evaluations == 1 && g_queries == 1);

    __Reset();
    __Add(g_root, "_CCA", ACPI_TYPE_METHOD)->Value = 1;
    __Check(OS_EOK);

    for (UINT64 value = 0; value <= 2; value += 2) {
        __Reset();
        __Add(g_root, "_CCA", ACPI_TYPE_INTEGER)->Value = value;
        __Check(value == 0 ? OS_ENOTSUPPORTED : OS_EINVALPARAMS);
    }
    __Reset();
    __Add(g_root, "_CCA", ACPI_TYPE_STRING);
    __Check(OS_EINVALPARAMS);

    __Reset();
    parent = g_root;
    coherency = __Add(parent, "_CCA", ACPI_TYPE_INTEGER);
    coherency->Value = 1;
    g_root = __Add(parent, "PCI1", ACPI_TYPE_DEVICE);
    __Add(g_root, "_CCA", ACPI_TYPE_INTEGER)->Value = 0;
    __Check(OS_EOK);
    assert(g_evaluations == 1);
    coherency->Value = 0;
    __Check(OS_ENOTSUPPORTED);

    __Reset();
    __Add(__Add(g_root, "DEV0", ACPI_TYPE_DEVICE), "_CCA", ACPI_TYPE_INTEGER)->Value = 1;
    __Check(OS_ENOTSUPPORTED);
}

static void
__Restrictions(void)
{
    struct __Node* parent;

    for (unsigned level = 0; level < 3; level++) {
        __Reset();
        __Add(g_root, "_CCA", ACPI_TYPE_INTEGER)->Value = 1;
        parent = level == 0 ? g_systemBus : g_root;
        if (level == 2) parent = __Add(g_root, "DEV0", ACPI_TYPE_DEVICE);
        // Buffer and method forms are both excluded without evaluating _DMA.
        __Add(parent, "_DMA", level == 1 ? ACPI_TYPE_METHOD : ACPI_TYPE_BUFFER);
        __Check(OS_ENOTSUPPORTED);
        assert(g_queries == 0);
    }
    __Reset();
    __Add(g_root, "_CCA", ACPI_TYPE_INTEGER)->Value = 1;
    for (unsigned i = 0; i < 4094; i++) __Add(g_root, "DATA", ACPI_TYPE_INTEGER);
    __Check(OS_EOK);
    __Add(g_root, "OVER", ACPI_TYPE_INTEGER);
    __Check(OS_ENOTSUPPORTED);

    __Reset();
    __Add(g_root, "_CCA", ACPI_TYPE_INTEGER)->Value = 1;
    parent = g_root;
    for (unsigned i = 0; i < 256; i++) parent = __Add(parent, "NODE", ACPI_TYPE_DEVICE);
    __Check(OS_EOK);
    __Add(parent, "MORE", ACPI_TYPE_DEVICE);
    __Check(OS_ENOTSUPPORTED);

    __Reset();
    for (unsigned i = 0; i < 256; i++) g_root = __Add(g_root, "NODE", ACPI_TYPE_DEVICE);
    __Add(g_root, "_CCA", ACPI_TYPE_INTEGER)->Value = 1;
    __Check(OS_ENOTSUPPORTED);
}

static void
__ArchitectureError(oserr_t expected)
{
    uint64_t limit = 0x9876543210ULL;

    assert(__GetPciDmaLimit(g_identity.Segment, &limit) == expected);
    assert(limit == 0x9876543210ULL);
    assert(g_tables == 0);
}

static void
__Failures(void)
{
    ACPI_STATUS* errors[] = {&g_nameStatus, &g_parentStatus, &g_handleStatus,
                            &g_nextStatus, &g_evaluateStatus, &g_walkStatus, &g_viotStatus};
    const oserr_t catalogueErrors[] = {OS_ENOENT, OS_ENOTSUPPORTED, OS_EINVALPARAMS, OS_EOOM, OS_EUNKNOWN};

    for (size_t i = 0; i < SIZEOF_ARRAY(errors); i++) {
        __Reset();
        __Add(g_root, "_CCA", ACPI_TYPE_INTEGER)->Value = 1;
        *errors[i] = AE_ERROR;
        __Check(OS_EUNKNOWN);
        *errors[i] = AE_NO_MEMORY;
        __Check(OS_EOOM);
    }
    __Reset();
    g_root = __Add(g_root, "PCI1", ACPI_TYPE_DEVICE);
    g_typeStatus = AE_ERROR;
    __Check(OS_EUNKNOWN);
    __Reset();
    __Add(g_root, "_CCA", ACPI_TYPE_INTEGER)->Value = 1;
    g_evaluateStatus = AE_BUFFER_OVERFLOW;
    __Check(OS_EINVALPARAMS);
    __Reset();
    __Add(g_root, "_CCA", ACPI_TYPE_INTEGER)->Value = 1;
    g_viotStatus = AE_OK;
    __Check(OS_ENOTSUPPORTED);
    assert(g_queries == 0);
    __ArchitectureError(OS_ENOTSUPPORTED);
    g_viotStatus = AE_ERROR;
    __ArchitectureError(OS_EUNKNOWN);
    for (size_t i = 0; i < SIZEOF_ARRAY(catalogueErrors); i++) {
        __Reset();
        __Add(g_root, "_CCA", ACPI_TYPE_INTEGER)->Value = 1;
        g_catalogueStatus = catalogueErrors[i];
        __Check(g_catalogueStatus == OS_ENOENT ? OS_ENOTSUPPORTED : g_catalogueStatus);
        __ArchitectureError(g_catalogueStatus == OS_ENOENT ? OS_ENOTSUPPORTED : g_catalogueStatus);
    }
    __Reset();
    __Add(g_root, "_CCA", ACPI_TYPE_INTEGER)->Value = 1;
    g_directStatus = OS_ENOTSUPPORTED;
    __Check(OS_ENOTSUPPORTED);
    assert(g_queries == 1);
    __ArchitectureError(OS_ENOTSUPPORTED);
    assert(ArchAcpiGetPciDma(NULL, &g_identity, NULL) == OS_EINVALPARAMS);
    assert(ArchAcpiGetPciDma(g_root, NULL, NULL) == OS_EINVALPARAMS);
    assert(ArchAcpiGetPciDma(g_root, &g_identity, NULL) == OS_EINVALPARAMS);
    assert(__GetPciDmaLimit(3, NULL) == OS_EINVALPARAMS);
}

static void
__NamespaceProperties(void)
{
    AcpiPciDmaProperties_t properties, before;
    struct __Node* cca;

    __Reset();
    assert(AcpiPciReadDmaProperties(g_root, &properties) == OS_EOK);
    assert(!properties.CoherencyPresent && !properties.Coherent && !properties.HasDma);
    cca = __Add(g_root, "_CCA", ACPI_TYPE_INTEGER);
    assert(AcpiPciReadDmaProperties(g_root, &properties) == OS_EOK);
    assert(properties.CoherencyPresent && !properties.Coherent && !properties.HasDma);
    cca->Value = 1;
    __Add(g_root, "_DMA", ACPI_TYPE_BUFFER);
    assert(AcpiPciReadDmaProperties(g_root, &properties) == OS_EOK);
    assert(properties.CoherencyPresent && properties.Coherent && properties.HasDma);

    memset(&properties, 0xa5, sizeof(properties)); before = properties;
    g_walkStatus = AE_ERROR;
    assert(AcpiPciReadDmaProperties(g_root, &properties) == OS_EUNKNOWN);
    assert(!memcmp(&properties, &before, sizeof(properties)));
    assert(AcpiPciReadDmaProperties(NULL, &properties) == OS_EINVALPARAMS);
    assert(!memcmp(&properties, &before, sizeof(properties)));
    assert(AcpiPciReadDmaProperties(g_root, NULL) == OS_EINVALPARAMS);
}

int
main(void)
{
    __NamespaceProperties();
    __Coherency();
    __Restrictions();
    __Failures();
    puts("ACPI PCI DMA policy: PASS");
    return 0;
}
