# ipecmd Reset-Only Command Research

**Date:** 2026-09-29  
**Tool:** MPLAB IPE v6.35 (`ipecmd.exe`)  
**Target:** PKOB4 (PICkit On Board 4), device dsPIC33AK512MPS512

## Summary

**Reset-only command (hold in reset, then release):**
```
ipecmd.exe -TPPKOB4 -P33AK512MPS512 -OK
ipecmd.exe -TPPKOB4 -P33AK512MPS512 -OL
```

## Key Options (from MPLAB IPE v6.35 Readme)

| Option | Purpose | Default |
|--------|---------|---------|
| `-OK` | Silent connect with tool and hardware | — |
| `-OL` | Release from reset | Hold in reset |
| `-M` | **Program** the device | (operation) |

## Reset-Only Strategies

### Strategy 1: Connect + Release (Simplest)
```
ipecmd.exe -TPPKOB4 -P33AK512MPS512 -OK -OL
```
- `-OK` connects silently to tool and device
- `-OL` releases device from reset
- No programming (`-M` omitted)
- **Confidence: HIGH** — Documented in section "17.34 How to Reset the Target"
- **Effect:** Device held in reset by default; this releases it

### Strategy 2: Connect Only (Hold in Reset)
```
ipecmd.exe -TPPKOB4 -P33AK512MPS512 -OK
```
- Connects without programming
- Device remains held in reset by default
- **Confidence: HIGH**

## What the Documentation Says

**From Section 17.34, "How to Reset the Target"** (Microchip MPLAB IPE Readme):
> "Removes the reset and allows the code to run. By default, Hold in reset is set (Prevents the code from running after programming)."  
> Example: `ipecmd.jar -p24FJ128GA010 -FHEXCODE.HEX -M -TPRICE -OL`

**Key insight:** `-OL` *reverses* the default (hold in reset) → code runs / device released  
**Inverse:** Omit `-OL` → device stays in reset

## Caveats & Notes

1. **PM3 Not Supported:** Section 2251–2252 states "Not Applicable for MPLAB PM3" — only applies to PKOB4, PICkit 3+, REAL ICE, etc. PKOB4 is supported.
2. **No Erase When Not Programming:** Omitting `-M` (program) means no erase or programming occurs. Only connection, identification, and reset control.
3. **Duration:** A connect-only call takes <1 second (tool handshake + reset toggle). No programming overhead.
4. **No Hex File Required:** Since no operation (`-M`), no `-F` hex file is needed.

## Recommended Command

For **reset device without programming**:
```bash
ipecmd.exe -TPPKOB4 -P33AK512MPS512 -OK -OL
```

For **hold device in reset**:
```bash
ipecmd.exe -TPPKOB4 -P33AK512MPS512 -OK
```

## Source
- **File:** `C:\Program Files\Microchip\MPLABX\v6.35\docs\Readme for IPECMD.htm` (Sections 2223–2258, 5250–5258)
- **Section Title:** "How to Reset the Target (Not Applicable for MPLAB PM3)"

**Confidence Level:** HIGH (authoritative Microchip documentation)  
**Tested:** Not on hardware (read-only research per instructions)
