/**
 * Copyright 2026, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * Statistics of the SD timing test (sdtiming.c). The program measures every
 * single call of a probe in nanoseconds and keeps all the samples. This file
 * turns the samples of one probe into the numbers of its result line. It
 * builds both for the target program and, in the host test
 * (testing/sdtiming_stats_test.py), with the host compiler.
 *
 * Percentiles use the nearest-rank method. For count sorted samples, the
 * p-th percentile is the sample at rank ceil(p * count / 100), counted from
 * 1. The median is the 50th percentile, so for an even count it is the lower
 * of the two middle samples. Every reported value is one of the measured
 * samples, except the mean.
 */

#ifndef __TIMING_STATISTICS_H__
#define __TIMING_STATISTICS_H__

#include <stddef.h>
#include <stdint.h>

// The host test builds statistics.c with the host compiler, which has no SAL
// annotations. The Vali headers define them.
#ifndef _In_
#define _In_
#endif
#ifndef _Out_
#define _Out_
#endif
#ifndef _InOut_
#define _InOut_
#endif

/**
 * @brief The numbers of one probe's result line, all in nanoseconds.
 * Mean is rounded to the nearest nanosecond.
 */
typedef struct TimingSummary {
    uint32_t Count;
    uint64_t Minimum;
    uint64_t Median;
    uint64_t Percentile90;
    uint64_t Maximum;
    uint64_t Mean;
} TimingSummary_t;

/**
 * @brief Picks the p-th percentile of sorted samples with the nearest-rank
 * method (see the top of this file).
 *
 * @param samples Samples sorted from small to large. Must hold count values.
 * @param count Number of samples. Must not be 0.
 * @param percent The percentile, from 1 to 100.
 * @return The sample at rank ceil(percent * count / 100).
 */
uint64_t
TimingPercentile(
    _In_ const uint64_t* samples,
    _In_ uint32_t        count,
    _In_ uint32_t        percent);

/**
 * @brief Computes the mean of the samples, rounded to the nearest
 * nanosecond. The sum is never formed, so the mean is exact for any sample
 * values and any count.
 *
 * @param samples The samples, in any order. Must hold count values.
 * @param count Number of samples. Must not be 0.
 * @return The mean in nanoseconds.
 */
uint64_t
TimingMean(
    _In_ const uint64_t* samples,
    _In_ uint32_t        count);

/**
 * @brief Sorts the samples from small to large and fills summary.
 *
 * @param samples The samples. They are sorted in place.
 * @param count Number of samples.
 * @param summary Receives the numbers. Left unchanged on failure.
 * @return 0 on success, or -1 when count is 0.
 */
int
TimingSummarize(
    _InOut_ uint64_t*        samples,
    _In_    uint32_t         count,
    _Out_   TimingSummary_t* summary);

/**
 * @brief Writes the result line of one probe, without the "sdtiming: "
 * prefix:
 *   probe <name> <count> calls, us min <a> median <b> p90 <c> max <d> mean <e>
 * Every value is in microseconds with three decimals, which is the
 * nanosecond resolution of the clock.
 *
 * @param text Receives the line, always ended with a zero byte when capacity
 *             is not 0.
 * @param capacity Size of text in bytes.
 * @param name Name of the probe.
 * @param summary Numbers of the probe.
 * @return The length of the line, or -1 when it does not fit into text.
 */
int
TimingFormatProbeLine(
    _Out_ char*                  text,
    _In_  size_t                 capacity,
    _In_  const char*            name,
    _In_  const TimingSummary_t* summary);

#endif //!__TIMING_STATISTICS_H__
