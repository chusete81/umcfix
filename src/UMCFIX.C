/*
 * UMCFIX.C - Permanent DOS startup fix for the UMC UM8886A PCI timing issue
 *
 * Target: 16-bit real-mode DOS, Open Watcom C/C++ 1.9
 * Build : wcl -bt=dos -ms UMCFIX.C
 *
 * PURPOSE
 * -------
 * Apply the configuration proven stable on the target M915i/UMC system:
 *
 *   UM8886A PCI configuration register A4h: 01h -> 00h
 *
 * Empirical validation on the target machine showed that A4h=00h fixes:
 *   - VGA DAC/palette corruption on S3 PCI graphics adapters
 *   - PCI Ethernet instability / packet loss with a Realtek RTL8029
 *
 * while leaving CPU and RAM benchmark throughput essentially unchanged.
 *
 * This is intentionally NOT a general UM8886 tuning tool.  It accepts only
 * the exact known states 01h (BIOS baseline) and 00h (known-good fix).  Any
 * other A4h value is refused.  No TEST10/11 or RESTORE functionality exists.
 *
 * Commands:
 *   UMCFIX             Apply fix, verbose
 *   UMCFIX /Q          Apply fix, quiet on success
 *   UMCFIX /CHECK      Read-only status check
 *   UMCFIX /CHECK /Q   Read-only status check, quiet
 *
 * /CHECK exit codes:
 *   0  A4h=00h: fix already active
 *   1  A4h=01h: known baseline; fix is applicable but was not written
 *   2+ error/refusal
 *
 * Normal apply exit code:
 *   0  success (already fixed or changed and verified)
 *   2+ error/refusal
 *
 * Intended AUTOEXEC.BAT use:
 *   C:\UTIL\UMCFIX.EXE /Q
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <i86.h>

#define UMCFIX_VERSION          "1.0"

#define PCI_BIOS_PRESENT        0xB101
#define PCI_READ_CONFIG_BYTE    0xB108
#define PCI_READ_CONFIG_WORD    0xB109
#define PCI_WRITE_CONFIG_BYTE   0xB10B
#define PCI_SUCCESS             0x00

#define UMC_VENDOR_ID           0x1060
#define UM8881F_DEVICE_ID       0x8881
#define UM8886A_DEVICE_ID       0x886A

#define UM8881_CLASS            0x06
#define UM8881_SUBCLASS         0x00
#define UM8881_REVISION         0x01

#define UM8886_CLASS            0x06
#define UM8886_SUBCLASS         0x01
#define UM8886_REVISION         0x01

#define UM8881_REG52            0x52
#define UM8881_REG53            0x53
#define UM8881_BASE52           0xB4
#define UM8881_BASE53           0x87

#define UM8886_REG_A4           0xA4
#define UM8886_A4_BASE          0x01
#define UM8886_A4_FIXED         0x00

#define WINDOW_FIRST            0xA0
#define WINDOW_LEN              16

#define EXIT_OK                 0
#define EXIT_CHECK_NEEDS_FIX    1
#define EXIT_USAGE              2
#define EXIT_NO_PCI_BIOS        3
#define EXIT_DEVICE_DISCOVERY   4
#define EXIT_IDENTITY           5
#define EXIT_GUARD_STATE        6
#define EXIT_UNEXPECTED_A4      7
#define EXIT_WRITE_FAILED       8
#define EXIT_VERIFY_FAILED      9

struct PciDevice {
    unsigned char bus;
    unsigned char devfn;
    unsigned int vendor;
    unsigned int device;
    unsigned char revision;
    unsigned char prog_if;
    unsigned char subclass;
    unsigned char class_code;
    unsigned char header_type;
};
typedef struct PciDevice PciDevice;

static unsigned char pci_last_bus = 0;
static unsigned char pci_version_major = 0;
static unsigned char pci_version_minor_bcd = 0;
static int quiet = 0;

static int strieq(const char *a, const char *b)
{
    unsigned char ca;
    unsigned char cb;

    while (*a != '\0' && *b != '\0') {
        ca = (unsigned char)toupper((unsigned char)*a);
        cb = (unsigned char)toupper((unsigned char)*b);
        if (ca != cb) {
            return 0;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static void info(const char *s)
{
    if (!quiet) {
        fputs(s, stdout);
    }
}

static int pci_bios_check(void)
{
    union REGS r;

    memset(&r, 0, sizeof(r));
    r.x.ax = PCI_BIOS_PRESENT;
    int86(0x1A, &r, &r);

    if (r.x.cflag || r.h.ah != PCI_SUCCESS) {
        return 0;
    }

    pci_version_major = r.h.bh;
    pci_version_minor_bcd = r.h.bl;
    pci_last_bus = r.h.cl;
    return 1;
}

static int pci_read_byte(unsigned char bus, unsigned char devfn,
                         unsigned int reg, unsigned char *value)
{
    union REGS r;

    memset(&r, 0, sizeof(r));
    r.x.ax = PCI_READ_CONFIG_BYTE;
    r.h.bh = bus;
    r.h.bl = devfn;
    r.x.di = reg;
    int86(0x1A, &r, &r);

    if (r.x.cflag || r.h.ah != PCI_SUCCESS) {
        return 0;
    }

    *value = r.h.cl;
    return 1;
}

static int pci_read_word(unsigned char bus, unsigned char devfn,
                         unsigned int reg, unsigned int *value)
{
    union REGS r;

    memset(&r, 0, sizeof(r));
    r.x.ax = PCI_READ_CONFIG_WORD;
    r.h.bh = bus;
    r.h.bl = devfn;
    r.x.di = reg;
    int86(0x1A, &r, &r);

    if (r.x.cflag || r.h.ah != PCI_SUCCESS) {
        return 0;
    }

    *value = r.x.cx;
    return 1;
}

static int pci_write_byte(unsigned char bus, unsigned char devfn,
                          unsigned int reg, unsigned char value)
{
    union REGS r;

    memset(&r, 0, sizeof(r));
    r.x.ax = PCI_WRITE_CONFIG_BYTE;
    r.h.bh = bus;
    r.h.bl = devfn;
    r.x.di = reg;
    r.h.cl = value;
    int86(0x1A, &r, &r);

    if (r.x.cflag || r.h.ah != PCI_SUCCESS) {
        return 0;
    }
    return 1;
}

static int read_device_identity(unsigned char bus, unsigned char devfn,
                                PciDevice *d)
{
    unsigned int vendor;
    unsigned int device;

    if (!pci_read_word(bus, devfn, 0x00, &vendor)) {
        return 0;
    }
    if (vendor == 0xFFFF || vendor == 0x0000) {
        return 0;
    }
    if (!pci_read_word(bus, devfn, 0x02, &device)) {
        return 0;
    }

    d->bus = bus;
    d->devfn = devfn;
    d->vendor = vendor;
    d->device = device;

    if (!pci_read_byte(bus, devfn, 0x08, &d->revision) ||
        !pci_read_byte(bus, devfn, 0x09, &d->prog_if) ||
        !pci_read_byte(bus, devfn, 0x0A, &d->subclass) ||
        !pci_read_byte(bus, devfn, 0x0B, &d->class_code) ||
        !pci_read_byte(bus, devfn, 0x0E, &d->header_type)) {
        return 0;
    }

    return 1;
}

static int find_exact_device(unsigned int wanted_device, PciDevice *found)
{
    unsigned int bus;
    unsigned int dev;
    unsigned char devfn;
    PciDevice d;
    unsigned int matches;

    matches = 0;

    /*
     * Scan function 0 only.  The target old PCI BIOS/chipset can alias
     * unsupported function numbers back to function 0.
     */
    for (bus = 0; bus <= (unsigned int)pci_last_bus; ++bus) {
        for (dev = 0; dev < 32; ++dev) {
            devfn = (unsigned char)(dev << 3);
            if (!read_device_identity((unsigned char)bus, devfn, &d)) {
                continue;
            }
            if (d.vendor == UMC_VENDOR_ID && d.device == wanted_device) {
                *found = d;
                ++matches;
            }
        }
    }

    if (matches != 1) {
        fprintf(stderr,
                "UMCFIX: ERROR: expected exactly one UMC 1060:%04X function-0 device; found %u.\n",
                wanted_device, matches);
        return 0;
    }
    return 1;
}

static int identity_is_safe(const PciDevice *d, unsigned int device,
                            unsigned char class_code,
                            unsigned char subclass,
                            unsigned char revision,
                            const char *name)
{
    if (d->vendor != UMC_VENDOR_ID || d->device != device ||
        d->class_code != class_code || d->subclass != subclass ||
        d->revision != revision || (d->header_type & 0x7F) != 0x00) {
        fprintf(stderr,
                "UMCFIX: REFUSING: unexpected %s identity/config header.\n",
                name);
        if (!quiet) {
            fprintf(stderr,
                    "  found %04X:%04X class=%02X/%02X rev=%02X hdr=%02X\n",
                    d->vendor, d->device, d->class_code, d->subclass,
                    d->revision, d->header_type);
        }
        return 0;
    }
    return 1;
}

static int stable_read_register(const PciDevice *d, unsigned int reg,
                                unsigned char *value)
{
    unsigned char a;
    unsigned char b;
    unsigned char c;

    if (!pci_read_byte(d->bus, d->devfn, reg, &a) ||
        !pci_read_byte(d->bus, d->devfn, reg, &b) ||
        !pci_read_byte(d->bus, d->devfn, reg, &c)) {
        fprintf(stderr,
                "UMCFIX: ERROR: PCI configuration read failed at %02Xh.\n",
                reg);
        return 0;
    }

    if (a != b || a != c) {
        fprintf(stderr,
                "UMCFIX: ERROR: unstable PCI configuration read at %02Xh: %02X %02X %02X.\n",
                reg, a, b, c);
        return 0;
    }

    *value = a;
    return 1;
}

static int read_window(const PciDevice *d, unsigned char *buf)
{
    unsigned int i;

    for (i = 0; i < WINDOW_LEN; ++i) {
        if (!pci_read_byte(d->bus, d->devfn, WINDOW_FIRST + i, &buf[i])) {
            return 0;
        }
    }
    return 1;
}

static int verify_only_a4_changed(const unsigned char *before,
                                  const unsigned char *after)
{
    unsigned int i;
    unsigned int reg;
    unsigned char expected;

    for (i = 0; i < WINDOW_LEN; ++i) {
        reg = WINDOW_FIRST + i;
        expected = before[i];
        if (reg == UM8886_REG_A4) {
            expected = UM8886_A4_FIXED;
        }
        if (after[i] != expected) {
            fprintf(stderr,
                    "UMCFIX: VERIFY ERROR: %02Xh expected %02X, read %02X.\n",
                    reg, expected, after[i]);
            return 0;
        }
    }
    return 1;
}

static int guard_state_ok(const PciDevice *hb)
{
    unsigned char v52;
    unsigned char v53;

    if (!stable_read_register(hb, UM8881_REG52, &v52) ||
        !stable_read_register(hb, UM8881_REG53, &v53)) {
        return 0;
    }

    if (v52 != UM8881_BASE52 || v53 != UM8881_BASE53) {
        fprintf(stderr,
                "UMCFIX: REFUSING: UM8881F guard state is 52=%02X 53=%02X; expected B4/87.\n",
                v52, v53);
        fprintf(stderr,
                "UMCFIX: Reboot/restore experimental chipset changes before applying this fix.\n");
        return 0;
    }

    if (!quiet) {
        printf("UM8881F guard state: 52=%02X 53=%02X (expected baseline)\n",
               v52, v53);
    }
    return 1;
}

static void usage(void)
{
    printf("UMCFIX %s - M915i/UMC PCI timing fix\n", UMCFIX_VERSION);
    printf("Usage:\n");
    printf("  UMCFIX             Apply known-good A4h=00h fix\n");
    printf("  UMCFIX /Q          Apply fix quietly\n");
    printf("  UMCFIX /CHECK      Read-only status check\n");
    printf("  UMCFIX /CHECK /Q   Read-only quiet status check\n");
    printf("\nIntended AUTOEXEC.BAT line:\n");
    printf("  C:\\UTIL\\UMCFIX.EXE /Q\n");
}

int main(int argc, char **argv)
{
    PciDevice isa;
    PciDevice hb;
    unsigned char a4;
    unsigned char before[WINDOW_LEN];
    unsigned char after[WINDOW_LEN];
    int check_only;
    int i;

    check_only = 0;

    for (i = 1; i < argc; ++i) {
        if (strieq(argv[i], "/Q") || strieq(argv[i], "-Q")) {
            quiet = 1;
        } else if (strieq(argv[i], "/CHECK") || strieq(argv[i], "-CHECK")) {
            check_only = 1;
        } else if (strieq(argv[i], "/?") || strieq(argv[i], "-?") ||
                   strieq(argv[i], "/H") || strieq(argv[i], "-H") ||
                   strieq(argv[i], "/HELP") || strieq(argv[i], "-HELP")) {
            usage();
            return EXIT_OK;
        } else {
            fprintf(stderr, "UMCFIX: unknown option '%s'.\n", argv[i]);
            usage();
            return EXIT_USAGE;
        }
    }

    if (!quiet) {
        printf("UMCFIX %s - M915i/UMC known-good PCI timing fix\n", UMCFIX_VERSION);
        printf("Target change: UM8886A A4h 01h -> 00h\n\n");
    }

    if (!pci_bios_check()) {
        fprintf(stderr, "UMCFIX: ERROR: PCI BIOS services not detected. No write performed.\n");
        return EXIT_NO_PCI_BIOS;
    }

    if (!quiet) {
        printf("PCI BIOS version %u.%02X, last bus %u\n",
               (unsigned int)pci_version_major,
               (unsigned int)pci_version_minor_bcd,
               (unsigned int)pci_last_bus);
    }

    if (!find_exact_device(UM8886A_DEVICE_ID, &isa) ||
        !find_exact_device(UM8881F_DEVICE_ID, &hb)) {
        return EXIT_DEVICE_DISCOVERY;
    }

    if (!identity_is_safe(&isa, UM8886A_DEVICE_ID,
                          UM8886_CLASS, UM8886_SUBCLASS,
                          UM8886_REVISION, "UM8886A") ||
        !identity_is_safe(&hb, UM8881F_DEVICE_ID,
                          UM8881_CLASS, UM8881_SUBCLASS,
                          UM8881_REVISION, "UM8881F")) {
        return EXIT_IDENTITY;
    }

    if (!stable_read_register(&isa, UM8886_REG_A4, &a4)) {
        return EXIT_UNEXPECTED_A4;
    }

    if (!quiet) {
        printf("Current UM8886A A4h=%02X\n", a4);
    }

    if (a4 == UM8886_A4_FIXED) {
        if (!guard_state_ok(&hb)) {
            return EXIT_GUARD_STATE;
        }
        info("UMCFIX: fix already active; no write needed.\n");
        return EXIT_OK;
    }

    if (a4 != UM8886_A4_BASE) {
        fprintf(stderr,
                "UMCFIX: REFUSING: unexpected UM8886A A4h=%02X; expected 01h or 00h.\n",
                a4);
        fprintf(stderr,
                "UMCFIX: This utility will not alter experimental/unknown A4h states.\n");
        return EXIT_UNEXPECTED_A4;
    }

    if (!guard_state_ok(&hb)) {
        return EXIT_GUARD_STATE;
    }

    if (check_only) {
        info("UMCFIX: known BIOS baseline detected; fix is applicable but /CHECK is read-only.\n");
        return EXIT_CHECK_NEEDS_FIX;
    }

    if (!read_window(&isa, before)) {
        fprintf(stderr,
                "UMCFIX: ERROR: could not snapshot UM8886A A0h-AFh. No write performed.\n");
        return EXIT_VERIFY_FAILED;
    }

    info("UMCFIX: applying UM8886A A4h 01h -> 00h ... ");
    if (!pci_write_byte(isa.bus, isa.devfn, UM8886_REG_A4, UM8886_A4_FIXED)) {
        if (!quiet) {
            printf("FAILED\n");
        }
        fprintf(stderr, "UMCFIX: ERROR: PCI configuration write failed.\n");
        return EXIT_WRITE_FAILED;
    }
    if (!quiet) {
        printf("OK\n");
    }

    if (!stable_read_register(&isa, UM8886_REG_A4, &a4) ||
        !read_window(&isa, after)) {
        fprintf(stderr,
                "UMCFIX: ERROR: could not verify fix. Attempting rollback to A4h=01h.\n");
        if (!pci_write_byte(isa.bus, isa.devfn, UM8886_REG_A4, UM8886_A4_BASE)) {
            fprintf(stderr,
                    "UMCFIX: ROLLBACK FAILED. Reboot/power-cycle immediately.\n");
        }
        return EXIT_VERIFY_FAILED;
    }

    if (a4 != UM8886_A4_FIXED || !verify_only_a4_changed(before, after)) {
        fprintf(stderr,
                "UMCFIX: ERROR: verification failed. Attempting rollback to A4h=01h.\n");
        if (!pci_write_byte(isa.bus, isa.devfn, UM8886_REG_A4, UM8886_A4_BASE)) {
            fprintf(stderr,
                    "UMCFIX: ROLLBACK FAILED. Reboot/power-cycle immediately.\n");
        }
        return EXIT_VERIFY_FAILED;
    }

    info("UMCFIX: fix active and verified (A4h=00h).\n");
    return EXIT_OK;
}
