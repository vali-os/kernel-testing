// Exercise the production AArch64 copy instructions with protected page ends.
#include <arch/mmu.h>

static long
__LinuxCall(
    _In_ long number,
    _In_ long first,
    _In_ long second,
    _In_ long third,
    _In_ long fourth,
    _In_ long fifth,
    _In_ long sixth)
{
    register long x0 __asm__("x0") = first;
    register long x1 __asm__("x1") = second;
    register long x2 __asm__("x2") = third;
    register long x3 __asm__("x3") = fourth;
    register long x4 __asm__("x4") = fifth;
    register long x5 __asm__("x5") = sixth;
    register long x8 __asm__("x8") = number;

    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                     "r"(x4), "r"(x5), "r"(x8) : "memory");
    return x0;
}

static void
__Check(
    _In_ int condition,
    _In_ int code)
{
    if (!condition) {
        __LinuxCall(93, code, 0, 0, 0, 0, 0);
        __builtin_unreachable();
    }
}

void
_start(void)
{
    long     mapping;
    uint8_t* pages;
    uint8_t  kernel[258];
    size_t   length;
    size_t   index;

    // Linux mmap/mprotect create readable RAM followed by an inaccessible page.
    mapping = __LinuxCall(222, 0, 8192, 3, 0x22, -1, 0);
    __Check(mapping > 0, 1);
    pages = (uint8_t*)mapping;
    __Check(__LinuxCall(226, mapping + 4096, 4096, 0, 0, 0, 0) == 0, 2);

    for (length = 0; length <= 257; length++) {
        for (index = 0; index < 4096; index++) {
            pages[index] = (uint8_t)index;
        }
        for (index = 0; index < sizeof(kernel); index++) {
            kernel[index] = 0xcc;
        }
        ArchCopyFromUser(kernel, pages + 4096 - length, length);
        for (index = 0; index < length; index++) {
            __Check(kernel[index] == (uint8_t)(4096 - length + index), 3);
        }
        __Check(kernel[length] == 0xcc, 4);

        for (index = 0; index < sizeof(kernel); index++) {
            kernel[index] = (uint8_t)(index ^ 0x55);
        }
        for (index = 0; index < 4096; index++) {
            pages[index] = 0xcc;
        }
        ArchCopyToUser(pages + 4096 - length, kernel, length);
        for (index = 0; index < length; index++) {
            __Check(pages[4096 - length + index] == kernel[index], 5);
        }
        __Check(pages[4095 - length] == 0xcc, 6);
    }
    __LinuxCall(93, 0, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}
