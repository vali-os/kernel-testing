/*
 * The PE linker needs these definitions even for discarded syscall sections.
 * Interrupt host tests never call these services.
 */
#include <stdlib.h>
void CpuCoreSetState(void) { abort(); }
void FirmwareQuery(void) { abort(); }
void FirmwareLocate(void) { abort(); }
void MemorySpaceMap(void) { abort(); }
void MemorySpaceChangeProtection(void) { abort(); }
void RegisterSystemDeviceIo(void) { abort(); }
void AcquireSystemDeviceIo(void) { abort(); }
void ReleaseSystemDeviceIo(void) { abort(); }
void DestroyHandle(void) { abort(); }
