/*
 * Raspberry Pi GPIO assignment for the T1000LE LCD (connector PJ7).
 *
 * All pins must be in GPIO bank 0 (0..31) so a single SET/CLR register write
 * can change several at once. Physical header pin numbers are in comments.
 *
 * Every signal goes through a 74AHCT245 (or 74AHCT541) buffer powered from
 * the *switched* LCD 5V rail, see README.md.
 */
#ifndef T1K_PINS_H
#define T1K_PINS_H

/* Data bus: upper half of the panel (UDx) and lower half (LDx). */
#define PIN_UD0 16 /* header 36 -> PJ7-11 */
#define PIN_UD1 17 /* header 11 -> PJ7-12 */
#define PIN_UD2 18 /* header 12 -> PJ7-13 */
#define PIN_UD3 19 /* header 35 -> PJ7-14 */
#define PIN_LD0 20 /* header 38 -> PJ7-18 */
#define PIN_LD1 21 /* header 40 -> PJ7-19 */
#define PIN_LD2 22 /* header 15 -> PJ7-20 */
#define PIN_LD3 23 /* header 16 -> PJ7-21 */

/* Timing. */
#define PIN_SCP 24  /* header 18 -> PJ7-9  shift clock          */
#define PIN_LP 25   /* header 22 -> PJ7-8  line latch pulse     */
#define PIN_FP 26   /* header 37 -> PJ7-7  frame pulse          */
#define PIN_PVBL 27 /* header 13 -> PJ7-1  unknown, see --pvbl  */

/* Backlight control (":000" suffix in the manual = active low). */
#define PIN_CBLON 5 /* header 29 -> PJ7-3 */
#define PIN_CBLHI 6 /* header 31 -> PJ7-4 */
#define CBL_ACTIVE_LOW 1

/* Power switches on our adapter board (active high). */
#define PIN_VEE_EN 12   /* header 32: enables the negative VEE supply -> PJ7-16 */
#define PIN_LCD5V_EN 13 /* header 33: enables switched 5V           -> PJ7-15 */

#endif
