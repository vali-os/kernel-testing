// Unrelated scheduler setup and service operations must not run in this fixture.
#include <stdlib.h>
void UhciUpdateCurrentFrame(void) { abort(); }
void HCIProcessElement(void) { abort(); }
void UHCIQHInitialize(void) { abort(); }
#if !defined(TEST_OHCI)
void OHCIQHInitialize(void) { abort(); }
#endif
void EHCIQHInitialize(void) { abort(); }
void UsbSchedulerFreeElement(void) { abort(); }
void UhciConditionCodeToIndex(void) { abort(); }
#if !defined(TEST_EHCI)
void EHCIConditionCodeToIndex(void) { abort(); }
#endif
void UHCIErrorCodeToTransferStatus(void) { abort(); }
void OHCIErrorCodeToTransferStatus(void) { abort(); }
#if !defined(TEST_EHCI)
void EHCIErrorCodeToTransferStatus(void) { abort(); }
#endif
void UsbSchedulerAllocateBandwidth(void) { abort(); }
void UsbSchedulerAllocateSplitBandwidth(void) { abort(); }
void UsbSchedulerFreeSplitBandwidth(void) { abort(); }
void UsbManagerIterateTransfers(void) { abort(); }
void UsbManagerProcessTransfers(void) { abort(); }
void UsbSchedulerCalculateBandwidth(void) { abort(); }

#if defined(TEST_EHCI)
void UsbSchedulerResetInternalData(void) { abort(); }
void UsbSchedulerSettingsCreate(void) { abort(); }
void UsbSchedulerSettingsAddPool(void) { abort(); }
void UsbSchedulerInitialize(void) { abort(); }
void UsbManagerClearTransfers(void) { abort(); }
void UsbSchedulerDestroy(void) { abort(); }
void UsbSchedulerGetPoolFromElement(void) { abort(); }
void EHCIQHDump(void) { abort(); }
void spinlock_acquire(void) { abort(); }
void UsbSchedulerLinkPeriodicElement(void) { abort(); }
void spinlock_release(void) { abort(); }
void UsbSchedulerUnchainElement(void) { abort(); }
void UsbSchedulerUnlinkPeriodicElement(void) { abort(); }
void EHCIQHRestart(void) { abort(); }
#endif

#if defined(TEST_OHCI)
void OHCICheckPorts(void) { abort(); }
void OHCIReset(void) { abort(); }
void UsbManagerScheduleTransfers(void) { abort(); }
void timespec_get(void) { abort(); }
void timespec_add(void) { abort(); }
void usched_job_sleep(void) { abort(); }
void __errno(void) { abort(); }
#endif
