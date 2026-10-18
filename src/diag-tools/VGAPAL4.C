/*
 * VGAPAL4.C - VGA DAC recovery: CPU-time delay vs I/O-serialization delay
 * Target: 16-bit real-mode MS-DOS, Open Watcom C/C++ 1.9
 *
 * Build:
 *   wcl -bt=dos -ms VGAPAL4.C
 *
 * Run from plain DOS / real-mode MS-DOS mode, preferably with TURBO ON.
 * Creates VGAPAL4.TXT in the current directory.
 *
 * Purpose of this revision:
 *   Determine whether the improvement previously obtained with port-80h
 *   delays is explained simply by elapsed CPU time, or whether performing
 *   an additional I/O transaction itself helps to serialize/drain the
 *   CPU->chipset->PCI/VGA path.
 *
 * Write strategies:
 *   FAST       : no inserted recovery delay.
 *   CPU_1US    : CPU-only calibrated spin, nominal ~1 us.
 *   CPU_2US    : CPU-only calibrated spin, nominal ~2 us.
 *   CPU_4US    : CPU-only calibrated spin, nominal ~4 us.
 *   CPU_8US    : CPU-only calibrated spin, nominal ~8 us.
 *   IO80_X1    : one OUT 80h between DAC writes.
 *   IO80_X2    : two OUT 80h between DAC writes.
 *   IO80_X4    : four OUT 80h between DAC writes.
 *   IO80_X8    : eight OUT 80h between DAC writes.
 *
 * For every delayed strategy, the same delay is inserted after the initial
 * OUT 3C8h and after every OUT 3C9h.  Thus the experiment covers 3C8->R,
 * R->G, G->B and B->next-R recovery intervals uniformly.
 *
 * Readback is deliberately conservative, inherited from VGAPAL3:
 *   - re-index 3C7h before every sampled entry;
 *   - 200 OUT 80h recovery writes after 3C7h and every 3C9h IN;
 *   - 3-sample majority vote, expanded to 5 if required.
 *
 * NOTE ABOUT CPU DELAYS:
 *   The CPU-only delay uses the same integer/rotate workload used by
 *   TURBOPROBE's CPU_ALU test and performs NO I/O.  Its unit time is
 *   calibrated at program start against the BIOS timer.  The target
 *   microsecond values are therefore approximate, not laboratory timing.
 */

#include <stdio.h>
#include <string.h>
#include <conio.h>
#include <dos.h>
#include <i86.h>

#define DAC_MASK        0x3C6
#define DAC_READ_INDEX  0x3C7
#define DAC_WRITE_INDEX 0x3C8
#define DAC_DATA        0x3C9

#define DAC_IO_DELAY_PORT          0x80
#define READER_DELAY_PORT80_WRITES 200

#define BIOS_TICK_SEG       0x0040
#define BIOS_TICK_OFF       0x006C
#define BIOS_TICK_HZ        18.2065

#define WRITE_STRATEGIES  9
#define WRITE_TEST_ROUNDS 12
#define ROBUST_BASE_SAMPLES 3
#define ROBUST_MAX_SAMPLES  5
#define READER_SELFTEST_REREADS 4
#define MAX_LOG_DIFFS 10

#define CALIBRATION_TICKS 9UL
#define CALIBRATION_SPIN_UNITS 256U
#define DELAY_MEASURE_TICKS 4UL
#define DELAY_MEASURE_BATCH 64U

struct RGBColor {
    unsigned char r;
    unsigned char g;
    unsigned char b;
};
typedef struct RGBColor RGB;

struct ReadQuality {
    unsigned long raw_reads;
    unsigned long raw_3f;
    unsigned long raw_7f;
    unsigned long raw_bf;
    unsigned long raw_ff;
    unsigned long raw_high_bits;
    unsigned long disagreement_components;
    unsigned long unresolved_components;
    unsigned long entries_needing_extra_samples;
};
typedef struct ReadQuality ReadQuality;

struct StrategyResult {
    unsigned int trials;
    unsigned int perfect_trials;
    unsigned int trials_with_stable_mismatch;
    unsigned long bad_components;
    unsigned long stable_bad_components;
    unsigned long noisy_bad_components;
    unsigned long bad_entries;
    unsigned long read_disagreement_components;
    unsigned long read_unresolved_components;
    unsigned long reader_raw_reads;
    unsigned long reader_raw_7f;
    unsigned long reader_raw_bf;
    unsigned long reader_raw_ff;
    unsigned long worst_bad_components;
};
typedef struct StrategyResult StrategyResult;

static RGB saved_palette[256];
static RGB expected[256];
static RGB observed[256];
static RGB reader_reference[256];
static RGB vote_samples[256][ROBUST_MAX_SAMPLES];
static unsigned char vote_sample_count[256];
static unsigned char read_unstable_mask[256];
static unsigned char read_unresolved_mask[256];

static unsigned char saved_mode = 3;
static unsigned char saved_page = 0;
static unsigned char saved_mask = 0xFF;
static FILE *log_file = NULL;

static StrategyResult strategy_results[WRITE_STRATEGIES];
static unsigned int cpu_delay_units[4];
static double cpu_delay_measured_us[4];
static double io80_delay_measured_us[4];
static unsigned int cpu_delay_sink = 0x1357U;

static const unsigned int cpu_target_us[4] = { 1U, 2U, 4U, 8U };
static const unsigned int io80_counts[4] = { 1U, 2U, 4U, 8U };

static const char *strategy_names[WRITE_STRATEGIES] = {
    "FAST",
    "CPU_1US",
    "CPU_2US",
    "CPU_4US",
    "CPU_8US",
    "IO80_X1",
    "IO80_X2",
    "IO80_X4",
    "IO80_X8"
};

/* --------------------------------------------------------------------- */
/* BIOS timer and video helpers                                           */
/* --------------------------------------------------------------------- */

static unsigned long read_bios_ticks(void)
{
    volatile unsigned long __far *p;
    unsigned long value;

    p = (volatile unsigned long __far *)MK_FP(BIOS_TICK_SEG, BIOS_TICK_OFF);
    _disable();
    value = *p;
    _enable();
    return value;
}

static unsigned long wait_next_tick(void)
{
    unsigned long t;
    unsigned long n;

    t = read_bios_ticks();
    do {
        n = read_bios_ticks();
    } while (n == t);
    return n;
}

static void bios_set_mode(unsigned char mode)
{
    union REGS inregs;
    union REGS outregs;

    memset(&inregs, 0, sizeof(inregs));
    memset(&outregs, 0, sizeof(outregs));
    inregs.h.ah = 0x00;
    inregs.h.al = mode;
    int86(0x10, &inregs, &outregs);
}

static void get_current_mode(void)
{
    union REGS inregs;
    union REGS outregs;

    memset(&inregs, 0, sizeof(inregs));
    memset(&outregs, 0, sizeof(outregs));
    inregs.h.ah = 0x0F;
    int86(0x10, &inregs, &outregs);
    saved_mode = (unsigned char)(outregs.h.al & 0x7F);
    saved_page = outregs.h.bh;
}

static void set_active_page(unsigned char page)
{
    union REGS inregs;
    union REGS outregs;

    memset(&inregs, 0, sizeof(inregs));
    memset(&outregs, 0, sizeof(outregs));
    inregs.h.ah = 0x05;
    inregs.h.al = page;
    int86(0x10, &inregs, &outregs);
}

static void prepare_mode13(void)
{
    bios_set_mode(0x13);
    outp(DAC_MASK, 0xFF);
}

/* --------------------------------------------------------------------- */
/* Reader delay and raw DAC read accounting                               */
/* --------------------------------------------------------------------- */

static void reader_io_delay(void)
{
    unsigned int i;

    for (i = 0; i < READER_DELAY_PORT80_WRITES; ++i) {
        outp(DAC_IO_DELAY_PORT, 0);
    }
}

static void clear_read_quality(ReadQuality *q)
{
    memset(q, 0, sizeof(*q));
}

static unsigned char read_raw_dac(ReadQuality *q)
{
    unsigned char v;

    v = (unsigned char)(inp(DAC_DATA) & 0xFF);
    ++q->raw_reads;
    if (v == 0x3F) { ++q->raw_3f; }
    if (v == 0x7F) { ++q->raw_7f; }
    if (v == 0xBF) { ++q->raw_bf; }
    if (v == 0xFF) { ++q->raw_ff; }
    if ((v & 0xC0) != 0) { ++q->raw_high_bits; }
    reader_io_delay();
    return v;
}

/* --------------------------------------------------------------------- */
/* CPU-only delay calibration                                             */
/* --------------------------------------------------------------------- */

/*
 * A deliberately register-friendly integer workload.  There is no I/O in
 * this function.  The return value is fed into the next call so the compiler
 * cannot legally discard the work.  Open Watcom's register calling
 * convention and the 'register' hints normally keep the hot state in CPU
 * registers; even if it uses stack temporaries, no explicit I/O transaction
 * is generated.
 */
static unsigned int cpu_spin(unsigned int units, unsigned int seed)
{
    register unsigned int i;
    register unsigned int a;
    register unsigned int b;

    a = seed;
    b = (unsigned int)(seed ^ 0x9BDFU);

    for (i = 0; i < units; ++i) {
        a = (unsigned int)(a + 0x9E37U);
        a ^= b;
        b = (unsigned int)((b << 1) | (b >> 15));
        b = (unsigned int)(b + a + 0x1234U);
        a = (unsigned int)((a >> 3) | (a << 13));
    }

    return (unsigned int)(a ^ b);
}

static double calibrate_cpu_spin_unit_us(void)
{
    unsigned long start;
    unsigned long now;
    unsigned long calls;
    unsigned long total_units;
    unsigned int seed;
    double seconds;

    seed = 0x1357U;
    calls = 0UL;
    start = wait_next_tick();
    now = start;

    while ((unsigned long)(now - start) < CALIBRATION_TICKS) {
        seed = cpu_spin(CALIBRATION_SPIN_UNITS, seed);
        ++calls;
        if ((calls & 31UL) == 0UL) {
            now = read_bios_ticks();
        }
    }

    cpu_delay_sink = seed;
    total_units = calls * (unsigned long)CALIBRATION_SPIN_UNITS;
    seconds = (double)(now - start) / BIOS_TICK_HZ;
    if (total_units == 0UL) {
        return 1.0;
    }
    return seconds * 1000000.0 / (double)total_units;
}

static unsigned int units_for_target(double unit_us, unsigned int target_us)
{
    double v;
    unsigned long n;

    if (unit_us <= 0.0) {
        return 1U;
    }

    v = (double)target_us / unit_us;
    n = (unsigned long)(v + 0.5);
    if (n < 1UL) { n = 1UL; }
    if (n > 65535UL) { n = 65535UL; }
    return (unsigned int)n;
}

static double measure_cpu_delay_us(unsigned int units)
{
    unsigned long start;
    unsigned long now;
    unsigned long groups;
    unsigned long calls;
    unsigned int j;
    unsigned int seed;
    double seconds;

    seed = 0x2468U;
    groups = 0UL;
    start = wait_next_tick();
    now = start;

    while ((unsigned long)(now - start) < DELAY_MEASURE_TICKS) {
        for (j = 0; j < DELAY_MEASURE_BATCH; ++j) {
            seed = cpu_spin(units, seed);
        }
        ++groups;
        now = read_bios_ticks();
    }

    cpu_delay_sink = seed;
    calls = groups * (unsigned long)DELAY_MEASURE_BATCH;
    seconds = (double)(now - start) / BIOS_TICK_HZ;
    if (calls == 0UL) { return 0.0; }
    return seconds * 1000000.0 / (double)calls;
}

static void io80_delay(unsigned int count)
{
    unsigned int i;

    for (i = 0; i < count; ++i) {
        outp(DAC_IO_DELAY_PORT, 0);
    }
}

static double measure_io80_delay_us(unsigned int count)
{
    unsigned long start;
    unsigned long now;
    unsigned long groups;
    unsigned long calls;
    unsigned int j;
    double seconds;

    groups = 0UL;
    start = wait_next_tick();
    now = start;

    while ((unsigned long)(now - start) < DELAY_MEASURE_TICKS) {
        for (j = 0; j < DELAY_MEASURE_BATCH; ++j) {
            io80_delay(count);
        }
        ++groups;
        now = read_bios_ticks();
    }

    calls = groups * (unsigned long)DELAY_MEASURE_BATCH;
    seconds = (double)(now - start) / BIOS_TICK_HZ;
    if (calls == 0UL) { return 0.0; }
    return seconds * 1000000.0 / (double)calls;
}

static void calibrate_delays(void)
{
    double unit_us;
    unsigned int i;

    unit_us = calibrate_cpu_spin_unit_us();

    for (i = 0; i < 4U; ++i) {
        cpu_delay_units[i] = units_for_target(unit_us, cpu_target_us[i]);
        cpu_delay_measured_us[i] = measure_cpu_delay_us(cpu_delay_units[i]);
        io80_delay_measured_us[i] = measure_io80_delay_us(io80_counts[i]);
    }

    if (log_file != NULL) {
        fprintf(log_file, "=== DELAY CALIBRATION ===\n");
        fprintf(log_file,
                "CPU spin unit estimate: %.4f us/unit (calibration body=%u units, %lu BIOS ticks)\n",
                unit_us, CALIBRATION_SPIN_UNITS, CALIBRATION_TICKS);
        fprintf(log_file,
                "Measured delay figures below include ordinary call/loop overhead and are approximate.\n");
        fprintf(log_file,
                "They are useful for comparing elapsed-time delay with explicit I/O serialization, not as oscilloscope timing.\n\n");
        fprintf(log_file, "CPU target   spin units   measured approx\n");
        for (i = 0; i < 4U; ++i) {
            fprintf(log_file, "  %2u us       %5u        %8.3f us\n",
                    cpu_target_us[i], cpu_delay_units[i],
                    cpu_delay_measured_us[i]);
        }
        fprintf(log_file, "\nPort-80 group  measured approx\n");
        for (i = 0; i < 4U; ++i) {
            fprintf(log_file, "  %2u OUT(s)      %8.3f us\n",
                    io80_counts[i], io80_delay_measured_us[i]);
        }
        fprintf(log_file, "=== END DELAY CALIBRATION ===\n\n");
        fflush(log_file);
    }
}

/* --------------------------------------------------------------------- */
/* Palette generator                                                      */
/* --------------------------------------------------------------------- */

static void make_test_palette(RGB *p, unsigned int phase)
{
    unsigned int i;

    for (i = 0; i < 256; ++i) {
        p[i].r = (unsigned char)((i + phase * 7U) & 0x3F);
        p[i].g = (unsigned char)(((i >> 2) + phase * 11U) & 0x3F);
        p[i].b = (unsigned char)((i * 13U + phase * 17U) & 0x3F);
    }
}

/* --------------------------------------------------------------------- */
/* Write strategies                                                       */
/* --------------------------------------------------------------------- */

static void write_fast(RGB *p)
{
    unsigned int i;

    _disable();
    outp(DAC_WRITE_INDEX, 0);
    for (i = 0; i < 256; ++i) {
        outp(DAC_DATA, p[i].r & 0x3F);
        outp(DAC_DATA, p[i].g & 0x3F);
        outp(DAC_DATA, p[i].b & 0x3F);
    }
    _enable();
}

static void write_cpu_delayed(RGB *p, unsigned int units)
{
    unsigned int i;
    register unsigned int seed;

    seed = 0x5A5AU;
    _disable();
    outp(DAC_WRITE_INDEX, 0);
    seed = cpu_spin(units, seed);
    for (i = 0; i < 256; ++i) {
        outp(DAC_DATA, p[i].r & 0x3F);
        seed = cpu_spin(units, seed);
        outp(DAC_DATA, p[i].g & 0x3F);
        seed = cpu_spin(units, seed);
        outp(DAC_DATA, p[i].b & 0x3F);
        seed = cpu_spin(units, seed);
    }
    _enable();
    cpu_delay_sink = seed;
}

static void write_io80_delayed(RGB *p, unsigned int count)
{
    unsigned int i;

    _disable();
    outp(DAC_WRITE_INDEX, 0);
    io80_delay(count);
    for (i = 0; i < 256; ++i) {
        outp(DAC_DATA, p[i].r & 0x3F);
        io80_delay(count);
        outp(DAC_DATA, p[i].g & 0x3F);
        io80_delay(count);
        outp(DAC_DATA, p[i].b & 0x3F);
        io80_delay(count);
    }
    _enable();
}

static void write_with_strategy(unsigned int strategy, RGB *p)
{
    if (strategy == 0U) {
        write_fast(p);
    } else if (strategy >= 1U && strategy <= 4U) {
        write_cpu_delayed(p, cpu_delay_units[strategy - 1U]);
    } else {
        write_io80_delayed(p, io80_counts[strategy - 5U]);
    }
}

/* --------------------------------------------------------------------- */
/* Robust readback                                                        */
/* --------------------------------------------------------------------- */

static void read_entry_sample(unsigned int index, RGB *sample, ReadQuality *q)
{
    unsigned char raw;

    _disable();
    outp(DAC_READ_INDEX, (unsigned char)index);
    reader_io_delay();
    raw = read_raw_dac(q); sample->r = (unsigned char)(raw & 0x3F);
    raw = read_raw_dac(q); sample->g = (unsigned char)(raw & 0x3F);
    raw = read_raw_dac(q); sample->b = (unsigned char)(raw & 0x3F);
    _enable();
}

static unsigned char component_value(RGB *s, unsigned int channel)
{
    if (channel == 0U) { return s->r; }
    if (channel == 1U) { return s->g; }
    return s->b;
}

static void set_component_value(RGB *s, unsigned int channel, unsigned char v)
{
    if (channel == 0U) { s->r = v; }
    else if (channel == 1U) { s->g = v; }
    else { s->b = v; }
}

static unsigned char mode_component(RGB *samples, unsigned int n,
                                    unsigned int channel,
                                    unsigned int *best_count)
{
    unsigned int i;
    unsigned int j;
    unsigned int count;
    unsigned int max_count;
    unsigned char candidate;
    unsigned char best;

    best = component_value(&samples[0], channel);
    max_count = 0U;

    for (i = 0; i < n; ++i) {
        candidate = component_value(&samples[i], channel);
        count = 0U;
        for (j = 0; j < n; ++j) {
            if (component_value(&samples[j], channel) == candidate) {
                ++count;
            }
        }
        if (count > max_count) {
            max_count = count;
            best = candidate;
        }
    }

    *best_count = max_count;
    return best;
}

static void robust_read_palette(RGB *p, ReadQuality *q)
{
    unsigned int i;
    unsigned int j;
    unsigned int channel;
    unsigned int count;
    unsigned int n;
    unsigned int need_extra;
    unsigned char v;

    clear_read_quality(q);
    memset(read_unstable_mask, 0, sizeof(read_unstable_mask));
    memset(read_unresolved_mask, 0, sizeof(read_unresolved_mask));
    memset(vote_sample_count, 0, sizeof(vote_sample_count));

    for (i = 0; i < 256; ++i) {
        for (j = 0; j < ROBUST_BASE_SAMPLES; ++j) {
            read_entry_sample(i, &vote_samples[i][j], q);
        }
        n = ROBUST_BASE_SAMPLES;
        need_extra = 0U;
        for (channel = 0; channel < 3U; ++channel) {
            (void)mode_component(vote_samples[i], n, channel, &count);
            if (count < 2U) {
                need_extra = 1U;
            }
        }

        if (need_extra) {
            ++q->entries_needing_extra_samples;
            for (j = ROBUST_BASE_SAMPLES; j < ROBUST_MAX_SAMPLES; ++j) {
                read_entry_sample(i, &vote_samples[i][j], q);
            }
            n = ROBUST_MAX_SAMPLES;
        }
        vote_sample_count[i] = (unsigned char)n;

        for (channel = 0; channel < 3U; ++channel) {
            v = mode_component(vote_samples[i], n, channel, &count);
            set_component_value(&p[i], channel, v);

            if (count != n) {
                read_unstable_mask[i] |= (unsigned char)(1U << channel);
                ++q->disagreement_components;
            }
            if (count <= (n / 2U)) {
                read_unresolved_mask[i] |= (unsigned char)(1U << channel);
                ++q->unresolved_components;
            }
        }
    }
}

/* --------------------------------------------------------------------- */
/* Comparison and logging                                                 */
/* --------------------------------------------------------------------- */

static unsigned long palette_fingerprint(RGB *p)
{
    unsigned int i;
    unsigned long h;

    h = 5381UL;
    for (i = 0; i < 256; ++i) {
        h = h * 33UL + p[i].r;
        h = h * 33UL + p[i].g;
        h = h * 33UL + p[i].b;
    }
    return h;
}

static int entry_differs(RGB *a, RGB *b)
{
    return a->r != b->r || a->g != b->g || a->b != b->b;
}

static void log_sample_triplets(unsigned int index)
{
    unsigned int j;
    unsigned int n;

    if (log_file == NULL) { return; }
    n = vote_sample_count[index];
    fprintf(log_file, " samples=");
    for (j = 0; j < n; ++j) {
        if (j != 0U) { fprintf(log_file, "|"); }
        fprintf(log_file, "%u/%u/%u",
                (unsigned)vote_samples[index][j].r,
                (unsigned)vote_samples[index][j].g,
                (unsigned)vote_samples[index][j].b);
    }
}

static unsigned long compare_reader_snapshots(RGB *want, RGB *got, int log_diffs)
{
    unsigned int i;
    unsigned long bad;
    int shown;

    bad = 0UL;
    shown = 0;
    for (i = 0; i < 256; ++i) {
        if (want[i].r != got[i].r) { ++bad; }
        if (want[i].g != got[i].g) { ++bad; }
        if (want[i].b != got[i].b) { ++bad; }
        if (log_diffs && entry_differs(&want[i], &got[i]) && shown < MAX_LOG_DIFFS) {
            fprintf(log_file,
                    "    idx=%03u expected=%02u/%02u/%02u voted=%02u/%02u/%02u",
                    i,
                    (unsigned)want[i].r, (unsigned)want[i].g, (unsigned)want[i].b,
                    (unsigned)got[i].r, (unsigned)got[i].g, (unsigned)got[i].b);
            if (read_unstable_mask[i] != 0U) {
                fprintf(log_file, " [read-vote disagreement mask=%u]",
                        (unsigned)read_unstable_mask[i]);
                log_sample_triplets(i);
            } else {
                fprintf(log_file, " [stable read vote]");
            }
            fprintf(log_file, "\n");
            ++shown;
        }
    }
    return bad;
}

static void run_reader_selftest(void)
{
    unsigned int pass;
    ReadQuality q;
    unsigned long bad;

    prepare_mode13();
    robust_read_palette(reader_reference, &q);

    if (log_file != NULL) {
        fprintf(log_file, "=== ROBUST READER SELF-TEST ===\n");
        fprintf(log_file,
                "Reference: fingerprint=%08lX; raw=%lu; read disagreements=%lu; unresolved=%lu; extra-entry-samples=%lu\n",
                palette_fingerprint(reader_reference), q.raw_reads,
                q.disagreement_components, q.unresolved_components,
                q.entries_needing_extra_samples);
        fprintf(log_file,
                "Raw aliases: 3Fh=%lu 7Fh=%lu BFh=%lu FFh=%lu; high-bit raw reads=%lu\n",
                q.raw_3f, q.raw_7f, q.raw_bf, q.raw_ff, q.raw_high_bits);
    }

    for (pass = 0; pass < READER_SELFTEST_REREADS; ++pass) {
        robust_read_palette(observed, &q);
        bad = compare_reader_snapshots(reader_reference, observed, 1);
        if (log_file != NULL) {
            fprintf(log_file,
                    "[Reader reread %u] %s: %lu component mismatch(es); fingerprint=%08lX; vote disagreements=%lu; unresolved=%lu; raw=%lu\n",
                    pass + 1U, bad == 0UL ? "STABLE" : "CHANGED", bad,
                    palette_fingerprint(observed), q.disagreement_components,
                    q.unresolved_components, q.raw_reads);
        }
    }

    if (log_file != NULL) {
        fprintf(log_file, "=== END ROBUST READER SELF-TEST ===\n\n");
        fflush(log_file);
    }
}

static unsigned int component_is_unstable(unsigned int index, unsigned int channel)
{
    return (read_unstable_mask[index] & (unsigned char)(1U << channel)) != 0U;
}

static void verify_write_trial(unsigned int strategy, unsigned int round)
{
    unsigned int i;
    unsigned int channel;
    unsigned int entry_bad;
    unsigned int shown;
    unsigned long bad;
    unsigned long stable_bad;
    unsigned long noisy_bad;
    unsigned long bad_entries;
    unsigned char want;
    unsigned char got;
    ReadQuality q;
    StrategyResult *r;

    robust_read_palette(observed, &q);

    bad = 0UL;
    stable_bad = 0UL;
    noisy_bad = 0UL;
    bad_entries = 0UL;
    shown = 0U;

    for (i = 0; i < 256; ++i) {
        entry_bad = 0U;
        for (channel = 0; channel < 3U; ++channel) {
            want = component_value(&expected[i], channel);
            got = component_value(&observed[i], channel);
            if (want != got) {
                ++bad;
                entry_bad = 1U;
                if (component_is_unstable(i, channel)) {
                    ++noisy_bad;
                } else {
                    ++stable_bad;
                }
            }
        }
        if (entry_bad) {
            ++bad_entries;
        }
    }

    r = &strategy_results[strategy];
    ++r->trials;
    if (bad == 0UL) { ++r->perfect_trials; }
    if (stable_bad != 0UL) { ++r->trials_with_stable_mismatch; }
    r->bad_components += bad;
    r->stable_bad_components += stable_bad;
    r->noisy_bad_components += noisy_bad;
    r->bad_entries += bad_entries;
    r->read_disagreement_components += q.disagreement_components;
    r->read_unresolved_components += q.unresolved_components;
    r->reader_raw_reads += q.raw_reads;
    r->reader_raw_7f += q.raw_7f;
    r->reader_raw_bf += q.raw_bf;
    r->reader_raw_ff += q.raw_ff;
    if (bad > r->worst_bad_components) {
        r->worst_bad_components = bad;
    }

    if (log_file != NULL) {
        fprintf(log_file,
                "[Round %02u %-10s] %s: bad=%lu (stable-read=%lu, read-noisy=%lu), entries=%lu; reader disagreements=%lu unresolved=%lu raw=%lu; fingerprint=%08lX\n",
                round + 1U, strategy_names[strategy],
                bad == 0UL ? "PASS" : "FAIL", bad, stable_bad, noisy_bad,
                bad_entries, q.disagreement_components, q.unresolved_components,
                q.raw_reads, palette_fingerprint(observed));

        if (bad != 0UL) {
            for (i = 0; i < 256 && shown < MAX_LOG_DIFFS; ++i) {
                if (entry_differs(&expected[i], &observed[i])) {
                    fprintf(log_file,
                            "    idx=%03u expected=%02u/%02u/%02u voted=%02u/%02u/%02u unstable-mask=%u unresolved-mask=%u",
                            i,
                            (unsigned)expected[i].r, (unsigned)expected[i].g, (unsigned)expected[i].b,
                            (unsigned)observed[i].r, (unsigned)observed[i].g, (unsigned)observed[i].b,
                            (unsigned)read_unstable_mask[i],
                            (unsigned)read_unresolved_mask[i]);
                    if (read_unstable_mask[i] != 0U) {
                        log_sample_triplets(i);
                    }
                    fprintf(log_file, "\n");
                    ++shown;
                }
            }
        }
        fflush(log_file);
    }
}

/* --------------------------------------------------------------------- */
/* Interleaved write-delay experiment                                     */
/* --------------------------------------------------------------------- */

static void log_strategy_description(unsigned int strategy)
{
    if (log_file == NULL) { return; }

    if (strategy == 0U) {
        fprintf(log_file, "  %-10s : no inserted delay\n", strategy_names[strategy]);
    } else if (strategy <= 4U) {
        fprintf(log_file,
                "  %-10s : CPU-only spin after 3C8 and each 3C9; units=%u; measured~%.3f us\n",
                strategy_names[strategy], cpu_delay_units[strategy - 1U],
                cpu_delay_measured_us[strategy - 1U]);
    } else {
        fprintf(log_file,
                "  %-10s : %u OUT 80h after 3C8 and each 3C9; measured group~%.3f us\n",
                strategy_names[strategy], io80_counts[strategy - 5U],
                io80_delay_measured_us[strategy - 5U]);
    }
}

static void run_write_delay_experiment(void)
{
    unsigned int round;
    unsigned int position;
    unsigned int strategy;

    memset(strategy_results, 0, sizeof(strategy_results));

    if (log_file != NULL) {
        fprintf(log_file, "=== CPU-TIME VS I/O-SERIALIZATION WRITE EXPERIMENT ===\n");
        fprintf(log_file,
                "Run this experiment with TURBO ON for the diagnostic comparison.\n");
        fprintf(log_file,
                "Verifier: re-index each entry; %u OUT 80h after 3C7 and every 3C9 IN; 3-sample vote, adaptive 5-sample retry.\n",
                READER_DELAY_PORT80_WRITES);
        fprintf(log_file,
                "All delayed write strategies delay after the initial 3C8 and after every 3C9 write.\n");
        fprintf(log_file,
                "Each round uses the same expected palette for all strategies; order rotates; mode 13h is reset before every trial.\n\n");

        for (strategy = 0; strategy < WRITE_STRATEGIES; ++strategy) {
            log_strategy_description(strategy);
        }
        fprintf(log_file, "\n");
    }

    for (round = 0; round < WRITE_TEST_ROUNDS; ++round) {
        make_test_palette(expected, 59U + round * 11U);
        if (log_file != NULL) {
            fprintf(log_file, "--- ROUND %u/%u expected fingerprint=%08lX ---\n",
                    round + 1U, WRITE_TEST_ROUNDS,
                    palette_fingerprint(expected));
        }

        for (position = 0; position < WRITE_STRATEGIES; ++position) {
            strategy = (round + position) % WRITE_STRATEGIES;
            prepare_mode13();
            write_with_strategy(strategy, expected);
            verify_write_trial(strategy, round);
        }
        if (log_file != NULL) {
            fprintf(log_file, "\n");
        }
    }

    if (log_file != NULL) {
        fprintf(log_file, "=== WRITE DELAY SUMMARY ===\n");
        for (strategy = 0; strategy < WRITE_STRATEGIES; ++strategy) {
            StrategyResult *r;
            r = &strategy_results[strategy];
            fprintf(log_file,
                    "%-10s: trials=%u perfect=%u stable-fail-trials=%u bad=%lu stable-bad=%lu read-noisy-bad=%lu bad-entries=%lu worst=%lu reader-disagreements=%lu unresolved=%lu raw7F=%lu rawBF=%lu rawFF=%lu/%lu reads\n",
                    strategy_names[strategy],
                    r->trials, r->perfect_trials, r->trials_with_stable_mismatch,
                    r->bad_components, r->stable_bad_components,
                    r->noisy_bad_components, r->bad_entries,
                    r->worst_bad_components,
                    r->read_disagreement_components,
                    r->read_unresolved_components,
                    r->reader_raw_7f, r->reader_raw_bf, r->reader_raw_ff,
                    r->reader_raw_reads);
        }
        fprintf(log_file, "=== END WRITE DELAY SUMMARY ===\n\n");
        fflush(log_file);
    }
}

/* --------------------------------------------------------------------- */
/* Save / restore                                                         */
/* --------------------------------------------------------------------- */

static void write_restore_palette(RGB *p)
{
    unsigned int i;

    _disable();
    for (i = 0; i < 256; ++i) {
        outp(DAC_WRITE_INDEX, (unsigned char)i);
        reader_io_delay();
        outp(DAC_DATA, p[i].r & 0x3F);
        reader_io_delay();
        outp(DAC_DATA, p[i].g & 0x3F);
        reader_io_delay();
        outp(DAC_DATA, p[i].b & 0x3F);
        reader_io_delay();
    }
    _enable();
}

static void save_original_palette(void)
{
    ReadQuality q;

    robust_read_palette(saved_palette, &q);
    if (log_file != NULL) {
        fprintf(log_file,
                "Original palette robust-read fingerprint=%08lX; read disagreements=%lu unresolved=%lu\n",
                palette_fingerprint(saved_palette),
                q.disagreement_components, q.unresolved_components);
    }
}

static void restore_original_video(void)
{
    bios_set_mode(saved_mode);
    set_active_page(saved_page);
    write_restore_palette(saved_palette);
    outp(DAC_MASK, saved_mask);
}

static void print_screen_summary(void)
{
    unsigned int strategy;

    printf("\nVGAPAL4 CPU-time vs I/O-serialization experiment complete.\n");
    printf("Rounds: %u. Detailed log: VGAPAL4.TXT\n\n", WRITE_TEST_ROUNDS);
    printf("Strategy     perfect stable-fail   bad  stable-bad  noisy-bad\n");
    printf("-------------------------------------------------------------\n");
    for (strategy = 0; strategy < WRITE_STRATEGIES; ++strategy) {
        StrategyResult *r;
        r = &strategy_results[strategy];
        printf("%-10s %3u/%-3u %3u/%-3u %6lu %10lu %10lu\n",
               strategy_names[strategy],
               r->perfect_trials, r->trials,
               r->trials_with_stable_mismatch, r->trials,
               r->bad_components, r->stable_bad_components,
               r->noisy_bad_components);
    }
    printf("\nRun intended for TURBO ON. Send the complete VGAPAL4.TXT for analysis.\n");
}

int main(int argc, char **argv)
{
    unsigned char mask_after_mode;
    const char *label;

    label = (argc >= 2) ? argv[1] : "UNLABELED";
    log_file = fopen("VGAPAL4.TXT", "w");

    get_current_mode();
    saved_mask = (unsigned char)(inp(DAC_MASK) & 0xFF);

    if (log_file != NULL) {
        fprintf(log_file, "VGAPAL4 VGA DAC CPU-time vs I/O-serialization diagnostic\n");
        fprintf(log_file, "Target: 16-bit real-mode DOS, VGA mode 13h\n");
        fprintf(log_file, "Run label: %s\n", label);
        fprintf(log_file, "Original BIOS mode=%02Xh page=%u PEL mask=%02Xh\n",
                (unsigned)saved_mode, (unsigned)saved_page,
                (unsigned)saved_mask);
        fprintf(log_file,
                "Robust reader delay=%u port-80h writes per read-side DAC access\n\n",
                READER_DELAY_PORT80_WRITES);
    }

    save_original_palette();
    calibrate_delays();

    prepare_mode13();
    mask_after_mode = (unsigned char)(inp(DAC_MASK) & 0xFF);
    if (log_file != NULL) {
        fprintf(log_file, "Mode 13h PEL mask after BIOS mode set=%02Xh\n\n",
                (unsigned)mask_after_mode);
    }

    run_reader_selftest();
    run_write_delay_experiment();

    restore_original_video();

    if (log_file != NULL) {
        fprintf(log_file,
                "Program exited; original BIOS mode/page/palette and PEL mask restored.\n");
        fclose(log_file);
        log_file = NULL;
    }

    print_screen_summary();
    return 0;
}
