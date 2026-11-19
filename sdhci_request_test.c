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
 * Host test for the SDHCI request engine (request.c and recovery.c), the
 * interrupt handling (interrupt.c) and the worker (worker.c). The fakes of
 * every outside call are in sdhci_fake_host.inc.
 *
 * On top of them this file scripts the hardware:
 * - A plan per command write says which status bits the host raises and
 *   when, and what the response registers hold.
 * - Script events latch status bits, change other registers, or start and
 *   end a fast handler call that runs "on another CPU" at a chosen time.
 * - __Run drives SdhciWorkerRunOnce the way the worker loop would: it moves
 *   the fake clock to the next wake or script event, latches what is due,
 *   delivers the interrupt by calling the real fast handler with a fake
 *   function table, then runs the next step.
 */

#include "sdhci_fake_host.inc"

#define EVENT_DESCRIPTOR 9
#define BUFFER_ADDRESS   0x20000000ULL
#define BLOCK_SIZE       512U
#define MAX_SCRIPT       64U
#define MAX_COMMANDS     8U
#define MAX_READS        256U
#define MAX_REPLIES      4U
#define RUN_LIMIT_US     10000000ULL

// Present state of a slot with a stable card and every line free and high.
#define PRESENT_IDLE (SDHCI_PRESENT_CARD_INSERTED | SDHCI_PRESENT_CARD_STABLE | \
                      SDHCI_PRESENT_WRITE_ENABLED | SDHCI_PRESENT_DAT_LEVEL_MASK | SDHCI_PRESENT_CMD_LEVEL)

#define CC SDHCI_INT_COMMAND_COMPLETE
#define TC SDHCI_INT_TRANSFER_COMPLETE

enum ScriptAction {
    // Latch Value into the interrupt status register (only enabled bits).
    ScriptStatus,
    // Store Value into the register at Offset with Width bytes.
    ScriptPoke,
    // A fast handler call starts on another CPU (Running goes up).
    ScriptHandlerEnter,
    // That call saves Value into Pending and returns (Running goes down).
    ScriptHandlerLeave
};

struct ScriptEvent {
    uint64_t          AtUs;
    enum ScriptAction Action;
    uint32_t          Offset;
    uint32_t          Width;
    uint32_t          Value;
    bool              Pending;
};

struct Reply {
    uint32_t DelayUs;
    uint32_t Status;
};

// What the fake host does after the Nth command write.
struct CommandPlan {
    struct Reply Replies[MAX_REPLIES];
    uint32_t     ReplyCount;
    uint32_t     Response[4];
};

struct ExpectedWrite {
    uint32_t Offset;
    uint32_t Width;
    uint32_t Value;
};

struct TestWork {
    SdWork_t Work;
    char     Name;
    uint32_t Runs;
    uint32_t RunsBeforeStop;
};

static SdhciController_t  g_controller;
static struct ScriptEvent g_script[MAX_SCRIPT];
static uint32_t           g_scriptCount;
static struct CommandPlan g_plans[MAX_COMMANDS];
static uint32_t           g_commandCount;
static uint32_t           g_commandWords[MAX_COMMANDS];
static uint32_t           g_commandArguments[MAX_COMMANDS];
static uint64_t           g_commandTimes[MAX_COMMANDS];
static bool               g_deliverInterrupts;
static uint32_t           g_delayLimitHz;
static uint64_t           g_statusReads[MAX_READS];
static uint32_t           g_statusReadCount;
static struct DMASegment  g_segments[600];

static uint32_t           g_completions;
static SdRequest_t*       g_completed;
static SdResult_t         g_result;
static uint64_t           g_completedAtUs;
static bool               g_inSubmit;
static void             (*g_onComplete)(SdRequest_t* request);
static char               g_order[32];
static uint32_t           g_orderLength;

// ---------------------------------------------------------------------------
// Scripted hardware
// ---------------------------------------------------------------------------

static void
__AddScript(
    _In_ uint64_t          atUs,
    _In_ enum ScriptAction action,
    _In_ uint32_t          offset,
    _In_ uint32_t          width,
    _In_ uint32_t          value)
{
    uint32_t kept = 0;
    uint32_t i;

    // Drop events that already happened, keeping the order of the rest.
    for (i = 0; i < g_scriptCount; i++) {
        if (g_script[i].Pending) {
            g_script[kept++] = g_script[i];
        }
    }
    g_scriptCount = kept;
    assert(g_scriptCount < MAX_SCRIPT);
    g_script[g_scriptCount++] = (struct ScriptEvent) { atUs, action, offset, width, value, true };
}

static void
__AddStatus(
    _In_ uint64_t atUs,
    _In_ uint32_t status)
{
    __AddScript(atUs, ScriptStatus, 0, 0, status);
}

static void
__AddPoke(
    _In_ uint64_t atUs,
    _In_ uint32_t offset,
    _In_ uint32_t width,
    _In_ uint32_t value)
{
    __AddScript(atUs, ScriptPoke, offset, width, value);
}

static void
__ApplyEvent(
    _In_ const struct ScriptEvent* event)
{
    SdhciInterruptShared_t* shared = &g_controller.Interrupt.Shared;
    uint32_t                enabled;

    switch (event->Action) {
        case ScriptStatus:
            // The host latches only the bits the status enable register allows.
            enabled = __LoadRegister(SDHCI_INTERRUPT_ENABLE, 4);
            __StoreRegister(SDHCI_INTERRUPT_STATUS, 4,
                __LoadRegister(SDHCI_INTERRUPT_STATUS, 4) | (event->Value & enabled));
            break;
        case ScriptPoke:
            __StoreRegister(event->Offset, event->Width, event->Value);
            break;
        case ScriptHandlerEnter:
            atomic_fetch_add(&shared->Running, 1);
            break;
        case ScriptHandlerLeave:
            atomic_fetch_or(&shared->Pending, event->Value);
            atomic_fetch_sub(&shared->Running, 1);
            break;
    }
}

static void
__LatchDue(void)
{
    uint32_t i;

    for (i = 0; i < g_scriptCount; i++) {
        if (g_script[i].Pending && g_script[i].AtUs <= g_nowUs) {
            g_script[i].Pending = false;
            __ApplyEvent(&g_script[i]);
        }
    }
}

static uint64_t
__NextScriptUs(void)
{
    uint64_t next = SDHCI_WAKE_NEVER;
    uint32_t i;

    for (i = 0; i < g_scriptCount; i++) {
        if (g_script[i].Pending && g_script[i].AtUs < next) {
            next = g_script[i].AtUs;
        }
    }
    return next;
}

static void
__OnRead(
    _In_ uint32_t offset)
{
    __LatchDue();
    if (offset == SDHCI_INTERRUPT_STATUS) {
        assert(g_statusReadCount < MAX_READS);
        g_statusReads[g_statusReadCount++] = g_nowUs;
    }
}

static void
__OnWrite(
    _In_ uint32_t offset,
    _In_ uint32_t width,
    _In_ uint32_t value)
{
    const struct CommandPlan* plan;
    uint32_t                  i;

    // The upper half of a 32-bit write at 0x0C, or a 16-bit write at 0x0E,
    // starts a command.
    if (!(offset == SDHCI_TRANSFER_MODE_COMMAND && width == 4) && !(offset == SDHCI_COMMAND && width == 2)) {
        return;
    }
    assert(g_commandCount < MAX_COMMANDS);
    plan = &g_plans[g_commandCount];
    g_commandWords[g_commandCount] = width == 4 ? value : value << SDHCI_COMMAND_WORD_SHIFT;
    g_commandArguments[g_commandCount] = __LoadRegister(SDHCI_ARGUMENT, 4);
    g_commandTimes[g_commandCount] = g_nowUs;
    for (i = 0; i < SDHCI_RESPONSE_COUNT; i++) {
        __StoreRegister(SDHCI_RESPONSE(i), 4, plan->Response[i]);
    }
    for (i = 0; i < plan->ReplyCount; i++) {
        __AddStatus(g_nowUs + plan->Replies[i].DelayUs, plan->Replies[i].Status);
    }
    g_commandCount++;
}

static void
__OnLineReset(void)
{
    uint32_t i;

    // A line reset aborts the command, so its later replies never come.
    for (i = 0; i < g_scriptCount; i++) {
        if (g_script[i].Action == ScriptStatus) {
            g_script[i].Pending = false;
        }
    }
}

static void
__Plan(
    _In_ uint32_t command,
    _In_ uint32_t delayUs,
    _In_ uint32_t status)
{
    struct CommandPlan* plan = &g_plans[command];

    assert(plan->ReplyCount < MAX_REPLIES);
    plan->Replies[plan->ReplyCount++] = (struct Reply) { delayUs, status };
}

static void
__PlanResponse(
    _In_ uint32_t command,
    _In_ uint32_t word0,
    _In_ uint32_t word1,
    _In_ uint32_t word2,
    _In_ uint32_t word3)
{
    g_plans[command].Response[0] = word0;
    g_plans[command].Response[1] = word1;
    g_plans[command].Response[2] = word2;
    g_plans[command].Response[3] = word3;
}

// ---------------------------------------------------------------------------
// Fast handler function table
// ---------------------------------------------------------------------------

static size_t
__FastRead(
    _In_ DeviceIo_t* ioSpace,
    _In_ size_t      offset,
    _In_ size_t      length)
{
    uint32_t value;

    assert(ioSpace == &g_controller.Registers);
    assert(length == 4);
    value = __LoadRegister((uint32_t)offset, 4);
    __Record(EventFastRead, (uint32_t)offset, 4, value);
    return value;
}

static oserr_t
__FastWrite(
    _In_ DeviceIo_t* ioSpace,
    _In_ size_t      offset,
    _In_ size_t      value,
    _In_ size_t      length)
{
    assert(ioSpace == &g_controller.Registers);
    assert(length == 4);
    __Record(EventFastWrite, (uint32_t)offset, 4, (uint32_t)value);
    if (!__StoreStatusWrite((uint32_t)offset, 4, (uint32_t)value)) {
        __StoreRegister((uint32_t)offset, 4, (uint32_t)value);
    }
    return OS_EOK;
}

// What the fake EventSignal returns. The kernel returns OS_EINCOMPLETE when
// it raised the event counter and OS_EOK when it was already raised.
static oserr_t g_signalResult = OS_EOK;

static oserr_t
__FastSignal(
    _In_ uuid_t handle)
{
    assert(handle == EVENT_DESCRIPTOR);
    __Record(EventSignal, 0, 0, 0);
    g_signals++;
    return g_signalResult;
}

static void
__FastTrace(
    _In_ const char* format, ...)
{
    (void)format;
}

static InterruptFunctionTable_t g_functionTable = {
    .ReadIoSpace = __FastRead,
    .WriteIoSpace = __FastWrite,
    .EventSignal = __FastSignal,
    .Trace = __FastTrace
};

static irqstatus_t
__CallFastHandler(void)
{
    assert(g_handlerResources.Handler == SdhciFastInterrupt);
    return g_handlerResources.Handler(&g_functionTable, &g_handlerResources);
}

static void
__DeliverInterrupt(void)
{
    uint32_t asserted;

    // The line is up while an enabled status bit is set.
    asserted = __LoadRegister(SDHCI_INTERRUPT_STATUS, 4) & __LoadRegister(SDHCI_INTERRUPT_SIGNAL, 4);
    if (!g_deliverInterrupts || !g_interruptEnabled || asserted == 0) {
        return;
    }
    if (__CallFastHandler() == IRQSTATUS_HANDLED) {
        SdhciInterruptDeferred(&g_controller);
    }
}

// ---------------------------------------------------------------------------
// Worker driving
// ---------------------------------------------------------------------------

static SdhciWake_t
__Step(void)
{
    SdhciWake_t wake;

    __LatchDue();
    __DeliverInterrupt();
    SdhciWorkerRunOnce(&g_controller, &wake);
    return wake;
}

static void
__Run(void)
{
    uint64_t    limit = g_nowUs + RUN_LIMIT_US;
    uint64_t    next;
    SdhciWake_t wake;

    // Like the worker loop: step, then wait for the wake or for the next
    // thing the hardware does, whichever comes first.
    for (;;) {
        wake = __Step();
        if (g_controller.Request.Active == NULL && g_controller.Worker.Work == NULL) {
            return;
        }
        next = wake.AtUs;
        if (__NextScriptUs() < next) {
            next = __NextScriptUs();
        }
        assert(next != SDHCI_WAKE_NEVER);
        if (next > g_nowUs) {
            g_nowUs = next;
        }
        assert(g_nowUs < limit);
    }
}

static void
__Complete(
    _In_ SdRequest_t* request)
{
    // Complete runs on the worker, outside Submit and outside the lock.
    assert(!g_inSubmit);
    assert(g_lockDepth == 0);
    assert(g_controller.Request.Active == NULL);
    g_completions++;
    g_completed = request;
    g_result = request->Result;
    g_completedAtUs = g_nowUs;
    if (g_onComplete != NULL) {
        g_onComplete(request);
    }
}

static oserr_t
__Submit(
    _In_ SdRequest_t* request)
{
    oserr_t status;

    g_inSubmit = true;
    status = g_controller.Host.Operations->Submit(&g_controller.Host, request);
    g_inSubmit = false;
    return status;
}

static oserr_t
__Stop(
    _In_ SdRequest_t* request)
{
    return g_controller.Host.Operations->Stop(&g_controller.Host, request);
}

static SdRequest_t
__Command(
    _In_ uint8_t             opcode,
    _In_ uint32_t            argument,
    _In_ enum SdResponseType responseType)
{
    SdRequest_t request;

    memset(&request, 0, sizeof(request));
    request.Opcode = opcode;
    request.Argument = argument;
    request.ResponseType = responseType;
    request.CommandTimeoutUs = 1000;
    request.DataTimeoutUs = SdResponseHasBusy(responseType) ? 5000 : 0;
    request.Complete = __Complete;
    return request;
}

static SdRequest_t
__Data(
    _In_ uint8_t              opcode,
    _In_ enum SdDataDirection direction,
    _In_ uint32_t             blocks,
    _In_ enum SdStopPolicy    stopPolicy)
{
    SdRequest_t request = __Command(opcode, 0x1000, SdResponseR1);

    request.Direction = direction;
    request.StopPolicy = stopPolicy;
    request.BlockSize = BLOCK_SIZE;
    request.BlockCount = blocks;
    request.DataTimeoutUs = 100000;
    g_segments[0] = (struct DMASegment) { BUFFER_ADDRESS, (uint64_t)BLOCK_SIZE * blocks };
    request.Data.Segments = g_segments;
    request.Data.SegmentCount = 1;
    request.Data.Length = (uint64_t)BLOCK_SIZE * blocks;
    return request;
}

static uint32_t
__TestWriteDelayUs(
    _In_ SdhciController_t* controller,
    _In_ uint32_t           cardClockHz)
{
    // Like BCM2711: a delay at or below the limit, the stopped clock included.
    (void)controller;
    return g_delayLimitHz != 0 && cardClockHz <= g_delayLimitHz ? 10U : 0U;
}

static void
__ResetTest(void)
{
    memset(g_script, 0, sizeof(g_script));
    g_scriptCount = 0;
    memset(g_plans, 0, sizeof(g_plans));
    g_commandCount = 0;
    g_deliverInterrupts = true;
    g_delayLimitHz = 0;
    g_statusReadCount = 0;
    g_completions = 0;
    g_completed = NULL;
    // Line resets finish at once unless a case asks otherwise, so the
    // completion times below are exact.
    g_lineResetBusyReads = 0;
    memset(&g_result, 0, sizeof(g_result));
    g_completedAtUs = 0;
    g_inSubmit = false;
    g_onComplete = NULL;
    memset(g_order, 0, sizeof(g_order));
    g_orderLength = 0;
}

static void
__PrepareOnly(
    _In_ uint32_t capabilities)
{
    __DefaultHost();
    __SetHost(SDHCI_SPEC_300, capabilities);
    __ResetTest();
    g_integration.WriteDelayUs = __TestWriteDelayUs;
    __Prepare(&g_controller);
    __StoreRegister(SDHCI_PRESENT_STATE, 4, PRESENT_IDLE);
}

static void
__SetupWith(
    _In_ uint32_t capabilities)
{
    __PrepareOnly(capabilities);
    assert(SdhciInterruptSetup(&g_controller, EVENT_DESCRIPTOR) == OS_EOK);
    assert(SdhciControllerStart(&g_controller) == OS_EOK);
    g_onRead = __OnRead;
    g_onWrite = __OnWrite;
    g_onLineReset = __OnLineReset;
}

static void
__Setup(void)
{
    __SetupWith(__DefaultCapabilities());
}

static void
__Teardown(void)
{
    assert(g_controller.Request.Active == NULL);
    __DestroyAll(&g_controller);
}

static void
__ExpectWrites(
    _In_ uint32_t                    start,
    _In_ const struct ExpectedWrite* expected,
    _In_ uint32_t                    count)
{
    uint32_t found = 0;
    uint32_t i;

    // Exactly these writes, in this order, with these widths and values.
    for (i = start; i < g_eventCount; i++) {
        if (g_events[i].Kind != EventWrite) {
            continue;
        }
        assert(found < count);
        assert(g_events[i].Offset == expected[found].Offset);
        assert(g_events[i].Width == expected[found].Width);
        assert(g_events[i].Value == expected[found].Value);
        found++;
    }
    assert(found == count);
}

static uint32_t
__WritesSince(
    _In_ uint32_t start)
{
    uint32_t count = 0;
    uint32_t i;

    for (i = start; i < g_eventCount; i++) {
        if (g_events[i].Kind == EventWrite) {
            count++;
        }
    }
    return count;
}

static int
__FindInterruptCall(
    _In_ enum InterruptCall call)
{
    uint32_t i;

    for (i = 0; i < g_eventCount; i++) {
        if (g_events[i].Kind == EventInterrupt && g_events[i].Offset == (uint32_t)call) {
            return (int)i;
        }
    }
    return -1;
}

static uint32_t
__CommandOf(
    _In_ uint32_t index)
{
    return g_commandWords[index] >> SDHCI_COMMAND_WORD_SHIFT;
}

static uint32_t
__ModeOf(
    _In_ uint32_t index)
{
    return g_commandWords[index] & 0xFFFFU;
}

static void
__ExpectResult(
    _In_ oserr_t  status,
    _In_ uint32_t transportErrors)
{
    assert(g_completions == 1);
    assert(g_result.Status == status);
    assert(g_result.TransportErrors == transportErrors);
}

static void
__Note(
    _In_ char name)
{
    assert(g_orderLength + 1 < sizeof(g_order));
    g_order[g_orderLength++] = name;
}

// ---------------------------------------------------------------------------
// Submit checks
// ---------------------------------------------------------------------------

static void
__ExpectRejected(
    _In_ SdRequest_t* request,
    _In_ oserr_t      expected)
{
    uint32_t mark = g_eventCount;
    uint32_t completions = g_completions;

    assert(__Submit(request) == expected);
    assert(__WritesSince(mark) == 0);
    assert(g_controller.Request.Active != request);
    assert(g_completions == completions);
}

static void
__TestSubmitChecks(void)
{
    SdRequest_t request;
    SdRequest_t other;
    uint32_t    i;
    uint32_t    mark;

    __Setup();

    // Every field outside the contract or the limits.
    request = __Command(13, 0, SdResponseR1); request.Complete = NULL; __ExpectRejected(&request, OS_EINVALPARAMS);
    request = __Command(64, 0, SdResponseR1); __ExpectRejected(&request, OS_EINVALPARAMS);
    request = __Command(13, 0, (enum SdResponseType)9); __ExpectRejected(&request, OS_EINVALPARAMS);
    request = __Command(13, 0, SdResponseR1); request.Direction = (enum SdDataDirection)5;
    __ExpectRejected(&request, OS_EINVALPARAMS);
    request = __Command(13, 0, SdResponseR1); request.StopPolicy = (enum SdStopPolicy)5;
    __ExpectRejected(&request, OS_EINVALPARAMS);
    request = __Command(13, 0, SdResponseR1); request.CommandTimeoutUs = 0; __ExpectRejected(&request, OS_EINVALPARAMS);
    request = __Command(13, 0, SdResponseR1); request.BlockSize = 512; __ExpectRejected(&request, OS_EINVALPARAMS);
    request = __Command(13, 0, SdResponseR1); request.Data.Length = 512; __ExpectRejected(&request, OS_EINVALPARAMS);
    request = __Command(13, 0, SdResponseR1); request.StopPolicy = SdStopCmd12; __ExpectRejected(&request, OS_EINVALPARAMS);
    request = __Command(7, 0, SdResponseR1b); request.DataTimeoutUs = 0; __ExpectRejected(&request, OS_EINVALPARAMS);
    request = __Data(17, SdDataRead, 1, SdStopNone); request.DataTimeoutUs = 0; __ExpectRejected(&request, OS_EINVALPARAMS);
    request = __Data(17, SdDataRead, 1, SdStopNone); request.BlockSize = 0; __ExpectRejected(&request, OS_EINVALPARAMS);
    request = __Data(17, SdDataRead, 1, SdStopNone); request.BlockSize = 1024; __ExpectRejected(&request, OS_EINVALPARAMS);
    request = __Data(18, SdDataRead, 1, SdStopCmd12); request.BlockCount = 0; __ExpectRejected(&request, OS_EINVALPARAMS);
    request = __Data(18, SdDataRead, 1, SdStopCmd12); request.BlockCount = 65536; __ExpectRejected(&request, OS_EINVALPARAMS);
    request = __Data(17, SdDataRead, 1, SdStopNone); request.Data.Segments = NULL; __ExpectRejected(&request, OS_EINVALPARAMS);
    request = __Data(17, SdDataRead, 1, SdStopNone); request.Data.SegmentCount = 0; __ExpectRejected(&request, OS_EINVALPARAMS);
    request = __Data(17, SdDataRead, 1, SdStopNone); request.Data.Length = 1024; __ExpectRejected(&request, OS_EINVALPARAMS);
    request = __Data(18, SdDataRead, 2, SdStopNone); __ExpectRejected(&request, OS_EINVALPARAMS);

    // The ADMA build rejects a misaligned segment and more segments than
    // the table holds (513 entries in a 4 KiB 32-bit table is one too many).
    request = __Data(17, SdDataRead, 1, SdStopNone);
    g_segments[0].Address = BUFFER_ADDRESS + 2;
    __ExpectRejected(&request, OS_EINVALPARAMS);
    request = __Data(18, SdDataRead, 513, SdStopCmd12);
    request.BlockSize = 4;
    request.Data.Length = 513 * 4;
    request.Data.SegmentCount = 513;
    for (i = 0; i < 513; i++) {
        g_segments[i] = (struct DMASegment) { BUFFER_ADDRESS + i * 8, 4 };
    }
    __ExpectRejected(&request, OS_EINVALPARAMS);
    assert(strstr(g_log, "building the ADMA table") != NULL);

    // A host that needs Auto CMD12 rejects an explicit stop.
    g_controller.Host.Limits.Flags |= SD_HOST_FLAG_AUTO_CMD12_REQUIRED;
    request = __Data(18, SdDataRead, 2, SdStopCmd12);
    __ExpectRejected(&request, OS_EINVALPARAMS);
    g_controller.Host.Limits.Flags &= ~SD_HOST_FLAG_AUTO_CMD12_REQUIRED;

    // No card in a slot with card detect.
    __StoreRegister(SDHCI_PRESENT_STATE, 4, PRESENT_IDLE & ~SDHCI_PRESENT_CARD_INSERTED);
    request = __Command(13, 0, SdResponseR1);
    __ExpectRejected(&request, OS_ENOENT);
    __StoreRegister(SDHCI_PRESENT_STATE, 4, PRESENT_IDLE);

    // A failed controller.
    g_controller.Failed = true;
    __ExpectRejected(&request, OS_EDEVFAULT);
    g_controller.Failed = false;

    // An accepted request writes no register and does not complete inside
    // Submit. A second one is busy.
    mark = g_eventCount;
    assert(__Submit(&request) == OS_EOK);
    assert(__WritesSince(mark) == 0 && g_completions == 0);
    other = __Command(13, 0, SdResponseR1);
    __ExpectRejected(&other, OS_EBUSY);
    __Plan(0, 100, CC);
    __Run();
    __ExpectResult(OS_EOK, 0);
    __Teardown();

    // A controller whose interrupt is not set up.
    __PrepareOnly(__DefaultCapabilities());
    request = __Command(13, 0, SdResponseR1);
    __ExpectRejected(&request, OS_EINVALPARAMS);
    __Teardown();
    printf("SDHCI submit checks passed\n");
}

// ---------------------------------------------------------------------------
// Programming order and command encoding
// ---------------------------------------------------------------------------

static void
__TestProgrammingOrder(void)
{
    SdRequest_t      request;
    SdhciAdmaEntry_t entry;
    uint32_t         table;
    uint32_t         mark;

    // A command without data: clear stale status, arm, argument, then the
    // combined transfer mode and command write.
    __Setup();
    request = __Command(13, 0x12340000, SdResponseR1);
    __Plan(0, 100, CC);
    assert(__Submit(&request) == OS_EOK);
    mark = g_eventCount;
    __Run();
    {
        // The fast handler acknowledges command complete with its own write
        // (a fast write, not in this list). The fence at the end switches
        // signals off and clears the status.
        const struct ExpectedWrite expected[] = {
            { SDHCI_INTERRUPT_STATUS, 4, SDHCI_INT_HANDLED },
            { SDHCI_INTERRUPT_SIGNAL, 4, SDHCI_INT_HANDLED },
            { SDHCI_ARGUMENT, 4, 0x12340000 },
            { SDHCI_TRANSFER_MODE_COMMAND, 4, 0x0D1AU << 16 },
            { SDHCI_INTERRUPT_SIGNAL, 4, 0 },
            { SDHCI_INTERRUPT_STATUS, 4, SDHCI_INT_HANDLED }
        };
        __ExpectWrites(mark, expected, 6);
        assert(__Find(EventFastWrite, SDHCI_INTERRUPT_STATUS, CC, mark) > 0);
    }
    __ExpectResult(OS_EOK, 0);
    assert(g_result.HardwareStopped && g_result.ConfirmedBytes == 0);
    __Teardown();

    // A single-block read: the descriptor table is built in Submit, then the
    // ADMA address, timeout, block size and count, argument, command.
    __Setup();
    table = (uint32_t)g_controller.Dma.Table.DeviceAddress;
    request = __Data(17, SdDataRead, 1, SdStopNone);
    __Plan(0, 100, CC);
    __Plan(0, 400, TC);
    assert(__Submit(&request) == OS_EOK);
    assert(g_controller.Dma.Table.EntryCount == 1);
    assert(SdhciAdmaTableReadEntry(&g_controller.Dma.Table, 0, &entry) == OS_EOK);
    assert(entry.Address == BUFFER_ADDRESS && entry.LengthField == BLOCK_SIZE);
    assert(entry.Attributes == (SDHCI_ADMA_ATTRIBUTE_VALID | SDHCI_ADMA_ATTRIBUTE_END | SDHCI_ADMA_ACTION_TRANSFER));
    mark = g_eventCount;
    assert(__Step().AtUs != SDHCI_WAKE_NEVER);
    {
        const struct ExpectedWrite expected[] = {
            { SDHCI_INTERRUPT_STATUS, 4, SDHCI_INT_HANDLED },
            { SDHCI_INTERRUPT_SIGNAL, 4, SDHCI_INT_HANDLED },
            { SDHCI_ADMA_ADDRESS, 4, table },
            { SDHCI_TIMEOUT_CONTROL, 1, SDHCI_TIMEOUT_VALUE_MAXIMUM },
            { SDHCI_BLOCK_SIZE_COUNT, 4, BLOCK_SIZE | (1U << 16) },
            { SDHCI_ARGUMENT, 4, 0x1000 },
            { SDHCI_TRANSFER_MODE_COMMAND, 4, (0x113AU << 16) | 0x0011U }
        };
        __ExpectWrites(mark, expected, 7);
    }
    __Run();
    __ExpectResult(OS_EOK, 0);
    assert(g_result.ConfirmedBytes == BLOCK_SIZE && g_result.HardwareStopped);
    __Teardown();

    // An eight-block write with Auto CMD12: no read bit, block count and
    // multiple blocks set, Auto CMD12 selected.
    __Setup();
    request = __Data(25, SdDataWrite, 8, SdStopAutoCmd12);
    __Plan(0, 100, CC);
    __Plan(0, 900, TC);
    assert(__Submit(&request) == OS_EOK);
    mark = g_eventCount;
    __Run();
    assert(__Find(EventWrite, SDHCI_BLOCK_SIZE_COUNT, BLOCK_SIZE | (8U << 16), mark) > 0);
    assert(g_commandWords[0] == ((0x193AU << 16) | 0x0027U));
    assert(g_commandCount == 1);
    __ExpectResult(OS_EOK, 0);
    assert(g_result.ConfirmedBytes == 8 * BLOCK_SIZE);
    __Teardown();

    // The 64-bit format writes the high address word before the low word.
    __SetupWith(__DefaultCapabilities() | SDHCI_CAPS_64BIT_V3);
    assert(g_controller.Dma.Format == SdhciAdmaFormat64);
    request = __Data(17, SdDataRead, 1, SdStopNone);
    __Plan(0, 100, CC | TC);
    assert(__Submit(&request) == OS_EOK);
    mark = g_eventCount;
    __Run();
    assert(__Find(EventWrite, SDHCI_ADMA_ADDRESS_HIGH, 0, mark) > 0);
    assert(__Find(EventWrite, SDHCI_ADMA_ADDRESS_HIGH, 0, mark) <
           __Find(EventWrite, SDHCI_ADMA_ADDRESS, (uint32_t)g_controller.Dma.Table.DeviceAddress, mark));
    __ExpectResult(OS_EOK, 0);
    __Teardown();
    printf("SDHCI programming order passed\n");
}

static void
__TestCommandEncoding(void)
{
    struct {
        uint8_t             Opcode;
        enum SdResponseType Type;
        uint32_t            Bits;
    } cases[] = {
        { 0,  SdResponseNone, 0x0000 },
        { 13, SdResponseR1,   0x0D1A },
        { 7,  SdResponseR1b,  0x071B },
        { 2,  SdResponseR2,   0x0209 },
        { 41, SdResponseR3,   0x2902 },
        { 3,  SdResponseR6,   0x031A },
        { 8,  SdResponseR7,   0x081A },
        // A CMD12 the card layer sends itself also gets the abort type.
        { 12, SdResponseR1b,  0x0CDB }
    };
    SdRequest_t request;
    uint32_t    i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        __Setup();
        request = __Command(cases[i].Opcode, 0, cases[i].Type);
        __Plan(0, 100, SdResponseHasBusy(cases[i].Type) ? (CC | TC) : CC);
        assert(__Submit(&request) == OS_EOK);
        __Run();
        assert(__CommandOf(0) == cases[i].Bits && __ModeOf(0) == 0);
        __ExpectResult(OS_EOK, 0);
        __Teardown();
    }
    printf("SDHCI command encoding passed\n");
}

// ---------------------------------------------------------------------------
// Completion
// ---------------------------------------------------------------------------

static void
__RunRead(
    _In_ uint32_t first,
    _In_ uint32_t firstDelayUs,
    _In_ uint32_t second,
    _In_ uint32_t secondDelayUs)
{
    SdRequest_t request;

    __Setup();
    request = __Data(17, SdDataRead, 1, SdStopNone);
    __Plan(0, firstDelayUs, first);
    if (second != 0) {
        __Plan(0, secondDelayUs, second);
    }
    assert(__Submit(&request) == OS_EOK);
    __Run();
}

static void
__TestCompletion(void)
{
    SdRequest_t request;
    uint64_t    start;

    // Command and transfer complete in either order and together.
    __RunRead(CC, 100, TC, 300);
    __ExpectResult(OS_EOK, 0);
    assert(g_result.ConfirmedBytes == BLOCK_SIZE && g_result.HardwareStopped);
    __Teardown();
    __RunRead(TC, 100, CC, 300);
    __ExpectResult(OS_EOK, 0);
    assert(g_completedAtUs >= g_commandTimes[0] + 300);
    __Teardown();
    __RunRead(CC | TC, 100, 0, 0);
    __ExpectResult(OS_EOK, 0);
    __Teardown();

    // An error in the same snapshot as success wins, in either phase.
    __RunRead(CC | TC | SDHCI_INT_DATA_CRC, 100, 0, 0);
    __ExpectResult(OS_EDEVFAULT, SD_TRANSPORT_DATA_CRC);
    assert(g_result.ConfirmedBytes == 0 && g_result.HardwareStopped);
    __Teardown();
    __RunRead(CC, 100, TC | SDHCI_INT_DATA_CRC, 300);
    __ExpectResult(OS_EDEVFAULT, SD_TRANSPORT_DATA_CRC);
    assert(g_result.ConfirmedBytes == 0);
    __Teardown();
    __Setup();
    request = __Command(13, 0, SdResponseR1);
    __Plan(0, 100, CC | SDHCI_INT_COMMAND_CRC);
    assert(__Submit(&request) == OS_EOK);
    __Run();
    __ExpectResult(OS_EDEVFAULT, SD_TRANSPORT_COMMAND_CRC);
    __Teardown();

    // R1b waits for busy release after the response.
    __Setup();
    request = __Command(7, 0x10000, SdResponseR1b);
    __Plan(0, 100, CC);
    __Plan(0, 2500, TC);
    assert(__Submit(&request) == OS_EOK);
    start = g_nowUs;
    __Run();
    __ExpectResult(OS_EOK, 0);
    assert(g_completedAtUs == start + 2500);
    __Teardown();

    // Busy that never ends: the data deadline counts from the response, and
    // DAT0 held low makes it a busy timeout.
    __Setup();
    request = __Command(7, 0x10000, SdResponseR1b);
    __Plan(0, 100, CC);
    __AddPoke(g_nowUs + 100, SDHCI_PRESENT_STATE, 4, PRESENT_IDLE & ~SDHCI_PRESENT_DAT0_LEVEL);
    assert(__Submit(&request) == OS_EOK);
    start = g_nowUs;
    __Run();
    __ExpectResult(OS_ETIMEOUT, SD_TRANSPORT_BUSY_TIMEOUT);
    assert(g_completedAtUs == start + 100 + request.DataTimeoutUs);
    __Teardown();

    // The hardware data timeout on a command without data is a busy timeout.
    __Setup();
    request = __Command(7, 0x10000, SdResponseR1b);
    __Plan(0, 100, CC);
    __Plan(0, 300, SDHCI_INT_DATA_TIMEOUT);
    assert(__Submit(&request) == OS_EOK);
    __Run();
    __ExpectResult(OS_ETIMEOUT, SD_TRANSPORT_BUSY_TIMEOUT);
    __Teardown();
    printf("SDHCI completion passed\n");
}

static void
__TestResponses(void)
{
    SdRequest_t request;

    // 48-bit: response register 0 is the card status, the rest is zero.
    __Setup();
    request = __Command(13, 0, SdResponseR1);
    request.CardErrorMask = 0xFFF90008U;
    __PlanResponse(0, 0x80000900U, 0x11111111U, 0x22222222U, 0x33333333U);
    __Plan(0, 100, CC);
    assert(__Submit(&request) == OS_EOK);
    __Run();
    __ExpectResult(OS_EOK, 0);
    assert(g_result.Response[0] == 0x80000900U);
    assert(g_result.Response[1] == 0 && g_result.Response[2] == 0 && g_result.Response[3] == 0);
    assert(g_result.CardErrors == 0x80000000U);
    __Teardown();

    // R3 carries the OCR. The card error mask does not apply.
    __Setup();
    request = __Command(41, 0x40300000, SdResponseR3);
    request.CardErrorMask = 0xFFFFFFFFU;
    __PlanResponse(0, 0xC0FF8000U, 0, 0, 0);
    __Plan(0, 100, CC);
    assert(__Submit(&request) == OS_EOK);
    __Run();
    assert(g_result.Response[0] == 0xC0FF8000U && g_result.CardErrors == 0);
    __Teardown();

    // R2: the registers hold card bits 127:8 in bits 119:0. Normalized, the
    // value is shifted up by 8 and stored with bits 127:96 first.
    __Setup();
    request = __Command(2, 0, SdResponseR2);
    request.CardErrorMask = 0xFFFFFFFFU;
    __PlanResponse(0, 0x11223344U, 0x55667788U, 0x99AABBCCU, 0x00DDEEFFU);
    __Plan(0, 100, CC);
    assert(__Submit(&request) == OS_EOK);
    __Run();
    __ExpectResult(OS_EOK, 0);
    assert(g_result.Response[0] == 0xDDEEFF99U);
    assert(g_result.Response[1] == 0xAABBCC55U);
    assert(g_result.Response[2] == 0x66778811U);
    assert(g_result.Response[3] == 0x22334400U);
    assert(SdResponseBits(g_result.Response, 120, 8) == 0xDDU);
    assert(SdResponseBits(g_result.Response, 8, 8) == 0x44U);
    assert(g_result.CardErrors == 0);
    __Teardown();
    printf("SDHCI responses passed\n");
}

// ---------------------------------------------------------------------------
// Stop commands
// ---------------------------------------------------------------------------

static void
__TestAutoCmd12(void)
{
    SdRequest_t request;
    uint32_t    mark;

    // Its response comes from response register 3.
    __Setup();
    request = __Data(18, SdDataRead, 4, SdStopAutoCmd12);
    request.CardErrorMask = 0x80000000U;
    __PlanResponse(0, 0x900, 0, 0, 0x80000900U);
    __Plan(0, 100, CC);
    __Plan(0, 600, TC);
    assert(__Submit(&request) == OS_EOK);
    __Run();
    __ExpectResult(OS_EOK, 0);
    assert(__ModeOf(0) == 0x0037U && g_commandCount == 1);
    assert(g_result.StopResponse == 0x80000900U && g_result.CardErrors == 0x80000000U);
    assert(g_result.StopStatus == OS_EOK && g_result.ConfirmedBytes == 4 * BLOCK_SIZE);
    __Teardown();

    // An Auto CMD12 timeout: stop status from the Auto CMD status register,
    // line reset, then an explicit CMD12 since the card may still send.
    __Setup();
    request = __Data(18, SdDataRead, 4, SdStopAutoCmd12);
    __Plan(0, 100, CC);
    __AddPoke(g_nowUs + 600, SDHCI_AUTO_CMD_STATUS, 2, SDHCI_AUTO_CMD_TIMEOUT);
    __Plan(0, 600, SDHCI_INT_AUTO_CMD);
    __PlanResponse(1, 0x900, 0, 0, 0);
    __Plan(1, 50, CC);
    __Plan(1, 200, TC);
    assert(__Submit(&request) == OS_EOK);
    mark = g_eventCount;
    __Run();
    __ExpectResult(OS_ETIMEOUT, SD_TRANSPORT_STOP);
    assert(g_result.StopStatus == OS_ETIMEOUT && g_result.HardwareStopped);
    assert(g_result.ConfirmedBytes == 0);
    assert(g_commandCount == 2 && __CommandOf(1) == 0x0CDBU && g_commandArguments[1] == 0);
    assert(__Find(EventWrite, SDHCI_SOFTWARE_RESET, SDHCI_RESET_COMMAND | SDHCI_RESET_DATA, mark) > 0);
    assert(g_result.StopResponse == 0x900);
    __Teardown();

    // CRC, end bit, index and not executed are faults.
    __Setup();
    request = __Data(18, SdDataRead, 4, SdStopAutoCmd12);
    __Plan(0, 100, CC);
    __AddPoke(g_nowUs + 600, SDHCI_AUTO_CMD_STATUS, 2, SDHCI_AUTO_CMD_CRC);
    __Plan(0, 600, SDHCI_INT_AUTO_CMD);
    __Plan(1, 50, CC | TC);
    assert(__Submit(&request) == OS_EOK);
    __Run();
    __ExpectResult(OS_EDEVFAULT, SD_TRANSPORT_STOP);
    assert(g_result.StopStatus == OS_EDEVFAULT);
    __Teardown();
    printf("SDHCI Auto CMD12 passed\n");
}

static void
__TestExplicitStop(void)
{
    SdRequest_t request;

    // CMD12 after the data phase: abort type, R1b, argument 0, no transfer
    // mode, and it does not wait for data inhibit.
    __Setup();
    request = __Data(18, SdDataRead, 4, SdStopCmd12);
    request.CardErrorMask = 0x00080000U;
    __Plan(0, 100, CC);
    __Plan(0, 700, TC);
    __AddPoke(g_nowUs + 700, SDHCI_PRESENT_STATE, 4, PRESENT_IDLE | SDHCI_PRESENT_DATA_INHIBIT);
    __PlanResponse(1, 0x00080900U, 0, 0, 0);
    __Plan(1, 50, CC);
    __Plan(1, 150, TC);
    assert(__Submit(&request) == OS_EOK);
    __Run();
    __ExpectResult(OS_EOK, 0);
    assert(__ModeOf(0) == 0x0033U);
    assert(g_commandCount == 2 && __CommandOf(1) == 0x0CDBU && __ModeOf(1) == 0 && g_commandArguments[1] == 0);
    assert(g_commandTimes[1] == g_commandTimes[0] + 700);
    assert(g_result.StopResponse == 0x00080900U && g_result.CardErrors == 0x00080000U);
    assert(g_result.ConfirmedBytes == 4 * BLOCK_SIZE && g_result.HardwareStopped);
    __Teardown();

    // The CMD12 fails on its own: a read keeps its confirmed bytes, because
    // transfer complete put them in memory.
    __Setup();
    request = __Data(18, SdDataRead, 4, SdStopCmd12);
    __Plan(0, 100, CC | TC);
    __Plan(1, 50, SDHCI_INT_COMMAND_TIMEOUT);
    assert(__Submit(&request) == OS_EOK);
    __Run();
    __ExpectResult(OS_ETIMEOUT, SD_TRANSPORT_STOP);
    assert(g_result.StopStatus == OS_ETIMEOUT && g_result.HardwareStopped);
    assert(g_result.ConfirmedBytes == 4 * BLOCK_SIZE && g_commandCount == 2);
    __Teardown();

    // The same for a write confirms nothing: the card may not have stored it.
    __Setup();
    request = __Data(25, SdDataWrite, 4, SdStopCmd12);
    __Plan(0, 100, CC | TC);
    __Plan(1, 50, CC | SDHCI_INT_COMMAND_CRC);
    assert(__Submit(&request) == OS_EOK);
    __Run();
    __ExpectResult(OS_EDEVFAULT, SD_TRANSPORT_STOP);
    assert(g_result.ConfirmedBytes == 0 && g_result.HardwareStopped);
    __Teardown();

    // A data error: the hardware sent no CMD12, so the engine does.
    __Setup();
    request = __Data(18, SdDataRead, 4, SdStopCmd12);
    __Plan(0, 100, CC);
    __Plan(0, 400, SDHCI_INT_DATA_CRC);
    __PlanResponse(1, 0x00000B00U, 0, 0, 0);
    __Plan(1, 50, CC | TC);
    assert(__Submit(&request) == OS_EOK);
    __Run();
    __ExpectResult(OS_EDEVFAULT, SD_TRANSPORT_DATA_CRC);
    assert(g_commandCount == 2 && __CommandOf(1) == 0x0CDBU);
    assert(g_result.StopStatus == OS_EOK && g_result.StopResponse == 0x00000B00U);
    assert(g_result.ConfirmedBytes == 0 && g_result.HardwareStopped);
    __Teardown();

    // A single block ends by itself, no CMD12 after its error.
    __RunRead(CC, 100, SDHCI_INT_DATA_END_BIT, 300);
    __ExpectResult(OS_EDEVFAULT, SD_TRANSPORT_DATA_END_BIT);
    assert(g_commandCount == 1);
    __Teardown();
    printf("SDHCI explicit stop passed\n");
}

// ---------------------------------------------------------------------------
// Errors and recovery
// ---------------------------------------------------------------------------

static void
__TestErrorMapping(void)
{
    struct {
        uint32_t Bits;
        bool     Data;
        oserr_t  Status;
        uint32_t Transport;
    } cases[] = {
        { SDHCI_INT_COMMAND_TIMEOUT, false, OS_ETIMEOUT, SD_TRANSPORT_COMMAND_TIMEOUT },
        { SDHCI_INT_COMMAND_CRC, false, OS_EDEVFAULT, SD_TRANSPORT_COMMAND_CRC },
        { SDHCI_INT_COMMAND_END_BIT, false, OS_EDEVFAULT, SD_TRANSPORT_COMMAND_END_BIT },
        { SDHCI_INT_COMMAND_INDEX, false, OS_EDEVFAULT, SD_TRANSPORT_COMMAND_INDEX },
        { SDHCI_INT_COMMAND_TIMEOUT | SDHCI_INT_COMMAND_CRC, false, OS_ETIMEOUT,
          SD_TRANSPORT_COMMAND_TIMEOUT | SD_TRANSPORT_COMMAND_CRC },
        { SDHCI_INT_CURRENT_LIMIT, false, OS_EDEVFAULT, SD_TRANSPORT_CURRENT_LIMIT },
        { SDHCI_INT_DATA_TIMEOUT, true, OS_ETIMEOUT, SD_TRANSPORT_DATA_TIMEOUT },
        { SDHCI_INT_DATA_CRC, true, OS_EDEVFAULT, SD_TRANSPORT_DATA_CRC },
        { SDHCI_INT_DATA_END_BIT, true, OS_EDEVFAULT, SD_TRANSPORT_DATA_END_BIT },
        { SDHCI_INT_ADMA, true, OS_EDEVFAULT, SD_TRANSPORT_DMA }
    };
    SdRequest_t request;
    uint32_t    i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        __Setup();
        if (cases[i].Data) {
            request = __Data(17, SdDataRead, 1, SdStopNone);
            __Plan(0, 100, CC);
            __Plan(0, 300, cases[i].Bits);
        } else {
            request = __Command(13, 0, SdResponseR1);
            __Plan(0, 100, cases[i].Bits);
        }
        assert(__Submit(&request) == OS_EOK);
        __Run();
        __ExpectResult(cases[i].Status, cases[i].Transport);
        assert(g_result.HardwareStopped && g_result.ConfirmedBytes == 0);
        __Teardown();
    }

    // An ADMA error logs the state and the table entry at the ADMA address.
    __Setup();
    request = __Data(18, SdDataRead, 2, SdStopCmd12);
    g_segments[0] = (struct DMASegment) { BUFFER_ADDRESS, BLOCK_SIZE };
    g_segments[1] = (struct DMASegment) { BUFFER_ADDRESS + 0x10000, BLOCK_SIZE };
    request.Data.SegmentCount = 2;
    __Plan(0, 100, CC);
    __AddPoke(g_nowUs + 300, SDHCI_ADMA_ERROR_STATUS, 1, SDHCI_ADMA_ERROR_STATE_FETCH);
    __AddPoke(g_nowUs + 300, SDHCI_ADMA_ADDRESS, 4, (uint32_t)g_controller.Dma.Table.DeviceAddress + 8U);
    __Plan(0, 300, SDHCI_INT_ADMA);
    __Plan(1, 50, CC | TC);
    assert(__Submit(&request) == OS_EOK);
    __Run();
    __ExpectResult(OS_EDEVFAULT, SD_TRANSPORT_DMA);
    assert(strstr(g_log, "CMD18 (argument 0x00001000, read, 2 blocks) failed") != NULL);
    assert(strstr(g_log, "ADMA state 0x01") != NULL);
    assert(strstr(g_log, "(entry 1)") != NULL);
    __Teardown();
    printf("SDHCI error mapping passed\n");
}

static void
__TestRateLimit(void)
{
    SdRequest_t request;
    uint32_t    i;
    const char* first;

    // An empty slot fails the same probe again and again. It is logged once,
    // then once more with the count after the repeat limit.
    __Setup();
    for (i = 0; i <= SDHCI_ERROR_LOG_REPEAT_LIMIT; i++) {
        request = __Command(8, 0x1AA, SdResponseR7);
        memset(g_plans, 0, sizeof(g_plans));
        g_commandCount = 0;
        __Plan(0, 100, SDHCI_INT_COMMAND_TIMEOUT);
        g_completions = 0;
        assert(__Submit(&request) == OS_EOK);
        __Run();
        __ExpectResult(OS_ETIMEOUT, SD_TRANSPORT_COMMAND_TIMEOUT);
    }
    first = strstr(g_log, "CMD8 (argument");
    assert(first != NULL);
    first = strstr(first + 1, "CMD8 (argument");
    assert(first != NULL && strstr(first, "repeated 100 times") != NULL);
    assert(strstr(first + 1, "CMD8 (argument") == NULL);

    // A different error ends the run and reports what was left out.
    request = __Command(55, 0, SdResponseR1);
    memset(g_plans, 0, sizeof(g_plans));
    g_commandCount = 0;
    g_completions = 0;
    __Plan(0, 100, SDHCI_INT_COMMAND_CRC);
    assert(__Submit(&request) == OS_EOK);
    __Run();
    assert(strstr(g_log, "CMD55 (argument") != NULL);
    __Teardown();
    printf("SDHCI error rate limit passed\n");
}

static void
__TestStopProof(void)
{
    SdRequest_t request;
    uint32_t    mark;
    int         diagnostics;
    int         reset;
    int         presentAfter;

    // The proof: diagnostics read before the reset, one write with both
    // reset bits, the bits clear, then the present state shows no transfer.
    __Setup();
    request = __Data(17, SdDataRead, 1, SdStopNone);
    __Plan(0, 100, CC);
    __Plan(0, 300, SDHCI_INT_DATA_TIMEOUT);
    assert(__Submit(&request) == OS_EOK);
    mark = g_eventCount;
    __Run();
    diagnostics = __Find(EventRead, SDHCI_ADMA_ERROR_STATUS, ANY_VALUE, mark);
    reset = __Find(EventWrite, SDHCI_SOFTWARE_RESET, SDHCI_RESET_COMMAND | SDHCI_RESET_DATA, mark);
    presentAfter = __Find(EventRead, SDHCI_PRESENT_STATE, ANY_VALUE, (uint32_t)reset);
    assert(diagnostics > 0 && reset > diagnostics && presentAfter > reset);
    assert(g_events[reset].Width == 1);
    assert(__Find(EventRead, SDHCI_SOFTWARE_RESET, 0, (uint32_t)reset) > reset);
    assert(__Find(EventRead, SDHCI_SOFTWARE_RESET, 0, (uint32_t)reset) < presentAfter);
    __ExpectResult(OS_ETIMEOUT, SD_TRANSPORT_DATA_TIMEOUT);
    assert(g_result.HardwareStopped && !g_controller.Failed);
    __Teardown();

    // A reset bit that stays set proves nothing: the buffer stays with the
    // hardware and the controller fails until a full reset.
    __Setup();
    g_lineResetBusyReads = FOREVER;
    request = __Data(17, SdDataRead, 1, SdStopNone);
    __Plan(0, 100, CC);
    __Plan(0, 300, SDHCI_INT_DATA_CRC);
    assert(__Submit(&request) == OS_EOK);
    __Run();
    __ExpectResult(OS_EDEVFAULT, SD_TRANSPORT_DATA_CRC);
    assert(!g_result.HardwareStopped && g_controller.Failed);
    assert(g_liveBuffers == 2);
    request = __Command(13, 0, SdResponseR1);
    __ExpectRejected(&request, OS_EDEVFAULT);
    assert(SdhciControllerReset(&g_controller) == OS_EOK && !g_controller.Failed);
    g_lineResetBusyReads = 1;
    __Plan(1, 100, CC);
    g_completions = 0;
    assert(__Submit(&request) == OS_EOK);
    __Run();
    __ExpectResult(OS_EOK, 0);
    __Teardown();

    // A transfer still active after the reset proves nothing either.
    __Setup();
    g_transferStuck = true;
    request = __Data(25, SdDataWrite, 4, SdStopCmd12);
    __Plan(0, 100, CC);
    __AddPoke(g_nowUs + 300, SDHCI_PRESENT_STATE, 4, PRESENT_IDLE | SDHCI_PRESENT_WRITE_ACTIVE);
    __Plan(0, 300, SDHCI_INT_DATA_CRC);
    assert(__Submit(&request) == OS_EOK);
    __Run();
    __ExpectResult(OS_EDEVFAULT, SD_TRANSPORT_DATA_CRC);
    assert(!g_result.HardwareStopped && g_controller.Failed);
    // No CMD12 goes to a failed host.
    assert(g_commandCount == 1);
    g_transferStuck = false;
    __StoreRegister(SDHCI_PRESENT_STATE, 4, PRESENT_IDLE);
    __Teardown();
    printf("SDHCI stop proof passed\n");
}

// ---------------------------------------------------------------------------
// Deadlines
// ---------------------------------------------------------------------------

static void
__TestDeadlines(void)
{
    SdRequest_t request;
    uint64_t    start;

    // Command deadline, counted from the command write. The register is read
    // directly once more before the deadline counts.
    __Setup();
    request = __Command(13, 0, SdResponseR1);
    assert(__Submit(&request) == OS_EOK);
    __Run();
    __ExpectResult(OS_ETIMEOUT, SD_TRANSPORT_DEADLINE);
    assert(g_completedAtUs == g_commandTimes[0] + request.CommandTimeoutUs);
    assert(g_statusReadCount >= 1 && g_statusReads[0] == g_completedAtUs);
    assert(g_result.HardwareStopped);
    __Teardown();

    // Data deadline, counted from the response.
    __Setup();
    request = __Data(17, SdDataRead, 1, SdStopNone);
    __Plan(0, 100, CC);
    assert(__Submit(&request) == OS_EOK);
    __Run();
    __ExpectResult(OS_ETIMEOUT, SD_TRANSPORT_DEADLINE);
    assert(g_completedAtUs == g_commandTimes[0] + 100 + request.DataTimeoutUs);
    __Teardown();

    // A lost interrupt: the bits are latched but the line never fires. The
    // direct read at the deadline finds them, and the request succeeds.
    __Setup();
    g_deliverInterrupts = false;
    request = __Data(17, SdDataRead, 1, SdStopNone);
    __Plan(0, 100, CC | TC);
    assert(__Submit(&request) == OS_EOK);
    start = g_nowUs;
    __Run();
    __ExpectResult(OS_EOK, 0);
    assert(g_completedAtUs == start + request.CommandTimeoutUs);
    assert(strstr(g_log, "found it by reading the register") != NULL);
    __Teardown();

    // Lines that never become free: the wait to issue has the command
    // deadline too, and nothing is written.
    __Setup();
    __StoreRegister(SDHCI_PRESENT_STATE, 4, PRESENT_IDLE | SDHCI_PRESENT_COMMAND_INHIBIT);
    request = __Command(13, 0, SdResponseR1);
    assert(__Submit(&request) == OS_EOK);
    start = g_nowUs;
    __Run();
    __ExpectResult(OS_ETIMEOUT, SD_TRANSPORT_DEADLINE);
    assert(g_commandCount == 0 && g_completedAtUs == start + request.CommandTimeoutUs);
    __Teardown();
    printf("SDHCI deadlines passed\n");
}

// ---------------------------------------------------------------------------
// Stop requests
// ---------------------------------------------------------------------------

static void
__TestStopRequest(void)
{
    SdRequest_t request;
    SdRequest_t other;

    // While waiting for the response.
    __Setup();
    request = __Command(13, 0, SdResponseR1);
    assert(__Submit(&request) == OS_EOK);
    (void)__Step();
    assert(g_commandCount == 1 && g_completions == 0);
    other = __Command(13, 0, SdResponseR1);
    assert(__Stop(&other) == OS_ENOENT);
    assert(__Stop(&request) == OS_EOK);
    assert(__Stop(&request) == OS_EOK);
    __Run();
    __ExpectResult(OS_ECANCELLED, 0);
    assert(g_result.HardwareStopped);
    assert(__Find(EventWrite, SDHCI_SOFTWARE_RESET, SDHCI_RESET_COMMAND | SDHCI_RESET_DATA, 0) > 0);
    assert(__Stop(&request) == OS_ENOENT);
    __Teardown();

    // Before the command was written: no CMD12, nothing sent at all.
    __Setup();
    __StoreRegister(SDHCI_PRESENT_STATE, 4, PRESENT_IDLE | SDHCI_PRESENT_DATA_INHIBIT);
    request = __Data(18, SdDataRead, 4, SdStopCmd12);
    assert(__Submit(&request) == OS_EOK);
    (void)__Step();
    assert(__Stop(&request) == OS_EOK);
    __Run();
    __ExpectResult(OS_ECANCELLED, 0);
    assert(g_commandCount == 0 && g_result.HardwareStopped);
    __Teardown();

    // During the data phase: the engine sends CMD12 and completes once.
    __Setup();
    request = __Data(18, SdDataRead, 4, SdStopCmd12);
    __Plan(0, 100, CC);
    __Plan(1, 50, CC | TC);
    assert(__Submit(&request) == OS_EOK);
    (void)__Step();
    g_nowUs += 100;
    (void)__Step();
    assert(g_controller.Request.Phase == SdhciPhaseTransfer);
    assert(__Stop(&request) == OS_EOK);
    __Run();
    __ExpectResult(OS_ECANCELLED, 0);
    assert(g_commandCount == 2 && __CommandOf(1) == 0x0CDBU);
    assert(g_result.HardwareStopped && g_result.ConfirmedBytes == 0);
    __Teardown();

    // Once only the CMD12 is left, the request is already stopping and
    // keeps its result.
    __Setup();
    request = __Data(18, SdDataRead, 4, SdStopCmd12);
    __Plan(0, 100, CC | TC);
    __Plan(1, 100, CC | TC);
    assert(__Submit(&request) == OS_EOK);
    (void)__Step();
    g_nowUs += 100;
    (void)__Step();
    assert(g_controller.Request.Command.IsStop);
    assert(__Stop(&request) == OS_EOK);
    __Run();
    __ExpectResult(OS_EOK, 0);
    assert(g_result.ConfirmedBytes == 4 * BLOCK_SIZE);

    // After completion the request is no longer accepted.
    assert(__Stop(&request) == OS_ENOENT);
    __Teardown();
    printf("SDHCI stop requests passed\n");
}

// ---------------------------------------------------------------------------
// Stale status and the fast handler
// ---------------------------------------------------------------------------

static SdRequest_t g_next;

static void
__SubmitNext(
    _In_ SdRequest_t* request)
{
    (void)request;
    g_onComplete = NULL;
    g_next = __Command(13, 0, SdResponseR1);
    assert(g_controller.Host.Operations->Submit(&g_controller.Host, &g_next) == OS_EOK);
}

static void
__TestStaleStatus(void)
{
    SdRequest_t request;

    // A duplicate command complete latched after the first request ended,
    // while signals were off. The next command must not take it.
    __Setup();
    request = __Command(13, 0, SdResponseR1);
    __Plan(0, 100, CC);
    assert(__Submit(&request) == OS_EOK);
    __Run();
    __ExpectResult(OS_EOK, 0);
    __AddStatus(g_nowUs + 50, CC);
    g_nowUs += 100;
    __LatchDue();
    assert((__LoadRegister(SDHCI_INTERRUPT_STATUS, 4) & CC) != 0);
    g_completions = 0;
    assert(__Submit(&request) == OS_EOK);
    __Run();
    __ExpectResult(OS_ETIMEOUT, SD_TRANSPORT_DEADLINE);
    __Teardown();

    // A fast handler call that started for the first request on another CPU
    // and saves its bits only after that request has completed. The next
    // request, submitted from Complete, must not be credited with them.
    __Setup();
    request = __Command(13, 0, SdResponseR1);
    __Plan(0, 100, CC);
    __AddScript(g_nowUs + 100, ScriptHandlerEnter, 0, 0, 0);
    __AddScript(g_nowUs + 130, ScriptHandlerLeave, 0, 0, CC);
    g_onStall = __LatchDue;
    g_onComplete = __SubmitNext;
    assert(__Submit(&request) == OS_EOK);
    __Run();
    assert(g_completions == 2);
    assert(g_completed == &g_next);
    assert(g_result.Status == OS_ETIMEOUT && g_result.TransportErrors == SD_TRANSPORT_DEADLINE);
    assert(atomic_load(&g_controller.Interrupt.Shared.Running) == 0);
    __Teardown();

    // A handler call that never returns fails the controller.
    __Setup();
    request = __Command(13, 0, SdResponseR1);
    __Plan(0, 100, CC);
    __AddScript(g_nowUs + 100, ScriptHandlerEnter, 0, 0, 0);
    assert(__Submit(&request) == OS_EOK);
    __Run();
    __ExpectResult(OS_EOK, 0);
    assert(g_controller.Failed);
    assert(strstr(g_log, "did not return") != NULL);
    atomic_store(&g_controller.Interrupt.Shared.Running, 0);
    __Teardown();
    printf("SDHCI stale status passed\n");
}

static void
__CheckSignalResult(
    _In_ oserr_t  signalResult,
    _In_ uint32_t expectedFailures)
{
    SdRequest_t request;

    // One handled interrupt while the fake EventSignal returns signalResult.
    __Setup();
    request = __Command(13, 0, SdResponseR1);
    assert(__Submit(&request) == OS_EOK);
    (void)__Step();
    g_signalResult = signalResult;
    __StoreRegister(SDHCI_INTERRUPT_STATUS, 4, CC);
    assert(__CallFastHandler() == IRQSTATUS_HANDLED);
    g_signalResult = OS_EOK;
    assert(atomic_load(&g_controller.Interrupt.Shared.Failures) == expectedFailures);
    assert(atomic_load(&g_controller.Interrupt.Shared.Pending) == CC);

    // The deferred path takes and reports the count, and the request still
    // completes from the saved bits.
    SdhciInterruptDeferred(&g_controller);
    assert(atomic_load(&g_controller.Interrupt.Shared.Failures) == 0);
    (void)__Step();
    __ExpectResult(OS_EOK, 0);
    __Teardown();
}

static void
__TestFastHandler(void)
{
    SdRequest_t request;
    uint32_t    mark;
    uint32_t    i;

    // Signals off: the bit is not enabled, so nothing is taken or written.
    __Setup();
    __StoreRegister(SDHCI_INTERRUPT_STATUS, 4, CC);
    mark = g_eventCount;
    assert(__CallFastHandler() == IRQSTATUS_NOT_HANDLED);
    assert(g_eventCount == mark + 2);
    assert(g_events[mark].Kind == EventFastRead && g_events[mark].Offset == SDHCI_INTERRUPT_SIGNAL);
    assert(g_events[mark + 1].Kind == EventFastRead && g_events[mark + 1].Offset == SDHCI_INTERRUPT_STATUS);
    assert(__LoadRegister(SDHCI_INTERRUPT_STATUS, 4) == CC && g_signals == 0);
    assert(atomic_load(&g_controller.Interrupt.Shared.Pending) == 0);
    __StoreRegister(SDHCI_INTERRUPT_STATUS, 4, 0);

    // Signals on: one 32-bit write of exactly the enabled bits that were
    // seen, then the event. A bit outside the mask stays set.
    request = __Command(13, 0, SdResponseR1);
    assert(__Submit(&request) == OS_EOK);
    (void)__Step();
    assert(__LoadRegister(SDHCI_INTERRUPT_SIGNAL, 4) == SDHCI_INT_HANDLED);
    mark = g_eventCount;
    assert(__CallFastHandler() == IRQSTATUS_NOT_HANDLED);
    assert(__Find(EventFastWrite, SDHCI_INTERRUPT_STATUS, ANY_VALUE, mark) < 0);
    __StoreRegister(SDHCI_INTERRUPT_STATUS, 4, CC | SDHCI_INT_BUFFER_READ_READY);
    mark = g_eventCount;
    assert(__CallFastHandler() == IRQSTATUS_HANDLED);
    assert(g_eventCount == mark + 4);
    assert(g_events[mark + 2].Kind == EventFastWrite && g_events[mark + 2].Offset == SDHCI_INTERRUPT_STATUS);
    assert(g_events[mark + 2].Value == CC);
    assert(g_events[mark + 3].Kind == EventSignal);
    assert(__LoadRegister(SDHCI_INTERRUPT_STATUS, 4) == SDHCI_INT_BUFFER_READ_READY);
    assert(atomic_load(&g_controller.Interrupt.Shared.Pending) == CC);
    assert(atomic_load(&g_controller.Interrupt.Shared.Running) == 0);

    // The deferred path hands the bits to the worker and wakes it.
    g_controller.Worker.WakeRequested = false;
    SdhciInterruptDeferred(&g_controller);
    assert(atomic_load(&g_controller.Interrupt.Shared.Pending) == 0);
    assert(atomic_load(&g_controller.Interrupt.Deferred) == CC);
    assert(g_controller.Worker.WakeRequested);
    (void)__Step();
    __ExpectResult(OS_EOK, 0);

    // The handler only ever touched the interrupt status and signal enable.
    for (i = 0; i < g_eventCount; i++) {
        if (g_events[i].Kind == EventFastRead) {
            assert(g_events[i].Offset == SDHCI_INTERRUPT_STATUS || g_events[i].Offset == SDHCI_INTERRUPT_SIGNAL);
        }
        if (g_events[i].Kind == EventFastWrite) {
            assert(g_events[i].Offset == SDHCI_INTERRUPT_STATUS);
        }
    }
    __StoreRegister(SDHCI_INTERRUPT_STATUS, 4, 0);
    __Teardown();

    // Both results of a working kernel signal count as success: OS_EINCOMPLETE
    // (the counter was raised, the usual case) and OS_EOK (already raised).
    // Any other result is a failure the deferred path reports. The first
    // QEMU boot counted every interrupt as failed because of OS_EINCOMPLETE.
    __CheckSignalResult(OS_EINCOMPLETE, 0);
    __CheckSignalResult(OS_EOK, 0);
    __CheckSignalResult(OS_EINVALPARAMS, 1);
    printf("SDHCI fast handler passed\n");
}

// ---------------------------------------------------------------------------
// Polling
// ---------------------------------------------------------------------------

static void
__SetClock(
    _In_ uint32_t clockHz)
{
    SdBusSettings_t settings = __Settings(clockHz, SdBusWidth1, SdBusTimingDefault, SdBusPowerOn);
    uint32_t        actual;

    assert(SdhciControllerSetBus(&g_controller, &settings, &actual) == OS_EOK);
    assert(actual == clockHz);
}

static void
__RunPolled(
    _In_  SdRequest_t* request,
    _Out_ bool*        stalls,
    _In_  uint32_t     maximum)
{
    SdhciWake_t wake;
    uint32_t    count = 0;

    // Like __Run, but without interrupts and recording the wait kind.
    assert(__Submit(request) == OS_EOK);
    for (;;) {
        wake = __Step();
        if (g_controller.Request.Active == NULL) {
            return;
        }
        assert(count < maximum);
        stalls[count++] = wake.Stall;
        assert(wake.AtUs != SDHCI_WAKE_NEVER && wake.AtUs > g_nowUs);
        g_nowUs = wake.AtUs;
    }
}

static void
__TestPolling(void)
{
    SdRequest_t request;
    bool        stalls[64];
    uint64_t    start;
    uint32_t    mark;
    uint32_t    i;

    // A delay applies at 400 kHz: no signal is ever enabled, the handler on
    // the shared line takes nothing, and the worker reads every 50 us for
    // 2 ms, then every 1 ms.
    __Setup();
    g_delayLimitHz = 400000;
    __SetClock(400000);
    assert(SdhciInterruptUsePolling(&g_controller));
    request = __Command(13, 0, SdResponseR1);
    request.CommandTimeoutUs = 10000;
    __Plan(0, 3500, CC);
    mark = g_eventCount;
    __RunPolled(&request, stalls, 64);
    __ExpectResult(OS_EOK, 0);
    start = g_commandTimes[0];
    assert(__CountWrites(SDHCI_INTERRUPT_SIGNAL, mark) == 0);
    assert(__Find(EventFastRead, 0, ANY_VALUE, mark) < 0);
    assert(g_statusReadCount == 42);
    for (i = 0; i < 40; i++) {
        assert(g_statusReads[i] == start + 50 * (i + 1));
    }
    assert(g_statusReads[40] == start + 3000 && g_statusReads[41] == start + 4000);
    assert(g_completedAtUs == start + 4000);
    assert(stalls[0] && stalls[39] && !stalls[40] && !stalls[41]);
    assert(__Find(EventWrite, SDHCI_INTERRUPT_STATUS, CC, mark) > 0);
    assert(__CallFastHandler() == IRQSTATUS_NOT_HANDLED);

    // A deadline in the 50 us part ...
    g_completions = 0;
    g_statusReadCount = 0;
    request.CommandTimeoutUs = 1000;
    __RunPolled(&request, stalls, 64);
    __ExpectResult(OS_ETIMEOUT, SD_TRANSPORT_DEADLINE);
    assert(g_completedAtUs == g_commandTimes[1] + 1000 && g_statusReads[g_statusReadCount - 1] == g_completedAtUs);

    // ... and in the 1 ms part, where the last wait is cut at the deadline.
    g_completions = 0;
    g_statusReadCount = 0;
    request.CommandTimeoutUs = 4500;
    __RunPolled(&request, stalls, 64);
    __ExpectResult(OS_ETIMEOUT, SD_TRANSPORT_DEADLINE);
    assert(g_completedAtUs == g_commandTimes[2] + 4500);
    assert(g_statusReads[g_statusReadCount - 2] == g_commandTimes[2] + 4000);

    // The clock rises above 400 kHz: signals go on before the command.
    __SetClock(25000000);
    assert(!SdhciInterruptUsePolling(&g_controller));
    g_completions = 0;
    request.CommandTimeoutUs = 1000;
    __Plan(3, 100, CC);
    assert(__Submit(&request) == OS_EOK);
    mark = g_eventCount;
    __Run();
    __ExpectResult(OS_EOK, 0);
    {
        const struct ExpectedWrite expected[] = {
            { SDHCI_INTERRUPT_STATUS, 4, SDHCI_INT_HANDLED },
            { SDHCI_INTERRUPT_SIGNAL, 4, SDHCI_INT_HANDLED },
            { SDHCI_ARGUMENT, 4, 0 },
            { SDHCI_TRANSFER_MODE_COMMAND, 4, 0x0D1AU << 16 },
            { SDHCI_INTERRUPT_SIGNAL, 4, 0 },
            { SDHCI_INTERRUPT_STATUS, 4, SDHCI_INT_HANDLED }
        };
        __ExpectWrites(mark, expected, 6);
    }

    // Before a clock change, SetBus switches signals off first.
    assert(SdhciInterruptArm(&g_controller) == OS_EOK);
    mark = g_eventCount;
    __SetClock(400000);
    assert(g_events[__Find(EventWrite, SDHCI_INTERRUPT_SIGNAL, ANY_VALUE, mark)].Value == 0);
    assert(__Find(EventWrite, SDHCI_INTERRUPT_SIGNAL, 0, mark) < __Find(EventWrite, SDHCI_CLOCK_CONTROL, ANY_VALUE, mark));

    // Back at 400 kHz the next request is polled again.
    g_completions = 0;
    __Plan(4, 100, CC);
    mark = g_eventCount;
    __RunPolled(&request, stalls, 64);
    __ExpectResult(OS_EOK, 0);
    assert(__CountWrites(SDHCI_INTERRUPT_SIGNAL, mark) == 0);
    __Teardown();
    printf("SDHCI polling passed\n");
}

// ---------------------------------------------------------------------------
// Scheduled work and the worker
// ---------------------------------------------------------------------------

static void
__WorkRecord(
    _In_ SdWork_t* work)
{
    struct TestWork* item = (struct TestWork*)work;

    // The lock is never held while a work function runs.
    assert(g_lockDepth == 0);
    assert(!work->HostScheduled);
    item->Runs++;
    __Note(item->Name);
}

static void
__WorkAgain(
    _In_ SdWork_t* work)
{
    struct TestWork* item = (struct TestWork*)work;

    __WorkRecord(work);
    assert(g_controller.Host.Operations->CancelWork(&g_controller.Host, work) == OS_ENOENT);
    if (item->Runs < item->RunsBeforeStop) {
        assert(g_controller.Host.Operations->Schedule(&g_controller.Host, work, 0) == OS_EOK);
    }
}

static SdRequest_t g_workRequest;
static uint32_t    g_markInWork;

static void
__WorkSubmit(
    _In_ SdWork_t* work)
{
    // The request of this step has already completed.
    __WorkRecord(work);
    assert(g_completions == 1);
    g_workRequest = __Command(13, 0, SdResponseR1);
    assert(g_controller.Host.Operations->Submit(&g_controller.Host, &g_workRequest) == OS_EOK);
    g_markInWork = g_eventCount;
}

static void
__CompleteRecord(
    _In_ SdRequest_t* request)
{
    (void)request;
    __Note('R');
}

static void
__InitWork(
    _Out_ struct TestWork* item,
    _In_  char             name,
    _In_  SdWorkFn         function)
{
    memset(item, 0, sizeof(*item));
    item->Work.Function = function;
    item->Name = name;
}

static oserr_t
__Schedule(
    _In_ struct TestWork* item,
    _In_ uint32_t         delayUs)
{
    return g_controller.Host.Operations->Schedule(&g_controller.Host, &item->Work, delayUs);
}

static void
__TestScheduledWork(void)
{
    struct TestWork a, b, c, d, s, t;
    SdWork_t        empty;
    SdRequest_t     request;
    uint64_t        start;
    SdhciWake_t     wake;

    // Due order, and arrival order among equal due times.
    __Setup();
    __InitWork(&a, 'A', __WorkRecord);
    __InitWork(&b, 'B', __WorkRecord);
    __InitWork(&c, 'C', __WorkRecord);
    __InitWork(&d, 'D', __WorkRecord);
    start = g_nowUs;
    assert(__Schedule(&a, 100) == OS_EOK);
    assert(__Schedule(&b, 50) == OS_EOK);
    assert(__Schedule(&c, 100) == OS_EOK);
    assert(__Schedule(&d, 50) == OS_EOK);
    assert(a.Work.HostScheduled && a.Work.HostDueUs == start + 100);
    assert(__Schedule(&a, 0) == OS_EBUSY);
    assert(g_controller.Host.Operations->Schedule(&g_controller.Host, NULL, 0) == OS_EINVALPARAMS);
    memset(&empty, 0, sizeof(empty));
    assert(g_controller.Host.Operations->Schedule(&g_controller.Host, &empty, 0) == OS_EINVALPARAMS);
    wake = __Step();
    assert(g_orderLength == 0 && wake.AtUs == start + 50 && !wake.Stall);
    __Run();
    assert(strcmp(g_order, "BDAC") == 0);
    assert(g_nowUs == start + 100 && g_lockDepth == 0);

    // A function that schedules itself again cannot keep others out.
    g_orderLength = 0;
    memset(g_order, 0, sizeof(g_order));
    __InitWork(&s, 'S', __WorkAgain);
    s.RunsBeforeStop = 3;
    __InitWork(&t, 'T', __WorkRecord);
    assert(__Schedule(&s, 0) == OS_EOK);
    assert(__Schedule(&t, 0) == OS_EOK);
    __Run();
    assert(strcmp(g_order, "STSS") == 0);

    // CancelWork: scheduled, then again, never scheduled, finished.
    __InitWork(&a, 'A', __WorkRecord);
    assert(__Schedule(&a, 10) == OS_EOK);
    assert(g_controller.Host.Operations->CancelWork(&g_controller.Host, &a.Work) == OS_EOK);
    assert(!a.Work.HostScheduled);
    assert(g_controller.Host.Operations->CancelWork(&g_controller.Host, &a.Work) == OS_ENOENT);
    assert(g_controller.Host.Operations->CancelWork(&g_controller.Host, &b.Work) == OS_ENOENT);
    __Run();
    assert(a.Runs == 0);
    assert(__Schedule(&a, 0) == OS_EOK);
    __Run();
    assert(a.Runs == 1);
    assert(g_controller.Host.Operations->CancelWork(&g_controller.Host, &a.Work) == OS_ENOENT);

    // Request processing and a work function in the same step: the request
    // completes first, and the request the work function submits is not
    // touched again in that step.
    g_orderLength = 0;
    memset(g_order, 0, sizeof(g_order));
    request = __Command(13, 0, SdResponseR1);
    g_plans[g_commandCount].ReplyCount = 0;
    __Plan(g_commandCount, 100, CC);
    assert(__Submit(&request) == OS_EOK);
    (void)__Step();
    __InitWork(&d, 'W', __WorkSubmit);
    assert(__Schedule(&d, 100) == OS_EOK);
    g_onComplete = __CompleteRecord;
    g_completions = 0;
    g_nowUs += 100;
    wake = __Step();
    assert(strcmp(g_order, "RW") == 0);
    assert(g_controller.Request.Active == &g_workRequest);
    assert(__WritesSince(g_markInWork) == 0);
    assert(wake.AtUs <= g_nowUs);
    g_onComplete = NULL;
    assert(__Stop(&g_workRequest) == OS_EOK);
    __Run();
    __Teardown();
    printf("SDHCI scheduled work passed\n");
}

static void
__StopOnUntimedWait(
    _In_ bool timed)
{
    // Another job calls SdhciWorkerStop while the loop waits without a time.
    if (!timed) {
        g_controller.Worker.StopRequested = true;
    }
}

static void
__TestWorkerLoop(void)
{
    struct TestWork a, b, c;
    SdRequest_t     request;
    uint64_t        start;

    __Setup();
    assert(SdhciWorkerStart(&g_controller) == OS_EOK);
    assert(g_jobEntry != NULL && g_jobArgument == &g_controller);
    assert(SdhciWorkerStart(&g_controller) == OS_EBUSY);

    // The loop runs both items, sleeping until the second is due, then ends
    // once the stop request arrives.
    __InitWork(&a, 'A', __WorkRecord);
    __InitWork(&b, 'B', __WorkRecord);
    start = g_nowUs;
    assert(__Schedule(&a, 0) == OS_EOK);
    assert(__Schedule(&b, 500) == OS_EOK);
    g_onConditionWait = __StopOnUntimedWait;
    g_jobEntry(g_jobArgument, NULL);
    assert(strcmp(g_order, "AB") == 0 && g_nowUs == start + 500);
    assert(SdhciWorkerStop(&g_controller) == OS_EOK);
    assert(g_joins == 1 && g_controller.Worker.Job == UUID_INVALID);
    assert(SdhciWorkerStop(&g_controller) == OS_EOK && g_joins == 1);

    // A stop with a request accepted waits for its completion, and a work
    // item left over is removed without running.
    assert(SdhciWorkerStart(&g_controller) == OS_EOK);
    request = __Command(13, 0, SdResponseR1);
    __Plan(0, 100, CC);
    assert(__Submit(&request) == OS_EOK);
    __InitWork(&c, 'C', __WorkRecord);
    assert(__Schedule(&c, 1000000) == OS_EOK);
    g_controller.Worker.StopRequested = true;
    g_jobEntry(g_jobArgument, NULL);
    __ExpectResult(OS_EOK, 0);
    assert(c.Runs == 0 && !c.Work.HostScheduled && g_controller.Worker.Work == NULL);
    assert(strstr(g_log, "1 work items that did not run") != NULL);
    assert(SdhciWorkerStop(&g_controller) == OS_EOK);
    __Teardown();
    printf("SDHCI worker loop passed\n");
}

// ---------------------------------------------------------------------------
// Controller rules while a request runs, and interrupt setup
// ---------------------------------------------------------------------------

static void
__TestBusyController(void)
{
    SdRequest_t     request;
    SdBusSettings_t settings;
    uint32_t        clockHz;
    uint32_t        mark;
    int             reset;

    __Setup();
    request = __Command(13, 0, SdResponseR1);
    assert(__Submit(&request) == OS_EOK);
    mark = g_eventCount;
    settings = __Settings(400000, SdBusWidth1, SdBusTimingDefault, SdBusPowerOn);
    assert(g_controller.Host.Operations->Reset(&g_controller.Host) == OS_EBUSY);
    assert(g_controller.Host.Operations->SetBus(&g_controller.Host, &settings, &clockHz) == OS_EBUSY);
    assert(__WritesSince(mark) == 0);
    assert(__Stop(&request) == OS_EOK);
    __Run();
    __ExpectResult(OS_ECANCELLED, 0);

    // A full reset restores the status enable mask and keeps signals off.
    mark = g_eventCount;
    assert(g_controller.Host.Operations->Reset(&g_controller.Host) == OS_EOK);
    reset = __Find(EventWrite, SDHCI_SOFTWARE_RESET, SDHCI_RESET_ALL, mark);
    assert(reset >= 0);
    assert(__Find(EventWrite, SDHCI_INTERRUPT_ENABLE, SDHCI_INT_HANDLED, (uint32_t)reset) > reset);
    assert(__Find(EventWrite, SDHCI_INTERRUPT_SIGNAL, 0, (uint32_t)reset) > reset);
    assert(__LoadRegister(SDHCI_INTERRUPT_ENABLE, 4) == SDHCI_INT_HANDLED);
    assert(__LoadRegister(SDHCI_INTERRUPT_SIGNAL, 4) == 0);
    assert(g_controller.Host.Operations->SetBus(&g_controller.Host, &settings, &clockHz) == OS_EOK);
    __Teardown();
    printf("SDHCI busy controller passed\n");
}

static void
__TestInterruptSetup(void)
{
    enum InterruptCall call;
    SdRequest_t        request;
    int                memory;
    int                enable;
    int                enableWrite;
    int                quiesce;
    int                destroy;

    // Order: reserve, describe, then the status bits, then delivery.
    __PrepareOnly(__DefaultCapabilities());
    assert(SdhciInterruptSetup(&g_controller, EVENT_DESCRIPTOR) == OS_EOK);
    memory = -1;
    for (call = InterruptCallInitialize; call <= InterruptCallMemory; call++) {
        assert(__FindInterruptCall(call) > memory);
        memory = __FindInterruptCall(call);
    }
    enable = __FindInterruptCall(InterruptCallEnable);
    enableWrite = __Find(EventWrite, SDHCI_INTERRUPT_ENABLE, SDHCI_INT_HANDLED, 0);
    assert(enableWrite > memory && enable > enableWrite);
    assert(__Find(EventWrite, SDHCI_INTERRUPT_STATUS, SDHCI_INT_HANDLED, (uint32_t)enableWrite) < enable);
    assert(g_handlerResources.Handler == SdhciFastInterrupt);
    assert(g_handlerResources.HandleResource == EVENT_DESCRIPTOR);
    assert(g_handlerResources.IoResources[0] == &g_controller.Registers);
    assert(g_handlerResources.MemoryResources[0].Address == (uintptr_t)&g_controller.Interrupt.Shared);
    assert(g_handlerResources.MemoryResources[0].Length == sizeof(SdhciInterruptShared_t));
    assert(g_controller.Interrupts.StatusEnable == SDHCI_INT_HANDLED);
    assert(g_controller.Interrupts.SignalEnable == 0);
    assert(SdhciInterruptSetup(&g_controller, EVENT_DESCRIPTOR) == OS_EINVALPARAMS);

    // Teardown: the quiesce reset, then the interrupt, then the DMA memory.
    assert(SdhciControllerStart(&g_controller) == OS_EOK);
    __Teardown();
    quiesce = __Find(EventWrite, SDHCI_SOFTWARE_RESET, SDHCI_RESET_ALL, (uint32_t)enable);
    destroy = __FindInterruptCall(InterruptCallDestroy);
    assert(quiesce > enable && destroy > quiesce);
    assert(__Find(EventDmaFree, 0, ANY_VALUE, 0) > destroy);

    // Not between prepare and start.
    __PrepareOnly(__DefaultCapabilities());
    assert(SdhciControllerStart(&g_controller) == OS_EOK);
    assert(SdhciInterruptSetup(&g_controller, EVENT_DESCRIPTOR) == OS_EINVALPARAMS);
    __Teardown();
    assert(g_interruptDestroys == 0);

    // Each failing step stops the setup, leaves the request engine closed,
    // and teardown still releases the object.
    for (call = InterruptCallAllocate; call <= InterruptCallEnable; call++) {
        __PrepareOnly(__DefaultCapabilities());
        g_failInterruptCall = call;
        assert(SdhciInterruptSetup(&g_controller, EVENT_DESCRIPTOR) == OS_EDEVFAULT);
        assert(!g_controller.Interrupt.Enabled);
        request = __Command(13, 0, SdResponseR1);
        __ExpectRejected(&request, OS_EINVALPARAMS);
        g_failInterruptCall = InterruptCallNone;
        __Teardown();
        assert(g_interruptDestroys == 1);
    }

    // A failed destroy keeps the DMA memory for a retry.
    __PrepareOnly(__DefaultCapabilities());
    assert(SdhciInterruptSetup(&g_controller, EVENT_DESCRIPTOR) == OS_EOK);
    g_failInterruptCall = InterruptCallDestroy;
    assert(SdhciControllerDestroy(&g_controller) == OS_EDEVFAULT);
    assert(g_liveBuffers == 2 && (g_controller.Stages & SDHCI_STAGE_INTERRUPT) != 0);
    g_failInterruptCall = InterruptCallNone;
    __Teardown();
    printf("SDHCI interrupt setup passed\n");
}

int
main(void)
{
    __TestSubmitChecks();
    __TestProgrammingOrder();
    __TestCommandEncoding();
    __TestCompletion();
    __TestResponses();
    __TestAutoCmd12();
    __TestExplicitStop();
    __TestErrorMapping();
    __TestRateLimit();
    __TestStopProof();
    __TestDeadlines();
    __TestStopRequest();
    __TestStaleStatus();
    __TestFastHandler();
    __TestPolling();
    __TestScheduledWork();
    __TestWorkerLoop();
    __TestBusyController();
    __TestInterruptSetup();
    printf("SDHCI request tests passed\n");
    return 0;
}
