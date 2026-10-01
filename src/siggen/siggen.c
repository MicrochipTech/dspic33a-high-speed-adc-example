/*
 * siggen.c - the signal generator (see siggen.h; docs/IMPLEMENTATION-PLAN.md
 * section SG, SG.3, 29.09.2026)
 *
 * Owns the table, the parameters and the running state. Touches no
 * register: lib/wavegen computes, dma.c (channel 2) transports, sccp.c
 * (SCCP2) paces, dac.c outputs, routing.c books the resources, timebase.c
 * times the status report's transfer-rate measurement.
 */

#include <stddef.h>
#include "siggen.h"
#include "dma_tx.h"
#include "sccp.h"
#include "dac.h"
#include "routing.h"
#include "timebase.h"

/* The table, in the ".dma_buffer" section beside capture.c's ADC buffer
 * (dma.c: DMALOW/DMAHIGH are shared by all channels and cover every SRAM
 * access, the source included - 13.4.5 p826 - so the window must hold
 * both; one section keeps anything else out of it). 4-byte aligned like
 * the buffer; 16-bit entries, the DAC's data half takes 16 bits. */
static uint16_t table[SIGGEN_N_MAX] __attribute__((section(".dma_buffer"), aligned(4)));

/* tab_wave_gen.py's defaults (its entry fields, lines 136-186): 10 kHz,
 * harmonics 0.2/0.4/0.1/0/0/0, decay 1000, amplitude 1.0 - with the DAC's
 * range instead of the script's 0..1023. n and play_hz come with every
 * start (the script's 500 kHz and 0.01 s = 5000 samples are the GUI's
 * defaults). */
static wavegen_cfg_t cfg = {
    .n = 0u, .play_hz = 0u, .f0_hz = 10000.0f,
    .harm = { 0.2f, 0.4f, 0.1f, 0.0f, 0.0f, 0.0f },
    .decay = 1000.0f, .amplitude = 1.0f,
    .out_min = SIGGEN_LO_DEFAULT, .out_max = SIGGEN_HI_DEFAULT,
};
/* The parameters as set, in millionths, for the status reply - exactly
 * what was typed, not a float's rounding of it. */
static int64_t p_f0 = 10000000000LL, p_decay = 1000000000LL, p_amp = 1000000;
static int64_t p_h[6] = { 200000, 400000, 100000, 0, 0, 0 };

static bool     s_on = false;
static uint8_t  s_dac = 0u;
static uint32_t s_n = 0u, s_play_hz = 0u;
static bool     s_snap = true, s_force = false;
static uint8_t  s_pace = 0u;
static float    s_f0_used = 0.0f;
static uint16_t s_min = 0u, s_max = 0u;
static uint8_t  s_wavegen_err = 0u, s_route_err = 0u;

static bool str_eq(const char *a, const char *b)
{
    while ((*a != '\0') && (*a == *b)) { a++; b++; }
    return *a == *b;
}

siggen_result_t siggen_set(const char *name, int64_t v)
{
    const float f = (float)v / 1e6f;
    if (str_eq(name, "f0")) {
        if ((v <= 0) || (v > (int64_t)SIGGEN_PLAY_HZ_MAX * 1000000)) { return SIGGEN_E_VALUE; }
        p_f0 = v; cfg.f0_hz = f; return SIGGEN_OK;
    }
    if ((name[0] == 'h') && (name[1] >= '2') && (name[1] <= '7') && (name[2] == '\0')) {
        if ((v < -100000000) || (v > 100000000)) { return SIGGEN_E_VALUE; }
        const uint32_t k = (uint32_t)(name[1] - '2');
        p_h[k] = v; cfg.harm[k] = f; return SIGGEN_OK;
    }
    if (str_eq(name, "decay")) {
        if ((v < 0) || (v > 1000000000000000LL)) { return SIGGEN_E_VALUE; }   /* <= 1e9 /s */
        p_decay = v; cfg.decay = f; return SIGGEN_OK;
    }
    if (str_eq(name, "amp")) {
        if ((v <= 0) || (v > 1000000)) { return SIGGEN_E_VALUE; }
        p_amp = v; cfg.amplitude = f; return SIGGEN_OK;
    }
    if (str_eq(name, "lo") || str_eq(name, "hi")) {
        if ((v < 0) || (v > 4095000000LL) || ((v % 1000000) != 0)) { return SIGGEN_E_VALUE; }
        const uint16_t code = (uint16_t)(v / 1000000);
        if (name[0] == 'l') { cfg.out_min = code; } else { cfg.out_max = code; }
        return SIGGEN_OK;
    }
    return SIGGEN_E_PARAM;
}

void siggen_stop(void)
{
    if (!s_on) { return; }
    sccp2_stop();                     /* no trigger first ...            */
    dma_tx_stop();                   /* ... then the transport ...      */
    dac_off(s_dac);                   /* ... then the output             */
    routing_gen_release();
    s_on  = false;
    s_dac = 0u;
}

void siggen_release_dac(uint8_t dac)
{
    if (s_on && (s_dac == dac)) { siggen_stop(); }
}

siggen_result_t siggen_start(uint8_t dac, uint32_t n, uint32_t play_hz,
                             bool snap, bool force, uint8_t pace)
{
    siggen_stop();
    s_wavegen_err = 0u;
    s_route_err   = 0u;

    /* Every check before the first driver call. */
    if ((dac != 1u) && (dac != 2u))                        { return SIGGEN_E_DAC; }
    if ((n < 2u) || (n > SIGGEN_N_MAX))                    { return SIGGEN_E_N; }
    if ((play_hz < SIGGEN_PLAY_HZ_MIN) || (play_hz > SIGGEN_PLAY_HZ_MAX)) { return SIGGEN_E_RATE; }
    if (cfg.out_max > 4095u)                               { return SIGGEN_E_RANGE; }
    if (!force && ((cfg.out_min < SIGGEN_LO_DEFAULT) || (cfg.out_max > SIGGEN_HI_DEFAULT))) {
        return SIGGEN_E_RANGE;
    }
    if (pace > (uint8_t)SCCP2_PACE_OC32)                   { return SIGGEN_E_CLOCK; }

    /* The table is computed at the rate SCCP2 will really run - its
     * period is a whole number of input clocks - so f0 and the snap are
     * true to what comes out, not to the rate asked for. */
    const uint32_t clk   = sccp2_hz();
    const uint32_t ticks = (clk + play_hz / 2u) / play_hz;
    const uint32_t real  = (ticks != 0u) ? (clk + ticks / 2u) / ticks : play_hz;
    wavegen_cfg_t c = cfg;
    c.n = n;
    c.play_hz = real;
    /* The resources first, then wavegen_fill(), which checks f0, decay,
     * amplitude, the range and the swing and writes the table only on
     * success. */
    const route_err_t re = routing_gen_check(dac, n);
    if (re != ROUTE_OK) { s_route_err = (uint8_t)re; return SIGGEN_E_ROUTE; }
    float f0u = 0.0f;
    const wavegen_result_t we = wavegen_fill(&c, table, snap, &f0u);
    if (we != WAVEGEN_OK) { s_wavegen_err = (uint8_t)we; return SIGGEN_E_WAVEGEN; }

    uint16_t mn = 0xFFFFu, mx = 0u;
    for (uint32_t i = 0u; i < n; i++) {
        if (table[i] < mn) { mn = table[i]; }
        if (table[i] > mx) { mx = table[i]; }
    }

    /* The DAC to a static level first (DC mode, UPDTRG = 3: every write
     * taken at once - dac.c), so the first DMA write lands on a running
     * output. dac_level_start() holds its level inside the DAC's usable
     * range even when a forced table leaves it. */
    uint16_t first = table[0];
    if (first < SIGGEN_LO_DEFAULT) { first = SIGGEN_LO_DEFAULT; }
    if (first > SIGGEN_HI_DEFAULT) { first = SIGGEN_HI_DEFAULT; }
    if (!dac_level_start(dac, first)) { return SIGGEN_E_DAC_START; }
    if (!dma_tx_start(DMA_TRIG_SCCP2, table, n, dac_dma_target(dac), DMA_SIZE_16)) {
        dac_off(dac);
        return SIGGEN_E_DMA;
    }
    if (!sccp2_start(ticks, (sccp2_pace_t)pace)) {
        sccp2_stop();
        dma_tx_stop();
        dac_off(dac);
        return SIGGEN_E_CLOCK;
    }
    (void)routing_gen_claim(dac, n);

    s_on = true;
    s_dac = dac; s_n = n; s_play_hz = play_hz;
    s_snap = snap; s_force = force; s_pace = pace;
    s_f0_used = f0u;
    s_min = mn; s_max = mx;
    return SIGGEN_OK;
}

bool     siggen_running(void)     { return s_on; }
uint8_t  siggen_dac(void)         { return s_on ? s_dac : 0u; }
uint32_t siggen_actual_hz(void)   { return s_on ? sccp2_actual_hz() : 0u; }
float    siggen_f0_used(void)     { return s_f0_used; }
uint8_t  siggen_wavegen_err(void) { return s_wavegen_err; }
uint8_t  siggen_route_err(void)   { return s_route_err; }
const uint16_t *siggen_table(void) { return table; }
uint32_t siggen_n(void)           { return s_n; }

const char *siggen_result_name(siggen_result_t r)
{
    switch (r) {
    case SIGGEN_OK:          return "ok";
    case SIGGEN_E_PARAM:     return "no such parameter (f0 h2..h7 decay amp lo hi)";
    case SIGGEN_E_VALUE:     return "value out of range";
    case SIGGEN_E_DAC:       return "dac must be 1 or 2";
    case SIGGEN_E_N:         return "n must be 2..8192";
    case SIGGEN_E_RATE:      return "play_hz must be 100..1000000";
    case SIGGEN_E_RANGE:     return "lo/hi outside 205..3890 (p1417) - add force";
    case SIGGEN_E_WAVEGEN:   return "wavegen refused the parameters";
    case SIGGEN_E_ROUTE:     return "routing refused: resource or DAC in use";
    case SIGGEN_E_DAC_START: return "DAC did not start";
    case SIGGEN_E_DMA:       return "DMA channel 2 refused";
    case SIGGEN_E_CLOCK:     return "SCCP2 refused the period";
    }
    return "?";
}

/* Transfers per second, measured: DMA2CNT counts down once per transfer
 * and reloads at the end of the table, so two reads a known time apart
 * give the count between them - as long as fewer than n transfers fall
 * in the window, which is sized to about half a table (at least 50 us,
 * at most 10 ms). 0 when nothing moved: the trigger does not reach the
 * DMA (the first question the board run asks, SG.8). */
static uint32_t s_flags = 0u;         /* sccp2_flags_read() over the window */

static uint32_t measure_tps(void)
{
    const uint32_t hz = sccp2_actual_hz();
    s_flags = 0u;
    if (!s_on || (hz == 0u)) { return 0u; }
    uint64_t win = ((uint64_t)TIMEBASE_HZ * (s_n / 2u)) / hz;
    if (win < TIMEBASE_HZ / 20000u) { win = TIMEBASE_HZ / 20000u; }
    if (win > TIMEBASE_HZ / 100u)   { win = TIMEBASE_HZ / 100u; }
    sccp2_flags_clear();
    const uint32_t t0 = timebase_ticks();
    const uint32_t c0 = dma_tx_remaining();
    uint32_t t1;
    do { t1 = timebase_ticks(); } while ((uint32_t)(t1 - t0) < (uint32_t)win);
    const uint32_t c1 = dma_tx_remaining();
    s_flags = sccp2_flags_read();
    const uint32_t moved = (c0 >= c1) ? (c0 - c1) : (c0 + s_n - c1);
    return (uint32_t)(((uint64_t)moved * TIMEBASE_HZ) / (uint32_t)(t1 - t0));
}

void siggen_visit(siggen_visit_t visit)
{
    static const char *const hname[6] = { "h2", "h3", "h4", "h5", "h6", "h7" };
    visit("on", s_on ? 1 : 0, SIGGEN_VIS_NUM);
    visit("dac", s_on ? s_dac : 0, SIGGEN_VIS_NUM);
    visit("n", s_n, SIGGEN_VIS_NUM);
    visit("play_hz", s_play_hz, SIGGEN_VIS_NUM);
    visit("play_hz_actual", siggen_actual_hz(), SIGGEN_VIS_NUM);
    visit("pace", s_pace, SIGGEN_VIS_NUM);
    visit("f0", p_f0, SIGGEN_VIS_DEC);
    visit("f0_used", (int64_t)(s_f0_used * 1000.0f) * 1000, SIGGEN_VIS_DEC);
    for (uint32_t k = 0u; k < 6u; k++) { visit(hname[k], p_h[k], SIGGEN_VIS_DEC); }
    visit("decay", p_decay, SIGGEN_VIS_DEC);
    visit("amp", p_amp, SIGGEN_VIS_DEC);
    visit("lo", cfg.out_min, SIGGEN_VIS_NUM);
    visit("hi", cfg.out_max, SIGGEN_VIS_NUM);
    visit("snap", s_snap ? 1 : 0, SIGGEN_VIS_NUM);
    visit("force", s_force ? 1 : 0, SIGGEN_VIS_NUM);
    visit("table_min", s_on ? s_min : 0, SIGGEN_VIS_NUM);
    visit("table_max", s_on ? s_max : 0, SIGGEN_VIS_NUM);
    visit("dma2_stat", dma_tx_status(), SIGGEN_VIS_HEX);
    visit("dma2_on", dma_tx_enabled() ? 1 : 0, SIGGEN_VIS_NUM);
    visit("transfers_per_s", measure_tps(), SIGGEN_VIS_NUM);
    /* Which SCCP2 event fired in that window - bit 0 the timer (CCT2IF),
     * bit 1 the IC/OC event (CCP2IF, the DMA's trigger, Table 13-2): the
     * answer to "does SCCP2 trigger the DMA at all" when transfers is 0. */
    visit("sccp2_flags", s_flags, SIGGEN_VIS_NUM);
    visit("window_gap", dma_window_gap(), SIGGEN_VIS_NUM);
}
