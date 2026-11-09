# Changelog

## 0.1.0

First public version.

- Reads 48 kHz, 16 bit, stereo audio from a named pipe, analyses it with a 512-point FFT in
  WLED's 16-band layout and sends WLED Audio Sync V2 packets (volume, peak, 16 bands, major
  peak) by UDP multicast, 50 per second.
- Own gain control (slowly decaying maximum per band and for the volume), so the effects look
  the same at any volume.
- `--lead-ms` holds every packet back so the light follows what is heard when the pipe runs
  ahead of the sound (as the copy from alsa-leadtap does).
- IEC 61937 data (AC3/DTS passthrough) and silence give zero packets.
- `--selftest` without audio hardware or network; `--dry` prints instead of sending.
