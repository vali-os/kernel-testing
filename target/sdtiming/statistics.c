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
 * Statistics of the SD timing test. The methods are described in
 * statistics.h. This file builds both for the target program and, in the
 * host test, with the host compiler.
 */

#include <stdio.h>
#include <stdlib.h>

#include "statistics.h"

#define __NSEC_PER_USEC 1000ULL

// Orders two samples for qsort, from small to large.
static int
__CompareSamples(
    _In_ const void* left,
    _In_ const void* right)
{
    uint64_t leftValue = *(const uint64_t*)left;
    uint64_t rightValue = *(const uint64_t*)right;

    if (leftValue < rightValue) {
        return -1;
    }
    return leftValue > rightValue ? 1 : 0;
}

uint64_t
TimingPercentile(
    _In_ const uint64_t* samples,
    _In_ uint32_t        count,
    _In_ uint32_t        percent)
{
    uint64_t rank;

    // rank = ceil(percent * count / 100). The product fits into 64 bits for
    // any 32-bit count. The rank is at least 1 and at most count.
    rank = (((uint64_t)percent * count) + 99U) / 100U;
    if (rank == 0) {
        rank = 1;
    }
    if (rank > count) {
        rank = count;
    }
    return samples[rank - 1];
}

uint64_t
TimingMean(
    _In_ const uint64_t* samples,
    _In_ uint32_t        count)
{
    uint64_t whole = 0;
    uint64_t remainder = 0;
    uint32_t i;

    // Each sample adds its whole share (sample / count) and its remainder
    // (sample % count). The remainders are carried into the whole part as
    // soon as they reach count, so remainder stays below count, and whole
    // never exceeds the largest sample. A plain sum could overflow.
    for (i = 0; i < count; i++) {
        whole += samples[i] / count;
        remainder += samples[i] % count;
        if (remainder >= count) {
            whole++;
            remainder -= count;
        }
    }

    // Round to the nearest nanosecond. remainder / count is the fraction.
    if (remainder * 2U >= count) {
        whole++;
    }
    return whole;
}

int
TimingSummarize(
    _InOut_ uint64_t*        samples,
    _In_    uint32_t         count,
    _Out_   TimingSummary_t* summary)
{
    if (count == 0) {
        return -1;
    }

    qsort(samples, count, sizeof(uint64_t), __CompareSamples);
    summary->Count = count;
    summary->Minimum = samples[0];
    summary->Median = TimingPercentile(samples, count, 50);
    summary->Percentile90 = TimingPercentile(samples, count, 90);
    summary->Maximum = samples[count - 1];
    summary->Mean = TimingMean(samples, count);
    return 0;
}

int
TimingFormatProbeLine(
    _Out_ char*                  text,
    _In_  size_t                 capacity,
    _In_  const char*            name,
    _In_  const TimingSummary_t* summary)
{
    int length;

    // Each value is printed as whole microseconds, a dot and the remaining
    // nanoseconds with three digits.
    length = snprintf(
        text,
        capacity,
        "probe %s %u calls, us min %llu.%03llu median %llu.%03llu p90 %llu.%03llu max %llu.%03llu "
        "mean %llu.%03llu",
        name,
        (unsigned)summary->Count,
        (unsigned long long)(summary->Minimum / __NSEC_PER_USEC),
        (unsigned long long)(summary->Minimum % __NSEC_PER_USEC),
        (unsigned long long)(summary->Median / __NSEC_PER_USEC),
        (unsigned long long)(summary->Median % __NSEC_PER_USEC),
        (unsigned long long)(summary->Percentile90 / __NSEC_PER_USEC),
        (unsigned long long)(summary->Percentile90 % __NSEC_PER_USEC),
        (unsigned long long)(summary->Maximum / __NSEC_PER_USEC),
        (unsigned long long)(summary->Maximum % __NSEC_PER_USEC),
        (unsigned long long)(summary->Mean / __NSEC_PER_USEC),
        (unsigned long long)(summary->Mean % __NSEC_PER_USEC)
    );
    if (length < 0 || (size_t)length >= capacity) {
        return -1;
    }
    return length;
}
