#include <stdio.h>
#include <AudioToolbox/AudioToolbox.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

// WAV header structure (minimal for PCM)
typedef struct {
    char chunk_id[4]; // "RIFF"
    uint32_t chunk_size; // File size - 8
    char format[4]; // "WAVE"

    char subchunk1_id[4]; // "fmt "
    uint32_t subchunk1_size; // 16 for PCM, can be larger for non-PCM, this size is referring to the total size header fields below
    uint16_t audio_format; // 1 for PCM
    uint16_t num_channels; // 1 = mono, 2 = stereo
    uint32_t sample_rate; // e.g., 44100
    uint32_t byte_rate; // sample_rate * num_channels * bits_per_sample / 8
    uint16_t block_align; // num_channels * bits_per_sample / 8
    uint16_t bits_per_sample; // e.g., 16
} WavHeader;

// Playback state
typedef struct {
    uint8_t *audio_data; // Raw PCM data
    uint32_t data_size; // Total size of audio data
    uint32_t offset; // Current position in audio data
    uint32_t sample_rate; // For timing calculations
    uint16_t num_channels; // WAV file channels
    uint16_t bits_per_sample; // WAV file bits per sample
    uint16_t output_channels; // Device output channels
} PlaybackState;

// Global playback state
static PlaybackState playback_state = {0};

// Audio Unit callback to feed audio data
OSStatus audioCallback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags,
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

    // Convert 16-bit integer PCM to 32-bit float
    int16_t *src = (int16_t *)(state->audio_data + state->offset);
    float *dst = (float *)buffer->mData;
    uint32_t samples_per_channel = inNumberFrames;
    for (uint32_t i = 0; i < samples_per_channel; i++) {
        // Convert left and right channels (stereo)
        dst[i * 2] = src[i * 2] / 32768.0f;       // Left channel: [-32768, 32767] to [-1.0, 1.0]
        dst[i * 2 + 1] = src[i * 2 + 1] / 32768.0f; // Right channel
    }
    buffer->mDataByteSize = inNumberFrames * output_bytes_per_frame;
    state->offset += input_bytes_to_copy;

    return noErr;
}

int main() {
    // Open WAV file
    FILE *file = fopen("sample.wav", "rb"); // Replace with your WAV file path
    if (!file) {
        printf("Error: Cannot open file\n");
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
            playback_state.data_size = chunk_size;
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
    printf("  Data Size: %u bytes\n", playback_state.data_size);
    printf("  Duration: %.2f seconds\n", (float)playback_state.data_size / header.byte_rate);

    // Read audio data
    playback_state.audio_data = malloc(playback_state.data_size);
    if (!playback_state.audio_data) {
        printf("Error: Memory allocation failed\n");
        fclose(file);
        return 1;
    }
    size_t bytes_read = fread(playback_state.audio_data, 1, playback_state.data_size, file);
    if (bytes_read != playback_state.data_size) {
        printf("Error: Failed to read audio data (%zu bytes read, expected %u)\n",
               bytes_read, playback_state.data_size);
        free(playback_state.audio_data);
        fclose(file);
        return 1;
    }
    fclose(file);
    playback_state.offset = 0;
    playback_state.sample_rate = header.sample_rate;
    playback_state.num_channels = header.num_channels;
    playback_state.bits_per_sample = header.bits_per_sample;
    playback_state.output_channels = 2; // Assume stereo output

    // Set up Audio Unit
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
        free(playback_state.audio_data);
        return 1;
    }
    OSStatus err = AudioComponentInstanceNew(comp, &audioUnit);
    if (err != noErr) {
        printf("Error: Failed to create audio unit instance (%d)\n", err);
        free(playback_state.audio_data);
        return 1;
    }
    err = AudioUnitInitialize(audioUnit);
    if (err != noErr) {
        printf("Error: Failed to initialize audio unit (%d)\n", err);
        AudioComponentInstanceDispose(audioUnit);
        free(playback_state.audio_data);
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
        .mBytesPerFrame = device_asbd.mChannelsPerFrame * 4, // 32-bit float
        .mBytesPerPacket = device_asbd.mChannelsPerFrame * 4
    };
    playback_state.output_channels = asbd.mChannelsPerFrame;
    printf("ASBD: sample_rate=%.0f, channels=%u, bits=%u, bytes_per_frame=%u, format=float\n",
           asbd.mSampleRate, asbd.mChannelsPerFrame, asbd.mBitsPerChannel, asbd.mBytesPerFrame);

    err = AudioUnitSetProperty(audioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &asbd, sizeof(asbd));
    if (err != noErr) {
        printf("Error: Failed to set output stream format (%d)\n", err);
        AudioUnitUninitialize(audioUnit);
        AudioComponentInstanceDispose(audioUnit);
        free(playback_state.audio_data);
        return 1;
    }
    err = AudioUnitSetProperty(audioUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &asbd, sizeof(asbd));
    if (err != noErr) {
        printf("Error: Failed to set input stream format (%d)\n", err);
        AudioUnitUninitialize(audioUnit);
        AudioComponentInstanceDispose(audioUnit);
        free(playback_state.audio_data);
        return 1;
    }

    // Set callback
    AURenderCallbackStruct callback = { .inputProc = audioCallback, .inputProcRefCon = &playback_state };
    err = AudioUnitSetProperty(audioUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback, sizeof(callback));
    if (err != noErr) {
        printf("Error: Failed to set render callback (%d)\n", err);
        AudioUnitUninitialize(audioUnit);
        AudioComponentInstanceDispose(audioUnit);
        free(playback_state.audio_data);
        return 1;
    }

    // Start playback
    printf("Playing audio...\n");
    err = AudioOutputUnitStart(audioUnit);
    if (err != noErr) {
        printf("Error: Failed to start audio unit (%d)\n", err);
        AudioUnitUninitialize(audioUnit);
        AudioComponentInstanceDispose(audioUnit);
        free(playback_state.audio_data);
        return 1;
    }

    // Wait for playback to finish
    float duration = (float)playback_state.data_size / header.byte_rate;
    printf("Expected duration: %.2f seconds\n", duration);
    while (playback_state.offset < playback_state.data_size) {
        usleep(100000); // Sleep 100ms
    }
    printf("Playback finished\n");

    // Cleanup
    AudioOutputUnitStop(audioUnit);
    AudioUnitUninitialize(audioUnit);
    AudioComponentInstanceDispose(audioUnit);
    free(playback_state.audio_data);

    return 0;
}
