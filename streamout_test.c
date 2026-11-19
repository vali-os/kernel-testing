// Exercise the shared printf engine (librt/libc/stdio/stream/streamout.c)
// through its vsnprintf front end. The kernel log (LogAppendMessage) and the
// userspace log path (libddk SystemDebug) both format with this code, built
// once with LIBC_KERNEL for the kernel and once without for libc. The runner
// compiles this file in both modes and on 64-bit and, when the host can,
// 32-bit targets.
//
// The test checks the z, t and j length modifiers on every integer
// conversion. A missing modifier makes the formatter drop the '%' and print
// the rest of the conversion as text without reading the argument. Every
// argument after it is then read from the wrong slot, so each case prints a
// marker value after the sized value and checks that it is still right.
#ifndef STREAMOUT_SOURCE
#define STREAMOUT_SOURCE "../librt/libc/stdio/stream/streamout.c"
#endif

// Rename the engine and its front end so they do not clash with the host
// C library that the test links against.
#define streamout ValiStreamout
#define _sxprintf ValiVsnprintf
#define USE_COUNT 1
#define USE_VARARGS 1
#define USE_VIRTUAL 1

#include STREAMOUT_SOURCE
#include "../librt/libc/stdio/stream/_sxprintf.c"
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

// Marker printed after each sized value. If the sized argument is skipped or
// read with the wrong width, the marker comes out wrong or missing.
#define MARKER 0x5A17u

#ifdef LIBC_KERNEL
// The kernel engine writes to the screen when the stream has no buffer. The
// test only formats into buffers, so this must never be called.
oserr_t
VideoPutCharacter(
    _In_ int character)
{
    (void)character;
    printf("VideoPutCharacter called for a buffer stream\n");
    exit(1);
}
#else
// The libc engine's float helper uses the Vali libm classifiers, which the
// host does not have. No case formats a float.
int __isfinitef(float value) { return __builtin_isfinite(value); }
int __isfinite(double value) { return __builtin_isfinite(value); }
int __isfinitel(long double value) { return __builtin_isfinite(value); }
#endif

// The engine refers to the mstring helpers for %ms. No case uses them.
int
mstr_fromchar(
    _In_ mchar_t character,
    _In_ char*   u8,
    _In_ size_t* length)
{
    (void)character;
    (void)u8;
    (void)length;
    return -1;
}

void
usched_mtx_init(
    _In_ struct usched_mtx* mutex,
    _In_ int                type)
{
    (void)mutex;
    (void)type;
}

// Formats into a fixed buffer and compares against the expected text.
static void
__Expect(
    _In_ int         line,
    _In_ const char* expected,
    _In_ const char* format, ...)
{
    char    buffer[128];
    va_list arguments;
    int     written;

    memset(&buffer[0], 0, sizeof(buffer));
    va_start(arguments, format);
    written = ValiVsnprintf(&buffer[0], sizeof(buffer) - 1, format, arguments);
    va_end(arguments);
    if (written < 0 || strcmp(&buffer[0], expected) != 0 ||
        (size_t)written != strlen(expected)) {
        printf("line %d: format \"%s\" gave \"%s\" (%d), expected \"%s\"\n",
               line, format, &buffer[0], written, expected);
        exit(1);
    }
}

#define EXPECT(expected, ...) __Expect(__LINE__, expected, __VA_ARGS__)

int
main(void)
{
    // Small values work on every target width.
    EXPECT("512 23063", "%zu %u", (size_t)512, MARKER);
    EXPECT("-7 23063", "%zd %u", (intptr_t)-7, MARKER);
    EXPECT("-7 23063", "%zi %u", (intptr_t)-7, MARKER);
    EXPECT("1ff 23063", "%zx %u", (size_t)0x1FF, MARKER);
    EXPECT("1FF 23063", "%zX %u", (size_t)0x1FF, MARKER);
    EXPECT("17 23063", "%zo %u", (size_t)15, MARKER);
    EXPECT("-9 23063", "%td %u", (ptrdiff_t)-9, MARKER);
    EXPECT("9 23063", "%ti %u", (ptrdiff_t)9, MARKER);
    EXPECT("ff 23063", "%tx %u", (ptrdiff_t)0xFF, MARKER);
    EXPECT("9 23063", "%tu %u", (ptrdiff_t)9, MARKER);
    EXPECT("11 23063", "%to %u", (ptrdiff_t)9, MARKER);
    EXPECT("FF 23063", "%tX %u", (ptrdiff_t)0xFF, MARKER);

    // intmax_t is 64 bits wide on every Vali target.
    EXPECT("-81985529216486895 23063", "%jd %u", (intmax_t)-0x123456789ABCDEFll, MARKER);
    EXPECT("-1 23063", "%ji %u", (intmax_t)-1, MARKER);
    EXPECT("18446744073709551615 23063", "%ju %u", (uintmax_t)UINTMAX_MAX, MARKER);
    EXPECT("123456789abcdef 23063", "%jx %u", (uintmax_t)0x123456789ABCDEFull, MARKER);
    EXPECT("123456789ABCDEF 23063", "%jX %u", (uintmax_t)0x123456789ABCDEFull, MARKER);
    EXPECT("1777777777777777777777 23063", "%jo %u", (uintmax_t)UINTMAX_MAX, MARKER);

    // Field width, padding and flags still apply to sized values.
    EXPECT("00000200 23063", "%08zx %u", (size_t)0x200, MARKER);
    EXPECT("  -5|23063", "%4zd|%u", (intptr_t)-5, MARKER);
    EXPECT("0x10 23063", "%#zx %u", (size_t)16, MARKER);

    // Several sized values in a row, as in the DMA log lines.
    EXPECT("context 235, offset 4096, length 512, direction 0 failed: 512",
           "context %u, offset %zu, length %zu, direction %u failed: %u",
           235u, (size_t)4096, (size_t)512, 0u, 512u);

#if SIZE_MAX > 0xFFFFFFFFu
    // On 64-bit targets size_t and ptrdiff_t hold values above 32 bits. The
    // upper half must be printed, and must not be read as the next argument.
    EXPECT("4886718345 23063", "%zu %u", (size_t)0x123456789ull, MARKER);
    EXPECT("18446744073709551615 23063", "%zu %u", (size_t)SIZE_MAX, MARKER);
    EXPECT("123456789 23063", "%zx %u", (size_t)0x123456789ull, MARKER);
    EXPECT("-4886718345 23063", "%zd %u", (intptr_t)-0x123456789ll, MARKER);
    EXPECT("-4886718345 23063", "%td %u", (ptrdiff_t)-0x123456789ll, MARKER);
    EXPECT("range 4886718345+8589934592 failed (5)",
           "range %zu+%zu failed (%d)",
           (size_t)0x123456789ull, (size_t)0x200000000ull, 5);
#else
    // On 32-bit targets size_t is 32 bits, and the full range still prints.
    EXPECT("4294967295 23063", "%zu %u", (size_t)SIZE_MAX, MARKER);
    EXPECT("-2147483648 23063", "%td %u", (ptrdiff_t)PTRDIFF_MIN, MARKER);
#endif

    // Existing modifiers keep working.
    EXPECT("4886718345 23063", "%llu %u", 0x123456789ull, MARKER);
    EXPECT("-4886718345 23063", "%lld %u", -0x123456789ll, MARKER);
    EXPECT("123456789 23063", "%I64x %u", 0x123456789ull, MARKER);
    EXPECT("-3 23063", "%hd %u", (int)-3, MARKER);
    EXPECT("100%", "%u%%", 100u);

#ifdef LIBC_KERNEL
    puts("streamout (kernel): z, t and j length modifiers passed");
#else
    puts("streamout (libc): z, t and j length modifiers passed");
#endif
    return 0;
}
