/** ARM instruction-level checks, linked without a host C runtime. */
#include <fenv.h>
#include <math.h>
#include <stdint.h>

static void
__Check(
    int condition,
    int code)
{
    register long status __asm__("x0") = code;
    register long syscall __asm__("x8") = 93;

    if (!condition) {
        __asm__ volatile("svc 0" :: "r"(status), "r"(syscall) : "memory");
        __builtin_unreachable();
    }
}

void
_start(void)
{
    fenv_t saved;
    int quotient;
    register long status __asm__("x0") = 0;
    register long syscall __asm__("x8") = 93;

    fesetenv(FE_DFL_ENV);
    __Check(fegetround() == FE_TONEAREST, 1);
    __Check(lrint(2.5) == 2 && lrint(3.5) == 4, 2);
    __Check(llrint(-2.5) == -2 && llrint(0x1p40) == (1LL << 40), 3);
    __Check(fetestexcept(FE_INEXACT) == FE_INEXACT, 4);
    fesetround(FE_DOWNWARD);
    __Check(lrint(2.75) == 2 && lrintf(-2.25f) == -3, 5);
    __Check(llrintf(-2.25f) == -3, 6);
    fegetenv(&saved);
    fesetround(FE_UPWARD);
    __Check(lrint(2.25) == 3 && lrint(-2.75) == -2, 7);
    fesetenv(&saved);
    __Check(fegetround() == FE_DOWNWARD, 8);
    _control87(_RC_CHOP, _MCW_RC);
    __Check(fegetround() == FE_TOWARDZERO && lrint(-2.75) == -2, 9);
    __Check((_control87(0, 0) & (_MCW_RC | _MCW_PC)) == (_RC_CHOP | _PC_53), 10);
    fesetenv(FE_DFL_ENV);
    __Check(sqrt(4.0) == 2.0 && sqrtf(9.0f) == 3.0f, 11);
    feclearexcept(FE_ALL_EXCEPT);
    __Check(sqrt(-1.0) != sqrt(-1.0), 12);
    __Check(fetestexcept(FE_INVALID) == FE_INVALID, 13);
    feholdexcept(&saved);
    __Check(fetestexcept(FE_ALL_EXCEPT) == 0, 14);
    feraiseexcept(FE_DIVBYZERO);
    feupdateenv(&saved);
    __Check(fetestexcept(FE_INVALID | FE_DIVBYZERO) == (FE_INVALID | FE_DIVBYZERO), 15);
    __Check(scalbn(1.0, -1074) == 0x1p-1074, 16);
    __Check(scalbn(0x1p1023, -2045) == 0x1p-1022, 17);
    __Check(scalbnf(1.0f, -149) == 0x1p-149f, 18);
    __Check(remquo(7.0, 2.0, &quotient) == -1.0 && quotient == 4, 19);
    __Check(remquo(-7.0, 2.0, &quotient) == 1.0 && quotient == -4, 20);
    __Check(remquo(0x1p-1074, 0x1p-1073, &quotient) == 0x1p-1074 && quotient == 0, 21);
    __asm__ volatile("svc 0" :: "r"(status), "r"(syscall) : "memory");
    __builtin_unreachable();
}
