# Investigation of the VGA Palette and PCI Timing Fault on a PCChips M915i

## 1. Executive summary

This investigation began with an apparently graphical fault: several DOS games displayed incorrect colors when dynamically updating the VGA palette. The problem occurred with both an **S3 Trio64V2/DX PCI** and an **S3 ViRGE PCI**, on a **PCChips M915i** motherboard with the **UMC UM8881F + UM8886A** chipset, using an **Intel 486 DX4-100**.

Initial tests showed that the problem was not simply analog color corruption or an isolated fault in a VGA card. The error pattern revealed that some I/O accesses to the VGA DAC ports were being lost, repeated, or handled incorrectly, desynchronizing the DAC's internal state and causing long sequences of shifted colors.

The investigation progressed from the graphical symptom to the motherboard's I/O subsystem. The key finding was that disabling TURBO dramatically improved the problem, but at a severe overall performance cost. It was later established that TURBO OFF barely reduced the CPU's internal arithmetic performance, but greatly slowed external accesses, especially I/O and RAM writes. This pointed to bus timing, wait states, or handshaking—not the processor's internal clock.

After ruling out several UM8881F bits tentatively associated with CPU-to-PCI Post Write and Burst Write, the `A4[1:0]` field of the **UM8886A** was tested in a controlled manner. Changing it from `01b` to `00b` produced an unambiguous result:

- The custom diagnostic utility `VGAPAL4` went from thousands of errors to **zero errors in every test**.
- Games stopped showing palette corruption.
- The Windows 95 startup logo no longer exhibited color-related problems.
- The **RTL8029** Ethernet adapter, which had previously dropped packets and made FTP transfers in Windows unusable, began working correctly.
- CPU and RAM performance remained practically the same as with TURBO ON.
- The observable Doom performance penalty was moderate: `timedemo` increased from **1891** to **2009 realtics**, approximately **6.24% more time** or **5.87% less effective performance**.

The conclusion was that the original `A4[1:0]=01b` setting left the PCI/I/O subsystem running with timing that was too aggressive for this motherboard. The `00b` state provides a stable configuration and is a practical, reproducible fix.

As a permanent solution, **UMCFIX** was created: a small utility to run from `AUTOEXEC.BAT`, designed exclusively to detect the expected hardware and safely apply `A4[1:0]=00b`.

---

## 2. System under investigation

### Motherboard

- **Model:** PCChips M915i
- **Chipset:** UMC UM8881F + UM8886A
- The board is known for using a “fake cache” implementation, although the (lack of) cache did not turn out to be the cause of this problem.

### CPU

- **Intel 486 DX4-100**

### Graphics cards tested

- **S3 Trio64V2/DX PCI**, 1 MB VRAM
- **S3 ViRGE PCI**, 2 MB VRAM

Both were affected (including by the separate 'pedestal bit' issue, which was fixed independently).

### Operating system

- Windows 95 OSR2.1 MS-DOS mode

### Other relevant devices

- PCI network card based on the **Realtek RTL8029**

---

## 3. Initial symptoms

The problem was especially noticeable in software that frequently updated the VGA palette.

Affected games:

- Doom
- Duke Nukem 3D
- Prince of Persia (1 and 2)
- The Lion King
- Mortal Kombat II

Symptoms were also observed during:

- the Windows 95 startup and shutdown logos;
- fades;
- flashes;
- effects that rapidly modified the VGA DAC.

The fault was not necessarily tied to VESA modes; it was mainly reproducible in **VGA mode 13h**, so the problem had to lie in a sufficiently basic path to affect the standard VGA DAC.

One important characteristic was that incorrect colors could persist until the next palette update. This suggested that the DAC was receiving incorrect data and retaining it, rather than the issue being momentary analog noise.

---

## 4. Initial hypotheses considered

During the investigation, the following possibilities, among others, were considered:

1. Faulty S3 Trio64V2/DX card.
2. Integrated RAMDAC problem.
3. VRAM problem.
4. Analog corruption in the VGA output.
5. Specific S3-chipset interaction.
6. Cache or memory problem.
7. Incorrect timing of PCI or legacy VGA I/O accesses.
8. Overly aggressive UMC chipset configuration.

Several observations quickly reduced the likelihood of the first hypotheses:

- the problem also occurred with a different S3 ViRGE;
- the incorrect values followed patterns consistent with DAC desynchronization;
- disabling caches did not solve the problem;
- changing the PCI slot, BIOS settings, and other common options did not fix it either;
- TURBO OFF changed the behavior (as measured with diagnostic tools).

---

## 5. Initial diagnosis of the VGA DAC

A 16-bit DOS utility was created to write to and read directly from the DAC through the standard ports:

```text
3C6h  PEL mask
3C7h  DAC read index
3C8h  DAC write index
3C9h  DAC data
```

The first rapid sequential-write tests showed large numbers of errors.

The pattern was particularly significant: in many cases, a palette entry ended up containing values that belonged to the preceding or following expected entry. In other words, sequences such as the following appeared:

```text
palette[n] ~= expected[n+1]
```

or similar shifts.

This was much more consistent with a lost, repeated, or improperly accepted I/O cycle than with random bit corruption.

The VGA DAC maintains an internal state along the lines of:

```text
R -> G -> B -> increment index
```

If an access to `3C9h` is lost or duplicated, the entire stream becomes offset, and subsequent bytes are interpreted as the wrong components or as data for the next entry.

This was one of the first strong clues that the problem lay in **I/O cycle timing**.

---

## 6. Tests with port 80h delays

As an initial test, `OUT 80h` writes were inserted between DAC accesses.

The effect was substantial:

- no delay: tens of thousands of incorrect components;
- with 200 writes to port 80h: the number of errors dropped by approximately **98%**.

This indicated that the problem was highly sensitive to timing.

However, residual errors remained, so the delay was not a complete solution.

---

## 7. VGAPAL2: separating read and write problems

**VGAPAL2** was created to allow delays to be introduced independently for:

- DAC writes;
- DAC reads.

Representative results:

| Write | Read | Incorrect components |
|---|---|---:|
| fast | fast | 24,758 |
| delayed | fast | 20,792 |
| fast | delayed | 4,159 |
| delayed | delayed | 327 |

Conclusions:

- there was a genuine **read** problem;
- there was also a genuine **write** problem;
- delays greatly reduced both;
- most of the problem initially observed during verification could be amplified by unstable reads.

Another important observation was that programming `3C7h` again before each palette entry prevented an isolated read failure from propagating into a long desynchronization.

This reinforced the model of an internal DAC state machine losing synchronization.

---

## 8. VGAPAL3: comparing write strategies

**VGAPAL3** introduced several strategies:

1. `SEQ_FAST`
2. `IDX_DELAY`
3. `DATA_DELAY`
4. `RGB_DELAY`
5. `REINDEX_FAST`
6. `REINDEX_FULL_DELAY`

Tests were performed with both TURBO OFF and TURBO ON.

### TURBO OFF

`SEQ_FAST` still showed many errors, but some strategies improved the results dramatically.

For example:

- `SEQ_FAST`: 10,330 incorrect components
- `IDX_DELAY`: 32
- `REINDEX_FULL_DELAY`: 33

### TURBO ON

Errors increased significantly:

- `SEQ_FAST`: 11,125 errors
- `REINDEX_FAST`: 457
- `REINDEX_FULL_DELAY`: 123

The `REINDEX_FAST` strategy, without adding a large delay, greatly reduced error propagation because it continually reset the DAC index.

Conclusion:

> The primary fault was probabilistic and intermittent, but the DAC's auto-increment feature turned a single error into a long chain of shifted entries.

---

## 9. TURBO OFF as a key clue

A practical observation was that results improved significantly with **TURBO OFF**.

This could mean one of two things:

1. the CPU's internal clock was reduced;
2. the CPU continued running at a similar speed, but the chipset added wait states or changed external-access timing.

**TURBOPROBE** was created to distinguish between these possibilities.

---

## 10. TURBOPROBE: what TURBO actually changes

TURBOPROBE measured:

- internal arithmetic operations;
- RAM reads;
- RAM writes;
- `OUT 80h`;
- `IN 61h`;
- VGA accesses to `3C8h` and `3CCh`.

### TURBO ON / TURBO OFF results

Approximately:

| Test | TURBO ON | TURBO OFF | ON/OFF ratio |
|---|---:|---:|---:|
| CPU ALU | 3.58 M/s | 3.56 M/s | 1.005x |
| RAM read | 8.39 M/s | 6.33 M/s | 1.32x |
| RAM write | 12.26 M/s | 1.26 M/s | 9.74x |
| OUT 80h | 777.8 k/s | 327.3 k/s | 2.38x |
| IN 61h | 1.325 M/s | 388.4 k/s | 3.41x |
| VGA OUT 3C8h | 776.8 k/s | 327.3 k/s | 2.37x |
| VGA IN 3CCh | 1.391 M/s | 395.7 k/s | 3.52x |

Key conclusion:

> TURBO OFF barely reduces the CPU's internal arithmetic performance, but greatly slows external accesses.

Therefore, TURBO appeared to operate through **READY, wait states, or bus timing**, rather than through a major reduction in the 486's internal clock.

This also explained why games became slow with TURBO OFF even though the CPU itself remained fast.

---

## 11. UMCPCI: checking whether TURBO changes PCI registers

**UMCPCI** was created as a read-only utility to dump the UMC chipset's PCI configuration.

Three states were compared:

```text
TURBO ON
TURBO OFF without rebooting
TURBO ON again
```

The dumps were byte-for-byte identical, apart from the text label used when running the utility.

This ruled out the TURBO button dynamically changing any visible PCI configuration register.

The conclusion shifted even further toward a hardware or external-timing change not reflected in standard PCI configuration space.

---

## 12. VGAPAL4: distinguishing “waiting” from “changing the cycle”

The next question was fundamental:

> Does TURBO OFF fix the fault simply by leaving more time between consecutive accesses, or does it change how the I/O cycle itself is executed?

**VGAPAL4** was created to answer this.

The following were compared:

- pure CPU delays of approximately 1, 2, 4, and 8 µs;
- groups of 1, 2, 4, and 8 `OUT 80h` instructions;
- fast writes with no delay.

All delays were applied after `3C8h` and after each write to `3C9h`.

### Results with the original configuration and TURBO ON

Summary:

| Strategy | Errors |
|---|---:|
| FAST | 8,313 |
| CPU 1 µs | 8,084 |
| CPU 2 µs | 8,102 |
| CPU 4 µs | 7,125 |
| CPU 8 µs | 6,335 |
| OUT80 x1 | 7,735 |
| OUT80 x2 | 7,498 |
| OUT80 x4 | 6,674 |
| OUT80 x8 | 5,563 |

Even introducing approximately 8.7 µs of CPU wait time after every write did not fix the problem.

Conclusion:

> TURBO OFF does not work simply because it leaves a “gap” between two accesses. It must change the timing or handshake of the external cycle itself.

This was consistent with:

- strobe width;
- READY/wait states;
- cycle acceptance by the bridge;
- CPU-to-PCI conversion;
- internal serialization.

---

## 13. Initial candidate registers in the UM8881F

Based on reconstructed chipset documentation, two bits were tentatively identified as being related to:

- CPU-to-PCI Post Write;
- CPU-to-PCI Burst Write.

**UMCTEST** was created to test them under tightly controlled conditions.

States tested:

```text
baseline : 52=B4, 53=87
WS1      : 52=34, 53=87
NOBURST  : 52=B4, 53=07
BOTH     : 52=34, 53=07
```

### VGAPAL4 results

`FAST` produced:

| Configuration | Errors |
|---|---:|
| baseline | 8,313 |
| WS1 | 8,605 |
| NOBURST | 8,386 |
| BOTH | 8,410 |

There was no meaningful improvement.

Decision:

> Bits 52.7 and 53.7 do not control the mechanism responsible for the DAC fault.

They were ruled out as a solution.

---

## 14. New candidate: UM8886A A4[1:0]

The reconstructed documentation tentatively suggested that `A4[1:0]` in the UM8886A might control a CPU-to-PCI ratio along these lines:

```text
00 -> 2:1
01 -> 1:1
10 -> 3:2
11 -> reserved
```

The machine originally booted with:

```text
A4 = 01h
```

**UMCA4** was created to modify only those two bits in a controlled manner.

The first test was:

```text
01b -> 00b
```

---

## 15. Decisive result with A4=00

### TURBOPROBE

With `A4=00`:

| Test | Result |
|---|---:|
| CPU ALU | 3.580 M/s |
| RAM read | 8.385 M/s |
| RAM write | 12.263 M/s |
| OUT 80h | 497,159 k/s |
| IN 61h | 993,282 k/s |
| VGA OUT 3C8h | 497,159 k/s |
| VGA IN 3CCh | 1,092 M/s |
| VGA 3C8 pair | 497,159 k/s |

Compared with the original TURBO ON state:

- CPU: no appreciable loss;
- RAM: no appreciable loss;
- I/O: slower and more conservative;
- VGA: the same trend as the rest of the I/O operations.

This was exactly the kind of change being sought.

### VGAPAL4

The result was complete:

```text
FAST      : 12/12 perfect, 0 errors
CPU_1US   : 12/12 perfect, 0 errors
CPU_2US   : 12/12 perfect, 0 errors
CPU_4US   : 12/12 perfect, 0 errors
CPU_8US   : 12/12 perfect, 0 errors
IO80_X1   : 12/12 perfect, 0 errors
IO80_X2   : 12/12 perfect, 0 errors
IO80_X4   : 12/12 perfect, 0 errors
IO80_X8   : 12/12 perfect, 0 errors
```

The `FAST` strategy, which had previously produced thousands of errors, now produced **zero**.

This was the strongest test in the entire investigation.

---

## 16. Validation with real software

The fix was repeatedly validated in real games.

With `UMCA4 TEST00 APPLY`, the following worked correctly:

- Prince of Persia
- Prince of Persia 2
- Doom
- Duke Nukem 3D
- Mortal Kombat II
- Windows 95

When running:

```text
UMCA4 RESTORE APPLY
```

the problem returned.

The following cycle was repeated several times:

```text
A4=01 -> faulty
A4=00 -> correct
A4=01 -> faulty
A4=00 -> correct
```

This established a very strong practical causal relationship.

---

## 17. Additional discovery: the PCI network was fixed too

Testing with Windows 95 revealed that the original problem was not exclusively graphical.

Before applying `A4=00`:

- the RTL8029 dropped packets;
- the network was slow;
- an FTP transfer could not complete successfully.

After applying `A4=00`:

- Ethernet worked correctly;
- Windows behavior became comparable to that obtained in MS-DOS using the packet driver;
- transfers were stable.

This finding changed the final interpretation of the problem:

> This was not an isolated incompatibility with the VGA DAC, but overly aggressive global PCI timing that affected more than one device.

VGA was simply one of the most sensitive peripherals, making the problem especially visible.

---

## 18. Testing the intermediate A4=10 state

The possibility was considered that `10b`, tentatively associated with an intermediate ratio, might offer better performance while maintaining stability.

An experimental mode was added to UMCA4 to test:

```text
A4[1:0] = 10b
```

Result:

- palette problems returned.

Decision:

> `10b` was ruled out.

The `00b` state remained the only tested stable configuration.

`11b` was not tested because it was considered reserved or undocumented.

---

## 19. Performance cost

Doom's `timedemo` was used to measure the real-world impact of `A4=00`.

Results:

```text
A4=01, original configuration: 1891 realtics
A4=00, stable configuration:  2009 realtics
```

For the same demo:

- increase in time: approximately **6.24%**;
- relative effective performance: approximately **94.13%**;
- effective performance loss: approximately **5.87%**.

This was considered a reasonable penalty, given that:

- CPU and RAM retain their performance;
- VGA becomes completely stable;
- the PCI network becomes stable;
- TURBO OFF was far more costly.

---

## 20. Final technical interpretation

The explanation that best fits the overall results is as follows:

1. The BIOS leaves the UM8886A with `A4[1:0]=01b`.
2. This state configures the CPU/PCI path or I/O subsystem with timing that is too aggressive for this particular motherboard.
3. Some PCI or legacy I/O cycles do not complete reliably enough.
4. In the VGA DAC, a single lost or improperly accepted cycle desynchronizes the R/G/B sequence and produces long chains of shifted colors.
5. In the RTL8029, the same problem manifests as packet loss or unstable transfers.
6. TURBO OFF masks the problem by introducing much more conservative external timing, but with an excessive performance penalty.
7. `A4=00` specifically changes I/O/PCI behavior without significantly penalizing CPU or RAM performance.
8. With `A4=00`, both the VGA DAC and the network card work correctly.

The PCI clock was not measured directly, so it cannot be stated with absolute certainty that `A4=00` means exactly “PCI at 16.7 MHz” or that `A4=01` means exactly “33 MHz.” The `2:1 / 1:1 / 3:2` interpretation comes from reconstructed documentation and must still be considered tentative.

What has been empirically demonstrated is the register's functional effect.

---

## 21. Permanent fix: UMCFIX

Once the stable configuration had been identified, **UMCFIX** was created, clearly separating:

- **UMCA4**: laboratory and diagnostic tool;
- **UMCFIX**: minimal solution for everyday use.

UMCFIX is intended to be run from `AUTOEXEC.BAT`.

Its behavior is deliberately conservative:

1. Detects the expected chipset.
2. Checks revisions and known values.
3. Reads `UM8886A:A4h`.
4. If it is already `00h`, does nothing.
5. If it is `01h`, modifies only `A4[1:0]` to set it to `00b`.
6. Verifies the write.
7. If it encounters an unexpected state, it does not write anything.

Recommended usage:

```bat
C:\UTIL\UMCFIX.EXE /Q
```

It also provides a check mode:

```bat
UMCFIX /CHECK
```

The idea is for the system to boot with its original BIOS and for UMCFIX to apply the stable timing immediately, before starting games, drivers, networking, or Windows 95.

---

## 22. Final decisions

The following decisions were made:

- Keep `A4[1:0]=00b` as the stable fix.
- Rule out `A4[1:0]=10b` because it reproduces palette corruption.
- Do not test `11b` because it is unknown or reserved.
- Do not use UM8881F bits 52.7 and 53.7 as a fix.
- Do not use TURBO OFF as a permanent workaround because of its performance cost.
- Keep UMCA4 as a diagnostic tool.
- Use UMCFIX as the permanent solution from `AUTOEXEC.BAT`.
- Do not modify the motherboard BIOS yet, since the software fix is simple, reversible, and sufficiently robust.

---

## 23. Conclusion

The investigation began as a VGA palette problem and ended by identifying a much more general timing fault in the PCChips M915i's PCI subsystem.

The turning point was establishing that TURBO OFF mainly affected external accesses rather than the CPU's internal performance. From there, the tests narrowed down the problem until they demonstrated that software delays were not enough: the bus timing itself had to be changed.

The controlled change to `UM8886A:A4[1:0]` from `01b` to `00b` produced a complete and reproducible fix:

- zero errors in the DAC tests;
- correct palettes in all tested games;
- stable Windows 95;
- stable RTL8029;
- CPU and RAM running at normal speed;
- only a moderate loss of real-world performance.

Therefore, the final adopted solution is:

```text
UM8886A A4[1:0] = 00b
```

applied automatically by **UMCFIX** during DOS startup.

The available evidence supports treating this configuration as the practical fix for the critical PCI timing fault on this particular machine.
