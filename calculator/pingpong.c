/*
 * pingpong.c - Voyage 200 side of the ESP32 ping-pong demo.
 * Build with TIGCC or GCC4TI:   tigcc -Os -o pingpong pingpong.c
 * (makes pingpong.v2z; change USE_V200 to USE_TI89 / USE_TI92PLUS for those models)
 * Run from the Home screen:     pingpong()
 *
 * Keys:
 *   F1   send one ping
 *   F2   toggle auto mode (ping continuously, as fast as the link allows)
 *   F3   show statistics
 *   ESC  quit
 * Pings coming from the ESP32 are answered automatically.
 *
 * Frame format: 0x7E | len | payload | checksum (sum of payload, mod 256)
 */

#define USE_V200
#define SAVE_SCREEN
#define MIN_AMS 200

#include <tigcclib.h>

#define FRAME_START  0x7E
#define MAX_PAYLOAD  120
#define PONG_TIMEOUT 20      /* timer ticks of 1/20 s -> 1 second */

static unsigned char rxBuf[MAX_PAYLOAD + 1];
static short rxState = 0;
static unsigned char rxLen, rxPos, rxSum;

static unsigned long seq = 0, sent = 0, ok = 0, lost = 0, answered = 0, bad = 0;
static short waiting = 0, autoMode = 0;
static unsigned long waitSeq = 0;

/* ---------- sending ---------- */

static short sendText(const char *msg)
{
    unsigned short len = strlen(msg);
    unsigned char hdr[2], sum = 0;
    unsigned short i;

    if (len == 0) return 1;
    if (len > MAX_PAYLOAD) len = MAX_PAYLOAD;
    for (i = 0; i < len; i++) sum += (unsigned char)msg[i];

    hdr[0] = FRAME_START;
    hdr[1] = (unsigned char)len;

    if (LIO_SendData(hdr, 2) || LIO_SendData(msg, len) || LIO_SendData(&sum, 1)) {
        printf("! send error\n");
        OSLinkReset();
        OSLinkOpen();
        return 1;
    }
    return 0;
}

static void sendPing(void)
{
    char msg[24];

    seq++;
    sprintf(msg, "PING %lu", seq);
    if (sendText(msg)) return;

    sent++;
    waiting = 1;
    waitSeq = seq;
    OSFreeTimer(USER_TIMER);
    OSRegisterTimer(USER_TIMER, PONG_TIMEOUT);
    if (!autoMode) printf("> %s\n", msg);
}

static void showStats(void)
{
    printf("sent %lu ok %lu lost %lu\n", sent, ok, lost);
    printf("answered %lu bad %lu\n", answered, bad);
}

/* ---------- handling a received frame ---------- */

static void handleFrame(const char *msg)
{
    if (!strncmp(msg, "PING ", 5)) {
        char reply[MAX_PAYLOAD + 1];
        sprintf(reply, "PONG %s", msg + 5);
        if (!sendText(reply)) answered++;
        printf("< %s -> pong\n", msg);

    } else if (!strncmp(msg, "PONG ", 5)) {
        unsigned long n = atol(msg + 5);
        if (waiting && n == waitSeq) {
            unsigned long ticks = PONG_TIMEOUT - OSTimerCurVal(USER_TIMER);
            waiting = 0;
            ok++;
            if (!autoMode)
                printf("< %s ~%lu ms\n", msg, ticks * 50);
            else if (ok % 50 == 0)
                showStats();
        } else {
            printf("< stray %s\n", msg);
        }

    } else {
        printf("< %s\n", msg);
    }
}

/* ---------- receiving (non-blocking byte state machine) ---------- */

static void pollLink(void)
{
    unsigned char c;

    while (OSReadLinkBlock((char *)&c, 1) == 1) {
        switch (rxState) {
        case 0:
            if (c == FRAME_START) rxState = 1;
            break;
        case 1:
            if (c == 0 || c > MAX_PAYLOAD) { rxState = 0; bad++; break; }
            rxLen = c; rxPos = 0; rxSum = 0; rxState = 2;
            break;
        case 2:
            rxBuf[rxPos++] = c;
            rxSum += c;
            if (rxPos == rxLen) rxState = 3;
            break;
        case 3:
            rxState = 0;
            if (c == rxSum) {
                rxBuf[rxLen] = 0;
                handleFrame((const char *)rxBuf);
            } else {
                bad++;
                printf("! bad checksum\n");
            }
            break;
        }
    }
}

/* Wait for a key to be released while still servicing the link. */
static void waitRelease(short row, short col)
{
    while (_keytest(row, col)) pollLink();
}

void _main(void)
{
    clrscr();
    printf("V200 <-> ESP32 ping-pong\n");
    printf("F1 ping  F2 auto  F3 stats  ESC quit\n\n");

    OSLinkReset();
    OSLinkOpen();

    /* _keytest reads the keyboard directly; ngetchx/kbhit would let the
       OS grab our raw bytes as link packets. */
    while (!_keytest(RR_ESC)) {
        pollLink();

        if (waiting && OSTimerExpired(USER_TIMER)) {
            waiting = 0;
            lost++;
            printf("! PING %lu lost\n", waitSeq);
        }

        if (autoMode && !waiting) sendPing();

        if (_keytest(RR_F1)) {
            if (!waiting) sendPing();
            waitRelease(RR_F1);
        } else if (_keytest(RR_F2)) {
            autoMode = !autoMode;
            printf(autoMode ? "auto ON\n" : "auto OFF\n");
            if (!autoMode) showStats();
            waitRelease(RR_F2);
        } else if (_keytest(RR_F3)) {
            showStats();
            waitRelease(RR_F3);
        }
    }

    while (_keytest(RR_ESC)) ;
    OSFreeTimer(USER_TIMER);
    OSLinkClose();
    GKeyFlush();
}
