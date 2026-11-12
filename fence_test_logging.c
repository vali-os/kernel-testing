/** Copyright 2026, Philip Meulengracht. SPDX-License-Identifier: GPL-3.0-or-later */
#include <ddk/utils.h>
#include <assert.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>

atomic_uint g_fenceLogCalls;

void
SystemDebug(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format, ...)
{
    va_list arguments;
    char    message[512];

    // Exercise diagnostic formatting without entering the Vali debug syscall.
    (void)level;
    va_start(arguments, format);
    assert(vsnprintf(message, sizeof(message), format, arguments) >= 0);
    va_end(arguments);
    atomic_fetch_add(&g_fenceLogCalls, 1);
}
