// served's setup job when /data fails under it. The job must end without
// ending the process, and a later run, queued when filed reports /data again,
// must do the whole setup.
//
// Production code compiled here: services/served/server/setup.c. The runner
// builds it with exit renamed to __TestExit, so a call to exit is recorded
// instead of ending the test. Doubles replace the steps the job runs
// (ServerEnsurePaths, StateLoad, InstallBundledApplications and ServerLoad
// from server/load.c, server/state.c and server/install.c). Each step can be
// set to fail, and each call is counted.
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ddk/utils.h>
#include <served/install.h>
#include <served/server.h>
#include <served/setup.h>
#include <served/state.h>

static int g_errors;
static int g_exits;

void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format,
    ...)
{
    if (level == OSSYSLOGLEVEL_ERROR) {
        g_errors++;
    }
}

void
__TestExit(
    _In_ int exitCode)
{
    g_exits++;
}

static oserr_t g_pathsResult;
static oserr_t g_stateResult;
static oserr_t g_loadResult;
static int     g_pathsCalls;
static int     g_stateCalls;
static int     g_loadCalls;
static int     g_installs;
static int     g_lockDepth;
static struct State g_state;

oserr_t ServerEnsurePaths(void) { g_pathsCalls++; return g_pathsResult; }
oserr_t StateLoad(void) { g_stateCalls++; g_state.FirstBoot = true; return g_stateResult; }
oserr_t ServerLoad(void) { g_loadCalls++; return g_loadResult; }
struct State* State(void) { return &g_state; }
void StateLock(void) { assert(g_lockDepth++ == 0); }
void StateUnlock(void) { assert(--g_lockDepth == 0); }
void InstallBundledApplications(void) { assert(g_lockDepth == 1); g_installs++; }

// Runs the job once with the given results, the way usched runs it after
// sys_file_event_storage_ready_invocation queued it for /data.
static void
__RunSetup(
    _In_ oserr_t pathsResult,
    _In_ oserr_t stateResult,
    _In_ oserr_t loadResult)
{
    g_pathsResult = pathsResult;
    g_stateResult = stateResult;
    g_loadResult = loadResult;
    g_pathsCalls = g_stateCalls = g_loadCalls = g_installs = g_errors = 0;
    served_server_setup_job(NULL, NULL);
    assert(g_exits == 0 && g_lockDepth == 0);
}

int
main(void)
{
    // /data went away while the paths were created. Nothing after that step
    // runs, and the failure is logged.
    __RunSetup(OS_ENOENT, OS_EOK, OS_EOK);
    assert(g_pathsCalls == 1 && g_stateCalls == 0 && g_loadCalls == 0 && g_errors == 1);

    // The state could not be read.
    __RunSetup(OS_EOK, OS_EUNKNOWN, OS_EOK);
    assert(g_stateCalls == 1 && g_installs == 0 && g_loadCalls == 0 && g_errors == 1);

    // The applications could not be loaded.
    __RunSetup(OS_EOK, OS_EOK, OS_ENOENT);
    assert(g_installs == 1 && g_loadCalls == 1 && g_errors == 1);

    // /data is back, and the setup runs to the end.
    __RunSetup(OS_EOK, OS_EOK, OS_EOK);
    assert(g_pathsCalls == 1 && g_stateCalls == 1 && g_installs == 1 && g_loadCalls == 1);
    assert(g_errors == 0 && !g_state.FirstBoot);
    puts("served setup without /data passed");
    return 0;
}
