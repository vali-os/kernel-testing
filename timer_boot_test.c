/* Calibration must never run before selection of the discovered reference. */
#include "../kernel/arch/x86/components/timers.c"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
ACPI_TABLE_FADT AcpiGbl_FADT;
static int g_discovered, g_selected, g_refreshes, g_calibrations, g_missing;
void LogAppendMessage(enum OSSysLogLevel level, const char* format, ...) {}
int AcpiAvailable(void) { return 0; }
oserr_t InterruptDecreasePenalty(int source) { return OS_EOK; }
oserr_t CmosInitialize(int available) { CHECK(available); g_discovered = 1; return OS_EOK; }
oserr_t PitInitialize(int rtcAvailable) { CHECK(rtcAvailable); return OS_EOK; }
void SystemTimerRefreshTimers(void) { CHECK(g_discovered); g_selected = !g_missing; g_refreshes++; }
void SystemTimerGetClockFrequency(UInteger64_t* frequency) { frequency->QuadPart = g_selected ? 1024 : 0; }
void TscInitialize(void) { CHECK(g_selected); g_calibrations++; }
void ApicTimerInitialize(void) { CHECK(g_selected && g_calibrations == 1); g_calibrations++; }
void RtcSetCalibrationMode(int enable) { CHECK(enable); }
void PitSetCalibrationMode(int enable) { CHECK(enable); }
int main(void) {
    CHECK(PlatformTimersInitialize() == OS_EOK);
    CHECK(g_selected && g_calibrations == 2 && g_refreshes == 2);
    g_missing = 1; g_calibrations = 0;
    CHECK(PlatformTimersInitialize() == OS_ENOTSUPPORTED);
    CHECK(g_calibrations == 0);
    puts("Timer boot: reference selected before calibration, retained fallback, missing-clock failure passed");
    return 0;
}
