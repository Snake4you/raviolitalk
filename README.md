# Canfunk

Canfunk ist ein verschluesseltes Walkie-Talkie aus Konservendosen. Zwei (oder mehr) Dosen sprechen direkt per ESP-NOW miteinander, ganz ohne Router. Ein Smartphone-Hotspot wird nur beim Start fuer Firmware-Updates (OTA) gebraucht.

Die Firmware laeuft auf zwei Hardware-Varianten, die miteinander kompatibel sind:

| | **Variante A: ESP32-S3-Dose** | **Variante B: Toy-AI-Core-C3-MINI** |
|---|---|---|
| PlatformIO-Environment | `retro-phone` (Standard) | `toy-c3` |
| Mikrocontroller | ESP32-S3 (bread-compact-wifi-Board) | ESP32-C3 (SpotPear-Fertigboard) |
| Mikrofon / Lautsprecher | INMP441 + MAX98357A, einzeln verdrahtet | ES8311-Codec + NS4150, auf dem Board |
| Neigungssensor (TILT, Klingeln) | ADXL345, Standard | optional, standardmaessig aus |
| Tasten | BOOT, PTT, VOL+, VOL- | nur BOOT (Mehrfachklick) |
| Status | im Einsatz getestet | gebaut, noch nicht auf dem Board getestet |

---

## Inhalt

1. [Funktionen](#funktionen)
2. [Software bauen und flashen](#software-bauen-und-flashen)
3. [Bauanleitung Variante A: ESP32-S3-Dose](#bauanleitung-variante-a-esp32-s3-dose)
4. [Bauanleitung Variante B: Toy-AI-Core-C3-MINI](#bauanleitung-variante-b-toy-ai-core-c3-mini)
5. [Konfiguration (platformio.ini)](#konfiguration-platformioini)
6. [Inbetriebnahme: Kalibrieren und Pairing](#inbetriebnahme-kalibrieren-und-pairing)
7. [Bedienung](#bedienung)
8. [OTA-Update ueber Hotspot](#ota-update-ueber-hotspot)
9. [Serielle Konsole](#serielle-konsole)
10. [Technik und Fehlersuche](#technik-und-fehlersuche)

---

## Funktionen

- **Sprechfunk** per ESP-NOW, G.711 u-law, 16 kHz, Mono. Nach dem Pairing mit AES-128 verschluesselt.
- **Drei Sende-Ausloeser:**
  - **Taste (PTT)**: sendet immer, solange sie gehalten wird.
  - **TILT**: Dose zum Mund kippen (35–70 Grad) startet das Senden, ans Ohr halten oder aufrecht stellen beendet es.
  - **VOX**: Sprachaktivierung, sendet sobald man spricht (1 s Nachlauf).
- **Klingeln**: Eine Dose kurbeln (ca. 1 s im Kreis schwenken) laesst alle anderen Dosen klingeln, die **auf dem Kopf** stehen.
- **OTA-Update** per Weboberflaeche ueber einen Smartphone-Hotspot.
- Alle Einstellungen (Lautstaerke, Modus, Empfindlichkeit, Kalibrierung, Schluessel) bleiben im Flash (NVS) gespeichert.

---

## Software bauen und flashen

Benoetigt wird [PlatformIO](https://platformio.org/) (CLI oder VS-Code-Erweiterung). Das Framework (ESP-IDF 5.2.1) laedt PlatformIO beim ersten Build selbst herunter.

```bash
git clone https://github.com/Snake4you/raviolitalk.git
```

```bash
cd raviolitalk
```

```bash
cp platformio_local.ini.example platformio_local.ini
```

In `platformio_local.ini` eigene Zugangsdaten fuer den OTA-Hotspot eintragen (siehe [Eigene Zugangsdaten](#eigene-zugangsdaten-platformio_localini)). Dann bauen und flashen:

```bash
pio run -e retro-phone
```

```bash
pio run -e retro-phone -t upload
```

```bash
pio device monitor -b 115200
```

Fuer Variante B ueberall `-e toy-c3` statt `-e retro-phone` verwenden. Die Firmwaredateien fuer OTA liegen danach unter `.pio/build/<environment>/firmware.bin`.

Mehrere Geraete angeschlossen? Port mit `pio device list` suchen und mit `--upload-port` bzw. `-p` angeben (Linux z. B. `/dev/ttyUSB0`, Windows z. B. `COM8`).

Die Unit-Tests fuer Tilt-, Kurbel- und Lautstaerke-Logik sind eigenstaendige Programme und laufen mit jedem C++-Compiler auf dem PC (kein Fehler = bestanden):

```bash
for t in test/test_*.cpp; do g++ -std=c++17 -Iinclude "$t" -o /tmp/canfunk_test && /tmp/canfunk_test && echo "OK $t"; done
```

---

## Bauanleitung Variante A: ESP32-S3-Dose

### Teileliste (pro Dose)

| Teil | Anzahl | Hinweis |
|---|---:|---|
| ESP32-S3-Board „bread-compact-wifi“ (oder ESP32-S3-DevKitC mit 16 MB Flash) | 1 | Board mit BOOT-Taste (GPIO 0) und WiFi/Reset-Taste (GPIO 1) |
| INMP441 I2S-MEMS-Mikrofon | 1 | |
| MAX98357A I2S-Verstaerker | 1 | |
| Lautsprecher 4–8 Ohm, ca. 3 W, 40–50 mm | 1 | passend zum Dosendurchmesser |
| ADXL345 Beschleunigungssensor (I2C-Breakout) | 1 | fuer TILT und Klingeln |
| Taster (Schliesser) | 2 | VOL+ und VOL- (plus PTT, falls das Board keine Taste an GPIO 1 hat) |
| Konservendose (sauber, entgratet) | 1 | |
| Stromversorgung | 1 | USB-Powerbank oder LiPo mit Lade-/Boost-Modul auf 5 V |
| Litze, Heisskleber, ggf. Lochrasterplatine | | |

### Verdrahtung

Alle Module laufen an **3,3 V**, der MAX98357A darf auch an 5 V (lauter). GND aller Module verbinden.

**INMP441 (Mikrofon)**

| INMP441 | ESP32-S3 |
|---|---|
| VDD | 3V3 |
| GND | GND |
| L/R | GND (linker Kanal, wird von der Firmware gelesen) |
| WS | GPIO 4 |
| SCK | GPIO 5 |
| SD | GPIO 6 |

**MAX98357A (Lautsprecher-Verstaerker)**

| MAX98357A | ESP32-S3 |
|---|---|
| VIN | 5V (oder 3V3) |
| GND | GND |
| BCLK | GPIO 15 |
| LRC | GPIO 16 |
| DIN | GPIO 7 |
| SD | offen lassen (Verstaerker an) |
| GAIN | offen lassen (9 dB) |
| OUT+ / OUT- | Lautsprecher |

**ADXL345 (Neigungssensor, I2C)**

| ADXL345 | ESP32-S3 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| CS | **3V3** (sonst ist der Sensor im SPI-Modus und antwortet nicht) |
| SDO | GND → Adresse `0x53` (an 3V3 → `0x1D`) |
| SDA | GPIO 41 |
| SCL | GPIO 42 |

**Taster** – jeweils zwischen GPIO und GND, die Pull-ups sind intern aktiv:

| Taster | GPIO |
|---|---:|
| BOOT (auf dem Board) | 0 |
| PTT (WiFi/Reset-Taste auf dem Board) | 1 |
| VOL- | 39 |
| VOL+ | 40 |

### Mechanischer Aufbau

1. **Lautsprecher** mittig in den Dosenboden setzen (Loecher bohren oder Lochmuster stanzen), mit Heisskleber abdichten. Die Dose wirkt als Resonanzkoerper.
2. **Mikrofon** nahe am Dosenrand bzw. der Oeffnung montieren, mit der Schalloeffnung nach aussen. Moeglichst weit weg vom Lautsprecher, damit es im VOX-Modus nicht rueckkoppelt.
3. **ADXL345 fest verkleben**, nicht lose haengen lassen. Die Ausrichtung ist egal (die Firmware kalibriert die Dosenachse), aber der Sensor darf sich danach nicht mehr bewegen. Nach jedem Umbau neu kalibrieren.
4. **Taster** so in die Dosenwand setzen, dass PTT gut mit dem Daumen erreichbar ist.
5. Board und Akku innen verkleben bzw. mit Schaumstoff fixieren. Darauf achten, dass keine Leiterbahn das Blech beruehrt (Isolierband oder Kapton innen auskleben).
6. Die Antenne des ESP32 nicht komplett in Metall einschliessen: Bei geschlossener Dose ist die Reichweite deutlich geringer. Am besten die Antenne an der offenen Seite bzw. an einem Kunststoffdeckel platzieren.

---

## Bauanleitung Variante B: Toy-AI-Core-C3-MINI

Das SpotPear Toy-AI-Core-C3-MINI bringt Mikrofon-Codec (ES8311), Verstaerker (NS4150) und eine Taste bereits mit. Es muss nur ein Lautsprecher und eine Stromversorgung angeschlossen werden.

### Teileliste (pro Dose)

| Teil | Anzahl | Hinweis |
|---|---:|---|
| SpotPear Toy-AI-Core-C3-MINI | 1 | |
| Lautsprecher 4–8 Ohm, 2–3 W | 1 | am Lautsprecher-Stecker des Boards |
| Akku / Powerbank | 1 | USB-C oder Akku-Anschluss des Boards |
| optional: ADXL345 | 1 | fuer TILT und Klingeln |
| Konservendose | 1 | |

### Belegung (fest auf dem Board)

| Funktion | GPIO |
|---|---:|
| BOOT-Taste (auch an den Steckern BOOT und AWK) | 9 |
| ES8311 MCLK, SCLK, LRCK | 10, 8, 6 |
| ES8311 DSDIN (Lautsprecher), ASDOUT (Mikrofon) | 5, 7 |
| NS4150 Verstaerker-Enable | 11 |
| I2C SDA, SCL (ES8311, Adresse `0x18`) | 3, 4 |

Logausgabe, serielle Konsole und Flashen laufen ueber den USB-C-Anschluss (USB Serial/JTAG, kein CH340).

### Optional: ADXL345 nachruesten

Ohne Sensor wird nur per Taste oder VOX gesprochen, und die Dose kann weder kurbeln noch klingeln. Mit Sensor:

| ADXL345 | Toy-AI-Core-C3 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| CS | 3V3 |
| SDO | GND (`0x53`) oder 3V3 (`0x1D`) |
| SDA | Display-Stecker (FPC) Pin 18 |
| SCL | Display-Stecker (FPC) Pin 17 |

Der Sensor teilt sich den I2C-Bus mit dem ES8311 (Adresse `0x18`), die Adressen kollidieren nicht. Danach in `platformio.ini` im Abschnitt `[env:toy-c3]` die Zeile `-DTILT_ENABLE=1` einkommentieren (siehe [Konfiguration](#konfiguration-platformioini)).

### Mechanischer Aufbau

Wie bei Variante A: Lautsprecher in den Dosenboden, Board isoliert befestigen, Mikrofon des Boards (auf der Platine) Richtung Dosenoeffnung ausrichten, die BOOT-Taste von aussen erreichbar machen (z. B. ueber einen externen Taster am BOOT-Stecker).

---

## Konfiguration (platformio.ini)

Alle Einstellungen stehen **auskommentiert** in [platformio.ini](platformio.ini). So wird eine Option gesetzt:

1. Zeile suchen und das fuehrende `;` entfernen. Die Einrueckung (vier Leerzeichen) muss bleiben.
2. Wert anpassen.
3. Neu bauen und flashen (USB oder OTA).

Nicht gesetzte Optionen behalten den Standardwert aus [src/config.h](src/config.h). Ungueltige Werte (z. B. Lautstaerke 12) brechen den Build mit einer verstaendlichen Fehlermeldung ab.

Die Datei hat drei Abschnitte:

- `[settings]` – gilt fuer **beide** Boards (Funk, OTA, Startwerte, Klingeln)
- `[env:retro-phone]` – nur Variante A (ESP32-S3)
- `[env:toy-c3]` – nur Variante B (C3)

Beispiel: Funkkanal 6 und Startlautstaerke 3 fuer alle Dosen:

```ini
[settings]
build_flags =
    -DESPNOW_CHANNEL=6
    -DDEFAULT_VOLUME=3
```

Text-Werte brauchen die Schreibweise `'-DNAME="Wert"'` mit beiden Anfuehrungszeichen-Arten.

### Eigene Zugangsdaten (platformio_local.ini)

Hotspot-Name und Passwoerter gehoeren nicht ins Git. Die Firmware bringt nur oeffentliche Platzhalter mit (`Canfunk-OTA` / `canfunk-ota` / `canfunk`), und der Build warnt, solange das OTA-Passwort der Platzhalter ist. **Vor dem ersten Flashen eigene Werte setzen:**

```bash
cp platformio_local.ini.example platformio_local.ini
```

Dann in `platformio_local.ini` eintragen:

```ini
[settings]
build_flags =
    '-DOTA_WIFI_SSID="MeinHotspot"'
    '-DOTA_WIFI_PASSWORD="mein-wlan-passwort"'
    '-DOTA_HTTP_PASSWORD="mein-ota-passwort"'
```

`platformio_local.ini` steht in `.gitignore` und wird automatisch mitgelesen. Ihr `[settings]`-Abschnitt **ersetzt** den aus `platformio.ini` komplett; weitere Optionen fuer beide Boards (Kanal, Startwerte, Klingeln) deshalb ebenfalls dort eintragen.

### Alle Optionen

**Funk / OTA** (`[settings]`, auf allen Dosen gleich einstellen!)

| Option | Standard | Bedeutung |
|---|---|---|
| `ESPNOW_CHANNEL` | `13` | WLAN-Kanal 1..13 fuer den Sprechfunk |
| `OTA_WIFI_SSID` | `Canfunk-OTA` (Platzhalter) | Name des Hotspots fuer OTA-Updates |
| `OTA_WIFI_PASSWORD` | `canfunk-ota` (Platzhalter) | WLAN-Passwort des Hotspots, mindestens 8 Zeichen |
| `OTA_HTTP_PASSWORD` | `canfunk` (Platzhalter) | Passwort der OTA-Webseite |

**Startwerte** (`[settings]`, gelten nur fuer Geraete ohne gespeicherte Einstellung, also z. B. nach dem ersten Flashen oder nach Loeschen des Flash)

| Option | Standard | Bedeutung |
|---|---|---|
| `DEFAULT_VOLUME` | `5` | Lautstaerke 1..10 |
| `DEFAULT_VOX_SENS` | `6` | VOX-Empfindlichkeit 1..10 (10 = reagiert auf leise Sprache) |
| `DEFAULT_PTT_MODE_VOX` | `0` | `1` = Start im VOX-Modus, `0` = TILT/Taste |

**Klingeln** (`[settings]`)

| Option | Standard | Bedeutung |
|---|---|---|
| `RING_TONE_LEVEL` | `3.0f` | Lautstaerke des Klingeltons relativ zu den anderen Signaltoenen |
| `RING_SEND_COOLDOWN_MS` | `10000` | Sender: so lange nach dem Kurbeln kein neues Klingeln |
| `RING_RX_COOLDOWN_MS` | `10000` | Empfaenger: so lange nach einem Klingeln kein neues |

**Neigungssensor** (`[env:retro-phone]` bzw. `[env:toy-c3]`)

| Option | Standard | Bedeutung |
|---|---|---|
| `TILT_ENABLE` | S3: `1`, C3: `0` | `1` = ADXL345 vorhanden. `0` = kein Sensor, nur Taste/VOX, kein Klingeln |
| `ADXL_I2C_ADDR` | nicht gesetzt | Feste Sensoradresse (`0x53` oder `0x1D`). Ohne Angabe werden beide probiert |
| `PIN_I2C_SDA`, `PIN_I2C_SCL` | `GPIO_NUM_41`, `GPIO_NUM_42` | I2C-Pins des Sensors (nur S3, beim C3 fest) |

**Audio** (`[env:toy-c3]`)

| Option | Standard | Bedeutung |
|---|---|---|
| `ES8311_MIC_GAIN` | `4` | Mikrofonverstaerkung 0..7 in 6-dB-Schritten (4 = 24 dB) |

**Pins** – jeder Pin aus [src/config.h](src/config.h) laesst sich ebenso ueberschreiben, z. B. fuer einen anders verdrahteten Aufbau:

```ini
[env:retro-phone]
build_flags =
    ${settings.build_flags}
    -DPIN_VOL_UP=GPIO_NUM_38
    -DPIN_VOL_DOWN=GPIO_NUM_37
```

> Startwerte wirken nur, solange nichts gespeichert ist. Um eine bereits benutzte Dose auf die neuen Startwerte zurueckzusetzen, den Flash loeschen (`pio run -e retro-phone -t erase`, danach neu flashen). Dabei gehen Pairing-Schluessel und Kalibrierung verloren. Einzelwerte lassen sich einfacher ueber die Tasten oder die [serielle Konsole](#serielle-konsole) aendern.

---

## Inbetriebnahme: Kalibrieren und Pairing

Nach dem ersten Flashen jede Dose einmal **kalibrieren** (nur mit Sensor) und anschliessend alle Dosen **pairen**.

### 1. Dosenachse kalibrieren (TILT)

Der Sensor ist in jeder Dose anders eingebaut. Die Firmware speichert deshalb die Richtung „Dose steht aufrecht“. Ohne Kalibrierung bleibt TILT im Empfang und die Dose kann nicht klingeln.

1. Dose aufrecht auf den Boden stellen, Oeffnung nach oben.
2. Ausloesen:
   - Variante A: BOOT und PTT gemeinsam **2 Sekunden** halten.
   - Variante B: BOOT **6x klicken**.
   - oder Konsolenbefehl `cal`.
3. Die Dose eine weitere Sekunde ruhig stehen lassen.
4. Ein aufsteigendes Dreifach-Piepen bestaetigt die gespeicherte Achse.

Wiederholen nach jedem Sensorumbau oder wenn der Flash geloescht wurde.

### 2. Pairing

Gepairte Dosen teilen einen AES-Schluessel und hoeren nur noch aufeinander. Ungepairte Dosen senden unverschluesselt an alle.

1. Hotspot aus, beide Dosen einschalten (sie muessen im ESP-NOW-Betrieb sein, siehe [OTA](#ota-update-ueber-hotspot)).
2. Auf beiden Dosen innerhalb von 15 Sekunden Pairing starten:
   - Variante A: VOL+ und VOL- gemeinsam **3 Sekunden** halten (aufsteigende Toene).
   - Variante B: BOOT **5x klicken**.
   - oder Konsolenbefehl `pair`.
3. Ein Bestaetigungston zeigt den Erfolg, der Schluessel ist gespeichert.

**Pairing loeschen:** Variante A VOL+ und VOL- **10 Sekunden** halten (5 schnelle Pieptoene), Variante B nur per Konsolenbefehl `reset`.

---

## Bedienung

### Variante A (ESP32-S3)

| Aktion | Wirkung |
|---|---|
| PTT halten | sprechen (in jedem Modus) |
| Dose zum Mund kippen | sprechen (Modus TILT) |
| einfach sprechen | sprechen (Modus VOX) |
| VOL+ / VOL- | Lautstaerke 1..10, wird gespeichert |
| BOOT halten + VOL+ / VOL- | VOX-Empfindlichkeit aendern (im VOX-Modus) |
| BOOT 5 s halten | TILT/VOX umschalten (aufsteigende Toene = VOX, absteigende = TILT) |
| BOOT + PTT 2 s halten | Dosenachse kalibrieren |
| VOL+ und VOL- 3 s halten | Pairing starten |
| VOL+ und VOL- 10 s halten | Pairing-Schluessel loeschen |
| Dose ca. 1 s kurbeln | andere Dosen klingeln lassen |

### Variante B (Toy-AI-Core-C3, eine Taste)

| BOOT-Taste | Wirkung |
|---|---|
| halten (ab 0,25 s) | sprechen |
| 2x klicken | lauter |
| 3x klicken | leiser |
| 4x klicken | TILT/VOX umschalten |
| 5x klicken | Pairing starten (15 s) |
| 6x klicken | Dosenachse kalibrieren |

### TILT-Zonen

Gemessen wird der Winkel der Dosenachse gegen die kalibrierte Senkrechte. Die Drehung um die eigene Achse ist egal.

| Winkel | Zustand |
|---|---|
| unter 25 Grad | Empfang (Dose steht bzw. ist kaum gekippt) |
| 35–70 Grad | **Senden** (Sprechhaltung) |
| ueber 75 Grad | Empfang (Ohrhaltung) |
| ueber 135 Grad, 0,3 s ruhig | „auf dem Kopf“: Dose klingelt bei Anruf |

Ein Wechsel muss 100 ms stabil sein. Bei starker Bewegung wird der Zustand eingefroren, damit Handbewegungen nicht flattern.

### Klingeln

1. Die angerufene Dose steht **auf dem Kopf** (Oeffnung nach unten) – das ist die „aufgelegte“ Position.
2. An der eigenen Dose ca. 1 Sekunde kurbeln (im Kreis schwenken wie die Kurbel eines alten Telefons). Kurzes Anheben oder Abstellen reicht nicht.
3. Alle gepairten Dosen auf dem Kopf klingeln. Danach ist das Klingeln 10 Sekunden gesperrt (einstellbar).

Klingeln geht nur mit Sensor und Kalibrierung. Per Konsole kann auch mit `ring` geklingelt werden.

### Signaltoene

| Ton | Bedeutung |
|---|---|
| Dreiklang aufsteigend (A-Cis-E) beim Start | bereit, ESP-NOW-Betrieb |
| Doppelton 800/1200 Hz beim Start | mit OTA-Hotspot verbunden, **kein** Sprechfunk |
| Dreifach-Piepen aufsteigend | Kalibrierung gespeichert / Pairing gestartet |
| Doppelton 1500/1800 Hz | Pairing erfolgreich |
| 5 schnelle Pieptoene | Pairing-Schluessel geloescht |
| Ton steigt/faellt mit Stufe | Lautstaerke bzw. Empfindlichkeit geaendert |

---

## OTA-Update ueber Hotspot

Beim Start sucht jede Dose **5 Sekunden** nach dem Hotspot `OTA_WIFI_SSID`:

| Zustand beim Start | Ergebnis |
|---|---|
| Hotspot gefunden | Dose verbindet sich und startet die OTA-Webseite. Sprechfunk geht dabei **nicht**, weil der Hotspot auf einem anderen Kanal laeuft. |
| Hotspot nicht da | Dose wechselt auf ESP-NOW (Kanal `ESPNOW_CHANNEL`): normaler Sprechbetrieb. |

Ablauf:

1. Firmware bauen (`pio run -e retro-phone` bzw. `-e toy-c3`).
2. Smartphone-Hotspot mit SSID `OTA_WIFI_SSID` und Passwort `OTA_WIFI_PASSWORD` einschalten (die Werte aus `platformio_local.ini`), am besten auf 2,4 GHz.
3. Dose neu starten und auf den Doppelton warten.
4. Im Browser `http://dose-<mac>.local` oeffnen, `<mac>` ist die WLAN-MAC in Kleinbuchstaben ohne Doppelpunkte (z. B. `dose-a1b2c3d4e5f6.local`). Sie steht im seriellen Log beim Start. Geht mDNS nicht, die IP in der Hotspot-Geraeteliste nachsehen.
5. Anmelden: Benutzername beliebig, Passwort `OTA_HTTP_PASSWORD`.
6. `.pio/build/<environment>/firmware.bin` hochladen. Die Dose startet danach neu.
7. **Hotspot ausschalten** und alle Dosen neu starten, bevor der Sprechfunk getestet wird.

Fuer Servicezwecke kann die Lautstaerke authentifiziert ueber `POST /volume` mit einem Wert `1` bis `10` gesetzt werden.

Tipp: Die MAC jeder Dose einmal notieren (z. B. Aufkleber auf der Dose), dann ist der mDNS-Name immer bekannt.

---

## Serielle Konsole

Ueber `pio device monitor -b 115200` (Variante A: CH340-USB-UART, Variante B: USB-C direkt). `help` zeigt alle Befehle.

| Befehl | Wirkung |
|---|---|
| `status` | Board, Modus, Lautstaerke, Empfindlichkeit, Dosenachse, Neigung, Lage, Pairing |
| `cal` | Dosenachse kalibrieren (Dose aufrecht hinstellen) |
| `mode tilt` / `mode vox` | Modus setzen |
| `sens <1-10>` | Empfindlichkeit des aktuellen Modus setzen |
| `vol <1-10>` | Lautstaerke setzen, z. B. `vol 3` |
| `pair` | Pairing starten (15 s) |
| `reset` | Pairing-Schluessel loeschen |
| `ring` | andere Dosen klingeln lassen |
| `reboot` | neu starten |

---

## Technik und Fehlersuche

### Architektur

- ESP-IDF 5.2.1 ueber PlatformIO (`espressif32@6.7.0`), 16 MB Flash, eigene Partitionstabelle mit zwei OTA-Slots
- ESP-NOW mit AES-128 und gespeichertem 16-Byte-LMK
- G.711 u-law, 16 kHz, Mono, 240 Samples (15 ms) pro Paket, 30 ms Jitterpuffer
- [src/main.cpp](src/main.cpp): Funk, OTA, Pairing, TILT, Klingeln, Tasten, Konsole
- [src/audio.cpp](src/audio.cpp): I2S-Mikrofon und -Lautsprecher bzw. ES8311-Codec
- [src/config.h](src/config.h): Pins und Standardwerte beider Boards
- [include/](include/): Tilt-Trigger, Kurbel-/Kopflage-Erkennung, Lautstaerkebefehl (hardwareunabhaengig, mit Tests in [test/](test/))

### Haeufige Probleme

| Problem | Ursache / Loesung |
|---|---|
| Dosen hoeren sich nicht | Hotspot noch an (Doppelton beim Start)? Gleicher `ESPNOW_CHANNEL` auf allen Dosen? Eine Dose gepairt, die andere nicht? Beide neu pairen oder beide `reset`. |
| TILT reagiert nicht | Nicht kalibriert (`status` zeigt die Dosenachse). Sensor nicht gefunden: CS an 3V3? SDA/SCL vertauscht? Im Log steht, an welcher Adresse gesucht wurde. |
| Dose klingelt nicht | Steht sie wirklich auf dem Kopf (`status` → „Lage“)? Cooldown von 10 s abgelaufen? Ohne Sensor ist Klingeln nicht moeglich. |
| Rauschen, Knattern, „Donnern“ beim Empfaenger | `MIC_BIT_SHIFT` wurde geaendert. Er muss `16` sein (siehe unten). |
| VOX sendet staendig / nie | Empfindlichkeit mit BOOT + VOL+/VOL- oder `sens` anpassen. Mikrofon weiter weg vom Lautsprecher. |
| OTA-Seite nicht erreichbar | mDNS wird von manchen Hotspots nicht weitergereicht: IP in der Hotspot-Liste nachsehen. |

### Mikrofonformat INMP441

Das INMP441 liefert signed 24-Bit-Audio, linksbuendig in einem 32-Bit-I2S-Wort. Fuer 16-Bit-PCM nutzt die Firmware die oberen 16 Bits: `MIC_BIT_SHIFT = 16` in [src/config.h](src/config.h).

Ein Shift von `14` verstaerkt das Mikrofon um Faktor vier. Bei Sprache laeuft der Wert beim Cast nach `int16_t` ueber; beim Empfaenger klingt das wie lautes Rauschen oder Knattern. Diesen Wert nur nach einer erneuten, gemessenen I2S-Formatpruefung aendern.

---

## Lizenz

Apache License 2.0, siehe [LICENSE](LICENSE).
