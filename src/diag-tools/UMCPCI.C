/*
 * UMCPCI.C - Read-only PCI configuration dumper for UMC 486 chipsets
 *
 * Target: 16-bit real-mode DOS, Open Watcom C 1.9
 * Build : wcl -bt=dos -ms UMCPCI.C
 *
 * This program performs READS ONLY. It does not write any PCI configuration
 * register and does not touch VGA registers.
 *
 * Intended use in this investigation:
 *   UMCPCI TURBO_OFF > UMC_OFF.TXT
 *   UMCPCI TURBO_ON  > UMC_ON.TXT
 *
 * Then compare the two text files.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <i86.h>

#define PCI_BIOS_PRESENT        0xB101
#define PCI_READ_CONFIG_BYTE    0xB108
#define PCI_READ_CONFIG_WORD    0xB109

#define UMC_VENDOR_ID           0x1060

#define PCI_SUCCESS             0x00

static unsigned char pci_last_bus = 0;
static unsigned char pci_version_major = 0;
static unsigned char pci_version_minor_bcd = 0;

static const char *umc_device_name(unsigned int device)
{
    switch (device) {
    case 0x0001: return "UM82C881";
    case 0x0002: return "UM82C886";
    case 0x0101: return "UM8673F";
    case 0x0881: return "UM8881";
    case 0x0886: return "UM8886F (alternate ID)";
    case 0x0891: return "UM8891A";
    case 0x1001: return "UM886A";
    case 0x673A: return "UM8886BF";
    case 0x673B: return "EIDE Master/DMA";
    case 0x8710: return "UM8710";
    case 0x886A: return "UM8886A";
    case 0x8881: return "UM8881F";
    case 0x8886: return "UM8886F";
    case 0x888A: return "UM8886A";
    case 0x8891: return "UM8891A";
    case 0x9017: return "UM9017F";
    case 0x9018: return "UM9018";
    case 0x9026: return "UM9026";
    case 0xE881: return "UM8881N";
    case 0xE886: return "UM8886N";
    case 0xE88A: return "UM8886N";
    case 0xE891: return "UM8891N";
    default:     return "Unknown UMC PCI device";
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

    /* PCI BIOS returns version in BH.BL and last bus in CL. */
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

static unsigned long fnv1a_32(const unsigned char *p, unsigned int len)
{
    unsigned long h;
    unsigned int i;

    h = 2166136261UL;
    for (i = 0; i < len; ++i) {
        h ^= (unsigned long)p[i];
        h *= 16777619UL;
    }
    return h;
}

static int read_config_256(unsigned char bus, unsigned char devfn,
                           unsigned char *cfg)
{
    unsigned int i;

    for (i = 0; i < 256; ++i) {
        if (!pci_read_byte(bus, devfn, i, &cfg[i])) {
            return 0;
        }
    }
    return 1;
}

static void dump_hex_256(const unsigned char *cfg)
{
    unsigned int row;
    unsigned int col;

    printf("      +00 +01 +02 +03 +04 +05 +06 +07 +08 +09 +0A +0B +0C +0D +0E +0F\n");
    for (row = 0; row < 256; row += 16) {
        printf("%02Xh :", row);
        for (col = 0; col < 16; ++col) {
            printf(" %02X", cfg[row + col]);
        }
        printf("\n");
    }
}

static void print_standard_header(const unsigned char *cfg)
{
    unsigned int vendor;
    unsigned int device;
    unsigned int command;
    unsigned int status;
    unsigned long classcode;

    vendor = (unsigned int)cfg[0x00] | ((unsigned int)cfg[0x01] << 8);
    device = (unsigned int)cfg[0x02] | ((unsigned int)cfg[0x03] << 8);
    command = (unsigned int)cfg[0x04] | ((unsigned int)cfg[0x05] << 8);
    status = (unsigned int)cfg[0x06] | ((unsigned int)cfg[0x07] << 8);
    classcode = ((unsigned long)cfg[0x0B] << 16) |
                ((unsigned long)cfg[0x0A] << 8) |
                (unsigned long)cfg[0x09];

    printf("  Vendor:Device = %04X:%04X\n", vendor, device);
    printf("  Command=%04X Status=%04X Revision=%02X\n",
           command, status, cfg[0x08]);
    printf("  Class/Subclass/ProgIF = %02X/%02X/%02X\n",
           cfg[0x0B], cfg[0x0A], cfg[0x09]);
    printf("  Header type=%02X  Latency timer=%02X  Cache line=%02X\n",
           cfg[0x0E], cfg[0x0D], cfg[0x0C]);
    printf("  Class code packed=%06lX\n", classcode);
}

static void dump_umc_function(unsigned char bus, unsigned char devfn,
                              unsigned int device)
{
    unsigned char cfg1[256];
    unsigned char cfg2[256];
    unsigned int i;
    unsigned int diffs;
    unsigned int dev;
    unsigned int fn;

    dev = (unsigned int)(devfn >> 3);
    fn = (unsigned int)(devfn & 7);

    printf("\n============================================================\n");
    printf("UMC DEVICE at %u:%u.%u  ID 1060:%04X  (%s)\n",
           (unsigned int)bus, dev, fn, device, umc_device_name(device));
    printf("============================================================\n");

    if (!read_config_256(bus, devfn, cfg1)) {
        printf("ERROR: Could not read the full 256-byte configuration space.\n");
        return;
    }

    if (!read_config_256(bus, devfn, cfg2)) {
        printf("ERROR: Second configuration-space read failed.\n");
        return;
    }

    diffs = 0;
    for (i = 0; i < 256; ++i) {
        if (cfg1[i] != cfg2[i]) {
            ++diffs;
        }
    }

    print_standard_header(cfg1);
    printf("  Config fingerprint = %08lX\n", fnv1a_32(cfg1, 256));
    printf("  Immediate reread differences = %u byte(s)\n", diffs);

    if (diffs != 0) {
        printf("  Reread differences:\n");
        for (i = 0; i < 256; ++i) {
            if (cfg1[i] != cfg2[i]) {
                printf("    offset %02Xh: first=%02X second=%02X\n",
                       i, cfg1[i], cfg2[i]);
            }
        }
    }

    printf("\nFull PCI configuration space (FIRST read):\n");
    dump_hex_256(cfg1);

    printf("\nChipset-specific focus window 40h-7Fh:\n");
    for (i = 0x40; i <= 0x7F; ++i) {
        printf("%02X=%02X%s", i, cfg1[i], ((i & 7) == 7) ? "\n" : "  ");
    }
    if ((i & 7) != 0) {
        printf("\n");
    }
}

static void scan_pci(void)
{
    unsigned int bus;
    unsigned int dev;
    unsigned int fn;
    unsigned char devfn;
    unsigned int vendor;
    unsigned int device;
    unsigned char class_hi;
    unsigned char subclass;
    unsigned char revision;
    unsigned int found;
    unsigned int umc_found;

    found = 0;
    umc_found = 0;

    printf("\n=== PCI DEVICE SUMMARY ===\n");
    printf("B:D.F  Vendor:Device  Class/Subclass  Rev  Description\n");

    for (bus = 0; bus <= (unsigned int)pci_last_bus; ++bus) {
        for (dev = 0; dev < 32; ++dev) {
            for (fn = 0; fn < 8; ++fn) {
                devfn = (unsigned char)((dev << 3) | fn);

                if (!pci_read_word((unsigned char)bus, devfn, 0x00, &vendor)) {
                    continue;
                }
                if (vendor == 0xFFFF || vendor == 0x0000) {
                    continue;
                }
                if (!pci_read_word((unsigned char)bus, devfn, 0x02, &device)) {
                    continue;
                }

                class_hi = 0xFF;
                subclass = 0xFF;
                revision = 0xFF;
                (void)pci_read_byte((unsigned char)bus, devfn, 0x0B, &class_hi);
                (void)pci_read_byte((unsigned char)bus, devfn, 0x0A, &subclass);
                (void)pci_read_byte((unsigned char)bus, devfn, 0x08, &revision);

                printf("%u:%02u.%u  %04X:%04X      %02X/%02X          %02X",
                       bus, dev, fn, vendor, device,
                       class_hi, subclass, revision);

                if (vendor == UMC_VENDOR_ID) {
                    printf("  UMC %s", umc_device_name(device));
                    ++umc_found;
                }
                printf("\n");
                ++found;
            }
        }
    }

    printf("Total PCI functions found: %u\n", found);
    printf("UMC PCI functions found:   %u\n", umc_found);

    if (umc_found == 0) {
        printf("\nWARNING: No PCI function with UMC vendor ID 1060h was found.\n");
        printf("The board may hide chipset registers, use a different ID, or the PCI BIOS may be incomplete.\n");
        return;
    }

    printf("\n=== UMC CONFIGURATION DUMPS ===\n");

    for (bus = 0; bus <= (unsigned int)pci_last_bus; ++bus) {
        for (dev = 0; dev < 32; ++dev) {
            for (fn = 0; fn < 8; ++fn) {
                devfn = (unsigned char)((dev << 3) | fn);

                if (!pci_read_word((unsigned char)bus, devfn, 0x00, &vendor)) {
                    continue;
                }
                if (vendor != UMC_VENDOR_ID) {
                    continue;
                }
                if (!pci_read_word((unsigned char)bus, devfn, 0x02, &device)) {
                    continue;
                }

                dump_umc_function((unsigned char)bus, devfn, device);
            }
        }
    }
}

int main(int argc, char **argv)
{
    const char *label;

    label = (argc >= 2) ? argv[1] : "UNSPECIFIED";

    printf("UMCPCI - read-only UMC PCI configuration dumper\n");
    printf("Target: 16-bit real-mode DOS / Open Watcom C 1.9\n");
    printf("Run label: %s\n", label);
    printf("IMPORTANT: this build performs PCI CONFIGURATION READS ONLY.\n");

    if (!pci_bios_check()) {
        printf("\nERROR: PCI BIOS services (INT 1Ah, AX=B101h) were not detected.\n");
        printf("No hardware state has been modified.\n");
        return 1;
    }

    printf("PCI BIOS version: %u.%u%u\n",
           (unsigned int)pci_version_major,
           (unsigned int)((pci_version_minor_bcd >> 4) & 0x0F),
           (unsigned int)(pci_version_minor_bcd & 0x0F));
    printf("Last PCI bus reported by BIOS: %u\n", (unsigned int)pci_last_bus);

    scan_pci();

    printf("\n=== END ===\n");
    printf("No PCI configuration register was written.\n");
    return 0;
}
