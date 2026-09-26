# Design: Multiple ADCs, Signal Generator, Goertzel, Routing

Status: 26.09.2026, revision `28e88fa`. Design draft, none of it is implemented. Builds on
`docs/FIRMWARE-STRUCTURE.md` and `docs/REFACTORING-PROPOSAL.md`.

## 1. Requirements

| # | Requirement |
|---|---|
| A1 | Multiple ADCs run in parallel, each with its own DMA channel into its own ping-pong buffer. All ADCs run on **the same clock**. |
| A2 | A **signal generator** produces a signal in a RAM table. The table is transferred **via DMA** to a selectable DAC. **Table size and playback frequency** are selectable. The synthesis parameters are the same as in `waveform_generator/firmware/src/tab_wave_gen.py`. |
| A3 | A **signal processing** stage as in `Goertzel/goertzel/` (Goertzel filter, low-pass, pulse detection with adaptive threshold). |
| A4 | A module **`routing.c`** wires up the signal paths: source → acquisition → processing → sink. |

Constraint: **No hardware available.** Everything that cannot be demonstrated on the host or
in the simulator is marked as "not run on silicon".

## 2. Resources of the dsPIC33AK512MPS512

From the ATDF of the pack `dsPIC33AK-MP_DFP 1.4.260` (the more reliable source, per the
rule in `CLAUDE.md`):

| Resource | Count | Requirement at full scale |
|---|---|---|
| ADC cores | 5 (ADC1..ADC5) | up to 5 |
| DMA channels | 8 (DMA0..DMA7) | **Bottleneck:** 5 ADCs + 8 DACs with table would be 13. Rule: active ADC channels + DACs in table mode ≤ 8 |
| DACs | **8** (`CMP_DAC1..8`, as part of the comparators) | all 8 should be usable (see 4.5) |
| DAC output pins | **2** (`DACOUT1`, `DACOUT2`) | limits how many DACs can go external at the same time |
| SCCP | 8 (SCCP1..8) + MCCP9 | 1 as the common ADC trigger, 1 as the DAC playback clock |
| Timer | 3 | Timer1 remains the stopwatch |
| RAM | 64 KB (0x4000..0x13FFF) | see budget below |
| UREF | 1 | internal DAC→ADC connection, `INSEL` 6..13 = DAC1..DAC8: **every DAC reachable, but only one at a time** |

Further facts from the ATDF about the DACs that drive the design:

- **Data range 205..3890** (`DACDATAH_MIN/MAX_VALUE`), not 0..4095. A signal table must
  be scaled into this range.
- **`DACDAT` sits in bit 31:16** of the data register. A 16-bit DMA transfer must target
  address + 2, or the transfer is 32 bits wide with the value in the upper half-word.
  This needs to be confirmed in the datasheet.
- Modes per the ATDF: DC, Hysteretic, Triangle-Wave, Slope. These need **no DMA
  channel**.
- `DACOEN` is a single bit per DAC. Which DAC can go to `DACOUT1` and which to
  `DACOUT2` needs to be clarified in the datasheet.

**Still to be confirmed in the datasheet**, before code is written for it:

- Which `CHSEL` codes trigger a DMA channel from an SCCP or timer (for the DAC
  playback clock).
- The maximum update rate of the DAC data register. It limits the playback frequency.
- Whether `DACxDAT` is a valid DMA target. This is confirmed for the ADC result
  register as a source, but not for the DAC as a target.
- Which ADC cores reach which pins, and whether multiple cores can read `ANn7`
  (UREF) at the same time.

### RAM budget (example, to be captured in code as `_Static_assert`)

| Consumer | Size |
|---|---|
| 5 ADCs × ping-pong 2 × 1024 samples × 2 bytes | 20 KB |
| Signal generator table up to 8192 samples × 2 bytes | 16 KB |
| Goertzel output per channel (optional, for the GUI) 1024 × 4 bytes × 5 | 20 KB |
| Stack, console, remainder | ~8 KB |
| **Total** | **~64 KB, so tight** |

So the Goertzel output per sample is only needed by whoever actually transmits it to
the GUI. In normal operation, the result per block and the counters are enough.

## 3. Target architecture

Extension of the layers from `REFACTORING-PROPOSAL.md`:

```
 app/       main   routing.c   clock_plan
              |        |
   +----------+--------+-----------+-------------+
   |          |        |           |             |
 acq/       siggen/  dsp_run/    link/ cli/    tests/
 (N channels) (table→DMA→DAC) (processing chain per channel)
   |          |        |
 pingpong/    |        |                             <- hardware-free
   |          |        |
 drivers/  adc[1..5]  dma[0..7]  dac[1..5]  sccp[1..8]  uref  uart  pll  clkgen
                                                     <- instances as parameters (V5)
 lib/      wavegen  goertzel  iir1  detect  crc16  stats  fmt  tri_eval
                                                     <- only <stdint.h>/<math.h>, host tests
```

New compared to the refactoring proposal: `acq/`, `siggen/`, `dsp_run/`, `routing.c`,
and in `lib/` the modules `wavegen`, `goertzel`, `iir1` and `detect`.

## 4. Modules

### 4.1 Multi-channel acquisition `acq/`

```c
typedef struct {
    adc_t      *adc;        /* Core 1..5                              */
    uint8_t     pinsel;     /* Input of the core                      */
    dma_ch_t   *dma;        /* assigned by routing.c                  */
    pingpong_t  pp;         /* Buffer + counters (missed, late, overrun)*/
} acq_ch_t;

bool acq_config(uint8_t n, const acq_ch_cfg_t *cfg);   /* up to 5 channels    */
bool acq_set_rate(uint32_t ksps);                      /* ONE clock for all   */
bool acq_start(void);                                  /* all simultaneously */
void acq_stop(void);
bool acq_service(void);                                /* for the main loop  */
```

- **One clock, one trigger.** PLL1 already feeds the ADC path today. All cores get
  the same clock and the same trigger (one SCCP, `TRG1SRC` in each core). This makes
  all channels sample at the same time, and samples with the same index belong
  together in time. For multi-channel evaluation, that matters more than the clock
  rate itself.
- **Back-to-back only exists in single-channel operation** (as a separate route,
  decision of 26.09.2026). Otherwise each core would run freely at its own conversion
  time, and the channels would drift apart. Triggered (the chain from `chain all`) is
  the only clean way here.
- **Ping-pong per channel.** `pingpong.c` from V7 becomes instantiable. Each
  channel's DMA interrupt reports the half to its `pingpong_t`.

**Risk that only the board can settle:** Open question 2 is already unresolved with
one channel. So far no variant has streamed without loss while the CPU processes. N
channels multiply the DMA bus load and the interrupt load. Realistically the combined
rate is well below 40 MSPS/N. Where the limit lies cannot be said without a board. The
firmware should therefore report the combined rate (N × rate) and measure it at the
first board run via an "N channels × rate" matrix.

### 4.2 Signal generator `siggen/` + `lib/wavegen.c`

**Synthesis (`lib/wavegen.c`, hardware-free)** with the same parameters as
`tab_wave_gen.py`:

| Parameter in tab_wave_gen.py | Here | Note |
|---|---|---|
| Sampling Rate (Hz) | `play_hz` | = playback frequency of the table |
| Fundamental Frequency (Hz) | `f0_hz` | |
| 2nd..7th Harmonic (factor) | `harm[6]` | Factor on the fundamental amplitude |
| Duration (s) | results from `n / play_hz` | Table size `n` is the parameter here |
| Decay Constant | `decay` | Envelope `exp(-decay * t)` |
| Amplitude | `amplitude` | 0..1 |

```c
typedef struct {
    uint32_t n;            /* Table size (samples)             */
    uint32_t play_hz;      /* Playback rate                     */
    float    f0_hz;
    float    harm[6];      /* 2nd to 7th harmonic                */
    float    decay;
    float    amplitude;
    uint16_t out_min;      /* Output range, for the DAC 205    */
    uint16_t out_max;      /*                  and 3890 (ATDF)  */
} wavegen_cfg_t;

bool wavegen_fill(const wavegen_cfg_t *c, uint16_t *table);
```

The Python script scales minimum → 0 and maximum → `1023 × amplitude` (10 bit). Here
the same formula applies to the range `out_min..out_max`: minimum → `out_min`,
maximum → `out_min + (out_max − out_min) × amplitude`. For the golden test against the
script, `out_min = 0`, `out_max = 1023` is set, so the values are comparable. The
dsPIC33A has an FPU, so `sinf`/`expf` computes on the target in reasonable time. The
table is computed once per parameter change, not in the real-time path.

**Note on looping:** The table is played back cyclically. Without decay, the signal
jumps at the end of the table if `f0_hz × n / play_hz` is not an integer. Option
`snap`: correct `f0` to the nearest integer number of periods and report the actual
frequency. With decay, the table is a pulse that repeats. That is exactly the test
signal the Goertzel pulse detection expects.

**The table is generated only on the target** (decision of 26.09.2026). There is no
upload of finished tables. The GUI only sends the parameters. WAV export and plotting
from the script are omitted in the firmware. The GUI can compute the table itself for
display, using the same formula.

**Output (`siggen/siggen.c`):**

```c
bool siggen_start(dac_t *dac, const uint16_t *table, uint32_t n, uint32_t play_hz);
void siggen_stop(void);
uint32_t siggen_actual_hz(void);   /* after rounding of the clock divider */
```

- DMA channel in Repeated Continuous mode, source = table, target = `DACxDAT`, window
  = table. This is the same pattern as with the ADC, just in the other direction.
- Trigger: a dedicated SCCP as the playback clock, derived from `play_hz`. The
  rounded, actual frequency is reported.
- `DAC UPDTRG` must be set so that every write takes effect immediately (currently
  11, see `CLAUDE.md`).
- No interrupt in normal operation. The CPU is not involved.

### 4.3 Signal processing `lib/goertzel.c`, `lib/iir1.c`, `lib/detect.c`

Taken over from `Goertzel/goertzel/firmware/src/goertzel.c`, adapted for
multi-channel operation and portability:

| In the template | Problem here | Adaptation |
|---|---|---|
| `static int32_t down_counter` in `Goertzel_i_Filter` | one counter for all instances | moved into the instance struct |
| `iIIR_Tap[N_FILTER]` global, `FLT_vIIR_Init()` resets all | a detection on channel 1 resets channel 2's low-pass | `iir1_t` per instance |
| `BLOCK_SIZE` fixed at 512 | half size is selectable here at runtime | `n` as a parameter |
| `>> 2` (12 → 10 bit) and `<< (FIXED_POINT_BITS - 2)` fixed | tied to the template's resolution | `in_shift` as a parameter, default as in the template |
| Pulse detection (threshold, counter, reset) in the filter | filter and detector mixed together | `detect.c`: threshold, counter, `max_amplitude`, adaptive threshold (`SIGNAL_SCALE_THRESHOLD` from the template's `main.c`) |
| Float and fixed-point variant | both kept | selectable at runtime, **default: float** (decision of 26.09.2026) |
| Double damping: `DAMPING_FACTOR` in the feedback term **and** `q − (q >> 8)` | two damping factors that multiply together | **only one damping factor:** the factor in the feedback term, as parameter `damping` (default 0.995). The shift stage is dropped |

```c
typedef struct { int32_t coeff, cos_t, sin_t, q1, q2; iir1_t lp; uint32_t win, win_cnt; } goertzel_i_t;
void goertzel_i_init(goertzel_i_t *g, float fs_hz, float f_hz, uint32_t window);
void goertzel_i_block(goertzel_i_t *g, const uint16_t *x, uint32_t n,
                      int32_t *mag_out /* may be NULL */, detect_t *d);
```

**Damping (decision of 26.09.2026):** The template damps twice: with
`DAMPING_FACTOR` (0.995) in the feedback term, and additionally with
`q1 − (q1 >> 8)` resp. `q2 − (q2 >> 8)` (≈ 0.9961) on the states. Only **the
factor** is carried over, as parameter `damping`. This means the module is no
longer bit-identical to the template. It is tested against a **Python reference
model** with a single damping factor (see section 8).

**Float as the default:** According to the template's readme, the float variant is
about six times slower. That was measured on a Cortex-M0+ without an FPU. The
dsPIC33A has an FPU, so the factor does not apply here. The fixed-point variant
remains available as an alternative. The compute time per block is measured once
the Goertzel filter enters the data path: in the simulator (stopwatch), if it
models FPU instructions cycle-accurately, otherwise on the board.

**Processing chain `dsp_run/`:** Each channel gets a chain of stages (none,
average/statistics, Goertzel + detector). `capture_service()` resp.
`acq_service()` calls the channel's chain for every finished half. The previous
sum (`process_buffer`) becomes one stage among several.

### 4.4 `routing.c`

Describes the signal paths as data and implements them **via the drivers**.
`routing.c` does not write any register itself.

**Sources:**

| Source | Path |
|---|---|
| `EXT(core, pinsel)` | external pin on the ADC core |
| `DAC_INT(dac)` | DAC → UREF → `ANn7` of a core (on-chip, only one DAC at a time) |
| `DAC_PIN(dac)` | DAC to its pin, wired externally to an ADC pin (like RA8 in the chain test) |
| `RAM_TABLE` | the signal generator's table directly as the input to processing, **without** ADC and DMA. Corresponds to `USE_ARTIFICIAL_SIGNAL` in the template |

**Processing:** chain per channel (see 4.3).

**Sinks:** RAM only (counters, results), GUI stream (raw data and/or Goertzel
magnitude), console.

**Signal generator target:** `DAC(n)` with output internal (UREF), at the pin, or
both.

```c
typedef struct {
    route_src_t src;        /* EXT / DAC_INT / DAC_PIN / RAM_TABLE */
    uint8_t     core, pinsel;
    dsp_chain_t chain;
    route_sink_t sink;
} route_t;

route_err_t routing_add(const route_t *r);      /* checks, assigns resources */
void        routing_clear(void);
route_err_t routing_apply(void);                 /* stops everything, configures, starts */
void        routing_list(reg_visit_t out);
```

**Tasks of `routing_add()`:**

- **Check conflicts:** A core in two routes? Two routes on UREF with different
  DACs? A DAC as generator and as triangle at the same time? A pin on a core that
  cannot reach it?
- **Assign resources:** DMA channels, SCCP for trigger and playback clock. The
  return value names the reason for a rejection, not a `bool` (as with the clock
  switches today).
- **Check the RAM budget.**

`routing_apply()` follows the order that applies to the clock switch today: all DMA
channels off, ADC cores off, set clock and trigger, cores on, DMA rebuilt from
scratch.

**Console and GUI:** `route add ext 5 5 goertzel 10000`, `route list`,
`route apply`, `siggen 8192 50000 f0=10000 h2=0.3 decay=20 amp=0.8`. The GUI receives
the routes as a table and displays them on the existing board SVG.

### 4.5 All eight DACs usable

Today's `dac.c` only knows DAC2. It becomes a driver with instances (`dac_t
DAC[1..8]`, addresses from the ATDF) and offers each mode as its own function:

```c
bool dac_dc(dac_t *d, uint16_t code);                         /* DC            */
bool dac_triangle(dac_t *d, uint16_t lo, uint16_t hi, uint16_t slope);
bool dac_slope(dac_t *d, ...);                                /* per ATDF      */
bool dac_table(dac_t *d, const uint16_t *tab, uint32_t n, uint32_t play_hz);
                                                              /* via siggen/, needs DMA + SCCP */
bool dac_output(dac_t *d, bool on);                           /* DACOEN        */
void dac_off(dac_t *d);
```

Which combinations can run at the same time is decided by `routing.c`, not the
driver:

| Resource | Limit | checked in |
|---|---|---|
| DMA channels | active ADC channels + DACs with table ≤ 8 | `routing_add()` |
| SCCP | one playback clock per table DAC (or one shared, if the rate is the same), plus one ADC trigger ≤ 8 | `routing_add()` |
| Output pins | at most 2 DACs external (`DACOUT1/2`) | `routing_add()` |
| UREF | at most 1 DAC on-chip to the ADCs | `routing_add()` |
| RAM | one table per table DAC, together within budget | `routing_add()` |

So DACs 1..8 can all be **configured and operated**. At most two are **visible** at
the pin at the same time, and one via UREF. The rest can serve as comparator
reference or for later routes.

**Proof without a board:** register trace per DAC instance and mode. This does not
prove that DAC3 through 8 do on silicon what DAC2 does, only that the driver writes
the same pattern for every instance.

## 5. Consequences for the refactoring plan

| Proposal | before | now |
|---|---|---|
| V5 instances as parameters | last step, optional | **prerequisite**: 5 ADCs, 6 DMA channels, 2 DACs, 2 SCCPs |
| V3 callbacks | deferred in the DMA ISR (weak symbol) | A weak symbol is not enough for N channels. Solution without function pointers: each ISR vector calls a shared body with a **constant** instance pointer, which the compiler can inline. Runtime comparable via disassembly |
| V7 split capture.c | medium | prerequisite: `pingpong` must be instantiable |
| V1 `lib/` | first step | remains the first step, plus `wavegen`, `goertzel`, `iir1`, `detect` |

The order follows from the version plan in section 8.

## 6. What can be demonstrated without hardware

| Part | Proof | Status after implementation |
|---|---|---|
| `wavegen` | host test against `tab_wave_gen.py` (same parameters, ±1 LSB after rounding) | proven |
| `goertzel`, `iir1`, `detect` | host test against a Python reference model with a single damping factor (float and fixed-point with tolerance); with wavegen pulses: pulse count matches | proven |
| `routing.c` validation | host test: every conflict rule with a case that triggers it | proven |
| `pingpong` with N channels | simulator (`sim_dma.c` extended to N channels) | proven (logic) |
| Path `RAM_TABLE` → Goertzel | simulator end-to-end | proven |
| Driver conversion to instances | register trace | conversion proven, register values only as good as before |
| Multi-channel DMA simultaneously, shared trigger | – | **not run on silicon** |
| DMA → DAC, playback rate | – | **not run on silicon** |
| achievable combined rate, CPU load | – | **not run on silicon** |

For the first board run after that, the firmware should deliver the following in a
single pass (per the rule "A board run costs a person their afternoon"):

- The previous `chain all` (one channel), as a reference against the old state.
- A matrix N = 1..5 channels × rate: does it stream without loss?
- The signal generator → DAC → UREF → ADC in a loop. The captured table must match
  the generated one, and the measured period must follow from `play_hz`. This is
  the same principle as today's DAC test, just with a known table instead of a
  triangle.
- Goertzel on the loop signal: pulse count equals the generator's pulse count.

## 7. Decisions (26.09.2026)

| Question | Decision |
|---|---|
| DACs in N+1 | The driver can handle DAC1..8, routing manages them, in operation DAC2 remains the source. No new console command |
| Signal synthesis in N+1 | `lib/wavegen` is carried over but not used (like the Goertzel filter) |
| Back-to-back | remains as single-channel mode, as its own route/variant in the routing model. The `test` suite keeps running. Multi-channel is always triggered |
| Directory structure | subfolders (`src/lib`, `src/drivers`, …) as in `REFACTORING-PROPOSAL.md`. `build.bat`, `tools/Makefile` and `configurations.xml` are adapted once |
| Table generation | target only, no upload of finished tables |
| Target for ADC channels | up to 5 (all cores) |
| Goertzel damping | only one damping factor: the factor in the feedback term, parameterizable (default 0.995) |
| Goertzel default | float, fixed-point remains as an alternative |
| Goertzel test reference | Python reference model with a single damping factor |

**Consequence of "up to 5 channels" for RAM:** With 5 × 1024-sample halves (20 KB)
and an 8K table (16 KB), the RAM budget is tight (section 2). The table size and
half size must therefore be checked against the budget at runtime, in
`routing_add()`. Fixed maximum sizes for both are not enough.

## 8. Version plan

Decision of 26.09.2026: **The routing is laid down in the core, but the next version
can functionally only do the streaming that is already implemented today.** The full
routing comes later. All DACs should be usable.

### Version N+1: core with routing, function as today

**Function for the user:** unchanged. `stream on`, `stream grab`, `blk`, `chain all`
and the `test` suite behave as they do today. The GUI notices no difference.

**Internally:**

| Part | Scope in N+1 |
|---|---|
| Refactoring | step 0 (register trace), V1, V2, V10, V9, V8, V4 |
| Drivers with instances (V5 + V3) | `adc` (cores 1..5), `dma` (0..7), `sccp` (1..8), `dac` (1..8). The driver can handle all instances, operation uses the same ones as today |
| `pingpong` (V7) | instantiable, one instance in operation |
| `routing.c` | **complete data model and API** (`route_t`, sources, sinks, resource table, conflict check, `routing_apply()`), but **exactly one permitted route**: today's chain SCCP1 → ADC core 5 (Single) → DMA0 → ping-pong → CPU, DAC2 as the signal source |
| `stream on` | internally calls `routing_apply(&ROUTE_STREAM)` instead of wiring the chain itself |
| Console | `route list` shows the fixed route and the resource allocation. `route add` etc. does not exist yet, or it rejects every other route with `ROUTE_ERR_NOT_YET` |
| Signal synthesis `lib/wavegen` | **carried over but not used**, like the Goertzel filter. No output to a DAC in N+1 |
| Signal processing from the Goertzel project | **carried over but not used:** `lib/goertzel` (fixed-point and float), `lib/iir1`, `lib/detect`, adapted as in 4.3. The modules are compiled in both build variants (`-Wall -Wextra` clean) so they do not rot. No call from the firmware, no route, no console command |
| not in N+1 | multi-channel operation, signal generator with DMA → DAC, processing chain `dsp_run/` (integrating the Goertzel filter into the data path), further routes |

**Why the routing already belongs in the core now:** If `stream on` already runs
through `routing_apply()`, later expansion is an extension of the permitted routes,
not a rebuild of the startup path. The resource table (which DMA channel, which
SCCP, which DAC is occupied) then already exists and is tested with just one route.

**Acceptance without a board:**

- The register trace of `stream on` (new, via `routing_apply()`) is **bit-identical**
  to the trace of `chain_stream_on()` in the current state. That is the central
  proof that N+1 functionally does the same thing.
- Register trace of all driver instances, including the unused ones (DAC1..8,
  DMA0..7 etc.).
- Host tests of the conflict check in `routing.c`, with all rules, even though
  only one route is permitted in N+1.
- Simulator acceptance (on request).
- GUI self-test.
- Host tests of the Goertzel modules against a **Python reference model** (numpy)
  with only one damping factor (`damping`): float and fixed-point within a
  tolerance, same input (512-sample blocks, `in_shift` = 2). Bit-identity with the
  template is not the goal, because the second damping factor is dropped. On
  pulses from `wavegen`, the pulse count must match.
- Host test of `wavegen` against `tab_wave_gen.py` (`out_min = 0`, `out_max = 1023`,
  ±1 LSB). In addition, a test with two instances that shows a detection on one
  does not affect the other. This is exactly what the template, with its global
  state, does not do.

**Status after N+1:** rebuilt, demonstrated on the host and in the simulator, **not
run on silicon**. The first board run afterward must reproduce the previous
`chain all`.

### Version N+2 and later: unlock routing

In this order, each as its own version:

1. **Further single-channel routes:** different core, different pin, different DAC
   (1..8) as the source via UREF or pin. Just an extension of the permitted routes.
2. **Signal generator:** `siggen/` (table → DMA → DAC) with the `lib/wavegen`
   carried over in N+1. Source `RAM_TABLE` for processing without hardware.
3. **Processing chain:** `dsp_run/` integrates the modules `goertzel`, `iir1` and
   `detect` carried over in N+1 into the data path. Only then does a measurement
   on the dsPIC33A settle whether float or fixed-point becomes the default.
4. **Multi-channel:** `acq/` with N channels, shared trigger.
