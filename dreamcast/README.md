# Dreamcast asset conversion — Sonic CD / RSDKv3

    ./generate_assets.sh <path to Data.rsdk>

Unpacks `Data.rsdk` and converts the two asset classes the Dreamcast build
cannot read as shipped. Everything else is left inside the datafile.

Output lands in `<stage>/Data/`, defaulting to the folder `Data.rsdk` is already
in — so pointing it at `build-dc` or `build-pc` puts the files where they belong.

**Put the converted `Data/` folder on the disc next to the untouched
`Data.rsdk`.** `Reader.cpp` prefers a loose file over the packed one, so the
converted music and sfx win while sprites, stages, scripts and palettes still
stream out of the datafile. That is why this doesn't unpack a whole game tree.

Requires `bash`, `python3`, `ffmpeg`, `ffprobe`. Takes about a minute.

## What it produces

| | format |
|---|---|
| `Data/Music/**.ogg` | headerless signed 8-bit PCM, **mono**, 22050 Hz |
| `Data/SoundFX/**.wav` | RIFF WAV, **unsigned** 8-bit, mono, 22050 Hz (two exceptions) |

Music keeps the `.ogg` extension because `GameConfig.bin` and the StageConfigs
resolve tracks by filename — only the contents change. The loader sniffs for
`OggS` and refuses a file that is still a real container, so a half-converted
set fails loudly rather than playing noise.

Music is mono because that is what the assets are: side/mid energy measures
2.9%, and ZoneComplete/GameOver run 8.4s/10.7s as mono, which are the correct
jingle lengths. If you convert in stereo, set `DC_MUSIC_CHANNELS` to 2 as well.

## Sound effect rates

Target format taken from the validated `Data` folder the shipping CDIs are built
from — all 74 files measured:

| | |
|---|---|
| all files | RIFF WAV, PCM, mono, **unsigned 8-bit** |
| 72 of 74 | **22050 Hz** |
| `Ring.wav`, `LoseRings.wav` | **44100 Hz**, and only those two |

Those two carry 13.5% and 11.0% of their energy above 11 kHz; every other sound
is under 5%. A 22050 signal cannot represent anything above 11025 Hz, so
downsampling them throws away the part that makes them sound like themselves —
they came out tinny and nothing else did. They are the exception because they
were measured to be the exception.

22050 for the rest is not a compromise. It halves the sfx footprint, and the
AICA's 65534-sample ceiling reaches 2.97s at 22050 against 1.49s at 44100 — so
*more* sounds fit on hardware voices, not fewer. 70 of 74 play on AICA voices.
The four that don't are `Achievement`, `TimeWarp`, `BombCarrier` (too long at any
rate) and `LoseRings` (deliberately native).

Sfx must stay **RIFF**, not headerless: the loader assumes `DC_SRC_RATE` for a
headerless file, so a per-file rate only survives if there's a header carrying
it. That's the whole reason the two exceptions can exist.

## Building the disc image

Once the assets are converted:

```sh
./builddir/mkdcdisc -e '<path to RSDKv3.elf>' -d '<path to Data dir>' -d  '<path to videos dir>' -f '<path to Data.rsdk>' -f '<path to settings.ini>' -N -n SonicCD -o SonicCD.cdi
```

`-d` adds a directory tree, `-f` adds a single file. **Both `Data/` and
`Data.rsdk` go on the disc** — that is the arrangement this converter is built
around, not a redundancy. `Reader.cpp` prefers a loose file over the packed one,
so the converted music and sfx win while sprites, stages, scripts and palettes
still stream out of the datafile.

`videos/` is produced by the separate FMV script and is not touched here.

## Not Sonic Mania's scripts

These are modelled on the Mania Dreamcast asset scripts but must not be
substituted for them — that build targets a different engine and would produce
wrong data twice over:

| | Mania | here |
|---|---|---|
| music | `-ac 2`, stereo | **mono** |
| sfx | Yamaha ADPCM via `wav2adpcm` | plain 8-bit PCM, **no ADPCM**, 22050 Hz |

Sfx must also stay **RIFF**, not headerless: the loader assumes `DC_SRC_RATE`
for a headerless file, so a per-file rate only survives if there's a header to
carry it.

FMV conversion is not handled here — see the separate video script.

## Files

    generate_assets.sh              orchestrator
    rsdkv3_extract.py               Data.rsdk -> tree (XOR'd RSDKv3 datafile)
    music_step_1_ogg_to_pcm.sh      Vorbis -> raw s8 mono 22050
    music_step_2_rename_to_ogg.sh   .s8 -> .ogg
    sfx_step_1_resample.sh          per-file rate pick -> 8-bit mono RIFF
