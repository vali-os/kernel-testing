/**
 * Copyright 2026, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
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
 * Host test for the kernel log ring buffer in kernel/output/log.c and for
 * SpinlockTryAcquireIrq in kernel/sync/spinlock.c. Run it through
 * kernel_log_test.py, which builds it with ASan and UBSan.
 *
 * Interrupt state is modelled as a nesting depth per thread. InterruptDisable
 * returns the depth before the call, and InterruptRestoreState sets it back.
 * A depth of 0 means interrupts are enabled.
 */

#ifndef LOG_SOURCE
#define LOG_SOURCE "../kernel/output/log.c"
#endif
#ifndef SPINLOCK_SOURCE
#define SPINLOCK_SOURCE "../kernel/sync/spinlock.c"
#endif

#include LOG_SOURCE
#include SPINLOCK_SOURCE

#include <stdio.h>
#include <stdlib.h>

// The host pthread.h cannot be included next to the Vali libc headers, as both
// define struct timespec. Declare the two calls the test needs. A pointer-sized
// handle matches pthread_t on 64 bit Linux and macOS hosts.
typedef void* HostThread_t;
extern int pthread_create(HostThread_t* thread, const void* attributes,
                          void* (*entry)(void*), void* argument);
extern int pthread_join(HostThread_t thread, void** result);

#define CHECK(condition) do { if (!(condition)) { \
    printf("line %d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

#define SERIAL_CAPTURE_SIZE (4 * 1024 * 1024)
#define READ_BUFFER_SIZE    (256 * 1024)

#define STRESS_THREADS  4
#define STRESS_MESSAGES 2000

static BootTerminal_t       g_terminal;
static char                 g_serial[SERIAL_CAPTURE_SIZE];
static size_t               g_serialLength;
static _Atomic(int)         g_serialWriters;
static int                  g_failAllocation;
static tick_t               g_now;
static _Thread_local irqstate_t g_interruptDepth;
static _Thread_local uuid_t     g_threadId = 7;
static uint8_t              g_readBuffer[READ_BUFFER_SIZE];

// When set, the next InterruptRestoreState(0) that happens while a CPU holds
// OutputState adds this message, as if another CPU logged it at that moment.
static const char*          g_lateMessage;

void
_assert_panic(
    _In_ const char* message)
{
    printf("%s\n", message);
    abort();
}

BootTerminal_t*
VideoGetTerminal(void)
{
    return &g_terminal;
}

void
SerialPutCharacter(
        _In_ int character)
{
    // Outside panic mode the printing CPU runs with interrupts disabled, so it
    // cannot be moved off the CPU while the other CPUs wait on its output.
    CHECK(g_interruptDepth > 0 || atomic_load(&g_kernelLog.Mode) == __LOG_MODE_PANIC);

    // Two CPUs printing at once would mix their lines together.
    CHECK(atomic_fetch_add(&g_serialWriters, 1) == 0);
    CHECK(g_serialLength + 1 < sizeof(g_serial));
    g_serial[g_serialLength++] = (char)character;
    g_serial[g_serialLength]   = '\0';
    atomic_fetch_sub(&g_serialWriters, 1);
}

oserr_t
SerialPortInitialize(void)
{
    return OS_EOK;
}

void
SystemTimerGetTimestamp(
        _Out_ tick_t* timestampOut)
{
    *timestampOut = g_now;
}

uuid_t
ArchGetProcessorCoreId(void)
{
    return 1;
}

uuid_t
ThreadCurrentHandle(void)
{
    return g_threadId;
}

void*
kmalloc(
    _In_ size_t size)
{
    if (g_failAllocation) {
        return NULL;
    }
    return malloc(size);
}

void
kfree(
    _In_ void* memory)
{
    free(memory);
}

void
MemorySpaceSyncProcessPending(void)
{
}

irqstate_t
InterruptDisable(void)
{
    return g_interruptDepth++;
}

irqstate_t
InterruptRestoreState(
    _In_ irqstate_t state)
{
    const char* message;

    g_interruptDepth = state;
    if (state == 0 && g_lateMessage != NULL &&
        atomic_load(&g_kernelLog.OutputState) == __LOG_OUTPUT_PRINTING) {
        message = g_lateMessage;
        g_lateMessage = NULL;
        LogAppendMessage(OSSYSLOGLEVEL_DEBUG, "%s", message);
    }
    return state;
}

static void
__ResetTest(
    _In_ unsigned int outputs)
{
    memset(&g_terminal, 0, sizeof(g_terminal));
    g_terminal.AvailableOutputs = outputs;
    memset(&g_kernelLog, 0, sizeof(g_kernelLog));
    g_serialLength   = 0;
    g_serial[0]      = '\0';
    g_failAllocation = 0;
    g_now            = 5 * NSEC_PER_MSEC;
    g_lateMessage    = NULL;
    LogInitialize();
}

static int
__Contains(
    _In_ const char* capture,
    _In_ size_t      length,
    _In_ const char* text)
{
    size_t textLength = strlen(text);

    for (size_t i = 0; i + textLength <= length; i++) {
        if (memcmp(&capture[i], text, textLength) == 0) {
            return 1;
        }
    }
    return 0;
}

static int
__SerialContains(
    _In_ const char* text)
{
    return __Contains(&g_serial[0], g_serialLength, text);
}

static size_t
__CountLines(
    _In_ const char* capture,
    _In_ size_t      length)
{
    size_t lines = 0;

    for (size_t i = 0; i < length; i++) {
        lines += capture[i] == '\n';
    }
    return lines;
}

/**
 * Reads every message from the given sequence on, in calls of chunkSize bytes,
 * and checks that the sequence numbers follow each other and that each text
 * reads "message <sequence>". Returns the number of messages read.
 */
static uint64_t
__ReadAndCheckAll(
    _In_  uint64_t  sequence,
    _In_  size_t    chunkSize,
    _Out_ uint64_t* droppedOut,
    _Out_ uint64_t* firstSequenceOut)
{
    OSKernelLogReadResult_t result;
    uint64_t                count    = 0;
    uint64_t                expected = 0;
    char                    text[OSKERNELLOG_MESSAGE_MAX + 1];
    char                    wanted[64];

    *droppedOut = 0;
    for (;;) {
        size_t offset = 0;

        CHECK(LogRead(sequence, &g_readBuffer[0], chunkSize, &result) == OS_EOK);
        if (count == 0) {
            *droppedOut       = result.Dropped;
            *firstSequenceOut = sequence + result.Dropped;
            expected          = *firstSequenceOut;
        } else {
            CHECK(result.Dropped == 0);
        }
        if (result.BytesRead == 0) {
            CHECK(result.NextSequence == sequence);
            break;
        }

        while (offset < result.BytesRead) {
            OSKernelLogRecord_t* record = (OSKernelLogRecord_t*)&g_readBuffer[offset];
            CHECK(record->Size % 8 == 0);
            CHECK(record->Size >= sizeof(OSKernelLogRecord_t) + record->Length);
            CHECK(record->Size < sizeof(OSKernelLogRecord_t) + record->Length + 8);
            CHECK(record->Sequence == expected);
            memcpy(&text[0], (char*)record + sizeof(OSKernelLogRecord_t), record->Length);
            text[record->Length] = '\0';
            snprintf(&wanted[0], sizeof(wanted), "message %llu", (unsigned long long)record->Sequence);
            CHECK(strcmp(&text[0], &wanted[0]) == 0);
            offset += record->Size;
            expected++;
            count++;
        }
        CHECK(offset == result.BytesRead);
        CHECK(result.NextSequence == expected);
        sequence = result.NextSequence;
    }
    return count;
}

static void
__LogNumberedMessages(
    _In_ uint64_t first,
    _In_ uint64_t count)
{
    for (uint64_t i = first; i < first + count; i++) {
        LogAppendMessage(OSSYSLOGLEVEL_DEBUG, "message %llu", (unsigned long long)i);
    }
}

static void
__TestPrintFormat(void)
{
    __ResetTest(VIDEO_UART);
    LogAppendMessage(OSSYSLOGLEVEL_WARNING, "hello %d", 42);
    CHECK(strncmp(&g_serial[0], "000000005 [warn-1-7] hello 42\n", g_serialLength) == 0);
    CHECK(g_serialLength == strlen("000000005 [warn-1-7] hello 42\n"));

    // Levels outside the enum must not index past the name table.
    LogAppendMessage((enum OSSysLogLevel)9, "odd level");
    CHECK(__SerialContains("[unknown-1-7] odd level\n"));
    CHECK(g_interruptDepth == 0);
}

static void
__TestHeldUntilOutputExists(void)
{
    // No serial port yet: messages wait in the buffer.
    __ResetTest(0);
    __LogNumberedMessages(0, 3);
    CHECK(g_serialLength == 0);

    // The SPCR path sets up the UART later and flushes the log.
    g_terminal.AvailableOutputs = VIDEO_UART;
    LogFlush();
    CHECK(__CountLines(&g_serial[0], g_serialLength) == 3);
    CHECK(strstr(&g_serial[0], "message 0\n") < strstr(&g_serial[0], "message 2\n"));

    // Printed once only.
    LogFlush();
    CHECK(__CountLines(&g_serial[0], g_serialLength) == 3);
}

static void
__TestTruncation(void)
{
    OSKernelLogReadResult_t result;
    OSKernelLogRecord_t*    record;
    char                    longText[400];

    __ResetTest(VIDEO_UART);
    memset(&longText[0], 'x', sizeof(longText) - 1);
    longText[sizeof(longText) - 1] = '\0';
    LogAppendMessage(OSSYSLOGLEVEL_DEBUG, "%s", &longText[0]);

    CHECK(LogRead(0, &g_readBuffer[0], sizeof(g_readBuffer), &result) == OS_EOK);
    record = (OSKernelLogRecord_t*)&g_readBuffer[0];
    CHECK(record->Length == OSKERNELLOG_MESSAGE_MAX);
    CHECK(record->Size == OSKERNELLOG_RECORD_MAX);
    CHECK(__CountLines(&g_serial[0], g_serialLength) == 1);
}

static void
__TestReadArguments(void)
{
    OSKernelLogReadResult_t result;

    __ResetTest(VIDEO_UART);
    __LogNumberedMessages(0, 2);

    CHECK(LogRead(0, NULL, 64, &result) == OS_EINVALPARAMS);
    CHECK(LogRead(0, &g_readBuffer[0], 64, NULL) == OS_EINVALPARAMS);
    CHECK(LogRead(3, &g_readBuffer[0], sizeof(g_readBuffer), &result) == OS_EINVALPARAMS);

    // Nothing new to read is not an error.
    CHECK(LogRead(2, &g_readBuffer[0], sizeof(g_readBuffer), &result) == OS_EOK);
    CHECK(result.BytesRead == 0);
    CHECK(result.NextSequence == 2);

    // A buffer too small for the next record.
    CHECK(LogRead(0, &g_readBuffer[0], sizeof(OSKernelLogRecord_t), &result) == OS_EBUFFER);
    CHECK(result.BytesRead == 0);
    CHECK(result.NextSequence == 0);
    CHECK(g_interruptDepth == 0);
}

static void
__TestReadSerialFlag(void)
{
    OSKernelLogReadResult_t result;

    // A log that prints to the serial port says so on every read, so the
    // syslog service does not print the same lines on a port a second time.
    __ResetTest(VIDEO_UART);
    __LogNumberedMessages(0, 1);
    memset(&result, 0xFF, sizeof(result));
    CHECK(LogRead(0, &g_readBuffer[0], sizeof(g_readBuffer), &result) == OS_EOK);
    CHECK(result.Flags == OSKERNELLOG_READ_SERIAL_CONSOLE);

    // Without a serial port the flag is clear, also on a read that finds
    // nothing new and on a read whose buffer is too small.
    __ResetTest(0);
    __LogNumberedMessages(0, 1);
    memset(&result, 0xFF, sizeof(result));
    CHECK(LogRead(0, &g_readBuffer[0], sizeof(g_readBuffer), &result) == OS_EOK);
    CHECK(result.Flags == 0);
    memset(&result, 0xFF, sizeof(result));
    CHECK(LogRead(1, &g_readBuffer[0], sizeof(g_readBuffer), &result) == OS_EOK);
    CHECK(result.Flags == 0);
    memset(&result, 0xFF, sizeof(result));
    CHECK(LogRead(0, &g_readBuffer[0], sizeof(OSKernelLogRecord_t), &result) == OS_EBUFFER);
    CHECK(result.Flags == 0);
}

static void
__TestWrapAndDrop(void)
{
    uint64_t dropped;
    uint64_t first;
    uint64_t count;

    // Many more messages than the boot buffer holds. Records must never cross
    // the end of the buffer (ASan catches that), and the oldest are dropped.
    __ResetTest(VIDEO_UART);
    __LogNumberedMessages(0, 3000);
    CHECK(__CountLines(&g_serial[0], g_serialLength) == 3000);

    count = __ReadAndCheckAll(0, sizeof(g_readBuffer), &dropped, &first);
    CHECK(dropped > 0);
    CHECK(first == dropped);
    CHECK(first + count == 3000);
    CHECK(g_kernelLog.TailSequence == first);

    // Reading in small pieces gives the same result.
    count = __ReadAndCheckAll(0, OSKERNELLOG_RECORD_MAX, &dropped, &first);
    CHECK(first + count == 3000);

    // Continuing from a sequence number that is still in the log.
    count = __ReadAndCheckAll(2990, 100, &dropped, &first);
    CHECK(dropped == 0);
    CHECK(first == 2990);
    CHECK(count == 10);
}

static void
__TestOutputDropped(void)
{
    char wanted[96];

    // Messages are dropped before the serial port exists. The first printed
    // line must say how many, and the rest must be the newest messages in order.
    __ResetTest(0);
    __LogNumberedMessages(0, 3000);
    g_terminal.AvailableOutputs = VIDEO_UART;
    LogFlush();

    snprintf(&wanted[0], sizeof(wanted),
             "[log] %llu messages were dropped before they were printed\n",
             (unsigned long long)g_kernelLog.TailSequence);
    CHECK(strncmp(&g_serial[0], &wanted[0], strlen(&wanted[0])) == 0);
    CHECK(__CountLines(&g_serial[0], g_serialLength) == 1 + (3000 - g_kernelLog.TailSequence));
    CHECK(__SerialContains("] message 2999\n"));
}

static void
__TestUpgrade(void)
{
    uint64_t dropped;
    uint64_t first;
    uint64_t count;
    uint64_t bootTail;

    // Wrapped boot buffer, nothing printed yet. After the move every message
    // keeps its sequence number and is printed exactly once.
    __ResetTest(0);
    __LogNumberedMessages(0, 500);
    bootTail = g_kernelLog.TailSequence;
    CHECK(bootTail > 0);

    LogInitializeFull();
    CHECK(g_kernelLog.Capacity == LOG_FULL_BUFFER_SIZE);
    CHECK(g_kernelLog.TailSequence == bootTail);
    CHECK(g_kernelLog.HeadSequence == 500);
    count = __ReadAndCheckAll(0, sizeof(g_readBuffer), &dropped, &first);
    CHECK(first == bootTail);
    CHECK(first + count == 500);

    __LogNumberedMessages(500, 100);
    g_terminal.AvailableOutputs = VIDEO_UART;
    LogFlush();
    CHECK(__CountLines(&g_serial[0], g_serialLength) == 1 + (600 - bootTail));
    CHECK(__SerialContains("] message 599\n"));
    free(g_kernelLog.Data);

    // Everything printed before the move: nothing is printed twice after it.
    __ResetTest(VIDEO_UART);
    __LogNumberedMessages(0, 10);
    LogInitializeFull();
    __LogNumberedMessages(10, 1);
    CHECK(__CountLines(&g_serial[0], g_serialLength) == 11);
    count = __ReadAndCheckAll(0, sizeof(g_readBuffer), &dropped, &first);
    CHECK(first == 0 && count == 11);
    free(g_kernelLog.Data);

    // Part printed before the move: printing continues where it stopped.
    __ResetTest(VIDEO_UART);
    __LogNumberedMessages(0, 4);
    g_terminal.AvailableOutputs = 0;
    __LogNumberedMessages(4, 4);
    LogInitializeFull();
    g_terminal.AvailableOutputs = VIDEO_UART;
    LogFlush();
    CHECK(__CountLines(&g_serial[0], g_serialLength) == 8);
    CHECK(__SerialContains("] message 7\n"));
    free(g_kernelLog.Data);

    // A failed allocation keeps the boot buffer and says so.
    __ResetTest(VIDEO_UART);
    g_failAllocation = 1;
    LogInitializeFull();
    CHECK(g_kernelLog.Capacity == LOG_BOOT_BUFFER_SIZE);
    CHECK(__SerialContains("LogInitializeFull failed to allocate"));
}

static void
__TestLateMessageIsPrinted(void)
{
    // Another CPU adds a message right after the printing CPU found nothing
    // more to print, but before it gave up OutputState. That CPU saw PRINTING
    // and returned, so the printing CPU must print the message.
    __ResetTest(VIDEO_UART);
    g_lateMessage = "late message";
    LogAppendMessage(OSSYSLOGLEVEL_DEBUG, "first message");
    CHECK(g_lateMessage == NULL);
    CHECK(__SerialContains("first message\n"));
    CHECK(__SerialContains("late message\n"));
    CHECK(atomic_load(&g_kernelLog.OutputState) == __LOG_OUTPUT_IDLE);
}

static void
__TestOtherCpuPrinting(void)
{
    // While another CPU holds OutputState, adding a message does not print it.
    __ResetTest(VIDEO_UART);
    atomic_store(&g_kernelLog.OutputState, __LOG_OUTPUT_PRINTING);
    LogAppendMessage(OSSYSLOGLEVEL_DEBUG, "waiting message");
    CHECK(g_serialLength == 0);

    // When that CPU is done, the next message prints both in order.
    atomic_store(&g_kernelLog.OutputState, __LOG_OUTPUT_IDLE);
    LogAppendMessage(OSSYSLOGLEVEL_DEBUG, "next message");
    CHECK(strstr(&g_serial[0], "waiting message\n") != NULL);
    CHECK(strstr(&g_serial[0], "waiting message\n") < strstr(&g_serial[0], "next message\n"));
}

static void
__TestPanicMode(void)
{
    // The printing CPU stopped while holding OutputState. Panic mode prints
    // the waiting messages anyway.
    __ResetTest(VIDEO_UART);
    atomic_store(&g_kernelLog.OutputState, __LOG_OUTPUT_PRINTING);
    LogAppendMessage(OSSYSLOGLEVEL_ERROR, "before panic");
    CHECK(g_serialLength == 0);
    LogEnterPanicMode();
    CHECK(__SerialContains("before panic\n"));
    LogAppendMessage(OSSYSLOGLEVEL_ERROR, "during panic");
    CHECK(__SerialContains("during panic\n"));

    // The ring lock is held by a CPU that will never release it. Messages are
    // still printed, without waiting forever.
    __ResetTest(VIDEO_UART);
    SpinlockAcquireIrq(&g_kernelLog.SyncObject);
    LogEnterPanicMode();
    LogAppendMessage(OSSYSLOGLEVEL_ERROR, "lock is stuck");
    CHECK(__SerialContains("[error-1-7] lock is stuck\n"));
    SpinlockReleaseIrq(&g_kernelLog.SyncObject);
    CHECK(g_interruptDepth == 0);

}

static void
__TestTryAcquire(void)
{
    Spinlock_t lock;

    SpinlockConstruct(&lock);
    CHECK(SpinlockTryAcquireIrq(&lock) == OS_EOK);
    CHECK(g_interruptDepth == 1);
    CHECK(SpinlockTryAcquireIrq(&lock) == OS_EBUSY);
    CHECK(g_interruptDepth == 1);
    SpinlockReleaseIrq(&lock);
    CHECK(g_interruptDepth == 0);

    // A waiter in line also makes the lock busy.
    atomic_store(&lock.Next, 2);
    atomic_store(&lock.Current, 1);
    CHECK(SpinlockTryAcquireIrq(&lock) == OS_EBUSY);
    CHECK(g_interruptDepth == 0);
}

static void*
__StressThread(
    _In_ void* context)
{
    g_threadId = (uuid_t)(uintptr_t)context;
    for (int i = 0; i < STRESS_MESSAGES; i++) {
        LogAppendMessage(OSSYSLOGLEVEL_DEBUG, "stress %u %d", g_threadId, i);
    }
    return NULL;
}

static void
__TestManyCpus(void)
{
    HostThread_t threads[STRESS_THREADS];
    int       lastSeen[STRESS_THREADS];
    uint64_t  dropped = 0;
    size_t    lines   = 0;
    char*     line;

    // Several CPUs log at once. Only one may print at a time (the serial stub
    // checks that), each CPU's messages must come out in order, and every
    // message is either printed or counted as dropped.
    __ResetTest(VIDEO_UART);
    for (int i = 0; i < STRESS_THREADS; i++) {
        lastSeen[i] = -1;
        CHECK(pthread_create(&threads[i], NULL, __StressThread, (void*)(uintptr_t)i) == 0);
    }
    for (int i = 0; i < STRESS_THREADS; i++) {
        CHECK(pthread_join(threads[i], NULL) == 0);
    }
    line = &g_serial[0];
    while (*line) {
        char*              end = strchr(line, '\n');
        unsigned int       thread;
        int                number;
        unsigned long long count;

        CHECK(end != NULL);
        if (sscanf(line, "[log] %llu messages were dropped", &count) == 1) {
            dropped += count;
        } else {
            CHECK(sscanf(strstr(line, "] ") + 2, "stress %u %d", &thread, &number) == 2);
            CHECK(thread < STRESS_THREADS);
            CHECK(number > lastSeen[thread]);
            lastSeen[thread] = number;
            lines++;
        }
        line = end + 1;
    }
    CHECK(lines + dropped == STRESS_THREADS * STRESS_MESSAGES);
    CHECK(g_kernelLog.OutputSequence == g_kernelLog.HeadSequence);
    CHECK(atomic_load(&g_kernelLog.OutputState) == __LOG_OUTPUT_IDLE);
}

int
main(void)
{
    __TestPrintFormat();
    __TestHeldUntilOutputExists();
    __TestTruncation();
    __TestReadArguments();
    __TestReadSerialFlag();
    __TestWrapAndDrop();
    __TestOutputDropped();
    __TestUpgrade();
    __TestLateMessageIsPrinted();
    __TestOtherCpuPrinting();
    __TestTryAcquire();
    __TestManyCpus();
    // Panic mode cannot be left, so it runs last.
    __TestPanicMode();
    printf("kernel log tests passed\n");
    return 0;
}
