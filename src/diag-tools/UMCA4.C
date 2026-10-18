/*
 * UMCA4.C - Controlled UM8886A register A4h experiment
 *
 * Target: 16-bit real-mode DOS, Open Watcom C/C++ 1.9
 * Build : wcl -bt=dos -ms UMCA4.C
 *
 * PURPOSE
 * -------
 * Test one narrowly scoped hypothesis from the VGA DAC investigation:
 * whether UM8886A PCI configuration register A4h bits 1:0 affect the
 * CPU-to-PCI clock/timing relationship strongly enough to change legacy
 * VGA I/O timing while leaving CPU/RAM performance largely intact.
 *
 * IMPORTANT: the register interpretation is reverse-engineered and tentative.
 * The known test-machine baseline is A4h = 01h.  This program supports
 * two controlled test encodings, bits 1:0 = 00b and bits 1:0 = 10b, while
 * preserving bits 7:2.  It deliberately does NOT expose the 11b encoding.
 *
 * Known hardware state used as safety guard:
 *   UM8886A: 1060:886A, class/subclass 06/01, revision 01, A4h = 01h
 *   UM8881F: 1060:8881, class/subclass 06/00, revision 01,
 *            52h = B4h, 53h = 87h
 *
 * Commands:
 *   UMCA4 SHOW
 *   UMCA4 TEST00 APPLY
 *   UMCA4 TEST10 APPLY
 *   UMCA4 RESTORE APPLY
 *
 * SHOW is read-only.
 * TEST00 and TEST10 are accepted only from the exact known baseline A4h=01h
 * and only when the UM8881F 52h/53h registers are also at their known baseline.
 * RESTORE is accepted only from A4h=00h, 01h, or 02h and restores A4h=01h.
 *
 * TEST00/TEST10 intentionally leave the altered state active after exit so
 * that TURBOPROBE and VGAPAL4 can be run.  RESTORE returns to the baseline.
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
#define UM8886_A4_FIELD_MASK    0x03
#define UM8886_A4_BASE          0x01
#define UM8886_A4_TEST00        0x00
#define UM8886_A4_TEST10        0x02

#define WINDOW_FIRST            0xA0
#define WINDOW_LAST             0xAF
#define WINDOW_LEN              16

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

static int find_exact_device(unsigned int wanted_device, PciDevice *found)
{
    unsigned int bus;
    unsigned int dev;
    unsigned char devfn;
    PciDevice d;
    unsigned int matches;

    matches = 0;

    /*
     * Scan function 0 only.  On this machine, probing nonexistent functions
     * on some old PCI devices can alias function 0.  Both chipset functions
     * needed here are known to exist at function 0.
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
        printf("ERROR: Expected exactly one UMC 1060:%04X function-0 device; found %u.\n",
               wanted_device, matches);
        return 0;
    }
    return 1;
}

static void print_device(const char *name, const PciDevice *d)
{
    unsigned int dev;
    unsigned int fn;

    dev = (unsigned int)(d->devfn >> 3);
    fn = (unsigned int)(d->devfn & 7);

    printf("%s candidate: %u:%u.%u  %04X:%04X\n",
           name, (unsigned int)d->bus, dev, fn, d->vendor, d->device);
    printf("  Class/Subclass/ProgIF=%02X/%02X/%02X  Revision=%02X  Header=%02X\n",
           d->class_code, d->subclass, d->prog_if,
           d->revision, d->header_type);
}

static int identity_is_safe(const PciDevice *d, unsigned int device,
                            unsigned char class_code,
                            unsigned char subclass,
                            unsigned char revision,
                            const char *name)
{
    if (d->vendor != UMC_VENDOR_ID || d->device != device) {
        printf("REFUSING WRITE: %s vendor/device mismatch.\n", name);
        return 0;
    }
    if (d->class_code != class_code || d->subclass != subclass) {
        printf("REFUSING WRITE: %s class/subclass mismatch; expected %02X/%02X.\n",
               name, class_code, subclass);
        return 0;
    }
    if (d->revision != revision) {
        printf("REFUSING WRITE: %s revision mismatch; expected %02X, found %02X.\n",
               name, revision, d->revision);
        return 0;
    }
    if ((d->header_type & 0x7F) != 0x00) {
        printf("REFUSING WRITE: %s unexpected PCI header type %02X.\n",
               name, d->header_type);
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

static void print_window(const unsigned char *buf)
{
    unsigned int i;

    for (i = 0; i < WINDOW_LEN; ++i) {
        printf("%02X=%02X%s",
               WINDOW_FIRST + i,
               buf[i],
               ((i & 7U) == 7U) ? "\n" : "  ");
    }
}

static int verify_window_only_a4_changed(const unsigned char *before,
                                         const unsigned char *after,
                                         unsigned char desired_a4)
{
    unsigned int i;
    unsigned int reg;
    unsigned char expected;
    int ok;

    ok = 1;
    for (i = 0; i < WINDOW_LEN; ++i) {
        reg = WINDOW_FIRST + i;
        expected = before[i];
        if (reg == UM8886_REG_A4) {
            expected = desired_a4;
        }
        if (after[i] != expected) {
            printf("VERIFY ERROR: offset %02Xh expected %02X, read %02X.\n",
                   reg, expected, after[i]);
            ok = 0;
        }
    }
    return ok;
}

static int um8881_baseline_ok(const PciDevice *hb)
{
    unsigned char v52;
    unsigned char v53;

    if (!stable_read_register(hb, UM8881_REG52, &v52) ||
        !stable_read_register(hb, UM8881_REG53, &v53)) {
        return 0;
    }

    printf("UM8881F guard registers: 52=%02X 53=%02X\n", v52, v53);
    if (v52 != UM8881_BASE52 || v53 != UM8881_BASE53) {
        printf("REFUSING TEST: UM8881F is not at the known baseline 52=%02X 53=%02X.\n",
               UM8881_BASE52, UM8881_BASE53);
        printf("Restore/reboot before testing UM8886A A4h.\n");
        return 0;
    }
    return 1;
}

static void print_a4_decode(unsigned char v)
{
    unsigned int field;

    field = (unsigned int)(v & UM8886_A4_FIELD_MASK);
    printf("A4h bits 1:0 = %u%u (value %u).\n",
           (field >> 1) & 1U, field & 1U, field);
    printf("Tentative reverse-engineered field: CPU-to-PCI ratio/timing.\n");
    printf("Working hypothesis only: 00=2:1, 01=1:1, 10=3:2, 11=reserved.\n");
    printf("This mapping is NOT treated as authoritative by this utility.\n");
}

static void show_state(const PciDevice *isa, const PciDevice *hb)
{
    unsigned char a4;
    unsigned char v52;
    unsigned char v53;
    unsigned char window[WINDOW_LEN];

    if (!stable_read_register(isa, UM8886_REG_A4, &a4)) {
        return;
    }

    printf("Current UM8886A A4h=%02X\n", a4);
    print_a4_decode(a4);

    if (a4 == UM8886_A4_BASE) {
        printf("A4 state: KNOWN BASELINE\n");
    } else if (a4 == UM8886_A4_TEST00) {
        printf("A4 state: UMCA4 TEST00 active\n");
    } else if (a4 == UM8886_A4_TEST10) {
        printf("A4 state: UMCA4 TEST10 active\n");
    } else {
        printf("A4 state: UNKNOWN / NOT AN EXPECTED UMCA4 STATE\n");
    }

    if (stable_read_register(hb, UM8881_REG52, &v52) &&
        stable_read_register(hb, UM8881_REG53, &v53)) {
        printf("UM8881F guard registers: 52=%02X 53=%02X%s\n",
               v52, v53,
               (v52 == UM8881_BASE52 && v53 == UM8881_BASE53)
                   ? " (known baseline)" : " (NOT baseline)");
    }

    if (read_window(isa, window)) {
        printf("UM8886A window A0h-AFh:\n");
        print_window(window);
    }
}

static int write_a4_and_verify(const PciDevice *isa,
                               const PciDevice *hb,
                               unsigned char desired_field,
                               int restoring)
{
    unsigned char before[WINDOW_LEN];
    unsigned char after[WINDOW_LEN];
    unsigned char original_a4;
    unsigned char desired_a4;
    unsigned char verify_a4;
    unsigned char preserved;
    int wrote;

    wrote = 0;

    if (!stable_read_register(isa, UM8886_REG_A4, &original_a4)) {
        return 0;
    }

    if (!restoring) {
        if (original_a4 != UM8886_A4_BASE) {
            printf("REFUSING TEST: A4h must be exact known baseline %02X, found %02X.\n",
                   UM8886_A4_BASE, original_a4);
            printf("Run UMCA4 RESTORE APPLY or reboot, then UMCA4 SHOW.\n");
            return 0;
        }
        if (!um8881_baseline_ok(hb)) {
            return 0;
        }
    } else {
        if (!(original_a4 == UM8886_A4_BASE ||
              original_a4 == UM8886_A4_TEST00 ||
              original_a4 == UM8886_A4_TEST10)) {
            printf("REFUSING RESTORE: current A4h=%02X is not a known UMCA4 state.\n",
                   original_a4);
            return 0;
        }
    }

    if (!read_window(isa, before)) {
        printf("ERROR: Could not read UM8886A A0h-AFh before write.\n");
        return 0;
    }

    preserved = (unsigned char)(original_a4 & (unsigned char)~UM8886_A4_FIELD_MASK);
    desired_a4 = (unsigned char)(preserved |
                    (desired_field & UM8886_A4_FIELD_MASK));

    /* For this exact-machine experiment, only the explicitly supported
       full-byte states are allowed.  This catches unexpected upper bits. */
    if ((!restoring &&
         !(desired_a4 == UM8886_A4_TEST00 ||
           desired_a4 == UM8886_A4_TEST10)) ||
        (restoring && desired_a4 != UM8886_A4_BASE)) {
        printf("REFUSING WRITE: computed A4h target %02X is outside known test states.\n",
               desired_a4);
        return 0;
    }

    printf("Before: UM8886A A4=%02X\n", original_a4);
    printf("Target: UM8886A A4=%02X (only bits 1:0 are intentionally changed)\n",
           desired_a4);

    if (original_a4 == desired_a4) {
        printf("A4h already has the requested value; no write needed.\n");
    } else {
        printf("Writing register A4h: %02X -> %02X ... ", original_a4, desired_a4);
        if (!pci_write_byte(isa->bus, isa->devfn, UM8886_REG_A4, desired_a4)) {
            printf("FAILED\n");
            return 0;
        }
        wrote = 1;
        printf("OK\n");
    }

    if (!stable_read_register(isa, UM8886_REG_A4, &verify_a4) ||
        !read_window(isa, after)) {
        printf("ERROR: Could not verify new state. Attempting rollback.\n");
        goto rollback;
    }

    if (verify_a4 != desired_a4) {
        printf("VERIFY ERROR: wanted A4=%02X, got A4=%02X.\n",
               desired_a4, verify_a4);
        goto rollback;
    }

    if (!verify_window_only_a4_changed(before, after, desired_a4)) {
        printf("Unexpected change inside A0h-AFh. Attempting rollback.\n");
        goto rollback;
    }

    printf("Verification OK. Current UM8886A A0h-AFh window:\n");
    print_window(after);
    return 1;

rollback:
    if (wrote) {
        printf("Rollback A4h -> %02X ... ", original_a4);
        if (pci_write_byte(isa->bus, isa->devfn, UM8886_REG_A4, original_a4)) {
            printf("OK\n");
        } else {
            printf("FAILED - REBOOT/POWER-CYCLE IMMEDIATELY\n");
        }
    }
    printf("Because verification failed, reboot/power-cycle before continuing.\n");
    return 0;
}

static void usage(void)
{
    printf("Usage:\n");
    printf("  UMCA4 SHOW\n");
    printf("  UMCA4 TEST00 APPLY\n");
    printf("  UMCA4 TEST10 APPLY\n");
    printf("  UMCA4 RESTORE APPLY\n");
    printf("\n");
    printf("SHOW is read-only. APPLY is mandatory for every hardware write.\n");
    printf("Run only in real-mode DOS, preferably with TURBO ON.\n");
    printf("TEST00 changes only UM8886A A4h bits 1:0 from 01b to 00b.\n");
    printf("TEST10 changes only UM8886A A4h bits 1:0 from 01b to 10b.\n");
    printf("Encoding 11b is intentionally not exposed.\n");
}

int main(int argc, char **argv)
{
    PciDevice isa;
    PciDevice hb;
    const char *cmd;
    int is_write_command;
    int ok;

    printf("UMCA4 - controlled UMC UM8886A A4h experiment\n");
    printf("WARNING: TEST00/TEST10/RESTORE alter live chipset PCI configuration.\n");
    printf("The A4h field interpretation is reverse-engineered and tentative.\n\n");

    if (argc < 2) {
        usage();
        return 1;
    }

    cmd = argv[1];
    is_write_command = !strieq(cmd, "SHOW");

    if (!(strieq(cmd, "SHOW") || strieq(cmd, "TEST00") ||
          strieq(cmd, "TEST10") || strieq(cmd, "RESTORE"))) {
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

    if (!find_exact_device(UM8886A_DEVICE_ID, &isa) ||
        !find_exact_device(UM8881F_DEVICE_ID, &hb)) {
        printf("No write performed.\n");
        return 4;
    }

    print_device("UM8886A", &isa);
    print_device("UM8881F", &hb);

    if (!identity_is_safe(&isa, UM8886A_DEVICE_ID,
                          UM8886_CLASS, UM8886_SUBCLASS,
                          UM8886_REVISION, "UM8886A") ||
        !identity_is_safe(&hb, UM8881F_DEVICE_ID,
                          UM8881_CLASS, UM8881_SUBCLASS,
                          UM8881_REVISION, "UM8881F")) {
        printf("No write performed.\n");
        return 5;
    }

    if (strieq(cmd, "SHOW")) {
        show_state(&isa, &hb);
        printf("\nSHOW completed. No PCI configuration register was written.\n");
        return 0;
    }

    printf("\n*** LIVE CHIPSET WRITE REQUESTED: %s ***\n", cmd);
    printf("Do not run this from Windows or a DOS box.\n");
    printf("If the machine hangs or behaves abnormally, reboot/power-cycle.\n");
    printf("Encoding 11b remains intentionally unsupported.\n\n");

    ok = 0;
    if (strieq(cmd, "TEST00")) {
        ok = write_a4_and_verify(&isa, &hb, 0x00, 0);
    } else if (strieq(cmd, "TEST10")) {
        ok = write_a4_and_verify(&isa, &hb, 0x02, 0);
    } else if (strieq(cmd, "RESTORE")) {
        ok = write_a4_and_verify(&isa, &hb, 0x01, 1);
    }

    if (!ok) {
        printf("\nOperation FAILED or was refused.\n");
        return 6;
    }

    if (strieq(cmd, "RESTORE")) {
        printf("\nKnown baseline restored and verified: UM8886A A4=%02X.\n",
               UM8886_A4_BASE);
    } else if (strieq(cmd, "TEST00")) {
        printf("\nTEST00 state is ACTIVE and intentionally remains active after exit.\n");
        printf("Recommended order:\n");
        printf("  1. TURBOPROBE A4_00\n");
        printf("  2. VGAPAL4 A4_00\n");
        printf("  3. UMCA4 RESTORE APPLY\n");
        printf("Restore before any further chipset experiment.\n");
    } else {
        printf("\nTEST10 state is ACTIVE and intentionally remains active after exit.\n");
        printf("Recommended order:\n");
        printf("  1. TURBOPROBE A4_10\n");
        printf("  2. VGAPAL4 A4_10\n");
        printf("  3. UMCA4 RESTORE APPLY\n");
        printf("Restore before any further chipset experiment.\n");
    }

    return 0;
}
