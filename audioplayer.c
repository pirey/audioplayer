#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#endif

// WAV header structure (minimal for PCM)
typedef struct {
    char chunk_id[4]; // "RIFF"
    uint32_t chunk_size; // File size - 8
    char format[4]; // "WAVE"
    char subchunk1_id[4]; // "fmt "
    uint32_t subchunk1_size; // 16 for PCM
    uint16_t audio_format; // 1 for PCM
    uint16_t num_channels; // 1 = mono, 2 = stereo
    uint32_t sample_rate; // e.g., 44100
    uint32_t byte_rate; // sample_rate * num_channels * bits_per_sample / 8
    uint16_t block_align; // num_channels * bits_per_sample / 8
    uint16_t bits_per_sample; // e.g., 16
} WavHeader;

// Playback state (platform-agnostic)
typedef struct {
    uint8_t *audio_data; // Raw PCM data
    uint32_t data_size; // Total size of audio data
    uint32_t offset; // Current position in audio data
    uint32_t sample_rate; // For timing calculations
    uint16_t num_channels; // WAV file channels
    uint16_t bits_per_sample; // WAV file bits per sample
    uint16_t output_channels; // Device output channels
} PlaybackState;

#ifdef __APPLE__
// macOS-specific callback for Core Audio
static OSStatus audioCallback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags,
                              const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber,
                              UInt32 inNumberFrames, AudioBufferList *ioData) {
    PlaybackState *state = (PlaybackState *)inRefCon;
    AudioBuffer *buffer = &ioData->mBuffers[0];

    // Log callback invocation
    printf("Callback: frames=%u, offset=%u/%u\n", inNumberFrames, state->offset, state->data_size);

    if (state->offset >= state->data_size) {
        buffer->mDataByteSize = 0; // Signal end of data
        return noErr;
    }

    // Calculate bytes to copy (input: 16-bit integer, output: 32-bit float)
    uint32_t input_bytes_per_frame = state->num_channels * (state->bits_per_sample / 8);
    uint32_t output_bytes_per_frame = state->output_channels * 4; // 32-bit float
    uint32_t input_bytes_to_copy = inNumberFrames * input_bytes_per_frame;
    if (state->offset + input_bytes_to_copy > state->data_size) {
        input_bytes_to_copy = state->data_size - state->offset;
        inNumberFrames = input_bytes_to_copy / input_bytes_per_frame;
    }

    // Convert 16-bit integer PCM to 32-bit float, with upmixing for mono
    int16_t *src = (int16_t *)(state->audio_data + state->offset);
    float *dst = (float *)buffer->mData;
    uint32_t samples_per_channel = inNumberFrames;
    if (state->num_channels == 2) {
        // Stereo: Direct conversion
        for (uint32_t i = 0; i < samples_per_channel; i++) {
            dst[i * 2] = src[i * 2] / 32768.0f;       // Left channel
            dst[i * 2 + 1] = src[i * 2 + 1] / 32768.0f; // Right channel
        }
    } else {
        // Mono: Upmix to stereo by duplicating samples
        for (uint32_t i = 0; i < samples_per_channel; i++) {
            float sample = src[i] / 32768.0f;
            dst[i * 2] = sample;     // Left channel
            dst[i * 2 + 1] = sample; // Right channel
        }
    }
    buffer->mDataByteSize = inNumberFrames * output_bytes_per_frame;
    state->offset += input_bytes_to_copy;

    return noErr;
}
#endif

// Read and parse WAV file (platform-agnostic)
static int read_wav_file(const char *filename, PlaybackState *state, float *duration) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        printf("Error: Cannot open file %s\n", filename);
        return 1;
    }

    // Read RIFF header
    char riff_id[4];
    uint32_t riff_size;
    char format[4];
    if (fread(riff_id, 4, 1, file) != 1 || fread(&riff_size, 4, 1, file) != 1 ||
        fread(format, 4, 1, file) != 1) {
        printf("Error: Failed to read RIFF header\n");
        fclose(file);
        return 1;
    }
    if (strncmp(riff_id, "RIFF", 4) != 0 || strncmp(format, "WAVE", 4) != 0) {
        printf("Error: Not a valid WAV file\n");
        fclose(file);
        return 1;
    }
    printf("RIFF chunk: size=%u, format=WAVE\n", riff_size);

    // Read fmt chunk
    WavHeader header = {0};
    if (fread(header.subchunk1_id, 4, 1, file) != 1 || fread(&header.subchunk1_size, 4, 1, file) != 1) {
        printf("Error: Failed to read fmt chunk header\n");
        fclose(file);
        return 1;
    }
    if (strncmp(header.subchunk1_id, "fmt ", 4) != 0) {
        printf("Error: fmt chunk not found\n");
        fclose(file);
        return 1;
    }
    printf("fmt chunk: size=%u\n", header.subchunk1_size);

    // Read fmt data
    if (fread(&header.audio_format, 2, 1, file) != 1 || fread(&header.num_channels, 2, 1, file) != 1 ||
        fread(&header.sample_rate, 4, 1, file) != 1 || fread(&header.byte_rate, 4, 1, file) != 1 ||
        fread(&header.block_align, 2, 1, file) != 1 || fread(&header.bits_per_sample, 2, 1, file) != 1) {
        printf("Error: Failed to read fmt chunk data\n");
        fclose(file);
        return 1;
    }
    if (header.audio_format != 1) {
        printf("Error: Only PCM format is supported (audio_format=%u)\n", header.audio_format);
        fclose(file);
        return 1;
    }
    if (header.bits_per_sample != 16 || (header.num_channels != 1 && header.num_channels != 2)) {
        printf("Error: Only 16-bit mono or stereo WAV files are supported\n");
        fclose(file);
        return 1;
    }
    if (header.sample_rate < 8000 || header.sample_rate > 96000) {
        printf("Error: Sample rate %u Hz is not supported (must be 8000–96000 Hz)\n", header.sample_rate);
        fclose(file);
        return 1;
    }

    // Skip extra fmt data if subchunk1_size > 16
    if (header.subchunk1_size > 16) {
        fseek(file, header.subchunk1_size - 16, SEEK_CUR);
    }

    // Find data chunk
    char chunk_id[4];
    uint32_t chunk_size;
    while (fread(chunk_id, 4, 1, file) == 1 && fread(&chunk_size, 4, 1, file) == 1) {
        printf("Chunk: id=%c%c%c%c, size=%u, file_pos=%ld\n",
               chunk_id[0], chunk_id[1], chunk_id[2], chunk_id[3], chunk_size, ftell(file) - 8);
        if (strncmp(chunk_id, "data", 4) == 0) {
            state->data_size = chunk_size;
            break;
        }
        fseek(file, chunk_size, SEEK_CUR); // Skip chunk
    }
    if (strncmp(chunk_id, "data", 4) != 0) {
        printf("Error: Could not find data chunk\n");
        fclose(file);
        return 1;
    }

    // Print WAV info
    printf("WAV Info:\n");
    printf("  Sample Rate: %u Hz\n", header.sample_rate);
    printf("  Channels: %u (%s)\n", header.num_channels, header.num_channels == 1 ? "Mono" : "Stereo");
    printf("  Bits per Sample: %u\n", header.bits_per_sample);
    printf("  Byte Rate: %u bytes/s\n", header.byte_rate);
    printf("  Block Align: %u bytes\n", header.block_align);
    printf("  Data Size: %u bytes\n", state->data_size);
    printf("  Duration: %.2f seconds\n", (float)state->data_size / header.byte_rate);

    // Read audio data
    state->audio_data = malloc(state->data_size);
    if (!state->audio_data) {
        printf("Error: Memory allocation failed\n");
        fclose(file);
        return 1;
    }
    size_t bytes_read = fread(state->audio_data, 1, state->data_size, file);
    if (bytes_read != state->data_size) {
        printf("Error: Failed to read audio data (%zu bytes read, expected %u)\n",
               bytes_read, state->data_size);
        free(state->audio_data);
        fclose(file);
        return 1;
    }
    fclose(file);

    // Initialize playback state
    state->offset = 0;
    state->sample_rate = header.sample_rate;
    state->num_channels = header.num_channels;
    state->bits_per_sample = header.bits_per_sample;
    state->output_channels = header.num_channels; // Default to WAV channels, updated by platform
    *duration = (float)state->data_size / header.byte_rate;

    return 0;
}

// Platform-specific audio playback
static int play_audio(PlaybackState *state, float duration) {
#ifdef __APPLE__
    // macOS: Use Core Audio
    AudioComponentInstance audioUnit;
    AudioComponentDescription desc = {
        .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_DefaultOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
        .componentFlags = 0,
        .componentFlagsMask = 0
    };

    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) {
        printf("Error: Cannot find audio component\n");
        free(state->audio_data);
        return 1;
    }
    OSStatus err = AudioComponentInstanceNew(comp, &audioUnit);
    if (err != noErr) {
        printf("Error: Failed to create audio unit instance (%d)\n", err);
        free(state->audio_data);
        return 1;
    }
    err = AudioUnitInitialize(audioUnit);
    if (err != noErr) {
        printf("Error: Failed to initialize audio unit (%d)\n", err);
        AudioComponentInstanceDispose(audioUnit);
        free(state->audio_data);
        return 1;
    }

    // Query device's preferred format
    AudioStreamBasicDescription device_asbd = {0};
    UInt32 size = sizeof(device_asbd);
    err = AudioUnitGetProperty(audioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &device_asbd, &size);
    if (err != noErr) {
        printf("Warning: Failed to get device stream format (%d), using default format\n", err);
        device_asbd.mSampleRate = 48000.0;
        device_asbd.mChannelsPerFrame = 2;
        device_asbd.mBitsPerChannel = 32;
        device_asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        device_asbd.mBytesPerFrame = 8;
        device_asbd.mBytesPerPacket = 8;
        device_asbd.mFramesPerPacket = 1;
        device_asbd.mFormatID = kAudioFormatLinearPCM;
    }
    printf("Device ASBD: sample_rate=%.0f, channels=%u, bits=%u, bytes_per_frame=%u, format=%s\n",
           device_asbd.mSampleRate, device_asbd.mChannelsPerFrame, device_asbd.mBitsPerChannel,
           device_asbd.mBytesPerFrame, (device_asbd.mFormatFlags & kAudioFormatFlagIsFloat) ? "float" : "integer");

    // Set stream format to match device (32-bit float)
    AudioStreamBasicDescription asbd = {
        .mSampleRate = device_asbd.mSampleRate,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
        .mBitsPerChannel = 32,
        .mChannelsPerFrame = device_asbd.mChannelsPerFrame,
        .mFramesPerPacket = 1,
        .mBytesPerFrame = device_asbd.mChannelsPerFrame * 4,
        .mBytesPerPacket = device_asbd.mChannelsPerFrame * 4
    };
    state->output_channels = asbd.mChannelsPerFrame;
    printf("ASBD: sample_rate=%.0f, channels=%u, bits=%u, bytes_per_frame=%u, format=float\n",
           asbd.mSampleRate, asbd.mChannelsPerFrame, asbd.mBitsPerChannel, asbd.mBytesPerFrame);

    err = AudioUnitSetProperty(audioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &asbd, sizeof(asbd));
    if (err != noErr) {
        printf("Error: Failed to set output stream format (%d)\n", err);
        AudioUnitUninitialize(audioUnit);
        AudioComponentInstanceDispose(audioUnit);
        free(state->audio_data);
        return 1;
    }
    err = AudioUnitSetProperty(audioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &asbd, sizeof(asbd));
    if (err != noErr) {
        printf("Error: Failed to set input stream format (%d)\n", err);
        AudioUnitUninitialize(audioUnit);
        AudioComponentInstanceDispose(audioUnit);
        free(state->audio_data);
        return 1;
    }

    // Set callback
    AURenderCallbackStruct callback = { .inputProc = audioCallback, .inputProcRefCon = state };
    err = AudioUnitSetProperty(audioUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback, sizeof(callback));
    if (err != noErr) {
        printf("Error: Failed to set render callback (%d)\n", err);
        AudioUnitUninitialize(audioUnit);
        AudioComponentInstanceDispose(audioUnit);
        free(state->audio_data);
        return 1;
    }

    // Start playback
    printf("Playing audio...\n");
    err = AudioOutputUnitStart(audioUnit);
    if (err != noErr) {
        printf("Error: Failed to start audio unit (%d)\n", err);
        AudioUnitUninitialize(audioUnit);
        AudioComponentInstanceDispose(audioUnit);
        free(state->audio_data);
        return 1;
    }

    // Wait for playback to finish
    printf("Expected duration: %.2f seconds\n", duration);
    while (state->offset < state->data_size) {
        usleep(100000); // Sleep 100ms
    }
    printf("Playback finished\n");

    // Cleanup
    AudioOutputUnitStop(audioUnit);
    AudioUnitUninitialize(audioUnit);
    AudioComponentInstanceDispose(audioUnit);
    free(state->audio_data);
    return 0;

#elif defined(_WIN32)
    // TODO: Implement Windows audio playback using DirectSound or WASAPI
    // Steps:
    // 1. Initialize DirectSound or WASAPI with device format (e.g., 44100 Hz, 16-bit PCM).
    // 2. Create a buffer for audio data.
    // 3. Copy state->audio_data to the buffer, handling format conversion if needed.
    // 4. Play the buffer and wait for completion.
    // 5. Clean up resources.
    printf("Error: Windows playback not implemented yet\n");
    free(state->audio_data);
    return 1;

#elif defined(__linux__)
    // TODO: Implement Linux audio playback using ALSA or PulseAudio
    // Steps:
    // 1. Open an ALSA/PulseAudio device with appropriate format (e.g., 44100 Hz, 16-bit PCM).
    // 2. Configure playback parameters (sample rate, channels, bit depth).
    // 3. Write state->audio_data to the device, handling format conversion if needed.
    // 4. Wait for playback to complete.
    // 5. Close the device and free resources.
    printf("Error: Linux playback not implemented yet\n");
    free(state->audio_data);
    return 1;

#else
    // TODO: Implement playback for other platforms (e.g., BSD, etc.)
    printf("Error: Unsupported platform\n");
    free(state->audio_data);
    return 1;
#endif
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <wav_file>\n", argv[0]);
        return 1;
    }

    PlaybackState state = {0};
    float duration = 0.0f;

    // Read WAV file
    if (read_wav_file(argv[1], &state, &duration) != 0) {
        return 1;
    }

    // Play audio (platform-specific)
    if (play_audio(&state, duration) != 0) {
        return 1;
    }

    return 0;
}
