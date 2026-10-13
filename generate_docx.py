import docx
from docx import Document
from docx.shared import Inches, Pt, RGBColor
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.enum.table import WD_TABLE_ALIGNMENT
from docx.oxml import parse_xml
from docx.oxml.ns import nsdecls
import os

doc = Document()

# Set standard margins
for s in doc.sections:
    s.top_margin = Inches(0.8)
    s.bottom_margin = Inches(0.8)
    s.left_margin = Inches(0.8)
    s.right_margin = Inches(0.8)

def set_cell_background(cell, fill_hex):
    tcPr = cell._tc.get_or_add_tcPr()
    shd = parse_xml(f'<w:shd {nsdecls("w")} w:fill="{fill_hex}"/>')
    tcPr.append(shd)

def create_table(headers, rows_data):
    table = doc.add_table(rows=1, cols=len(headers))
    table.alignment = WD_TABLE_ALIGNMENT.CENTER
    hdr_cells = table.rows[0].cells
    for i, h in enumerate(headers):
        hdr_cells[i].text = h
        set_cell_background(hdr_cells[i], "0A8491")
        for r in hdr_cells[i].paragraphs[0].runs:
            r.font.bold = True
            r.font.color.rgb = RGBColor(0xFF, 0xFF, 0xFF)
            r.font.size = Pt(10)
    for row_idx, row in enumerate(rows_data):
        r_cells = table.add_row().cells
        bg = "F5F8FA" if row_idx % 2 == 0 else "FFFFFF"
        for i, val in enumerate(row):
            r_cells[i].text = str(val)
            set_cell_background(r_cells[i], bg)
            for r in r_cells[i].paragraphs[0].runs:
                r.font.size = Pt(9.5)
    doc.add_paragraph()

# Document Title
p_title = doc.add_paragraph()
p_title.alignment = WD_ALIGN_PARAGRAPH.CENTER
run_title = p_title.add_run("POV 3D HOLOGRAM DISPLAY SYSTEM (v4)")
run_title.font.name = 'Segoe UI'
run_title.font.size = Pt(24)
run_title.font.bold = True
run_title.font.color.rgb = RGBColor(0x0a, 0x84, 0x91)

p_sub = doc.add_paragraph()
p_sub.alignment = WD_ALIGN_PARAGRAPH.CENTER
run_sub = p_sub.add_run("Complete Engineering Documentation, Circuit Schematics, Timing Physics & Poster Specifications\nDesigned & Developed by: Aryan Khona")
run_sub.font.name = 'Segoe UI'
run_sub.font.size = Pt(12)
run_sub.font.italic = True
run_sub.font.color.rgb = RGBColor(0x33, 0x33, 0x33)

doc.add_paragraph()

# 1. Hardware & Pin Configuration
doc.add_heading("1. Hardware & Pin Configuration", level=1)
hw_headers = ["Component", "Pin on ESP8266", "Details & Function"]
hw_data = [
    ("WS2812 Strip-1", "D1 (GPIO 5)", "8 LEDs (Blade A, high-speed 800kHz bitbang data line)"),
    ("WS2812 Strip-2", "D2 (GPIO 4)", "8 LEDs (Blade B, opposite rotor blade 180° offset)"),
    ("Hall Effect Sensor", "D7 (GPIO 13)", "1 pulse / revolution (A3144 interrupt zero-index trigger)"),
    ("Battery Monitoring", "A0 (ADC0)", "10kΩ/10kΩ 1:1 voltage divider, calibrated for 3.6V to 4.2V"),
    ("High-Speed Motor", "Separate Supply", "Dedicated 3.5V supply for stable rotor rotation (900-1300 RPM)")
]
create_table(hw_headers, hw_data)

# 2. Circuit Diagram Description & Architecture
doc.add_heading("2. Circuit Architecture & Power Distribution", level=1)
doc.add_paragraph(
    "The electrical circuit consists of two isolated, independently powered subsystems:\n"
    "1. Stator Motor Drive: A dedicated 4.2V Li-ion battery feeds a TP4056 USB charging module, controlled by an ON/OFF switch. A 1N4007 protection diode steps down the output to 3.5V for the DC motor. A neodymium magnet is fixed to the stator frame.\n"
    "2. Rotor Display Subsystem: A separate on-rotor 4.2V Li-ion battery and TP4056 module supply power through an ON/OFF switch and 1N4007 diode down to 3.5V (+Vcc), powering both the NodeMCU ESP8266 and the two WS2812 LED strips. The A3144 Hall sensor connects to +Vcc, GND, and D7. Pin A0 taps the midpoint of a 10kΩ/10kΩ resistor divider across +Vcc and GND."
)

# 3. Wi-Fi Configuration
doc.add_heading("3. Wi-Fi & Network Architecture", level=1)
wifi_headers = ["Mode", "SSID", "Password", "Default IP Address", "Operational Role"]
wifi_data = [
    ("Station Mode (STA)", "POV_Hologram_STA_MODE", "013171317", "10.218.184.220", "Connects to existing home/lab Wi-Fi network"),
    ("Access Point (AP)", "POV_Hologram_AP_MODE", "013171317", "192.168.4.1", "Autonomous field hotspot if STA fails")
]
create_table(wifi_headers, wifi_data)
doc.add_paragraph(
    "• Web Server Port: 80\n"
    "• STA Connection Timeout: 15 seconds\n"
    "• Automatic Failover: If the STA router connection cannot be established within 15 seconds, Access Point (AP) mode launches automatically."
)

# 4. Display Modes
doc.add_heading("4. Operational Display Modes", level=1)
mode_headers = ["Mode ID", "Mode Name", "Primary Function & Capabilities"]
mode_data = [
    ("0", "Text", "Custom static & scrolling text input (dynamic letter limit, 5x5 font, anti-ghosting blanking)"),
    ("1", "Canvas", "User drawing / custom graphics (100 columns, flat & polar modes, EEPROM storage)"),
    ("2", "Digital Clock", "12/24 hour digital time readout with live phone epoch synchronization"),
    ("3", "Analog Clock", "Virtual clock face with hours, minutes, second sweep hand, and dial tick markers"),
    ("4", "Date & Day", "Smooth automated alternation between 3-letter weekday abbreviation and date/month"),
    ("5", "Shapes", "Parametric vector icons: Heart (scalable width/height) and smiley face with color pickers"),
    ("6", "Auto Mode", "Autonomous carousel loop cycling through all enabled modes at 2 to 60 second intervals"),
    ("7", "Sentence (Ticker)", "New in v4: Long sentences (up to 100 chars), angular aperture window (0°-270°), dynamic rainbow cycling")
]
create_table(mode_headers, mode_data)

# 5. RPM & Timing Parameters
doc.add_heading("5. RPM, LED Synchronization & Timing Parameters", level=1)
timing_headers = ["Parameter", "Value / Mathematical Calculation", "Technical Purpose"]
timing_data = [
    ("Good Display RPM", ">= 900 RPM", "Provides stable, flicker-free persistence of vision"),
    ("Revolution Time (T_rev)", "T_rev = 60 / RPM", "Total duration for one complete 360° rotor turn"),
    ("Example Benchmark RPM", "1200 RPM", "Ideal rotational benchmark speed"),
    ("T_rev at 1200 RPM", "T_rev = 60 / 1200 = 0.05 s = 50 ms", "50,000 microseconds per rotation"),
    ("Canvas Columns", "100 columns", "Cylindrical space partitioned into 100 angular slices"),
    ("Angle per Column", "Angle = 360° / 100 = 3.6°", "Angular width of each column strobe"),
    ("LED Column Time", "T_column = 50 ms / 100 = 0.5 ms (500 µs)", "Approximate time budget per column at 1200 RPM"),
    ("LED Latch Time", "60 µs to 80 µs", "Reset signal duration to latch WS2812 pixel data"),
    ("Hall Pulse Filter", "2 ms", "Debounce window to reject magnetic jitter and noise"),
    ("Motor Stop Detection", "1 second", "Safely blanks LEDs when rotor is stationary"),
    ("CPU Core Frequency", "160 MHz", "Eliminates timing jitter during Wi-Fi and FastLED operations")
]
create_table(timing_headers, timing_data)

# 6. Technology Stack
doc.add_heading("6. Language & Technology Stack", level=1)
tech_headers = ["Technology / Tool", "Application in Project"]
tech_data = [
    ("C++ (Arduino C++)", "Core ESP8266 firmware, fast dual-blade bitbang driver, predictive timing filter"),
    ("HTML / CSS", "Responsive web dashboard structure and modern dark UI theme"),
    ("JavaScript", "Interactive dashboard controls, real-time telemetry, and canvas rendering engine"),
    ("Arduino IDE", "Compilation environment, ESP8266 core, and flash toolchain"),
    ("ESP8266 Web Server", "Lightweight HTTP server running on port 80"),
    ("NTP", "Network Time Protocol for automatic atomic clock synchronization"),
    ("EEPROM Emulation", "Persistent non-volatile storage for custom text, modes, and drawings"),
    ("OTA (Over-The-Air)", "Wireless firmware uploading directly over Wi-Fi without USB cables")
]
create_table(tech_headers, tech_data)

# 7. Summary of 17 Core Features
doc.add_heading("7. 17 Core Engineering Features", level=1)
features = [
    "1. Wi-Fi web dashboard", "2. Custom text/sentence input", "3. Automatic looping text scrolling",
    "4. Adjustable scrolling speed", "5. Adjustable hold time & letter gap", "6. Brightness and colour control",
    "7. Horizontal / vertical flip", "8. Display angle adjustment", "9. Digital & analog clock",
    "10. Date & day display", "11. Heart & smiley shapes", "12. Automatic mode cycling",
    "13. Real-time RPM monitoring", "14. Battery percentage monitoring", "15. Motor jitter / quality monitoring",
    "16. EEPROM setting storage", "17. OTA (Over-The-Air) firmware update"
]
for f in features:
    doc.add_paragraph(f"• {f}")

doc.add_paragraph()
p_foot = doc.add_paragraph("Report generated for GitHub Pages & Project Showcase by Aryan Khona.")
p_foot.alignment = WD_ALIGN_PARAGRAPH.RIGHT

doc_path = r"docs\downloads\POV_Hologram_Project_Report_Aryan_Khona.docx"
doc.save(doc_path)
print("Updated DOCX saved successfully at:", doc_path)
