// Exercise the ACPICA vsnprintf (librt/libacpi/.../utprint.c) on the host.
// Kernels built with ACPI link this vsnprintf ahead of the one in libk, so
// every kernel TRACE, WARNING and ERROR line is formatted by it.
//
// The test checks the z, t and j length modifiers on every integer
// conversion. Without them the formatter prints the conversion as text, and
// every argument after it is read from the wrong slot. Each case therefore
// prints a marker value after the sized value and checks that it is right.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef UTPRINT_SOURCE
#define UTPRINT_SOURCE "../../librt/libacpi/source/components/utilities/utprint.c"
#endif

// Rename the ACPICA C library functions so they do not replace the host
// ones. The host stdio.h is already included above with the real names.
#define vsnprintf AcpiTestVsnprintf
#define snprintf  AcpiTestSnprintf
#define sprintf   AcpiTestSprintf
#define vsprintf  AcpiTestVsprintf
#include UTPRINT_SOURCE
#undef vsnprintf
#undef snprintf
#undef sprintf
#undef vsprintf

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

// Marker printed after each sized value. If the sized argument is skipped or
// read with the wrong width, the marker comes out wrong or missing.
#define MARKER 0x5A17u

const char AcpiGbl_LowerHexDigits[] = "0123456789abcdef";
const char AcpiGbl_UpperHexDigits[] = "0123456789ABCDEF";

// The formatter uses the ACPICA 64-bit math helpers from utmath.c. The
// Vali build sets ACPI_USE_NATIVE_MATH64, so plain C operators match.
ACPI_STATUS
AcpiUtDivide(
    UINT64  dividend,
    UINT64  divisor,
    UINT64* quotient,
    UINT64* remainder)
{
    if (divisor == 0) {
        return AE_AML_DIVIDE_BY_ZERO;
    }
    if (quotient != NULL) {
        *quotient = dividend / divisor;
    }
    if (remainder != NULL) {
        *remainder = dividend % divisor;
    }
    return AE_OK;
}

ACPI_STATUS
AcpiUtShortMultiply(
    UINT64  multiplicand,
    UINT32  multiplier,
    UINT64* product)
{
    if (product != NULL) {
        *product = multiplicand * multiplier;
    }
    return AE_OK;
}

// Formats into a fixed buffer and compares against the expected text.
static void
__Expect(
    int         line,
    const char* expected,
    const char* format, ...)
{
    char    buffer[128];
    va_list arguments;
    int     written;

    memset(&buffer[0], 0, sizeof(buffer));
    va_start(arguments, format);
    written = AcpiTestVsnprintf(&buffer[0], sizeof(buffer) - 1, format, arguments);
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

    // The kernel DMA log line that printed "%zu" before the fix.
    EXPECT("ScDmaMapShm: SHMDeviceMap context 235, SHM 376, offset 4096, length 512, direction 1 failed: 3",
           "ScDmaMapShm: SHMDeviceMap context %u, SHM %u, offset %zu, length %zu, direction %u failed: %u",
           235u, 376u, (size_t)4096, (size_t)512, 1u, 3u);

#if SIZE_MAX > 0xFFFFFFFFu
    // On 64-bit targets size_t and ptrdiff_t hold values above 32 bits. The
    // upper half must be printed, and must not be read as the next argument.
    EXPECT("4886718345 23063", "%zu %u", (size_t)0x123456789ull, MARKER);
    EXPECT("18446744073709551615 23063", "%zu %u", (size_t)SIZE_MAX, MARKER);
    EXPECT("123456789 23063", "%zx %u", (size_t)0x123456789ull, MARKER);
    EXPECT("-4886718345 23063", "%zd %u", (intptr_t)-0x123456789ll, MARKER);
    EXPECT("-4886718345 23063", "%td %u", (ptrdiff_t)-0x123456789ll, MARKER);
    EXPECT("physical 0x13e5a9000, length 8589934592, extent 2",
           "physical 0x%llx, length %zu, extent %u",
           0x13E5A9000ull, (size_t)0x200000000ull, 2u);
#else
    // On 32-bit targets size_t is 32 bits, and the full range still prints.
    EXPECT("4294967295 23063", "%zu %u", (size_t)SIZE_MAX, MARKER);
    EXPECT("-2147483648 23063", "%td %u", (ptrdiff_t)PTRDIFF_MIN, MARKER);
#endif

    // Existing modifiers keep working.
    EXPECT("4886718345 23063", "%llu %u", 0x123456789ull, MARKER);
    EXPECT("-4886718345 23063", "%lld %u", -0x123456789ll, MARKER);
    EXPECT("-3 23063", "%hd %u", (int)-3, MARKER);
    EXPECT("100%", "%u%%", 100u);
    EXPECT("name: dma", "name: %s", "dma");

    puts("ACPICA vsnprintf: z, t and j length modifiers passed");
    return 0;
}
