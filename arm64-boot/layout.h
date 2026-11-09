// VBoot 1.2 byte layout shared with the BIOS assembly descriptor.
_Static_assert(VBOOT_VERSION == 0x00010002, "VBoot version");
_Static_assert(sizeof(struct VBoot) == 176, "VBoot size");
_Static_assert(sizeof(struct VBootMemoryEntry) == 36, "Memory entry size");
_Static_assert(offsetof(struct VBoot, ConfigurationTable) == 20, "Configuration table");
_Static_assert(offsetof(struct VBoot, Memory) == 28, "Memory");
_Static_assert(offsetof(struct VBoot, Video) == 44, "Video");
_Static_assert(offsetof(struct VBoot, Ramdisk) == 100, "Ramdisk");
_Static_assert(offsetof(struct VBoot, Kernel) == 112, "Kernel");
_Static_assert(offsetof(struct VBoot, Stack) == 132, "Stack");
_Static_assert(offsetof(struct VBoot, Phoenix) == 144, "Phoenix");
_Static_assert(offsetof(struct VBoot, DeviceTree) == 164, "DeviceTree");

// Numeric memory types are mirrored by BIOS assembly as well as the layout.
_Static_assert(VBootMemoryType_Reserved == 0, "Reserved memory type");
_Static_assert(VBootMemoryType_Firmware == 1, "Firmware memory type");
_Static_assert(VBootMemoryType_ACPI == 2, "ACPI memory type");
_Static_assert(VBootMemoryType_NVS == 3, "NVS memory type");
_Static_assert(VBootMemoryType_Available == 4, "Available memory type");
_Static_assert(VBootMemoryType_Reclaim == 5, "Reclaim memory type");
_Static_assert(VBootMemoryType_DevicePool == 6, "DevicePool memory type");
