/**
 * Copyright 2026, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * Host test for the ARM64 device tree console choice and for the address
 * translation it uses. The real kernel/devicetree/tree.c, resources.c and
 * kernel/arch/aarch64/devicetree/serial-console.c run on a DTB file.
 * test_console.py builds this file twice: as a normal build, and with
 * __OSCONFIG_RPI5_EARLY_SERIAL and an __OSCONFIG_RPI5_EARLY_SERIAL_ADDRESS. It
 * passes the expected result of each check on the command line:
 *
 *   console_test <dtb> console <status> <path, -, driver, unreachable or build> <address> <length>
 *     Calls Arm64DeviceTreeFindConsole. A path of "-" expects no console.
 *     "driver" also expects no console, because the board leaves its serial
 *     port to the PL011 driver. "unreachable" also expects no console,
 *     because the stdout-path port is behind a bus the kernel can't
 *     translate. "build" expects a console that was not read from the tree,
 *     so Node and Path must be NULL. Any other value is the stdout-path of
 *     the node.
 *   console_test <dtb> register <node path> <index> <status> <address> <length>
 *     Calls DeviceTreeReadRegister on the node.
 *
 * Status is a name such as OS_ENOTSUPPORTED. The address and length are only
 * compared when the expected status is OS_EOK. A mismatch prints "mismatch"
 * and exits with 1, so the runner can tell it apart from a crash.
 *
 * The kernel log is replaced by LogAppendMessage below, which counts lines.
 * The early serial build must log exactly one line per call, a debug line
 * that names VALI_RPI5_EARLY_SERIAL_ADDRESS. The normal build logs one debug
 * line that names the PL011 driver when the console is left to the driver
 * ("driver"), one warning line that says the bus can't be translated when the
 * port can't be reached ("unreachable"), and nothing otherwise.
 */

#include <devicetree.h>
#include <log.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "serial-console.h"

// Large enough for every bundled Raspberry Pi tree.
#define __BLOB_SIZE_MAX 262144

// The kernel passes ARM64_UART_REGION_MINIMUM from
// kernel/arch/aarch64/common/private.h: FR at 0x18 plus its 4 bytes.
#define __UART_REGION_MINIMUM 0x1c

// The log lines a console check expects from a normal build.
enum __ExpectedLog {
    __EXPECTED_LOG_NONE,
    __EXPECTED_LOG_DRIVER,
    __EXPECTED_LOG_UNREACHABLE
};

static unsigned int g_logLines;
static unsigned int g_debugLinesNamingSetting;
static unsigned int g_debugLinesNamingDriver;
static unsigned int g_warningLinesNamingBus;

void*
kmalloc(size_t size)
{
    return malloc(size);
}

void
kfree(void* pointer)
{
    free(pointer);
}

void
LogAppendMessage(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format, ...)
{
    char    line[512];
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);

    printf("log %u: %s\n", (unsigned int)level, line);
    g_logLines++;
    if (level == OSSYSLOGLEVEL_DEBUG && strstr(line, "VALI_RPI5_EARLY_SERIAL_ADDRESS")) {
        g_debugLinesNamingSetting++;
    }
    if (level == OSSYSLOGLEVEL_DEBUG && strstr(line, "PL011 driver")) {
        g_debugLinesNamingDriver++;
    }
    if (level == OSSYSLOGLEVEL_WARNING && strstr(line, "bus the kernel can't translate")) {
        g_warningLinesNamingBus++;
    }
}

static const struct {
    const char* Name;
    oserr_t     Status;
} g_statusNames[] = {
    { "OS_EOK",           OS_EOK },
    { "OS_ENOENT",        OS_ENOENT },
    { "OS_EINVALPARAMS",  OS_EINVALPARAMS },
    { "OS_ENOTSUPPORTED", OS_ENOTSUPPORTED },
    { "OS_EOVERFLOW",     OS_EOVERFLOW }
};

static int
__ParseStatus(
    _In_  const char* name,
    _Out_ oserr_t*    statusOut)
{
    for (size_t i = 0; i < sizeof(g_statusNames) / sizeof(g_statusNames[0]); i++) {
        if (!strcmp(g_statusNames[i].Name, name)) {
            *statusOut = g_statusNames[i].Status;
            return 0;
        }
    }
    printf("unknown status name %s\n", name);
    return 1;
}

static int
__Mismatch(
    _In_ const char* what,
    _In_ const char* expected,
    _In_ const char* actual)
{
    printf("mismatch in %s: expected %s, got %s\n", what, expected, actual);
    return 1;
}

static int
__CheckValue(
    _In_ const char* what,
    _In_ uint64_t    expected,
    _In_ uint64_t    actual)
{
    char expectedText[32];
    char actualText[32];

    if (expected == actual) {
        return 0;
    }
    snprintf(expectedText, sizeof(expectedText), "0x%llx", (unsigned long long)expected);
    snprintf(actualText, sizeof(actualText), "0x%llx", (unsigned long long)actual);
    return __Mismatch(what, expectedText, actualText);
}

static int
__CheckStatus(
    _In_ const char* what,
    _In_ oserr_t     expected,
    _In_ oserr_t     actual)
{
    char expectedText[16];
    char actualText[16];

    if (expected == actual) {
        return 0;
    }
    snprintf(expectedText, sizeof(expectedText), "%u", (unsigned int)expected);
    snprintf(actualText, sizeof(actualText), "%u", (unsigned int)actual);
    return __Mismatch(what, expectedText, actualText);
}

/**
 * Checks the log lines of one console call, see the comment at the top.
 *
 * @param expected The lines a normal build must log. The early serial build
 *        always logs the same line.
 */
static int
__CheckLog(
    _In_ enum __ExpectedLog expected)
{
#if defined(__OSCONFIG_RPI5_EARLY_SERIAL)
    (void)expected;
    if (g_logLines != 1 || g_debugLinesNamingSetting != 1 || g_debugLinesNamingDriver != 0 ||
        g_warningLinesNamingBus != 0) {
        return __Mismatch("log", "one debug line that names VALI_RPI5_EARLY_SERIAL_ADDRESS", "something else");
    }
#else
    if (g_debugLinesNamingSetting) {
        return __Mismatch("log", "no line that names VALI_RPI5_EARLY_SERIAL_ADDRESS", "such a line");
    }
    switch (expected) {
        case __EXPECTED_LOG_DRIVER:
            if (g_logLines != 1 || g_debugLinesNamingDriver != 1) {
                return __Mismatch("log", "one debug line that names the PL011 driver", "something else");
            }
            break;
        case __EXPECTED_LOG_UNREACHABLE:
            if (g_logLines != 1 || g_warningLinesNamingBus != 1) {
                return __Mismatch("log", "one warning line about a bus the kernel can't translate",
                                  "something else");
            }
            break;
        default:
            if (g_logLines) {
                return __Mismatch("log", "no log lines", "log lines");
            }
            break;
    }
#endif
    return 0;
}

static enum __ExpectedLog
__ExpectedLogForPath(
    _In_ const char* path)
{
    if (!strcmp(path, "driver")) {
        return __EXPECTED_LOG_DRIVER;
    }
    if (!strcmp(path, "unreachable")) {
        return __EXPECTED_LOG_UNREACHABLE;
    }
    return __EXPECTED_LOG_NONE;
}

static int
__CheckConsole(
    _In_ const DeviceTree_t* tree,
    _In_ char**              arguments)
{
    Arm64DeviceTreeConsole_t console;
    const char*              path = arguments[1];
    oserr_t                  expected;
    oserr_t                  status;

    if (__ParseStatus(arguments[0], &expected)) {
        return 1;
    }

    status = Arm64DeviceTreeFindConsole(tree, __UART_REGION_MINIMUM, &console);
    if (__CheckStatus("console status", expected, status)) {
        return 1;
    }
    if (__CheckLog(__ExpectedLogForPath(path))) {
        return 1;
    }
    if (status != OS_EOK) {
        return 0;
    }

    if (__ExpectedLogForPath(path) != __EXPECTED_LOG_NONE || !strcmp(path, "-")) {
        if (console.Node || console.Path || console.Address || console.Length) {
            return __Mismatch("console", "none (all fields zero)", "a console");
        }
        return 0;
    }

    if (!strcmp(path, "build")) {
        if (console.Node || console.Path) {
            return __Mismatch("console", "an address from the build", "a node from the tree");
        }
    } else if (!console.Node || !console.Path) {
        return __Mismatch("console node", path, "none");
    }
    if (console.Path && strcmp(console.Path, path)) {
        return __Mismatch("console path", path, console.Path);
    }
    if (console.Node && DeviceTreeFindPath(tree, path) != console.Node) {
        return __Mismatch("console node", path, "a different node");
    }
    if (__CheckValue("console address", strtoull(arguments[2], NULL, 0), console.Address)) {
        return 1;
    }
    return __CheckValue("console length", strtoull(arguments[3], NULL, 0), console.Length);
}

static int
__CheckRegister(
    _In_ const DeviceTree_t* tree,
    _In_ char**              arguments)
{
    const DeviceTreeNode_t* node;
    uint64_t                address = 0;
    uint64_t                length = 0;
    oserr_t                 expected;
    oserr_t                 status;

    node = DeviceTreeFindPath(tree, arguments[0]);
    if (!node) {
        return __Mismatch("node lookup", arguments[0], "no node");
    }
    if (__ParseStatus(arguments[2], &expected)) {
        return 1;
    }

    status = DeviceTreeReadRegister(node, (unsigned int)strtoul(arguments[1], NULL, 0), &address, &length);
    if (__CheckStatus("register status", expected, status)) {
        return 1;
    }
    if (status != OS_EOK) {
        return 0;
    }

    if (__CheckValue("register address", strtoull(arguments[3], NULL, 0), address)) {
        return 1;
    }
    return __CheckValue("register length", strtoull(arguments[4], NULL, 0), length);
}

int
main(
    _In_ int    argc,
    _In_ char** argv)
{
    static unsigned char blob[__BLOB_SIZE_MAX];
    DeviceTree_t*        tree;
    FILE*                file;
    size_t               size;
    oserr_t              status;
    int                  result;

    if (argc < 3) {
        printf("usage: %s <dtb> console|register ...\n", argv[0]);
        return 2;
    }

    file = fopen(argv[1], "rb");
    if (!file) {
        printf("cannot open %s\n", argv[1]);
        return 2;
    }
    size = fread(blob, 1, sizeof(blob), file);
    fclose(file);
    if (size == 0 || size == sizeof(blob)) {
        printf("cannot read %s, or it is larger than %u bytes\n", argv[1], __BLOB_SIZE_MAX - 1);
        return 2;
    }

    status = DeviceTreeCreate(blob, (uint32_t)size, &tree);
    if (status != OS_EOK) {
        printf("DeviceTreeCreate failed on %s: %u\n", argv[1], (unsigned int)status);
        return 2;
    }

    if (!strcmp(argv[2], "console") && argc == 7) {
        result = __CheckConsole(tree, &argv[3]);
    } else if (!strcmp(argv[2], "register") && argc == 8) {
        result = __CheckRegister(tree, &argv[3]);
    } else {
        printf("bad arguments for %s\n", argv[2]);
        result = 2;
    }

    DeviceTreeDestroy(tree);
    return result;
}
