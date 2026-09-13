"""Fetch the public-domain sound archive the near layer plays from (13.09.2026).

  python Tools/library/fetch_archive.py [--only nasa] [--dry-run]

Everything here is a work of the United States government (NASA, JPL, the Chandra X-ray Center's
NASA-funded sonifications) and so free of copyright, per NASA's Media Usage Guidelines
(https://www.nasa.gov/nasa-brand-center/images-and-media/). Two items on the NASA pages are music
made for NASA by named artists ("Liftoff", "We Rise Together") and are left out, since the
guidelines except third-party works. Every file is written with its source URL and its page into
Library/Archive/SOURCES.md, which is what the content pack ships beside the files.

The archive is NOT tracked in git (Library/ is content), and it is fetched, never generated.
"""
import argparse
import os
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ARCHIVE = os.path.join(ROOT, "Library", "Archive")

# (folder, page, title, url)
NASA_HISTORICAL_PAGE = "https://www.nasa.gov/historical-sounds/"
NASA_BEYOND_PAGE = "https://www.nasa.gov/sounds-from-beyond/"
NASA_SONIFICATION_PAGE = "https://www.nasa.gov/data-sonifications/"
U = "https://www.nasa.gov/wp-content/uploads/2015/01/"
U24 = "https://www.nasa.gov/wp-content/uploads/2024/05/"
W = "https://www3.nasa.gov/specials/sounds/"

ITEMS = [
    # ---- Historical sounds: the radio loops, the voices, the beeps
    ("NASA/Historical/Discovery", NASA_HISTORICAL_PAGE, "Discovery - APU shutdown", U + "640148main_APU20Shutdown.mp3"),
    ("NASA/Historical/Discovery", NASA_HISTORICAL_PAGE, "Discovery - Computers are in control", U + "640149main_Computers20are20in20Control.mp3"),
    ("NASA/Historical/Discovery", NASA_HISTORICAL_PAGE, "Discovery - Go at throttle up 1", U + "640150main_Go20at20Throttle20Up.mp3"),
    ("NASA/Historical/Discovery", NASA_HISTORICAL_PAGE, "Discovery - Go at throttle up 2", U + "640151main_Go20at20Throttle20Up202.mp3"),
    ("NASA/Historical/Discovery", NASA_HISTORICAL_PAGE, "Discovery - Go for deploy", U + "640164main_Go20for20Deploy.mp3"),
    ("NASA/Historical/Discovery", NASA_HISTORICAL_PAGE, "Discovery - Good picture of Steve", U + "639893main_Good_Picture_of_Steve.mp3"),
    ("NASA/Historical/Discovery", NASA_HISTORICAL_PAGE, "Discovery - Houston Discovery", U + "639898main_Houston_Discovery.mp3"),
    ("NASA/Historical/Discovery", NASA_HISTORICAL_PAGE, "Discovery - Houston Discovery 2", U + "639896main_Houston_Discovery_2.mp3"),
    ("NASA/Historical/Discovery", NASA_HISTORICAL_PAGE, "Discovery - How do you read", U + "639900main_How_do_you_Read.mp3"),
    ("NASA/Historical/Discovery", NASA_HISTORICAL_PAGE, "Discovery - Lookin at it", U + "640165main_Lookin20At20It.mp3"),
    ("NASA/Historical/Discovery", NASA_HISTORICAL_PAGE, "Discovery - MECO", U + "640166main_MECO.mp3"),
    ("NASA/Historical/Discovery", NASA_HISTORICAL_PAGE, "Discovery - Nice to be in orbit", U + "640167main_Nice20to20be20in20Orbit.mp3"),
    ("NASA/Historical/Discovery", NASA_HISTORICAL_PAGE, "Discovery - On its way to orbit", U + "640168main_On20its20way20to20Orbit.mp3"),
    ("NASA/Historical/Discovery", NASA_HISTORICAL_PAGE, "Discovery - Press to ATO", U + "640169main_Press20to20ATO.mp3"),
    ("NASA/Historical/Discovery", NASA_HISTORICAL_PAGE, "Discovery - Roger roll", U + "640170main_Roger20Roll.mp3"),
    ("NASA/Historical/Discovery", NASA_HISTORICAL_PAGE, "Discovery - STS-26 liftoff", U + "640392main_STS-26_Liftoff.mp3"),
    ("NASA/Historical/Discovery", NASA_HISTORICAL_PAGE, "Discovery - STS-41D liftoff", U + "640393main_STS-41D_Liftoff.mp3"),
    ("NASA/Historical/Discovery", NASA_HISTORICAL_PAGE, "Discovery STS-131 - sound of launch", U + "590189main_ringtone_131_launchNats.mp3"),
    ("NASA/Historical/Discovery", NASA_HISTORICAL_PAGE, "Discovery - Vector transfer", U + "640173main_Vector20Transfer.mp3"),
    ("NASA/Historical/Discovery", NASA_HISTORICAL_PAGE, "Discovery - Wheelstop", U + "640174main_Wheel20Stop.mp3"),
    ("NASA/Historical/Shuttle", NASA_HISTORICAL_PAGE, "STS-1 - We're going to dust it off first", U + "581097main_STS-1_Dust-it-Off.mp3"),
    ("NASA/Historical/Shuttle", NASA_HISTORICAL_PAGE, "STS-7 - That was definitely an E-ticket", U + "582362main_Sally-Ride_e-ticket.mp3"),
    ("NASA/Historical/Shuttle", NASA_HISTORICAL_PAGE, "STS-132 - Shuttle gear drop", U + "590327main_ringtone_landingGearDrop.mp3"),
    ("NASA/Historical/Shuttle", NASA_HISTORICAL_PAGE, "STS-135 - Countdown to launch", U + "590318main_ringtone_135_launch.mp3"),
    ("NASA/Historical/Shuttle", NASA_HISTORICAL_PAGE, "STS-135 - Launch commentary", U + "577774main_STS-135Launchringtone-v2.mp3"),
    ("NASA/Historical/Shuttle", NASA_HISTORICAL_PAGE, "STS-135 - Landing commander comments", U + "590196main_ringtone_135_landingCommanderComments.mp3"),
    ("NASA/Historical/Shuttle", NASA_HISTORICAL_PAGE, "STS-135 - Landing comments", U + "590316main_ringtone_135_landingNaviusComments.mp3"),
    ("NASA/Historical/Apollo-Mercury", NASA_HISTORICAL_PAGE, "Apollo 8 - Merry Christmas", U + "581549main_Apollo-8_Merry-Christmas.mp3"),
    ("NASA/Historical/Apollo-Mercury", NASA_HISTORICAL_PAGE, "Apollo 11 - We have a lift-off", U + "590320main_ringtone_apollo11_countdown.mp3"),
    ("NASA/Historical/Apollo-Mercury", NASA_HISTORICAL_PAGE, "Apollo 11 - Eagle has landed", U + "569462main_eagle_has_landed.mp3"),
    ("NASA/Historical/Apollo-Mercury", NASA_HISTORICAL_PAGE, "Apollo 11 - Eagle has landed extended", U + "590333main_ringtone_eagleHasLanded_extended.mp3"),
    ("NASA/Historical/Apollo-Mercury", NASA_HISTORICAL_PAGE, "Apollo 11 - One small step", U + "590331main_ringtone_smallStep.mp3"),
    ("NASA/Historical/Apollo-Mercury", NASA_HISTORICAL_PAGE, "Apollo 12 - Cardiac sim", U + "584851main_Apollo-12_Cardiac-Sim.mp3"),
    ("NASA/Historical/Apollo-Mercury", NASA_HISTORICAL_PAGE, "Apollo 12 - All weather testing", U + "584852main_Apollo-12_All-Weather-Testing.mp3"),
    ("NASA/Historical/Apollo-Mercury", NASA_HISTORICAL_PAGE, "Apollo 13 - Houston we've had a problem", U + "574928main_houston_problem.mp3"),
    ("NASA/Historical/Apollo-Mercury", NASA_HISTORICAL_PAGE, "JFK - Return him safely to Earth", U + "591240main_JFKmoonspeech.mp3"),
    ("NASA/Historical/Apollo-Mercury", NASA_HISTORICAL_PAGE, "JFK - We choose the moon with Apollo 11 launch", U + "590325main_ringtone_kennedy_WeChoose.mp3"),
    ("NASA/Historical/Apollo-Mercury", NASA_HISTORICAL_PAGE, "JFK - We choose the moon", U + "586447main_JFKwechoosemoonspeech.mp3"),
    ("NASA/Historical/Apollo-Mercury", NASA_HISTORICAL_PAGE, "Mercury 4 - Clock started", U + "582369main_Mercury-4_Clock-Started.mp3"),
    ("NASA/Historical/Apollo-Mercury", NASA_HISTORICAL_PAGE, "Mercury 6 - Zero G", U + "582367main_Mercury-6_Zero-G.mp3"),
    ("NASA/Historical/Apollo-Mercury", NASA_HISTORICAL_PAGE, "Mercury 6 - Godspeed", U + "582368main_Mercury-6_God-Speed.mp3"),
    ("NASA/Historical/Apollo-Mercury", NASA_HISTORICAL_PAGE, "Mercury 7 - Liftoff", U + "582371main_Aurora-7_Liftoff.mp3"),
    ("NASA/Historical/Apollo-Mercury", NASA_HISTORICAL_PAGE, "Mercury 7 - Fireflies", U + "582374main_Aurora-7_Fireflies.mp3"),
    ("NASA/Historical/Apollo-Mercury", NASA_HISTORICAL_PAGE, "Mercury 7 - Guaymas greeting", U + "582382main_Aurora-7_Guyamas-Greeting.mp3"),
    ("NASA/Historical/Apollo-Mercury", NASA_HISTORICAL_PAGE, "Mercury 9 - Cooper comments", U + "582370main_mercury_Cooper_Orbit-Comments.mp3"),
    ("NASA/Historical/Missions", NASA_HISTORICAL_PAGE, "Atlas V - Launch", U + "590329main_ringtone_SDO_launchNats.mp3"),
    ("NASA/Historical/Missions", NASA_HISTORICAL_PAGE, "Cassini - Enceladus sound", U + "584796main_enceladus.mp3"),
    ("NASA/Historical/Missions", NASA_HISTORICAL_PAGE, "Cassini - Saturn radio emissions 1", U + "584791main_spookysaturn.mp3"),
    ("NASA/Historical/Missions", NASA_HISTORICAL_PAGE, "Cassini - Saturn radio emissions 2", U + "584795main_saturn_radio_waves.mp3"),
    ("NASA/Historical/Missions", NASA_HISTORICAL_PAGE, "Juno - Morse code HI from Earth", "https://www.jpl.nasa.gov/multimedia/sounds/RingTone01_Longer.mp3"),
    ("NASA/Historical/Missions", NASA_HISTORICAL_PAGE, "Kepler - Star KIC12268220C light curve", U + "578358main_kepler_star_KIC12268220C.mp3"),
    ("NASA/Historical/Missions", NASA_HISTORICAL_PAGE, "Kepler - Star KIC7671081B light curve", U + "578359main_kepler_star_KIC7671081B.mp3"),
    ("NASA/Historical/Missions", NASA_HISTORICAL_PAGE, "LCROSS - Water on the Moon song", U + "583775main_lcross_marmie_water_moon.mp3"),
    ("NASA/Historical/Missions", NASA_HISTORICAL_PAGE, "SOFIA - Takeoff", U + "sofia_takeoff_audio.mp3"),
    ("NASA/Historical/Missions", NASA_HISTORICAL_PAGE, "Stardust - Passing comet Tempel 1", U + "598980main_stardust_tempel1.mp3"),
    ("NASA/Historical/Missions", NASA_HISTORICAL_PAGE, "Voyager - Interstellar plasma sounds", "https://www.nasa.gov/externalflash/interstellar.mp3"),
    ("NASA/Historical/Missions", NASA_HISTORICAL_PAGE, "Voyager - Lightning on Jupiter", U + "603921main_voyager_jupiter_lightning.mp3"),
    ("NASA/Historical/Beeps", NASA_HISTORICAL_PAGE, "Chorus radio waves in Earth's atmosphere", U + "693857main_emfisis_chorus_1.mp3"),
    ("NASA/Historical/Beeps", NASA_HISTORICAL_PAGE, "Quindar sound 1", U + "578628main_hskquindar.mp3"),
    ("NASA/Historical/Beeps", NASA_HISTORICAL_PAGE, "Quindar sound 2", U + "578629main_hawquindar.mp3"),
    ("NASA/Historical/Beeps", NASA_HISTORICAL_PAGE, "Sputnik beep", U + "578626main_sputnik-beep.mp3"),
    ("NASA/Historical/Beeps", NASA_HISTORICAL_PAGE, "SLS test fire", U + "663784main_SLS_Audio_D.mp3"),
    # ---- Sounds from beyond: Mars, InSight, Juno
    ("NASA/Beyond", NASA_BEYOND_PAGE, "First audio recording of sounds on Mars", U24 + "scam-mic-sol001-run001.wav"),
    ("NASA/Beyond", NASA_BEYOND_PAGE, "Perseverance SuperCam records wind on Mars", U24 + "scam-mic-sol004-run001.wav"),
    ("NASA/Beyond", NASA_BEYOND_PAGE, "First acoustic recording of laser shots on Mars", U24 + "scam-mic-sol012-run001.wav"),
    ("NASA/Beyond", NASA_BEYOND_PAGE, "Ingenuity Mars helicopter in flight", U24 + "jpl-20210506-listen-to-nasas-ingenuity-helicopter-as-it-flies-on-mars.wav"),
    ("NASA/Beyond", NASA_BEYOND_PAGE, "Perseverance records a Martian dust devil", U24 + "pia25657.wav"),
    ("NASA/Beyond", NASA_BEYOND_PAGE, "Perseverance gaseous dust removal tool", U24 + "em-0346-edlc-mic-gdrt-session-34.wav"),
    ("NASA/Beyond", NASA_BEYOND_PAGE, "Perseverance gaseous dust removal tool (filtered)", U24 + "em-0346-edlc-mic-gdrt-session-34-filtered.wav"),
    ("NASA/Beyond", NASA_BEYOND_PAGE, "Perseverance rover driving, sol 16 (16 minutes)", U24 + "raw-audio-mic-00000008-000-001011.wav"),
    ("NASA/Beyond", NASA_BEYOND_PAGE, "Perseverance rover driving, sol 16 (highlights)", U24 + "filtered-highlights-sol16roverdrivehighlights.wav"),
    ("NASA/Beyond", NASA_BEYOND_PAGE, "Sounds from Mars, rover self-noise filtered out", U24 + "sounds-from-mars-filters-out-rover-self-noise.wav"),
    ("NASA/Beyond", NASA_BEYOND_PAGE, "Sounds from Mars, rover self-noise included", U24 + "sounds-from-mars-includes-rover-self-noise.wav"),
    ("NASA/Beyond", NASA_BEYOND_PAGE, "InSight seismometer dinks and donks", U + "Cropped-Dinks-and-Donks-sample.wav"),
    ("NASA/Beyond", NASA_BEYOND_PAGE, "Marsquake magnitude 3.3, 25 July 2019", U + "Quake-Sol-235.wav"),
    ("NASA/Beyond", NASA_BEYOND_PAGE, "Marsquake magnitude 3.7, 22 May 2019", U + "Quake-Sol-173.wav"),
    ("NASA/Beyond", NASA_BEYOND_PAGE, "InSight robotic arm", U + "20190819-Sol-98-SEIS-Spatialized-reflective.wav"),
    ("NASA/Beyond", NASA_BEYOND_PAGE, "InSight seismometer raw (full length)", U + "06-MASTERRESAMPLED-48Kraw_velocity_0.6_normalisedx1.wav"),
    ("NASA/Beyond", NASA_BEYOND_PAGE, "InSight seismometer raw (short)", U + "07-Mars_sound1a_20s_x100.wav"),
    ("NASA/Beyond", NASA_BEYOND_PAGE, "InSight seismometer, two octaves up", U + "08-Brian-Cook_raw_velocity_0.6_normalisedx1_2octavesUp_03.wav"),
    ("NASA/Beyond", NASA_BEYOND_PAGE, "Juno Ganymede flyby", U24 + "e2-wave-ganymede-flyby-compressed.wav"),
    # ---- Webb sonifications
    ("NASA/Sonification/Webb", NASA_SONIFICATION_PAGE, "Webb - WASP-96 b", W + "wasp-notes-Aug-3.wav"),
    ("NASA/Sonification/Webb", NASA_SONIFICATION_PAGE, "Webb - Southern Ring Nebula, mid-infrared", W + "ring-miri-Aug-%203.wav"),
    ("NASA/Sonification/Webb", NASA_SONIFICATION_PAGE, "Webb - Southern Ring Nebula, near-infrared", W + "ring-nircam-Aug-3.wav"),
    ("NASA/Sonification/Webb", NASA_SONIFICATION_PAGE, "Webb - Southern Ring Nebula, side by side", W + "ring-side-by-side-Aug-3.wav"),
    ("NASA/Sonification/Webb", NASA_SONIFICATION_PAGE, "Webb - Cosmic Cliffs, stars", W + "carina-stars-Aug-3.wav"),
    ("NASA/Sonification/Webb", NASA_SONIFICATION_PAGE, "Webb - Cosmic Cliffs, mountains", W + "carina-bot-Aug-3.wav"),
    ("NASA/Sonification/Webb", NASA_SONIFICATION_PAGE, "Webb - Cosmic Cliffs, sky", W + "carina-top-Aug-3.wav"),
    ("NASA/Sonification/Webb", NASA_SONIFICATION_PAGE, "Webb - Cosmic Cliffs", W + "carina-Aug-3.wav"),
]


def safe_name(title, url):
    ext = os.path.splitext(url.split("?")[0])[1].lower() or ".bin"
    keep = "".join(ch if ch.isalnum() or ch in " -_'" else " " for ch in title)
    keep = " ".join(keep.split()).replace("'", "")
    return keep + ext


def fetch(url, path, dry):
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return "kept", os.path.getsize(path)
    if dry:
        return "would fetch", 0
    req = urllib.request.Request(url, headers={"User-Agent": "Noctuary-archive/1.0 (+library fetch, one file at a time)"})
    with urllib.request.urlopen(req, timeout=120) as r, open(path + ".part", "wb") as f:
        while True:
            chunk = r.read(1 << 16)
            if not chunk:
                break
            f.write(chunk)
    os.replace(path + ".part", path)
    return "fetched", os.path.getsize(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", default="", help="a substring of the folder to fetch (e.g. 'Beyond')")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    rows, bad, total = [], 0, 0
    for folder, page, title, url in ITEMS:
        if a.only and a.only.lower() not in folder.lower():
            continue
        d = os.path.join(ARCHIVE, folder)
        os.makedirs(d, exist_ok=True)
        path = os.path.join(d, safe_name(title, url))
        try:
            what, size = fetch(url, path, a.dry_run)
            total += size
            print(f"  {what:11s} {size / 1e6:7.2f} MB  {folder}/{os.path.basename(path)}")
            rows.append((folder, os.path.basename(path), title, url, page))
        except Exception as e:
            bad += 1
            print(f"  FAILED  {folder}/{os.path.basename(path)}: {e}")
        time.sleep(0.3)   # one file at a time, politely
    if not a.dry_run:
        src = os.path.join(ARCHIVE, "SOURCES.md")
        lines = ["# Library/Archive -- where every file came from", "",
                 "Fetched by Tools/library/fetch_archive.py. All NASA/JPL material is a work of the United States",
                 "government and not subject to copyright (NASA Media Usage Guidelines); the sonifications are",
                 "NASA products credited NASA/CXC/SAO (Chandra) and NASA/ESA/CSA/STScI (Webb). Nothing here was",
                 "made by a third party for NASA -- the two songs on the pages were left out for that reason.", ""]
        by_folder = {}
        for folder, name, title, url, page in rows:
            by_folder.setdefault(folder, []).append((name, title, url, page))
        for folder in sorted(by_folder):
            lines.append(f"## {folder}")
            lines.append("")
            for name, title, url, page in by_folder[folder]:
                lines.append(f"- `{name}` -- {title}  ")
                lines.append(f"  {url}  (from {page})")
            lines.append("")
        with open(src, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))
        print(f"wrote {src}")
    print(f"{len(rows)} files, {total / 1e6:.1f} MB, {bad} failed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
