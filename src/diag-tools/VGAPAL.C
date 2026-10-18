/*
 * VGAPAL.C - VGA Mode 13h / DAC palette diagnostic
 * Target: 16-bit real-mode MS-DOS, Open Watcom C/C++ 1.9
 *
 * Build from a DOS/Watcom environment:
 *   wcl -bt=dos -ms VGAPAL.C
 *
 * Run from plain DOS / real-mode MS-DOS mode, not a Windows DOS box.
 * Creates VGAPAL.TXT in the current directory.
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <conio.h>
#include <dos.h>
#include <i86.h>

#define DAC_MASK       0x3C6
#define DAC_READ_INDEX 0x3C7
#define DAC_WRITE_INDEX 0x3C8
#define DAC_DATA       0x3C9
#define VGA_WIDTH      320
#define VGA_HEIGHT     200

#define AUTO_FULL_STRESS_PASSES    32
#define AUTO_PARTIAL_STRESS_PASSES 64

/*
 * Optional VGA DAC I/O delay used by the second automatic test pass.
 * One write to legacy port 80h is the classic PC-compatible I/O delay.
 * Increase this value if a longer recovery interval is desired.
 */
#define DAC_IO_DELAY_PORT          0x80
#define DAC_IO_DELAY_PORT80_WRITES 200

struct RGBColor {
    unsigned char r;
    unsigned char g;
    unsigned char b;
};
typedef struct RGBColor RGB;

static RGB saved_palette[256];
static RGB expected[256];
static RGB observed[256];
static RGB base_palette[256];
static RGB work_palette[256];
static FILE *log_file = NULL;

static unsigned char saved_mode = 3;
static unsigned char saved_page = 0;
static unsigned char saved_mask = 0xFF;

static unsigned int auto_checks = 0;
static unsigned int auto_failures = 0;
static unsigned long auto_bad_components = 0UL;
static int last_bad_entries = 0;

static int dac_io_delay_enabled = 0;

static unsigned int auto_checks_no_delay = 0;
static unsigned int auto_failures_no_delay = 0;
static unsigned long auto_bad_components_no_delay = 0UL;
static unsigned int auto_checks_with_delay = 0;
static unsigned int auto_failures_with_delay = 0;
static unsigned long auto_bad_components_with_delay = 0UL;

/* --------------------------------------------------------------------- */
/* BIOS video mode and basic DAC access                                  */
/* --------------------------------------------------------------------- */

/*
 * Insert a legacy I/O recovery delay after a VGA DAC port access.
 * The delay is deliberately based on an I/O transaction rather than a
 * CPU-speed-dependent software loop, so TURBO ON/OFF remains comparable.
 */
static void dac_io_delay(void)
{
    unsigned int i;

    if (!dac_io_delay_enabled) {
        return;
    }
    for (i = 0; i < DAC_IO_DELAY_PORT80_WRITES; ++i) {
        outp(DAC_IO_DELAY_PORT, 0);
    }
}

static void dac_out(unsigned int port, unsigned char value)
{
    outp(port, value);
    dac_io_delay();
}

static unsigned char dac_in(unsigned int port)
{
    unsigned char value;

    value = (unsigned char)(inp(port) & 0xFF);
    dac_io_delay();
    return value;
}

/* Clear the text screen through BIOS INT 10h; clrscr() is not part of
 * OpenWatcom's standard conio interface (it is commonly a Borland extension).
 */
static void clear_text_screen(void)
{
    union REGS inregs;
    union REGS outregs;

    memset(&inregs, 0, sizeof(inregs));
    memset(&outregs, 0, sizeof(outregs));
    inregs.h.ah = 0x06;       /* Scroll window up */
    inregs.h.al = 0x00;       /* Clear entire window */
    inregs.h.bh = 0x07;       /* Normal text attribute */
    inregs.w.cx = 0x0000;     /* Upper-left: row 0, column 0 */
    inregs.w.dx = 0x184F;     /* Lower-right: row 24, column 79 */
    int86(0x10, &inregs, &outregs);

    memset(&inregs, 0, sizeof(inregs));
    memset(&outregs, 0, sizeof(outregs));
    inregs.h.ah = 0x02;       /* Set cursor position */
    inregs.h.bh = 0x00;       /* Display page 0 */
    inregs.w.dx = 0x0000;     /* Row 0, column 0 */
    int86(0x10, &inregs, &outregs);
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

static void copy_palette(RGB *dst, RGB *src)
{
    unsigned int i;
    for (i = 0; i < 256; ++i) {
        dst[i] = src[i];
    }
}

/*
 * Read all 256 DAC entries. A VGA DAC read is: write index to 3C7h,
 * then read R, G, B from 3C9h. Interrupts are disabled so an ISR/TSR
 * cannot interrupt the 3-byte sequence. VGA component values are 6-bit.
 */
static void read_palette(RGB *p)
{
    unsigned int i;

    _disable();
    dac_out(DAC_READ_INDEX, 0);
    for (i = 0; i < 256; ++i) {
        p[i].r = (unsigned char)(dac_in(DAC_DATA) & 0x3F);
        p[i].g = (unsigned char)(dac_in(DAC_DATA) & 0x3F);
        p[i].b = (unsigned char)(dac_in(DAC_DATA) & 0x3F);
    }
    _enable();
}

/* Write all 256 entries in a single sequential DAC write transaction. */
static void write_palette(RGB *p, int protect_interrupts)
{
    unsigned int i;

    if (protect_interrupts) {
        _disable();
    }
    dac_out(DAC_WRITE_INDEX, 0);
    for (i = 0; i < 256; ++i) {
        dac_out(DAC_DATA, p[i].r & 0x3F);
        dac_out(DAC_DATA, p[i].g & 0x3F);
        dac_out(DAC_DATA, p[i].b & 0x3F);
    }
    if (protect_interrupts) {
        _enable();
    }
}

/* Write a consecutive span without crossing palette index 255. */
static void write_palette_span(RGB *p, unsigned int start,
                               unsigned int count, int protect_interrupts)
{
    unsigned int k;

    if (protect_interrupts) {
        _disable();
    }
    dac_out(DAC_WRITE_INDEX, (unsigned char)(start & 0xFF));
    for (k = 0; k < count; ++k) {
        dac_out(DAC_DATA, p[start + k].r & 0x3F);
        dac_out(DAC_DATA, p[start + k].g & 0x3F);
        dac_out(DAC_DATA, p[start + k].b & 0x3F);
    }
    if (protect_interrupts) {
        _enable();
    }
}

/* Write a span that deliberately wraps from index 255 back to index 0. */
static void write_palette_wrapped(RGB *p, unsigned int start,
                                  unsigned int count, int protect_interrupts)
{
    unsigned int k;
    unsigned int idx;

    if (protect_interrupts) {
        _disable();
    }
    dac_out(DAC_WRITE_INDEX, (unsigned char)(start & 0xFF));
    for (k = 0; k < count; ++k) {
        idx = (start + k) & 0xFF;
        dac_out(DAC_DATA, p[idx].r & 0x3F);
        dac_out(DAC_DATA, p[idx].g & 0x3F);
        dac_out(DAC_DATA, p[idx].b & 0x3F);
    }
    if (protect_interrupts) {
        _enable();
    }
}

/* order: 0=ascending, 1=descending, 2=permuted index order. */
static void write_palette_individually(RGB *p, int order,
                                       int protect_each_entry)
{
    unsigned int n;
    unsigned int idx;

    for (n = 0; n < 256; ++n) {
        if (order == 0) {
            idx = n;
        } else if (order == 1) {
            idx = 255 - n;
        } else {
            /* 73 is odd, so this visits every 8-bit index exactly once. */
            idx = (n * 73U) & 0xFF;
        }

        if (protect_each_entry) {
            _disable();
        }
        dac_out(DAC_WRITE_INDEX, (unsigned char)idx);
        dac_out(DAC_DATA, p[idx].r & 0x3F);
        dac_out(DAC_DATA, p[idx].g & 0x3F);
        dac_out(DAC_DATA, p[idx].b & 0x3F);
        if (protect_each_entry) {
            _enable();
        }
    }
}

static void prepare_mode13(void)
{
    bios_set_mode(0x13);
    dac_out(DAC_MASK, 0xFF);
}

/* --------------------------------------------------------------------- */
/* Palette generators                                                    */
/* --------------------------------------------------------------------- */

/* A deliberately non-default, index-sensitive pattern. */
static void make_test_palette(RGB *p, unsigned int phase)
{
    unsigned int i;
    for (i = 0; i < 256; ++i) {
        p[i].r = (unsigned char)((i + phase * 7U) & 0x3F);
        p[i].g = (unsigned char)(((i >> 2) + phase * 11U) & 0x3F);
        p[i].b = (unsigned char)((i * 13U + phase * 17U) & 0x3F);
    }
}

static void make_band_palette(RGB *p)
{
    unsigned int i;
    for (i = 0; i < 64; ++i) {
        p[i].r = (unsigned char)i;
        p[i].g = 0;
        p[i].b = 0;

        p[64 + i].r = 0;
        p[64 + i].g = (unsigned char)i;
        p[64 + i].b = 0;

        p[128 + i].r = 0;
        p[128 + i].g = 0;
        p[128 + i].b = (unsigned char)i;

        p[192 + i].r = (unsigned char)i;
        p[192 + i].g = (unsigned char)i;
        p[192 + i].b = (unsigned char)i;
    }
}

static void make_gray_palette(RGB *p)
{
    unsigned int i;
    unsigned char level;
    for (i = 0; i < 256; ++i) {
        level = (unsigned char)(i >> 2);
        p[i].r = level;
        p[i].g = level;
        p[i].b = level;
    }
}

/* 3:3:2 RGB cube: blue changes fastest, then green, then red. */
static void make_cube_palette(RGB *p)
{
    unsigned int i;
    for (i = 0; i < 256; ++i) {
        p[i].r = (unsigned char)(((i >> 5) & 7) * 9);
        p[i].g = (unsigned char)(((i >> 2) & 7) * 9);
        p[i].b = (unsigned char)((i & 3) * 21);
    }
}

/* Six-segment RGB wheel, component values 0..63. */
static void wheel_color(unsigned int index, RGB *c)
{
    unsigned int v;
    unsigned int segment;
    unsigned int level;

    v = (index & 0xFF) * 6U;
    segment = v >> 8;
    level = ((v & 0xFF) * 63U) / 255U;

    if (segment == 0) {
        c->r = 63; c->g = (unsigned char)level; c->b = 0;
    } else if (segment == 1) {
        c->r = (unsigned char)(63 - level); c->g = 63; c->b = 0;
    } else if (segment == 2) {
        c->r = 0; c->g = 63; c->b = (unsigned char)level;
    } else if (segment == 3) {
        c->r = 0; c->g = (unsigned char)(63 - level); c->b = 63;
    } else if (segment == 4) {
        c->r = (unsigned char)level; c->g = 0; c->b = 63;
    } else {
        c->r = 63; c->g = 0; c->b = (unsigned char)(63 - level);
    }
}

static void make_wheel_palette(RGB *p, unsigned int phase)
{
    unsigned int i;
    for (i = 0; i < 256; ++i) {
        wheel_color((i + phase) & 0xFF, &p[i]);
    }
}

/* --------------------------------------------------------------------- */
/* Palette readback comparison and logging                               */
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

static unsigned long compare_palettes(RGB *want, RGB *got, int *bad_entries)
{
    unsigned int i;
    unsigned long bad_components;
    int entry_bad;

    bad_components = 0UL;
    *bad_entries = 0;

    for (i = 0; i < 256; ++i) {
        entry_bad = 0;
        if (want[i].r != got[i].r) { ++bad_components; entry_bad = 1; }
        if (want[i].g != got[i].g) { ++bad_components; entry_bad = 1; }
        if (want[i].b != got[i].b) { ++bad_components; entry_bad = 1; }
        if (entry_bad) {
            ++(*bad_entries);
        }
    }
    return bad_components;
}

static void log_palette_differences(RGB *want, RGB *got)
{
    unsigned int i;
    int shown;

    shown = 0;
    if (log_file == NULL) {
        return;
    }
    for (i = 0; i < 256 && shown < 12; ++i) {
        if (want[i].r != got[i].r || want[i].g != got[i].g || want[i].b != got[i].b) {
            fprintf(log_file,
                "    idx=%03u expected=%02u/%02u/%02u read=%02u/%02u/%02u\n",
                (unsigned)i,
                (unsigned)want[i].r, (unsigned)want[i].g, (unsigned)want[i].b,
                (unsigned)got[i].r, (unsigned)got[i].g, (unsigned)got[i].b);
            ++shown;
        }
    }
}

static unsigned long verify_palette(const char *name, RGB *want)
{
    unsigned long bad;
    int entries;

    read_palette(observed);
    bad = compare_palettes(want, observed, &entries);
    last_bad_entries = entries;

    if (log_file != NULL) {
        fprintf(log_file, "[%s] %s: %lu component mismatch(es), %d palette entr(y/ies); fingerprint=%08lX\n",
                name, bad == 0UL ? "PASS" : "FAIL", bad, entries,
                palette_fingerprint(observed));
        if (bad == 0UL) {
            fprintf(log_file, "    All 768 RGB component values match.\n");
        } else {
            log_palette_differences(want, observed);
        }
        fflush(log_file);
    }
    return bad;
}

static void auto_verify(const char *name, RGB *want)
{
    unsigned long bad;
    bad = verify_palette(name, want);
    ++auto_checks;
    auto_bad_components += bad;
    if (bad != 0UL) {
        ++auto_failures;
    }
}

/* --------------------------------------------------------------------- */
/* Automated electrical/protocol tests                                    */
/* --------------------------------------------------------------------- */

static void run_automatic_tests_variant(int use_delay)
{
    unsigned int k;
    unsigned int pass;
    unsigned int idx;
    RGB c;
    char label[72];
    unsigned char mask_now;

    auto_checks = 0;
    auto_failures = 0;
    auto_bad_components = 0UL;
    dac_io_delay_enabled = use_delay;

    /* Give both variants the same BIOS mode-13h starting point. */
    prepare_mode13();
    mask_now = dac_in(DAC_MASK);
    if (log_file != NULL) {
        fprintf(log_file, "\n=== AUTOMATIC DAC TESTS: %s ===\n",
                use_delay ? "WITH I/O DELAY" : "NO DELAY");
        fprintf(log_file, "I/O delay after each VGA DAC access: %u write(s) to port %02Xh.\n",
                use_delay ? DAC_IO_DELAY_PORT80_WRITES : 0U, DAC_IO_DELAY_PORT);
        fprintf(log_file, "PEL mask immediately after entering mode 13h: %02Xh\n",
                (unsigned)mask_now);
        if (mask_now != 0xFF) {
            fprintf(log_file, "NOTE: BIOS mode 13h did not leave the PEL mask at FFh. Program forces FFh for tests.\n");
        }
        fprintf(log_file, "\n");
    }
    dac_out(DAC_MASK, 0xFF);

    /* Sequential 768-byte write; protected against interrupts. */
    make_test_palette(expected, 3);
    write_palette(expected, 1);
    auto_verify("A1 full sequential write, interrupts disabled", expected);
    auto_verify("A2 second read of same palette, no intervening write", expected);

    /* Same transaction with interrupts enabled. */
    make_test_palette(expected, 9);
    write_palette(expected, 0);
    auto_verify("B1 full sequential write, interrupts enabled", expected);

    /* Re-select the index before every RGB triple, in different orders. */
    make_test_palette(expected, 5);
    write_palette_individually(expected, 0, 1);
    auto_verify("C1 individual entries ascending, protected RGB triples", expected);

    make_test_palette(expected, 13);
    write_palette_individually(expected, 1, 1);
    auto_verify("D1 individual entries descending, protected RGB triples", expected);

    make_test_palette(expected, 23);
    write_palette_individually(expected, 2, 1);
    auto_verify("E1 individual entries permuted, protected RGB triples", expected);

    /* Index auto-increment wrapping across entry 255 -> entry 0. */
    make_test_palette(expected, 7);
    write_palette(expected, 1);
    for (k = 0; k < 12; ++k) {
        idx = (250U + k) & 0xFF;
        expected[idx].r = (unsigned char)((k * 5U + 1U) & 0x3F);
        expected[idx].g = (unsigned char)((k * 11U + 7U) & 0x3F);
        expected[idx].b = (unsigned char)((k * 17U + 13U) & 0x3F);
    }
    write_palette_wrapped(expected, 250, 12, 1);
    auto_verify("F1 sequential write crossing index 255 -> 0", expected);

    /* Full palette changed repeatedly; verify after EVERY complete write. */
    if (log_file != NULL) {
        fprintf(log_file, "\n--- Full-palette stress: %u passes, IRQs enabled during write ---\n",
                AUTO_FULL_STRESS_PASSES);
    }
    for (pass = 0; pass < AUTO_FULL_STRESS_PASSES; ++pass) {
        make_test_palette(expected, pass + 31U);
        write_palette(expected, 0);
        sprintf(label, "G full stress pass %u", (unsigned)(pass + 1));
        auto_verify(label, expected);
    }

    /* Simulate Windows-style updates of just a 32-entry animated banner. */
    make_test_palette(base_palette, 17);
    write_palette(base_palette, 1);
    if (log_file != NULL) {
        fprintf(log_file, "\n--- Partial-palette stress: %u updates to entries 224..255 ---\n",
                AUTO_PARTIAL_STRESS_PASSES);
    }
    for (pass = 0; pass < AUTO_PARTIAL_STRESS_PASSES; ++pass) {
        copy_palette(expected, base_palette);
        for (k = 0; k < 32; ++k) {
            wheel_color((k * 8U + pass * 3U) & 0xFF, &c);
            expected[224 + k] = c;
        }
        write_palette_span(expected, 224, 32, 0);
        sprintf(label, "H partial-banner stress pass %u", (unsigned)(pass + 1));
        auto_verify(label, expected);
    }

    if (use_delay) {
        auto_checks_with_delay = auto_checks;
        auto_failures_with_delay = auto_failures;
        auto_bad_components_with_delay = auto_bad_components;
    } else {
        auto_checks_no_delay = auto_checks;
        auto_failures_no_delay = auto_failures;
        auto_bad_components_no_delay = auto_bad_components;
    }

    if (log_file != NULL) {
        fprintf(log_file, "\nAUTOMATIC SUMMARY (%s): %u checks, %u failed check(s), %lu mismatched RGB component(s) in total.\n",
                use_delay ? "WITH I/O DELAY" : "NO DELAY",
                auto_checks, auto_failures, auto_bad_components);
        fprintf(log_file, "=== END AUTOMATIC DAC TESTS: %s ===\n\n",
                use_delay ? "WITH I/O DELAY" : "NO DELAY");
        fflush(log_file);
    }
}

static void run_automatic_tests(void)
{
    run_automatic_tests_variant(0);
    run_automatic_tests_variant(1);

    /* Keep interactive visual tests identical to the original program. */
    dac_io_delay_enabled = 0;

    if (log_file != NULL) {
        fprintf(log_file, "=== DELAY COMPARISON ===\n");
        fprintf(log_file, "NO DELAY : %u checks, %u failed, %lu mismatched RGB components.\n",
                auto_checks_no_delay, auto_failures_no_delay,
                auto_bad_components_no_delay);
        fprintf(log_file, "WITH DELAY: %u checks, %u failed, %lu mismatched RGB components.\n",
                auto_checks_with_delay, auto_failures_with_delay,
                auto_bad_components_with_delay);
        fprintf(log_file, "=== END DELAY COMPARISON ===\n\n");
        fflush(log_file);
    }
}

/* --------------------------------------------------------------------- */
/* Visual patterns in mode 13h                                            */
/* --------------------------------------------------------------------- */

static void draw_channel_bands(void)
{
    unsigned char __far *vram;
    unsigned char __far *line;
    unsigned int x;
    unsigned int y;
    unsigned int band;
    unsigned int level;

    vram = (unsigned char __far *)MK_FP(0xA000, 0);
    for (y = 0; y < VGA_HEIGHT; ++y) {
        line = vram + (unsigned long)y * 320UL;
        band = y / 50;
        for (x = 0; x < VGA_WIDTH; ++x) {
            level = x / 5;
            line[x] = (unsigned char)(band * 64 + level);
        }
    }
}

static void draw_gray_ramp(void)
{
    unsigned char __far *vram;
    unsigned char __far *line;
    unsigned int x;
    unsigned int y;
    unsigned int index;

    vram = (unsigned char __far *)MK_FP(0xA000, 0);
    for (y = 0; y < VGA_HEIGHT; ++y) {
        line = vram + (unsigned long)y * 320UL;
        for (x = 0; x < VGA_WIDTH; ++x) {
            index = (x * 4U) / 5U;
            line[x] = (unsigned char)index;
        }
    }
}

static void draw_cube_grid(void)
{
    unsigned char __far *vram;
    unsigned char __far *line;
    unsigned int x;
    unsigned int y;
    unsigned int col;
    unsigned int row;

    vram = (unsigned char __far *)MK_FP(0xA000, 0);
    for (y = 0; y < VGA_HEIGHT; ++y) {
        line = vram + (unsigned long)y * 320UL;
        row = (y * 16U) / VGA_HEIGHT;
        for (x = 0; x < VGA_WIDTH; ++x) {
            col = x / 20;
            line[x] = (unsigned char)(row * 16 + col);
        }
    }
}

/* Top 160 lines use only entries 0..223; bottom banner uses 224..255. */
static void draw_banner_pattern(void)
{
    unsigned char __far *vram;
    unsigned char __far *line;
    unsigned int x;
    unsigned int y;
    unsigned int row;
    unsigned int col;

    vram = (unsigned char __far *)MK_FP(0xA000, 0);
    for (y = 0; y < 160; ++y) {
        line = vram + (unsigned long)y * 320UL;
        row = y / 10;
        for (x = 0; x < VGA_WIDTH; ++x) {
            col = x / 23;
            line[x] = (unsigned char)(row * 14 + col);
        }
    }
    for (y = 160; y < VGA_HEIGHT; ++y) {
        line = vram + (unsigned long)y * 320UL;
        for (x = 0; x < VGA_WIDTH; ++x) {
            line[x] = (unsigned char)(224 + x / 10);
        }
    }
}

static void draw_full_wheel_ramp(void)
{
    draw_gray_ramp();
}

/* --------------------------------------------------------------------- */
/* Interactive visual tests                                              */
/* --------------------------------------------------------------------- */

static void return_to_text_and_report(const char *label)
{
    unsigned long bad;

    bad = verify_palette(label, expected);
    bios_set_mode(0x03);
    printf("\n%s\n", label);
    if (bad == 0UL) {
        printf("DAC readback: PASS (all 768 RGB components match).\n");
    } else {
        printf("DAC readback: FAIL (%lu component mismatches in %d entries).\n",
               bad, last_bad_entries);
        printf("See VGAPAL.TXT for the first mismatching entries.\n");
    }
}

static void visual_channel_bands(void)
{
    prepare_mode13();
    make_band_palette(expected);
    write_palette(expected, 1);
    draw_channel_bands();
    getch();
    return_to_text_and_report("VISUAL 1: red / green / blue / grayscale bands");
}

static void visual_gray_ramp(void)
{
    prepare_mode13();
    make_gray_palette(expected);
    write_palette(expected, 1);
    draw_gray_ramp();
    getch();
    return_to_text_and_report("VISUAL 2: 256-index grayscale ramp (64 DAC intensity levels)");
}

static void visual_cube_grid(void)
{
    prepare_mode13();
    make_cube_palette(expected);
    write_palette(expected, 1);
    draw_cube_grid();
    getch();
    return_to_text_and_report("VISUAL 3: 3:3:2 RGB color cube, 16x16 index grid");
}

static void visual_full_palette_animation(void)
{
    unsigned int phase;

    prepare_mode13();
    make_wheel_palette(expected, 0);
    write_palette(expected, 1);
    draw_full_wheel_ramp();
    phase = 1;

    /* Framebuffer stays unchanged; only the full DAC palette is rotated. */
    while (!kbhit()) {
        make_wheel_palette(expected, phase);
        write_palette(expected, 0);
        phase = (phase + 1U) & 0xFF;
        delay(20);
    }
    getch();
    return_to_text_and_report("VISUAL 4: full-palette cycling (framebuffer unchanged)");
}

static void visual_bios_default_palette(void)
{
    prepare_mode13();
    /* Capture what the VGA BIOS loaded; do not write any DAC palette entry. */
    read_palette(expected);
    if (log_file != NULL) {
        fprintf(log_file, "\n[VISUAL 6 setup] BIOS-loaded mode 13h palette fingerprint=%08lX\n",
                palette_fingerprint(expected));
        fflush(log_file);
    }
    draw_gray_ramp();
    getch();
    return_to_text_and_report("VISUAL 6: BIOS default mode 13h palette, no palette writes by this test");
}

static void visual_partial_banner_animation(void)
{
    unsigned int phase;
    unsigned int k;

    prepare_mode13();
    make_cube_palette(expected);
    for (k = 0; k < 32; ++k) {
        wheel_color(k * 8U, &expected[224 + k]);
    }
    write_palette(expected, 1);
    draw_banner_pattern();
    phase = 1;

    /* Only DAC entries 224..255 change; the pixel buffer stays unchanged. */
    while (!kbhit()) {
        for (k = 0; k < 32; ++k) {
            wheel_color((k * 8U + phase * 3U) & 0xFF, &expected[224 + k]);
        }
        write_palette_span(expected, 224, 32, 0);
        phase = (phase + 1U) & 0xFF;
        delay(25);
    }
    getch();
    return_to_text_and_report("VISUAL 5: animated 32-entry banner; upper image should stay unchanged");
}

static void print_menu(void)
{
    clear_text_screen();
    printf("VGAPAL - VGA DAC palette diagnostic (OpenWatcom 1.9 / DOS 16-bit)\n");
    printf("=================================================================\n\n");
    printf("Automatic tests on this run (same tests executed twice):\n");
    printf("  NO DELAY : %u checks, %u failed, %lu RGB component mismatch(es).\n",
           auto_checks_no_delay, auto_failures_no_delay, auto_bad_components_no_delay);
    printf("  WITH DELAY: %u checks, %u failed, %lu RGB component mismatch(es).\n",
           auto_checks_with_delay, auto_failures_with_delay, auto_bad_components_with_delay);
    printf("Log file: VGAPAL.TXT\n\n");
    printf("  1  Static palette: red / green / blue / grayscale bands\n");
    printf("  2  Static palette: grayscale ramp\n");
    printf("  3  Static palette: 3:3:2 RGB color cube (16x16 blocks)\n");
    printf("  4  Animate the entire 256-entry palette (press a key to stop)\n");
    printf("  5  Animate only DAC entries 224..255 (press a key to stop)\n");
    printf("  6  Display the BIOS-loaded mode 13h palette without rewriting it\n");
    printf("  R  Rerun all automatic write/read/stress tests\n");
    printf("  Q  Quit and restore the original video mode/palette\n\n");
    printf("For static tests, press any key after inspecting the image.\n");
    printf("For animations, press any key to stop and read back the final palette.\n");
    printf("Run in real-mode DOS, not a Windows DOS window.\n\n");
    printf("Choice: ");
}

static void restore_original_video(void)
{
    bios_set_mode(saved_mode);
    set_active_page(saved_page);
    write_palette(saved_palette, 1);
    dac_out(DAC_MASK, saved_mask);
}

int main(void)
{
    unsigned char mask_after_mode;
    int ch;

    log_file = fopen("VGAPAL.TXT", "w");
    get_current_mode();
    saved_mask = (unsigned char)dac_in(DAC_MASK);
    read_palette(saved_palette);

    if (log_file != NULL) {
        fprintf(log_file, "VGAPAL diagnostic log\n");
        fprintf(log_file, "Target: 16-bit real-mode DOS, VGA mode 13h (320x200x256)\n");
        fprintf(log_file, "Automatic suite runs twice: first without delay, then with port-80h I/O delay after every DAC access.\n");
        fprintf(log_file, "Original BIOS mode=%02Xh, page=%u, PEL mask=%02Xh\n",
                (unsigned)saved_mode, (unsigned)saved_page, (unsigned)saved_mask);
        fprintf(log_file, "Original DAC fingerprint=%08lX\n\n",
                palette_fingerprint(saved_palette));
        fflush(log_file);
    }

    prepare_mode13();
    mask_after_mode = (unsigned char)dac_in(DAC_MASK);
    read_palette(observed);
    if (log_file != NULL) {
        fprintf(log_file, "Mode 13h BIOS palette fingerprint=%08lX; PEL mask after BIOS mode set=%02Xh\n\n",
                palette_fingerprint(observed), (unsigned)mask_after_mode);
        fflush(log_file);
    }
    run_automatic_tests();
    bios_set_mode(0x03);

    for (;;) {
        print_menu();
        ch = toupper(getch());

        if (ch == '1') {
            printf("\nFour horizontal bands should be smooth: red, green, blue, then grayscale.\n");
            printf("Inspect the image; press any key there to return.\n");
            visual_channel_bands();
            getch();
        } else if (ch == '2') {
            printf("\nThe image should be a smooth left-to-right grayscale ramp.\n");
            printf("Inspect the image; press any key there to return.\n");
            visual_gray_ramp();
            getch();
        } else if (ch == '3') {
            printf("\nA 16x16 grid of orderly RGB colors should appear.\n");
            printf("Inspect the image; press any key there to return.\n");
            visual_cube_grid();
            getch();
        } else if (ch == '4') {
            printf("\nThe color ramp should cycle while its pixel data remains fixed.\n");
            printf("Press any key to stop and return.\n");
            visual_full_palette_animation();
            getch();
        } else if (ch == '5') {
            printf("\nOnly the bottom banner should animate; the upper pattern should remain stable.\n");
            printf("Press any key to stop and return.\n");
            visual_partial_banner_animation();
            getch();
        } else if (ch == '6') {
            printf("\nThis shows the palette loaded by BIOS INT 10h mode 13h, without writing DAC entries.\n");
            printf("Inspect the ramp; press any key there to return.\n");
            visual_bios_default_palette();
            getch();
        } else if (ch == 'R') {
            prepare_mode13();
            run_automatic_tests();
            bios_set_mode(0x03);
            printf("\nAutomatic tests complete; each test was executed twice.\n");
            printf("NO DELAY : %u checks, %u failed, %lu RGB component mismatch(es).\n",
                   auto_checks_no_delay, auto_failures_no_delay, auto_bad_components_no_delay);
            printf("WITH DELAY: %u checks, %u failed, %lu RGB component mismatch(es).\n",
                   auto_checks_with_delay, auto_failures_with_delay, auto_bad_components_with_delay);
            printf("Detailed results are in VGAPAL.TXT. Press any key for the menu.\n");
            getch();
        } else if (ch == 'Q') {
            break;
        }
    }

    restore_original_video();
    if (log_file != NULL) {
        fprintf(log_file, "\nProgram exited; original BIOS mode/palette and PEL mask restored.\n");
        fclose(log_file);
        log_file = NULL;
    }
    return 0;
}
