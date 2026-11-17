/* Pico GPIO wiring to the T1000LE keyboard connector (PJ8). */
#ifndef MATRIX_PINS_H
#define MATRIX_PINS_H

#define N_SCAN 11 /* KBSC0..10: driven low one at a time */
#define N_RTN 8   /* KBRTN0..7: read back, pulled up     */

/*                                KBSC0 .. KBSC10  ->  PJ8 pins 1..11 */
static const unsigned scan_pins[N_SCAN] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
/*                                KBRTN0 .. KBRTN7 ->  PJ8 pins 12..19 */
static const unsigned rtn_pins[N_RTN] = {11, 12, 13, 14, 15, 16, 17, 18};

#endif
