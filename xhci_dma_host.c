// Unrelated HCI paths must not run in the DMA fixture.
#include <stdlib.h>
void UsbSchedulerResetInternalData(void) { abort(); }
void UsbSchedulerDestroy(void) { abort(); }
void XhciHalt(void) { abort(); }
void UsbManagerClearTransfers(void) { abort(); }
void SystemDebug(void) { abort(); }
void timespec_get(void) { abort(); }
void timespec_add(void) { abort(); }
void usched_job_sleep(void) { abort(); }
void __errno(void) { abort(); }
// The fixture executes one thread; lists still use the production links.
void spinlock_init(void) { }
void spinlock_acquire(void) { }
void spinlock_release(void) { }
