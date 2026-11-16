// The host system ships an unrelated event.h that is found before the
// production header. The interrupt test needs the real event descriptor API.
#include "../../../librt/libc/include/event.h"
