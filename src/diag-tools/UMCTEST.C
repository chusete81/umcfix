/*
 * UMCTEST.C - Controlled UM8881F PCI timing-register experiment
 *
 * Target: 16-bit real-mode DOS, Open Watcom C/C++ 1.9
 * Build : wcl -bt=dos -ms UMCTEST.C
 *
 * IMPORTANT:
 *   This program WRITES PCI configuration registers of the UMC UM8881F.
 *   It is intentionally tailored to the exact machine state observed during
 *   this investigation and refuses to apply a test unless all safety checks
 *   match the known baseline.
 *
 * Known baseline on the test machine:
 *   Vendor:Device = 1060:8881
 *   Class/Subclass = 06/00
 *   Revision = 01
 *   register 52h = B4h
 *   register 53h = 87h
 *
 * Reverse-engineered register interpretation being tested:
 *   52h bit 7: CPU -> PCI Post Write wait state
 *       1 = 0 WS, 0 = 1 WS
 *   53h bit 7: CPU -> PCI Burst Write enable
 *       1 = enabled, 0 = disabled
 *
 * Commands:
 *   UMCTEST SHOW
 *   UMCTEST WS1 APPLY
 *   UMCTEST NOBURST APPLY
 *   UMCTEST BOTH APPLY
 *   UMCTEST RESTORE APPLY
 *
 * The APPLY token is mandatory for every command that writes hardware.
 * Test settings remain active after the program exits so another DOS program
 * can measure them.  RESTORE returns 52h/53h to the known baseline.
 * A reboot/power cycle should also cause the BIOS to reinitialize the chipset.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <i86.h>

#define PCI_BIOS_PRESENT        0xB101
#define PCI_READ_CONFIG_BYTE    0xB108
#define PCI_READ_CONFIG_WORD    0xB109
#define PCI_WRITE_CONFIG_BYTE   0xB10B

#define PCI_SUCCESS             0x00

#define UMC_VENDOR_ID           0x1060
#define UM8881F_DEVICE_ID       0x8881

#define EXPECTED_CLASS          0x06
#define EXPECTED_SUBCLASS       0x00
#define EXPECTED_REVISION       0x01

#define REG_POSTWRITE           0x52
#define REG_BURSTWRITE          0x53

#define BASE_REG52              0xB4
#define BASE_REG53              0x87
#define WS1_REG52               0x34
#define NOBURST_REG53           0x07

#define FOCUS_FIRST             0x50
#define FOCUS_LAST              0x5F
#define FOCUS_LEN               16

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

static int find_exact_um8881f(PciDevice *found)
{
    unsigned int bus;
    unsigned int dev;
    unsigned char devfn;
    PciDevice d;
    unsigned int matches;

    matches = 0;

    /*
     * Scan function 0 only.  On this vintage BIOS/chipset, nonexistent
     * functions 1..7 were observed to alias function 0, so probing only
     * function 0 avoids counting those aliases as real devices.
     */
    for (bus = 0; bus <= (unsigned int)pci_last_bus; ++bus) {
        for (dev = 0; dev < 32; ++dev) {
            devfn = (unsigned char)(dev << 3);
            if (!read_device_identity((unsigned char)bus, devfn, &d)) {
                continue;
            }
            if (d.vendor == UMC_VENDOR_ID && d.device == UM8881F_DEVICE_ID) {
                *found = d;
                ++matches;
            }
        }
    }

    if (matches != 1) {
        printf("ERROR: Expected exactly one UMC 1060:8881 function-0 device; found %u.\n",
               matches);
        return 0;
    }
    return 1;
}

static int read_focus_window(const PciDevice *d, unsigned char *buf)
{
    unsigned int i;

    for (i = 0; i < FOCUS_LEN; ++i) {
        if (!pci_read_byte(d->bus, d->devfn, FOCUS_FIRST + i, &buf[i])) {
            return 0;
        }
    }
    return 1;
}

static void print_focus_window(const unsigned char *buf)
{
    unsigned int i;

    for (i = 0; i < FOCUS_LEN; ++i) {
        printf("%02X=%02X%s",
               FOCUS_FIRST + i,
               buf[i],
               ((i & 7U) == 7U) ? "\n" : "  ");
    }
}

static void print_device(const PciDevice *d)
{
    unsigned int dev;
    unsigned int fn;

    dev = (unsigned int)(d->devfn >> 3);
    fn = (unsigned int)(d->devfn & 7);

    printf("UM8881F candidate: %u:%u.%u  %04X:%04X\n",
           (unsigned int)d->bus, dev, fn, d->vendor, d->device);
    printf("  Class/Subclass/ProgIF=%02X/%02X/%02X  Revision=%02X  Header=%02X\n",
           d->class_code, d->subclass, d->prog_if,
           d->revision, d->header_type);
}

static int identity_is_safe(const PciDevice *d)
{
    if (d->vendor != UMC_VENDOR_ID || d->device != UM8881F_DEVICE_ID) {
        printf("REFUSING WRITE: Vendor/device mismatch.\n");
        return 0;
    }
    if (d->class_code != EXPECTED_CLASS || d->subclass != EXPECTED_SUBCLASS) {
        printf("REFUSING WRITE: Expected class/subclass 06/00.\n");
        return 0;
    }
    if (d->revision != EXPECTED_REVISION) {
        printf("REFUSING WRITE: Expected UM8881F revision 01, found %02X.\n",
               d->revision);
        return 0;
    }
    if ((d->header_type & 0x7F) != 0x00) {
        printf("REFUSING WRITE: Unexpected PCI header type %02X.\n",
               d->header_type);
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
        printf("ERROR: PCI configuration read failed at %02Xh.\n", reg);
        return 0;
    }

    if (a != b || a != c) {
        printf("ERROR: Unstable read at %02Xh: %02X %02X %02X. No write performed.\n",
               reg, a, b, c);
        return 0;
    }

    *value = a;
    return 1;
}

static int verify_only_expected_changes(const unsigned char *before,
                                        const unsigned char *after,
                                        unsigned char desired52,
                                        unsigned char desired53)
{
    unsigned int i;
    unsigned int reg;
    unsigned char expected;
    int ok;

    ok = 1;
    for (i = 0; i < FOCUS_LEN; ++i) {
        reg = FOCUS_FIRST + i;
        expected = before[i];
        if (reg == REG_POSTWRITE) {
            expected = desired52;
        } else if (reg == REG_BURSTWRITE) {
            expected = desired53;
        }

        if (after[i] != expected) {
            printf("VERIFY ERROR: offset %02Xh expected %02X, read %02X.\n",
                   reg, expected, after[i]);
            ok = 0;
        }
    }
    return ok;
}

static int write_and_verify_state(const PciDevice *d,
                                  unsigned char desired52,
                                  unsigned char desired53,
                                  int restoring)
{
    unsigned char before[FOCUS_LEN];
    unsigned char after[FOCUS_LEN];
    unsigned char v52;
    unsigned char v53;
    unsigned char original52;
    unsigned char original53;
    int wrote52;
    int wrote53;

    wrote52 = 0;
    wrote53 = 0;

    if (!stable_read_register(d, REG_POSTWRITE, &original52) ||
        !stable_read_register(d, REG_BURSTWRITE, &original53)) {
        return 0;
    }

    if (!restoring) {
        if (original52 != BASE_REG52 || original53 != BASE_REG53) {
            printf("REFUSING TEST: baseline must be 52=%02X 53=%02X, found 52=%02X 53=%02X.\n",
                   BASE_REG52, BASE_REG53, original52, original53);
            printf("Run UMCTEST RESTORE APPLY first, or reboot, then SHOW.\n");
            return 0;
        }
    } else {
        /* Restore only from the four states generated by this utility. */
        if (!((original52 == BASE_REG52 || original52 == WS1_REG52) &&
              (original53 == BASE_REG53 || original53 == NOBURST_REG53))) {
            printf("REFUSING RESTORE: current 52/53 values are not a known UMCTEST state.\n");
            printf("Current values: 52=%02X 53=%02X\n", original52, original53);
            return 0;
        }
    }

    if (!read_focus_window(d, before)) {
        printf("ERROR: Could not read 50h-5Fh before write.\n");
        return 0;
    }

    printf("Before: 52=%02X 53=%02X\n", original52, original53);
    printf("Target: 52=%02X 53=%02X\n", desired52, desired53);

    if (original52 != desired52) {
        printf("Writing register 52h: %02X -> %02X ... ", original52, desired52);
        if (!pci_write_byte(d->bus, d->devfn, REG_POSTWRITE, desired52)) {
            printf("FAILED\n");
            return 0;
        }
        wrote52 = 1;
        printf("OK\n");
    }

    if (original53 != desired53) {
        printf("Writing register 53h: %02X -> %02X ... ", original53, desired53);
        if (!pci_write_byte(d->bus, d->devfn, REG_BURSTWRITE, desired53)) {
            printf("FAILED\n");
            if (wrote52) {
                printf("Attempting rollback of 52h to %02X ... ", original52);
                if (pci_write_byte(d->bus, d->devfn, REG_POSTWRITE, original52)) {
                    printf("OK\n");
                } else {
                    printf("FAILED - REBOOT IMMEDIATELY\n");
                }
            }
            return 0;
        }
        wrote53 = 1;
        printf("OK\n");
    }

    if (!stable_read_register(d, REG_POSTWRITE, &v52) ||
        !stable_read_register(d, REG_BURSTWRITE, &v53) ||
        !read_focus_window(d, after)) {
        printf("ERROR: Could not verify new state. Attempting rollback.\n");
        goto rollback;
    }

    if (v52 != desired52 || v53 != desired53) {
        printf("VERIFY ERROR: wanted 52=%02X 53=%02X, got 52=%02X 53=%02X.\n",
               desired52, desired53, v52, v53);
        goto rollback;
    }

    if (!verify_only_expected_changes(before, after, desired52, desired53)) {
        printf("Unexpected change inside 50h-5Fh. Attempting rollback.\n");
        goto rollback;
    }

    printf("Verification OK. Current focus window:\n");
    print_focus_window(after);
    return 1;

rollback:
    if (wrote53) {
        printf("Rollback 53h -> %02X ... ", original53);
        if (pci_write_byte(d->bus, d->devfn, REG_BURSTWRITE, original53)) {
            printf("OK\n");
        } else {
            printf("FAILED\n");
        }
    }
    if (wrote52) {
        printf("Rollback 52h -> %02X ... ", original52);
        if (pci_write_byte(d->bus, d->devfn, REG_POSTWRITE, original52)) {
            printf("OK\n");
        } else {
            printf("FAILED\n");
        }
    }
    printf("Because verification failed, reboot/power-cycle before continuing.\n");
    return 0;
}

static void show_state(const PciDevice *d)
{
    unsigned char focus[FOCUS_LEN];
    unsigned char v52;
    unsigned char v53;

    if (!stable_read_register(d, REG_POSTWRITE, &v52) ||
        !stable_read_register(d, REG_BURSTWRITE, &v53)) {
        return;
    }

    printf("Current registers: 52=%02X 53=%02X\n", v52, v53);
    printf("Tentative reverse-engineered decode:\n");
    printf("  52h bit7 CPU->PCI Post Write: %s\n",
           (v52 & 0x80) ? "1 = 0 WS" : "0 = 1 WS");
    printf("  53h bit7 CPU->PCI Burst Write: %s\n",
           (v53 & 0x80) ? "1 = enabled" : "0 = disabled");

    if (v52 == BASE_REG52 && v53 == BASE_REG53) {
        printf("State: KNOWN BASELINE\n");
    } else if (v52 == WS1_REG52 && v53 == BASE_REG53) {
        printf("State: UMCTEST WS1 active\n");
    } else if (v52 == BASE_REG52 && v53 == NOBURST_REG53) {
        printf("State: UMCTEST NOBURST active\n");
    } else if (v52 == WS1_REG52 && v53 == NOBURST_REG53) {
        printf("State: UMCTEST BOTH active\n");
    } else {
        printf("State: UNKNOWN / NOT ONE OF THE EXPECTED TEST STATES\n");
    }

    if (read_focus_window(d, focus)) {
        printf("Focus window 50h-5Fh:\n");
        print_focus_window(focus);
    }
}

static void usage(void)
{
    printf("Usage:\n");
    printf("  UMCTEST SHOW\n");
    printf("  UMCTEST WS1 APPLY\n");
    printf("  UMCTEST NOBURST APPLY\n");
    printf("  UMCTEST BOTH APPLY\n");
    printf("  UMCTEST RESTORE APPLY\n");
    printf("\n");
    printf("SHOW is read-only.  APPLY is mandatory for every hardware write.\n");
    printf("Run only in real-mode DOS, with TURBO ON for the intended experiment.\n");
}

int main(int argc, char **argv)
{
    PciDevice d;
    const char *cmd;
    int is_write_command;
    int ok;

    printf("UMCTEST - controlled UMC UM8881F timing experiment\n");
    printf("WARNING: write modes alter live chipset PCI configuration.\n\n");

    if (argc < 2) {
        usage();
        return 1;
    }

    cmd = argv[1];
    is_write_command = !strieq(cmd, "SHOW");

    if (!(strieq(cmd, "SHOW") || strieq(cmd, "WS1") ||
          strieq(cmd, "NOBURST") || strieq(cmd, "BOTH") ||
          strieq(cmd, "RESTORE"))) {
        printf("ERROR: Unknown command '%s'.\n\n", cmd);
        usage();
        return 1;
    }

    if (is_write_command) {
        if (argc != 3 || !strieq(argv[2], "APPLY")) {
            printf("REFUSING WRITE: explicit APPLY token is required.\n\n");
            usage();
            return 2;
        }
    } else if (argc != 2) {
        usage();
        return 1;
    }

    if (!pci_bios_check()) {
        printf("ERROR: PCI BIOS services were not detected. No write performed.\n");
        return 3;
    }

    printf("PCI BIOS version %u.%02X, last bus %u\n",
           (unsigned int)pci_version_major,
           (unsigned int)pci_version_minor_bcd,
           (unsigned int)pci_last_bus);

    if (!find_exact_um8881f(&d)) {
        printf("No write performed.\n");
        return 4;
    }

    print_device(&d);

    if (!identity_is_safe(&d)) {
        printf("No write performed.\n");
        return 5;
    }

    if (strieq(cmd, "SHOW")) {
        show_state(&d);
        printf("\nSHOW completed. No PCI configuration register was written.\n");
        return 0;
    }

    printf("\n*** LIVE CHIPSET WRITE REQUESTED: %s ***\n", cmd);
    printf("Do not run this from Windows or a DOS box.\n");
    printf("If the machine hangs or behaves abnormally, reboot/power-cycle.\n\n");

    ok = 0;
    if (strieq(cmd, "WS1")) {
        ok = write_and_verify_state(&d, WS1_REG52, BASE_REG53, 0);
    } else if (strieq(cmd, "NOBURST")) {
        ok = write_and_verify_state(&d, BASE_REG52, NOBURST_REG53, 0);
    } else if (strieq(cmd, "BOTH")) {
        ok = write_and_verify_state(&d, WS1_REG52, NOBURST_REG53, 0);
    } else if (strieq(cmd, "RESTORE")) {
        ok = write_and_verify_state(&d, BASE_REG52, BASE_REG53, 1);
    }

    if (!ok) {
        printf("\nOperation FAILED or was refused.\n");
        return 6;
    }

    if (strieq(cmd, "RESTORE")) {
        printf("\nBaseline restored and verified: 52=%02X 53=%02X.\n",
               BASE_REG52, BASE_REG53);
    } else {
        printf("\nTest state is ACTIVE and intentionally remains active after exit.\n");
        printf("Run the VGA palette diagnostic now, then execute:\n");
        printf("  UMCTEST RESTORE APPLY\n");
        printf("before trying the next test state.\n");
    }

    return 0;
}
