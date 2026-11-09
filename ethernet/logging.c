#include <ddk/utils.h>
#include <stdarg.h>
#include <stdio.h>

unsigned g_diagnosticLogs;

void
SystemDebug(
    _In_ enum OSSysLogLevel level,
    _In_ const char* format, ...)
{
    char buffer[1024];
    va_list arguments;
    (void)level;

    va_start(arguments, format);
    vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    g_diagnosticLogs++;
}
