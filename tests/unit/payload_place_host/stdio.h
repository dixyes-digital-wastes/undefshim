/* toml.h includes stdio unconditionally. Use the driver's FILE declarations,
 * but leave assert.h and the other host headers untouched for this test */
#ifndef US_PLACEMENT_HOST_STDIO_H
#define US_PLACEMENT_HOST_STDIO_H
#include <uefi.h>
#endif
