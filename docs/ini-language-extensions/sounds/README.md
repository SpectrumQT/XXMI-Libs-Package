# Sounds

**XXMI DLL** can play sound from an INI, through [DirectXTK](https://github.com/microsoft/DirectXTK)'s XAudio2 wrapper.

Nothing is created until an INI declares a sound: a mod that makes no sound opens no audio device, mixes nothing and starts no thread.

## Declaring a Sound

A sound is declared in a section whose name begins with `Sound`:

```ini
[SoundClick]
filename = click.wav
volume = 0.8
overlap = true
```

| Key | Default | Meaning |
| --- | --- | --- |
| `filename` | — | The `.wav` to play. Resolved beside the DLL, or inside the namespace of the INI that declared it, exactly as `[Resource*]` filenames are. |
| `volume` | `1.0` | A multiplier on the file's own level. |
| `speed` | `1.0` | A multiplier on the file's own rate, from `0.5` to `2.0`. |
| `loop` | `false` | Whether playback repeats until stopped. |
| `overlap` | `false` | Whether each play starts a voice of its own. See [Overlap](#overlap). |

The file must be a real `.wav` holding PCM, IEEE float or MS ADPCM data, and the whole of it is read into RAM. An MP3 or Ogg renamed to `.wav` is the usual mistake and is rejected with a reason on the overlay; convert it first:

```
ffmpeg -i music.mp3 -c:a pcm_s16le music.wav
```

The audio device is opened and the files are read while the INI is read, so neither happens inside a frame. A declaration that cannot be satisfied — no audio device, a missing file — is a warning and a silent sound, never a dead mod.

`speed` resamples a single buffer on a single voice, so it shifts the pitch as much as it changes the tempo. XAudio2 creates its voices with a maximum frequency ratio of two, which is where the range comes from.

## Overlap

A `[Sound*]` is played in one of two ways, and `overlap` chooses which:

**`overlap = false`** — the section owns one voice for the life of the process. Each `play` restarts that voice, cutting off whatever it was already playing, and because the voice is the DLL's own it can be stopped, started part way in, and asked where it is. This is what a music track wants.

**`overlap = true`** — each `play` takes a voice from the engine, which recycles it once the sound ends. Several plays sound at once, so hits in quick succession pile up rather than cutting each other off. Nothing holds the voice afterwards, so such a sound cannot be stopped, cannot be started part way in, and reads as not playing at position zero. This is what a click or a hit wants.

Each sounding voice costs a resample and a mix on the audio thread, so hundreds of overlapping plays are a CPU cost like any other. There is no count at which a play is refused.

## Playing and Stopping

```ini
[KeyPlay]
key = p
play = SoundMap
```

`play` optionally takes a second argument, an expression giving where in the sound to begin, in milliseconds:

```ini
play = SoundMap, 500          ; Half a second in
play = SoundMap, $resume_at
```

An offset is only meaningful for a sound that owns its voice; giving one to an overlapping sound is a warning and is ignored.

`stop` stops a sound that owns its voice, and does nothing to one that overlaps:

```ini
stop = SoundMap
```

Both commands work wherever a command list runs, and in `[Key]` and `[Preset]` sections.

## Properties

A declared sound answers for its own state through `->`, which makes it a clock to place anything else against:

| Property | Meaning |
| --- | --- |
| `Position` | How far into the sound playback is, in milliseconds, counted from the samples XAudio2 has played rather than from frames. Includes the offset it was started at. |
| `Length` | The length of the file, in milliseconds. |
| `Playing` | `1` while the sound is playing, `0` otherwise. |
| `Speed` | The `speed` the section was declared with. |
| `Volume` | The `volume` the section was declared with. |

```ini
[CommandListBeat]
$beat = SoundMap->Position / 500
if SoundMap->Playing && SoundMap->Position > SoundMap->Length - 2000
    ; Two seconds left
endif
```

`Position` and `Playing` only answer for a sound that owns its voice; an overlapping one reports `0`.

The position is in the sound's own time, so `speed` does not change what `Position` and `Length` mean.
