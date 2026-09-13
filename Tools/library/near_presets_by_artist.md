# Near-Presets je Künstler — welche, wie oft

Stand: `Noctuary-near/Core/src/NearPresets.inc`, 107 Near-Presets in 12 Familien (Flutes, Bowls, Water, Signals, Bells, Radio, Library, Voices, Ice, Strings, Sequences, Archive), zusammengefasst zu 35 Gruppen. Ein Pack-Preset trägt genau ein Near-Preset, also ist die Tabelle ein Zuggewicht pro Künstler, kein Stundenbudget.

Drei Größen pro Künstler:

- **Share** — Anteil seiner Presets, die überhaupt eine Nah-Ebene bekommen.
- **Gewicht** (1–3) — wie oft eine Gruppe gezogen wird, relativ zu den anderen Gruppen des Künstlers; innerhalb der Gruppe gleichverteilt.
- **Klasse** — wie oft das Ereignis dann *auftritt*, als Faktor auf das `fore_rate` des Near-Presets, damit ein Sonar-Ping ein Ping bleibt und eine Glocke eine Glocke: **H** häufig ×0,6–1,0 · **G** gelegentlich ×1,0–2,0 · **S** selten ×2,0–4,0 (in 10–900 s geklemmt).

Die JSON enthält pro Künstler jedes Near-Preset mit normiertem Gewicht und dem fertigen `fore_rate`-Bereich in Sekunden; die CSV ist die Matrix Künstler × Preset mit `Gewicht+Klasse` in der Zelle.


## Gruppen

| Gruppe | Near-Presets |
|---|---|
| **Flöte Einzelton** (`flute_note`) | Bamboo Breath, Airy Flute, Overblown, Night Flute |
| **Flöte Phrase** (`flute_phrase`) | Flute Phrase, Shakuhachi |
| **Eule und fernes Heulen** (`flute_animal`) | Night Owl, Distant Howl |
| **Klangschale** (`bowl`) | Singing Bowl, Rim Rub, Deep Bowl, Bowl Beating, Bowl Phrase |
| **Wassertropfen** (`drops`) | Cave Drops, Cistern, Wet Marimba, Steady Drip, Rain In A Bowl |
| **Sonar** (`sonar`) | Sonar Ping, Echo Sounder, Deep Sonar |
| **Nebelhorn** (`foghorn`) | Foghorn |
| **Whistler (VLF)** (`whistler`) | Whistler |
| **Rassel** (`shaker`) | Seed Pod |
| **Geigerzähler** (`geiger`) | Geiger Counter |
| **Leuchtstoffröhre** (`tube`) | Bunker Tube |
| **Krell-Schaltkreis** (`krell`) | Krell Circuit |
| **Raumsonden-Bake** (`beacon`) | Deep Space Beacon |
| **Zahlensender** (`morse`) | Number Station |
| **Kurzwellenskala** (`dial`) | Shortwave Dial |
| **Kleine Glocken** (`bells_small`) | Ting-Sha, Ship's Bell |
| **Ferne Kirchenglocke** (`bell_far`) | Church Bell Far |
| **Windglocken** (`wind_chimes`) | Wind Chimes |
| **Radio-Clips (Quiet Please)** (`radio_clips`) | Quiet, Please, Night Announcer, Whisper At The Ear, Lost Transmission, Radio Cut-Up |
| **Archiv-Clips (LoC)** (`library_clips`) | Edison Cylinder, Variety Stage, Screening Room, New York, 1950s, Interview, Cylinder Cut-Up |
| **Murmelstimme** (`murmur`) | Radio Murmur, Ground Control, Whisper Close, Far Chatter, Low Voice |
| **Eis und altes Holz** (`ice`) | Ice Shift, Old Wood, Glacier, Hull Strain |
| **Gestrichene Saiten** (`strings_bowed`) | E-Bow Glide, Lap Steel Cry, Cello Under |
| **Gezupfte Saiten** (`strings_plucked`) | Koto, Harp Touch |
| **Sequenz Berliner Schule** (`seq_berlin`) | Phaedra, Rubycon, Slow Cathedral, Wavetable Run, Glass Pluck, Harmonic Pulse |
| **Sequenz Mallet und Glas** (`seq_mallet`) | Gamelan Line, Koto Line, Glass Balafon, Beating Bell |
| **Sequenz Vokalpuls** (`seq_vocal`) | Vocal Pulse |
| **Sequenz Puls** (`seq_pulse`) | Subterranean Pulse, Machine Pulse, Heartbeat |
| **Sequenz organisch** (`seq_organic`) | Flute Line, Bowl Line, Bowed Line, Drop Sequence, Ice Steps, Tuned Noise |
| **NASA Apollo · Mercury · JFK** (`archive_apollo`) | Houston, We've Had a Problem, The Eagle Has Landed, One Small Step, Merry Christmas from Apollo 8, Godspeed, John Glenn, Fireflies, We Choose the Moon |
| **NASA Shuttle-Funk** (`archive_shuttle`) | Roger Roll, Go at Throttle Up, Nice to Be in Orbit, Houston, Discovery, Dust It Off First |
| **NASA Beeps** (`archive_beeps`) | Quindar, Sputnik |
| **NASA Weltraumfunk** (`archive_space`) | Chorus, Saturn Radio, Enceladus, Jupiter Lightning, Kepler Star, Ganymede |
| **NASA Mars** (`archive_mars`) | Wind on Mars, Ingenuity, Dust Devil, Laser on Mars, Marsquake, Dinks and Donks |
| **NASA Webb-Sonifikationen** (`archive_webb`) | Cosmic Cliffs, Southern Ring |

## Künstler

| Künstler | Familie | Share | Gruppen (Gewicht + Klasse) |
|---|---|---|---|
| Robert Rich | sleep | 0.7 | Wassertropfen 3H, Flöte Einzelton 3H, Flöte Phrase 3H, Gestrichene Saiten 3H, Klangschale 2G, Rassel 1G, Gezupfte Saiten 1G, Windglocken 1G, Kleine Glocken 1S, Eule und fernes Heulen 1S, Sequenz Mallet und Glas 1S, Sequenz organisch 1S |
| Steve Roach | sleep | 0.6 | Wassertropfen 2G, Flöte Einzelton 2G, Sequenz Berliner Schule 2G, Sequenz Mallet und Glas 2G, Rassel 2G, Gezupfte Saiten 2G, Windglocken 2G, Flöte Phrase 1G, Sequenz organisch 1G, Kleine Glocken 1S, Klangschale 1S, Eule und fernes Heulen 1S, Sequenz Vokalpuls 1S, Gestrichene Saiten 1S, Whistler (VLF) 1S |
| Klaus Wiese | sleep | 0.5 | Klangschale 3H, Kleine Glocken 2G, Flöte Einzelton 2G, Windglocken 1G, Flöte Phrase 1S, Sequenz organisch 1S, Rassel 1S, Gezupfte Saiten 1S |
| Oöphoi | sleep | 0.35 | Klangschale 2G, Wassertropfen 1S, Flöte Einzelton 1S, Sequenz organisch 1S, Gestrichene Saiten 1S, Windglocken 1S |
| Mathias Grassow | sleep | 0.35 | Klangschale 2G, Ferne Kirchenglocke 1S, Kleine Glocken 1S, Flöte Einzelton 1S, Sequenz organisch 1S, Gestrichene Saiten 1S |
| Jim Cole | sleep | 0.6 | Klangschale 2G, Kleine Glocken 1G, Flöte Einzelton 1S, Windglocken 1S |
| Voice of Eye | sleep | 0.6 | Rassel 3H, Kleine Glocken 2G, Klangschale 2G, Wassertropfen 1G, Eule und fernes Heulen 1G, Murmelstimme 1G, Sequenz organisch 1G, Eis und altes Holz 1S, Sequenz Vokalpuls 1S, Gezupfte Saiten 1S |
| Tom Heasley | sleep | 0.6 | Nebelhorn 2G, Gestrichene Saiten 1G, Ferne Kirchenglocke 1S, Klangschale 1S, Flöte Einzelton 1S |
| Jeff Pearce | sleep | 0.6 | Gestrichene Saiten 3H, Gezupfte Saiten 2G, Sequenz Berliner Schule 1S, Sequenz organisch 1S, Windglocken 1S |
| Amir Baghiri | sleep | 0.6 | Rassel 3H, Flöte Einzelton 2G, Sequenz Mallet und Glas 2G, Gezupfte Saiten 2G, Windglocken 2G, Kleine Glocken 1G, Wassertropfen 1G, Flöte Phrase 1G, Sequenz organisch 1G, Sequenz Vokalpuls 1S |
| Vidna Obmana | sleep | 0.6 | Wassertropfen 2G, Flöte Einzelton 2G, Sequenz Mallet und Glas 2G, Sequenz organisch 2G, Gezupfte Saiten 2G, Windglocken 2G, Flöte Phrase 1G, Rassel 1G, Klangschale 1S |
| Lustmord | deep | 0.35 | Wassertropfen 2G, Sequenz Puls 2G, NASA Mars 1S, NASA Weltraumfunk 1S, Ferne Kirchenglocke 1S, Nebelhorn 1S, Geigerzähler 1S, Eis und altes Holz 1S, Murmelstimme 1S, Sonar 1S, Leuchtstoffröhre 1S |
| Sleep Research Facility | deep | 0.35 | Sequenz Puls 3H, Sonar 2G, Leuchtstoffröhre 2G, Nebelhorn 1G, Geigerzähler 1G, NASA Mars 1S, NASA Weltraumfunk 1S, Kurzwellenskala 1S, Eis und altes Holz 1S, Radio-Clips (Quiet Please) 1S |
| Inade | deep | 0.35 | Kleine Glocken 2G, Sequenz Puls 2G, Ferne Kirchenglocke 1G, Murmelstimme 1G, Wassertropfen 1S, Geigerzähler 1S, Eis und altes Holz 1S, Krell-Schaltkreis 1S, Rassel 1S, Leuchtstoffröhre 1S |
| Kammarheit | deep | 0.35 | Wassertropfen 2G, NASA Mars 1S, Ferne Kirchenglocke 1S, Eis und altes Holz 1S, Sequenz Puls 1S, Sonar 1S, Leuchtstoffröhre 1S |
| Gustaf Hildebrand | deep | 0.35 | Ferne Kirchenglocke 1G, Wassertropfen 1G, Nebelhorn 1G, Sequenz Puls 1G, NASA Weltraumfunk 1S, Murmelstimme 1S, Sequenz Berliner Schule 1S, Sonar 1S, Gestrichene Saiten 1S |
| Atomine Elektrine | deep | 0.35 | NASA Weltraumfunk 2G, Raumsonden-Bake 2G, Whistler (VLF) 2G, NASA Webb-Sonifikationen 1G, Kurzwellenskala 1G, Krell-Schaltkreis 1G, NASA Mars 1S, Zahlensender 1S, Sequenz Berliner Schule 1S, Sequenz Puls 1S |
| Arecibo | deep | 0.55 | NASA Weltraumfunk 3H, Raumsonden-Bake 3H, NASA Beeps 2G, NASA Webb-Sonifikationen 2G, Kurzwellenskala 2G, Zahlensender 2G, Whistler (VLF) 2G, Geigerzähler 1G, Krell-Schaltkreis 1S, Sequenz Puls 1S, Sonar 1S |
| Tho-So-Aa | deep | 0.35 | Sequenz Puls 2G, Geigerzähler 1G, Leuchtstoffröhre 1G, Ferne Kirchenglocke 1S, Wassertropfen 1S, Eis und altes Holz 1S, Murmelstimme 1S, Rassel 1S |
| Ulf Söderberg | deep | 0.35 | Wassertropfen 2G, Ferne Kirchenglocke 1G, Eis und altes Holz 1G, Kleine Glocken 1S, Eule und fernes Heulen 1S, Nebelhorn 1S, Murmelstimme 1S, Sequenz Puls 1S |
| Moljebka Pvlse | deep | 0.35 | Sequenz Puls 2G, Geigerzähler 1G, Eis und altes Holz 1G, Leuchtstoffröhre 1G, Kleine Glocken 1S, Kurzwellenskala 1S, Archiv-Clips (LoC) 1S, Murmelstimme 1S, Rassel 1S |
| Apoptose | deep | 0.35 | Kleine Glocken 2G, Murmelstimme 1G, Sequenz Puls 1G, Rassel 1G, Wassertropfen 1S, Eis und altes Holz 1S, Archiv-Clips (LoC) 1S, Leuchtstoffröhre 1S |
| Land:Fire | deep | 0.35 | Sequenz Puls 2G, Geigerzähler 1G, Leuchtstoffröhre 1G, Ferne Kirchenglocke 1S, Kurzwellenskala 1S, Eis und altes Holz 1S, Murmelstimme 1S, Rassel 1S |
| Polygon | deep | 0.35 | Sequenz Puls 1G, Ferne Kirchenglocke 1S, Wassertropfen 1S, Eis und altes Holz 1S, Sonar 1S, Leuchtstoffröhre 1S |
| Raison d'Être | ritual | 0.4 | Ferne Kirchenglocke 2G, Kleine Glocken 2G, Wassertropfen 2G, Murmelstimme 2G, Klangschale 1G, Eule und fernes Heulen 1S, Eis und altes Holz 1S, Archiv-Clips (LoC) 1S, Sequenz Puls 1S, Sequenz Vokalpuls 1S |
| Deutsch Nepal | ritual | 0.4 | Archiv-Clips (LoC) 2G, Murmelstimme 2G, Sequenz Puls 2G, Kleine Glocken 1G, Radio-Clips (Quiet Please) 1G, Ferne Kirchenglocke 1S, Geigerzähler 1S, Rassel 1S, Leuchtstoffröhre 1S |
| Troum | ritual | 0.4 | Archiv-Clips (LoC) 2G, Kleine Glocken 1G, Murmelstimme 1G, Gestrichene Saiten 1G, Klangschale 1S, Wassertropfen 1S, Eis und altes Holz 1S, Radio-Clips (Quiet Please) 1S, Sequenz organisch 1S, Rassel 1S |
| Biosphere | cold | 0.4 | Eis und altes Holz 3H, Radio-Clips (Quiet Please) 2G, Kurzwellenskala 1G, Wassertropfen 1G, Archiv-Clips (LoC) 1G, Sequenz Mallet und Glas 1G, Sonar 1G, Gezupfte Saiten 1G, Whistler (VLF) 1G, NASA Mars 1S, NASA Weltraumfunk 1S, Kleine Glocken 1S, Zahlensender 1S, Murmelstimme 1S, Sequenz Berliner Schule 1S |
| Thomas Köner | cold | 0.3 | Eis und altes Holz 3H, Ferne Kirchenglocke 2G, Nebelhorn 1G, NASA Mars 1S, Kurzwellenskala 1S, Wassertropfen 1S, Sequenz Puls 1S, Sonar 1S |
| Loscil | cold | 0.25 | Sequenz Mallet und Glas 1G, Gestrichene Saiten 1G, Gezupfte Saiten 1G, Kleine Glocken 1S, Wassertropfen 1S, Nebelhorn 1S, Eis und altes Holz 1S, Sequenz Berliner Schule 1S, Sequenz Puls 1S, Sonar 1S |
| BJNilsen | cold | 0.25 | Kurzwellenskala 2G, Eis und altes Holz 2G, Geigerzähler 1G, Leuchtstoffröhre 1G, Whistler (VLF) 1G, NASA Mars 1S, Ferne Kirchenglocke 1S, Wassertropfen 1S, Nebelhorn 1S, Zahlensender 1S, Rassel 1S |
| Hazard | cold | 0.25 | Kurzwellenskala 2G, Geigerzähler 2G, Leuchtstoffröhre 2G, Eis und altes Holz 1G, Zahlensender 1G, Sequenz Puls 1G, Whistler (VLF) 1G, NASA Mars 1S, Raumsonden-Bake 1S, Sonar 1S |
| Paul Bradley | cold | 0.05 | Ferne Kirchenglocke 1S, Klangschale 1S, Gestrichene Saiten 1S |
| Francisco López | cold | 0.12 | Wassertropfen 1S, Geigerzähler 1S, Eis und altes Holz 1S, Rassel 1S |
| Chris Watson | cold | 0.2 | Wassertropfen 2G, Eis und altes Holz 2G, Eule und fernes Heulen 1G, Rassel 1G, NASA Mars 1S, Geigerzähler 1S, Windglocken 1S |
| Bass Communion | cold | 0.25 | Gestrichene Saiten 2G, Archiv-Clips (LoC) 1G, Radio-Clips (Quiet Please) 1G, Kleine Glocken 1S, Wassertropfen 1S, Eis und altes Holz 1S, Murmelstimme 1S, Sequenz Mallet und Glas 1S, Whistler (VLF) 1S |
| Andrew Chalk | brit | 0.2 | Gestrichene Saiten 2G, Windglocken 1G, Kleine Glocken 1S, Klangschale 1S, Wassertropfen 1S, Flöte Einzelton 1S, Archiv-Clips (LoC) 1S, Gezupfte Saiten 1S |
| Jonathan Coleclough | brit | 0.2 | Kleine Glocken 2G, Eis und altes Holz 2G, Ferne Kirchenglocke 1G, Wassertropfen 1G, Rassel 1G, Geigerzähler 1S, Archiv-Clips (LoC) 1S, Sequenz Puls 1S, Leuchtstoffröhre 1S |
| Mirror | brit | 0.2 | Ferne Kirchenglocke 1G, Kleine Glocken 1G, Klangschale 1G, Wassertropfen 1S, Archiv-Clips (LoC) 1S, Radio-Clips (Quiet Please) 1S, Sequenz Mallet und Glas 1S, Gestrichene Saiten 1S |
| Mimir | brit | 0.35 | Archiv-Clips (LoC) 3H, Radio-Clips (Quiet Please) 2G, Sequenz Mallet und Glas 1G, Gezupfte Saiten 1G, Kleine Glocken 1S, Kurzwellenskala 1S, Geigerzähler 1S, Murmelstimme 1S, Sequenz Berliner Schule 1S |
| In Camera | brit | 0.35 | Archiv-Clips (LoC) 2G, Murmelstimme 2G, Radio-Clips (Quiet Please) 2G, Kurzwellenskala 1G, Wassertropfen 1S, Geigerzähler 1S, Eis und altes Holz 1S, Gestrichene Saiten 1S, Leuchtstoffröhre 1S |
| Colin Potter | brit | 0.2 | Archiv-Clips (LoC) 2G, Radio-Clips (Quiet Please) 2G, Kurzwellenskala 1G, Geigerzähler 1G, Kleine Glocken 1S, Eis und altes Holz 1S, Murmelstimme 1S, Sequenz Berliner Schule 1S, Sequenz Puls 1S, Rassel 1S |
| Darren Tate | brit | 0.2 | Gestrichene Saiten 2G, Kleine Glocken 1G, Eis und altes Holz 1G, Ferne Kirchenglocke 1S, Klangschale 1S, Wassertropfen 1S, Rassel 1S |
| ora | brit | 0.1 | Ferne Kirchenglocke 1S, Klangschale 1S, Wassertropfen 1S, Gestrichene Saiten 1S, Windglocken 1S |
| Monos | brit | 0.2 | Eis und altes Holz 1G, Gestrichene Saiten 1G, Ferne Kirchenglocke 1S, Wassertropfen 1S, Archiv-Clips (LoC) 1S, Murmelstimme 1S |
| Brian Eno | luminous | 0.4 | Gezupfte Saiten 2G, Kleine Glocken 1G, Sequenz Mallet und Glas 1G, Gestrichene Saiten 1G, Wassertropfen 1S, Archiv-Clips (LoC) 1S, Radio-Clips (Quiet Please) 1S, Sequenz Berliner Schule 1S, Sequenz organisch 1S, Windglocken 1S |
| Stars of the Lid | luminous | 0.4 | Gestrichene Saiten 2G, Ferne Kirchenglocke 1S, Kleine Glocken 1S, Archiv-Clips (LoC) 1S, Radio-Clips (Quiet Please) 1S, Sequenz organisch 1S |
| Tim Hecker | luminous | 0.3 | Archiv-Clips (LoC) 2G, Kleine Glocken 1G, Radio-Clips (Quiet Please) 1G, Gestrichene Saiten 1G, Kurzwellenskala 1S, Geigerzähler 1S, Eis und altes Holz 1S, Murmelstimme 1S, Sequenz Berliner Schule 1S, Sequenz Mallet und Glas 1S |
| Michael Stearns | space | 0.55 | Sequenz Berliner Schule 2G, Windglocken 2G, Sequenz Mallet und Glas 1G, NASA Apollo · Mercury · JFK 1S, NASA Weltraumfunk 1S, Raumsonden-Bake 1S, Ferne Kirchenglocke 1S, Klangschale 1S, Flöte Einzelton 1S, Gestrichene Saiten 1S, Whistler (VLF) 1S |
| Martin Stürtzer | space | 0.7 | Sequenz Berliner Schule 3H, NASA Apollo · Mercury · JFK 2G, NASA Shuttle-Funk 2G, NASA Weltraumfunk 2G, Raumsonden-Bake 2G, NASA Beeps 1G, NASA Webb-Sonifikationen 1G, Murmelstimme 1G, Sequenz Mallet und Glas 1G, Whistler (VLF) 1G, Krell-Schaltkreis 1S, Zahlensender 1S, Radio-Clips (Quiet Please) 1S, Sonar 1S, Windglocken 1S |
| Phelios | space | 0.55 | NASA Weltraumfunk 2G, Sequenz Berliner Schule 2G, NASA Mars 1G, Raumsonden-Bake 1G, Sequenz Puls 1G, Whistler (VLF) 1G, NASA Apollo · Mercury · JFK 1S, Kurzwellenskala 1S, Krell-Schaltkreis 1S, Zahlensender 1S, Leuchtstoffröhre 1S |
| Ian Boddy | space | 0.7 | Sequenz Berliner Schule 3H, Sequenz Mallet und Glas 2G, Gezupfte Saiten 1G, NASA Beeps 1S, NASA Shuttle-Funk 1S, Kleine Glocken 1S, Krell-Schaltkreis 1S, Sequenz Vokalpuls 1S, Whistler (VLF) 1S, Windglocken 1S |
| Jeff Greinke | space | 0.55 | Sequenz Berliner Schule 2G, Sequenz Mallet und Glas 2G, Gestrichene Saiten 1G, Gezupfte Saiten 1G, Windglocken 1G, Kleine Glocken 1S, Wassertropfen 1S, Flöte Einzelton 1S, Radio-Clips (Quiet Please) 1S |
| Tholen | space | 0.55 | Raumsonden-Bake 2G, Krell-Schaltkreis 2G, NASA Weltraumfunk 1G, NASA Webb-Sonifikationen 1G, Kurzwellenskala 1G, Zahlensender 1G, Whistler (VLF) 1G, Geigerzähler 1S, Sequenz Berliner Schule 1S, Sonar 1S, Leuchtstoffröhre 1S |
| S.E.T.I. | space | 0.6 | NASA Weltraumfunk 3H, Raumsonden-Bake 3H, NASA Beeps 2G, NASA Webb-Sonifikationen 2G, Kurzwellenskala 2G, Zahlensender 2G, Whistler (VLF) 2G, NASA Apollo · Mercury · JFK 1G, NASA Mars 1G, Geigerzähler 1G, Krell-Schaltkreis 1G, Murmelstimme 1G, Sequenz Puls 1S |
| Bad Sector | space | 0.55 | Kurzwellenskala 2G, Geigerzähler 2G, Krell-Schaltkreis 2G, Leuchtstoffröhre 2G, NASA Beeps 1G, NASA Weltraumfunk 1G, Raumsonden-Bake 1G, Zahlensender 1G, Sequenz Puls 1G, Whistler (VLF) 1G, Murmelstimme 1S, Radio-Clips (Quiet Please) 1S, Sequenz Berliner Schule 1S |

## Nach Gruppe

| Gruppe | häufig | gelegentlich | selten |
|---|---|---|---|
| Flöte Einzelton | Robert Rich | Steve Roach, Klaus Wiese, Amir Baghiri, Vidna Obmana | Oöphoi, Mathias Grassow, Jim Cole, Tom Heasley, Andrew Chalk, Michael Stearns, Jeff Greinke |
| Flöte Phrase | Robert Rich | Steve Roach, Amir Baghiri, Vidna Obmana | Klaus Wiese |
| Eule und fernes Heulen | — | Voice of Eye, Chris Watson | Robert Rich, Steve Roach, Ulf Söderberg, Raison d'Être |
| Klangschale | Klaus Wiese | Robert Rich, Oöphoi, Mathias Grassow, Jim Cole, Voice of Eye, Raison d'Être, Mirror | Steve Roach, Tom Heasley, Vidna Obmana, Troum, Paul Bradley, Andrew Chalk, Darren Tate, ora, Michael Stearns |
| Wassertropfen | Robert Rich | Steve Roach, Voice of Eye, Amir Baghiri, Vidna Obmana, Lustmord, Kammarheit, Gustaf Hildebrand, Ulf Söderberg, Raison d'Être, Biosphere, Chris Watson, Jonathan Coleclough | Oöphoi, Inade, Tho-So-Aa, Apoptose, Polygon, Troum, Thomas Köner, Loscil, BJNilsen, Francisco López, Bass Communion, Andrew Chalk, Mirror, In Camera, Darren Tate, ora, Monos, Brian Eno, Jeff Greinke |
| Sonar | — | Sleep Research Facility, Biosphere | Lustmord, Kammarheit, Gustaf Hildebrand, Arecibo, Polygon, Thomas Köner, Loscil, Hazard, Martin Stürtzer, Tholen |
| Nebelhorn | — | Tom Heasley, Sleep Research Facility, Gustaf Hildebrand, Thomas Köner | Lustmord, Ulf Söderberg, Loscil, BJNilsen |
| Whistler (VLF) | — | Atomine Elektrine, Arecibo, Biosphere, BJNilsen, Hazard, Martin Stürtzer, Phelios, Tholen, S.E.T.I., Bad Sector | Steve Roach, Bass Communion, Michael Stearns, Ian Boddy |
| Rassel | Voice of Eye, Amir Baghiri | Robert Rich, Steve Roach, Vidna Obmana, Apoptose, Chris Watson, Jonathan Coleclough | Klaus Wiese, Inade, Tho-So-Aa, Moljebka Pvlse, Land:Fire, Deutsch Nepal, Troum, BJNilsen, Francisco López, Colin Potter, Darren Tate |
| Geigerzähler | — | Sleep Research Facility, Arecibo, Tho-So-Aa, Moljebka Pvlse, Land:Fire, BJNilsen, Hazard, Colin Potter, S.E.T.I., Bad Sector | Lustmord, Inade, Deutsch Nepal, Francisco López, Chris Watson, Jonathan Coleclough, Mimir, In Camera, Tim Hecker, Tholen |
| Leuchtstoffröhre | — | Sleep Research Facility, Tho-So-Aa, Moljebka Pvlse, Land:Fire, BJNilsen, Hazard, Bad Sector | Lustmord, Inade, Kammarheit, Apoptose, Polygon, Deutsch Nepal, Jonathan Coleclough, In Camera, Phelios, Tholen |
| Krell-Schaltkreis | — | Atomine Elektrine, Tholen, S.E.T.I., Bad Sector | Inade, Arecibo, Martin Stürtzer, Phelios, Ian Boddy |
| Raumsonden-Bake | Arecibo, S.E.T.I. | Atomine Elektrine, Martin Stürtzer, Phelios, Tholen, Bad Sector | Hazard, Michael Stearns |
| Zahlensender | — | Arecibo, Hazard, Tholen, S.E.T.I., Bad Sector | Atomine Elektrine, Biosphere, BJNilsen, Martin Stürtzer, Phelios |
| Kurzwellenskala | — | Atomine Elektrine, Arecibo, Biosphere, BJNilsen, Hazard, In Camera, Colin Potter, Tholen, S.E.T.I., Bad Sector | Sleep Research Facility, Moljebka Pvlse, Land:Fire, Thomas Köner, Mimir, Tim Hecker, Phelios |
| Kleine Glocken | — | Klaus Wiese, Jim Cole, Voice of Eye, Amir Baghiri, Inade, Apoptose, Raison d'Être, Deutsch Nepal, Troum, Jonathan Coleclough, Mirror, Darren Tate, Brian Eno, Tim Hecker | Robert Rich, Steve Roach, Mathias Grassow, Ulf Söderberg, Moljebka Pvlse, Biosphere, Loscil, Bass Communion, Andrew Chalk, Mimir, Colin Potter, Stars of the Lid, Ian Boddy, Jeff Greinke |
| Ferne Kirchenglocke | — | Inade, Gustaf Hildebrand, Ulf Söderberg, Raison d'Être, Thomas Köner, Jonathan Coleclough, Mirror | Mathias Grassow, Tom Heasley, Lustmord, Kammarheit, Tho-So-Aa, Land:Fire, Polygon, Deutsch Nepal, BJNilsen, Paul Bradley, Darren Tate, ora, Monos, Stars of the Lid, Michael Stearns |
| Windglocken | — | Robert Rich, Steve Roach, Klaus Wiese, Amir Baghiri, Vidna Obmana, Andrew Chalk, Michael Stearns, Jeff Greinke | Oöphoi, Jim Cole, Jeff Pearce, Chris Watson, ora, Brian Eno, Martin Stürtzer, Ian Boddy |
| Radio-Clips (Quiet Please) | — | Deutsch Nepal, Biosphere, Bass Communion, Mimir, In Camera, Colin Potter, Tim Hecker | Sleep Research Facility, Troum, Mirror, Brian Eno, Stars of the Lid, Martin Stürtzer, Jeff Greinke, Bad Sector |
| Archiv-Clips (LoC) | Mimir | Deutsch Nepal, Troum, Biosphere, Bass Communion, In Camera, Colin Potter, Tim Hecker | Moljebka Pvlse, Apoptose, Raison d'Être, Andrew Chalk, Jonathan Coleclough, Mirror, Monos, Brian Eno, Stars of the Lid |
| Murmelstimme | — | Voice of Eye, Inade, Apoptose, Raison d'Être, Deutsch Nepal, Troum, In Camera, Martin Stürtzer, S.E.T.I. | Lustmord, Gustaf Hildebrand, Tho-So-Aa, Ulf Söderberg, Moljebka Pvlse, Land:Fire, Biosphere, Bass Communion, Mimir, Colin Potter, Monos, Tim Hecker, Bad Sector |
| Eis und altes Holz | Biosphere, Thomas Köner | Ulf Söderberg, Moljebka Pvlse, BJNilsen, Hazard, Chris Watson, Jonathan Coleclough, Darren Tate, Monos | Voice of Eye, Lustmord, Sleep Research Facility, Inade, Kammarheit, Tho-So-Aa, Apoptose, Land:Fire, Polygon, Raison d'Être, Troum, Loscil, Francisco López, Bass Communion, In Camera, Colin Potter, Tim Hecker |
| Gestrichene Saiten | Robert Rich, Jeff Pearce | Tom Heasley, Troum, Loscil, Bass Communion, Andrew Chalk, Darren Tate, Monos, Brian Eno, Stars of the Lid, Tim Hecker, Jeff Greinke | Steve Roach, Oöphoi, Mathias Grassow, Gustaf Hildebrand, Paul Bradley, Mirror, In Camera, ora, Michael Stearns |
| Gezupfte Saiten | — | Robert Rich, Steve Roach, Jeff Pearce, Amir Baghiri, Vidna Obmana, Biosphere, Loscil, Mimir, Brian Eno, Ian Boddy, Jeff Greinke | Klaus Wiese, Voice of Eye, Andrew Chalk |
| Sequenz Berliner Schule | Martin Stürtzer, Ian Boddy | Steve Roach, Michael Stearns, Phelios, Jeff Greinke | Jeff Pearce, Gustaf Hildebrand, Atomine Elektrine, Biosphere, Loscil, Mimir, Colin Potter, Brian Eno, Tim Hecker, Tholen, Bad Sector |
| Sequenz Mallet und Glas | — | Steve Roach, Amir Baghiri, Vidna Obmana, Biosphere, Loscil, Mimir, Brian Eno, Michael Stearns, Martin Stürtzer, Ian Boddy, Jeff Greinke | Robert Rich, Bass Communion, Mirror, Tim Hecker |
| Sequenz Vokalpuls | — | — | Steve Roach, Voice of Eye, Amir Baghiri, Raison d'Être, Ian Boddy |
| Sequenz Puls | Sleep Research Facility | Lustmord, Inade, Gustaf Hildebrand, Tho-So-Aa, Moljebka Pvlse, Apoptose, Land:Fire, Polygon, Deutsch Nepal, Hazard, Phelios, Bad Sector | Kammarheit, Atomine Elektrine, Arecibo, Ulf Söderberg, Raison d'Être, Thomas Köner, Loscil, Jonathan Coleclough, Colin Potter, S.E.T.I. |
| Sequenz organisch | — | Steve Roach, Voice of Eye, Amir Baghiri, Vidna Obmana | Robert Rich, Klaus Wiese, Oöphoi, Mathias Grassow, Jeff Pearce, Troum, Brian Eno, Stars of the Lid |
| NASA Apollo · Mercury · JFK | — | Martin Stürtzer, S.E.T.I. | Michael Stearns, Phelios |
| NASA Shuttle-Funk | — | Martin Stürtzer | Ian Boddy |
| NASA Beeps | — | Arecibo, Martin Stürtzer, S.E.T.I., Bad Sector | Ian Boddy |
| NASA Weltraumfunk | Arecibo, S.E.T.I. | Atomine Elektrine, Martin Stürtzer, Phelios, Tholen, Bad Sector | Lustmord, Sleep Research Facility, Gustaf Hildebrand, Biosphere, Michael Stearns |
| NASA Mars | — | Phelios, S.E.T.I. | Lustmord, Sleep Research Facility, Kammarheit, Atomine Elektrine, Biosphere, Thomas Köner, BJNilsen, Hazard, Chris Watson |
| NASA Webb-Sonifikationen | — | Atomine Elektrine, Arecibo, Martin Stürtzer, Tholen, S.E.T.I. | — |