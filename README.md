# wled-audiosync

Makes [WLED](https://kno.wled.ge/) audio-reactive effects follow the music of a
Linux player, without a microphone on the WLED controller.

wled-audiosync reads 48 kHz, 16 bit, stereo audio from a named pipe, runs an FFT with
WLED's 16-band layout and sends the result to WLED as **Audio Sync V2** packets (the
protocol of WLED's audioreactive usermod: volume, beat peak, 16 frequency bands, major
peak) by UDP multicast, 50 times a second. A WLED controller that is set to receive audio
sync then runs its sound-reactive effects on that data.

- **Any process can feed the pipe**: `ffmpeg`, `arecord`, a player's pipe output, ...
- **Intended source: [alsa-leadtap](https://github.com/ehb/alsa-leadtap)**, an ALSA plugin
  that writes a copy of what a player outputs into a pipe a fixed time *before* it
  becomes audible.
- **`--lead-ms`** holds every packet back by that time, so the light follows what you
  *hear*, not what is coming. Set it to the plugin's `lead`. For a pipe that is not ahead
  of the sound, use `--lead-ms 0`.
- **Own gain control.** WLED does no gain control on received data (it does it on its own
  microphone input), so a quiet recording would barely move the effects. Each band and the
  volume follow a slowly decaying maximum, so the effects look the same at any volume.
- AC3/DTS (IEC 61937) in the pipe and silence give zero packets, so the effects rest.
- Linux only, one C file, no dependencies besides libm.

## Usage

```
wled-audiosync --pipe /run/leadtap-sync.fifo --lead-ms 180
```

| Option | Default | Meaning |
|---|---|---|
| `--pipe PATH` | (required) | named pipe to read; created if missing, must be a FIFO (anything else: exit 2) |
| `--lead-ms N` | 180 | hold packets back this long, 0..2000; a packet goes out at most 2 ms early |
| `--target IP` | 239.0.0.1 | destination; the default is WLED's multicast group, a unicast address of one controller works too |
| `--port N` | 11988 | UDP port 1..65535 (WLED's default) |
| `--gain G` | 1.0 | scales volume and bands after the gain control; a number with 0 < G <= 100 |
| `--dry` | | print what would be sent (about five lines a second), send nothing |
| `-v` | | print the same while sending |
| `--selftest` | | run the built-in tests and exit |

Exit status: 0 after SIGINT/SIGTERM, 2 for bad arguments or a path that is not a named pipe (a
supervisor should not restart), 1 for a runtime error such as a failed read (restart). Send
errors (for example no network) do not end the program; the first is printed, then at most
one message every 10 s.

On the WLED side: *Config -> Sync Interfaces -> Audio Sync*, mode **Receive**, port 11988
(or the port you configured). Use an effect marked as sound-reactive. The WLED build must
include the audioreactive usermod.

Feeding the pipe by hand, for a try-out with a file:

```
ffmpeg -re -i music.flac -f s16le -ar 48000 -ac 2 - > /run/leadtap-sync.fifo
```

Example with alsa-leadtap: the plugin's `fifo` setting names the pipe, its `lead` setting
and `--lead-ms` have the same value.

## Install

Static binaries for 64-bit ARM (aarch64) and 32-bit ARM (armhf) are attached to each
release on the [Releases page](https://github.com/ehb/wled-audiosync/releases); they need
no libraries. On LibreELEC the alsa-leadtap add-on already contains the aarch64 binary.

## Build

```
make            # build/wled-audiosync
make test       # runs --selftest, no audio hardware or network needed
make CC=aarch64-linux-gnu-gcc    # cross build, e.g. for a 64-bit Raspberry Pi
```

The selftest covers: tones landing in the expected band and major peak, distant bands
staying quiet, silence, the IEC 61937 hold, the gain control recovering after a loud
passage, reassembly of periods from a pipe that delivers arbitrary byte counts, and the
packet layout (44 bytes).

## How the analysis works

Stereo is mixed to mono and halved in rate (two frames averaged), giving a 512-point FFT
at about 22 kHz as in WLED. The 16 bands use WLED's bin groups. WLED's smoothing (fast rise,
slower fall) and a square-root curve are used for the band values. What differs on purpose:
WLED's pink-noise factors and its lift of the high bands make up for a microphone; a
digital signal has full treble, so one curve is used for all bands. The beat flag fires when
the two lowest bands jump above their recent average.

The output approximates what WLED computes from a microphone; it is not bit-identical and
has been tuned by eye on a few kinds of music and speech.

## Status

Early. It has run on **one** Raspberry Pi 4 (64-bit LibreELEC, fed by alsa-leadtap) with
one WLED controller receiving, for the author's own use. Nothing else has been tested: no
other WLED versions or builds, no other hardware, no other sources for the pipe than
alsa-leadtap and a test file. How closely the light follows the sound was judged by eye;
the delay between sound and light was not measured with instruments. Multicast needs a
network where it works (some Wi-Fi access points drop it); use `--target` with the
controller's address if it does not.

## Credits and licence of the ideas

The band layout, the smoothing constants and the packet format follow
[WLED](https://github.com/wled/WLED) (usermod audioreactive, by its authors); WLED is not
affiliated with this project and did not review it. The program is written independently
(own FFT, own gain control) and contains no WLED source text; the table of bin ranges and a
few numeric constants are taken from WLED so that the effects behave alike.

## How this was made

The code was written by Claude Code (Anthropic); the commits carry an `Assisted-by:`
trailer. Other models reviewed it. I directed the work, made the design decisions and
tested it on my own hardware, but I have not read the code line by line. Treat it like any
outside code -- issues and questions are welcome.

## License

GPL-3.0-or-later, see `LICENSE`.
