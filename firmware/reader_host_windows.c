// DDK host intrinsics have external definitions on Windows. Use one translation
// unit here; the Unix runner continues to compile the production files separately.
#include "../../librt/libfdt/parser.c"
#include "../../services/deviced/firmware/resources.c"
#include "../../services/deviced/firmware/fdt.c"
#include "reader_test.c"
