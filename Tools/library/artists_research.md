# The 56 packs: who they are after, and what that means for the synth

Research notes behind `Tools/library/artists.py` (11.09.2026). Each entry: what the artist's sound is
made of, as the sources describe it, then how that turns into this instrument's controls and into the
choice of material from the library. The packs are written in the spirit of these artists; nothing is
sampled from or affiliated with them.

The sample library was generated from prompt lists whose every clip names up to four `style_targets`
(`G:/Tools/VRAudio/_Sources/StableAudio3/prompts/*.jsonl`); 40 of the 56 artists appear there. The material
profile quoted per artist (categories, worlds, gestures) is counted from those lists.

## Part 1

### Robert Rich
- Sound: just intonation (divisions of the harmonic series), all-night sleep concerts meant to reach REM
  sleep, played in caves and cathedrals; electronic drones with bamboo flutes and steel (glide) guitar
  floating above, animal calls and rain forest (Rainforest 1989); ethno-fusion to dark ambient.
- Material: analog-pad, guitar, flute, singing-bowl, just-chord, modular-drone, gamelan, mallet; warm and
  cosmic.
- Synth: JI 7-limit / Otonality / Harmonic scales, purity high, long attacks and releases, portamento on a
  glide lead, flute and steel-guitar clips as the voice, rain-forest beds, huge soft reverbs.
- [Wikipedia](https://en.wikipedia.org/wiki/Robert_Rich_(musician)), [AllMusic](https://www.allmusic.com/artist/robert-rich-mn0000285399), [robertrich.com](https://robertrich.com/about/)

### Lustmord
- Sound: the first dark ambient project (Heresy, 1990): recordings from crypts, catacombs, caves, mines,
  shelters and cathedrals, seismic and volcanic material, signals from space, ritual incantations and
  Tibetan horns, enriched with earth-shaking sub bass; psycho-acoustic low frequencies, existential dread.
- Material: gong, choir, chant, brass, modular-drone, bowed-low-string, low-wind, engine-hull; dark, ritual.
- Synth: the lowest register, sub carrying the weight, dark far reverb, cavern and vast rooms, slow swells,
  cave and seismic beds, the Cosmos resonator on the root.
- [Wikipedia](https://en.wikipedia.org/wiki/Lustmord), [Heresy](https://en.wikipedia.org/wiki/Heresy_(Lustmord_album)), [RBMA](https://daily.redbullmusicacademy.com/2015/10/lustmord-feature/)

### Steve Roach
- Sound: Structures from Silence (1984): minimalist, subtly phasing repetition and suspense after a
  Berlin-school start; Dreamtime Return (1988): the Australian desert, field recordings and rhythms from
  indigenous traditions; dense swirling textures and hypnotic rhythms, space and tribal ambient.
- Material: flute, analog-pad, modular-drone, drone-string, just-chord, gamelan, rain forest, digital-bell.
- Synth: phasing pads (two slots on near-identical ratios), tempo-synced delays as slow pulses, desert and
  rain-forest beds, warm filters, long reverb.
- [Structures from Silence](https://en.wikipedia.org/wiki/Structures_from_Silence), [Boomkat](https://boomkat.com/products/structures-from-silence), [RYM](https://rateyourmusic.com/artist/steve_roach)

### Deutsch Nepal
- Sound: industrial ambient with hypnotic rhythms, voice samples and repetitive structures, surreal
  landscapes cut through by precise pounding percussion; ritual, almost industrial.
- Material: bowed-metal, gong, chant, pipe-tank, feedback-drone, metal-creak, abandoned, electrical-hum.
- Synth: the feedback bus with tape and drive, the Patina, loops in the Memory, a Strike on metal as the
  pulse, small hard rooms.
- [Wikipedia](https://en.wikipedia.org/wiki/Deutsch_Nepal), [AllMusic](https://www.allmusic.com/artist/deutsch-nepal-mn0000245842), [Side-Line](https://www.side-line.com/click-interview-with-deutsch-nepal-i-always-wanted-to-create-the-music-that-wasnt-yet-around/)

### Biosphere
- Sound: Tromsø, inside the Arctic circle: loops and odd samples from science fiction and nature;
  Substrata (1997): cold, mountains, glaciers, running water, howling wind and creaking wood around
  sonorous, quietly suspenseful music.
- Material: digital-bell, string-ensemble, low-wind, glass, choir, piano, ice, sea; cold and luminous.
- Synth: loops in the Memory, a slow sub pulse, glass and bell tables, wind and sea beds, cold high cuts,
  cavern rooms.
- [Wikipedia](https://en.wikipedia.org/wiki/Biosphere_(musician)), [Substrata](https://en.wikipedia.org/wiki/Substrata_(album))

### Michael Stearns
- Sound: Planetary Unfolding (1981), slowly evolving atmospheric swirls from a Serge modular; the scores
  for Chronos, Baraka and Samsara; The Beam, a giant string instrument, used sparingly.
- Material: singing-bowl, drone-string, flute, just-chord, overtone-voice, analog-pad, overtone-horn.
- Synth: harmonic stacks, the bowed string as The Beam, the Cosmos shimmer, enormous bright far reverb,
  vast and cathedral rooms.
- [Planetary Unfolding](https://en.wikipedia.org/wiki/Planetary_Unfolding), [Echoes](https://echoes.org/2022/04/04/echoes-april-cd-of-the-month-michael-stearns-planetary-unfolding/)

### Brian Eno
- Sound: tape loops of different lengths (Discreet Music, Music for Airports): simple fragments that never
  clash, left to meet in ever new combinations; generative systems rather than compositions.
- Material: mallet, reed-organ, analog-pad, flute, choir, solo-voice, zither-harp, piano; luminous.
- Synth: the conductor sparse with the deja-vu ring, Memory lines of prime lengths, pentatonic and just
  major, quiet delays, halls.
- [Discreet Music](https://en.wikipedia.org/wiki/Discreet_Music), [Reverb Machine](https://reverbmachine.com/blog/deconstructing-brian-eno-music-for-airports/)

### Mathias Grassow
- Sound: meditative drone ambient; overtone and subharmonic chant, long deep synthesizer drones, Indian
  classical music, singing bowls (after Klaus Wiese), tamboura, zither, flutes.
- Material: singing-bowl, choir, overtone-voice, drone-string, just-chord, drone-ensemble, chant.
- Synth: subharmonic and just scales, stacks, the sub on the difference tone, cathedral and vast rooms,
  hardly any movement.
- [Wikipedia](https://en.wikipedia.org/wiki/Mathias_Grassow), [Interview](https://desertmountaindust.blogspot.com/2017/01/my-music-is-echo-of-my-call.html)

### Raison d'etre
- Sound: ecclesiastical hymns and Gregorian chant tapes with keyboards and percussion; strings, bells and
  choirs over industrial loops; melancholy.
- Material: chant, choir, gong, brass, pipe-organ, bowed-low-string, bell, cave-wind; ritual and dark.
- Synth: chant clips through Spectral at a standstill, the Strike on bells, cathedral rooms morphing to
  vast ones, the Memory for the loops.
- [Wikipedia](https://en.wikipedia.org/wiki/Raison_d'%C3%AAtre_(band)), [Cyclic Defrost](https://www.cyclicdefrost.com/2023/06/listen-to-the-classic-dark-ambient-soundscapes-of-raison-detre/)

### Thomas Koener
- Sound: gongs recorded in rooms and under water, heavily processed, and home-made wind instruments; by
  Permafrost the attack and decay are gone and only the drone remains; deep low frequencies.
- Material: gong, bell, ice, cave-wind, water-tone, pure-tone, singing-bowl; cold and dark.
- Synth: gong clips frozen (Spectral at rate 0, Stretch at geological factors), low cut-offs, sub, cavern
  and vast rooms, near-silence dynamics.
- [PopMatters](https://www.popmatters.com/132255-thomas-koener-nunatak-teimo-permafrost-2496124629.html), [Soundohm](https://www.soundohm.com/product/permafrost), [Ambientblog](https://www.ambientblog.net/blog/thomas-koener-nunatak-teimo-permafrost/)

### Loscil
- Sound: guitar, cello and electric piano blended with electronics (First Narrows); rain on tin, aquatic
  crackle, dub pulses after Pole (Endless Falls).
- Material: low-wind, piano, mallet, string-ensemble, digital-bell, tine, electric-piano, rain on roofs.
- Synth: tempo-synced dub delays with filtered feedback, a soft sub pulse, crackle noise, electric piano
  and mallet clips, halls and chambers.
- [First Narrows](https://en.wikipedia.org/wiki/First_Narrows_(album)), [Endless Falls](https://en.wikipedia.org/wiki/Endless_Falls)

### Sleep Research Facility
- Sound: Nostromo, ultra deep ambience after the opening of Alien, deck by deck into the ship; Deep Frieze,
  glacial layered drones after Antarctic coordinates.
- Material: sub-drone, engine-hull, low-wind, electrical-hum, brass; factory, tunnel, ship and dark-drone
  beds.
- Synth: the lowest register, machine hum, noise allowed as the lead, comb z-plane on the hull pitch, far
  and vast rooms.
- [Deep Frieze](https://en.wikipedia.org/wiki/Deep_Frieze), [Cold Spring](https://coldspring.bandcamp.com/album/nostromo-csr34cd)

### Hazard
- Sound: BJ Nilsen's early project: extreme weather and the perception of time; deep drones, glitches and
  processed field recordings (Wind, with Chris Watson); very dark (Lech).
- Material: electrical-hum, pipe-tank, sub-drone, engine-hull, pure-tone; electric, ventilation, factory
  and contact beds.
- Synth: wind and electric beds, low hums, grain glitches from the Cloud, dark industrial rooms.
- [Ash International](https://ashinternational.com/ash-4-5-hazard-north/), [Bandcamp](https://bennynilsen.bandcamp.com/album/wind-with-chris-watson)

### Andrew Chalk
- Sound: small instruments, field recordings, piano, guitar and reels of tape; lonely, unsettlingly
  beautiful drones; ghostly guitar, church-organ synthesizers; an English arts-and-crafts aesthetic.
- Material: piano, tape loops, tape-keyboard, pipe-organ, reed-organ, string-ensemble, guitar; tape.
- Synth: the Patina, Blur, the Memory's age, organ tables, halls.
- [RBMA](https://daily.redbullmusicacademy.com/2015/02/uk-drone-feature/), [Soundohm](https://www.soundohm.com/artist/andrew-chalk?layout=big-grid)

### Jonathan Coleclough
- Sound: everyday objects turned into richly textured drones: water boiling, sheep bells on a hillside,
  pins dropping, a sheet of glass, a metal bowl, a melting ice cube; bowed metals, air through brass.
- Material: pipe-organ, bell, glass, blown-vessel, bowed-metal, bowed-low-string; grain textures.
- Synth: objects through Spectral into chords, the Cloud's resonators, plates and chambers.
- [Last.fm](https://www.last.fm/music/Jonathan+Coleclough/+wiki), [Biography](http://www.coleclough.plus.com/pages/biography.html)

### Polygon
- No source found that identifies the artist (searches for dark ambient and drone labels came back
  empty). The pack is therefore built from the name and the company it keeps in the list: cold,
  geometric, electronic -- glass and digital bells, modular drones, exact filter shapes. To be corrected
  once it is clear who is meant.

### Land:Fire
- Sound: German dark and space ambient with industrial and radio noise (Shortwave Transmission, 2009;
  GONE, 2002; with Ionosphere).
- Synth: shortwave hiss as a noise band, radio beds, the Cosmos shifter, comb shapes, dark cosmic rooms.
- [Discogs](https://www.discogs.com/release/2009670-LandFire-Shortwave-Transmission), [Bandcamp](https://shortwavetransmission.bandcamp.com/album/gone)

### Apoptose
- Sound: German dark ambient; Nordland, a journey to the north of Europe, a landmark of the genre;
  Blutopfer, built on field recordings of the Easter drumming processions of Calanda.
- Synth: processions of drums (the Strike on wood, modal frame drums, the Hawkes clock), chant and bells,
  ritual beds, cathedrals.
- [Bandcamp](https://apoptose-official.bandcamp.com/album/nordland), [Funprox](https://www.funprox.com/reviews/apoptose-blutopfer/), [Santa Sangre](https://santasangremagazine.wordpress.com/2012/03/18/falling-leaves-interview-with-apoptose/)

### Atomine Elektrine
- Sound: Peter Andersson's side project: old-school space ambient after Tangerine Dream and Klaus
  Schulze -- sweeping analogue synthesizers, toned-down rhythms, sudden jagged electronics; atoms and space.
- Synth: analogue pads with a resonant ladder, quantised pulses, the Cosmos, vast rooms.
- [Release Magazine](http://releasemagazine.net/Onrecord/oratomineeau.htm), [Prog Archives](http://www.progarchives.com/album.asp?id=22689)

### Martin Stuertzer
- Sound: from Wuppertal; best known for the dark ambient of Phelios, under his own name ambient, dub techno
  and space music (Starfields, Void Pulse, Nebular Resonance, Antares Station).
- Material: analog-pad, modular-drone, brass, engine-hull, digital-bell, radio-space, sub-drone.
- Synth: cosmic pads, dub delays and a sub pulse, digital bells, radio beds, halls and vast rooms.
- [martinstuertzer.de](https://martinstuertzer.de/), [Bandcamp](https://phelios.bandcamp.com/album/starfields)

### Ulf Soederberg
- Sound: the Swedish composer behind Sephiroth; tribal, ritual and dark ambient -- Tidvatten, a hypnotic
  journey into the deep forests of northern Sweden; Vindarnas hus; Inland, ritual electronic music.
- Material: pipe-tank, sub-drone, electrical-hum, engine-hull, cave-wind; tunnel, abandoned, cave beds.
- Synth: low ritual drones, wooden strikes now and then, forest and cave beds, caverns.
- [Discogs](https://www.discogs.com/release/185839-Ulf-S%C3%B6derberg-Tidvatten), [Projekt](https://www.projekt.com/store/product/slm09807/)

### Monos
- No source found. Built from the prompt lists' profile: pipe organ, guitar, drone ensemble, bowed metal,
  long strings, feedback drones; tape and dark.

### Paul Bradley
- Sound: UK drone artist and the Twenty Hertz label (Drone Works, Water Mountain, Moraines; with Darren
  Tate, Colin Potter, Adam Sonderberg): long-form drone rooted in profound darkness.
- Material: pipe-organ, modular-drone, singing-wire, bowed-low-string, pure-tone; empty space, ice.
- Synth: one long tone, minimal change, purity near one, deep reverb.
- [Bandcamp](https://twentyhertz.bandcamp.com/album/drone-works-1), [RYM](https://rateyourmusic.com/artist/paul_bradley)

### Tho-So-Aa
- Sound: Lutz Rach's dark ambient project (Germany, since 1995): Enrielle, Epoch, Index 1.0 Coma, Absorb,
  Minus, Identify, on Drone Records and Tesco.
- Material: factory, tunnel, dark drone noise, ventilation, geothermal, mud bubbles (all field recordings).
- Synth: noise as the lead, machine beds, sub, bunkers and chambers.
- [Discogs](https://www.discogs.com/artist/95129-Tho-So-Aa), [Last.fm](https://www.last.fm/music/Tho-So-Aa)

### Arecibo
- Sound: a Lustmord side project: pulsars, quasars, thermal radiation and supernova remnants as recorded by
  NASA's deep space network (Pharos, with the 1974 Arecibo message).
- Material: radio-space, seismic, underwater, geothermal, dark drone noise.
- Synth: pulsar pulses (sub pulse, synced LFOs), noise bands, cosmic dark rooms, the lowest register.
- [Pharos](https://en.wikipedia.org/wiki/Pharos_(album)), [Last.fm](https://www.last.fm/music/Arecibo)

### Gustaf Hildebrand
- Sound: dark ambient and drone from Eskilstuna, astronomy the main influence: Starscape, Primordial
  Resonance (sweeping soundscapes, delicate textures, distant shrieks of surreal machinery), Heliopause.
- Synth: cosmic pads, sub, wide stereo, machinery beds, vast rooms.
- [Last.fm](https://www.last.fm/music/Gustaf+Hildebrand/+wiki), [Cyclic Law](https://cycliclaw.bandcamp.com/album/primordial-resonance)

### Tholen
- Sound: Sternklang, a cosmic dark ambient tale with memories of early Krautrock; Neuropol, an industrial
  post-apocalyptic city of pneumatics, fibre optics, electric arcs and voices.
- Synth: kosmische analogue swells against electric and factory beds, the Cosmos shifter, plates and vast
  rooms.
- [Cyclic Law: Sternklang](https://cycliclaw.bandcamp.com/album/sternklang), [Neuropol](https://cycliclaw.bandcamp.com/album/neuropol), [Interview](http://www.tokafi.com/15questions/interview-tholen/)

### Amir Baghiri
- Sound: electro-tribal sound design, space and tribal ambient: deep, emotional, mystical soundworlds,
  spacious soundscapes with occasional percussion.
- Synth: flutes, zither and bowls, warm filters, a wooden strike now and then, halls and cathedrals.
- [Sonic Immersion](https://www.sonicimmersion.org/tag/amir-baghiri/), [Databloem](https://databloem.bandcamp.com/album/the-serenity)

## Part 2

### Polygon (corrected)
- Sound: the project of Ingo Lindmeier (formerly of Mortal Constraint), a figure of the 1990s dark scene
  known for sonic abstractions and a melancholic, hybrid sound; Refuge (Glasnost 1995, remastered 2024).
- Synth: melancholic abstract drones, hybrid electronic and organic material, dim rooms.
- [Bandcamp: Refuge](https://aliensproduction.bandcamp.com/album/refuge)

### Monos (corrected)
- Sound: Darren Tate's duo with Colin Potter (Tate works "under his own name and as Ora and Monos"): deep,
  organic, microtonal, "vegetal" drone with a phantom sense of place (Morgendaemmerung, with Lol Coxhill
  and Daisuke Suzuki).
- Material: pipe-organ, guitar, drone-ensemble, bowed-metal, long-string, feedback-drone; tape and dark.
- Synth: slowly beating microtonal drones (purity drifting, close ratios), organ and guitar clips, organic
  beds, chambers.
- [Darren Tate](https://en.wikipedia.org/wiki/Darren_Tate), [RBMA](https://daily.redbullmusicacademy.com/2015/02/uk-drone-feature/)

### Mirror
- Sound: Andrew Chalk and Christoph Heemann (about 1998-2005): sometimes dark and brooding, sometimes
  ethereal, always stark, bare, minimal drone; soothing and ominous at once (Eye of the Storm, Mirror of
  the Sea, Ringstones, Still Valley).
- Material: reed-organ, glass, pipe-organ, tape-keyboard, mallet, solo-voice, zither-harp; tape, luminous.
- Synth: harmonium and glass tables, bare two-voice drones, the nebula's smear, enveloping halls.
- [Bandcamp](https://christophheemann.bandcamp.com/album/ringstones), [Christoph Heemann](https://en.wikipedia.org/wiki/Christoph_Heemann)

### Mimir
- Sound: Christoph Heemann, Andreas Martin, Edward Ka-Spel, the Silverman and Jim O'Rourke: collages of
  ambience, noise, loops, drums and guitar melodies beside quiet deep movements; Mimyriad, one 49-minute
  gliding tonal float with swooping drones.
- Material: tape-keyboard, piano, tape loops, string-ensemble, guitar, tine; grain textures, city, crackle.
- Synth: loops in the Memory, the Patina, gliding portamento, chords rather than single tones, staggered
  collage-like entries.
- [Wikipedia](https://en.wikipedia.org/wiki/Mimir_(band)), [Brainwashed](https://brainwashed.com/common/htdocs/discog/mimir.php)

### In Camera
- No source found that describes an ambient project of this name. Built from the prompt lists' profile:
  low wind, tape loops, piano, reed organ, blown vessels, bowed low strings, radio, electric piano; tape
  and cold -- small dim rooms, close and quiet.

### Colin Potter
- Sound: the ICR label, engineer and member of Nurse With Wound: disquieting drones and electronic
  psychedelia -- languid sway, relentless rhythms, rolling drones, "steam engines lost in space, galleons
  passing in electric fog" (And Then); woozy modular electronics and delay-laden drums (Here).
- Material: bowed-metal, guitar, grain textures, tunnel, tape loops, radio, singing-wire, feedback-drone.
- Synth: long tape delays in series, the Memory with drive, modular FM, tunnel and radio beds.
- [ICR](https://icrdistribution.bandcamp.com/album/and-then), [RBMA interview](https://daily.redbullmusicacademy.com/2015/03/colin-potter-interview/)

### Darren Tate
- Sound: York; drone since the 1980s, solo and in Ora and Monos: from semi-static field recordings
  animated by little but air against the microphone to psych-folk, always an uncanny stillness at the
  centre; organic, microtonal, vegetal.
- Material: bell, grain textures, blown-vessel, night country, guitar, rain on land, wind, foliage.
- Synth: field recordings at the centre, very still, microtonal purity drift, blown and bell clips.
- [Wikipedia](https://en.wikipedia.org/wiki/Darren_Tate), [Last.fm](https://www.last.fm/music/Darren+Tate/+wiki)

### ora
- Sound: Andrew Chalk and Darren Tate (with Coleclough, Coxhill, Potter, Suzuki): drones from field
  recordings, bowed metals and electronics, from boulder-like scrapes to tiny fluctuations of flutes and
  sitars; Amalgam built on recordings made near water.
- Material: pipe-organ, guitar, glass, blown-vessel, reed-organ, drone-ensemble, long-string, bowed-metal.
- Synth: bowed metal and organ, water beds, minimal movement, tape.
- [Brainwashed](http://brainwashed.com/ora/), [ICR](https://icrdistribution.bandcamp.com/album/final-2)

### BJNilsen
- Sound: under his own name on Touch: field recordings and electronic composition; The Eye of the
  Microphone, a derive through the City of London (Victoria station, the Thames, a building site), close to
  Chris Watson's hyperrealism; The Invisible City.
- Material: ice, electrical-hum, radio, bell, contact recordings, pure-tone, singing-wire, sea, wind.
- Synth: city and contact beds, hums, a few bells, wide but unadorned rooms.
- [Touch](https://touch33.net/catalogue/to95-bj-nilsen-eye-of-the-microphone.html), [Headphone Commute](https://headphonecommute.com/2014/02/21/bj-nilsen-eye-of-the-microphone-touch/)

### Klaus Wiese
- Sound: the master of the Tibetan singing bowl; Persian string instruments, bells, voice; tanpura with
  Popol Vuh (Hosianna Mantra); Sufi teachings; ritual, environmental; El-Hadra with Mathias Grassow.
- Material: singing-bowl, flute, overtone-voice, bowed-folk, drone-string, reed-organ, drone-ensemble.
- Synth: bowls through the Cloud's resonators and Spectral, tanpura drones, just scales, very long holds.
- [Wikipedia](https://en.wikipedia.org/wiki/Klaus_Wiese), [Spectrum Culture](https://spectrumculture.com/2026/02/17/klaus-wiese-uranus-review/)

### Ooephoi
- Sound: Gianluigi Gasparetti: electronic dronescapes with concrete noises and acoustic wind and chordal
  instruments; singing bowls, flutes, wood chimes, stones, shells, crystal chimes, processed voices, all
  analogue; ancient places and hermeticism (The Spirals of Time, Athlit).
- Material: rain forest, insects, ritual objects, fire, river (field recordings).
- Synth: pure slow banks, chimes via the Cloud's resonators, organic beds, deep space rooms.
- [Wikipedia](https://en.wikipedia.org/wiki/O%C3%B6phoi), [Bandcamp](https://oophoi.bandcamp.com/album/the-spirals-of-time)

### Inade
- Sound: Aldebaran: walls of deep pulsating waves, crushing stellar explosions, wails and voices from deep
  space, atonal skitterings echoing in dark cathedrals; esoteric, dizzying.
- Material: cave, ritual objects, tunnel, dark drone noise, seismic.
- Synth: the Cosmos heavy, pulsation from the sub and synced LFOs, modal metal, cathedral and vast rooms.
- [Chain D.L.K.](https://www.chaindlk.com/reviews/4416), [Cold Spring](https://coldspring.bandcamp.com/album/aldebaran-csr13cd)

### Troum
- Sound: Bremen duo from Maeror Tri: "Tiefenmusik" between dark ambient, drone and transcendental noise,
  made without samplers or synthesizers -- guitars, bass, accordion, balalaika, flutes, harmonica, gongs,
  field recordings -- towards hypnagogic dream states.
- Material: grain textures, abandoned, empty space, murmur, metal creak, fire, ritual objects.
- Synth: massed Stretch beds, the feedback's warmth, the Memory with drive, reed-like tables, caverns.
- [Wikipedia](https://en.wikipedia.org/wiki/Troum), [Cyclic Law](https://cycliclaw.bandcamp.com/album/emphasys)

### S.E.T.I.
- Sound: Andrew Lagowski: space ambient -- Sleep Environments for Interplanetary Travel (eight hours),
  Final Trajectory (a craft's life support and propulsion, hallucinatory voices), Pod (mission dialogue,
  monitoring-station noise, eerie desolate space noise).
- Material: geothermal, radio-space, electric, seismic, ventilation, underwater.
- Synth: sleep-long slow presets, ship systems as beds, radio noise bands, vast rooms.
- [Loki Foundation](https://loki-found.bandcamp.com/album/sleep-environments-for-interplanetary-travel), [Ambientblog](https://www.ambientblog.net/seti-frame/)

### Bad Sector
- Sound: Massimo Magrini (Tuscany, 1992): emotional dark ambient noise about microbiology, algorithms,
  physics and space; Kosmodrom: deep pulsating drones and processed rhythmic sequences over radio signals,
  the Soviet ANS and Aelita synthesizers; recordings of electromagnetic fields.
- Material: factory, dark drone noise, radio-space, ventilation, electric.
- Synth: algorithmic sequences (quantised conductor, cascade), ANS-like spectral bands, radio beds.
- [Wikipedia](https://en.wikipedia.org/wiki/Bad_Sector), [Ambientblog](https://www.ambientblog.net/blog/2014-12/bad-sector-kosmodrom/)

### Phelios
- Sound: Martin Stuertzer's cosmic doom: majestic nebulous drones, ominous strings, rising waves of tribal
  percussion, earthen darkness, long reverbs over vast distances, sub-orchestral brass (Gates of Atlantis,
  Astral Unity).
- Material: radio-space, desert, dark drone noise, wind, underwater, ice, seismic.
- Synth: nebulous pads, string and brass clips, wooden and skin strikes in processions, vast rooms.
- [Malignant](https://malignantrecs.bandcamp.com/album/gates-of-atlantis), [Musique Machine](https://www.musiquemachine.com/reviews/reviews_template.php?id=4626)

### Moljebka Pvlse
- Sound: Fredrik Mathias Josefson (Stockholm): heavily processed guitars, electronics, field recordings and
  found sounds in very minimal, slowly evolving, zen-like drones where duration is the material
  (Sadalmelik, one 72-minute track).
- Material: metal creak, cave, factory, abandoned, ship port, electric, contact recordings.
- Synth: one slow unfolding per preset, processed guitar-like drones, industrial beds, long holds.
- [moljebka.com](http://www.moljebka.com/), [Funprox](https://www.funprox.com/reviews/moljebka-pvlse-sadalmelik/)

### Francisco Lopez
- Sound: a field biologist in the Costa Rican rain forest (La Selva): straightforward recordings presented
  as abstract sound works, "blind" profound listening, no boundary between industrial and wild sound.
- Material: underwater, wind, grain textures, contact recordings, wetland, desert, foliage, river, cave.
- Synth: environments as the whole preset, near-inaudible to enveloping dynamics, no melody.
- [AllMusic](https://www.allmusic.com/album/la-selva-mw0001155564), [franciscolopez.net](https://www.franciscolopez.net/env.html)

### Chris Watson
- Sound: the field recordist: intimate natural environments; El Tren Fantasma, a Mexican railway journey
  with looped train rhythms as a cinematic narrative; Weather Report.
- Material: underwater, wind, sea, river, polar station, foliage, contact recordings, insects, ice.
- Synth: recordings as a continuum on the Vector's corners, a quiet centre, realistic rooms.
- [Wikipedia](https://en.wikipedia.org/wiki/Chris_Watson_(musician)), [Touch](https://touch33.net/catalogue/to42-chris-watson-el-tren-fantasma.html)

### Voice of Eye
- Sound: Jim Wilson and Bonnie McNairn (Houston, later Taos): shamanic tribal ambient made without
  synthesizers from acoustic and home-made instruments, layered, with ritual percussion and Middle Eastern
  overtones (Transmigration, Vespers, Isolation).
- Material: ritual objects, wetland, fire, rain forest, murmur.
- Synth: acoustic-object beds, ritual strikes, bowls and bells in the Cloud, warm deep rooms.
- [Wikipedia](https://en.wikipedia.org/wiki/Voice_of_Eye), [Bandcamp](https://voiceofeye.bandcamp.com/album/transmigration)

### Bass Communion
- Sound: Steven Wilson: Ghosts on Magnetic Tape, processed 78 rpm records and piano -- subtle murmurings,
  scratching noises, soft dark drones, crackling tape, a transmission from the netherworld.
- Material: crackle media, city, ice and snow, transport, sea.
- Synth: the Patina's age and hiss, crackle noise, piano and string tables through the Memory, dim rooms.
- [Ghosts on Magnetic Tape](https://en.wikipedia.org/wiki/Ghosts_on_Magnetic_Tape), [Steven Wilson HQ](https://stevenwilsonhq.com/recordings/bass-communion/)

### Kammarheit
- Sound: Paer Bostroem: The Starwheel -- deeply serene drones and melancholic soundscapes with an engulfing
  spiritual depth; a benchmark of the second wave of dark ambient.
- Synth: serene low drones, sparse events, cavern and cathedral rooms, slow bells now and then.
- [Wikipedia](https://en.wikipedia.org/wiki/Kammarheit), [This Is Darkness](http://www.thisisdarkness.com/2017/03/19/kammarheit-the-starwheel-2005-retro-review/)

### Stars of the Lid
- Sound: melancholy orchestral drones of strings, horns and piano that swell and dissolve, layered one
  instrument at a time and blended by heavy reverberation in an empty concert hall.
- Synth: bowed-string and brass clips entering one after another, consonant just major, long swells, halls.
- [The Tired Sounds of](https://en.wikipedia.org/wiki/The_Tired_Sounds_of_Stars_of_the_Lid), [Rolling Stone](https://rollingstone.com/music/music-news/how-stars-of-the-lid-made-two-ambient-masterworks-63198/amp)

### Tim Hecker
- Sound: Ravedeath, 1972: a church pipe organ in Reykjavik with digital processing; the bellows' halting
  breath like glitch, decaying tape loops, enveloping distortion, mournful; loops like waterfalls, wind and
  filtered light.
- Synth: pipe-organ clips through the driven feedback and the Memory's drive, the fold, blur, cathedrals.
- [Musicworks](https://www.musicworks.ca/reviews/recordings/tim-hecker-ravedeath-1972), [RA](https://ra.co/reviews/8510)

### Ian Boddy
- Sound: the DiN label: ambient electronics between 1970s analogue pioneers and digital soundscapes; modular
  systems from calm soundscapes to pulsing Berlin-school sequences; drones, shimmering pulses, blips.
- Synth: resonant filter movement, quantised conductor pulses, FM blips, crossed delays, plates.
- [ianboddy.com](http://ianboddy.com/din/), [Igloo](https://igloomag.com/profiles/ian-boddy-25-years-of-din)

### Jeff Greinke
- Sound: Cities in Fog (Seattle, 1984-85): electronic and acoustic textures, haunting yet inviting, a strong
  sense of place -- a harbour city worn by the sea under a green-grey fog that swallows even its sounds.
- Synth: fog as low-passed noise and blur, city and harbour beds, muted keys, layered textures.
- [Bandcamp](https://jeffgreinke.bandcamp.com/album/cities-in-fog-1), [Opus](https://opus.ing/reviews/cities-in-fog-jeff-greinke-1996-projekt)

### Vidna Obmana
- Sound: Dirk Serries: The River of Appearance -- flutes, ocarina, harmonica and percussion over loops and
  electronics, sparse piano, drifting keyboard loops, rainstick; slowly building subtle loops, serene.
- Synth: flute clips, gentle Memory loops, rain beds, a soft pulse, warm halls.
- [Zoharum](https://zoharum.bandcamp.com/album/the-river-of-appearance-25th-anniversary-edit), [Opus](https://opus.ing/posts/exploring-depths-river-appearance-vidnaobmana-ambient-masterpiece)

### Tom Heasley
- Sound: ambient tuba: low notes stacked by delays and live looping into shifting drones, throat singing,
  a home-made didgeridoo; spiralling brass swells shifting like seismic plates.
- Synth: brass clips in the low register, Memory stacks, overtone formants, a sub, deep swells.
- [Wikipedia](https://en.wikipedia.org/wiki/Tom_Heasley), [Perfect Sound Forever](https://www.furious.com/perfect/tomheasley.html)

### Jeff Pearce
- Sound: ambient electric guitar: EBow lines into a looper and reverb, distortion, sometimes bowed with a
  knife; tones processed, delayed and looped until they no longer sound like a guitar; also solo piano.
- Synth: guitar and steel-guitar clips, slow volume swells (source envelopes), loops in the Memory, halls.
- [Wikipedia](https://en.wikipedia.org/wiki/Jeff_Pearce_(American_musician)), [Ambient Music Guide](http://ambientmusicguide.com/a-z-essential-albums/jeff-pearce/)

### Jim Cole
- Sound: Jim Cole and Spectral Voices: group harmonic overtone singing recorded live in an empty water
  tower, its reverb the only effect; Theravada chant, Tibetan monks and the Harmonic Choir as influences.
- Synth: overtone-voice and vowel tables, formant sweeps, the Harmonic series tuning, one enormous room.
- [Spectral Voices](https://spectralvoices.com/about/), [Expose](http://www.expose.org/index.php/articles/display/jim-cole-and-spectral-voices-innertones-3.html)
