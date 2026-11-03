// These unrelated service paths must not run in the enumeration fixture.
#include <stdlib.h>
void GetGrachtClient(void) { abort(); }
void ctt_usbhost_configure_hub(void) { abort(); }
void ctt_usbhost_configure_hub_result(void) { abort(); }
void gracht_client_await(void) { abort(); }
