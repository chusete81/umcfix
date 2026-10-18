/*
 * TURBOPROBE.C
 *
 * Diagnostic benchmark for old 486-class PCs with a hardware TURBO switch.
 * Target: 16-bit real-mode DOS, Open Watcom C/C++ 1.9, small memory model.
 *
 * Purpose:
 *   Compare TURBO ON vs TURBO OFF without modifying chipset PCI registers.
 *   The program measures several classes of work against the PC/AT BIOS
 *   timer tick (PIT channel 0 -> IRQ0 -> BIOS data area 0040:006C).
 *
 * Tests:
 *   1. CPU-only integer/bit operations.
 *   2. Sequential RAM reads over a 32 KiB buffer.
 *   3. Sequential RAM writes over a 32 KiB buffer.
 *   4. OUT to port 80h (POST/delay port).
 *   5. IN from port 61h (system control/speaker port, read only).
 *   6. OUT to VGA DAC write-index port 3C8h (safe: changes index only).
 *   7. IN from VGA Misc Output read port 3CCh (read only).
 *   8. Back-to-back OUT pairs to VGA DAC write-index port 3C8h.
 *
 * Each test is repeated three times.  The median result is reported.
 * A single repetition lasts about 18 BIOS ticks (~0.99 s), so a complete
 * run takes roughly 25 seconds.
 *
 * IMPORTANT:
 *   - Run in real-mode DOS, not a Windows DOS box.
 *   - This program does NOT write PCI configuration registers.
 *   - It DOES perform harmless ordinary I/O to ports 80h, 3C8h, etc.
 *   - Port 3C8h changes only the VGA DAC write index; palette RAM is not
 *     modified because no data is written to 3C9h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <dos.h>
#include <i86.h>

#define BIOS_TICK_SEG       0x0040
#define BIOS_TICK_OFF       0x006C

#define TEST_TICKS          18UL
#define REPEATS             3

#define RAM_SIZE            32768U
#define CPU_BATCH_ITERS     8192U
#define IO_BATCH_ITERS      1024U
#define VGA_PAIR_ITERS      512U

#define PORT_POST           0x0080
#define PORT_SYSCTRL        0x0061
#define PORT_VGA_DAC_WIDX   0x03C8
#define PORT_VGA_MISC_R     0x03CC

static volatile unsigned char ram_buf[RAM_SIZE];
static volatile unsigned int sink16 = 0;
static volatile unsigned char sink8 = 0;

static unsigned int cpu_a = 0x1357U;
static unsigned int cpu_b = 0x9BDFU;
static unsigned char io_value = 0;

struct BenchDef {
    const char *name;
    const char *unit;
    void (*batch)(void);
    unsigned long ops_per_batch;
};

typedef struct BenchDef BenchDef;

/*
 * Read the BIOS tick counter atomically enough for this diagnostic.
 * We briefly disable interrupts so the IRQ0 handler cannot update 0040:006C
 * halfway through the 32-bit load.  This happens only at benchmark control
 * points, never during the benchmark body itself.
 */
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

/* Wait for a fresh BIOS timer edge so each repetition starts similarly. */
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

static void batch_cpu(void)
{
    unsigned int i;
    unsigned int a;
    unsigned int b;

    a = cpu_a;
    b = cpu_b;

    for (i = 0; i < CPU_BATCH_ITERS; ++i) {
        a = (unsigned int)(a + 0x9E37U);
        a ^= b;
        b = (unsigned int)((b << 1) | (b >> 15));
        b = (unsigned int)(b + a + 0x1234U);
        a = (unsigned int)((a >> 3) | (a << 13));
    }

    cpu_a = a;
    cpu_b = b;
    sink16 = (unsigned int)(a ^ b);
}

static void batch_ram_read(void)
{
    unsigned int i;
    unsigned int sum;

    sum = sink16;
    for (i = 0; i < RAM_SIZE; ++i) {
        sum = (unsigned int)(sum + ram_buf[i]);
    }
    sink16 = sum;
}

static void batch_ram_write(void)
{
    unsigned int i;
    unsigned char v;

    v = io_value;
    for (i = 0; i < RAM_SIZE; ++i) {
        ram_buf[i] = (unsigned char)(v + (unsigned char)i);
    }
    io_value = (unsigned char)(v + 1U);
}

static void batch_out80(void)
{
    unsigned int i;
    unsigned char v;

    v = io_value;
    for (i = 0; i < IO_BATCH_ITERS; ++i) {
        outp(PORT_POST, v);
        ++v;
    }
    io_value = v;
}

static void batch_in61(void)
{
    unsigned int i;
    unsigned char v;

    v = sink8;
    for (i = 0; i < IO_BATCH_ITERS; ++i) {
        v ^= (unsigned char)inp(PORT_SYSCTRL);
    }
    sink8 = v;
}

static void batch_vga_out3c8(void)
{
    unsigned int i;
    unsigned char v;

    v = io_value;
    for (i = 0; i < IO_BATCH_ITERS; ++i) {
        outp(PORT_VGA_DAC_WIDX, v);
        ++v;
    }
    io_value = v;
}

static void batch_vga_in3cc(void)
{
    unsigned int i;
    unsigned char v;

    v = sink8;
    for (i = 0; i < IO_BATCH_ITERS; ++i) {
        v ^= (unsigned char)inp(PORT_VGA_MISC_R);
    }
    sink8 = v;
}

static void batch_vga_pair3c8(void)
{
    unsigned int i;
    unsigned char v;

    v = io_value;
    for (i = 0; i < VGA_PAIR_ITERS; ++i) {
        outp(PORT_VGA_DAC_WIDX, v);
        outp(PORT_VGA_DAC_WIDX, (unsigned char)(v + 1U));
        v = (unsigned char)(v + 2U);
    }
    io_value = v;
}

static unsigned long run_one(const BenchDef *b)
{
    unsigned long start;
    unsigned long now;
    unsigned long batches;

    batches = 0UL;
    start = wait_next_tick();

    for (;;) {
        b->batch();
        ++batches;

        now = read_bios_ticks();
        if ((unsigned long)(now - start) >= TEST_TICKS) {
            break;
        }
    }

    return batches * b->ops_per_batch;
}

static unsigned long median3(unsigned long a, unsigned long b, unsigned long c)
{
    if (a > b) {
        unsigned long t = a; a = b; b = t;
    }
    if (b > c) {
        unsigned long t = b; b = c; c = t;
    }
    if (a > b) {
        unsigned long t = a; a = b; b = t;
    }
    return b;
}

static void init_ram(void)
{
    unsigned int i;

    for (i = 0; i < RAM_SIZE; ++i) {
        ram_buf[i] = (unsigned char)((i * 37U + 11U) & 0xFFU);
    }
}

static void print_rate(unsigned long ops)
{
    /* Standard PC BIOS tick rate: 18.2065 Hz. */
    double seconds;
    double rate;

    seconds = (double)TEST_TICKS / 18.2065;
    rate = (double)ops / seconds;

    if (rate >= 1000000.0) {
        printf("%9.3f M/s", rate / 1000000.0);
    } else if (rate >= 1000.0) {
        printf("%9.3f k/s", rate / 1000.0);
    } else {
        printf("%9.3f /s", rate);
    }
}

static unsigned long run_benchmark(const BenchDef *b)
{
    unsigned long r[REPEATS];
    unsigned long med;
    int i;

    printf("\n%-18s [%s]\n", b->name, b->unit);

    for (i = 0; i < REPEATS; ++i) {
        r[i] = run_one(b);
        printf("  run %d: %10lu  ", i + 1, r[i]);
        print_rate(r[i]);
        printf("\n");
    }

    med = median3(r[0], r[1], r[2]);
    printf("  MEDIAN: %10lu  ", med);
    print_rate(med);
    printf("\n");

    return med;
}

static void print_header(const char *label)
{
    unsigned long t;

    t = read_bios_ticks();

    printf("TURBOPROBE - 486 TURBO behavior probe\n");
    printf("Target: real-mode DOS / Open Watcom C 1.9\n");
    printf("Run label: %s\n", label);
    printf("Timing source: BIOS timer tick at 0040:006C (~18.2065 Hz)\n");
    printf("Ticks per repetition: %lu (~%.3f s)\n",
           TEST_TICKS, (double)TEST_TICKS / 18.2065);
    printf("Repetitions per test: %d (median reported)\n", REPEATS);
    printf("Initial BIOS tick count: %lu\n", t);
    printf("\nNo PCI configuration register is written.\n");
    printf("No VGA palette data is written; 3C8h changes DAC index only.\n");
}

int main(int argc, char **argv)
{
    static const BenchDef tests[] = {
        { "CPU_ALU",       "iterations", batch_cpu,          CPU_BATCH_ITERS },
        { "RAM_READ_32K",  "bytes",      batch_ram_read,     RAM_SIZE },
        { "RAM_WRITE_32K", "bytes",      batch_ram_write,    RAM_SIZE },
        { "OUT_80",        "OUTs",       batch_out80,        IO_BATCH_ITERS },
        { "IN_61",         "INs",        batch_in61,         IO_BATCH_ITERS },
        { "VGA_OUT_3C8",   "OUTs",       batch_vga_out3c8,   IO_BATCH_ITERS },
        { "VGA_IN_3CC",    "INs",        batch_vga_in3cc,    IO_BATCH_ITERS },
        { "VGA_3C8_PAIR",  "OUTs",       batch_vga_pair3c8,  VGA_PAIR_ITERS * 2UL }
    };
    const char *label;
    unsigned int i;
    unsigned int ntests;
    unsigned long medians[16];

    label = (argc >= 2) ? argv[1] : "UNLABELED";

    init_ram();
    print_header(label);

    ntests = (unsigned int)(sizeof(tests) / sizeof(tests[0]));
    for (i = 0; i < ntests; ++i) {
        medians[i] = run_benchmark(&tests[i]);
    }

    /* Leave the DAC write index in a predictable state. */
    outp(PORT_VGA_DAC_WIDX, 0);

    printf("\n=== COMPACT SUMMARY ===\n");
    printf("Run label: %s\n", label);
    printf("Test                 Median ops      Approx. rate\n");
    printf("----------------------------------------------------\n");
    for (i = 0; i < ntests; ++i) {
        printf("%-18s %10lu  ", tests[i].name, medians[i]);
        print_rate(medians[i]);
        printf("\n");
    }
    printf("\nFor diagnosis, compare this table between TURBO ON and OFF.\n");
    printf("\nInterpretation guide:\n");
    printf("  CPU changes strongly, I/O little  -> CPU clock/divisor mainly changes.\n");
    printf("  CPU and RAM change similarly      -> CPU/memory timing changes together.\n");
    printf("  I/O changes disproportionately    -> bus/I/O recovery timing is involved.\n");
    printf("  VGA I/O differs from 80h/61h     -> VGA/PCI legacy path is special.\n");
    printf("\n=== END ===\n");

    return 0;
}
