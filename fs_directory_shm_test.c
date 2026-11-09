/** Exercise real filesystem readers with an odd offset in the original SHM view. */
#include "../modules/filesystems/valifs/main.c"
#define __WriteDirectoryEntry __MfsWriteDirectoryEntry
#include "../modules/filesystems/mfs/directory_operations.c"
#undef __WriteDirectoryEntry

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static uint8_t g_buffer[512];
static int g_next;
static int g_errno;

void
SystemDebug(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format,
    ...)
{
    (void)level;
    (void)format;
}

errno_t*
__errno(void)
{
    return &g_errno;
}

uint64_t
vafs_object_reader_read(
    _In_ struct VaFsObjectReader* reader,
    _In_ void*                   buffer,
    _In_ uint64_t                length)
{
    (void)reader;
    CHECK(buffer == g_buffer + 17 && length == 128);
    memset(buffer, 0xab, 7);
    return 7;
}

int
vafs_directory_reader_next(
    _In_  struct VaFsDirectoryReader* reader,
    _Out_ struct VaFsEntry*           entry)
{
    (void)reader;
    if (g_next++ != 0) {
        return -1;
    }
    *entry = (struct VaFsEntry){ .Name = "hello", .Type = VaFsEntryType_Directory };
    return 0;
}

void
MFSFromNativeFlags(
    _In_  FileRecord_t*  record,
    _Out_ unsigned int* flags,
    _Out_ unsigned int* permissions)
{
    (void)record;
    *flags = FILE_FLAG_DIRECTORY;
    *permissions = 0;
}

static void
__CheckGuards(
    _In_ size_t offset,
    _In_ size_t length)
{
    size_t i;

    for (i = 0; i < sizeof(g_buffer); i++) {
        if (i < offset || i >= offset + length) {
            CHECK(g_buffer[i] == 0x31);
        }
    }
}

int
main(void)
{
    struct __ValiFSHandle handle = { .Type = VALIFS_HANDLE_TYPE_FILE };
    struct VFSDirectoryEntry entry;
    struct VaFsEntry source = { .Name = "hello", .Type = VaFsEntryType_Directory };
    FileRecord_t record = { 0 };
    size_t bytes;
    size_t expected = sizeof(entry) + 6;

    memset(g_buffer, 0x31, sizeof(g_buffer));
    CHECK(FsRead(NULL, &handle, 42, g_buffer, 17, 128, &bytes) == OS_EOK);
    CHECK(bytes == 7 && g_buffer[17] == 0xab && g_buffer[23] == 0xab);
    __CheckGuards(17, bytes);

    memset(g_buffer, 0x31, sizeof(g_buffer));
    handle.Type = VALIFS_HANDLE_TYPE_DIR;
    CHECK(FsRead(NULL, &handle, 42, g_buffer, 17, sizeof(g_buffer) - 17, &bytes) == OS_EOK);
    CHECK(bytes == expected);
    memcpy(&entry, g_buffer + 17, sizeof(entry));
    CHECK(entry.NameLength == 6 && entry.Flags == FILE_FLAG_DIRECTORY);
    CHECK(memcmp(g_buffer + 17 + sizeof(entry), "hello", 6) == 0);
    __CheckGuards(17, bytes);

    // An entry that does not fit must leave all bytes untouched, including its
    // header. A zero-byte directory result excludes the caller's initial offset.
    memset(g_buffer, 0x31, sizeof(g_buffer));
    g_next = 0;
    CHECK(FsRead(NULL, &handle, 42, g_buffer, 17, sizeof(entry) + 1, &bytes) == OS_EOK);
    CHECK(bytes == 0);
    __CheckGuards(0, 0);
    CHECK(__WriteDirectoryEntry(g_buffer + 17, 1, &source) == 0);
    __CheckGuards(0, 0);

    memcpy(record.Name, "hello", 6);
    CHECK(__MfsWriteDirectoryEntry(&record, g_buffer + 17, 1) == 0);
    __CheckGuards(0, 0);
    CHECK(__MfsWriteDirectoryEntry(&record, g_buffer + 17, expected) == expected);
    memcpy(&entry, g_buffer + 17, sizeof(entry));
    CHECK(entry.NameLength == 6 && entry.Flags == FILE_FLAG_DIRECTORY);
    CHECK(memcmp(g_buffer + 17 + sizeof(entry), "hello", 6) == 0);
    __CheckGuards(17, expected);
    puts("Filesystem readers: odd SHM offsets, exact directory counts, unaligned headers and short buffers passed");
    return 0;
}
