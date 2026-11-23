// filed's requests to filesystem drivers through the real gracht client, when
// the driver answers with an error message. __GrachtAwaitCompleted
// (internal/_gracht.h) then abandons the request. gracht_client_await used to
// leave the awaiter's id in the request when the wait ended, so the abandon
// failed with EBUSY and every error answer left one request in the client for
// ever.
//
// Production code compiled here: interfaces/driver_ops.c, the generated
// ctt_filesystem client, __GrachtAwaitCompleted and the libgracht client
// runtime. A link double takes the place of the IPC link to the driver. The
// error answer arrives in one of two ways:
// - before the wait: the link's send delivers it at once, so the await finds
//   the request done and returns without waiting.
// - during the wait: the test replaces the cnd_wait of the awaiter and
//   delivers the answer there, the way the thread that reads the link would.
//   While the wait lasts, the request must not be abandoned.
// Each operation must return OS_EUNKNOWN, and afterwards the client must hold
// no request. No failed abandon may be logged.
//
// Built with -DGRACHT_ABANDON_REFUSED against the libgracht client from before
// the fix, the test checks the other side: each request stays behind, and
// __GrachtAwaitCompleted logs the failed abandon once per request.
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The client runtime is included, so the test can replace the condition wait
// of the awaiter and read the client's table of requests.
// GRACHT_CLIENT_SOURCE names another copy of client.c, for example the one
// from before the fix.
#ifndef GRACHT_CLIENT_SOURCE
#define GRACHT_CLIENT_SOURCE "../librt/libgracht/runtime/client.c"
#endif
#include "thread_api.h"
static int __TestWait(cnd_t* condition, mtx_t* mutex);
#undef cnd_wait
#define cnd_wait __TestWait
#include GRACHT_CLIENT_SOURCE
#undef cnd_wait
#define cnd_wait pthread_cond_wait

#include <ddk/utils.h>
#include <vfs/interface.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

enum __AnswerTime {
    ANSWER_BEFORE_WAIT,
    ANSWER_DURING_WAIT
};

static const char* g_answerTimeNames[] = { "before the wait", "during the wait" };

extern struct VFSOperations g_driverOps;

static gracht_client_t*   g_client;
static struct gracht_link g_link;
static enum __AnswerTime  g_answerTime;
static uint32_t           g_lastMessageId;
static int                g_sends;
static int                g_waits;
static int                g_errors;
static int                g_abandonErrors;

void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format,
    ...)
{
    if (level != OSSYSLOGLEVEL_ERROR) {
        return;
    }

    g_errors++;
    if (strstr(format, "abandon") != NULL) {
        g_abandonErrors++;
    }
    if (getenv("FILED_TEST_VERBOSE") != NULL) {
        va_list arguments;

        va_start(arguments, format);
        vfprintf(stderr, format, arguments);
        va_end(arguments);
        fprintf(stderr, "\n");
    }
}

gracht_client_t*
GetGrachtClient(void)
{
    return g_client;
}

// control.c also holds the server side of the control protocol, which needs
// these from the server runtime. The client never calls them.
void
gracht_control_subscribe_invocation(
    _In_ const struct gracht_message* message,
    _In_ const uint16_t               protocol)
{
    (void)message;
    (void)protocol;
    CHECK(!"gracht_control_subscribe_invocation called");
}

void
gracht_control_unsubscribe_invocation(
    _In_ const struct gracht_message* message,
    _In_ const uint16_t               protocol)
{
    (void)message;
    (void)protocol;
    CHECK(!"gracht_control_unsubscribe_invocation called");
}

// The driver gets the request and answers with an error message, as gracht
// does when it cannot run a call. Before the wait, the answer is delivered
// here. The client has already stored the request when send runs.
static int
__LinkSend(
    _In_ struct gracht_link*  link,
    _In_ struct gracht_buffer* message,
    _In_ void*                 messageContext)
{
    struct gracht_message_context* context = messageContext;
    (void)link;
    (void)message;

    CHECK(context != NULL);
    g_lastMessageId = context->message_id;
    g_sends++;
    if (g_answerTime == ANSWER_BEFORE_WAIT) {
        gracht_control_error_invocation(g_client, context->message_id, EIO);
    }
    return 0;
}

// The awaiter waits for the answer of the last request. The request cannot be
// abandoned while it waits. Then the answer is delivered with the awaiter's
// mutex released, as another thread would do it.
static int
__TestWait(
    _In_ cnd_t* condition,
    _In_ mtx_t* mutex)
{
    struct gracht_message_context context = { .message_id = g_lastMessageId };
    (void)condition;

    CHECK(g_answerTime == ANSWER_DURING_WAIT);
    g_waits++;
    pthread_mutex_unlock(mutex);
    errno = 0;
    CHECK(gracht_client_abandon(g_client, &context) == -1);
    CHECK(errno == EBUSY);
    gracht_control_error_invocation(g_client, g_lastMessageId, EIO);
    pthread_mutex_lock(mutex);
    return 0;
}

static void
__CreateClient(void)
{
    gracht_client_configuration_t config;

    memset(&g_link, 0, sizeof(g_link));
    g_link.type = gracht_link_packet_based;
    g_link.ops.client.send = __LinkSend;
    gracht_client_configuration_init(&config);
    gracht_client_configuration_set_link(&config, &g_link);
    CHECK(gracht_client_create(&config, &g_client) == 0);
}

// Runs the operations of driver_ops.c that take no path or name. Each sends
// one request, gets an error answer and must return OS_EUNKNOWN.
static int
__RunOperations(
    _In_ struct VFSInterface* interface)
{
    struct VFSStatFS stats;
    uint64_t         position = 0x5a5a;

    CHECK(g_driverOps.Stat(interface, (void*)1, &stats) == OS_EUNKNOWN);
    CHECK(g_driverOps.Close(interface, (void*)1, (void*)2) == OS_EUNKNOWN);
    CHECK(g_driverOps.Truncate(interface, (void*)1, (void*)2, 512) == OS_EUNKNOWN);
    CHECK(g_driverOps.Seek(interface, (void*)1, (void*)2, 512, &position) == OS_EUNKNOWN);
    CHECK(position == 0x5a5a);
    CHECK(g_driverOps.Destroy(interface, (void*)1, 0) == OS_EUNKNOWN);
    return 5;
}

int
main(void)
{
    struct VFSInterface interface;
    int                 requests = 0;
    int                 answerTime;

    memset(&interface, 0, sizeof(interface));
    interface.DriverID = 7;
    __CreateClient();

    for (answerTime = ANSWER_BEFORE_WAIT; answerTime <= ANSWER_DURING_WAIT; answerTime++) {
        int waitsBefore = g_waits;
        int count;

        g_answerTime = (enum __AnswerTime)answerTime;
        count = __RunOperations(&interface);
        requests += count;
        CHECK(g_sends == requests);

        // Only an answer during the wait makes the awaiter wait.
        if (g_answerTime == ANSWER_DURING_WAIT) {
            CHECK(g_waits - waitsBefore == count);
        } else {
            CHECK(g_waits == waitsBefore);
        }

#ifdef GRACHT_ABANDON_REFUSED
        // The old client refuses every abandon, and each refusal is logged.
        CHECK(g_client->messages.element_count == (size_t)requests);
        CHECK(g_abandonErrors == requests);
#else
        CHECK(g_client->messages.element_count == 0);
        CHECK(g_abandonErrors == 0);
#endif
        // driver_ops.c logs one error for each request without an answer.
        CHECK(g_errors - g_abandonErrors == requests);
        printf("error answers %s: %d requests, %zu left in the client\n",
               g_answerTimeNames[answerTime], count, g_client->messages.element_count);
    }

    gracht_client_shutdown(g_client);
    printf("filed driver requests through gracht passed\n");
    return 0;
}
