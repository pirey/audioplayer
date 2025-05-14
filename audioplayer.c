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
    uint16_t num_channels; // Any positive number
    uint32_t sample_rate; // e.g., 44100
    uint32_t byte_rate; // sample_rate * num_channels * bits_per_sample / 8
    uint16_t block_align; // num_channels * bits_per_sample / 8
    uint16_t bits_per_sample; // e.g., 8, 16, 24, 32
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
    uint16_t is_float; // 1 for float PCM, 0 for integer
    uint16_t output_bits_per_channel; // Device bits per channel
    uint16_t output_is_float; // 1 for float output, 0 for integer
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

    // Calculate bytes to copy
    uint32_t input_bytes_per_frame = state->num_channels * (state->bits_per_sample / 8);
    uint32_t output_bytes_per_frame = state->output_channels * (state->output_bits_per_channel / 8);
    uint32_t input_bytes_to_copy = inNumberFrames * input_bytes_per_frame;
    if (state->offset + input_bytes_to_copy > state->data_size) {
        input_bytes_to_copy = state->data_size - state->offset;
        inNumberFrames = input_bytes_to_copy / input_bytes_per_frame;
    }

    // Convert PCM to device format, with channel mapping
    uint32_t samples_per_channel = inNumberFrames;
    uint16_t out_channels = state->output_channels;
    uint16_t in_channels = state->num_channels;

    // Convert input to float for intermediate processing
    float *temp_buffer = malloc(samples_per_channel * in_channels * sizeof(float));
    if (!temp_buffer) {
        printf("Error: Callback memory allocation failed\n");
        buffer->mDataByteSize = 0;
        return noErr;
    }

    if (state->is_float) {
        // 32-bit float PCM
        float *src = (float *)(state->audio_data + state->offset);
        for (uint32_t i = 0; i < samples_per_channel; i++) {
            for (uint16_t ch = 0; ch < in_channels; ch++) {
                temp_buffer[i * in_channels + ch] = src[i * in_channels + ch];
            }
        }
    } else if (state->bits_per_sample == 8) {
        // 8-bit unsigned integer PCM
        uint8_t *src = (uint8_t *)(state->audio_data + state->offset);
        for (uint32_t i = 0; i < samples_per_channel; i++) {
            for (uint16_t ch = 0; ch < in_channels; ch++) {
                temp_buffer[i * in_channels + ch] = (src[i * in_channels + ch] - 128) / 128.0f;
            }
        }
    } else if (state->bits_per_sample == 16) {
        // 16-bit signed integer PCM
        int16_t *src = (int16_t *)(state->audio_data + state->offset);
        for (uint32_t i = 0; i < samples_per_channel; i++) {
            for (uint16_t ch = 0; ch < in_channels; ch++) {
                temp_buffer[i * in_channels + ch] = src[i * in_channels + ch] / 32768.0f;
            }
        }
    } else if (state->bits_per_sample == 24) {
        // 24-bit signed integer PCM
        uint8_t *src = state->audio_data + state->offset;
        for (uint32_t i = 0; i < samples_per_channel; i++) {
            for (uint16_t ch = 0; ch < in_channels; ch++) {
                int32_t val = (int32_t)(src[i * input_bytes_per_frame + ch * 3]) |
                              (int32_t)(src[i * input_bytes_per_frame + ch * 3 + 1] << 8) |
                              (int32_t)((int8_t)src[i * input_bytes_per_frame + ch * 3 + 2] << 16);
                temp_buffer[i * in_channels + ch] = val / 8388608.0f;
            }
        }
    } else if (state->bits_per_sample == 32) {
        // 32-bit signed integer PCM
        int32_t *src = (int32_t *)(state->audio_data + state->offset);
        for (uint32_t i = 0; i < samples_per_channel; i++) {
            for (uint16_t ch = 0; ch < in_channels; ch++) {
                temp_buffer[i * in_channels + ch] = src[i * in_channels + ch] / 2147483648.0f;
            }
        }
    } else {
        // Unsupported bit depth
        printf("Warning: Unsupported bit depth %u, outputting silence\n", state->bits_per_sample);
        memset(temp_buffer, 0, samples_per_channel * in_channels * sizeof(float));
    }

    // Convert float to device format with channel mapping
    if (state->output_is_float) {
        // Float output
        float *dst = (float *)buffer->mData;
        for (uint32_t i = 0; i < samples_per_channel; i++) {
            for (uint16_t ch = 0; ch < out_channels; ch++) {
                float sample = 0.0f;
                if (ch < in_channels) {
                    sample = temp_buffer[i * in_channels + ch];
                } else if (in_channels == 1) {
                    sample = temp_buffer[i];
                } else {
                    for (uint16_t in_ch = 0; in_ch < in_channels; in_ch++) {
                        sample += temp_buffer[i * in_channels + in_ch] / in_channels;
                    }
                }
                dst[i * out_channels + ch] = sample;
            }
        }
    } else if (state->output_bits_per_channel == 16) {
        // 16-bit integer output
        int16_t *dst = (int16_t *)buffer->mData;
        for (uint32_t i = 0; i < samples_per_channel; i++) {
            for (uint16_t ch = 0; ch < out_channels; ch++) {
                float sample = 0.0f;
                if (ch < in_channels) {
                    sample = temp_buffer[i * in_channels + ch];
                } else if (in_channels == 1) {
                    sample = temp_buffer[i];
                } else {
                    for (uint16_t in_ch = 0; in_ch < in_channels; in_ch++) {
                        sample += temp_buffer[i * in_channels + in_ch] / in_channels;
                    }
                }
                // Clip to [-1.0, 1.0] and scale to 16-bit
                if (sample > 1.0f) sample = 1.0f;
                if (sample < -1.0f) sample = -1.0f;
                dst[i * out_channels + ch] = (int16_t)(sample * 32767.0f);
            }
        }
    } else if (state->output_bits_per_channel == 24) {
        // 24-bit integer output
        uint8_t *dst = (uint8_t *)buffer->mData;
        for (uint32_t i = 0; i < samples_per_channel; i++) {
            for (uint16_t ch = 0; ch < out_channels; ch++) {
                float sample = 0.0f;
                if (ch < in_channels) {
                    sample = temp_buffer[i * in_channels + ch];
                } else if (in_channels == 1) {
                    sample = temp_buffer[i];
                } else {
                    for (uint16_t in_ch = 0; in_ch < in_channels; in_ch++) {
                        sample += temp_buffer[i * in_channels + in_ch] / in_channels;
                    }
                }
                if (sample > 1.0f) sample = 1.0f;
                if (sample < -1.0f) sample = -1.0f;
                int32_t val = (int32_t)(sample * 8388607.0f);
                dst[i * output_bytes_per_frame + ch * 3] = val & 0xFF;
                dst[i * output_bytes_per_frame + ch * 3 + 1] = (val >> 8) & 0xFF;
                dst[i * output_bytes_per_frame + ch * 3 + 2] = (val >> 16) & 0xFF;
            }
        }
    } else if (state->output_bits_per_channel == 32) {
        // 32-bit integer output
        int32_t *dst = (int32_t *)buffer->mData;
        for (uint32_t i = 0; i < samples_per_channel; i++) {
            for (uint16_t ch = 0; ch < out_channels; ch++) {
                float sample = 0.0f;
                if (ch < in_channels) {
                    sample = temp_buffer[i * in_channels + ch];
                } else if (in_channels == 1) {
                    sample = temp_buffer[i];
                } else {
                    for (uint16_t in_ch = 0; in_ch < in_channels; in_ch++) {
                        sample += temp_buffer[i * in_channels + in_ch] / in_channels;
                    }
                }
                if (sample > 1.0f) sample = 1.0f;
                if (sample < -1.0f) sample = -1.0f;
                dst[i * out_channels + ch] = (int32_t)(sample * 2147483647.0f);
            }
        }
    } else {
        // Unsupported output bit depth
        printf("Warning: Unsupported output bit depth %u, outputting silence\n", state->output_bits_per_channel);
        memset(buffer->mData, 0, inNumberFrames * output_bytes_per_frame);
    }

    free(temp_buffer);
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
    state->is_float = 0;
    if (header.audio_format == 1) {
        // PCM (integer)
        if (header.bits_per_sample % 8 != 0) {
            printf("Error: Bits per sample %u must be a multiple of 8\n", header.bits_per_sample);
            fclose(file);
            return 1;
        }
    } else if (header.audio_format == 3 && header.bits_per_sample == 32) {
        // IEEE Float (32-bit float)
        state->is_float = 1;
    } else {
        printf("Error: Only PCM or 32-bit float WAV files are supported (audio_format=%u)\n", header.audio_format);
        fclose(file);
        return 1;
    }
    if (header.num_channels < 1) {
        printf("Error: Invalid number of channels %u\n", header.num_channels);
        fclose(file);
        return 1;
    }
    if (header.sample_rate < 8000 || header.sample_rate > 96000) {
        printf("Error: Sample rate %u Hz is not supported (must be 8000–96000 Hz)\n", header.sample_rate);
        fclose(file);
        return 1;
    }
    if (header.byte_rate != header.sample_rate * header.num_channels * (header.bits_per_sample / 8)) {
        printf("Error: Invalid byte rate %u (expected %u)\n", header.byte_rate,
               header.sample_rate * header.num_channels * (header.bits_per_sample / 8));
        fclose(file);
        return 1;
    }
    if (header.block_align != header.num_channels * (header.bits_per_sample / 8)) {
        printf("Error: Invalid block align %u (expected %u)\n", header.block_align,
               header.num_channels * (header.bits_per_sample / 8));
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
    printf("  Channels: %u\n", header.num_channels);
    printf("  Bits per Sample: %u\n", header.bits_per_sample);
    printf("  Format: %s\n", state->is_float ? "Float" : "Integer PCM");
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
    state->output_channels = header.num_channels; // Default, updated by platform
    state->output_bits_per_channel = 16; // Default, updated by platform
    state->output_is_float = 0; // Default, updated by platform
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
        device_asbd.mBitsPerChannel = 16;
        device_asbd.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        device_asbd.mBytesPerFrame = 2 * device_asbd.mChannelsPerFrame;
        device_asbd.mBytesPerPacket = device_asbd.mBytesPerFrame;
        device_asbd.mFramesPerPacket = 1;
        device_asbd.mFormatID = kAudioFormatLinearPCM;
    }
    printf("Device ASBD: sample_rate=%.0f, channels=%u, bits=%u, bytes_per_frame=%u, format=%s\n",
           device_asbd.mSampleRate, device_asbd.mChannelsPerFrame, device_asbd.mBitsPerChannel,
           device_asbd.mBytesPerFrame, (device_asbd.mFormatFlags & kAudioFormatFlagIsFloat) ? "float" :
                                       (device_asbd.mFormatFlags & kAudioFormatFlagIsSignedInteger) ? "signed integer" : "unsigned integer");

    // Validate device format
    if (device_asbd.mFormatID != kAudioFormatLinearPCM) {
        printf("Error: Device format is not linear PCM\n");
        AudioUnitUninitialize(audioUnit);
        AudioComponentInstanceDispose(audioUnit);
        free(state->audio_data);
        return 1;
    }
    if (!(device_asbd.mFormatFlags & kAudioFormatFlagIsFloat) &&
        !(device_asbd.mFormatFlags & kAudioFormatFlagIsSignedInteger)) {
        printf("Error: Device format must be float or signed integer\n");
        AudioUnitUninitialize(audioUnit);
        AudioComponentInstanceDispose(audioUnit);
        free(state->audio_data);
        return 1;
    }
    if (device_asbd.mBitsPerChannel != 16 && device_asbd.mBitsPerChannel != 24 && device_asbd.mBitsPerChannel != 32) {
        printf("Error: Unsupported device bit depth %u\n", device_asbd.mBitsPerChannel);
        AudioUnitUninitialize(audioUnit);
        AudioComponentInstanceDispose(audioUnit);
        free(state->audio_data);
        return 1;
    }

    // Set stream format to match device
    AudioStreamBasicDescription asbd = {
        .mSampleRate = device_asbd.mSampleRate,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = device_asbd.mFormatFlags,
        .mBitsPerChannel = device_asbd.mBitsPerChannel,
        .mChannelsPerFrame = device_asbd.mChannelsPerFrame,
        .mFramesPerPacket = 1,
        .mBytesPerFrame = device_asbd.mChannelsPerFrame * (device_asbd.mBitsPerChannel / 8),
        .mBytesPerPacket = device_asbd.mChannelsPerFrame * (device_asbd.mBitsPerChannel / 8)
    };
    if (state->num_channels > device_asbd.mChannelsPerFrame) {
        printf("Warning: WAV has %u channels, downmixing to %u channels\n",
               state->num_channels, device_asbd.mChannelsPerFrame);
    }
    state->output_channels = asbd.mChannelsPerFrame;
    state->output_bits_per_channel = asbd.mBitsPerChannel;
    state->output_is_float = (asbd.mFormatFlags & kAudioFormatFlagIsFloat) ? 1 : 0;
    printf("ASBD: sample_rate=%.0f, channels=%u, bits=%u, bytes_per_frame=%u, format=%s\n",
           asbd.mSampleRate, asbd.mChannelsPerFrame, asbd.mBitsPerChannel, asbd.mBytesPerFrame,
           state->output_is_float ? "float" : "signed integer");

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
    // 1. Query device format (e.g., WASAPI: GetMixFormat for sample rate, bits per channel, channels).
    // 2. Initialize device with queried format (e.g., 16-bit integer, 44.1 kHz, stereo).
    // 3. Create a buffer for audio data.
    // 4. Convert state->audio_data to device format (any bit depth, channel count).
    // 5. Play the buffer and wait for completion.
    // 6. Clean up resources.
    printf("Error: Windows playback not implemented yet\n");
    free(state->audio_data);
    return 1;

#elif defined(__linux__)
    // TODO: Implement Linux audio playback using ALSA or PulseAudio
    // Steps:
    // 1. Query device format (e.g., ALSA: snd_pcm_hw_params_get_format, PulseAudio: pa_get_sample_spec).
    // 2. Open device with queried format (e.g., 24-bit integer, 48 kHz, 2 channels).
    // 3. Configure playback parameters (sample rate, channels, bit depth).
    // 4. Convert state->audio_data to device format (any bit depth, channel count).
    // 5. Write to device and wait for completion.
    // 6. Close device and free resources.
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
