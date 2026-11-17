#ifndef KEYCODES_H
#define KEYCODES_H

#include <stdint.h>

#include "class/hid/hid.h"

/* Special entries for keymap.h. 0x01-0x03 are HID error codes that never
 * appear as real keys, so they're free to reuse. */
#define KC_NO 0x00   /* nothing */
#define KC_TRNS 0x01 /* Fn layer: fall through to the base layer */
#define KC_FN 0x03   /* the Fn key itself */

#endif
