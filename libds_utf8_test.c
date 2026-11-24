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
 * libds UTF-8 decoding host test
 * - Built by libds_utf8_test.py with the mstring sources from librt/libds.
 * - Feeds valid and malformed UTF-8 to mstr_next and checks each character it
 *   returns and how far the index moves.
 * - Checks that mstr_len_u8, mstr_u8_to_internal and mstr_new_u8 agree with
 *   mstr_next, and that the end of a string is never taken for a character.
 * - Each input is copied to a heap buffer of the exact size, so ASan stops the
 *   test on any read past the zero terminator.
 */

#include <ds/mstring.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Not in ds/mstring.h. These are shared between the mstring sources through
// mstring/common/private.h.
extern size_t mstr_len_u8(const char* u8);
extern void   mstr_u8_to_internal(const char* u8, mchar_t* out);

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

#define CHECK_CASE(decodeCase, condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: case '%s': CHECK failed: %s\n", \
                __FILE__, __LINE__, (decodeCase)->Name, #condition); \
        exit(1); \
    } \
} while (0)

#define REPLACEMENT 0xFFFD

// The longest input below decodes to 21 characters.
#define MAX_STEPS 24

// One call to mstr_next: the character it must return and how many bytes the
// index must move. A Length of 0 marks the end of the list.
typedef struct DecodeStep {
    mchar_t Character;
    int     Length;
} DecodeStep_t;

// An input string and the characters mstr_next must return for it, in order.
typedef struct DecodeCase {
    const char*  Name;
    const char*  Bytes;
    DecodeStep_t Steps[MAX_STEPS];
} DecodeCase_t;

// A rejected byte: U+FFFD, and the index moves one byte.
#define BAD { REPLACEMENT, 1 }

static const DecodeCase_t g_cases[] = {
    // Valid characters of each length, including the first and last character
    // of each length.
    { "ascii", "Az~", { { 'A', 1 }, { 'z', 1 }, { '~', 1 } } },
    { "1 byte limit", "\x01\x7F", { { 0x01, 1 }, { 0x7F, 1 } } },
    { "2 bytes", "\xC3\xA9", { { 0xE9, 2 } } },
    { "2 byte limits", "\xC2\x80\xDF\xBF", { { 0x80, 2 }, { 0x7FF, 2 } } },
    { "3 bytes", "\xE2\x82\xAC", { { 0x20AC, 3 } } },
    { "3 byte limits", "\xE0\xA0\x80\xEF\xBF\xBF", { { 0x800, 3 }, { 0xFFFF, 3 } } },
    { "around surrogates", "\xED\x9F\xBF\xEE\x80\x80", { { 0xD7FF, 3 }, { 0xE000, 3 } } },
    { "replacement character", "\xEF\xBF\xBD", { { REPLACEMENT, 3 } } },
    { "4 bytes", "\xF0\x9F\x98\x80", { { 0x1F600, 4 } } },
    { "4 byte limits", "\xF0\x90\x80\x80\xF4\x8F\xBF\xBF", { { 0x10000, 4 }, { 0x10FFFF, 4 } } },
    { "mixed", "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80z",
      { { 'a', 1 }, { 0xE9, 2 }, { 0x20AC, 3 }, { 0x1F600, 4 }, { 'z', 1 } } },

    // Sequences cut off by the end of the string. Every byte is rejected on
    // its own, and nothing after the zero terminator is read.
    { "cut off 2 bytes", "\xC3", { BAD } },
    { "cut off 3 bytes", "\xE2\x82", { BAD, BAD } },
    { "cut off 4 bytes", "\xF0\x9F\x98", { BAD, BAD, BAD } },
    { "cut off after text", "ab\xF0\x9F", { { 'a', 1 }, { 'b', 1 }, BAD, BAD } },

    // Sequences cut off by a byte that is not a continuation byte. That byte
    // is decoded on its own.
    { "cut off by ascii", "\xE2\x82" "A", { BAD, BAD, { 'A', 1 } } },
    { "cut off by lead byte", "\xE2\xC3\xA9", { BAD, { 0xE9, 2 } } },

    // Continuation bytes without a lead byte, and long runs of them after a
    // lead byte. The old decoder read past its offset table on these.
    { "stray continuation", "\x80", { BAD } },
    { "stray after character", "\xC3\xA9\x80\xBF", { { 0xE9, 2 }, BAD, BAD } },
    { "long run after 2 byte lead", "\xC0\x80\x80\x80\x80\x80\x80\x80\x80\x80",
      { BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD } },
    { "long run after 2 byte character", "\xC3\xA9\x80\x80\x80\x80\x80\x80\x80\x80",
      { { 0xE9, 2 }, BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD } },
    { "long run after 4 byte lead",
      "\xF0\x9F\x98\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80",
      { { 0x1F600, 4 }, BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD,
        BAD, BAD, BAD, BAD } },
    { "long run after ascii", "a\xBF\xBF\xBF\xBF\xBF\xBF\xBF\xBF\xBF",
      { { 'a', 1 }, BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD } },

    // Overlong forms: a character encoded with more bytes than it needs.
    { "overlong zero", "\xC0\x80", { BAD, BAD } },
    { "overlong 2 bytes", "\xC1\xBF", { BAD, BAD } },
    { "overlong 3 bytes", "\xE0\x9F\xBF", { BAD, BAD, BAD } },
    { "overlong 3 byte zero", "\xE0\x80\x80", { BAD, BAD, BAD } },
    { "overlong 4 bytes", "\xF0\x8F\xBF\xBF", { BAD, BAD, BAD, BAD } },

    // Values above U+10FFFF, and lead bytes of the old 5 and 6 byte forms.
    { "above max", "\xF4\x90\x80\x80", { BAD, BAD, BAD, BAD } },
    { "highest 4 byte form", "\xF7\xBF\xBF\xBF", { BAD, BAD, BAD, BAD } },
    { "5 byte form", "\xF8\x88\x80\x80\x80", { BAD, BAD, BAD, BAD, BAD } },
    { "6 byte form", "\xFC\x84\x80\x80\x80\x80", { BAD, BAD, BAD, BAD, BAD, BAD } },
    { "FE and FF", "\xFE\xFF" "a", { BAD, BAD, { 'a', 1 } } },

    // Surrogates are reserved for UTF-16 and are not characters.
    { "first surrogate", "\xED\xA0\x80", { BAD, BAD, BAD } },
    { "last surrogate", "\xED\xBF\xBF", { BAD, BAD, BAD } },
};

// Returns a heap copy of the string with no room after the zero terminator.
static char*
__ExactCopy(
    const char* text)
{
    size_t size = strlen(text) + 1;
    char*  copy = malloc(size);

    CHECK(copy != NULL);
    memcpy(copy, text, size);
    return copy;
}

static int
__StepCount(
    const DecodeCase_t* decodeCase)
{
    int count = 0;

    while (count < MAX_STEPS && decodeCase->Steps[count].Length != 0) {
        count++;
    }
    return count;
}

static void
__CheckNext(
    const DecodeCase_t* decodeCase,
    const char*         text)
{
    int     stepCount = __StepCount(decodeCase);
    int     index     = 0;
    mchar_t character;

    for (int i = 0; i < stepCount; i++) {
        int before = index;

        character = mstr_next(text, &index);
        CHECK_CASE(decodeCase, character == decodeCase->Steps[i].Character);
        CHECK_CASE(decodeCase, index == before + decodeCase->Steps[i].Length);
    }
    CHECK_CASE(decodeCase, (size_t)index == strlen(text));

    // At the zero terminator, every call returns MSTRING_EOS and leaves the
    // index where it is.
    for (int i = 0; i < 3; i++) {
        character = mstr_next(text, &index);
        CHECK_CASE(decodeCase, character == MSTRING_EOS);
        CHECK_CASE(decodeCase, (size_t)index == strlen(text));
    }
}

static void
__CheckConversions(
    const DecodeCase_t* decodeCase,
    const char*         text)
{
    int        stepCount = __StepCount(decodeCase);
    mchar_t*   characters;
    mstring_t* string;

    CHECK_CASE(decodeCase, mstr_len_u8(text) == (size_t)stepCount);

    // Exactly sized, so ASan stops the test if more characters are written
    // than mstr_len_u8 counted.
    characters = malloc((size_t)stepCount * sizeof(mchar_t));
    CHECK(characters != NULL);
    mstr_u8_to_internal(text, characters);
    for (int i = 0; i < stepCount; i++) {
        CHECK_CASE(decodeCase, characters[i] == decodeCase->Steps[i].Character);
    }
    free(characters);

    string = mstr_new_u8(text);
    CHECK(string != NULL);
    CHECK_CASE(decodeCase, mstr_len(string) == (size_t)stepCount);
    for (int i = 0; i < stepCount; i++) {
        CHECK_CASE(decodeCase, string->__data[i] == decodeCase->Steps[i].Character);
    }
    CHECK_CASE(decodeCase, mstr_cmp_u8(string, text) == 0);
    mstr_delete(string);
}

static void
__TestCases(void)
{
    for (size_t i = 0; i < sizeof(g_cases) / sizeof(g_cases[0]); i++) {
        char* text = __ExactCopy(g_cases[i].Bytes);

        // Keeps MAX_STEPS large enough. A full list has no end marker.
        CHECK_CASE(&g_cases[i], __StepCount(&g_cases[i]) < MAX_STEPS);
        __CheckNext(&g_cases[i], text);
        __CheckConversions(&g_cases[i], text);
        free(text);
    }
}

static void
__TestInvalidParameters(void)
{
    char* text  = __ExactCopy("a");
    int   index = 0;

    CHECK(mstr_next(NULL, &index) == MSTRING_EOS);
    CHECK(index == 0);
    CHECK(mstr_next(text, NULL) == MSTRING_EOS);
    CHECK(mstr_len_u8(NULL) == 0);
    free(text);

    // An empty string has no characters.
    text = __ExactCopy("");
    CHECK(mstr_next(text, &index) == MSTRING_EOS);
    CHECK(index == 0);
    CHECK(mstr_len_u8(text) == 0);
    free(text);
}

static void
__TestEndIsNotReplacement(void)
{
    char*      shorter = __ExactCopy("a");
    mstring_t* string;

    // The end of a string must not look like a rejected byte. "a\x80" decodes
    // to 'a' and U+FFFD, and the end of "a" must not match that U+FFFD.
    CHECK(MSTRING_EOS != REPLACEMENT);
    string = mstr_new_u8("a\x80");
    CHECK(string != NULL);
    CHECK(mstr_len(string) == 2);
    CHECK(mstr_cmp_u8(string, shorter) != 0);
    mstr_delete(string);
    free(shorter);
}

int main(void)
{
    __TestCases();
    __TestInvalidParameters();
    __TestEndIsNotReplacement();
    printf("libds UTF-8 tests passed\n");
    return 0;
}
