// The PE linker resolves even discarded functions. Unused device services must
// fail immediately if accidentally called by the bounded mapping regression.
#include <stdlib.h>
void list_find(void) { abort(); }
void list_cmp_default(void) { abort(); }
void list_remove(void) { abort(); }
void list_append(void) { abort(); }
void list_enumerate(void) { abort(); }
void kmalloc(void) { abort(); }
void kfree(void) { abort(); }
void CreateHandle(void) { abort(); }
void ArchGetProcessorCoreId(void) { abort(); }
void GetCurrentMemorySpaceHandle(void) { abort(); }
void SetDirectIoAccess(void) { abort(); }
void MemorySpaceUnmap(void) { abort(); }
void LogAppendMessage(void) { abort(); }
