/** Check bus type conversion and compatibility with the former uint32 PCI flag. */
#include <ddk/convert.h>
#include <stdio.h>

#define CHECK(value) do { if (!(value)) { printf("line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

_Static_assert(BusTypeGeneric == 0 && BusTypePci == 1, "DDK bus type values");
_Static_assert(SYS_BUS_TYPE_GENERIC == 0 && SYS_BUS_TYPE_PCI == 1, "Protocol bus type values");
_Static_assert(sizeof(BusType_t) == sizeof(unsigned int), "DDK bus type layout");

static void
__SerializeLegacyBus(
    _InOut_ gracht_buffer_t*             buffer,
    _In_    const struct sys_device_bus* device,
    _In_    uint32_t                     isPci)
{
    uint32_t i;

    serialize_uuid_t(buffer, device->id);
    serialize_uuid_t(buffer, device->parent_id);
    serialize_sys_device_identification(buffer, &device->identification);
    serialize_uint32(buffer, isPci);
    serialize_int(buffer, device->irq_line);
    serialize_int(buffer, device->irq_pin);
    serialize_uint32(buffer, device->acpi_conform_flags);
    serialize_uint32(buffer, device->segment);
    serialize_uint32(buffer, device->bus);
    serialize_uint32(buffer, device->slot);
    serialize_uint32(buffer, device->function);
    serialize_uint32(buffer, device->ios_count);
    for (i = 0; i < device->ios_count; i++) {
        serialize_sys_bus_io(buffer, &device->ios[i]);
    }
}

static void
__TestBusType(
    _In_ BusType_t type)
{
    BusDevice_t device = {
        .Base = {.Id = 42, .ParentId = 7, .Length = sizeof(BusDevice_t)},
        .Type = type,
        .InterruptLine = -1,
        .InterruptPin = 2,
        .InterruptAcpiConform = 0x15,
        .Segment = 2,
        .Bus = 5,
        .Slot = 3,
        .Function = 7
    };
    BusDevice_t           restored;
    struct sys_device_bus wire = {0};
    struct sys_device_bus decoded = {0};
    char                  bytes[1024];
    char                  legacyBytes[1024];
    gracht_buffer_t       encoded = {.data = bytes, .limit = sizeof(bytes)};
    gracht_buffer_t       legacy = {.data = legacyBytes, .limit = sizeof(legacyBytes)};
    gracht_buffer_t       reader;
    uint32_t              i;

    to_sys_device_bus(&device, &wire);
    CHECK(wire.type == (enum sys_bus_type)type);
    CHECK(wire.ios_count == __DEVICEMANAGER_MAX_IOSPACES);
    serialize_sys_device_bus(&encoded, &wire);
    __SerializeLegacyBus(&legacy, &wire, (uint32_t)type);
    CHECK(encoded.error == 0 && legacy.error == 0);
    CHECK(encoded.index == legacy.index);
    CHECK(memcmp(bytes, legacyBytes, encoded.index) == 0);

    reader = (gracht_buffer_t){.data = legacyBytes, .limit = legacy.index};
    deserialize_sys_device_bus(&reader, &decoded);
    CHECK(reader.error == 0 && reader.index == legacy.index);
    CHECK(decoded.type == wire.type);
    from_sys_device_bus(&decoded, &restored);
    CHECK(restored.Type == type);
    CHECK(restored.Base.Id == device.Base.Id && restored.Base.ParentId == device.Base.ParentId);
    CHECK(restored.Base.Length == sizeof(BusDevice_t));
    CHECK(restored.InterruptLine == device.InterruptLine);
    CHECK(restored.InterruptPin == device.InterruptPin);
    CHECK(restored.InterruptAcpiConform == device.InterruptAcpiConform);
    CHECK(restored.Segment == device.Segment && restored.Bus == device.Bus);
    CHECK(restored.Slot == device.Slot && restored.Function == device.Function);
    for (i = 0; i < __DEVICEMANAGER_MAX_IOSPACES; i++) {
        CHECK(restored.IoSpaces[i].Id == UUID_INVALID);
    }

    free(restored.Base.Identification.Description);
    free(restored.Base.Identification.Manufacturer);
    free(restored.Base.Identification.Product);
    free(restored.Base.Identification.Revision);
    free(restored.Base.Identification.Serial);
    sys_device_bus_destroy(&decoded);
    sys_device_bus_destroy(&wire);
}

int
main(void)
{
    BusDevice_t device = {0};

    CHECK(device.Type == BusTypeGeneric);
    __TestBusType(BusTypeGeneric);
    __TestBusType(BusTypePci);
    puts("Device bus: generic/PCI conversion, uint32 wire compatibility and metadata preservation PASS");
    return 0;
}
