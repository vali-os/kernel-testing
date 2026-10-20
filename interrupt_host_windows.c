/* The PE linker requires definitions even for discarded syscall sections.
 * These unrelated services are never exercised by the interrupt host tests. */
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
void GetMachine(void) { abort(); }
