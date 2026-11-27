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
 * Host helper of testing/sdtiming_stats_test.py. It runs the SD timing
 * test's statistics code (testing/target/sdtiming/statistics.c) on the host,
 * so the test can compare its numbers with a reference in Python.
 *
 * Usage:
 *   sdtiming_stats_host <name> <capacity>
 *       Reads decimal samples in nanoseconds from standard input, one per
 *       line, and prints:
 *         summary <status> [<count> <min> <median> <p90> <max> <mean>]
 *         percentile <p> <value>      for p from 1 to 100, after sorting
 *         sorted <value>              every sample, after sorting
 *         line <length> <text>        TimingFormatProbeLine with a buffer of
 *                                     <capacity> bytes. The text is left out
 *                                     when the length is -1.
 *       With no samples only the summary line is printed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "statistics.h"

// At most this many samples are read, as many as the program takes.
#define __MAX_SAMPLES 100000

int
main(
    _In_ int    argc,
    _In_ char** argv)
{
    static uint64_t    samples[__MAX_SAMPLES];
    TimingSummary_t    summary;
    unsigned long long value;
    uint32_t           count = 0;
    uint32_t           percent;
    uint32_t           i;
    size_t             capacity;
    char*              line;
    int                status;
    int                length;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <name> <capacity>\n", argv[0]);
        return 2;
    }
    capacity = (size_t)strtoul(argv[2], NULL, 10);

    while (scanf("%llu", &value) == 1) {
        if (count == __MAX_SAMPLES) {
            fprintf(stderr, "more than %d samples\n", __MAX_SAMPLES);
            return 2;
        }
        samples[count++] = (uint64_t)value;
    }

    status = TimingSummarize(samples, count, &summary);
    if (status != 0) {
        printf("summary %d\n", status);
        return 0;
    }
    printf("summary %d %u %llu %llu %llu %llu %llu\n", status, (unsigned)summary.Count,
           (unsigned long long)summary.Minimum, (unsigned long long)summary.Median,
           (unsigned long long)summary.Percentile90, (unsigned long long)summary.Maximum,
           (unsigned long long)summary.Mean);
    for (percent = 1; percent <= 100; percent++) {
        printf("percentile %u %llu\n", (unsigned)percent,
               (unsigned long long)TimingPercentile(samples, count, percent));
    }
    for (i = 0; i < count; i++) {
        printf("sorted %llu\n", (unsigned long long)samples[i]);
    }

    line = malloc(capacity == 0 ? 1 : capacity);
    if (line == NULL) {
        fprintf(stderr, "out of memory\n");
        return 2;
    }
    length = TimingFormatProbeLine(line, capacity, argv[1], &summary);
    if (length < 0) {
        printf("line -1\n");
    } else {
        printf("line %d %s\n", length, line);
    }
    free(line);
    return 0;
}
