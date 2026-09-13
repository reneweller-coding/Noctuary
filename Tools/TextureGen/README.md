# TextureGen — text-to-audio textures for the Texture source slots

A small PySide6 program that turns a prompt into WAV files for Noctuary's
*Texture* sources (Source 2 / Source 3, Type = Texture). The models run in a
child process (`texturegen_worker.py`) that stays alive between jobs, so a
model is loaded once per session.

## Models

| key | model | output | best for |
|---|---|---|---|
| `sao` | Stable Audio Open 1.0 | 44.1 kHz stereo, ≤ 47 s | textures, field recordings, metal, air — the first choice |
| `musicgen-large` | MusicGen Large | 32 kHz mono, ≤ 30 s | tonal drones, held chords, choirs |
| `musicgen-medium` / `-small` | MusicGen | 32 kHz | quicker variants |
| `audioldm2` | AudioLDM 2 Large | 16 kHz mono | dark effects; no air above 8 kHz |

Stable Audio Open is gated: accept the licence on its Hugging Face page and
run `huggingface-cli login` once. The others download without a login. All
weights land in the Hugging Face cache (several GB each). Stable Audio's
scheduler needs `torchsde`; without it diffusers fails with an empty
`ImportError` at load time (it is in `requirements.txt`).

## Setup

```
python -m venv .venv
.venv\Scripts\python -m pip install torch torchaudio --index-url https://download.pytorch.org/whl/cu128
.venv\Scripts\python -m pip install -r requirements.txt
.venv\Scripts\python texturegen.py
```

(`cu128` is for RTX 50-series cards; pick the CUDA build that matches yours.)

## Output

`Textures/<slug>_<model>_<seed>[_<note>].wav` (32-bit float, peak −6 dBFS)
plus a `.txt` with the prompt and settings. `<note>` is the detected base
pitch (autocorrelation, only written when the clip is clearly periodic).
Noctuary, the render tool and the Quest app read a `_A3`-style suffix and
pitch the texture to the key when the slot's *Pitch* is *Note*; unpitched
textures (rain, wind) simply have no suffix and play free.

Command line, without the GUI:

```
.venv\Scripts\python texturegen_worker.py --model sao --prompt "bowed metal plate, long shimmer" --seconds 30 --count 4
.venv\Scripts\python texturegen_worker.py --batch prompts_example.txt --model sao --seconds 30 --count 2
```

A batch file has one prompt per line (`#` comments); a line may end with
`| name=... seconds=... model=... steps=... guidance=... seed=...` to override
the defaults for that prompt, and a `.json` list of job objects works too.
Jobs are grouped by model so each model loads once. `--count` renders that
many seeds per prompt.
