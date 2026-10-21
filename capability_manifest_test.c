#include "../services/processd/include/discover.h"
#include "../services/deviced/include/discover.h"
#include "../services/deviced/include/configparser.h"
#include <ddk/utils.h>
#include <internal/_syscalls.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { printf("line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

// CapabilityParse shares an object with OS helpers. None may run in this test.
SCTYPE
syscall1(
    _In_ SCTYPE function,
    _In_ SCTYPE arg0)
{
    (void)function;
    (void)arg0;
    CHECK(0);
    return 0;
}

SCTYPE
syscall2(
    _In_ SCTYPE function,
    _In_ SCTYPE arg0,
    _In_ SCTYPE arg1)
{
    (void)function;
    (void)arg0;
    (void)arg1;
    CHECK(0);
    return 0;
}

oserr_t
OSSystemQuery(
    _In_ enum OSSystemQueryRequest request,
    _In_ void*                     buffer,
    _In_ size_t                    maxSize,
    _In_ size_t*                   bytesQueriedOut)
{
    (void)request;
    (void)buffer;
    (void)maxSize;
    (void)bytesQueriedOut;
    CHECK(0);
    return OS_ENOTSUPPORTED;
}

void
SystemDebug(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format, ...)
{
    va_list arguments;

    (void)level;
    va_start(arguments, format);
    vprintf(format, arguments);
    va_end(arguments);
    puts("");
}

static void
__FreeDependency(
    _In_ element_t* entry,
    _In_ void*      context)
{
    (void)context;
    mstr_delete(entry->value);
    free(entry);
}

static void
__CheckService(
    _In_ const char*    yaml,
    _In_ Capabilities_t expected,
    _In_ int            dependencies)
{
    struct SystemService service = {0};
    char*                path;
    char*                dependency;

    CHECK(PSParseServiceYAML(&service, (const uint8_t*)yaml, strlen(yaml)) == OS_EOK);
    CHECK(service.Capabilities == expected);
    path = mstr_u8(service.APIPath);
    CHECK(path != NULL);
    CHECK(strcmp(path, "/service/test") == 0);
    CHECK(list_count(&service.Dependencies) == dependencies);
    if (dependencies) {
        dependency = mstr_u8(service.Dependencies.head->value);
        CHECK(dependency != NULL);
        CHECK(strcmp(dependency, "filed") == 0);
        free(dependency);
    }

    free(path);
    mstr_delete(service.APIPath);
    list_clear(&service.Dependencies, __FreeDependency, NULL);
}

static void
__ServiceCases(void)
{
    __CheckService(
        "service:\n  capabilities: [deviceio-access, interrupt-control]\n"
        "  path: /service/test\n",
        CAPABILITY_DEVICEIO_ACCESS | CAPABILITY_INTERRUPT_CONTROL,
        0
    );
    __CheckService(
        "service:\n  path: /service/test\n  capabilities: [ramdisk-access]\n",
        CAPABILITY_RAMDISK_ACCESS,
        0
    );
    __CheckService(
        "service:\n  capabilities: [grant-control]\n"
        "  depends:\n    services: [filed]\n  path: /service/test\n",
        CAPABILITY_GRANT_CAPABILITY,
        1
    );
    __CheckService(
        "service:\n  depends:\n    services: [filed]\n"
        "  capabilities: []\n  path: /service/test\n",
        0,
        1
    );
    __CheckService("service:\n  path: /service/test\n", 0, 0);
    puts("Service capability lists: following path, end of mapping, dependencies, empty and absent passed");
}

int
main(
    _In_ int    argc,
    _In_ char** argv)
{
    struct DriverConfiguration* driver;
    Capabilities_t              expected;
    FILE*                       file;
    char*                       yaml;
    long                        length;

    if (argc == 1) {
        __ServiceCases();
        return 0;
    }

    CHECK(argc == 3);
    expected = (Capabilities_t)strtoul(argv[2], NULL, 0);
    file = fopen(argv[1], "rb");
    CHECK(file != NULL);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    length = ftell(file);
    CHECK(length > 0);
    CHECK(fseek(file, 0, SEEK_SET) == 0);
    yaml = malloc((size_t)length);
    CHECK(yaml != NULL);
    CHECK(fread(yaml, 1, (size_t)length, file) == (size_t)length);
    fclose(file);

    driver = calloc(1, sizeof(*driver));
    CHECK(driver != NULL);
    CHECK(DmDriverConfigParseYaml((const uint8_t*)yaml, (size_t)length, driver) == OS_EOK);
    CHECK(driver->Capabilities == expected);
    DmDriverConfigDestroy(driver);
    free(yaml);
    printf("%s: capabilities=0x%x\n", argv[1], expected);
    return 0;
}
