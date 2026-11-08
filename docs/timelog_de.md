# Zeitprotokoll: Fraktal-Demo auf ZCU102 mit AMD Agentic AI

Datum: 06.11.2026. Uhrzeiten sind lokale Zeit auf dem Entwicklungsrechner
(i5-13420H, 8 Kerne, 24 GB RAM). Pausen und Gespräche über das Konzept sind
in den Zeitspannen enthalten.

| Uhrzeit | Schritt | Ergebnis |
|---|---|---|
| 13:19 | Vivado-Session gestartet | Vivado 2026.1.1, ZCU102-Board-Files vorhanden |
| 13:19–13:3x | Konzept besprochen, Architektur festgelegt | Fraktal per HLS, Bildspeicher im DDR, DisplayPort aus dem PS |
| 13:3x | Projekt angelegt, Pfadlängenproblem (Windows 260 Zeichen) | Projekt nach `C:\Projekte\fractal` verlegt |
| 13:37 | Vivado im Projektordner neu gestartet | Block Design: Zynq + DisplayPort, HP0, 250 MHz, GPIO für Taster/DIP/LEDs, validiert |
| 13:38 | Naiver Fraktal-Kern und Testbench geschrieben | Mandelbrot + Julia, Festkomma Q4.28, Farbpalette |
| 13:40 | C-Simulation | bitgenau gegen Golden-Modell bestanden |
| 13:41 | C-Synthese naive Version (41 s) | II=5, 1 Rechenwerk, 15 DSP, ca. 0,5 FPS bei 1080p |
| 15:24 | Start der HLS-Optimierung mit `/hls-architect` | |
| 15:27 | Architektur v1 geschrieben: 16 Rechenwerke à 8 Slots, Sammler, Burst-Ausgabe | C-Simulation bitgenau im ersten Anlauf |
| 15:31 | C-Synthese v1 (171 s) | Dataflow ok, Bursts ok; Rechenwerke noch II=7 (Scheinabhängigkeit), 282 DSP, 18 % LUT |
| 15:33 | Übergabe an `/hls-optimize`, Git-Historie für jeden Versuch | |
| 15:38 | Versuch 1: PIPELINE + DEPENDENCE (Slot-Distanz 8) | II 7 → 3 |
| 15:42 | Versuch 2: verzögerter Schleifenabbruch | II 3 → 1, Takt aber 6,6 ns |
| 15:46 | Versuch 3: Abbruch über Slot-Array mit Abhängigkeitsdistanz | **II = 1 bei 250 MHz, Timing erfüllt** |
| 15:53 | Versuch 4: 32 Rechenwerke | Latenz −50 %, 554 DSP (21 %), 39 % LUT; Modell ≈ 63 FPS bei 1080p |
| 15:55–16:00 | Zwei Werkzeugprobleme gelöst: Vitis-Pfad-Bug unter Windows (Bericht), Testbench-Puffer für Co-Simulation | IP-Export funktioniert |
| 16:01 | Co-Simulation der optimierten Version gestartet | |
| 16:13 | Baseline-Implementierung (16 Rechenwerke) fertig: 35 min | Timing bei 250 MHz erfüllt (3,44 ns) |
| 16:20 | Co-Simulation (19 min) | **PASS, RTL bitgenau**; Takte bestätigen das Leistungsmodell |
| 16:25 | Fraktal-IP ins Block Design eingebaut (Steuerung über HPM0, Bildspeicher über HP0, Interrupt) | Block Design validiert |
| 16:27 | Synthese + Implementierung + Bitstream des Gesamtsystems gestartet | läuft |
| 16:29 | Board per JTAG geprüft | Digilent-Kabel, XCZU9EG + ARM-DAP erkannt; UART-Treiber (CP2108) fehlt |
| 16:30–16:39 | Bare-Metal-Software geschrieben (parallel zur Implementierung) | DisplayPort/DPDMA mit Triple-Buffering, Kernsteuerung, CPU-Vergleich, HUD, Taster/DIP; Build- und Ladeskript |
| 16:40–16:56 | Treiber für die serielle Konsole (CP2108) eingerichtet | COM9–COM12 verfügbar |
| 17:10 | **Bitstream fertig** (Gesamtsystem 42 min auf dem Notebook) | Timing bei 250 MHz erfüllt (WNS +0,16 ns); 41k LUT (15 %), 72k FF (13 %), 557 DSP (22 %), 128 BRAM36 |
| 17:13 | XSA exportiert, Plattform gebaut (FSBL + Standalone-BSP) | |
| 17:18 | Anwendung gebaut (-O2, keine Warnungen) | fractal_app.elf bereit; Board-Test steht aus |

## Tag 2: Board-Test (07.11.2026)

| Uhrzeit | Schritt | Ergebnis |
|---|---|---|
| 10:07 | Erster Start auf dem ZCU102 per JTAG | FSBL und Anwendung laufen ohne PMU-Firmware |
| 10:10–10:25 | DisplayPort in Betrieb genommen | Neu-Trainings-Schleife behoben; PL-Reset beim JTAG-Laden fehlte (Bus hing); globales Alpha 255 statt 0 → **erstes Bild** |
| 10:25–10:35 | Fehlerbilder am Monitor analysiert | DIP-Schalter standen alle auf ON (CPU-Modus); Treiberfehler gefunden: Signalpegel auf falscher GTR-Lane → nur RBR/640x480; korrigiert → HBR, 1280x720 |
| 10:35–10:45 | Ruckeln und Bildrate | Pufferwechsel-Synchronisation, Zoom-Rundung, 4 Bildspeicher, Info-Box ohne Lesezugriffe; Messausgabe auf der Konsole |
| 10:45 | Messung 720p | 60 FPS bei tiefem Zoom, 28–50 FPS im mittleren Zoom (Kern 20–35 ms durch Innenflächen) |
| 10:47–10:55 | Kern: Innenflächen-Test (Kardioide + Kreis) | csim bitgenau, 79 % der Pixel im Testbild übersprungen; csynth II=1, 2,92 ns, 714 DSP |
| 10:58 | HBR2-Ursache: Monitor kann nur HBR, dafür 4 Lanes | PS auf DP mit 2 Lanes umgestellt (PCIe aus), GTR-Multiplexer per I2C; neuer Bitstream-Lauf gestartet |
| 11:41 | Bitstream fertig (Innenflächen-Test + DP 2 Lanes) | Timing erfüllt (WNS +0,16 ns); 45k LUT (16 %), 724 DSP (29 %) |
| 11:45–12:39 | Plattform neu gebaut; FSBL nutzte veraltete PS-Initialisierung (Vitis-Eigenheit) | Abgleich ins Build-Skript eingebaut |
| 12:40 | **1920x1080 über 2 Lanes (HBR)** | Mittlerer Zoom jetzt 8,5 ms statt 20–35 ms (bei 2,25-facher Pixelzahl); 47–60 FPS über den ganzen Flug |
| 12:45 | Board: Bild verrauscht/unscharf, blauer Streifen links | Diagnose: Innenflächen-Test verletzte zugesicherte Slot-Latenz (Cosim war übersprungen) |
| 12:50–13:09 | Korrektur (Test in Vorschleife), csim, csynth, **Cosim PASS** (17 min) | Slot-Schleife wieder Tiefe 10, RTL bitgenau |
| 13:10 | Bitstream-Lauf gestartet | |
| 14:21 | Bitstream fertig (korrigierter Kern) | Timing erfüllt (WNS +0,123 ns, WHS +0,010 ns) |
| 14:25–14:35 | XSA, Plattform, App neu; Board geladen | Link 2 Lanes HBR, 1080p, 0 Symbolfehler |
| 14:36 | Messung mit korrektem Kern (Autopilot-Flug) | Übersicht bis Zoom 50: 60 FPS (8,6–15 ms); tiefer 25–52 FPS (19–42 ms). **Die 47–60 FPS von 12:40 waren durch den Fehler geschönt** (falsche Pixel brachen früh ab) |
| bis 14:40 | Diagnose: CPU-Stichprobe der Iterationen pro Bild (UART-Zeile DIAG) | Kern läuft mit 95–98 % des theoretischen Optimums, Lastverteilung ist kein Engpass. Untergrenze 8,6 ms = 1 Pixel/Takt am Ausgang. Langsame Bilder: Mini-Mandelbrot (24–39 % der Pixel bei max_iter, bis 63 ms) und tiefer Rand (42 ms, kaum max_iter) |
| 14:40 | PC-Versuch Periodenerkennung (Brent) | Exakter Vergleich greift in Festkomma fast nie. Mit Toleranz 255 LSB: Mini-Mandelbrot-Kosten auf 39–73 %, praktisch keine abweichenden Pixel; Toleranz 4095: 30–56 %, ~0,01 % Pixel abweichend |
| 14:40–15:00 | Periodenerkennung (Toleranz 255) in Golden-Modell, Kern und CPU-Pfad; csim bitgenau, neues Testbild Mini-Mandelbrot; csynth: Slot-Schleife II=1, Tiefe 10 (unverändert), DSP 714 (unverändert) | Cosim gestartet |
| 15:00–15:21 | Experimente 48/40/36 Bit, csynth mit 2 Rechenwerken (parallel zur Cosim) | DSP je Werk: 32 Bit 22, 36 Bit 23, **40 Bit 26**, 48 Bit 40. Slot-Tiefe bis 40 Bit unverändert 10, bei 48 Bit 13. 40 Bit (Q4.36) → Zoom-Grenze ~4 Mio× bei 1080p |
| 15:31 | **Cosim Periodenerkennung PASS** (31 min, 5 Testbilder bitgenau) | Commit 34a2fd4 |
| 15:32–15:47 | Kern auf 40 Bit (Q4.36): Golden-Modell mit 128-Bit-Zwischenwerten, neues Testbild Zoom ~1e6; csim bitgenau; csynth: DSP 842, Slot-Tiefe 10 | Commit 955bc05 |
| 15:40–16:04 | PC-Versuche: Genauigkeit bei 4 Mio× ok (0–5 % chaotische Randpixel wie heute); Zoomziele geprüft: Elefantental ab 1e4, Spirale ab 1e6 einfarbig → Koordinaten automatisch verfeinert. Software auf 64-Bit-Register umgestellt, neue Registeroffsets | |
| 15:47–16:03 | 64 Rechenwerke: csim bitgenau; csynth (10 min): DSP 1674 (66 %), Slot-Tiefe 10, geschätzt 2,92 ns | Commit 308e729 |
| 16:03–20:53 | **Cosim 64 Rechenwerke × 40 Bit PASS** (4 h 49 min, 6 Testbilder bitgenau inkl. Mini-Mandelbrot und Tiefzoom; Periodenerkennung ändert 0 Pixel) | Bitstream erst nach der Cosim gestartet, um den Rechner nicht doppelt zu belasten |
| Korrektur | Die Uhrzeiten der Zeilen ab 14:40 standen zunächst rund 1 h zu spät im Protokoll; um 20:55 anhand der Dateizeitstempel (Logs, Commits) berichtigt | |
| 20:55–20:58 | IP paketiert, im Blockdesign aktualisiert (Adressen unverändert), Bitstream-Lauf gestartet | altes IP gesichert in ip_backup/ |
| 22:48 | Bitstream fertig (64 Rechenwerke × 40 Bit, Periodenerkennung), Lauf 110 min | Timing erfüllt (WNS +0,071 ns, WHS +0,010 ns); 139,6k LUT (51 %), 174k FF (32 %), 1681 DSP (67 %), 352,5 BRAM (39 %) |
| 22:50–22:57 | XSA exportiert, Plattform und App gebaut | Registeroffsets im erzeugten Treiber = main.c; Board-Test steht aus (Board aus) |
| 08.11. morgens | **Board-Test 64 Rechenwerke × 40 Bit** | Bild sauber; Zoom bis 4,0 Mio× erreicht (vorher ~15 700×). Mittlere FPS laut UART: Zoom <1e3: 54, 1e3–1e5: 51, 1e5–1e6: 41, ≥1e6: 30. Langsamste Bilder: ~9 FPS (124 ms) bei Zoom ~3 Mio× mit 1024 Iterationen. Mini-Mandelbrot nach Augenschein flüssig (gestern bis 6,7 FPS) |
| 08.11. | Tasterbelegung am Board ermittelt (SW5 Mitte 0x01, SW4 links 0x02, SW6 unten 0x04, SW7 rechts 0x08, SW8 oben 0x10; vorherige Annahme war falsch). Neue Bedienung: Steuerkreuz verschiebt, Mitte halten + oben/unten zoomt, Mitte + links/rechts Ziel, Mitte antippen Autopilot. HUD zeigt 64 Rechenwerke | Am Board getestet, Bedienung vom Nutzer bestätigt |
| 08.11. | DIP-Schalter am Board geprüft: Bitfolge kommt gespiegelt an (Schalter 1 = Bit 7) → beim Einlesen umgedreht, Belegung jetzt wie dokumentiert. Julia-Animation (DIP 1 + 7) funktioniert | Julia-Autopilot zoomt bisher nicht (zeigt ganze Menge) |
| 08.11. | Julia-Autopilot: Zoomflug auf Urbild des abstoßenden Fixpunkts (liegt immer auf der Menge; PC-Prüfung: bei c=-0,8+0,156i bis 4 Mio× 30–40 % Struktur), Animationsgeschwindigkeit ∝ 1/Zoom. CPU-Modus: FPS-Glättung korrigiert (zeigte 43 FPS bei 6,6 s/Bild) | gebaut und geladen, Board-Test steht aus |
| 08.11. | Farben am Board geprüft: Rot/Blau korrekt. CPU-Modus: HUD verschwand während des Bildaufbaus (CPU überschrieb es) → HUD zu Beginn und nach jedem Streifen im HUD-Bereich neu gezeichnet | gebaut und geladen, Board-Test steht aus |
| 08.11. | HUD- und Konsolentexte auf Englisch. BOOT.BIN (FSBL + Bitstream + App, ohne PMUFW) mit Bootgen erzeugt, auf SD-Karte kopiert | **Boot von SD-Karte läuft** (SW6 = ON/OFF/OFF/OFF), vom Nutzer bestätigt |
| 08.11. | Quellcode-Kommentare auf Englisch (Code per Vergleich ohne Kommentare unverändert), Repository `zcu102-fractal-explorer` mit Dokumentation vorbereitet (README, Entwicklungsweg, Aufwand, Zeitprotokoll englisch), Skripte auf relative Pfade umgestellt | |
| 08.11. | Kontroll-Neubau aus einer sauberen Kopie des Repositorys: `create_project.tcl` (166 min inkl. IP-Synthese), `build_sw.py` (3 min) | Timing identisch (WNS +0,071 ns, WHS +0,010 ns), 139 550 LUT, 1681 DSP; daraus erzeugte BOOT.BIN **läuft am Board von SD-Karte**, vom Nutzer bestätigt |
