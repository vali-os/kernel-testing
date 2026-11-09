// Exercise the same interval conversion used to program the ARM64 timer.
#include "../kernel/arch/aarch64/common/utils.h"
#include <assert.h>
#include <stdio.h>

int
main(void)
{
    const uint64_t frequencies[] = {
        1,
        54000000,
        NSEC_PER_SEC,
        2 * NSEC_PER_SEC,
        UINT32_MAX,
        UINT64_MAX
    };
    const uint64_t intervals[] = {
        0,
        1,
        NSEC_PER_SEC - 1,
        NSEC_PER_SEC,
        NSEC_PER_SEC + 1,
        INT64_MAX - 1,
        INT64_MAX,
        (uint64_t)INT64_MAX + 1,
        UINT64_MAX
    };
    unsigned __int128 expected;
    size_t frequencyIndex;
    size_t intervalIndex;

    // A wider reference calculation checks both overflow guards and rounding.
    for (frequencyIndex = 0; frequencyIndex < sizeof(frequencies) / sizeof(frequencies[0]); frequencyIndex++) {
        for (intervalIndex = 0; intervalIndex < sizeof(intervals) / sizeof(intervals[0]); intervalIndex++) {
            expected = (unsigned __int128)intervals[intervalIndex] * frequencies[frequencyIndex];
            expected = expected / NSEC_PER_SEC + (expected % NSEC_PER_SEC != 0);
            if (expected > INT64_MAX) {
                expected = INT64_MAX;
            }
            assert(__TimerIntervalTicks(intervals[intervalIndex], frequencies[frequencyIndex]) == expected);
        }
    }
    puts("ARM64 timer: sub-tick rounding, large intervals, high frequencies and saturation passed");
    return 0;
}
