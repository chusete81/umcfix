/*
 * VGAPAL3.C - focused VGA DAC write-timing diagnostic
 * Target: 16-bit real-mode MS-DOS, Open Watcom C/C++ 1.9
 *
 * Build:
 *   wcl -bt=dos -ms VGAPAL3.C
 *
 * Run from plain DOS / real-mode MS-DOS mode, not a Windows DOS box.
 * Creates VGAPAL3.TXT in the current directory.
 *
 * Purpose of this revision:
 *   - Compare six precisely defined DAC write strategies.
 *   - Keep the readback path deliberately conservative and identical for
 *     every strategy: re-index 3C7h before every sampled entry, wait after
 *     3C7h and every 3C9h IN, and use majority voting.
 *   - Distinguish write mismatches seen by a stable read vote from mismatches
 *     accompanied by read-vote disagreement.
 *   - Interleave strategies by round and reset mode 13h before each trial,
 *     reducing bias from good/bad temporal runs.
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
#define DAC_IO_DELAY_PORT80_WRITES 200

#define WRITE_STRATEGIES  6
#define WRITE_TEST_ROUNDS 16
#define ROBUST_BASE_SAMPLES 3
#define ROBUST_MAX_SAMPLES  5
#define READER_SELFTEST_REREADS 4
#define MAX_LOG_DIFFS 12

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

static const char *strategy_names[WRITE_STRATEGIES] = {
    "SEQ_FAST",
    "IDX_DELAY",
    "DATA_DELAY",
    "RGB_DELAY",
    "REINDEX_FAST",
    "REINDEX_FULL_DELAY"
};

static const char *strategy_descriptions[WRITE_STRATEGIES] = {
    "3C8 once; then all 3C9 RGB writes with no inserted delay",
    "delay only after the initial 3C8 write; 3C9 stream is fast",
    "delay after every 3C9 write (R, G and B); no delay after initial 3C8",
    "delay only after each complete RGB triple; R->G and G->B stay fast",
    "write 3C8 before every palette entry; RGB triple itself is fast",
    "write 3C8 before every entry and delay after 3C8 and every 3C9 write"
};

/* --------------------------------------------------------------------- */
/* BIOS video helpers                                                     */
/* --------------------------------------------------------------------- */

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
/* Delay and raw DAC read accounting                                      */
/* --------------------------------------------------------------------- */

static void dac_io_delay(void)
{
    unsigned int i;

    for (i = 0; i < DAC_IO_DELAY_PORT80_WRITES; ++i) {
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
    dac_io_delay();
    return v;
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
/* Six write strategies                                                   */
/*
 * These functions intentionally bypass a generic wrapper. The location of
 * every explicit delay is part of the experiment, and the fast variants do
 * not acquire an extra runtime branch/function call between DAC accesses.
 */
/* --------------------------------------------------------------------- */

static void write_seq_fast(RGB *p)
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

static void write_idx_delay(RGB *p)
{
    unsigned int i;

    _disable();
    outp(DAC_WRITE_INDEX, 0);
    dac_io_delay();
    for (i = 0; i < 256; ++i) {
        outp(DAC_DATA, p[i].r & 0x3F);
        outp(DAC_DATA, p[i].g & 0x3F);
        outp(DAC_DATA, p[i].b & 0x3F);
    }
    _enable();
}

static void write_data_delay(RGB *p)
{
    unsigned int i;

    _disable();
    outp(DAC_WRITE_INDEX, 0);
    for (i = 0; i < 256; ++i) {
        outp(DAC_DATA, p[i].r & 0x3F);
        dac_io_delay();
        outp(DAC_DATA, p[i].g & 0x3F);
        dac_io_delay();
        outp(DAC_DATA, p[i].b & 0x3F);
        dac_io_delay();
    }
    _enable();
}

static void write_rgb_delay(RGB *p)
{
    unsigned int i;

    _disable();
    outp(DAC_WRITE_INDEX, 0);
    for (i = 0; i < 256; ++i) {
        outp(DAC_DATA, p[i].r & 0x3F);
        outp(DAC_DATA, p[i].g & 0x3F);
        outp(DAC_DATA, p[i].b & 0x3F);
        dac_io_delay();
    }
    _enable();
}

static void write_reindex_fast(RGB *p)
{
    unsigned int i;

    _disable();
    for (i = 0; i < 256; ++i) {
        outp(DAC_WRITE_INDEX, (unsigned char)i);
        outp(DAC_DATA, p[i].r & 0x3F);
        outp(DAC_DATA, p[i].g & 0x3F);
        outp(DAC_DATA, p[i].b & 0x3F);
    }
    _enable();
}

static void write_reindex_full_delay(RGB *p)
{
    unsigned int i;

    _disable();
    for (i = 0; i < 256; ++i) {
        outp(DAC_WRITE_INDEX, (unsigned char)i);
        dac_io_delay();
        outp(DAC_DATA, p[i].r & 0x3F);
        dac_io_delay();
        outp(DAC_DATA, p[i].g & 0x3F);
        dac_io_delay();
        outp(DAC_DATA, p[i].b & 0x3F);
        dac_io_delay();
    }
    _enable();
}

static void write_with_strategy(unsigned int strategy, RGB *p)
{
    switch (strategy) {
    case 0: write_seq_fast(p); break;
    case 1: write_idx_delay(p); break;
    case 2: write_data_delay(p); break;
    case 3: write_rgb_delay(p); break;
    case 4: write_reindex_fast(p); break;
    default: write_reindex_full_delay(p); break;
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
    dac_io_delay();
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

/* Return the most frequent value and its count among n samples. */
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

/*
 * Re-index every sample, then read RGB with 200 port-80h writes after the
 * index write and after each 3C9h IN. Three samples are normally enough.
 * If any channel has no majority among the first three samples, two more
 * complete entry samples are taken before voting again.
 */
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
                "[Round %02u %-18s] %s: bad=%lu (stable-read=%lu, read-noisy=%lu), entries=%lu; reader disagreements=%lu unresolved=%lu raw=%lu; fingerprint=%08lX\n",
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
/* Interleaved strategy experiment                                        */
/* --------------------------------------------------------------------- */

static void run_write_strategy_experiment(void)
{
    unsigned int round;
    unsigned int position;
    unsigned int strategy;

    memset(strategy_results, 0, sizeof(strategy_results));

    if (log_file != NULL) {
        fprintf(log_file, "=== WRITE STRATEGY EXPERIMENT ===\n");
        fprintf(log_file, "Delay quantum: %u writes to port 80h.\n",
                DAC_IO_DELAY_PORT80_WRITES);
        fprintf(log_file,
                "Verifier: re-index each entry; delay after 3C7h and every 3C9h IN; 3-sample vote, adaptive 5-sample retry if no initial majority.\n");
        fprintf(log_file,
                "Each round uses the same expected palette for all six strategies. Strategy order rotates by round. Mode 13h is reset before every strategy trial.\n\n");

        for (strategy = 0; strategy < WRITE_STRATEGIES; ++strategy) {
            fprintf(log_file, "  %-18s : %s\n",
                    strategy_names[strategy], strategy_descriptions[strategy]);
        }
        fprintf(log_file, "\n");
    }

    for (round = 0; round < WRITE_TEST_ROUNDS; ++round) {
        make_test_palette(expected, 41U + round * 7U);
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
        fprintf(log_file, "=== WRITE STRATEGY SUMMARY ===\n");
        for (strategy = 0; strategy < WRITE_STRATEGIES; ++strategy) {
            StrategyResult *r;
            r = &strategy_results[strategy];
            fprintf(log_file,
                    "%-18s: trials=%u perfect=%u stable-fail-trials=%u bad=%lu stable-bad=%lu read-noisy-bad=%lu bad-entries=%lu worst=%lu reader-disagreements=%lu unresolved=%lu raw7F=%lu rawBF=%lu rawFF=%lu/%lu reads\n",
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
        fprintf(log_file, "=== END WRITE STRATEGY SUMMARY ===\n\n");
        fflush(log_file);
    }
}

/* --------------------------------------------------------------------- */
/* Save / restore                                                         */
/* --------------------------------------------------------------------- */

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
    write_reindex_full_delay(saved_palette);
    outp(DAC_MASK, saved_mask);
}

static void print_screen_summary(void)
{
    unsigned int strategy;

    printf("\nVGAPAL3 focused write-timing experiment complete.\n");
    printf("Delay quantum: %u port-80h writes. Rounds: %u.\n\n",
           DAC_IO_DELAY_PORT80_WRITES, WRITE_TEST_ROUNDS);
    printf("Strategy            perfect stable-fail  bad  stable-bad  noisy-bad\n");
    printf("------------------------------------------------------------------\n");
    for (strategy = 0; strategy < WRITE_STRATEGIES; ++strategy) {
        StrategyResult *r;
        r = &strategy_results[strategy];
        printf("%-18s %3u/%-3u %3u/%-3u %5lu %10lu %10lu\n",
               strategy_names[strategy],
               r->perfect_trials, r->trials,
               r->trials_with_stable_mismatch, r->trials,
               r->bad_components, r->stable_bad_components,
               r->noisy_bad_components);
    }
    printf("\nDetailed log: VGAPAL3.TXT\n");
}

int main(void)
{
    unsigned char mask_after_mode;

    log_file = fopen("VGAPAL3.TXT", "w");

    get_current_mode();
    saved_mask = (unsigned char)(inp(DAC_MASK) & 0xFF);

    if (log_file != NULL) {
        fprintf(log_file, "VGAPAL3 focused VGA DAC write-timing diagnostic\n");
        fprintf(log_file, "Target: 16-bit real-mode DOS, VGA mode 13h\n");
        fprintf(log_file, "Original BIOS mode=%02Xh page=%u PEL mask=%02Xh\n",
                (unsigned)saved_mode, (unsigned)saved_page,
                (unsigned)saved_mask);
        fprintf(log_file, "Configured delay=%u port-80h writes\n\n",
                DAC_IO_DELAY_PORT80_WRITES);
    }

    save_original_palette();

    prepare_mode13();
    mask_after_mode = (unsigned char)(inp(DAC_MASK) & 0xFF);
    if (log_file != NULL) {
        fprintf(log_file, "Mode 13h PEL mask after BIOS mode set=%02Xh\n\n",
                (unsigned)mask_after_mode);
    }

    run_reader_selftest();
    run_write_strategy_experiment();

    restore_original_video();

    if (log_file != NULL) {
        fprintf(log_file, "Program exited; original BIOS mode/page/palette and PEL mask restored.\n");
        fclose(log_file);
        log_file = NULL;
    }

    print_screen_summary();
    return 0;
}
