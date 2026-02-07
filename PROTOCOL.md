# TCL AC UART Protocol Documentation

Technical documentation of the TCL Air Conditioner UART protocol (Realtek RTL8710C WiFi module).

**Reverse-engineered from 1,757 captured UART packets** (1,253 MCU→AC, 504 AC→MCU) from a real TCL AC unit using a serial sniffer on both UART directions.

## Table of Contents
1. [Overview](#overview)
2. [Packet Structure](#packet-structure)
3. [Commands](#commands)
4. [SET Packet (MCU→AC, 38 bytes)](#set-packet-mcuac-38-bytes)
5. [POLL Packet (MCU→AC, 31 bytes)](#poll-packet-mcuac-31-bytes)
6. [STATUS Response (AC→MCU, 61 bytes)](#status-response-acmcu-61-bytes)
7. [Power Response (AC→MCU, 51 bytes)](#power-response-acmcu-51-bytes)
8. [Other Packet Types](#other-packet-types)
9. [Temperature Encoding](#temperature-encoding)
10. [Checksum Algorithm](#checksum-algorithm)
11. [Validation Results](#validation-results)

---

## Overview

**Communication Settings:**
- Baud Rate: **9600**
- Data Bits: **8**
- Parity: **EVEN** (critical!)
- Stop Bits: **1**
- Protocol: **8E1**

**WiFi Module:** RTL8710C (firmware V8-R82CT04-LF1V025.0.1.11)  
**Cloud:** AWS IoT Shadow (thing ID: DBw3ChFAAAI)

**Direction Markers:**
| Direction | Header | Byte[1] | Byte[2] |
|-----------|--------|---------|---------|
| MCU → AC | `BB 00 01` | 0x00 | 0x01 |
| AC → MCU | `BB 01 00` | 0x01 | 0x00 |

---

## Packet Structure

All packets follow this generic structure:

```
Offset  Size  Description
------  ----  -----------
[0]     1     Header: 0xBB (always)
[1]     1     Direction byte 1
[2]     1     Direction byte 2
[3]     1     Command ID (0x03-0x0B)
[4]     1     Data length: N
[5..4+N] N    Data payload
[5+N]   1     XOR checksum of bytes [0..4+N]
```

---

## Commands

| CMD  | Name           | Direction | Total Size | Data Length | Description |
|------|----------------|-----------|------------|-------------|-------------|
| 0x03 | SET_PARAMS     | MCU→AC    | 38 bytes   | 32          | Set AC parameters |
| 0x03 | SET_RESPONSE   | AC→MCU    | 61 bytes   | 55          | Acknowledge SET with full status |
| 0x04 | POLL           | MCU→AC    | 31 bytes   | 25          | Request status update |
| 0x04 | POLL_RESPONSE  | AC→MCU    | 61 bytes   | 55          | Status response (same format as SET_RESPONSE) |
| 0x05 | TEMP_SET       | MCU→AC    | 38 bytes   | 32          | Temperature-only SET |
| 0x05 | TEMP_RESPONSE  | AC→MCU    | 17 bytes   | 11          | Temperature data |
| 0x09 | SHORT_QUERY    | MCU→AC    | 8 bytes    | 2           | Short status query |
| 0x09 | SHORT_RESPONSE | AC→MCU    | 51 bytes   | 45          | Short status (100% constant) |
| 0x0A | POWER_QUERY    | MCU→AC    | 9 bytes    | 3           | Power status query |
| 0x0A | POWER_RESPONSE | AC→MCU    | 51 bytes   | 45          | Power state + counters |
| 0x0B | TIME_SYNC      | MCU→AC    | 22 bytes   | 16          | Set date/time |
| 0x0B | TIME_ACK       | AC→MCU    | 8 bytes    | 2           | Time sync acknowledgement |

---

## SET Packet (MCU→AC, 38 bytes)

**43 captured SET packets analyzed, 100% checksum validated.**

### Complete Byte Map

```
Byte  Default  Description
----  -------  -----------
[0]   0xBB     Header
[1]   0x00     Direction (MCU→)
[2]   0x01     Direction (→AC)
[3]   0x03     Command: SET_PARAMS
[4]   0x20     Data length: 32
[5]   0x03     Constant (required)
[6]   0x01     Constant (required)
[7]   varies   Mode + Flags (see below)
[8]   varies   Operating Mode + Special Flags (see below)
[9]   varies   Target temperature: raw = 111 - celsius
[10]  varies   Fan speed (bits 0-2) + Vertical swing enable (bits 3-5)
[11]  varies   Horizontal swing enable (0x08 = enabled)
[12]  0x00     Temperature unit (0x00 = Celsius)
[13]  0x01     Constant (required — AC rejects without this!)
[14-18] 0x00   Reserved
[19]  varies   Sleep mode (0=OFF, 1=Mode1, 2=Mode2)
[20-28] 0x00   Reserved
[29]  0x20     Constant (required)
[30]  0x00     Reserved
[31]  0x00     Unused (always 0x00 in all 43 captures)
[32]  varies   Vertical swing direction (bits 3-4) + airflow position (bits 0-2)
[33]  varies   Horizontal swing direction (bits 3-5) + airflow position (bits 0-2)
[34-36] 0x00   Reserved
[37]  varies   XOR Checksum
```

### Byte[7]: Power / Display / Beeper / ECO

```
Bit 7 (0x80): ECO mode       — 1x observed (with AUTO)
Bit 6 (0x40): Display ON     — 5x observed
Bit 5 (0x20): Beeper ON      — 35/43 = 81% (default ON)
Bit 4 (0x10): Reserved
Bit 3 (0x08): Reserved
Bit 2 (0x04): POWER ON       — absence = OFF
Bits 0-1:     Reserved
```

**Observed Values:**
| Value | Binary   | Count | Meaning |
|-------|----------|-------|---------|
| 0x24  | 00100100 | 35x   | Beeper + Power ON |
| 0x64  | 01100100 | 5x    | Display + Beeper + Power ON |
| 0xA4  | 10100100 | 1x    | ECO + Beeper + Power ON |
| 0x20  | 00100000 | 1x    | Beeper only (Power OFF) |
| 0x44  | 01000100 | 1x    | Display + Power ON (no beeper) |

### Byte[8]: Operating Mode (bits 0-3) + Special Flags (bits 4-7)

```
Bit 7 (0x80): Quiet mode
Bit 6 (0x40): Turbo mode
Bit 5 (0x20): Health mode
Bit 4 (0x10): Comfort mode
Bits 0-3:     Operating mode
```

**Operating Modes (bits 0-3):**
| Value | Mode     | Count | Notes |
|-------|----------|-------|-------|
| 0x01  | HEAT     | 34x   | Most common in capture |
| 0x02  | DRY      | 1x    | |
| 0x03  | COOL     | 2x    | |
| 0x07  | FAN_ONLY | 1x    | |
| 0x08  | AUTO     | 1x    | Usually with ECO |

**Combined with Turbo (value 0x41 = TURBO|HEAT, 3x observed).**

### Byte[10]: Fan Speed (bits 0-2) + Vertical Swing Enable (bits 3-5)

**Fan speed is in byte[10] bits 0-2, NOT byte[8]!**

| Value | Speed       | Count |
|-------|-------------|-------|
| 0x00  | Auto        | 32x   |
| 0x02  | Medium-Low  | 3x    |
| 0x05  | High        | 3x    |
| 0x06  | Very High   | 1x    |

**Vertical swing enable: bits 3-5 all set (0x38) activates vertical swing.**

| Value | Meaning |
|-------|---------|
| 0x00  | No swing, Auto fan |
| 0x38  | Vertical swing ON, Auto fan |
| 0x3D  | Vertical swing ON + High fan |

### Byte[32]: Vertical Direction

```
Bits 3-4: Swing mode (0=OFF, 1=UP_DOWN, 2=UPSIDE, 3=DOWNSIDE)
Bits 0-2: Fixed position (0=Last, 1=MaxUp, 2=Up, 3=Center, 4=Down, 5=MaxDown)
```

**Common:** 0x1D = DOWNSIDE swing + MAX_DOWN position (34/43 = 79%)

### Byte[33]: Horizontal Direction

```
Bits 3-5: Swing mode (0=OFF, 1=LeftRight, 2=Leftside, 3=Center, 4=Rightside)
Bits 0-2: Fixed position (0=Last, 1=MaxLeft, 2=Left, 3=Center, 4=Right, 5=MaxRight)
```

**Common:** 0x25 = RIGHTSIDE swing + MAX_RIGHT position (28/43 = 65%)

### Example SET Packet (HEAT mode, 25°C, Beeper ON)
```
BB 00 01 03 20 03 01 24 01 56 00 00 00 01 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 20 00 00
1D 25 00 00 00 F1
                ^^─ checksum
```

---

## POLL Packet (MCU→AC, 31 bytes)

**979 identical POLL packets captured. The content is 100% constant — a pure heartbeat.**

```
BB 00 01 04 19 00 00 00 08 0F 00 00 00 06 00 00
00 00 00 00 1F 1F 1F 1F 1F 1F 1F 1F 1F 1F A6
```

> **Note:** Previous implementations used a 7-byte POLL (`BB 00 01 04 01 00 A6`).
> The correct POLL is 31 bytes as shown above. The 7-byte version may still work
> but is not what the original firmware sends.

---

## STATUS Response (AC→MCU, 61 bytes)

**416 STATUS packets analyzed (CMD 0x03 and 0x04 responses).**  
**99.2% valid checksums. 32 variable bytes, 29 constant bytes.**

### Key Payload Bytes (offsets from payload start, payload = packet[5..59])

**IMPORTANT: STATUS and SET use completely different byte layouts!**

```
Offset  Description
------  -----------
[0]     Type indicator: 0x04 (constant)
[1]     Sub-type: 0x00 (constant)
[2]     mainPara — MAIN STATUS BYTE (power, ECO, mode)
[3]     secPara — Fan speed (upper nibble) + Target temp (lower nibble)
[4]     Comfort flag (bit 2 = comfort mode)
[5]     Swing mode (bits 5-6: 0x00=off, 0x20=horiz, 0x40=vert, 0x60=both)
[6-11]  Reserved (zeros)
[12-13] Room temperature — 16-bit NTC sensor value (see Temperature section)
[14]    Sleep mode (bit 0 = sleep active)
[15-24] AC state data (0x1F padding when OFF, zeros when ON)
[25]    Internal pipe/evaporator sensor (NOT room temperature!)
[26]    Usually 0xFF
[27]    Usually 0x42
[28]    Quiet fan flag (bit 7 = quiet mode active)
[29-39] Extended status / sensor data
[40-54] Timing data, sensor readings
```

### Payload[2]: mainPara — Power State + Mode Detection

**IMPORTANT: The bit meanings are completely different from SET byte[7]!**

```
Bit 7 (0x80): Power OFF / standby indicator
Bit 6 (0x40): ECO mode (NOT display! Display is write-only)
Bit 5 (0x20): Part of ON pattern (NOT beeper! Beeper is write-only)
Bit 4 (0x10): Power ON / mode active (THE reliable ON indicator!)
Bits 0-3:     Operating mode (see below)
Bit 2 (0x04): Power-latch (remains set after IR remote OFF — unreliable!)
```

**Power logic:** `ac_is_on = (bit4 is set) AND (bit7 is clear)`

> **CRITICAL:** Bit 2 (0x04) is a power-latch that stays set even when the AC is turned
> off via IR remote! The correct ON indicator is **bit 4 (0x10)**, matching the
> original I-am-nightingale/tclac project which checks `dataRX[7] & (1 << 4)`.

> **CRITICAL:** Display and beeper flags are **NOT present** in STATUS responses!
> They exist only in SET byte[7]. Bit 6 is ECO mode, bit 5 is part of the ON-pattern.
> Health and turbo are also write-only (SET byte[8]).

**Mode detection (lower nibble, bits 0-3):**
| Value | Mode     |
|-------|----------|
| 0x01  | COOL     |
| 0x02  | FAN_ONLY |
| 0x03  | DRY      |
| 0x04  | HEAT     |
| 0x05  | AUTO     |

**Observed mainPara values:**
| Value | Binary   | Count | Bit4 | Meaning |
|-------|----------|-------|------|----------|
| 0x00  | 00000000 | 151x  | 0    | Fully OFF / idle |
| 0x14  | 00010100 | live  | 1    | ON, HEAT mode (lower nibble=4) |
| 0x34  | 00110100 | 130x  | 1    | ON, HEAT mode |
| 0x35  | 00110101 | live  | 1    | ON, AUTO mode (lower nibble=5) |
| 0x32  | 00110010 | live  | 1    | ON, FAN_ONLY mode (lower nibble=2) |
| 0x31  | 00110001 | live  | 1    | ON, COOL mode (lower nibble=1) |
| 0xB4  | 10110100 | 81x   | 1*   | Power OFF transition (bit7=1 overrides → OFF) |
| 0x24  | 00100100 | 47x   | 0    | **OFF via IR remote** (bit2 still set, but bit4=0!) |
| 0x74  | 01110100 | 3x    | 1    | ON, HEAT + ECO (bit6=1) |

### Payload[3]: secPara — Fan Speed + Target Temperature

```
Bits 4-7 (upper nibble): Fan speed
Bits 0-3 (lower nibble): Target temperature = value + 16 (range 16-31°C)
```

**Fan speed constants (upper nibble):**
| Value | Fan Speed |
|-------|-----------|
| 0x80  | AUTO      |
| 0x90  | LOW       |
| 0xA0  | MEDIUM    |
| 0xB0  | FOCUS     |
| 0xC0  | MIDDLE    |
| 0xD0  | HIGH      |

> **IMPORTANT:** These fan speed values differ from SET byte[10]! STATUS uses the
> upper nibble of payload[3], while SET uses byte[10] bits 0-2. The constants are
> also different (e.g., SET HIGH=0x05, STATUS HIGH=0xD0).

**Target temperature examples:**
| secPara | Lower nibble | Temp (°C) |
|---------|-------------|----------|
| 0xB5    | 0x05        | 21°C     |
| 0x8A    | 0x0A        | 26°C     |
| 0xBE    | 0x0E        | 30°C     |

### Payload[5]: Swing Mode

```
Bits 5-6 (0x60 mask): Swing mode
  0x00 = OFF
  0x20 = Horizontal swing
  0x40 = Vertical swing
  0x60 = Both (horizontal + vertical)
```

### Payload[4]: Comfort Mode

Bit 2 (0x04): Comfort preset active.

### Payload[14]: Sleep Mode

Bit 0 (0x01): Sleep preset active.

### Payload[28]: Quiet Fan

Bit 7 (0x80): Quiet fan mode active.

### Payload[12:13]: Room Temperature (16-bit NTC Sensor)

**Formula:** `celsius = ((payload[12] << 8 | payload[13]) / 374.0 - 32.0) / 1.8`

This is a 16-bit NTC thermistor value. The formula first converts to Fahrenheit (divided by 374),
then to Celsius. Matches the original I-am-nightingale/tclac project (`dataRX[17:18]`).

| Raw (hex) | Raw (dec) | Fahrenheit | Celsius | Validation |
|-----------|-----------|------------|---------|------------|
| 0x6E23    | 28195     | 75.4°F     | 24.1°C  | DHT22 = 24.5°C ✓ |
| 0x6E03    | 28163     | 75.3°F     | 24.1°C  | Stable during ON/OFF |
| 0x6623    | 26147     | 69.9°F     | 21.1°C  | Plausible room temp ✓ |
| 0x692B    | 26923     | 72.0°F     | 22.2°C  | Plausible room temp ✓ |

**Filter:** Accept calculated values in range 0°C to 50°C.

> **Note:** Works in ALL states (ON and OFF). No warmup delay. This is the primary
> internal temperature source.

### Payload[25]: Internal Pipe/Evaporator Sensor (NOT Room Temp!)

This byte does NOT contain room temperature. It appears to be an internal AC sensor
(evaporator coil or refrigerant pipe).

| Raw  | Hex  | raw-127 | State | Notes |
|------|------|---------|-------|-------|
| 120  | 0x78 | -7°C    | ON (startup) | Evaporator cooling down |
| 175  | 0xAF | 48°C    | ON (COOL) | Evaporator at full cooling |
| 100  | 0x64 | -27°C   | Compressor stopped | Warming back up |
| 102  | 0x66 | -25°C   | OFF | Idle baseline |

### Example STATUS Packets

**OFF state:**
```
BB 01 00 04 37 04 00 00 8A 00 00 00 00 00 00 00
00 69 23 08 1F 1F 1F 1F 1F 1F 1F 1F 1F 1F 66 FF
42 00 00 20 20 1F 00 00 80 00 00 00 00 E8 00 00
00 54 40 00 00 00 00 7A 00 00 00 00 87
```

**ON (cooling, 23°C room temp):**
```
BB 01 00 03 37 04 00 34 BD 00 00 00 00 00 00 00
00 6B 0B 88 00 00 00 00 00 00 00 00 00 20 96 FF
42 00 3C 21 11 5A 52 00 C0 00 00 00 00 E7 00 00
00 54 40 1D 25 00 00 7A 00 00 00 00 95
```

---

## Power Response (AC→MCU, 51 bytes)

**CMD 0x0A: 38 packets analyzed. Most bytes are zero.**

### Key Bytes

```
Offset  Description
------  -----------
[2]     Power flag: 0x0C in all observed responses (unreliable for power state!)
[3]     Secondary flags (0x85 observed)
[4]     Sub-flag (0x05 observed)
[15]    Unknown diagnostic byte (0x59 observed)
[16]    Room temperature: raw - 127 = °C  ← VALIDATED (see below)
[17]    Counter/timestamp byte (increments slowly: 0x3B, 0x3C, ...)
[18]    Unknown (0x31 observed)
```

> **IMPORTANT:** Payload[2] (power flag) always returns 0x0C regardless of whether the AC
> is ON or OFF. Do NOT use this byte for power state detection. Use STATUS payload[2]
> (mainPara) as the authoritative power state source.

### Room Temperature from CMD 0x0A

**Formula:** `celsius = raw - 127` (same as STATUS payload[25])

**Validated:** payload[16] = 0x93 → 147 - 127 = **20°C**, matching DHT22 sensor reading of 19.9°C.

> **⚠️ IMPORTANT:** This temperature is **only valid when AC is OFF** (standby).
> When AC is ON, payload[16] contains non-temperature data (observed 0x46 = garbage).
> The STATUS response payload[25] is the correct temperature source when AC is running
> (after ~3–5 min evaporator warmup).

| Raw  | Hex  | Temperature | Notes |
|------|------|-------------|-------|
| 147  | 0x93 | 20°C        | Validated against DHT22 (19.9°C) |

**Filter:** Accept raw values in range 137-167 (10°C to 40°C).

### Example Power Response (AC OFF, 20°C room)
```
BB 01 00 0A 2D 04 00 0C 85 05 00 00 00 00 00 00
00 00 00 00 59 93 3B 31 00 00 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 D5
            ^^─ payload[16]=0x93 → 20°C
```

### Power Query (MCU→AC, 9 bytes)
```
BB 00 01 0A 03 02 00 05 B4
```

---

## Other Packet Types

### Short Status Query/Response (CMD 0x09)

**Query (8 bytes):** `BB 00 01 09 02 04 00 B5`  
**Response (51 bytes):** 100% constant — device capabilities. All bytes fixed.

### Time Sync (CMD 0x0B)

**MCU→AC, 22 bytes:**
```
Offset  Description
------  -----------
[7-8]   Year (16-bit big-endian, e.g., 0x07E9 = 2025)
[9]     Month (1-12)
[10]    Day (1-31)
[11]    Hour (0-23)
[12]    Minute (0-59)
[13]    Second (0-59)
[14-20] Reserved (zeros)
```

**Example:** `BB 00 01 0B 10 05 00 E9 07 0A 10 09 2C 34 00 00 00 00 00 00 00 41`  
= 2025-10-16 09:44:52

---

## Temperature Encoding

### Target Temperature (SET Byte[9])

**Formula:** `raw = 111 - celsius`  
**Inverse:** `celsius = 111 - raw`

| Celsius | Raw  | Hex  | Observed |
|---------|------|------|----------|
| 16°C    | 95   | 0x5F | 2x       |
| 22°C    | 89   | 0x59 | -        |
| 24°C    | 87   | 0x57 | 1x       |
| 25°C    | 86   | 0x56 | 32x      |
| 26°C    | 85   | 0x55 | 4x       |
| 28°C    | 83   | 0x53 | 2x       |

### Room Temperature

**Primary source:** STATUS payload[12:13] — 16-bit NTC sensor, works in ALL states.

**Formula:** `celsius = ((payload[12] << 8 | payload[13]) / 374.0 - 32.0) / 1.8`

This matches the original I-am-nightingale/tclac project. The 16-bit value represents
an NTC thermistor reading that is first converted to Fahrenheit (÷374), then to Celsius.

| Raw (hex) | Celsius | Source | Validation |
|-----------|---------|--------|------------|
| 0x6E23    | 24.1°C  | STATUS | DHT22 = 24.3-24.5°C ✓ |
| 0x6623    | 21.1°C  | STATUS | Plausible ✓ |
| 0x692B    | 22.2°C  | STATUS | Plausible ✓ |

> **Previous incorrect approach:** payload[25] with `raw - 127` was identified as an
> internal evaporator/pipe sensor, NOT room temperature. It reads -20°C to 48°C during
> active cooling, which is clearly not ambient temperature.

**External sensor recommended:** For maximum accuracy, use a DHT22/SHT30 as the
primary temperature source. The internal NTC (±0.5°C) works as a fallback.

**Filter:** Accept calculated values in range 0°C to 50°C.

---

## Checksum Algorithm

**Simple XOR of all bytes except the checksum byte itself.**

```c
uint8_t xor_checksum(const uint8_t *data, size_t length) {
    uint8_t cs = 0;
    for (size_t i = 0; i < length; i++) cs ^= data[i];
    return cs;
}
```

**Validation:** 1,743 of 1,757 packets (99.2%) had valid checksums. The 14 invalid ones were due to concatenated/corrupted log entries.

---

## Validation Results

### Capture Statistics

| Direction | Packets | Checksum Valid |
|-----------|---------|----------------|
| MCU → AC  | 1,253   | 99%+           |
| AC → MCU  | 504     | 99%+           |
| **Total** | **1,757** | **99.2%**    |

### Packet Type Distribution (MCU→AC)

| Type | CMD | Size | Count | Notes |
|------|-----|------|-------|-------|
| POLL | 0x04 | 31 bytes | 979 | 100% identical (heartbeat) |
| SHORT_QUERY | 0x09 | 8 bytes | 105 | 100% identical |
| POWER_QUERY | 0x0A | 9 bytes | 102 | 97% variant 0x05, 3% variant 0x0D |
| SET | 0x03 | 38 bytes | 43 | Main control packets |
| TEMP_SET | 0x05 | 38 bytes | 16 | Temperature-only changes |
| TIME_SYNC | 0x0B | 22 bytes | 4 | Date/time updates |

### Known Limitations

1. **SET vs STATUS layout**: SET and STATUS use completely different byte layouts. SET byte[7]=power/display/beeper, but STATUS byte[7]=power/ECO/mode. SET byte[8]=mode, but STATUS byte[8]=fan speed + target temp. Never assume the same byte has the same meaning in both directions.
2. **CMD 0x0A power flag unreliable**: payload[2] always returns 0x0C regardless of AC power state. Do not use for ON/OFF detection.
3. **Aux query batching**: CMD 0x09 and 0x0A must be sent individually (not back-to-back). The AC ignores the second query if two are sent in rapid succession. Stagger them between POLL cycles.
4. **Power bit 2 vs bit 4**: STATUS payload[2] bit 2 (0x04) is a power-latch that stays set after IR remote OFF. Always use bit 4 (0x10) for reliable ON/OFF detection.
5. **Payload[25] is NOT room temperature**: This byte tracks an internal evaporator/pipe sensor. Use payload[12:13] with the 16-bit NTC formula instead.

---

## References

- **This Implementation**: Based on serial sniffer captures from Oct 2025
- **I-am-nightingale Repository**: https://github.com/I-am-nightingale/tclac
- **ESPHome Climate**: https://esphome.io/components/climate/

---

**Document Version:** 2.4  
**Last Updated:** February 2026  
**Based on:** 1,757 packets from real TCL AC unit (RTL8710C) + live ESPHome validation  
**Validation Status:** Production-ready  
**Key fixes in v2.4:**
- Complete STATUS field mapping: target temp, fan speed, swing mode, presets (ECO/comfort/sleep/quiet)
- Mode detection from STATUS: lower nibble of mainPara (1=cool, 2=fan, 3=dry, 4=heat, 5=auto)
- Corrected mainPara bit layout: bit 6 = ECO (not display), bit 5 = ON-pattern (not beeper)
- Display, beeper, health, turbo confirmed as write-only (not present in STATUS responses)
- Fan speed readback from secPara upper nibble with STATUS-specific constants
**Key fixes in v2.3:**
- Room temperature: 16-bit NTC formula from payload[12:13] (was incorrectly using payload[25])
- ON/OFF detection: bit 4 (0x10) is reliable, bit 2 (0x04) is a power-latch (stays ON after IR OFF)
- Both fixes validated against original I-am-nightingale/tclac project and live testing
