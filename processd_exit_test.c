// Exercise how processd ends a process: PmTerminateProcess, the exit watcher
// (PmRemoveExitedProcesses), joins, signals, lookups and the crash report
// request with the real debugger. The fixtures of the startup server test are
// reused, so process.c and requests.c are the production sources.
//
// exit() reports the exit code to processd before it runs the exit handlers.
// processd used to remove the process at that report, so a handler that
// crashed afterwards got OS_ENOENT for its crash report and nothing was
// printed. The process now stays TERMINATING until its primary thread is gone,
// or until PROCESS_EXIT_TIMEOUT_MS has passed.
#define main __StartupServerTestMain
#include "process_startup_server_test.c"
#undef main

#include <os/context.h>
#include <signal.h>

#define CRASH_MODULE      "crashing.dll"
#define CRASH_MODULE_BASE 0x400000
#define CRASH_ADDRESS     0x401234

static oserr_t      g_crashResponse;
static unsigned int g_crashResponses;
static unsigned int g_signals;
static unsigned int g_crashLines;
static unsigned int g_watcherWarnings;
static unsigned int g_lookupWarnings;
static uintptr_t    g_stack[4];

static void
__CountLogLines(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        line)
{
    if (strstr(line, "Crashed in module " CRASH_MODULE ", at offset 0x1234") != NULL) {
        g_crashLines++;
    }
    if (level == OSSYSLOGLEVEL_WARNING && strstr(line, "still runs") != NULL) {
        g_watcherWarnings++;
    }
    if (level == OSSYSLOGLEVEL_WARNING && strstr(line, "failed to look up thread") != NULL) {
        g_lookupWarnings++;
    }
}

int
sys_process_report_crash_response(
    _In_ struct gracht_message* message,
    _In_ oserr_t                result)
{
    g_crashResponse = result;
    g_crashResponses++;
    return 0;
}

oserr_t
Syscall_ThreadSignal(
    _In_ uuid_t thread,
    _In_ int    signal)
{
    assert(thread == 78 + 11);
    g_signals++;
    return OS_EOK;
}

// The debugger's view of the crashed process: one module, and a stack that
// holds one address inside it.
void
SymbolInitialize(void)
{
}

oserr_t
SymbolLookup(
    _In_  mstring_t*   moduleName,
    _In_  uintptr_t    binaryOffset,
    _Out_ const char** symbolName,
    _Out_ uintptr_t*   symbolOffset)
{
    return OS_ENOENT;
}

oserr_t
PEImageLoadContextImageDetailsByAddress(
    _In_  struct PEImageLoadContext* loadContext,
    _In_  uintptr_t                  mappedAddress,
    _Out_ uintptr_t*                 moduleBaseOut,
    _Out_ mstring_t**                moduleNameOut)
{
    assert(loadContext != NULL);
    if (mappedAddress < CRASH_MODULE_BASE || mappedAddress >= CRASH_MODULE_BASE + 0x10000) {
        return OS_ENOENT;
    }
    *moduleBaseOut = CRASH_MODULE_BASE;
    *moduleNameOut = (mstring_t*)CRASH_MODULE;
    return OS_EOK;
}

oserr_t
DebugMapThreadMemory(
    _In_  uuid_t    thread,
    _In_  uintptr_t address,
    _Out_ void**    topOfStack,
    _Out_ void**    pointer)
{
    assert(thread == 88);
    g_stack[1] = CRASH_ADDRESS + 0x10;
    *pointer = &g_stack[0];
    *topOfStack = &g_stack[4];
    return OS_EOK;
}

oserr_t
MemoryFree(
    _In_ void*  pointer,
    _In_ size_t length)
{
    assert(pointer == &g_stack[0] && length == sizeof(g_stack));
    return OS_EOK;
}

// Lists a process with handle id, primary thread id + 11 (88 for process 77)
// and the name "test", the way __CreateProcess does after the primary thread
// started. The test's mstrings are plain C strings.
static Process_t*
__AddProcess(
    _In_ uuid_t id)
{
    Process_t* process = calloc(1, sizeof(*process));

    assert(process != NULL);
    process->handle.ID = id;
    process->primary_thread_id = id + 11;
    process->references = 1;
    process->state = ProcessState_RUNNING;
    process->name = (mstring_t*)strdup("test");
    process->working_directory = (mstring_t*)strdup("/");
    process->assembly_directory = (mstring_t*)strdup("/");
    process->load_context = calloc(1, sizeof(*process->load_context));
    process->load_context->RootModule = (mstring_t*)"test";
    hashtable_set(&g_processes, &(struct process_entry) { id, process });
    hashtable_set(&g_threadmappings, &(struct thread_mapping) { id, id + 11 });
    return process;
}

static void
__ReportCrash(
    _In_ uuid_t id)
{
    Context_t context = { 0 };

    CONTEXT_IP(&context) = CRASH_ADDRESS;
    CONTEXT_USERSP(&context) = (uintptr_t)&g_stack[0];
    g_crashResponse = OS_EUNKNOWN;
    sys_process_report_crash_invocation(NULL, 88, id, (const uint8_t*)&context, sizeof(context), SIGSEGV);
}

// Removes every TERMINATING process, the way the watcher does once the
// threads are gone.
static void
__ThreadsGone(void)
{
    g_threadLookupStatus = OS_ENOENT;
    assert(PmRemoveExitedProcesses() == 0);
    g_threadLookupStatus = OS_EOK;
}

// A process that crashes in an exit handler, after exit() reported its code.
static void
__TestCrashAfterTerminate(void)
{
    Process_t*   process = __AddProcess(77);
    unsigned int deleted = g_deleted;
    unsigned int notifies = g_conditionNotifies;
    unsigned int jobs = g_watcherJobs;
    int          exitCode = -1;

    assert(PmTerminateProcess(77, 5) == OS_EOK);
    assert(process->state == ProcessState_TERMINATING && process->exit_code == 5);
    assert(g_conditionNotifies == notifies + 1 && g_watcherJobs == jobs + 1);

    // Joiners get the exit code at once, as before.
    assert(PmJoinProcess(77, 1, &exitCode) == OS_EOK && exitCode == 5);

    // The crash report finds the process and the debugger prints the module.
    __ReportCrash(77);
    assert(g_crashResponses == 1 && g_crashResponse == OS_EOK && g_crashLines == 1);
    assert(process->references == 1 && g_deleted == deleted);

    // _Exit reports again after the crash. The first exit code stays, for a
    // join now and for one after the record is gone.
    assert(PmTerminateProcess(77, EXIT_FAILURE) == OS_EOK);
    assert(process->exit_code == 5 && g_conditionNotifies == notifies + 1);
    assert(PmJoinProcess(77, 1, &exitCode) == OS_EOK && exitCode == 5);

    // The thread is still there, so nothing is removed.
    assert(PmRemoveExitedProcesses() == 1 && g_deleted == deleted);
    __ThreadsGone();
    assert(g_deleted == deleted + 1 && PmGetProcessByHandle(77) == NULL);
    assert(PmJoinProcess(77, 1, &exitCode) == OS_EOK && exitCode == 5);

    // Once the record is gone, a late crash report and a late terminate are
    // refused, and neither changes the history.
    __ReportCrash(77);
    assert(g_crashResponse == OS_ENOENT && g_crashLines == 1);
    assert(PmTerminateProcess(77, 9) == OS_ENOENT);
    assert(PmJoinProcess(77, 1, &exitCode) == OS_EOK && exitCode == 5);
}

// Every other lookup of a TERMINATING process.
static void
__TestLookups(void)
{
    Process_t*   process = __AddProcess(78);
    Process_t*   referenced;
    mstring_t*   name;
    mstring_t*   directory;
    UInteger64_t tick;
    unsigned int signals = g_signals;

    // A RUNNING process gets the signal.
    assert(PmSignalProcess(1, 78, SIGTERM) == OS_EOK && g_signals == signals + 1);
    assert(PmTerminateProcess(78, 0) == OS_EOK);

    // A kill of a TERMINATING process is refused with OS_EINPROGRESS, and the
    // thread gets no signal: it already reported its exit code.
    assert(PmSignalProcess(1, 78, SIGKILL) == OS_EINPROGRESS && g_signals == signals + 1);

    // Name, directory and tick queries, and the startup lookup by thread, keep
    // working while the exit handlers run.
    assert(PmGetName(78, &name) == OS_EOK && strcmp((char*)name, "test") == 0);
    assert(PmGetWorkingDirectory(78, &directory) == OS_EOK);
    assert(PmGetAssemblyDirectory(78, &directory) == OS_EOK);
    assert(PmGetTickBase(78, &tick) == OS_EOK);
    assert(PmReferenceProcessByThread(78 + 11, &referenced) == OS_EOK && referenced == process);
    PmReleaseProcess(referenced);
    assert(PmReferenceProcessByHandle(78, &referenced) == OS_EOK && referenced == process);

    // A reference held over the removal keeps the record, which is EXITED and
    // no longer listed. The last release frees it.
    __ThreadsGone();
    assert(process->state == ProcessState_EXITED && process->references == 1);
    assert(PmGetProcessByHandle(78) == NULL && PmGetName(78, &name) == OS_ENOENT);
    assert(PmSignalProcess(1, 78, SIGKILL) == OS_ENOENT);
    PmReleaseProcess(referenced);
}

// The watcher job: one per batch of TERMINATING processes, the deadline, a
// failing lookup, a failed queue and more processes than one pass removes.
static void
__TestWatcher(void)
{
    struct timespec now;
    Process_t*      process;
    unsigned int    jobs;
    unsigned int    deleted;
    int             i;

    // Two TERMINATING processes share one watcher job. The job runs until the
    // last one is gone, and the next terminate queues a new one.
    __AddProcess(80);
    __AddProcess(81);
    jobs = g_watcherJobs;
    assert(PmTerminateProcess(80, 0) == OS_EOK && g_watcherJobs == jobs + 1);
    assert(PmTerminateProcess(81, 0) == OS_EOK && g_watcherJobs == jobs + 1);

    // A pass that leaves processes keeps the watcher queued, so a third
    // terminate does not queue another job.
    assert(PmRemoveExitedProcesses() == 2);
    __AddProcess(86);
    assert(PmTerminateProcess(86, 0) == OS_EOK && g_watcherJobs == jobs + 1);
    g_threadLookupStatus = OS_ENOENT;
    g_watcherEntry(NULL, NULL);
    g_threadLookupStatus = OS_EOK;
    assert(PmGetProcessByHandle(80) == NULL && PmGetProcessByHandle(81) == NULL);
    assert(PmGetProcessByHandle(86) == NULL);
    process = __AddProcess(82);
    assert(PmTerminateProcess(82, 0) == OS_EOK && g_watcherJobs == jobs + 2);

    // The thread never ends. The deadline is PROCESS_EXIT_TIMEOUT_MS after the
    // terminate, and the record is removed with a warning once it has passed.
    timespec_get(&now, TIME_UTC);
    assert(process->exit_deadline.tv_sec >= now.tv_sec + PROCESS_EXIT_TIMEOUT_MS / 1000 - 1);
    assert(process->exit_deadline.tv_sec <= now.tv_sec + PROCESS_EXIT_TIMEOUT_MS / 1000 + 1);
    deleted = g_deleted;
    assert(PmRemoveExitedProcesses() == 1 && g_deleted == deleted && g_watcherWarnings == 0);
    process->exit_deadline = (struct timespec) { 0 };
    assert(PmRemoveExitedProcesses() == 0 && g_deleted == deleted + 1 && g_watcherWarnings == 1);

    // A lookup that fails for another reason than OS_ENOENT means processd
    // cannot tell. The process counts as gone, with a warning.
    __AddProcess(83);
    assert(PmTerminateProcess(83, 0) == OS_EOK);
    g_threadLookupStatus = OS_EPERMISSIONS;
    assert(PmRemoveExitedProcesses() == 0 && g_deleted == deleted + 2 && g_lookupWarnings == 1);
    g_threadLookupStatus = OS_EOK;

    // A watcher that cannot be queued is tried again by the next terminate.
    jobs = g_watcherJobs;
    __AddProcess(84);
    __AddProcess(85);
    g_watcherQueueFails = true;
    assert(PmTerminateProcess(84, 0) == OS_EOK && g_watcherJobs == jobs);
    g_watcherQueueFails = false;
    assert(PmTerminateProcess(85, 0) == OS_EOK && g_watcherJobs == jobs + 1);
    __ThreadsGone();

    // More processes than one pass removes: the rest stay counted, so the
    // watcher keeps running for them.
    for (i = 0; i < __EXIT_BATCH_SIZE + 4; i++) {
        __AddProcess(100 + i);
        assert(PmTerminateProcess(100 + i, 0) == OS_EOK);
    }
    deleted = g_deleted;
    g_threadLookupStatus = OS_ENOENT;
    assert(PmRemoveExitedProcesses() == 4 && g_deleted == deleted + __EXIT_BATCH_SIZE);
    assert(PmRemoveExitedProcesses() == 0 && g_deleted == deleted + __EXIT_BATCH_SIZE + 4);
    g_threadLookupStatus = OS_EOK;
}

int
main(void)
{
    PmInitialize();
    g_logHook = __CountLogLines;
    __TestCrashAfterTerminate();
    __TestLookups();
    __TestWatcher();
    assert(g_processes.element_count == 0);
    hashtable_destroy(&g_threadmappings);
    hashtable_destroy(&g_processes);
    hashtable_destroy(&g_processHistory);
    puts("processd exit: crash report after terminate, joins, signals, lookups and the exit watcher passed");
    return 0;
}
