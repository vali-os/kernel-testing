/**
 * Copyright 2026, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
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
 * syslogd host test
 * - Built twice by syslogd_test.py.
 * - Without SYSLOGD_TEST_READER it links text.c, fb.c, font.c and console.c,
 *   plus mstr_unicode.c from libds for UTF-8 decoding. It checks the text
 *   grid, the framebuffer drawing and the console on a framebuffer in memory.
 * - With SYSLOGD_TEST_READER it links reader.c, and replaces the console and
 *   OSKernelLogRead with stubs that record what the reader does.
 */

#include <os/osdefs.h>
#include <os/types/syslog.h>
#include <os/types/video.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "fb.h"
#include "reader.h"
#include "text.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
        abort(); \
    } \
} while (0)

static int  g_errorCount;
static char g_lastLog[512];

void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(&g_lastLog[0], sizeof(g_lastLog), format, arguments);
    va_end(arguments);
    if (level == OSSYSLOGLEVEL_ERROR) {
        g_errorCount++;
    }
}

#ifndef SYSLOGD_TEST_READER

extern const uint32_t g_fontHeight;
extern const uint32_t g_fontWidth;
extern const uint32_t g_fontCharCount;
extern const uint8_t  g_font[];
extern const uint16_t g_fontIndices[];

// Pixel layout used by most UEFI modes: blue in the low byte, then green,
// red and alpha.
#define BGRX_BLACK 0xFF000000U

static OSBootVideoDescriptor_t g_video;
static oserr_t                 g_videoStatus;
static void*                   g_mapping;
static int                     g_mapCount;

oserr_t
BootVideoQuery(
    _In_ OSBootVideoDescriptor_t* descriptor)
{
    if (g_videoStatus != OS_EOK) {
        return g_videoStatus;
    }
    *descriptor = g_video;
    return OS_EOK;
}

void*
BootVideoMapFramebuffer(void)
{
    g_mapCount++;
    return g_mapping;
}

static OSBootVideoDescriptor_t
__MakeMode(
    _In_ size_t width,
    _In_ size_t height,
    _In_ size_t padding)
{
    OSBootVideoDescriptor_t video = { 0 };

    video.Type             = OSBOOTVIDEO_FRAMEBUFFER;
    video.Width            = width;
    video.Height           = height;
    video.BytesPerScanline = width * 4 + padding;
    video.DepthBits        = 32;
    video.Blue             = (VideoColorComponent_t) { 0, 0xFF };
    video.Green            = (VideoColorComponent_t) { 8, 0xFF };
    video.Red              = (VideoColorComponent_t) { 16, 0xFF };
    video.Alpha            = (VideoColorComponent_t) { 24, 0xFF };
    return video;
}

static uint32_t*
__Pixel(
    _In_ void*                          pixels,
    _In_ const OSBootVideoDescriptor_t* video,
    _In_ uint32_t                       x,
    _In_ uint32_t                       y)
{
    return (uint32_t*)((uint8_t*)pixels + (size_t)y * video->BytesPerScanline) + x;
}

static void
__FillPixels(
    _In_ void*                          pixels,
    _In_ const OSBootVideoDescriptor_t* video,
    _In_ uint8_t                        value)
{
    memset(pixels, value, video->BytesPerScanline * video->Height);
}

/**
 * @brief Tells whether the cell at column, row shows character in rgb on a
 * black background, by comparing every pixel with the glyph in the font.
 */
static bool
__CellShows(
    _In_ void*                          pixels,
    _In_ const OSBootVideoDescriptor_t* video,
    _In_ uint32_t                       column,
    _In_ uint32_t                       row,
    _In_ uint32_t                       character,
    _In_ uint32_t                       rgb)
{
    uint32_t glyph = 0;
    uint32_t left  = FRAMEBUFFER_MARGIN + column * (g_fontWidth + 1);
    uint32_t top   = FRAMEBUFFER_MARGIN + row * g_fontHeight;

    for (uint32_t i = 0; i < g_fontCharCount; i++) {
        if (g_fontIndices[i] == character) {
            glyph = i;
            break;
        }
    }

    for (uint32_t y = 0; y < g_fontHeight; y++) {
        for (uint32_t x = 0; x < g_fontWidth + 1; x++) {
            bool     set    = (g_font[glyph * g_fontHeight + y] & (0x80 >> x)) != 0;
            uint32_t wanted = set ? (0xFF000000U | rgb) : BGRX_BLACK;
            if (*__Pixel(pixels, video, left + x, top + y) != wanted) {
                return false;
            }
        }
    }
    return true;
}

static bool
__RowShows(
    _In_ void*                          pixels,
    _In_ const OSBootVideoDescriptor_t* video,
    _In_ uint32_t                       row,
    _In_ const char*                    text,
    _In_ uint32_t                       rgb)
{
    for (uint32_t i = 0; text[i]; i++) {
        if (!__CellShows(pixels, video, i, row, (unsigned char)text[i], rgb)) {
            return false;
        }
    }
    return true;
}

static bool
__GridRowIs(
    _In_ const TextGrid_t* grid,
    _In_ uint32_t          row,
    _In_ const char*       text)
{
    const TextCell_t* cells  = TextGridRow(grid, row);
    size_t            length = strlen(text);

    for (uint32_t i = 0; i < grid->Columns; i++) {
        uint32_t wanted = i < length ? (unsigned char)text[i] : ' ';
        if (cells[i].Character != wanted) {
            return false;
        }
    }
    return true;
}

static void
__TestTextWrapAndScroll(void)
{
    TextGrid_t grid;

    CHECK(TextGridCreate(&grid, 0, 3) == OS_EINVALPARAMS);
    CHECK(TextGridCreate(&grid, 4, 3) == OS_EOK);
    CHECK(grid.DirtyFirst == 0 && grid.DirtyLast == 2);

    // Text that reaches the right edge continues on the next row.
    TextGridWrite(&grid, 0x123456, "abcdef");
    CHECK(__GridRowIs(&grid, 0, "abcd"));
    CHECK(__GridRowIs(&grid, 1, "ef"));
    CHECK(grid.CursorRow == 1 && grid.CursorColumn == 2);
    CHECK(TextGridRow(&grid, 0)[0].Color == 0x123456);

    // A line that fills the row exactly does not leave an empty row behind.
    TextGridWrite(&grid, 0, "\nwxyz\n");
    CHECK(grid.FirstRow == 1);
    CHECK(grid.CursorRow == 2 && grid.CursorColumn == 0);
    CHECK(__GridRowIs(&grid, 0, "ef"));
    CHECK(__GridRowIs(&grid, 1, "wxyz"));
    CHECK(__GridRowIs(&grid, 2, ""));

    // Scrolling moves every row, so all of them must be drawn again.
    TextGridClearDirty(&grid);
    TextGridWrite(&grid, 0, "q\n");
    CHECK(grid.FirstRow == 2);
    CHECK(__GridRowIs(&grid, 0, "wxyz"));
    CHECK(__GridRowIs(&grid, 1, "q"));
    CHECK(__GridRowIs(&grid, 2, ""));
    CHECK(grid.DirtyFirst == 0 && grid.DirtyLast == 2);

    // Without scrolling, only the rows that were written are marked.
    TextGridClearDirty(&grid);
    CHECK(grid.DirtyFirst > grid.DirtyLast);
    TextGridWrite(&grid, 0, "r");
    CHECK(grid.DirtyFirst == 2 && grid.DirtyLast == 2);
    TextGridDestroy(&grid);
    CHECK(grid.Cells == NULL);
}

static void
__TestTextControls(void)
{
    TextGrid_t grid;

    CHECK(TextGridCreate(&grid, 20, 2) == OS_EOK);

    // Tab moves to the next multiple of 8, carriage return to the start of
    // the row, and other control characters are skipped.
    TextGridWrite(&grid, 0, "ab\tc");
    CHECK(__GridRowIs(&grid, 0, "ab      c"));
    TextGridWrite(&grid, 0, "\rX\x01\x1b");
    CHECK(__GridRowIs(&grid, 0, "Xb      c"));
    CHECK(grid.CursorColumn == 1);

    // A tab near the right edge stops at the edge.
    TextGridWrite(&grid, 0, "\r1234567890123456789\t");
    CHECK(grid.CursorRow == 0 && grid.CursorColumn == 20);
    TextGridDestroy(&grid);
}

static void
__TestTextUtf8(void)
{
    TextGrid_t        grid;
    const TextCell_t* cells;

    CHECK(TextGridCreate(&grid, 16, 1) == OS_EOK);

    // Two, three and four byte characters.
    TextGridWrite(&grid, 0, "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");
    cells = TextGridRow(&grid, 0);
    CHECK(cells[0].Character == 0xE9);
    CHECK(cells[1].Character == 0x20AC);
    CHECK(cells[2].Character == 0x1F600);
    CHECK(grid.CursorColumn == 3);

    // A stray continuation byte, an overlong form and a value above U+10FFFF
    // each become U+FFFD. Only the first byte is skipped, so the bytes after
    // it are decoded on their own.
    TextGridWrite(&grid, 0, "\r\x80" "a" "\xC0\x80" "\xF4\x90\x80\x80");
    cells = TextGridRow(&grid, 0);
    CHECK(cells[0].Character == 0xFFFD);
    CHECK(cells[1].Character == 'a');
    CHECK(cells[2].Character == 0xFFFD);
    CHECK(cells[3].Character == 0xFFFD);
    CHECK(cells[4].Character == 0xFFFD);
    CHECK(cells[5].Character == 0xFFFD);
    CHECK(cells[6].Character == 0xFFFD);
    CHECK(cells[7].Character == 0xFFFD);
    CHECK(grid.CursorColumn == 8);
    TextGridDestroy(&grid);

    // A sequence cut off by the end of the text, and a lead byte followed by
    // many continuation bytes. Nothing past the zero byte is read. The text
    // is copied to the heap so the sanitizer sees a read past its end.
    {
        static const char cutOff[] = "z\xE2\x82";
        char*             copy     = malloc(sizeof(cutOff));

        CHECK(copy != NULL);
        memcpy(copy, cutOff, sizeof(cutOff));
        CHECK(TextGridCreate(&grid, 16, 1) == OS_EOK);
        TextGridWrite(&grid, 0, copy);
        cells = TextGridRow(&grid, 0);
        CHECK(cells[0].Character == 'z');
        CHECK(cells[1].Character == 0xFFFD);
        CHECK(cells[2].Character == 0xFFFD);
        CHECK(grid.CursorColumn == 3);
        free(copy);

        TextGridWrite(&grid, 0, "\r\xC0\x80\x80\x80\x80\x80\x80\x80\x80\x80");
        CHECK(grid.CursorColumn == 10);
        for (uint32_t i = 0; i < 10; i++) {
            CHECK(TextGridRow(&grid, 0)[i].Character == 0xFFFD);
        }
        TextGridDestroy(&grid);
    }
}

static void
__TestFramebufferCheckMode(void)
{
    OSBootVideoDescriptor_t video = __MakeMode(64, 40, 0);

    CHECK(FramebufferCheckMode(NULL) == OS_EINVALPARAMS);
    CHECK(FramebufferCheckMode(&video) == OS_EOK);

    video.Type = OSBOOTVIDEO_TTY;
    CHECK(FramebufferCheckMode(&video) == OS_ENOTSUPPORTED);

    video = __MakeMode(64, 40, 0);
    video.DepthBits = 24;
    CHECK(FramebufferCheckMode(&video) == OS_ENOTSUPPORTED);

    video = __MakeMode(64, 40, 0);
    video.BytesPerScanline = 64 * 4 - 1;
    CHECK(FramebufferCheckMode(&video) == OS_ENOTSUPPORTED);

    // One cell plus the margin on both sides is the smallest usable mode.
    video = __MakeMode(2 * FRAMEBUFFER_MARGIN + g_fontWidth, 40, 0);
    CHECK(FramebufferCheckMode(&video) == OS_ENOTSUPPORTED);
    video = __MakeMode(2 * FRAMEBUFFER_MARGIN + g_fontWidth + 1, 40, 0);
    CHECK(FramebufferCheckMode(&video) == OS_EOK);
    video = __MakeMode(64, 2 * FRAMEBUFFER_MARGIN + g_fontHeight - 1, 0);
    CHECK(FramebufferCheckMode(&video) == OS_ENOTSUPPORTED);
}

static void
__TestFramebufferColor(void)
{
    OSBootVideoDescriptor_t video = __MakeMode(64, 40, 0);
    Framebuffer_t           framebuffer;
    uint32_t                pixels[64 * 40];

    CHECK(FramebufferCreate(&framebuffer, &video, &pixels[0]) == OS_EOK);
    CHECK(FramebufferColor(&framebuffer, 0x123456) == 0xFF123456U);

    // Red in the low byte, and no alpha component.
    framebuffer.Red   = (VideoColorComponent_t) { 0, 0xFF };
    framebuffer.Blue  = (VideoColorComponent_t) { 16, 0xFF };
    framebuffer.Alpha = (VideoColorComponent_t) { 0, 0 };
    CHECK(FramebufferColor(&framebuffer, 0x123456) == 0x00563412U);

    // A 5 bit red component scales 0 to 255 down to 0 to 31.
    framebuffer.Red = (VideoColorComponent_t) { 11, 0x1F };
    CHECK(FramebufferColor(&framebuffer, 0xFF0000) == (0x1FU << 11));
    CHECK(FramebufferColor(&framebuffer, 0x000000) == 0);
    FramebufferDestroy(&framebuffer);
    CHECK(framebuffer.Scratch == NULL);
}

static void
__TestFramebufferDraw(void)
{
    // 4 padding bytes at the end of each scanline, and a height that is not
    // a multiple of the font height.
    OSBootVideoDescriptor_t video = __MakeMode(64, 40, 4);
    Framebuffer_t           framebuffer;
    TextGrid_t              grid;
    void*                   pixels = malloc(video.BytesPerScanline * video.Height);

    CHECK(pixels != NULL);
    __FillPixels(pixels, &video, 0xAA);
    CHECK(FramebufferCreate(&framebuffer, &video, pixels) == OS_EOK);
    CHECK(framebuffer.Columns == (64 - 2 * FRAMEBUFFER_MARGIN) / (g_fontWidth + 1));
    CHECK(framebuffer.Rows == (40 - 2 * FRAMEBUFFER_MARGIN) / g_fontHeight);

    // Clear reaches every visible pixel, including the rows below the last
    // full text row.
    FramebufferClear(&framebuffer);
    for (uint32_t y = 0; y < video.Height; y++) {
        for (uint32_t x = 0; x < video.Width; x++) {
            CHECK(*__Pixel(pixels, &video, x, y) == BGRX_BLACK);
        }
    }

    // A known glyph, and a code point without one, which shows glyph 0.
    CHECK(TextGridCreate(&grid, framebuffer.Columns, framebuffer.Rows) == OS_EOK);
    TextGridWrite(&grid, 0xFF0000, "!A\xF4\x8F\xBF\xBF");
    FramebufferDrawGrid(&framebuffer, &grid);
    CHECK(__CellShows(pixels, &video, 0, 0, '!', 0xFF0000));
    CHECK(__CellShows(pixels, &video, 1, 0, 'A', 0xFF0000));
    CHECK(__CellShows(pixels, &video, 2, 0, 0, 0xFF0000));
    CHECK(__CellShows(pixels, &video, 0, 1, ' ', 0));
    CHECK(grid.DirtyFirst > grid.DirtyLast);

    // The margin stays black.
    for (uint32_t x = 0; x < video.Width; x++) {
        for (uint32_t y = 0; y < FRAMEBUFFER_MARGIN; y++) {
            CHECK(*__Pixel(pixels, &video, x, y) == BGRX_BLACK);
        }
    }

    // Only changed rows are drawn. A mark on row 1 survives a change on row 0.
    *__Pixel(pixels, &video, FRAMEBUFFER_MARGIN, FRAMEBUFFER_MARGIN + g_fontHeight) = 0x01020304;
    TextGridWrite(&grid, 0x00FF00, "\rB");
    FramebufferDrawGrid(&framebuffer, &grid);
    CHECK(__CellShows(pixels, &video, 0, 0, 'B', 0x00FF00));
    CHECK(*__Pixel(pixels, &video, FRAMEBUFFER_MARGIN, FRAMEBUFFER_MARGIN + g_fontHeight) == 0x01020304);

    TextGridDestroy(&grid);
    FramebufferDestroy(&framebuffer);
    free(pixels);
}

static void
__TestConsoleSetupFailures(void)
{
    static uint32_t pixels[16];

    // Nothing is mapped for a mode that cannot be drawn on.
    g_video    = __MakeMode(64, 40, 0);
    g_video.Type = OSBOOTVIDEO_TTY;
    g_mapping  = &pixels[0];
    g_mapCount = 0;
    CHECK(ConsoleInitialize() == OS_ENOTSUPPORTED);
    CHECK(g_mapCount == 0);

    g_videoStatus = OS_ENOTSUPPORTED;
    g_errorCount  = 0;
    CHECK(ConsoleInitialize() == OS_ENOTSUPPORTED);
    CHECK(g_mapCount == 0);
    CHECK(g_errorCount == 1);
    g_videoStatus = OS_EOK;

    g_video   = __MakeMode(64, 40, 0);
    g_mapping = NULL;
    CHECK(ConsoleInitialize() != OS_EOK);
    CHECK(g_mapCount == 1);
    CHECK(g_errorCount == 2);
}

static void
__TestConsole(void)
{
    OSBootVideoDescriptor_t* video = &g_video;
    OSKernelLogRecord_t      record = { 0 };
    void*                    pixels;
    void*                    snapshot;
    size_t                   size;

    // 500 pixels fit 70 cells, so none of the lines below wraps.
    g_video   = __MakeMode(500, 5 * g_fontHeight + 2 * FRAMEBUFFER_MARGIN, 0);
    size      = g_video.BytesPerScanline * g_video.Height;
    pixels    = malloc(size);
    snapshot  = malloc(size);
    CHECK(pixels != NULL && snapshot != NULL);
    __FillPixels(pixels, video, 0xAA);
    g_mapping  = pixels;
    g_mapCount = 0;
    CHECK(ConsoleInitialize() == OS_EOK);
    CHECK(g_mapCount == 1);
    CHECK(*__Pixel(pixels, video, 0, 0) == BGRX_BLACK);

    // Lines use the kernel format and level color. Trace is not shown.
    record.Level     = OSSYSLOGLEVEL_ERROR;
    record.CoreID    = 1;
    record.ThreadID  = 7;
    record.Timestamp = 5 * NSEC_PER_MSEC;
    ConsoleShowRecord(&record, "hi");
    record.Level = OSSYSLOGLEVEL_TRACE;
    ConsoleShowRecord(&record, "hidden");
    record.Level = 9;
    ConsoleShowRecord(&record, "bad level");
    ConsoleShowDropped(3);
    ConsoleDraw();
    CHECK(__RowShows(pixels, video, 0, "000000005 [error-1-7] hi", 0xFF392B));
    CHECK(__RowShows(pixels, video, 1, "[syslogd] 3 messages were dropped", 0x9B59B6));
    CHECK(__RowShows(pixels, video, 2, "    ", 0));

    // While inhibited nothing is drawn, but messages are kept.
    ConsoleInhibit();
    record.Level = OSSYSLOGLEVEL_DEBUG;
    ConsoleShowRecord(&record, "while inhibited");
    memcpy(snapshot, pixels, size);
    ConsoleDraw();
    CHECK(memcmp(snapshot, pixels, size) == 0);

    // A second uninhibit, or one without an inhibit, changes nothing.
    // After an uninhibit, the screen is cleared and drawn again.
    __FillPixels(pixels, video, 0x55);
    ConsoleUninhibit();
    ConsoleUninhibit();
    ConsoleDraw();
    CHECK(*__Pixel(pixels, video, 0, 0) == BGRX_BLACK);
    CHECK(__RowShows(pixels, video, 0, "000000005 [error-1-7] hi", 0xFF392B));
    CHECK(__RowShows(pixels, video, 2, "000000005 [debug-1-7] while inhibited", 0x2ECC71));

    memcpy(snapshot, pixels, size);
    ConsoleUninhibit();
    ConsoleDraw();
    CHECK(memcmp(snapshot, pixels, size) == 0);

    free(snapshot);
    free(pixels);
}

int
main(void)
{
    __TestTextWrapAndScroll();
    __TestTextControls();
    __TestTextUtf8();
    __TestFramebufferCheckMode();
    __TestFramebufferColor();
    __TestFramebufferDraw();
    __TestConsoleSetupFailures();
    __TestConsole();
    printf("syslogd console tests passed\n");
    return 0;
}

#else // SYSLOGD_TEST_READER

#define MAX_SHOWN 16
#define MAX_READS 8

struct __ReadReply {
    oserr_t  Status;
    uint64_t NextSequence;
    uint64_t Dropped;
    uint8_t  Data[512];
    uint32_t Length;
};

static struct __ReadReply g_replies[MAX_READS];
static int                g_replyCount;
static int                g_readCount;
static uint64_t           g_readSequences[MAX_READS];

static char     g_shown[MAX_SHOWN][OSKERNELLOG_MESSAGE_MAX + 1];
static int      g_shownCount;
static uint64_t g_droppedShown;
static int      g_drawCount;

static int g_cancelAfter;
static int g_cancelChecks;
static int g_yieldCount;
static int g_sleepCount;
static int g_sleepResult;

oserr_t
OSKernelLogRead(
    _In_  uint64_t                 sequence,
    _In_  void*                    buffer,
    _In_  size_t                   bufferSize,
    _Out_ OSKernelLogReadResult_t* result)
{
    struct __ReadReply* reply;

    CHECK(bufferSize == LOG_READER_BUFFER_SIZE);
    CHECK(g_readCount < g_replyCount);
    g_readSequences[g_readCount] = sequence;
    reply = &g_replies[g_readCount++];
    if (reply->Status != OS_EOK) {
        return reply->Status;
    }

    memcpy(buffer, &reply->Data[0], reply->Length);
    result->NextSequence = reply->NextSequence;
    result->Dropped      = reply->Dropped;
    result->BytesRead    = reply->Length;
    return OS_EOK;
}

void
ConsoleShowRecord(
    _In_ const OSKernelLogRecord_t* record,
    _In_ const char*                text)
{
    CHECK(g_shownCount < MAX_SHOWN);
    CHECK(strlen(text) == record->Length);
    strcpy(&g_shown[g_shownCount++][0], text);
}

void
ConsoleShowDropped(
    _In_ uint64_t count)
{
    // Dropped messages are reported before the messages after them.
    CHECK(g_shownCount == 0);
    g_droppedShown += count;
}

void
ConsoleDraw(void)
{
    g_drawCount++;
}

bool
usched_is_cancelled(
    _In_ const void* cancellationToken)
{
    return ++g_cancelChecks > g_cancelAfter;
}

void
usched_job_yield(void)
{
    g_yieldCount++;
}

int
usched_job_sleep_us(
    _In_ uint64_t microseconds)
{
    CHECK(microseconds > 0);
    g_sleepCount++;
    return g_sleepResult;
}

static void
__ResetReader(void)
{
    memset(&g_replies[0], 0, sizeof(g_replies));
    g_replyCount   = 0;
    g_readCount    = 0;
    g_shownCount   = 0;
    g_droppedShown = 0;
    g_drawCount    = 0;
    g_cancelAfter  = 1000;
    g_cancelChecks = 0;
    g_yieldCount   = 0;
    g_sleepCount   = 0;
    g_sleepResult  = 0;
    g_errorCount   = 0;
}

/**
 * @brief Adds a record to a reply the way the kernel lays it out. The padding
 * after the text is filled with 'X', so a reader that expects a zero byte
 * after the text shows the padding as well.
 */
static OSKernelLogRecord_t*
__AddRecord(
    _In_ struct __ReadReply* reply,
    _In_ uint64_t            sequence,
    _In_ const char*         text)
{
    OSKernelLogRecord_t* record = (OSKernelLogRecord_t*)&reply->Data[reply->Length];
    uint32_t             length = (uint32_t)strlen(text);
    uint32_t             size   = (uint32_t)((sizeof(OSKernelLogRecord_t) + length + 7) & ~7U);

    CHECK(reply->Length + size + 8 <= sizeof(reply->Data));
    memset(record, 'X', size);
    memset(record, 0, sizeof(OSKernelLogRecord_t));
    record->Size     = size;
    record->Length   = (uint16_t)length;
    record->Level    = OSSYSLOGLEVEL_DEBUG;
    record->Sequence = sequence;
    memcpy((uint8_t*)record + sizeof(OSKernelLogRecord_t), text, length);
    reply->Length      += size;
    reply->NextSequence = sequence + 1;
    return record;
}

static void
__TestShowNext(void)
{
    LogReader_t reader;
    uint32_t    count;

    __ResetReader();
    CHECK(LogReaderCreate(NULL) == OS_EINVALPARAMS);
    CHECK(LogReaderCreate(&reader) == OS_EOK);
    CHECK(reader.NextSequence == 0);

    // Messages 3 and 4 after 3 that were dropped, then nothing new. The
    // longer message comes first, so a text copy without its zero byte shows
    // the end of the previous message.
    g_replies[0].Dropped = 3;
    __AddRecord(&g_replies[0], 3, "a longer first message");
    __AddRecord(&g_replies[0], 4, "second");
    g_replies[1].NextSequence = 5;
    g_replyCount = 2;

    CHECK(LogReaderShowNext(&reader, &count) == OS_EOK);
    CHECK(count == 2);
    CHECK(g_droppedShown == 3);
    CHECK(strcmp(&g_shown[0][0], "a longer first message") == 0);
    CHECK(strcmp(&g_shown[1][0], "second") == 0);
    CHECK(reader.NextSequence == 5);

    CHECK(LogReaderShowNext(&reader, &count) == OS_EOK);
    CHECK(count == 0);
    CHECK(g_readSequences[0] == 0 && g_readSequences[1] == 5);
    LogReaderDestroy(&reader);
    CHECK(reader.Buffer == NULL);
}

static void
__TestReadFailure(void)
{
    LogReader_t reader;
    uint32_t    count = 99;

    __ResetReader();
    CHECK(LogReaderCreate(&reader) == OS_EOK);
    reader.NextSequence = 12;
    g_replies[0].Status = OS_EPERMISSIONS;
    g_replyCount = 1;
    CHECK(LogReaderShowNext(&reader, &count) == OS_EPERMISSIONS);
    CHECK(count == 0);
    CHECK(reader.NextSequence == 12);
    LogReaderDestroy(&reader);
}

/**
 * @brief Reads one reply whose second record was damaged by damage(). The
 * first record must still be shown, and the reader must move past the batch.
 */
static void
__CheckMalformed(
    _In_ void (*damage)(struct __ReadReply*, OSKernelLogRecord_t*))
{
    LogReader_t          reader;
    OSKernelLogRecord_t* second;
    uint32_t             count;

    __ResetReader();
    CHECK(LogReaderCreate(&reader) == OS_EOK);
    __AddRecord(&g_replies[0], 0, "good");
    second = __AddRecord(&g_replies[0], 1, "damaged");
    damage(&g_replies[0], second);
    g_replyCount = 1;

    CHECK(LogReaderShowNext(&reader, &count) == OS_EPROTOCOL);
    CHECK(g_shownCount == 1 && strcmp(&g_shown[0][0], "good") == 0);
    CHECK(count == 1);
    CHECK(reader.NextSequence == 2);
    LogReaderDestroy(&reader);
}

static void
__SizeTooSmall(struct __ReadReply* reply, OSKernelLogRecord_t* record)
{
    record->Size = sizeof(OSKernelLogRecord_t) + record->Length - 1;
}

static void
__SizePastEnd(struct __ReadReply* reply, OSKernelLogRecord_t* record)
{
    record->Size += 8;
}

static void
__LengthTooLarge(struct __ReadReply* reply, OSKernelLogRecord_t* record)
{
    record->Length = OSKERNELLOG_MESSAGE_MAX + 1;
    record->Size   = OSKERNELLOG_RECORD_MAX + 8;
    reply->Length  = (uint32_t)(((uint8_t*)record - &reply->Data[0]) + record->Size);
    CHECK(reply->Length <= sizeof(reply->Data));
}

static void
__HeaderCutOff(struct __ReadReply* reply, OSKernelLogRecord_t* record)
{
    reply->Length = (uint32_t)(((uint8_t*)record - &reply->Data[0]) + 8);
}

static void
__TestMalformed(void)
{
    __CheckMalformed(__SizeTooSmall);
    __CheckMalformed(__SizePastEnd);
    __CheckMalformed(__LengthTooLarge);
    __CheckMalformed(__HeaderCutOff);
}

static LogReader_t*
__NewReader(void)
{
    LogReader_t* reader = malloc(sizeof(LogReader_t));

    CHECK(reader != NULL);
    CHECK(LogReaderCreate(reader) == OS_EOK);
    return reader;
}

static void
__TestJob(void)
{
    // A backlog is read again right away, and the job only sleeps once there
    // is nothing new. A failed read ends the job with an error, and the job
    // frees the reader (the leak checker fails the test otherwise).
    __ResetReader();
    __AddRecord(&g_replies[0], 0, "backlog");
    g_replies[1].NextSequence = 1;
    g_replies[2].Status = OS_EINVALPARAMS;
    g_replyCount = 3;
    LogReaderJob(__NewReader(), NULL);
    CHECK(g_readCount == 3);
    CHECK(g_yieldCount == 1);
    CHECK(g_sleepCount == 1);
    CHECK(g_drawCount == 2);
    CHECK(g_errorCount == 1);
    CHECK(strstr(&g_lastLog[0], "message 1") != NULL);

    // A cancelled job ends without reading.
    __ResetReader();
    g_cancelAfter = 0;
    LogReaderJob(__NewReader(), NULL);
    CHECK(g_readCount == 0);
    CHECK(g_errorCount == 0);

    // A failed sleep ends the job instead of spinning.
    __ResetReader();
    g_replyCount  = 1;
    g_sleepResult = -1;
    LogReaderJob(__NewReader(), NULL);
    CHECK(g_readCount == 1);
    CHECK(g_sleepCount == 1);
    CHECK(g_errorCount == 1);
}

int
main(void)
{
    __TestShowNext();
    __TestReadFailure();
    __TestMalformed();
    __TestJob();
    printf("syslogd reader tests passed\n");
    return 0;
}

#endif // SYSLOGD_TEST_READER
