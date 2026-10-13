# POV Hologram Display System (v4)

**High-Speed Cylindrical Persistence of Vision (POV) Display with ESP8266 & Dual Addressable RGB LED Strips**  
**Designed & Developed by:** Aryan Khona

---

## 🌟 Project Overview
The POV Hologram is an electro-mechanical persistence of vision display engineered to render high-resolution mid-air floating text, analog & digital clocks, dates, animated vector icons, and custom pixel art.

By rotating dual addressable RGB LED strips at 900–1300 RPM and synchronizing stroboscopic pulses via a Hall-effect magnetic sensor, individual radial flashes merge seamlessly into floating cylindrical imagery.

---

## 🔌 Circuit Diagram & Architecture
The electrical schematic is split into two independent power stages:
- **Motor Subsystem (Stator):** 4.2V Li-ion battery $\rightarrow$ ON/OFF Switch $\rightarrow$ TP4056 charging module $\rightarrow$ 1N4007 protection diode $\rightarrow$ 3.5V High-Speed DC Motor.
- **Rotor Subsystem (Armature):** 4.2V Li-ion battery $\rightarrow$ ON/OFF Switch $\rightarrow$ TP4056 charging module $\rightarrow$ 1N4007 diode $\rightarrow$ NodeMCU ESP8266 & Dual WS2812 Strips.
- **Sensors & Dividers:** A3144 Hall-effect sensor to D7 (GPIO 13), and 10kΩ/10kΩ 1:1 voltage divider to A0 (ADC0) for battery monitoring.

---

## ⚡ Technical Specifications & Pin Configuration

| Component | ESP8266 Pin | GPIO | Function |
| :--- | :---: | :---: | :--- |
| **WS2812 Strip-1 (Blade A)** | `D1` | GPIO 5 | 8 LEDs, 800 kHz bitbang data out |
| **WS2812 Strip-2 (Blade B)** | `D2` | GPIO 4 | 8 LEDs, 180° opposite blade data out |
| **Hall Effect Sensor (A3144)** | `D7` | GPIO 13 | 1 pulse / revolution (Zero-index interrupt trigger) |
| **Battery Divider (10kΩ/10kΩ)** | `A0` | ADC0 | Real-time analog battery measurement |
| **DC Motor Drive** | Separate Supply | — | Dedicated 3.5V supply for rotor rotation |
| **CPU Core Frequency** | — | — | 160 MHz overclock for microsecond timing stability |

---

## 📶 Wi-Fi & Network Configuration

| Mode | SSID | Password | IP Address | Role |
| :--- | :--- | :--- | :--- | :--- |
| **Station (STA)** | `POV_Hologram_STA_MODE` | `013171317` | `10.218.184.220` | Connects to existing Wi-Fi |
| **Access Point (AP)** | `POV_Hologram_AP_MODE` | `013171317` | `192.168.4.1` | Autonomous field hotspot |

- **Port:** 80 (HTTP Server)
- **Timeout:** 15 seconds (If STA connection fails, AP mode auto-starts automatically)

---

## ⏱️ RPM, LED Synchronization & Timing Calculations

| Parameter | Value / Formula | Engineering Purpose |
| :--- | :--- | :--- |
| **Good Display RPM** | $\ge 900\text{ RPM}$ | Stable, flicker-free persistence of vision |
| **Revolution Time ($T_{\text{rev}}$)** | $T_{\text{rev}} = \frac{60}{\text{RPM}}$ | Duration for one complete $360^\circ$ rotation |
| **Benchmark RPM** | $1200\text{ RPM}$ | Typical optimal operating speed |
| **$T_{\text{rev}}$ at 1200 RPM** | $T_{\text{rev}} = \frac{60}{1200} = 0.05\text{ s} = 50\text{ ms}$ | $50,000\,\mu\text{s}$ per revolution |
| **Canvas Columns** | $100\text{ columns}$ | $360^\circ$ display split into 100 angular slices |
| **Angle per Column** | $\Delta\theta = \frac{360^\circ}{100} = 3.6^\circ$ | Angular position of each column strobe |
| **LED Column Time** | $T_{\text{column}} = \frac{50\text{ ms}}{100} = 0.5\text{ ms}$ | $500\,\mu\text{s}$ timing budget per column |
| **LED Latch Time** | $60\,\mu\text{s} - 80\,\mu\text{s}$ | WS2812 latch reset pulse duration |
| **Hall Pulse Filter** | $2\text{ ms}$ | Debounce window to reject magnetic false triggers |
| **Motor Stop Detection** | $1\text{ s}$ | Detects stationary rotor and blanks LEDs |

---

## 🛠️ Complete 17 Core Features Checklist
1. Wi-Fi Web Dashboard
2. Custom text / sentence input
3. Automatic looping text scrolling
4. Adjustable scrolling speed
5. Adjustable hold time & letter gap
6. Brightness and colour control
7. Horizontal / vertical flip
8. Display angle adjustment
9. Digital & analog clock
10. Date & day display
11. Heart & smiley shapes
12. Automatic mode cycling
13. Real-time RPM monitoring
14. Battery percentage monitoring
15. Motor jitter / quality monitoring
16. EEPROM setting storage
17. OTA (Over-The-Air) firmware update

---

## 📥 Project Downloads
- **Arduino Firmware:** [`docs/downloads/POV_Hologram_v4.ino`](docs/downloads/POV_Hologram_v4.ino)
- **Technical Project Report:** [`docs/downloads/POV_Hologram_Project_Report_Aryan_Khona.docx`](docs/downloads/POV_Hologram_Project_Report_Aryan_Khona.docx)

---

## 👨‍💻 Developer & Credits
**Aryan Khona**  
Project: POV Hologram Display System (ESP8266 v4)
