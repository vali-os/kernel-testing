// The early bootstrap job must survive before constructor registration provides
// primary TLS callbacks. Ordinary jobs must still invoke registered callbacks.
#include <stdio.h>
#include <stdlib.h>
// Keep the host C++/sanitizer exit hooks separate from the Vali fixture.
#define __cxa_atexit __TestAtExit
#define __cxa_at_quick_exit __TestAtQuickExit
#define __cxa_thread_atexit_impl __TestThreadAtExit
#include "../librt/libc/os/clang.c"
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); abort(); } } while (0)
static unsigned int initialized;
static unsigned int finalized;
static unsigned int released;
static void __Initialize(void) { initialized++; }
static void __Finalize(void) { finalized++; }
int __tls_prepare_modules(void) { return 0; }
void __tls_run_destructors(void* module) {}
void __tls_release_modules(void) { released++; }
void tss_cleanup(void) {}
void _fpreset(void) {}
void __cxa_at_exit_run(thrd_t thread, void* module, int code) {}
int __at_exit_impl(thrd_t thread, void (*callback)(void*), void* argument, void* module) { return 0; }
int __at_quick_exit_impl(thrd_t thread, void (*callback)(void*), void* argument, void* module) { return 0; }
int __tls_atexit(void (*callback)(void*), void* argument, void* module) { return 0; }
int main(void)
{
    __cxa_threadinitialize();
    __cxa_threadfinalize();
    CHECK(initialized == 0 && finalized == 0 && released == 1);
    __cxa_primary_tls_thread_init = __Initialize;
    __cxa_primary_tls_thread_finit = __Finalize;
    __cxa_threadinitialize();
    __cxa_threadfinalize();
    CHECK(initialized == 1 && finalized == 1 && released == 2);
    puts("Bootstrap TLS: unset callbacks skipped; registered callbacks invoked");
    return 0;
}
