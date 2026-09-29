/*
 * tilink.c - Voyager 200 side of the Arduino raw link demo.
 * Build with TIGCC or GCC4TI:   tigcc -Os -o tilink tilink.c
 * (produces tilink.v2z for the Voyager 200; change USE_V200 to USE_TI89
 *  or USE_TI92PLUS for those models)
 *
 * Keys:
 *   F1  send "HELLO"
 *   F2  send "A0?"     (Arduino replies with its analog reading)
 *   F3  send "COUNT n"
 *   ESC quit
 * Anything the Arduino sends is printed on screen.
 *
 * Frame format: 0x7E | len | payload | checksum (sum of payload, mod 256)
 */

#define USE_V200
#define SAVE_SCREEN
#define MIN_AMS 200

#include <tigcclib.h>

#define FRAME_START 0x7E
#define MAX_PAYLOAD 120

static unsigned char rxBuf[MAX_PAYLOAD + 1];
static short rxState = 0;
static unsigned char rxLen, rxPos, rxSum;

/* ---- sending ---- */

static void sendText(const char *msg)
{
    unsigned short len = strlen(msg);
    unsigned char hdr[2], sum = 0;
    unsigned short i;

    if (len == 0) return;
    if (len > MAX_PAYLOAD) len = MAX_PAYLOAD;
    for (i = 0; i < len; i++) sum += (unsigned char)msg[i];

    hdr[0] = FRAME_START;
    hdr[1] = (unsigned char)len;

    if (LIO_SendData(hdr, 2) || LIO_SendData(msg, len) || LIO_SendData(&sum, 1)) {
        printf("! send error\n");
        OSLinkReset();
        OSLinkOpen();
        return;
    }
    printf("> %s\n", msg);
}

/* ---- receiving (non-blocking, byte-by-byte state machine) ---- */

static void pollLink(void)
{
    unsigned char c;

    while (OSReadLinkBlock((char *)&c, 1) == 1) {
        switch (rxState) {
        case 0: /* waiting for start byte */
            if (c == FRAME_START) rxState = 1;
            break;
        case 1: /* length */
            if (c == 0 || c > MAX_PAYLOAD) { rxState = 0; break; }
            rxLen = c; rxPos = 0; rxSum = 0; rxState = 2;
            break;
        case 2: /* payload */
            rxBuf[rxPos++] = c;
            rxSum += c;
            if (rxPos == rxLen) rxState = 3;
            break;
        case 3: /* checksum */
            if (c == rxSum) {
                rxBuf[rxLen] = 0;
                printf("< %s\n", rxBuf);
            } else {
                printf("! bad checksum\n");
            }
            rxState = 0;
            break;
        }
    }
}

/* Wait for a key to be released, still servicing the link. */
static void waitRelease(short row, short col)
{
    while (_keytest(row, col)) pollLink();
}

void _main(void)
{
    char msg[32];
    short counter = 0;

    clrscr();
    printf("TI <-> Arduino link\n");
    printf("F1 HELLO  F2 A0?  F3 COUNT  ESC quit\n\n");

    OSLinkReset();
    OSLinkOpen();

    /* _keytest reads the keyboard matrix directly, so unlike ngetchx/kbhit
       it never lets the OS grab our raw bytes as link packets. */
    while (!_keytest(RR_ESC)) {
        pollLink();

        if (_keytest(RR_F1)) {
            sendText("HELLO");
            waitRelease(RR_F1);
        } else if (_keytest(RR_F2)) {
            sendText("A0?");
            waitRelease(RR_F2);
        } else if (_keytest(RR_F3)) {
            sprintf(msg, "COUNT %d", counter++);
            sendText(msg);
            waitRelease(RR_F3);
        }
    }

    while (_keytest(RR_ESC)) ;
    OSLinkClose();
    GKeyFlush();
}
