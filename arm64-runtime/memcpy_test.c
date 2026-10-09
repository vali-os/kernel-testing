#include <os/mollenos.h>

#define memcpy vali_memcpy
#include "../../librt/libc/mem/memcpy.c"

#ifndef LIBC_KERNEL
#include "../../kernel/arch/aarch64/common/exceptions.c"
#include "../../kernel/arch/aarch64/common/cpu.c"
#endif
#undef memcpy

#ifndef TEST_FEATURES
#define TEST_FEATURES OSSYSTEMCPUFEATURE_NEON
#endif
#ifndef TEST_QUERY_ERROR
#define TEST_QUERY_ERROR 0
#endif
#ifndef TEST_SHORT_QUERY
#define TEST_SHORT_QUERY 0
#endif

typedef void* (*CopyFunction)(void*, const void*, size_t);

static unsigned char __source[12288] __attribute__((aligned(4096)));
static unsigned char __destination[12288] __attribute__((aligned(4096)));
static unsigned int __queryCount;

static long
__Syscall(
    long number,
    long argument0,
    long argument1,
    long argument2)
{
    register long value0 __asm__("x0") = argument0;
    register long value1 __asm__("x1") = argument1;
    register long value2 __asm__("x2") = argument2;
    register long syscallNumber __asm__("x8") = number;

    __asm__ volatile("svc 0" : "+r"(value0) : "r"(value1), "r"(value2),
        "r"(syscallNumber) : "memory");
    return value0;
}

static void
__Check(
    int condition,
    int code)
{
    if (!condition) {
        __Syscall(93, code, 0, 0);
        __builtin_unreachable();
    }
}

oserr_t
OSSystemQuery(
    _In_ enum OSSystemQueryRequest request,
    _In_ void*                     buffer,
    _In_ size_t                    maxSize,
    _In_ size_t*                   bytesQueriedOut)
{
    OSSystemCPUFeaturesInfo_t* features = buffer;
    unsigned char source = 0x73;
    unsigned char destination = 0;

    __queryCount++;
    __Check(__queryCount == 1, 1);
    __Check(request == OSSYSTEMQUERY_CPUFEATURES && maxSize == sizeof(*features), 2);
    __Check(vali_memcpy(&destination, &source, 1) == &destination && destination == source, 3);
    features->Features = TEST_FEATURES;
    *bytesQueriedOut = TEST_SHORT_QUERY ? 0 : sizeof(*features);
    return TEST_QUERY_ERROR ? OS_ENOTSUPPORTED : OS_EOK;
}

#ifndef LIBC_KERNEL
static SystemMachine_t __machine;

SystemMachine_t*
GetMachine(void)
{
    return &__machine;
}

static void
__TestCpuFeatures(void)
{
    unsigned int coreId;

    __machine.NumberOfCores = 0;
    __Check(Arm64GetCpuFeatures() == 0, 30);
    __machine.NumberOfCores = ARM64_CPU_COUNT + 1;
    __Check(Arm64GetCpuFeatures() == 0, 31);
    __machine.NumberOfCores = 4;
    for (coreId = 0; coreId < 4; coreId++) {
        atomic_store(&g_arm64CpuLocals[coreId].UserCpuFeatures,
            OSSYSTEMCPUFEATURE_NEON | OSSYSTEMCPUFEATURE_MOPS);
    }
    __Check(Arm64GetCpuFeatures() ==
        (OSSYSTEMCPUFEATURE_NEON | OSSYSTEMCPUFEATURE_MOPS), 32);
    atomic_store(&g_arm64CpuLocals[2].UserCpuFeatures, OSSYSTEMCPUFEATURE_NEON);
    __Check(Arm64GetCpuFeatures() == OSSYSTEMCPUFEATURE_NEON, 33);
    atomic_store(&g_arm64CpuLocals[3].UserCpuFeatures, 0);
    __Check(Arm64GetCpuFeatures() == 0, 34);
}

static void*
__CopyNeon(
    void* destination,
    const void* source,
    size_t count)
{
    asm_memcpy_neon(destination, source, count / 128, count % 128);
    return destination;
}

static void*
__CopyMops(
    void* destination,
    const void* source,
    size_t count)
{
    asm_memcpy_mops(destination, source, count);
    return destination;
}

static void
__TestRestart(void)
{
    Context_t context = { 0 };
    unsigned int destinationRegister = 4;
    unsigned int sourceRegister = 5;
    unsigned int sizeRegister = 6;
    unsigned int format;
    unsigned int wrongOption;
    unsigned int epilogue;
    unsigned int set;
    unsigned int backward;
    int formatA;

    for (set = 0; set < 2; set++) {
        for (format = 0; format < 2; format++) {
            for (wrongOption = 0; wrongOption < 2; wrongOption++) {
                for (epilogue = 0; epilogue < 2; epilogue++) {
                    for (backward = 0; backward < (set ? 1U : 2U); backward++) {
                        formatA = format ^ wrongOption;
                        context.ErrorCode = (0x27ULL << 26) | (set << 24) |
                            (epilogue << 18) | (wrongOption << 17) | (format << 16) |
                            (destinationRegister << 10) | (sourceRegister << 5) | sizeRegister;
                        context.X[destinationRegister] = 8192;
                        context.X[sourceRegister] = 16384;
                        context.X[sizeRegister] = formatA && !backward ? -64ULL : 64;
                        context.Pstate = backward ? 1ULL << 31 : 0;
                        context.Pc = 4096;
                        __Check(__RestartMops(&context), 20);
                        __Check(context.X[sizeRegister] == 64, 21);
                        __Check(context.X[destinationRegister] ==
                            (backward && !formatA ? 8128U : formatA && !backward ? 8128U : 8192U), 22);
                        __Check(context.X[sourceRegister] ==
                            (set ? 16384U : backward && !formatA ? 16320U :
                                formatA && !backward ? 16320U : 16384U), 23);
                        __Check(context.Pc == (epilogue ? 4088U : 4092U), 24);
                    }
                }
            }
        }
    }
    context.ErrorCode = (0x27ULL << 26) | (31U << 10) | sizeRegister;
    __Check(!__RestartMops(&context), 25);
}
#endif

static void
__TestCopy(
    CopyFunction copy)
{
    static const size_t offsets[] = { 0, 1, 3, 7, 8, 15, 16, 31 };
    size_t count;
    size_t sourceOffset;
    size_t destinationOffset;
    size_t index;
    unsigned char* destination;
    const unsigned char* source;

    for (index = 0; index < 8192; index++) {
        __source[index] = (unsigned char)(index * 37 + index / 256);
    }
    for (sourceOffset = 0; sourceOffset < sizeof(offsets) / sizeof(offsets[0]); sourceOffset++) {
        for (destinationOffset = 0; destinationOffset < sizeof(offsets) / sizeof(offsets[0]); destinationOffset++) {
            source = __source + offsets[sourceOffset];
            destination = __destination + offsets[destinationOffset] + 1;
            for (count = 0; count <= 8192 - 64; count++) {
                if (count > 256 && (count < 1279 || count > 1408) &&
                    count != 4095 && count != 4096 && count != 4097 && count != 8128) {
                    continue;
                }
                destination[-1] = 0xa5;
                for (index = 0; index <= count; index++) {
                    destination[index] = 0xa5;
                }
                __Check(copy(destination, source, count) == destination, 4);
                __Check(destination[-1] == 0xa5 && destination[count] == 0xa5, 5);
                for (index = 0; index < count; index++) {
                    __Check(destination[index] == source[index], 6);
                }
            }
        }
    }
    for (count = 0; count <= 4096; count++) {
        if (count > 256 && (count < 1279 || count > 1408) && count != 4095 && count != 4096) {
            continue;
        }
        destination = __destination + 8192 - count;
        source = __source + 8192 - count;
        __Check(copy(destination, source, count) == destination, 7);
        for (index = 0; index < count; index++) {
            __Check(destination[index] == source[index], 8);
        }
        __Check(copy(__destination, source, count) == __destination, 9);
        __Check(copy(destination, __source, count) == destination, 10);
    }
}

void
_start(void)
{
#ifndef LIBC_KERNEL
    MemCpyTemplate expected = memcpy_base;

    if (!TEST_QUERY_ERROR && !TEST_SHORT_QUERY) {
        if (TEST_FEATURES & OSSYSTEMCPUFEATURE_MOPS) {
            expected = memcpy_mops;
        } else if (TEST_FEATURES & OSSYSTEMCPUFEATURE_NEON) {
            expected = memcpy_neon;
        }
    }
#endif

    __Check(__Syscall(226, (long)(__source + 8192), 4096, 0) == 0, 11);
    __Check(__Syscall(226, (long)(__destination + 8192), 4096, 0) == 0, 12);
    __TestCopy(vali_memcpy);
#ifdef LIBC_KERNEL
    __Check(__queryCount == 0, 13);
#else
    __Check(__queryCount == 1, 14);
    __Check(atomic_load(&g_memcpyImpl) == expected, 15);
    if (!TEST_QUERY_ERROR && !TEST_SHORT_QUERY) {
        if (TEST_FEATURES & OSSYSTEMCPUFEATURE_NEON) {
            __TestCopy(__CopyNeon);
        }
        if (TEST_FEATURES & OSSYSTEMCPUFEATURE_MOPS) {
            __TestCopy(__CopyMops);
        }
    }
    __TestRestart();
    __TestCpuFeatures();
#endif
    __Syscall(93, 0, 0, 0);
    __builtin_unreachable();
}
