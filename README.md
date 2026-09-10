# Prometheus99: Lightweight HMI Runtime

> **Architects:** Team Doomsday (Abhilash L, Adarsh Abhilash, Nikhil Nuguri)  
> **Language Standard:** C99 (`ISO/IEC 9899:1999`)  
> **Graphics Engine:** LVGL Presentation Architecture  
> **Target:** Microcontrollers (MCU/RTOS), Embedded Linux, & Windows Desktop (Win32 GDI)

---

## Overview

**Prometheus99** is a lightweight, deterministic HMI (Human-Machine Interface) runtime engine designed for resource-constrained embedded environments and high-reliability industrial control systems.

### Key Technical Pillars

1. **Up to 90% Resource & Overhead Reduction**:
   - **Zero Heap Fragmentation**: Static memory allocation (`BSS`/`Data` section) post-init. Zero `malloc`/`free` calls during runtime execution.
   - **Packed C99 Binary Data Ingestion**: Sensor telemetry data is ingested into a 36-byte packed C99 binary structure with CRC16 data integrity checks. Binary data is parsed into human-readable strings strictly at the Layer 3 presentation layer.

2. **Dual-Loop Watchdog Supervisor (AND-Gate Architecture)**:
   - Monitors both the **Hardware Heartbeat** (Layer 1 Display Flush Callback) and the **Software Heartbeat** (Layer 3 UI Timer 30 Hz loop).
   - If both loops report alive within the 500 ms window, the supervisor pets the physical hardware watchdog. If either loop hangs or fails, the runtime initiates a hot-standby failover.

3. **Input Agnosticism & Accessibility**:
   - Unified input group abstraction accommodating physical buttons, touch digitizers, rotary encoders, keyboard shortcuts, and CLI commands.
   - Built-in High Contrast accessibility theme toggleable at runtime.

---

## 4-Layer Architecture Model

```
+-------------------------------------------------------------------------+
|                        Layer 0: Hardware & I/O                          |
|   Sensors / Fieldbus, Touch Digitizer, Buttons, Display, Hardware WDT   |
+------------------------------------+------------------------------------+
                                     |
                               Raw Hardware Data
                                     |
+------------------------------------+------------------------------------+
|                        Layer 1: HAL & Drivers                           |
|   1000 Hz Sensor ISR, Debounced Input Drivers, Display Flush Callback   |
+------------------------------------+------------------------------------+
                                     |
                               Binary Telemetry
                                     |
+------------------------------------+------------------------------------+
|                   Layer 2: Core & State Management                      |
|   Shared State Buffer, FSM Controller, Dual-Loop Watchdog Supervisor    |
+------------------------------------+------------------------------------+
                                     |
                              Dirty Flag & Age
                                     |
+------------------------------------+------------------------------------+
|                       Layer 3: LVGL Presentation                        |
|   LVGL Input Group, 30 Hz UI Timer, Pre-Allocated Static Screens        |
+-------------------------------------------------------------------------+
```

---

## Building and Running

### Prerequisites
- GCC compiler supporting C99 standard (e.g. MinGW64 / MSYS2 GCC on Windows or GCC/Clang on Linux).

### Quick Build (Windows GCC)
```cmd
.\build.bat
```

### Run Executable
```cmd
.\prometheus99.exe
```
or double-click `Run_HMI_Runtime.bat`.

---

## Keyboard Shortcuts & HMI Controls

| Key | Action |
| :--- | :--- |
| **`1`** | Navigates to **Boot Screen** (System progress bar & initialization checklist) |
| **`2`** | Navigates to **Dashboard Screen** (Live gauges for Temp, Pressure, RPM, Voltage) |
| **`3`** | Navigates to **Diagnostics Screen** (Inspect raw hex binary telemetry packet, CRC16, & memory audit) |
| **`4`** | Navigates to **Alarm Screen** (Active trip condition monitor & safety controls) |
| **`5`** | Navigates to **Settings Screen** (HMI refresh specs & accessibility options) |
| **`6` / `F`** | Toggles **Hot Standby Failover Mode** |
| **`A`** | Acknowledges active alarms |
| **`C`** | Toggles between Dark HMI Mode & High Contrast Accessible Mode |
| **Mouse Click** | Touch digitizer navigation on bottom HMI tabs |
