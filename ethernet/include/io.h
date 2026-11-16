// The shared test include path carries a stub that only declares write. The
// interrupt test also needs read and close, so it takes the production header.
#include "../../../librt/libc/include/io.h"
