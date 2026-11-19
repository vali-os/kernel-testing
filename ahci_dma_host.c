// Protocol plumbing outside the DMA fixture. Unexpected use aborts the test.
#include <ioctl.h>
#include <stdlib.h>
void*
__crt_get_module_server(void)
{
    return NULL;
}
int
gracht_server_get_aio_handle(void)
{
    return 0;
}
void
gracht_server_register_protocol(void)
{
    abort();
}
void
gracht_server_defer_message(void)
{
}
// The event counter is always reported as 0. Reads abort, so a test sees at
// once when a driver reads an empty counter.
int
__TestIoctl(
    int           descriptor,
    unsigned long request,
    int*          value)
{
    (void)descriptor;
    if (request == FIONREAD) {
        *value = 0;
    }
    return 0;
}
int
__TestRead(void)
{
    abort();
}
const char ctt_driver_server_protocol[256] = {0};
const char ctt_storage_server_protocol[256] = {0};
