// The PE linker resolves unused functions too. Fail if this test reaches one.
#include <stdlib.h>

void
SystemMemoryReleaseAllocation(void)
{
    abort();
}

void
SystemMemoryAllocateDevice(void)
{
    abort();
}

void
MemorySpaceUnmap(void)
{
    abort();
}

void
MemorySpaceCommit(void)
{
    abort();
}

void
GetMemorySpaceMapping(void)
{
    abort();
}

void
ArchSHMTypeToPageMask(void)
{
    abort();
}

void
AcquireHandleOfType(void)
{
    abort();
}

void
LookupHandleOfType(void)
{
    abort();
}

void
DestroyHandle(void)
{
    abort();
}

void
AreMemorySpacesRelated(void)
{
    abort();
}

void
MutexLock(void)
{
    abort();
}

void
MutexUnlock(void)
{
    abort();
}

void
LogAppendMessage(void)
{
    abort();
}

void
syscall4(void)
{
    abort();
}

void
AllocatePhysicalMemory(void)
{
    abort();
}

void
__errno(void)
{
    abort();
}
