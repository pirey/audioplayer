# Audio Player

## Audio Sample

Sample -> a value -> amplitude/loudness/how loud the air is vibrating

Sequence of sample over time -> waveform -> tone, pitch, and timbre

- Pitch -> frequency of a sound wave
- Tone -> character/quality of sound
- Timbre -> color/texture of sound

## Quantization

Convert continuous analog value into a fixed, discrete digital value during digital-to-analog conversion.

## Sample Rate

How many sample per second

- 44.1kHz Sample Rate -> 441000Hz -> 441000 sample in one second
- Nyquist theorem: Human hearing range 20kHz, need > 40kHz to capture the audio

## Bit Depth/Bit Per Sample

How many bits per sample.

## Amplitude

Higher amplitude = louder sound

## Dynamic Range

Range between the quietest and loudest sound a system can capture.

Higher bit depth = wider range.

## Audio Fidelity

How accurately a recording/playback reproduces the original sound.

## Byte Rate

How many bytes per second = bit per sample * sample rate

## WAV format
- binary file with RIFF format
- raw uncompressed audio data
- binary layout
  - RIFF header
  - fmt header
  - raw audio samples

Offset | Size | Description
-------|------|------------
0      | 4    | "RIFF" (ASCII)
4      | 4    | total file size minus 8
8      | 4    | "WAVE"
12     | 4    | "fmt " (subchunk ID)
16     | 4    | Subchunk1 size (usually 16 for PCM), this refers to the total size of the fields below
20     | 2    | Audio format (1 = PCM)
22     | 2    | Num channels
24     | 4    | Sample rate
28     | 4    | Byte rate = SampleRate × NumChannels × BitsPerSample / 8
32     | 2    | Block align = NumChannels × BitsPerSample / 8
34     | 2    | Bits per sample
36     | 4    | "data" (subchunk ID)
40     | 4    | Subchunk2 size (data size)
44     | ...  | Actual audio data

## RIFF format

RIFF = Resource Interchange File Format

Store data in tagged chunk.

Tags: "RIFF", "fmt ", "data", "LIST"

## PCM - Pulse Code Modulation

Standard format for uncompressed digital audio

1. Sampling
2. Quantization
3. Encoding

## Soundcard/Audio Output

## Audio Output API
- Linux: ALSA/PulseAudio
- macOS: CoreAudio
- Windows: WASAPI/DirectSound

## Synchronization

## Upmixing

Convert audio with fewer channel to format with more channels, e.g. duplicate mono channel into stereo

## Resampling

Change audio sample rate to other rate, e.g. to match device sample rate

## Misc

Generate wav file using sox:

```bash
sox -n output.wav synth 3 sine 440
```

## TODO
- [ ] play basic WAV file
- [ ] handle large file
- [ ] user input: pause/stop
- [ ] support more audio format: MP3, AAC, OGG, FLACC
- [ ] TUI
- [ ] GUI
