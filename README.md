# Prometheus99: Ultra-Lightweight Industrial HMI Runtime

> **Problem Statement:** PS4 — Lightweight HMI Runtime  
> **Team Name:** Team Doomsday (Abhilash L, Adarsh Abhilash, Nikhil Nuguri)  
> **Repository:** [https://github.com/adarsh-p-abhilash/prometheus99](https://github.com/adarsh-p-abhilash/prometheus99)  

---

## 🎯 Overview

- **Our Goal:** To engineer a lightweight HMI system so optimized it can run seamlessly on virtually any hardware constraint.
- **The Problem:** Standard industrial HMI models require massive amounts of memory, storage, and processing power just to function.
- **The Impact:** Beats or matches the most lightweight systems on the market by completely eliminating software bloat.

### 🏆 Benchmarks & Results
- **Application Size:** 30 Kilobytes
- **Final Memory Usage:** Less than 200 KB (0.10 MB during live rendering)
- **Heap Allocations:** 0 Bytes (Zero dynamic memory allocation)

---

## ⚙️ How We Achieved That Goal

- **Smart Graphics Processing:** Utilized the LVGL library with a 4-bit indexed color model and a partial draw band buffer to drastically cut the visual footprint.
- **Infinite Uptime:** Engineered with 100% static memory (zero dynamic heap) to guarantee zero memory leaks or crashes over time.
- **Efficient Data Handling:** Serialized data using fixed-width binary structs and managed logic via Finite State Machines for predictable, reliable execution.
- **Thread Safety:** Implemented mutex locks and atomic double-buffering to safely sync data, ensuring high-speed background threads never overwrite the slower screen refresh.
- **Built-in Accessibility:** Includes a high-contrast mode specifically designed for visually impaired operators and harsh factory lighting conditions.

---

## 🚀 Future Scalability and Proof of Concept

- **Proven Proof of Concept:** Not a simulation. The system actively processes live, physical hardware sensor data in real-time.
- **Innovative:** Achieves high performance through extreme subtraction and efficiency, rather than relying on expensive hardware upgrades.
- **Highly Scalable:** Because the footprint is so small, the exact same system can scale from a high-end industrial PC down to a $3 edge microchip.
- **Reliable for Modern Systems:** Mathematically immune to memory-fragmentation crashes, providing the continuous, safe uptime demanded by modern industrial factories.

---

## 🛠️ Quick Start & Controls

### Building & Running
```cmd
# Build with automated MSVC / GCC size optimization:
.\build.bat

# Run executable:
.\prometheus99.exe
```

### Controls & Keybindings
| Key / Input | Action |
|:---|:---|
| **`1` – `5`** | Navigate between Dashboard, Diagnostics, Alarm, Settings, and Failover |
| **`A` / Click WD Pill** | Acknowledge active alarm & silence horn |
| **`C`** | Toggle High-Contrast Accessibility Theme |
| **`F`** | Toggle Standby Node (Primary Node A $\leftrightarrow$ Hot Standby Node B) |
| **`T`** | Test Alarm Trip & Top Header Watchdog Flasher |
| **`W`** | Simulate Watchdog Timeout Fault (Auto-engages failover) |
| **Mouse / Touch** | Full interactive digitizer hit-testing on all screen buttons and navbar |
