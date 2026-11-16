// The shared test include path carries a stub that only forward declares the
// I/O set types. The interrupt test builds a real event, so it takes the
// production header instead.
#include "../../../librt/libc/include/ioset.h"
