# Parameterbereiche je Künstler — Conductor, Autoplay, Brain 2, Tuning

Ergänzung zum *Regelwerk für einen selbstspielenden Noten- und Akkordgenerator*. Wie die vorhandenen Packs sind das keine Werte, sondern **Bereiche**, aus denen ein Generator pro Preset zieht: gleichverteilt (`3–5`), im Logarithmus gleichverteilt (`log 40–90 s`), als gewichtete Auswahl (`{Free 60 % / Chords 40 %}`) oder fest (`on`). Die Bereiche decken die bestehenden Conductor-Parameter und die in Abschnitt 14 des Regelwerks vorgeschlagenen (mit „neu“ markiert); wer die neuen nicht baut, lässt sie einfach weg. Die Klangparameter der Packs — Quellen, Filter, Raum — bleiben, wie sie sind; hier geht es nur darum, *was* gespielt wird.

Sieben Familien tragen die Grundbereiche; jeder Künstler gehört zu einer Familie und überschreibt nur die Zeilen, die ihn unterscheiden. Die maschinenlesbare Fassung `conductor_ranges.json` enthält für jeden Künstler alle Bereiche aufgelöst.

**Still und bewegt.** Für die bewegte Hälfte eines Packs: `brain_rate` und `auto_rate` mal 0,6, `brain_wander` plus 0,1, `auto_root_move` mal 1,5, `brain_rate_breath` plus 0,2, `brain_cascade` plus 0,1 — jeweils im Wertebereich geklemmt.

**Zwei Grenzen, die kein Bereich überschreiten sollte:** `brain_hold_max` bleibt über dem Doppelten von `brain_hold_min`, sonst wird die Haltezeit ein fester Wert — in der Familie *brit* und bei Paul Bradley ist genau das die Absicht; und `brain2_rate` steht zu `brain_rate` nie in einem Verhältnis nahe 1, 2 oder 3 — wo die Bereiche sich überlappen, sorgt `brain2_golden` dafür.


## Familien

- **sleep** — Schlafkonzert, organisch: Reine Stimmung, Obertonreihe, Haltezeiten in Minuten, Schwebung erwünscht, ein zweiter Conductor im Hintergrund.
- **deep** — Tiefe und Dunkelheit: Tiefes Register, fast keine Terzen, Grundton fällt, lange Haltezeiten, Ereignisse in Böen.
- **ritual** — Ritual und Sakrales: Phrygische Farbe: Quinte und kleine Sekunde über dem Grundton, Grundton fast fest, Fundament leer.
- **cold** — Kalt und minimal: Ein bis drei Stimmen, Quinten, sehr lange Abstände, keine Farbe, keine Luft, geplante Stille.
- **brit** — Britische Drone-Tradition: Ein bis drei Stimmen, Haltezeiten bis zur Obergrenze, Grundton fest, Wechsel als Erinnerung; je weniger, desto richtiger.
- **luminous** — Leuchtend und konsonant: Terzen und Septimen erlaubt, echte Akkordwechsel mit kleinem Voice Leading, tonales Zentrum stark.
- **space** — Weltraum und Berliner Schule: Weite Registerspreizung, Obertonreihe oder Lydisch, schnellere Ereignisse, gelegentlich ein Raster.

| Parameter | sleep | deep | ritual | cold | brit | luminous | space |
|---|---|---|---|---|---|---|---|
| **Brain** | | | | | | | |
| `brain_density` | 3–5 | 2–4 | 3–5 | 1–3 | 1–3 | 3–5 | 3–5 |
| `brain_rate` | log 40–90 s | log 60–150 s | log 60–120 s | log 90–200 s | log 150–300 s | log 30–75 s | log 30–75 s |
| `brain_hold_min` | 90–180 s | 180–300 s | 120–240 s | 120–300 s | 300–600 s | 45–90 s | 60–150 s |
| `brain_hold_max` | 300–480 s | 480–600 s | 400–600 s | 400–600 s | 600 s | 150–300 s | 200–400 s |
| `brain_low` | 36–43 MIDI | 24–30 MIDI | 31–38 MIDI | 36–43 MIDI | 40–48 MIDI | 43–48 MIDI | 31–38 MIDI |
| `brain_high` | 79–86 MIDI | 55–64 MIDI | 72–84 MIDI | 67–76 MIDI | 64–72 MIDI | 84–91 MIDI | 88–96 MIDI |
| `brain_consonance` | 0.7–0.9 | 0.6–0.85 | 0.6–0.8 | 0.8–0.95 | 0.75–0.9 | 0.6–0.85 | 0.6–0.8 |
| `brain_wander` | 0.1–0.3 | 0.05–0.15 | 0–0.1 | 0–0.1 | 0 | 0.2–0.4 | 0.2–0.4 |
| `brain_quantize` | Free | Free | Free | Free | Free | Free | {Free 70 % / 8 bars 15 % / 16 bars 15 %} |
| `brain_timbre` | 0.2–0.5 | 0.3–0.6 | 0.3–0.6 | 0.1–0.3 | 0.2–0.4 | 0.1–0.3 | 0.1–0.3 |
| `brain_spacing` | 0.3–0.6 | 0.5–0.9 | 0.2–0.6 | 0.6–0.9 | 0.3–0.6 | 0.2–0.5 | 0.3–0.6 |
| `brain_harmonic` | 0.5–0.8 | 0.4–0.7 | 0.4–0.6 | 0.5–0.8 | 0.4–0.7 | 0.3–0.6 | 0.5–0.8 |
| `brain_key` | 0.3–0.6 | 0.1–0.3 | 0.4–0.7 | 0.2–0.4 | 0.5–0.8 | 0.6–0.9 | 0.4–0.7 |
| `brain_even` | 0.1–0.3 | 0–0.1 | 0.1–0.3 | 0.1–0.3 | 0.2–0.4 | 0.5–0.8 | 0.3–0.5 |
| `brain_smooth` | 0.5–0.8 | 0.4–0.7 | 0.5–0.8 | 0.6–0.9 | 0.7–1 | 0.8–1 | 0.5–0.8 |
| `brain_blend` | 0–0.1 | 0–0.05 | 0–0.1 | 0–0.05 | 0–0.05 | 0–0.2 | 0–0.2 |
| `brain_cascade` | 0–0.2 | 0.1–0.4 | 0–0.2 | 0 | 0 | 0–0.1 | 0.1–0.3 |
| `brain_surprise` | 0.4–0.55 | 0.3–0.45 | 0.35–0.5 | 0.25–0.4 | 0.2–0.35 | 0.4–0.55 | 0.45–0.6 |
| `brain_homeostat` | 0.3–0.5 | 0.3–0.5 | 0.4–0.6 | 0.4–0.6 | 0.5–0.7 | 0.3–0.5 | 0.3–0.5 |
| `brain_dejavu` | 0.1–0.3 | 0.2–0.4 | 0.3–0.5 | 0.3–0.5 | 0.4–0.7 | 0.2–0.4 | 0.2–0.5 |
| `brain_loop` | 6–10 | 4–8 | 4–8 | 3–6 | 3–5 | 4–8 | 6–12 |
| `brain_spread` | 0.5–0.75 | 0.6–0.85 | 0.5–0.7 | 0.3–0.5 | 0.2–0.4 | 0.4–0.6 | 0.5–0.8 |
| `brain_bias` | 0–0.2 | 0.2–0.4 | 0.1–0.3 | -0.2–0.1 | 0–0.2 | -0.1–0.1 | 0–0.2 |
| **Autoplay** | | | | | | | |
| `auto_mode` | {Free 60 % / Chords 40 %} | {Free 80 % / Chords 20 %} | {Free / Chords} | {Free 70 % / Chords 30 %} | {Free 40 % / Chords 60 %} | {Free 20 % / Chords 80 %} | {Free / Chords} |
| `auto_rate` | log 60–180 s | log 120–300 s | log 90–240 s | log 150–400 s | log 300–900 s | log 30–120 s | log 45–150 s |
| `auto_sync` | Free | Free | Free | Free | Free | Free | Free |
| `auto_lead` | 2–5 st | 1–3 st | 1–3 st | 1–3 st | 1–2 st | 2–4 st | 3–7 st |
| `auto_tension` | 0.1–0.3 | 0.2–0.5 | 0.3–0.6 | 0–0.2 | 0–0.2 | 0.1–0.3 | 0.2–0.4 |
| `auto_root_move` | 0.05–0.15 | 0.02–0.08 | 0–0.05 | 0–0.05 | 0–0.03 | 0.15–0.35 | 0.1–0.25 |
| **Brain 2** | | | | | | | |
| `brain2_on` | {on 60 % / off 40 %} | {on 70 % / off 30 %} | {on / off} | {on / off} | {on 40 % / off 60 %} | {on 30 % / off 70 %} | {on 70 % / off 30 %} |
| `brain2_density` | 1–2 | 1–2 | 1–2 | 1 | 1 | 1–2 | 1–3 |
| `brain2_rate` | log 120–300 s | log 200–500 s | log 180–400 s | log 240–600 s | log 300–600 s | log 120–300 s | log 90–240 s |
| `brain2_hold_min` | 120–240 s | 240–480 s | 240–480 s | 300–600 s | 300–600 s | 120–300 s | 90–240 s |
| `brain2_hold_max` | 480–900 s | 600–1200 s | 600–1000 s | 800–1200 s | 900–1200 s | 400–800 s | 300–600 s |
| `brain2_low` | 24–31 MIDI | 20–24 MIDI | 24–31 MIDI | 24–36 MIDI | 28–36 MIDI | 31–36 MIDI | 24–31 MIDI |
| `brain2_high` | 48–55 MIDI | 36–40 MIDI | 43–52 MIDI | 48–60 MIDI | 48–55 MIDI | 55–60 MIDI | 60–72 MIDI |
| **Tuning** | | | | | | | |
| `scale` | {Ptolemy major / 7-limit / harmonic 8-16 / otonality 1-3-5-7-9-11 / just pentatonic} | {just minor / subharmonic 16-8 / Pythagorean / 7-limit} | {just minor / Pythagorean / subharmonic 16-8 / 7-limit} | {just pentatonic / Pythagorean / Ptolemy major / 12-TET} | {Ptolemy major / just minor / 12-TET / just pentatonic} | {Ptolemy major / 12-TET / just pentatonic / 7-limit} | {harmonic 8-16 23 % / otonality 1-3-5-7-9-11 23 % / Ptolemy major 23 % / 7-limit 23 % / Bohlen-Pierce 7 %} |
| `purity` | 0.85–1 | 0.7–0.95 | 0.8–1 | 0.6–0.9 | 0.5–0.9 | 0.6–0.9 | 0.7–1 |
| `purity_drift` | 0.1–0.3 | 0.05–0.2 | 0.05–0.15 | 0–0.1 | 0.1–0.3 | 0.05–0.2 | 0.1–0.3 |
| **Brain (neu)** | | | | | | | |
| `brain_layers` | 0.6–0.9 | 0.7–1 | 0.6–0.9 | 0.4–0.7 | 0.3–0.6 | 0.3–0.6 | 0.6–0.9 |
| `brain_bass_hold` | 2–4 x | 3–6 x | 3–6 x | 2–4 x | 2–3 x | 1.5–3 x | 2–4 x |
| `brain_top_soft` | 0.4–0.7 | 0.5–0.8 | 0.3–0.6 | 0.5–0.8 | 0.3–0.6 | 0.2–0.5 | 0.4–0.7 |
| `brain_low_spacing` | 0.6–0.9 | 0.8–1 | 0.7–1 | 0.7–1 | 0.5–0.8 | 0.4–0.7 | 0.6–0.9 |
| `brain_third_floor` | 48–55 MIDI | 60–72 MIDI | 55–64 MIDI | 55–60 MIDI | 48–55 MIDI | 43–48 MIDI | 48–55 MIDI |
| `brain_leading` | 0.8–1 | 0.9–1 | 0.6–0.9 | 0.9–1 | 0.8–1 | 0.5–0.8 | 0.7–1 |
| `brain_thirds` | 0–0.3 | -0.8–-0.4 | -0.6–-0.2 | -0.4–0 | -0.2–0.2 | 0.4–0.8 | 0–0.4 |
| `brain_seconds` | -0.3–0 | -0.6–-0.2 | 0.2–0.6 | -0.4–0 | -0.3–0 | 0.1–0.4 | 0–0.3 |
| `brain_seventh` | 0.4–0.8 | 0.3–0.6 | 0.3–0.6 | 0.2–0.5 | 0.2–0.4 | 0.2–0.5 | 0.4–0.8 |
| `brain_degree_swap` | 0.1–0.3 | 0–0.15 | 0.05–0.15 | 0–0.1 | 0 | 0.1–0.3 | 0.1–0.3 |
| `brain_rate_breath` | 0.3–0.6 | 0.2–0.5 | 0.2–0.4 | 0.1–0.3 | 0–0.2 | 0.2–0.4 | 0.3–0.6 |
| `brain_overlap` | 10–25 s | 15–40 s | 15–30 s | 20–45 s | 30–60 s | 10–20 s | 8–20 s |
| `brain_onset_guard` | on | on | on | on | on | on | on |
| `brain_retrigger` | 30–60 s | 60–120 s | 45–90 s | 60–120 s | 90–120 s | 30–60 s | 20–45 s |
| `brain_silence` | 0–0.1 | 0.05–0.2 | 0.05–0.15 | 0.1–0.3 | 0–0.1 | 0–0.05 | 0–0.1 |
| `brain_root_steps` | {Fifths / Diatonic} | {Falling / Fifths} | {Fifths 60 % / Falling 40 %} | Fifths | {Fifths / Diatonic} | Diatonic | {Fifths / Diatonic} |
| `brain_root_down` | 0.3–0.6 | 0.6–1 | 0.5–0.8 | 0.2–0.5 | 0–0.3 | 0–0.3 | 0.2–0.5 |
| `brain_pivot` | 30–60 s | 40–90 s | 40–80 s | 30–60 s | 45–90 s | 20–40 s | 20–45 s |
| `brain_home` | 0.3–0.6 | 0.1–0.3 | 0.5–0.8 | 0.3–0.5 | 0.6–0.9 | 0.5–0.8 | 0.3–0.6 |
| `brain_memory` | 8–12 min | 10–20 min | 10–15 min | 15–25 min | 10–20 min | 6–10 min | 8–12 min |
| **Brain 2 (neu)** | | | | | | | |
| `brain2_golden` | on | on | on | on | on | on | on |
| `brain2_interval` | {Fifth / Root / Octave} | {Root / Fifth / Octave} | {Fifth / Root} | {Fifth / Octave} | {Fifth / Root} | {Root / Fifth} | {Fifth / Octave / Seventh} |
| **Tuning (neu)** | | | | | | | |
| `tuning_hold_sounding` | on | on | on | on | on | on | on |
| `beat_ceiling` | 1–2 Hz | 0.3–0.8 Hz | 0.5–1.2 Hz | 0.2–0.6 Hz | 0.8–1.5 Hz | 1–2 Hz | 1–2 Hz |
| **Space (neu)** | | | | | | | |
| `layer_depth` | 0.5–0.8 | 0.6–0.9 | 0.5–0.8 | 0.6–0.9 | 0.3–0.6 | 0.3–0.5 | 0.7–1 |
| **Amp Env (neu)** | | | | | | | |
| `env_vel_attack` | 0.3–0.6 | 0.4–0.8 | 0.3–0.6 | 0.5–0.8 | 0.3–0.6 | 0.2–0.5 | 0.3–0.6 |
| **Source 1 (neu)** | | | | | | | |
| `strand_low_detune` | 0.4–0.7 | 0.7–1 | 0.5–0.8 | 0.6–0.9 | 0.3–0.6 | 0.3–0.5 | 0.4–0.7 |
| **Review 25.09.2026** | | | | | | | |
| `purity_adapt` | 0.4–0.8 | 0.4–0.8 | 0.4–0.8 | 0.4–0.8 | 0.4–0.8 | 0.4–0.8 | 0.4–0.8 |
| `brain_root_targets` | Modal | Modal | Phrygian | Modal | Modal | Mediant | Modal |
| `brain_utonal` | 0–0.2 | 0.6–0.9 | 0.4–0.7 | 0.3–0.6 | 0–0.2 | 0–0.1 | 0–0.2 |
| `brain_series` | 0.2–0.5 | 0–0.1 | 0–0.1 | 0–0.1 | 0–0.1 | 0–0.2 | 0.2–0.5 |
| `arc_harmony` | 0.2–0.4 | 0.1–0.3 | 0.2–0.4 | 0–0.2 | 0–0.15 | 0.3–0.5 | 0.3–0.5 |

**Review 25.09.2026.** Die Zeilen unter *Review* kommen aus der Prüfung des Conductors gegen die Ambient-Harmonielehre: `purity_adapt` überall dort, wo der Grundton wandert (Befund F3, gilt nur mit `brain_wander` oder `auto_root_move` über null); `brain_root_targets` gewichtet die Grundtonziele statt sie gleichverteilt zu ziehen, mit dem Ganzton als häufigstem Schritt nach Quinte und Quarte (F8); `brain_utonal` lässt Harmonic auch Untertonreihen als verwurzelt hören (F2b); `brain_series` ist die Harmonic Cloud, Teiltöne eines nicht klingenden Fundaments (Abschnitt 4); `arc_harmony` legt den Stundenbogen auf Konsonanz, Tonart und Harmonizität (Abschnitt 4). In *deep* liegt `brain2_low` jetzt bei 20–24 MIDI (26–33 Hz) und `brain2_high` bei 36–40 (F7): 16 Hz spielt kein Wiedergabesystem, aber jeder Limiter hört ihn. Die Tabelle dazu ist `Tools/library/review.py`; `retrofit_review.py` hat sie auf die bestehenden Presets angewandt.

## Künstler


### Familie *sleep* — Schlafkonzert, organisch

#### Robert Rich

Siebener-Limit, die Septime als 7/4, dichte Cluster in Minuten; der zweite Conductor immer an.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 4–6 | 3–5 |
| `brain_low` | 31–36 MIDI | 36–43 MIDI |
| `brain2_on` | on | {on 60 % / off 40 %} |
| `scale` | {Ptolemy major / 7-limit / otonality 1-3-5-7-9-11} | {Ptolemy major / 7-limit / harmonic 8-16 / otonality 1-3-5-7-9-11 / just pentatonic} |
| `purity` | 0.95–1 | 0.85–1 |
| `brain_thirds` | 0.1–0.4 | 0–0.3 |
| `brain_seventh` | 0.6–0.9 | 0.4–0.8 |
| `beat_ceiling` | 1.2–2 Hz | 1–2 Hz |

#### Steve Roach

Organischer, atmender, mit Ereignisböen und gelegentlich einem Raster im Hintergrund.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_rate` | log 30–70 s | log 40–90 s |
| `brain_wander` | 0.2–0.4 | 0.1–0.3 |
| `brain_quantize` | {Free 80 % / 8 bars 20 %} | Free |
| `brain_cascade` | 0.2–0.4 | 0–0.2 |
| `brain_dejavu` | 0.3–0.5 | 0.1–0.3 |
| `brain_loop` | 8–16 | 6–10 |
| `scale` | {Ptolemy major / just pentatonic / harmonic 8-16} | {Ptolemy major / 7-limit / harmonic 8-16 / otonality 1-3-5-7-9-11 / just pentatonic} |
| `brain_low_spacing` | 0.5–0.8 | 0.6–0.9 |

#### Klaus Wiese

Fast still: wenige Stimmen, Grundton fest, reinste Stimmung, kaum Schwebung, immer nach Hause.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 2–4 | 3–5 |
| `brain_rate` | log 90–180 s | log 40–90 s |
| `brain_hold_min` | 240–480 s | 90–180 s |
| `brain_hold_max` | 600 s | 300–480 s |
| `brain_wander` | 0 | 0.1–0.3 |
| `auto_root_move` | 0–0.03 | 0.05–0.15 |
| `scale` | {Ptolemy major / just pentatonic / Pythagorean} | {Ptolemy major / 7-limit / harmonic 8-16 / otonality 1-3-5-7-9-11 / just pentatonic} |
| `purity` | 0.95–1 | 0.85–1 |
| `brain_silence` | 0–0.05 | 0–0.1 |
| `brain_home` | 0.8–1 | 0.3–0.6 |
| `beat_ceiling` | 0.5–1 Hz | 1–2 Hz |

#### Mathias Grassow

Dichte Akkordwände in reiner Stimmung, ein Grundton für das Ganze, zweiter Conductor mit zwei bis drei Stimmen.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 5–8 | 3–5 |
| `brain_rate` | log 60–120 s | log 40–90 s |
| `brain_hold_min` | 180–300 s | 90–180 s |
| `brain_hold_max` | 480–600 s | 300–480 s |
| `brain_harmonic` | 0.7–0.9 | 0.5–0.8 |
| `brain_key` | 0.5–0.8 | 0.3–0.6 |
| `brain2_on` | on | {on 60 % / off 40 %} |
| `brain2_density` | 2–3 | 1–2 |
| `scale` | {Ptolemy major / just minor / 7-limit} | {Ptolemy major / 7-limit / harmonic 8-16 / otonality 1-3-5-7-9-11 / just pentatonic} |
| `purity` | 0.9–1 | 0.85–1 |
| `brain_layers` | 0.7–1 | 0.6–0.9 |

#### Oöphoi

Rein und extrem langsam: nichts hat eine Kante, langsame Einsätze, lange Überlappung, keine Böen.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 2–4 | 3–5 |
| `brain_rate` | log 90–180 s | log 40–90 s |
| `brain_hold_min` | 300–600 s | 90–180 s |
| `brain_hold_max` | 600 s | 300–480 s |
| `brain_consonance` | 0.85–1 | 0.7–0.9 |
| `brain_wander` | 0–0.1 | 0.1–0.3 |
| `brain_cascade` | 0 | 0–0.2 |
| `scale` | {Ptolemy major / otonality 1-3-5-7-9-11 / just pentatonic} | {Ptolemy major / 7-limit / harmonic 8-16 / otonality 1-3-5-7-9-11 / just pentatonic} |
| `purity` | 0.95–1 | 0.85–1 |
| `brain_thirds` | 0–0.2 | 0–0.3 |
| `brain_overlap` | 20–40 s | 10–25 s |
| `env_vel_attack` | 0.6–0.9 | 0.3–0.6 |

#### Voice of Eye

Rituell-organisch: inharmonische Objekte, Böen, Sekunden oben, mehr Extreme als die Familie.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 3–5 | 3–5 |
| `brain_consonance` | 0.6–0.8 | 0.7–0.9 |
| `brain_timbre` | 0.4–0.7 | 0.2–0.5 |
| `brain_cascade` | 0.2–0.4 | 0–0.2 |
| `brain_dejavu` | 0.3–0.5 | 0.1–0.3 |
| `brain_spread` | 0.6–0.85 | 0.5–0.75 |
| `scale` | {otonality 1-3-5-7-9-11 / just minor / harmonic 8-16} | {Ptolemy major / 7-limit / harmonic 8-16 / otonality 1-3-5-7-9-11 / just pentatonic} |
| `brain_seconds` | 0–0.3 | -0.3–0 |

#### Vidna Obmana

Organisch und zyklisch: Böen, lange Ringe, Slendro als Gast, Sekunden oben, Stimmung driftet.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 3–5 | 3–5 |
| `brain_quantize` | {Free 80 % / 8 bars 20 %} | Free |
| `brain_timbre` | 0.3–0.6 | 0.2–0.5 |
| `brain_cascade` | 0.2–0.4 | 0–0.2 |
| `brain_dejavu` | 0.3–0.6 | 0.1–0.3 |
| `brain_loop` | 6–12 | 6–10 |
| `scale` | {just minor 30 % / Ptolemy major 30 % / just pentatonic 30 % / slendro 9 %} | {Ptolemy major / 7-limit / harmonic 8-16 / otonality 1-3-5-7-9-11 / just pentatonic} |
| `purity` | 0.6–0.9 | 0.85–1 |
| `purity_drift` | 0.2–0.4 | 0.1–0.3 |
| `brain_seconds` | 0–0.3 | -0.3–0 |

#### Tom Heasley

Tuba-Drones: alles tief, zwei bis drei Stimmen, Fundament sehr lang, Septime als 7/4, langsamste Einsätze.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 2–3 | 3–5 |
| `brain_rate` | log 60–120 s | log 40–90 s |
| `brain_hold_min` | 120–240 s | 90–180 s |
| `brain_hold_max` | 300–600 s | 300–480 s |
| `brain_low` | 26–31 MIDI | 36–43 MIDI |
| `brain_high` | 55–64 MIDI | 79–86 MIDI |
| `brain_consonance` | 0.8–0.95 | 0.7–0.9 |
| `brain2_on` | on | {on 60 % / off 40 %} |
| `brain2_low` | 19–24 MIDI | 24–31 MIDI |
| `scale` | {Ptolemy major / harmonic 8-16 / just pentatonic} | {Ptolemy major / 7-limit / harmonic 8-16 / otonality 1-3-5-7-9-11 / just pentatonic} |
| `purity` | 0.8–1 | 0.85–1 |
| `brain_layers` | 0.8–1 | 0.6–0.9 |
| `brain_bass_hold` | 3–6 x | 2–4 x |
| `brain_seventh` | 0.5–0.8 | 0.4–0.8 |
| `env_vel_attack` | 0.5–0.8 | 0.3–0.6 |

#### Jeff Pearce

Gitarrenschichten: mittleres bis hohes Register, Terzen, Zentrum, Akkordmodus, Figuren, die wiederkehren.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 3–5 | 3–5 |
| `brain_rate` | log 30–75 s | log 40–90 s |
| `brain_hold_min` | 45–120 s | 90–180 s |
| `brain_hold_max` | 150–300 s | 300–480 s |
| `brain_low` | 40–45 MIDI | 36–43 MIDI |
| `brain_high` | 79–88 MIDI | 79–86 MIDI |
| `brain_key` | 0.6–0.9 | 0.3–0.6 |
| `brain_smooth` | 0.8–1 | 0.5–0.8 |
| `brain_dejavu` | 0.3–0.6 | 0.1–0.3 |
| `brain_loop` | 4–8 | 6–10 |
| `auto_mode` | {Free 30 % / Chords 70 %} | {Free 60 % / Chords 40 %} |
| `auto_lead` | 2–4 st | 2–5 st |
| `auto_root_move` | 0.1–0.25 | 0.05–0.15 |
| `scale` | {Ptolemy major / just pentatonic / 12-TET} | {Ptolemy major / 7-limit / harmonic 8-16 / otonality 1-3-5-7-9-11 / just pentatonic} |
| `purity` | 0.4–0.8 | 0.85–1 |
| `brain_layers` | 0.3–0.6 | 0.6–0.9 |
| `brain_thirds` | 0.3–0.6 | 0–0.3 |
| `env_vel_attack` | 0.4–0.7 | 0.3–0.6 |

#### Amir Baghiri

Tribal-organisch: Böen, Sekunden oben, lange Ringe, gelegentlich ein Raster, der Grundton wandert.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 3–5 | 3–5 |
| `brain_wander` | 0.2–0.4 | 0.1–0.3 |
| `brain_quantize` | {Free 70 % / 8 bars 30 %} | Free |
| `brain_timbre` | 0.2–0.5 | 0.2–0.5 |
| `brain_cascade` | 0.2–0.5 | 0–0.2 |
| `brain_dejavu` | 0.3–0.6 | 0.1–0.3 |
| `brain_loop` | 6–12 | 6–10 |
| `scale` | {just minor 31 % / Pythagorean 31 % / Ptolemy major 31 % / slendro 6 %} | {Ptolemy major / 7-limit / harmonic 8-16 / otonality 1-3-5-7-9-11 / just pentatonic} |
| `purity` | 0.7–0.95 | 0.85–1 |
| `brain_seconds` | 0–0.3 | -0.3–0 |

#### Jim Cole

Obertongesang: die Reihe als Skala, ein Grundton, rein, Partialtöne verriegelt, kaum Schwebung.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 3–5 | 3–5 |
| `brain_hold_min` | 120–240 s | 90–180 s |
| `brain_hold_max` | 300–600 s | 300–480 s |
| `brain_low` | 36–43 MIDI | 36–43 MIDI |
| `brain_high` | 79–91 MIDI | 79–86 MIDI |
| `brain_consonance` | 0.85–1 | 0.7–0.9 |
| `brain_timbre` | 0–0.2 | 0.2–0.5 |
| `brain_harmonic` | 0.8–1 | 0.5–0.8 |
| `brain2_on` | on | {on 60 % / off 40 %} |
| `scale` | {harmonic 8-16 / otonality 1-3-5-7-9-11} | {Ptolemy major / 7-limit / harmonic 8-16 / otonality 1-3-5-7-9-11 / just pentatonic} |
| `purity` | 1 | 0.85–1 |
| `brain_thirds` | 0.1–0.4 | 0–0.3 |
| `brain_seventh` | 0.6–0.9 | 0.4–0.8 |
| `brain2_interval` | {Fifth / Octave} | {Fifth / Root / Octave} |
| `beat_ceiling` | 0.3–0.8 Hz | 1–2 Hz |


### Familie *deep* — Tiefe und Dunkelheit

#### Sleep Research Facility

Der Maschinenraum: ein bis zwei Stimmen an der Haltegrenze, nur Quinten und Oktaven, Fundament ganz unten.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 1–2 | 2–4 |
| `brain_rate` | log 200–300 s | log 60–150 s |
| `brain_hold_min` | 400–600 s | 180–300 s |
| `brain_hold_max` | 600 s | 480–600 s |
| `brain_high` | 48–55 MIDI | 55–64 MIDI |
| `brain_consonance` | 0.9–1 | 0.6–0.85 |
| `brain_cascade` | 0 | 0.1–0.4 |
| `brain2_on` | on | {on 70 % / off 30 %} |
| `brain2_low` | 20–22 MIDI | 20–24 MIDI |
| `scale` | {Pythagorean / subharmonic 16-8} | {just minor / subharmonic 16-8 / Pythagorean / 7-limit} |
| `brain_layers` | 1 | 0.7–1 |
| `brain_bass_hold` | 5–8 x | 3–6 x |
| `brain_silence` | 0.1–0.3 | 0.05–0.2 |

#### Ulf Söderberg

Nordisch, mit phrygischer Farbe oben und einem tonalen Zentrum; Hintergrund immer an.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 3–4 | 2–4 |
| `brain_high` | 64–72 MIDI | 55–64 MIDI |
| `brain_key` | 0.4–0.6 | 0.1–0.3 |
| `brain_cascade` | 0.1–0.3 | 0.1–0.4 |
| `brain2_on` | on | {on 70 % / off 30 %} |
| `scale` | {just minor / Pythagorean} | {just minor / subharmonic 16-8 / Pythagorean / 7-limit} |
| `brain_seconds` | 0.1–0.4 | -0.6–-0.2 |
| `brain_silence` | 0.1–0.2 | 0.05–0.2 |

#### Inade

Rituelles Metall: Böen, inharmonische Klänge, Sekunden oben erlaubt, lauter und breiter als die Familie.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 3–5 | 2–4 |
| `brain_high` | 64–72 MIDI | 55–64 MIDI |
| `brain_timbre` | 0.4–0.7 | 0.3–0.6 |
| `brain_cascade` | 0.2–0.5 | 0.1–0.4 |
| `brain_spread` | 0.7–0.9 | 0.6–0.85 |
| `brain_bias` | 0.2–0.4 | 0.2–0.4 |
| `brain2_on` | on | {on 70 % / off 30 %} |
| `scale` | {just minor / subharmonic 16-8 / Pythagorean} | {just minor / subharmonic 16-8 / Pythagorean / 7-limit} |
| `brain_seconds` | 0–0.3 | -0.6–-0.2 |
| `brain_silence` | 0.05–0.15 | 0.05–0.2 |

#### Arecibo

Tiefe und Kosmos: nur reine Intervalle, Fundament ganz unten, Stille zwischen Signalen.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 1–3 | 2–4 |
| `brain_high` | 55–67 MIDI | 55–64 MIDI |
| `brain_consonance` | 0.8–1 | 0.6–0.85 |
| `brain_timbre` | 0.3–0.6 | 0.3–0.6 |
| `brain_cascade` | 0.1–0.3 | 0.1–0.4 |
| `brain_spread` | 0.6–0.9 | 0.6–0.85 |
| `brain2_on` | on | {on 70 % / off 30 %} |
| `brain2_low` | 20–24 MIDI | 20–24 MIDI |
| `scale` | {Pythagorean / subharmonic 16-8 / otonality 1-3-5-7-9-11} | {just minor / subharmonic 16-8 / Pythagorean / 7-limit} |
| `brain_silence` | 0.1–0.3 | 0.05–0.2 |

#### Moljebka Pvlse

Industriell und schleifend: starke Böen, inharmonisch, Stimmung treibt, lange Ringe kehren wieder.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 2–4 | 2–4 |
| `brain_high` | 60–72 MIDI | 55–64 MIDI |
| `brain_consonance` | 0.6–0.85 | 0.6–0.85 |
| `brain_timbre` | 0.4–0.7 | 0.3–0.6 |
| `brain_cascade` | 0.3–0.6 | 0.1–0.4 |
| `brain_dejavu` | 0.3–0.6 | 0.2–0.4 |
| `brain_loop` | 4–10 | 4–8 |
| `brain_spread` | 0.7–0.9 | 0.6–0.85 |
| `scale` | {just minor / Pythagorean / 12-TET} | {just minor / subharmonic 16-8 / Pythagorean / 7-limit} |
| `purity` | 0.5–0.8 | 0.7–0.95 |
| `purity_drift` | 0.2–0.4 | 0.05–0.2 |

#### Tho-So-Aa

Subharmonisch und laut: Böen, Cluster im Ansatz erlaubt, Fundament so tief wie möglich.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 2–4 | 2–4 |
| `brain_low` | 24–28 MIDI | 24–30 MIDI |
| `brain_consonance` | 0.6–0.8 | 0.6–0.85 |
| `brain_timbre` | 0.4–0.7 | 0.3–0.6 |
| `brain_spacing` | -0.1–0.3 | 0.5–0.9 |
| `brain_cascade` | 0.3–0.5 | 0.1–0.4 |
| `brain_bias` | 0.3–0.5 | 0.2–0.4 |
| `scale` | {subharmonic 16-8 / just minor} | {just minor / subharmonic 16-8 / Pythagorean / 7-limit} |
| `brain_seconds` | -0.2–0.2 | -0.6–-0.2 |
| `brain_silence` | 0.05–0.2 | 0.05–0.2 |

#### Atomine Elektrine

Tiefe mit Weltraum: Otonalität und Subharmonik, höheres Register als die Familie, Septime als Farbe.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 2–4 | 2–4 |
| `brain_high` | 67–79 MIDI | 55–64 MIDI |
| `brain_key` | 0.2–0.4 | 0.1–0.3 |
| `brain_cascade` | 0.1–0.3 | 0.1–0.4 |
| `brain2_on` | on | {on 70 % / off 30 %} |
| `scale` | {otonality 1-3-5-7-9-11 / Pythagorean / subharmonic 16-8} | {just minor / subharmonic 16-8 / Pythagorean / 7-limit} |
| `purity` | 0.7–0.95 | 0.7–0.95 |
| `brain_seventh` | 0.4–0.7 | 0.3–0.6 |
| `brain_silence` | 0.05–0.15 | 0.05–0.2 |

#### Polygon

Ruhige Tiefe: zwei bis drei Stimmen, Moll, wenig Böen, alles im unteren Register.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 2–3 | 2–4 |
| `brain_rate` | log 120–240 s | log 60–150 s |
| `brain_low` | 24–31 MIDI | 24–30 MIDI |
| `brain_high` | 55–64 MIDI | 55–64 MIDI |
| `brain_consonance` | 0.8–0.95 | 0.6–0.85 |
| `brain_cascade` | 0.1–0.3 | 0.1–0.4 |
| `brain_spread` | 0.6–0.8 | 0.6–0.85 |
| `scale` | {just minor / Pythagorean} | {just minor / subharmonic 16-8 / Pythagorean / 7-limit} |

#### Kammarheit

Leere Räume: wenige Noten, alles fern, Stille, ein schwaches tonales Zentrum.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 1–3 | 2–4 |
| `brain_rate` | log 150–300 s | log 60–150 s |
| `brain_hold_min` | 300–600 s | 180–300 s |
| `brain_hold_max` | 600 s | 480–600 s |
| `brain_high` | 60–67 MIDI | 55–64 MIDI |
| `brain_consonance` | 0.8–0.95 | 0.6–0.85 |
| `brain_key` | 0.3–0.5 | 0.1–0.3 |
| `brain_bias` | -0.2–0.1 | 0.2–0.4 |
| `scale` | {just minor / Pythagorean} | {just minor / subharmonic 16-8 / Pythagorean / 7-limit} |
| `purity` | 0.6–0.9 | 0.7–0.95 |
| `brain_silence` | 0.15–0.3 | 0.05–0.2 |
| `layer_depth` | 0.8–1 | 0.6–0.9 |

#### Gustaf Hildebrand

Filmisch: mehr Stimmen, ein Zentrum, Akkordmodus, der Grundton reist; zweiter Conductor an.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 3–5 | 2–4 |
| `brain_high` | 72–79 MIDI | 55–64 MIDI |
| `brain_harmonic` | 0.6–0.8 | 0.4–0.7 |
| `brain_key` | 0.4–0.7 | 0.1–0.3 |
| `brain_cascade` | 0.1–0.3 | 0.1–0.4 |
| `brain_bias` | 0.2–0.4 | 0.2–0.4 |
| `auto_mode` | {Free / Chords} | {Free 80 % / Chords 20 %} |
| `auto_lead` | 2–4 st | 1–3 st |
| `auto_root_move` | 0.05–0.15 | 0.02–0.08 |
| `brain2_on` | on | {on 70 % / off 30 %} |
| `scale` | {just minor / Ptolemy major / harmonic 8-16} | {just minor / subharmonic 16-8 / Pythagorean / 7-limit} |
| `brain_layers` | 0.7–1 | 0.7–1 |
| `brain_thirds` | -0.3–0.1 | -0.8–-0.4 |

#### Apoptose

Ritual in der Tiefe: Sekunden oben, Böen, gelegentlich ein Raster, kurze Erinnerungsringe.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 2–4 | 2–4 |
| `brain_high` | 64–72 MIDI | 55–64 MIDI |
| `brain_quantize` | {Free 70 % / 8 bars 30 %} | Free |
| `brain_timbre` | 0.3–0.6 | 0.3–0.6 |
| `brain_cascade` | 0.2–0.4 | 0.1–0.4 |
| `brain_dejavu` | 0.4–0.7 | 0.2–0.4 |
| `brain_loop` | 3–6 | 4–8 |
| `brain_spread` | 0.6–0.85 | 0.6–0.85 |
| `scale` | {just minor / Pythagorean / subharmonic 16-8} | {just minor / subharmonic 16-8 / Pythagorean / 7-limit} |
| `brain_seconds` | 0.1–0.4 | -0.6–-0.2 |

#### Land:Fire

Glut: Böen, inharmonisch, Konsonanz locker, höheres Register als die Familie, treibende Stimmung.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 2–4 | 2–4 |
| `brain_high` | 60–72 MIDI | 55–64 MIDI |
| `brain_consonance` | 0.6–0.85 | 0.6–0.85 |
| `brain_timbre` | 0.3–0.6 | 0.3–0.6 |
| `brain_cascade` | 0.2–0.4 | 0.1–0.4 |
| `scale` | {just minor / subharmonic 16-8 / Pythagorean} | {just minor / subharmonic 16-8 / Pythagorean / 7-limit} |
| `purity_drift` | 0.1–0.3 | 0.05–0.2 |
| `brain_silence` | 0.05–0.2 | 0.05–0.2 |

#### Lustmord

Die Referenz der Familie: Fundament ganz unten, Hintergrund noch tiefer, laut und lang, Schichten streng.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 2–4 | 2–4 |
| `brain_low` | 24–28 MIDI | 24–30 MIDI |
| `brain_high` | 52–60 MIDI | 55–64 MIDI |
| `brain_consonance` | 0.7–0.9 | 0.6–0.85 |
| `brain_timbre` | 0.3–0.6 | 0.3–0.6 |
| `brain_cascade` | 0.1–0.3 | 0.1–0.4 |
| `brain_bias` | 0.3–0.5 | 0.2–0.4 |
| `brain2_on` | on | {on 70 % / off 30 %} |
| `brain2_low` | 20–22 MIDI | 20–24 MIDI |
| `scale` | {Pythagorean / subharmonic 16-8 / just minor} | {just minor / subharmonic 16-8 / Pythagorean / 7-limit} |
| `brain_layers` | 0.9–1 | 0.7–1 |
| `brain_bass_hold` | 4–8 x | 3–6 x |
| `brain_silence` | 0.05–0.15 | 0.05–0.2 |


### Familie *ritual* — Ritual und Sakrales

#### Raison d'Être

Die Referenz der Familie: kleine Sekunde über dem Grundton, Hintergrund immer an, gelegentliche Stille.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain2_on` | on | {on / off} |
| `scale` | {just minor / Pythagorean} | {just minor / Pythagorean / subharmonic 16-8 / 7-limit} |
| `brain_seconds` | 0.3–0.6 | 0.2–0.6 |
| `brain_silence` | 0.1–0.2 | 0.05–0.15 |

#### Deutsch Nepal

Wenige Stimmen, fallender Grundton, laute lange Töne, kurze Schleifen, die wiederkehren; manchmal ein Raster.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 1–3 | 3–5 |
| `brain_rate` | log 120–300 s | log 60–120 s |
| `brain_hold_min` | 300–600 s | 120–240 s |
| `brain_hold_max` | 600 s | 400–600 s |
| `brain_wander` | 0 | 0–0.1 |
| `brain_quantize` | {Free 60 % / 4 bars 20 % / 8 bars 20 %} | Free |
| `brain_cascade` | 0.2–0.5 | 0–0.2 |
| `brain_dejavu` | 0.5–0.8 | 0.3–0.5 |
| `brain_loop` | 2–4 | 4–8 |
| `brain_spread` | 0.7–0.9 | 0.5–0.7 |
| `brain_bias` | 0.3–0.5 | 0.1–0.3 |
| `brain_thirds` | -0.8–-0.5 | -0.6–-0.2 |
| `brain_root_steps` | Falling | {Fifths 60 % / Falling 40 %} |
| `brain_root_down` | 0.8–1 | 0.5–0.8 |

#### Troum

Weicher als die Familie: mehr Stimmen, weniger rein, Stimmung driftet, kürzere Haltezeiten, Schichten flacher.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 3–5 | 3–5 |
| `brain_rate` | log 45–120 s | log 60–120 s |
| `brain_hold_min` | 90–180 s | 120–240 s |
| `brain_hold_max` | 300–480 s | 400–600 s |
| `brain_consonance` | 0.6–0.8 | 0.6–0.8 |
| `brain_timbre` | 0.3–0.6 | 0.3–0.6 |
| `brain_cascade` | 0.1–0.3 | 0–0.2 |
| `scale` | {just minor / 12-TET / Ptolemy major} | {just minor / Pythagorean / subharmonic 16-8 / 7-limit} |
| `purity` | 0.4–0.7 | 0.8–1 |
| `purity_drift` | 0.2–0.4 | 0.05–0.15 |
| `brain_layers` | 0.4–0.7 | 0.6–0.9 |
| `brain_thirds` | 0–0.3 | -0.6–-0.2 |


### Familie *cold* — Kalt und minimal

#### Thomas Köner

Tiefer und noch stiller als die Familie: ein bis zwei Stimmen, Quinten, viel Stille, temperiert erlaubt.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 1–2 | 1–3 |
| `brain_rate` | log 150–300 s | log 90–200 s |
| `brain_hold_min` | 240–600 s | 120–300 s |
| `brain_hold_max` | 600 s | 400–600 s |
| `brain_low` | 24–31 MIDI | 36–43 MIDI |
| `brain_high` | 55–64 MIDI | 67–76 MIDI |
| `brain_consonance` | 0.85–1 | 0.8–0.95 |
| `brain_bias` | 0.2–0.4 | -0.2–0.1 |
| `scale` | {Pythagorean / just pentatonic / 12-TET} | {just pentatonic / Pythagorean / Ptolemy major / 12-TET} |
| `purity` | 0.5–0.8 | 0.6–0.9 |
| `brain_layers` | 0.7–1 | 0.4–0.7 |
| `brain_seconds` | -0.2–0.3 | -0.4–0 |
| `brain_silence` | 0.2–0.4 | 0.1–0.3 |

#### Loscil

Die Ausnahme von G2: Moll-Sept-Klänge, temperiert, Wechsel auf 8 oder 16 Takte, kleines Voice Leading.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 3–4 | 1–3 |
| `brain_rate` | log 30–60 s | log 90–200 s |
| `brain_low` | 36–43 MIDI | 36–43 MIDI |
| `brain_high` | 72–79 MIDI | 67–76 MIDI |
| `brain_quantize` | {Free 40 % / 8 bars 30 % / 16 bars 30 %} | Free |
| `brain_key` | 0.6–0.8 | 0.2–0.4 |
| `brain_smooth` | 0.8–1 | 0.6–0.9 |
| `brain_dejavu` | 0.4–0.7 | 0.3–0.5 |
| `brain_loop` | 4–8 | 3–6 |
| `auto_mode` | {Free 20 % / Chords 80 %} | {Free 70 % / Chords 30 %} |
| `auto_rate` | log 60–150 s | log 150–400 s |
| `auto_sync` | {Free 40 % / 8 bars 30 % / 16 bars 30 %} | Free |
| `auto_lead` | 1–3 st | 1–3 st |
| `scale` | {just minor / 12-TET} | {just pentatonic / Pythagorean / Ptolemy major / 12-TET} |
| `purity` | 0.3–0.6 | 0.6–0.9 |
| `brain_thirds` | 0.2–0.5 | -0.4–0 |
| `brain_seventh` | 0.5–0.8 | 0.2–0.5 |

#### Hazard

Rauschen mit Grundton: ein bis zwei Stimmen, nichts bewegt sich, laut und lang, viel Stille.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 1–2 | 1–3 |
| `brain_rate` | log 200–300 s | log 90–200 s |
| `brain_hold_min` | 300–600 s | 120–300 s |
| `brain_hold_max` | 600 s | 400–600 s |
| `brain_low` | 31–36 MIDI | 36–43 MIDI |
| `brain_high` | 55–60 MIDI | 67–76 MIDI |
| `brain_consonance` | 0.9–1 | 0.8–0.95 |
| `brain_wander` | 0 | 0–0.1 |
| `brain_spread` | 0.2–0.4 | 0.3–0.5 |
| `brain_bias` | 0.3–0.5 | -0.2–0.1 |
| `auto_root_move` | 0 | 0–0.05 |
| `scale` | {Pythagorean / 12-TET} | {just pentatonic / Pythagorean / Ptolemy major / 12-TET} |
| `purity` | 0.3–0.7 | 0.6–0.9 |
| `brain_layers` | 0.8–1 | 0.4–0.7 |
| `brain_silence` | 0.2–0.4 | 0.1–0.3 |

#### BJNilsen

Feldaufnahme mit Ton: Quinten, inharmonische Quellen, Ereignisse in Böen, viel Stille.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 1–2 | 1–3 |
| `brain_low` | 31–38 MIDI | 36–43 MIDI |
| `brain_consonance` | 0.85–1 | 0.8–0.95 |
| `brain_timbre` | 0.4–0.7 | 0.1–0.3 |
| `brain_spacing` | 0.5–0.9 | 0.6–0.9 |
| `brain_cascade` | 0.2–0.4 | 0 |
| `scale` | {Pythagorean / 12-TET} | {just pentatonic / Pythagorean / Ptolemy major / 12-TET} |
| `purity` | 0.4–0.8 | 0.6–0.9 |
| `brain_silence` | 0.15–0.3 | 0.1–0.3 |

#### Francisco López

Der Conductor als Abwesenheit: eine Stimme, sehr leise, alles fest, Stille als Hauptereignis.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 1 | 1–3 |
| `brain_rate` | log 250–300 s | log 90–200 s |
| `brain_hold_min` | 300–600 s | 120–300 s |
| `brain_hold_max` | 600 s | 400–600 s |
| `brain_consonance` | 1 | 0.8–0.95 |
| `brain_wander` | 0 | 0–0.1 |
| `brain_spread` | 0.8–1 | 0.3–0.5 |
| `brain_bias` | -0.5–-0.2 | -0.2–0.1 |
| `auto_root_move` | 0 | 0–0.05 |
| `scale` | {12-TET / Pythagorean} | {just pentatonic / Pythagorean / Ptolemy major / 12-TET} |
| `purity` | 0.5–1 | 0.6–0.9 |
| `brain_layers` | 1 | 0.4–0.7 |
| `brain_silence` | 0.3–0.5 | 0.1–0.3 |

#### Chris Watson

Der Ort ist das Instrument: eine Stimme, sehr leise, Stille, Böen wie in der Natur.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 1 | 1–3 |
| `brain_rate` | log 250–300 s | log 90–200 s |
| `brain_hold_min` | 300–600 s | 120–300 s |
| `brain_hold_max` | 600 s | 400–600 s |
| `brain_consonance` | 0.9–1 | 0.8–0.95 |
| `brain_wander` | 0 | 0–0.1 |
| `brain_cascade` | 0.3–0.6 | 0 |
| `brain_bias` | -0.5–-0.2 | -0.2–0.1 |
| `auto_root_move` | 0 | 0–0.05 |
| `purity` | 0.5–0.9 | 0.6–0.9 |
| `brain_layers` | 1 | 0.4–0.7 |
| `brain_silence` | 0.3–0.5 | 0.1–0.3 |

#### Bass Communion

Wärmer und voller als die Familie: Streicherakkorde, Terzen, Akkordmodus mit kleinem Voice Leading.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 3–5 | 1–3 |
| `brain_rate` | log 60–150 s | log 90–200 s |
| `brain_hold_min` | 120–240 s | 120–300 s |
| `brain_hold_max` | 300–600 s | 400–600 s |
| `brain_high` | 72–84 MIDI | 67–76 MIDI |
| `brain_smooth` | 0.8–1 | 0.6–0.9 |
| `brain_cascade` | 0.1–0.3 | 0 |
| `auto_mode` | {Free 40 % / Chords 60 %} | {Free 70 % / Chords 30 %} |
| `auto_lead` | 1–3 st | 1–3 st |
| `scale` | {Ptolemy major / just minor / 12-TET} | {just pentatonic / Pythagorean / Ptolemy major / 12-TET} |
| `purity` | 0.5–0.8 | 0.6–0.9 |
| `purity_drift` | 0.1–0.3 | 0–0.1 |
| `brain_layers` | 0.3–0.6 | 0.4–0.7 |
| `brain_thirds` | 0.1–0.4 | -0.4–0 |

#### Biosphere

Wärmer als die Familie: Moll und Dur, temperiert erlaubt, Ringe, manchmal ein Raster, kürzere Haltezeiten.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 2–4 | 1–3 |
| `brain_rate` | log 45–120 s | log 90–200 s |
| `brain_hold_min` | 60–180 s | 120–300 s |
| `brain_hold_max` | 200–400 s | 400–600 s |
| `brain_low` | 31–40 MIDI | 36–43 MIDI |
| `brain_high` | 72–84 MIDI | 67–76 MIDI |
| `brain_quantize` | {Free 60 % / 8 bars 20 % / 16 bars 20 %} | Free |
| `brain_key` | 0.5–0.8 | 0.2–0.4 |
| `brain_cascade` | 0.1–0.3 | 0 |
| `brain_dejavu` | 0.4–0.7 | 0.3–0.5 |
| `brain_loop` | 4–8 | 3–6 |
| `scale` | {just minor / Ptolemy major / 12-TET / just pentatonic} | {just pentatonic / Pythagorean / Ptolemy major / 12-TET} |
| `purity` | 0.4–0.8 | 0.6–0.9 |
| `brain_layers` | 0.4–0.7 | 0.4–0.7 |
| `brain_thirds` | 0–0.4 | -0.4–0 |
| `brain_silence` | 0.1–0.2 | 0.1–0.3 |


### Familie *brit* — Britische Drone-Tradition

#### Andrew Chalk

Wärmer und höher als die Familie, Terzen als Farbe, Stimmung, die atmet, starkes Zentrum.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 2–4 | 1–3 |
| `brain_rate` | log 120–240 s | log 150–300 s |
| `brain_high` | 72–79 MIDI | 64–72 MIDI |
| `brain_key` | 0.7–0.9 | 0.5–0.8 |
| `brain_smooth` | 1 | 0.7–1 |
| `scale` | {Ptolemy major / just pentatonic} | {Ptolemy major / just minor / 12-TET / just pentatonic} |
| `purity` | 0.7–0.95 | 0.5–0.9 |
| `purity_drift` | 0.2–0.4 | 0.1–0.3 |
| `brain_thirds` | 0.2–0.5 | -0.2–0.2 |

#### Jonathan Coleclough

Mechanisch und akustisch: inharmonische Quellen, deshalb Timbre hoch; Ereignisse in Böen, Stille erlaubt.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 1–2 | 1–3 |
| `brain_consonance` | 0.85–1 | 0.75–0.9 |
| `brain_timbre` | 0.5–0.8 | 0.2–0.4 |
| `brain_spacing` | 0.6–0.9 | 0.3–0.6 |
| `brain_cascade` | 0.2–0.4 | 0 |
| `scale` | {12-TET / Pythagorean} | {Ptolemy major / just minor / 12-TET / just pentatonic} |
| `purity` | 0.4–0.8 | 0.5–0.9 |
| `brain_retrigger` | 30–60 s | 90–120 s |
| `brain_silence` | 0.1–0.2 | 0–0.1 |

#### Mirror

Geisterhaftes Harmonium: hoch, dünn, glasig; Timbre hört die Inharmonizität, Stimmung wobbelt.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 3–5 | 1–3 |
| `brain_high` | 79–88 MIDI | 64–72 MIDI |
| `brain_timbre` | 0.4–0.7 | 0.2–0.4 |
| `scale` | {Ptolemy major / 12-TET} | {Ptolemy major / just minor / 12-TET / just pentatonic} |
| `purity` | 0.5–0.8 | 0.5–0.9 |
| `purity_drift` | 0.2–0.4 | 0.1–0.3 |
| `brain_layers` | 0.2–0.5 | 0.3–0.6 |
| `brain_top_soft` | 0.2–0.4 | 0.3–0.6 |
| `brain_thirds` | 0.1–0.4 | -0.2–0.2 |

#### Mimir

Verzogene Schleifen: Deja Vu hoch, kurze Ringe, manchmal ein Raster, die Stimmung eiert.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 2–4 | 1–3 |
| `brain_rate` | log 45–120 s | log 150–300 s |
| `brain_quantize` | {Free 50 % / 4 bars 25 % / 8 bars 25 %} | Free |
| `brain_dejavu` | 0.6–0.9 | 0.4–0.7 |
| `brain_loop` | 2–6 | 3–5 |
| `brain_spread` | 0.5–0.8 | 0.2–0.4 |
| `scale` | {12-TET / Ptolemy major} | {Ptolemy major / just minor / 12-TET / just pentatonic} |
| `purity` | 0.3–0.6 | 0.5–0.9 |
| `purity_drift` | 0.3–0.5 | 0.1–0.3 |
| `brain_thirds` | 0–0.4 | -0.2–0.2 |

#### In Camera

Kleine dunkle Räume: alles nah, wenige Stimmen, mittleres Register, leiser als die Familie.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 1–3 | 1–3 |
| `brain_rate` | log 120–300 s | log 150–300 s |
| `brain_low` | 36–43 MIDI | 40–48 MIDI |
| `brain_high` | 60–67 MIDI | 64–72 MIDI |
| `brain_consonance` | 0.8–0.95 | 0.75–0.9 |
| `brain_bias` | -0.3–0 | 0–0.2 |
| `scale` | {12-TET / just minor} | {Ptolemy major / just minor / 12-TET / just pentatonic} |
| `purity` | 0.3–0.6 | 0.5–0.9 |
| `brain_silence` | 0.1–0.2 | 0–0.1 |
| `layer_depth` | 0–0.3 | 0.3–0.6 |

#### Colin Potter

Bandschleifen: Erinnerung hoch, Böen, gelegentlich auf Takte, die Stimmung treibt.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 2–4 | 1–3 |
| `brain_rate` | log 60–150 s | log 150–300 s |
| `brain_quantize` | {Free 60 % / 8 bars 40 %} | Free |
| `brain_cascade` | 0.2–0.4 | 0 |
| `brain_dejavu` | 0.5–0.8 | 0.4–0.7 |
| `brain_loop` | 3–8 | 3–5 |
| `scale` | {12-TET / Pythagorean / just minor} | {Ptolemy major / just minor / 12-TET / just pentatonic} |
| `purity` | 0.4–0.7 | 0.5–0.9 |
| `purity_drift` | 0.2–0.4 | 0.1–0.3 |

#### Darren Tate

Gestrichenes Metall: ein bis zwei Stimmen, Timbre hört das Metall, weite Abstände, sehr lange Töne.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 1–2 | 1–3 |
| `brain_rate` | log 150–300 s | log 150–300 s |
| `brain_hold_min` | 300–600 s | 300–600 s |
| `brain_hold_max` | 600 s | 600 s |
| `brain_consonance` | 0.8–0.95 | 0.75–0.9 |
| `brain_timbre` | 0.4–0.7 | 0.2–0.4 |
| `brain_spacing` | 0.5–0.8 | 0.3–0.6 |
| `scale` | {Pythagorean / just minor} | {Ptolemy major / just minor / 12-TET / just pentatonic} |
| `purity` | 0.6–0.9 | 0.5–0.9 |

#### ora

Das Leiseste der Familie: ein bis zwei Stimmen, an der Haltegrenze, pentatonisch, unter der Aufmerksamkeit.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 1–2 | 1–3 |
| `brain_rate` | log 200–300 s | log 150–300 s |
| `brain_hold_min` | 400–600 s | 300–600 s |
| `brain_hold_max` | 600 s | 600 s |
| `brain_consonance` | 0.9–1 | 0.75–0.9 |
| `brain_bias` | -0.2–0 | 0–0.2 |
| `scale` | {just pentatonic / Ptolemy major} | {Ptolemy major / just minor / 12-TET / just pentatonic} |
| `purity` | 0.6–0.9 | 0.5–0.9 |
| `brain_silence` | 0.05–0.15 | 0–0.1 |

#### Monos

Dunkler als Chalk allein: Moll, mittleres Register, leichte Böen, Stimmung treibt.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 1–3 | 1–3 |
| `brain_rate` | log 120–240 s | log 150–300 s |
| `brain_low` | 36–40 MIDI | 40–48 MIDI |
| `brain_high` | 60–67 MIDI | 64–72 MIDI |
| `brain_cascade` | 0.1–0.3 | 0 |
| `scale` | {just minor / Pythagorean} | {Ptolemy major / just minor / 12-TET / just pentatonic} |
| `purity` | 0.5–0.8 | 0.5–0.9 |
| `purity_drift` | 0.2–0.4 | 0.1–0.3 |

#### Paul Bradley

Ein Ton. Eine Stimme vorn, eine hinten, nichts wandert, nichts driftet, langes Gedächtnis.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 1 | 1–3 |
| `brain_rate` | log 250–300 s | log 150–300 s |
| `brain_hold_min` | 600 s | 300–600 s |
| `brain_hold_max` | 600 s | 600 s |
| `brain_wander` | 0 | 0 |
| `auto_root_move` | 0 | 0–0.03 |
| `brain2_on` | on | {on 40 % / off 60 %} |
| `brain2_density` | 1 | 1 |
| `scale` | {Pythagorean / Ptolemy major} | {Ptolemy major / just minor / 12-TET / just pentatonic} |
| `purity` | 0.9–1 | 0.5–0.9 |
| `purity_drift` | 0–0.05 | 0.1–0.3 |
| `brain_silence` | 0 | 0–0.1 |
| `brain_memory` | 20–30 min | 10–20 min |


### Familie *luminous* — Leuchtend und konsonant

#### Brian Eno

Wenige Stimmen, temperiert oder fast, Figuren, die als Erinnerung wiederkehren und langsam mutieren.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 2–4 | 3–5 |
| `brain_rate` | log 45–90 s | log 30–75 s |
| `brain_dejavu` | 0.4–0.7 | 0.2–0.4 |
| `brain_loop` | 5–9 | 4–8 |
| `auto_mode` | {Free 60 % / Chords 40 %} | {Free 20 % / Chords 80 %} |
| `scale` | {12-TET / Ptolemy major / just pentatonic} | {Ptolemy major / 12-TET / just pentatonic / 7-limit} |
| `purity` | 0.4–0.8 | 0.6–0.9 |
| `brain_thirds` | 0.4–0.7 | 0.4–0.8 |
| `brain_memory` | 6–8 min | 6–10 min |

#### Stars of the Lid

Konsonante Streicherschwellungen: Akkorde kommen zusammen (Blend hoch), Terzen, Diatonik, das Zentrum stark.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 3–5 | 3–5 |
| `brain_hold_min` | 60–120 s | 45–90 s |
| `brain_hold_max` | 150–300 s | 150–300 s |
| `brain_key` | 0.7–0.9 | 0.6–0.9 |
| `brain_even` | 0.6–0.9 | 0.5–0.8 |
| `brain_smooth` | 1 | 0.8–1 |
| `brain_blend` | 0.7–1 | 0–0.2 |
| `auto_mode` | Chords | {Free 20 % / Chords 80 %} |
| `auto_rate` | log 45–120 s | log 30–120 s |
| `auto_lead` | 1–3 st | 2–4 st |
| `auto_root_move` | 0.2–0.4 | 0.15–0.35 |
| `scale` | {Ptolemy major / 12-TET} | {Ptolemy major / 12-TET / just pentatonic / 7-limit} |
| `purity` | 0.5–0.85 | 0.6–0.9 |
| `brain_thirds` | 0.5–0.9 | 0.4–0.8 |
| `brain_root_steps` | Diatonic | Diatonic |
| `env_vel_attack` | 0.5–0.8 | 0.2–0.5 |

#### Tim Hecker

Die Regelverletzung als Stil: Cluster im Körper, Abstandsregel aus, Onset Guard aus, dicht und laut.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 5–8 | 3–5 |
| `brain_rate` | log 20–60 s | log 30–75 s |
| `brain_hold_min` | 30–90 s | 45–90 s |
| `brain_hold_max` | 120–240 s | 150–300 s |
| `brain_consonance` | 0.3–0.6 | 0.6–0.85 |
| `brain_spacing` | -0.8–-0.3 | 0.2–0.5 |
| `brain_blend` | 0.3–0.7 | 0–0.2 |
| `brain_cascade` | 0.2–0.5 | 0–0.1 |
| `brain_surprise` | 0.5–0.65 | 0.4–0.55 |
| `brain_bias` | 0.2–0.5 | -0.1–0.1 |
| `scale` | {12-TET / Ptolemy major} | {Ptolemy major / 12-TET / just pentatonic / 7-limit} |
| `purity` | 0.3–0.7 | 0.6–0.9 |
| `purity_drift` | 0.3–0.5 | 0.05–0.2 |
| `brain_layers` | 0.1–0.4 | 0.3–0.6 |
| `brain_low_spacing` | 0–0.3 | 0.4–0.7 |
| `brain_third_floor` | 0 MIDI | 43–48 MIDI |
| `brain_thirds` | 0.2–0.6 | 0.4–0.8 |
| `brain_seconds` | 0.4–0.8 | 0.1–0.4 |
| `brain_onset_guard` | off | on |


### Familie *space* — Weltraum und Berliner Schule

#### Michael Stearns

Die Obertonreihe als Thema, sehr hohe Luftschicht, alles auf einen Grundton bezogen.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 4–6 | 3–5 |
| `brain_high` | 91–100 MIDI | 88–96 MIDI |
| `brain_harmonic` | 0.7–0.9 | 0.5–0.8 |
| `scale` | {harmonic 8-16 / otonality 1-3-5-7-9-11} | {harmonic 8-16 23 % / otonality 1-3-5-7-9-11 23 % / Ptolemy major 23 % / 7-limit 23 % / Bohlen-Pierce 7 %} |
| `brain_seventh` | 0.5–0.9 | 0.4–0.8 |
| `brain2_interval` | {Fifth / Octave} | {Fifth / Octave / Seventh} |

#### S.E.T.I.

Signale aus dem Rauschen: wenige Stimmen, pythagoreisch oder fremd, Extreme in Lautstärke und Dauer, Stille.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 2–4 | 3–5 |
| `brain_high` | 79–88 MIDI | 88–96 MIDI |
| `brain_consonance` | 0.7–0.9 | 0.6–0.8 |
| `brain_quantize` | Free | {Free 70 % / 8 bars 15 % / 16 bars 15 %} |
| `brain_cascade` | 0.2–0.4 | 0.1–0.3 |
| `brain_dejavu` | 0.1–0.3 | 0.2–0.5 |
| `brain_spread` | 0.7–0.9 | 0.5–0.8 |
| `scale` | {Pythagorean 42 % / otonality 1-3-5-7-9-11 42 % / Bohlen-Pierce 17 %} | {harmonic 8-16 23 % / otonality 1-3-5-7-9-11 23 % / Ptolemy major 23 % / 7-limit 23 % / Bohlen-Pierce 7 %} |
| `purity` | 0.6–0.9 | 0.7–1 |
| `brain_silence` | 0.1–0.2 | 0–0.1 |

#### Bad Sector

Kalte Wissenschaft: Bohlen-Pierce häufig, Konsonanz locker, Cluster erlaubt, Böen, Überraschung hoch.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 2–4 | 3–5 |
| `brain_low` | 28–36 MIDI | 31–38 MIDI |
| `brain_consonance` | 0.5–0.75 | 0.6–0.8 |
| `brain_timbre` | 0.4–0.7 | 0.1–0.3 |
| `brain_spacing` | -0.2–0.3 | 0.3–0.6 |
| `brain_cascade` | 0.3–0.6 | 0.1–0.3 |
| `brain_surprise` | 0.5–0.65 | 0.45–0.6 |
| `brain_spread` | 0.7–0.9 | 0.5–0.8 |
| `brain_bias` | 0.2–0.4 | 0–0.2 |
| `scale` | {Bohlen-Pierce 14 % / Pythagorean 29 % / 12-TET 29 % / otonality 1-3-5-7-9-11 29 %} | {harmonic 8-16 23 % / otonality 1-3-5-7-9-11 23 % / Ptolemy major 23 % / 7-limit 23 % / Bohlen-Pierce 7 %} |
| `brain_silence` | 0.1–0.25 | 0–0.1 |

#### Martin Stürtzer

Berliner Erbe: oft ein Raster, temperiert bis halbrein, Terzen erlaubt, lange Ringe, Akkordmodus.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 3–5 | 3–5 |
| `brain_rate` | log 30–60 s | log 30–75 s |
| `brain_low` | 31–36 MIDI | 31–38 MIDI |
| `brain_high` | 84–91 MIDI | 88–96 MIDI |
| `brain_quantize` | {Free 50 % / 8 bars 25 % / 16 bars 25 %} | {Free 70 % / 8 bars 15 % / 16 bars 15 %} |
| `brain_dejavu` | 0.3–0.6 | 0.2–0.5 |
| `brain_loop` | 8–16 | 6–12 |
| `auto_mode` | {Free 30 % / Chords 70 %} | {Free / Chords} |
| `auto_lead` | 2–5 st | 3–7 st |
| `scale` | {Ptolemy major / just minor / 12-TET / harmonic 8-16} | {harmonic 8-16 23 % / otonality 1-3-5-7-9-11 23 % / Ptolemy major 23 % / 7-limit 23 % / Bohlen-Pierce 7 %} |
| `purity` | 0.5–0.8 | 0.7–1 |
| `brain_thirds` | 0.2–0.5 | 0–0.4 |

#### Phelios

Stürtzers dunklere Seite: Moll und Pythagoräisch, keine Terzen, tieferes Register, Böen.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 2–4 | 3–5 |
| `brain_high` | 72–84 MIDI | 88–96 MIDI |
| `brain_quantize` | {Free 60 % / 8 bars 20 % / 16 bars 20 %} | {Free 70 % / 8 bars 15 % / 16 bars 15 %} |
| `brain_cascade` | 0.1–0.3 | 0.1–0.3 |
| `brain_dejavu` | 0.3–0.5 | 0.2–0.5 |
| `brain_loop` | 8–16 | 6–12 |
| `scale` | {just minor / Pythagorean / harmonic 8-16} | {harmonic 8-16 23 % / otonality 1-3-5-7-9-11 23 % / Ptolemy major 23 % / 7-limit 23 % / Bohlen-Pierce 7 %} |
| `purity` | 0.6–0.9 | 0.7–1 |
| `brain_thirds` | -0.2–0.2 | 0–0.4 |

#### Tholen

Kalte Science-Fiction: Bohlen-Pierce oft, nichts Organisches, keine Terzen, Extreme.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 2–4 | 3–5 |
| `brain_low` | 28–36 MIDI | 31–38 MIDI |
| `brain_high` | 79–91 MIDI | 88–96 MIDI |
| `brain_consonance` | 0.6–0.85 | 0.6–0.8 |
| `brain_timbre` | 0.3–0.6 | 0.1–0.3 |
| `brain_cascade` | 0.1–0.3 | 0.1–0.3 |
| `brain_spread` | 0.6–0.85 | 0.5–0.8 |
| `scale` | {Bohlen-Pierce 14 % / Pythagorean 29 % / otonality 1-3-5-7-9-11 29 % / 12-TET 29 %} | {harmonic 8-16 23 % / otonality 1-3-5-7-9-11 23 % / Ptolemy major 23 % / 7-limit 23 % / Bohlen-Pierce 7 %} |
| `purity` | 0.5–0.8 | 0.7–1 |
| `brain_thirds` | -0.4–0 | 0–0.4 |

#### Ian Boddy

Modular und Berliner Schule: fast immer ein Raster, Akkordmodus auf Takte, temperiert, Guard aus.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 3–5 | 3–5 |
| `brain_rate` | log 20–45 s | log 30–75 s |
| `brain_hold_min` | 30–90 s | 60–150 s |
| `brain_hold_max` | 120–240 s | 200–400 s |
| `brain_quantize` | {8 bars 40 % / 16 bars 30 % / 4 bars 15 % / Free 15 %} | {Free 70 % / 8 bars 15 % / 16 bars 15 %} |
| `brain_key` | 0.6–0.9 | 0.4–0.7 |
| `brain_even` | 0.5–0.8 | 0.3–0.5 |
| `brain_blend` | 0.3–0.8 | 0–0.2 |
| `brain_dejavu` | 0.4–0.7 | 0.2–0.5 |
| `brain_loop` | 4–8 | 6–12 |
| `auto_mode` | {Free 20 % / Chords 80 %} | {Free / Chords} |
| `auto_sync` | {8 bars 40 % / 16 bars 30 % / 4 bars 30 %} | Free |
| `auto_lead` | 2–5 st | 3–7 st |
| `auto_root_move` | 0.15–0.3 | 0.1–0.25 |
| `scale` | {12-TET / Ptolemy major / just minor} | {harmonic 8-16 23 % / otonality 1-3-5-7-9-11 23 % / Ptolemy major 23 % / 7-limit 23 % / Bohlen-Pierce 7 %} |
| `purity` | 0.3–0.7 | 0.7–1 |
| `brain_thirds` | 0.2–0.5 | 0–0.4 |
| `brain_onset_guard` | off | on |

#### Jeff Greinke

Atmosphärisch und mild: Terzen, Zentrum, Akkordmodus, gelegentlich ein Raster, hohe Luft.

| Parameter | Bereich | Familie |
|---|---|---|
| `brain_density` | 3–5 | 3–5 |
| `brain_high` | 84–91 MIDI | 88–96 MIDI |
| `brain_quantize` | {Free 70 % / 8 bars 30 %} | {Free 70 % / 8 bars 15 % / 16 bars 15 %} |
| `brain_timbre` | 0.2–0.5 | 0.1–0.3 |
| `brain_key` | 0.5–0.8 | 0.4–0.7 |
| `brain_cascade` | 0.1–0.3 | 0.1–0.3 |
| `auto_mode` | {Free 40 % / Chords 60 %} | {Free / Chords} |
| `auto_lead` | 2–4 st | 3–7 st |
| `scale` | {Ptolemy major / 12-TET / just pentatonic} | {harmonic 8-16 23 % / otonality 1-3-5-7-9-11 23 % / Ptolemy major 23 % / 7-limit 23 % / Bohlen-Pierce 7 %} |
| `purity` | 0.5–0.8 | 0.7–1 |
| `brain_thirds` | 0.2–0.5 | 0–0.4 |
