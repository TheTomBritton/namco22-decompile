/*
 * tc2_link.c -- THE TWIN-CABINET LINK of Time Crisis 2: which cabinet this is (the Link ID), the C422 RS-422 communication controller
 * as our own model, and online play (src/eng/net.c, NMN2 through zonesync.net or the built-in LAN host) carrying it.
 *
 * THE CABINET (Link ID). The game is built for two cabinets side by side, Left/Red and Right/Blue; the I/O board tells it which one it is
 * (MAME 0.289 timecrs2 "Link ID", JVS_PLAYER1 0x4000 = BUTTON4 = I/O board 0x6001 bit 2, src/tc2_h8.c io_switches). The menu's Cabinet
 * page chooses it (`link_id` in tc2.cfg; $TC2_LINKID=0/1 for tests); in an online session the slot decides (slot 0 Left/Red, 1 Right/Blue).
 *
 * THE C422 (MAME: "RS422 Networking (Twin Cabinet) Communication Controller", registers 0x06400000-0x0640000F, RAM 0x06200000-0x06203FFF;
 * MAME keeps both as plain memory: "LAN unemulated"). Its behaviour here is read off the game's own driver, 0x8000BB6C..0x8000C3A0:
 *   +0 STATUS   bit 1 = a frame has been received, bit 2 = the transmit block is done (the interrupt handler 0x8000BEAC tests both; the
 *               receive handler 0x8000BD24 clears bit 1 by writing it back)
 *   +2 MASK     0 bits enable the status bits' interrupt (reset 0x00FF; the driver clears bits 1/2). POWER-ON TEST: 0xFFFB raises the
 *               interrupt, 0x000F acknowledges it (MAME c422_irq_w; kept as it was)
 *   +6 CONTROL  2 = transmit, 3 = stop / set up; any write clears status bit 2
 *   +A COUNT    words left to transmit (the driver writes at most 0xFF a block and reads the low byte as "still busy")
 *   +C RX PTR   index (0..0xFFF) of the last word written into the receive ring
 *   +E TX ADDR  word index in the C422 RAM of the next word to transmit (the driver writes it once a packet; the chip steps it)
 *   RAM 0x0000-0x1FFF  four transmit slots of 0x400 words ((0x802F75CA + 0x400) & 0xC00): word 0/1 = length, then the packet
 *   RAM 0x2000-0x3FFF  the receive ring: 4096 words, the byte in bits 0-7, bit 8 = the last word of a frame
 * A packet on the wire (0x8000BF38): 0x5A, seq hi, seq lo, a status byte, a rotating status byte, the payload (0x802F3910 bytes from
 * 0x802F3912), a checksum (the negated sum), length hi, length lo | 0x100. The driver sends each frame's packet up to four times
 * (0x802F75C6 = 3 resends); the receiver keeps a 16-entry queue of the frames it found and checks each one's 0x5A and checksum.
 * The model is ARMED only while a link is up (an online session, or $TC2_LINK for a direct test pair): unarmed, the registers stay plain
 * memory exactly as MAME has them, so single-player runs are unchanged.
 *
 * ON THE NETWORK. Every word the chip transmits is kept; each game frame the complete frames sent since the last one go out as ONE
 * payload (tc2_net_*), the peer's arrive into a queue and are written into the receive ring one frame per interrupt, the next only
 * after the game has acknowledged the last. Stall-and-wait keeps the two machines within a few frames of each other.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "tc2_game.h"
#include "eng/eng_cfg.h"
extern uint32_t rr_frame;

/* ---------------- the cabinet (Link ID) ---------------- */
static int link_id_cfg = -1;                   /* the menu's choice (-1 = not loaded yet) */
static int link_id_forced = -1;                /* an online session's slot, -1 = none */
int tc2_link_id(void)
{
    if (!tc2_game.twin) return 0;
    if (link_id_forced >= 0) return link_id_forced;
    { static long flip = -2; if (flip == -2) { const char *e = getenv("TC2_LINKID_FLIP"); flip = e ? atol(e) : -1; }   /* tests: flip at that frame */
      if (flip > 0 && rr_frame >= (uint32_t)flip) { flip = -1; link_id_cfg = !tc2_link_id(); fprintf(stderr, "[LINK] f%u Link ID flipped to %d\n", rr_frame, link_id_cfg); } }
    if (link_id_cfg < 0) {
        const char *e = getenv("TC2_LINKID");
        link_id_cfg = e ? (atoi(e) != 0) : (eng_cfg_int("link_id", 0) != 0);
    }
    return link_id_cfg;
}
void tc2_link_id_set(int id) { link_id_cfg = id != 0; eng_cfg_set_int("link_id", link_id_cfg); }
void tc2_link_id_force(int id) { link_id_forced = id; }

/* ---------------- the C422 ---------------- */
void tc2_irq_set(int src, int on);
uint64_t tc2_cycles_now(void);
uint8_t *tc2_vram_ptr(uint32_t p);
extern uint32_t rr_frame;

static uint16_t reg[8];                        /* +0 .. +E */
static int armed;                              /* the active model (a link is up); 0 = plain registers, as MAME */
static int post_irq;                           /* the POWER-ON TEST's raise (0xFFFB to +2) */
static int tx_busy; static uint64_t tx_done_at;
static int dbg = -1;
static uint16_t ram_rd(uint32_t word) { const uint8_t *p = tc2_vram_ptr(0x06200000u + (word & 0x1FFFu) * 2); return (uint16_t)(p[0] << 8 | p[1]); }
static void ram_wr(uint32_t word, uint16_t v) { uint8_t *p = tc2_vram_ptr(0x06200000u + (word & 0x1FFFu) * 2); p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* the transmitted words (bit 8 = a frame's last word): whole frames wait here for the transport */
#define TXQ 8192
static uint16_t txq[TXQ]; static int txq_n;    /* complete frames only, back to back */
static uint16_t cur[1024]; static int cur_n;   /* the frame being transmitted */
/* received frames, waiting for the ring */
#define RXQ 8192
static uint16_t rxq[RXQ]; static int rxq_n;

static void irq_update(void)
{
    const int on = post_irq || ((reg[0] & ~reg[1] & 6) != 0);
    tc2_irq_set(TC2_SRC_C422, on);
}
static uint64_t cyc_per_word(void)              /* the line's speed: 11-bit characters at $TC2_LINK_BAUD (default 2 Mbit/s) */
{
    static uint64_t c; if (!c) { const char *e = getenv("TC2_LINK_BAUD"); const double baud = e ? atof(e) : 2e6; c = (uint64_t)(tc2_game.clock * 11.0 / (baud > 1000 ? baud : 2e6)); if (!c) c = 1; }
    return c;
}
static void tx_word(uint16_t w)
{
    if (cur_n < (int)(sizeof cur / sizeof cur[0])) cur[cur_n++] = (uint16_t)(w & 0x1FF);
    if (w & 0x100) {                             /* the frame's last word */
        if (txq_n + cur_n <= TXQ) { memcpy(txq + txq_n, cur, (size_t)cur_n * 2); txq_n += cur_n; }
        else if (dbg) fprintf(stderr, "[LINK] f%u transmit queue full: a frame of %d words dropped\n", rr_frame, cur_n);
        cur_n = 0;
    }
}
static void tx_start(void)
{
    const int n = reg[5] & 0xFF;
    for (int i = 0; i < n; i++) tx_word(ram_rd(reg[7] + (uint32_t)i));
    reg[7] = (uint16_t)(reg[7] + n);
    tx_busy = 1; tx_done_at = tc2_cycles_now() + (uint64_t)(n ? n : 1) * cyc_per_word();
}

int tc2_c422_read(unsigned k, uint32_t *v)
{
    *v = reg[k & 7];
    return 1;
}
void tc2_c422_write(unsigned k, uint16_t v)
{
    k &= 7;
    reg[k] = v;
    if (k == 1 && (v == 0xFFFBu || v == 0x000Fu)) { post_irq = v == 0xFFFBu; tc2_irq_set(TC2_SRC_C422, post_irq || (armed && (reg[0] & ~reg[1] & 6))); return; }
    if (!armed) return;                          /* plain registers, as MAME */
    if (k == 3) {                                /* a write to CONTROL acknowledges "transmit done" (the driver writes it before every
                                                  * block and when the packet is over; it never clears status bit 2 itself) */
        reg[0] &= (uint16_t)~4;
        if (v == 2 && !tx_busy) tx_start();
    }
    irq_update();
}

/* the receive ring: one queued frame at a time, the next once the game has taken the last (status bit 1 clear) */
static void rx_deliver(void)
{
    if (!rxq_n || (reg[0] & 2)) return;
    int n = 0; while (n < rxq_n && !(rxq[n] & 0x100)) n++;
    if (n == rxq_n) { rxq_n = 0; return; }       /* no end marker: not a frame */
    n++;
    uint16_t p = reg[6];
    for (int i = 0; i < n; i++) { p = (uint16_t)((p + 1) & 0xFFF); ram_wr(0x1000u + p, rxq[i]); }
    reg[6] = p;
    memmove(rxq, rxq + n, (size_t)(rxq_n - n) * 2); rxq_n -= n;
    reg[0] |= 2;
    irq_update();
}

/* time passed (src/tc2_main.c tc2_host_step) */
void tc2_link_step(void)
{
    if (!armed) return;
    if (tx_busy && tc2_cycles_now() >= tx_done_at) { tx_busy = 0; reg[5] &= 0xFF00; reg[0] |= 4; irq_update(); }
    rx_deliver();
}

/* the transport's side */
void tc2_link_arm(int on)
{
    if (dbg < 0) dbg = getenv("TC2_LINKLOG") != NULL;
    if (armed == (on != 0)) return;
    armed = on != 0; tx_busy = 0; txq_n = cur_n = 0; rxq_n = 0;
    if (dbg) fprintf(stderr, "[LINK] f%u C422 %s\n", rr_frame, armed ? "ARMED (the link is up)" : "unarmed (plain registers)");
    irq_update();
}
int tc2_link_armed(void) { return armed; }
/* the frames transmitted since the last call, back to back (bit 8 ends each); returns the word count */
int tc2_link_take(uint16_t *out, int max)
{
    int n = 0;
    while (n < txq_n) {                          /* whole frames that fit */
        int e = n; while (e < txq_n && !(txq[e] & 0x100)) e++;
        if (e >= txq_n || e + 1 > max) break;
        n = e + 1;
    }
    memcpy(out, txq, (size_t)n * 2);
    memmove(txq, txq + n, (size_t)(txq_n - n) * 2); txq_n -= n;
    return n;
}
void tc2_link_give(const uint16_t *w, int n)
{
    if (rxq_n + n > RXQ) { if (dbg) fprintf(stderr, "[LINK] f%u receive queue full: %d words dropped\n", rr_frame, n); return; }
    memcpy(rxq + rxq_n, w, (size_t)n * 2); rxq_n += n;
}
void tc2_c422_load(unsigned k, uint16_t v) { reg[k & 7] = v; }   /* a MAME snapshot's registers (src/tc2_chips.c) */

/* ---------------- online: the NMN2 client carries the link (src/eng/net.c, ../docs/NETPLAY.md) ----------------
 * The PAYLOAD (TC2_FRAME_SIZE bytes, every frame): byte 0 = the number of link frames k, then k times { BE16 id, BE16 n, n bytes },
 * oldest first -- one byte a word, bit 8 (end of frame) being the last word's. A packet the driver resends is carried once. The sender
 * keeps its recent link frames numbered; each payload carries the newest ones that fit, which repeats the last frame's too -- a lost
 * datagram costs nothing -- and the receiver hands the game only the ids it has not had yet. A gameplay packet is ~250 bytes (measured:
 * 242 payload bytes + 8), so the payload is 1 KB: the server needs Time Crisis 2 ("t2") in its table (server/src/lib.rs GAMES).
 *   TC2_NET_SERVER=host[:port] [TC2_NET_NAME=x]   connect at boot (headless too)     TC2_NET_HOST=1   host a LAN game (built-in server)
 *   TC2_NET_DISCOVER=1                             join the first LAN host found       TC2_NET_AUTOSTART=N (src/eng/net.c)
 *   TC2_NET_LEAD=n   stall-and-wait lead (default auto: 2 + the ping in frames)       TC2_LINKLOG=1   [LINK] lines */
#include "eng/net.h"
#define TC2_FRAME_SIZE 1024
#define HIST 64
static struct { uint16_t id; int n; uint16_t w[1024]; } hist[HIST];   /* our transmitted link frames, by id % HIST */
static uint16_t next_id, rx_last; static int rx_have, net_on, in_session;
static unsigned long n_tx_frames, n_tx_dup, n_rx_frames, n_rx_dup;
static void hint(const char *t, int frames) { void eng_ui_set_hint(const char *, int); eng_ui_set_hint(t, frames); }

static void session_begin(int slot, int players)
{
    if (slot < 0 || slot > 1) { fprintf(stderr, "[TC2] online: slot %d -- Time Crisis 2 links two cabinets; staying alone\n", slot); return; }
    tc2_link_id_force(slot);
    rx_have = 0; next_id = 0; in_session = 1;
    tc2_link_arm(1);
    fprintf(stderr, "[TC2] online: cabinet %s (slot %d of %d) -- the link is up\n", slot ? "Right/Blue" : "Left/Red", slot, players);
    hint(slot ? "Linked: you are the RIGHT / BLUE cabinet" : "Linked: you are the LEFT / RED cabinet", 300);
}
static void session_end(void)
{
    in_session = 0; tc2_link_arm(0); tc2_link_id_force(-1);
    fprintf(stderr, "[TC2] online: session over (link frames out %lu, in %lu, repeats %lu)\n", n_tx_frames, n_rx_frames, n_rx_dup);
    hint("The link is down: playing alone", 300);
}
static int frame_out(uint8_t *buf)
{
    static uint16_t w[TXQ];
    for (int n; (n = tc2_link_take(w, (int)(sizeof w / sizeof w[0]))) > 0; ) {   /* this frame's transmitted frames into the history */
        for (int s = 0; s < n; ) {
            int e = s; while (e < n && !(w[e] & 0x100)) e++;
            const int len = e - s + 1;
            const int prev = (uint16_t)(next_id - 1) % HIST;     /* the driver sends each packet up to four times: once is enough here */
            const int dup = n_tx_frames && hist[prev].n == len && !memcmp(hist[prev].w, w + s, (size_t)len * 2);
            if (len <= 1024 && !dup) { hist[next_id % HIST].id = next_id; hist[next_id % HIST].n = len; memcpy(hist[next_id % HIST].w, w + s, (size_t)len * 2); next_id++; n_tx_frames++; }
            else if (dup) n_tx_dup++;
            s = e + 1;
        }
    }
    memset(buf, 0, TC2_FRAME_SIZE);
    int k = 0, bytes = 1;                          /* the newest frames that fit, counted backwards */
    while (k < HIST && (unsigned long)k < n_tx_frames && k < 255) {
        const int sz = 4 + hist[(uint16_t)(next_id - 1 - k) % HIST].n;
        if (bytes + sz > TC2_FRAME_SIZE) break;
        bytes += sz; k++;
    }
    buf[0] = (uint8_t)k; int o = 1;
    for (int i = k - 1; i >= 0; i--) {
        const uint16_t id = (uint16_t)(next_id - 1 - i); const int h = id % HIST;
        buf[o++] = (uint8_t)(id >> 8); buf[o++] = (uint8_t)id; buf[o++] = (uint8_t)(hist[h].n >> 8); buf[o++] = (uint8_t)hist[h].n;
        for (int j = 0; j < hist[h].n; j++) buf[o++] = (uint8_t)hist[h].w[j];   /* the byte; bit 8 is set on the last word only */
    }
    return 1;
}
static void frame_in(int slot, const uint8_t *buf, uint32_t seq)
{
    (void)seq;
    if (!in_session || slot == eng_net_slot()) return;
    const int k = buf[0]; int o = 1;
    for (int i = 0; i < k; i++) {
        if (o + 4 > TC2_FRAME_SIZE) return;
        const uint16_t id = (uint16_t)(buf[o] << 8 | buf[o + 1]); const int n = buf[o + 2] << 8 | buf[o + 3]; o += 4;
        if (n < 1 || n > 1024 || o + n > TC2_FRAME_SIZE) return;
        if (!rx_have || (int16_t)(id - rx_last) > 0) {
            uint16_t w[1024];
            for (int j = 0; j < n; j++) w[j] = buf[o + j];
            w[n - 1] |= 0x100;
            tc2_link_give(w, n); rx_last = id; rx_have = 1; n_rx_frames++;
        } else n_rx_dup++;
        o += n;
    }
}
static eng_net_game net_game = {
    "t2", 1, 2, TC2_FRAME_SIZE, "TC2_NET", "game", 0,
    session_begin, session_end, frame_out, frame_in, hint,
};

/* once at start-up, after the cfg is loaded: the client (inert until the menu or $TC2_NET_* connects it) */
void tc2_link_init(void)
{
    if (!tc2_game.twin || net_on) return;
    if (dbg < 0) dbg = getenv("TC2_LINKLOG") != NULL;
    eng_net_init(&net_game); net_on = 1;
    const char *srv = getenv("TC2_NET_SERVER"), *hst = getenv("TC2_NET_HOST"), *dsc = getenv("TC2_NET_DISCOVER"), *nm = getenv("TC2_NET_NAME");
    if (nm && *nm) eng_net_set_name(nm);
    if (hst && *hst == '1') eng_net_host_start();
    else if (dsc && *dsc == '1') { eng_net_discover_autojoin(1); eng_net_discover(); }
    else if (srv && *srv && eng_net_set_server(srv)) eng_net_connect();
}
/* once a game frame (src/tc2_main.c tc2_on_vblank): the client, then stall-and-wait so neither cabinet runs ahead of the other */
extern uint8_t tc2_ram[0x1000000];
static unsigned ram16(uint32_t a) { return (unsigned)(tc2_ram[a & 0xFFFFFF] << 8 | tc2_ram[(a + 1) & 0xFFFFFF]); }
void tc2_link_frame(void)
{
    if (dbg > 0 && rr_frame % 60 == 0)                /* the game's link state: payload length, state, silence count, resends left; the C422 */
        fprintf(stderr, "[LINK] f%u len %u state %u silent %u resend %d seq %u | C422 st %04X mask %04X ctl %04X cnt %04X rx %04X tx %04X%s\n", rr_frame,
                ram16(0x2F3910), tc2_ram[0x2F3502], ram16(0x2F3504), (int16_t)ram16(0x2CEDE6), ram16(0x2CEDD8),
                reg[0], reg[1], reg[3], reg[5], reg[6], reg[7], armed ? " ARMED" : "");
    if (!net_on) return;
    eng_net_poll();
    if (in_session) {
        static int lead = -2; if (lead == -2) { const char *e = getenv("TC2_NET_LEAD"); lead = e ? atoi(e) : -1; }
        if (lead < 1000) eng_net_wait(lead < 0 ? eng_net_auto_lead() : lead, 1000);
        if (dbg && rr_frame % 600 == 0) fprintf(stderr, "[LINK] f%u link frames out %lu (+%lu resends), in %lu (repeats %lu), ping %d ms\n", rr_frame, n_tx_frames, n_tx_dup, n_rx_frames, n_rx_dup, eng_net_ping_ms());
    }
}
void tc2_link_paused(void) { if (net_on) eng_net_poll_paused(); }   /* the menu is open: keep the session alive, send nothing */

/* ---------------- the menu: Cabinet ---------------- */
#include "eng/eng_ui.h"
static int cab_rows(void) { return 1; }
static bool cab_enabled(int r) { (void)r; return link_id_forced < 0; }
static void cab_text(int r, char *l, size_t ln, char *v, size_t vn)
{
    (void)r; snprintf(l, ln, "Cabinet (player)");
    snprintf(v, vn, "%s%s", tc2_link_id() ? "Right / Blue (player 2)" : "Left / Red (player 1)", link_id_forced >= 0 ? " -- set by the online session" : "");
}
static void cab_change(int r, int dir) { (void)r; (void)dir; if (link_id_forced < 0) tc2_link_id_set(!tc2_link_id()); }
static void cab_notes(void (*line)(const char *, ...))
{
    line("Two cabinets side by side: Left/Red (Richard)");
    line("and Right/Blue (Keith). Alone, play either one;");
    line("it takes effect at the next game. Online, the");
    line("session picks the side (Online page).");
}
static const eng_ui_page cab_page = { "Cabinet", 420, 140, 0, cab_rows, NULL, cab_enabled, cab_text, cab_change, cab_notes };
const eng_ui_page *tc2_link_page(void) { return &cab_page; }
