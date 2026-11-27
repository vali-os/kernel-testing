// DDK host intrinsics have external definitions on Windows. Use one translation
// unit here; the Unix runner continues to compile the production files separately.
#include "../../librt/libfdt/parser.c"
#include "../../services/deviced/firmware/tree/resources.c"
#include "../../services/deviced/firmware/tree/decode-log.c"
#include "../../services/deviced/firmware/tree/reader.c"
#include "../../services/deviced/firmware/tree/tree.c"
#include "reader_test.c"
