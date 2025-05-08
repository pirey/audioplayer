# Audio Player

## Audio Data

### WAV format
- binary file with RIFF format
- raw audio data
- binary layout
  - RIFF header
  - fmt header
    - sample rate
    - channels
    - bit depth
    - etc
  - raw audio samples

Offset | Size | Description
-------|------|------------
0      | 4    | "RIFF" (ASCII)
4      | 4    | File size - 8
8      | 4    | "WAVE"
12     | 4    | "fmt " (subchunk ID)
16     | 4    | Subchunk1 size (usually 16 for PCM)
20     | 2    | Audio format (1 = PCM)
22     | 2    | Num channels
24     | 4    | Sample rate
28     | 4    | Byte rate = SampleRate × NumChannels × BitsPerSample / 8
32     | 2    | Block align = NumChannels × BitsPerSample / 8
34     | 2    | Bits per sample
36     | 4    | "data" (subchunk ID)
40     | 4    | Subchunk2 size (data size)
44     | ...  | Actual audio data

#### RIFF format

RIFF = Resource Interchange File Format

### PCM - Pulse Code Modulation

### Soundcard/Audio Output

## File I/O
- Read WAV file
- Parsing WAV

## Playing Audio
- Audio Output API: to interact with audio driver
  - Linux: ALSA/PulseAudio
  - macOS: CoreAudio
  - Windows: WASAPI/DirectSound

## Simple Playback
  - static tone: e.g. sine wave
  - pre-recorded WAV file

## Synchronization
need to sync so audio plays at the correct speed according to sample rate.

- manage buffers, handle timing
- simple timer

## Misc

Generate wav file using sox:

```bash
sox -n output.wav synth 3 sine 440
```

# Binary File

## Structure
- Header (optional)
- Data blocks - raw bytes
- Footer (optional) - checksums, padding, markers

## Binary Layout
How data is laid out in memory or file, byte by byte.

Every binary files has its own layout, defined by whoever designed it.

```c
typedef struct {
  uint32_t id; // 4 bytes
  float value; // 4 bytes
  char name[4]; // 4 bytes
} Record;

// Record -> 12 bytes
//
```

See [Hex Dump](#hex-dump).

C structs are stored in memory as raw bytes and written to binary file directly.

## Hex Dump <a name="hex-dump"></a>
Human readable view of binary data

View hexdump using tools, e.g. `xxd`

```
[00 00 00 01] [00 00 80 3F] [41 42 43 00]
   id = 1       value = 1.0f   name = "ABC"

```

## Endianness

Byte Order

Little-endian: least significant byte first → 0x12345678 → 78 56 34 12

Big-endian: most significant byte first → 0x12345678 → 12 34 56 78

# TODO
- [ ] play basic WAV file
- [ ] handle large file
- [ ] synchronize audio
- [ ] user input: pause/stop
- [ ] support more audio format
- [ ] TUI
- [ ] GUI
