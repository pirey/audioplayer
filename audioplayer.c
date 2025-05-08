#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#pragma pack(push, 1) // Ensure no padding in the structure
typedef struct {
	char chunkID[4]; // RIFF
	uint32_t chunkSize;
	char format[4]; // WAVE
} WAVHeader;

typedef struct {
	char subchunkID[4]; // fmt
	uint32_t subchunkSize;
	uint16_t audioFormat;
	uint16_t numChannels;
	uint32_t sampleRate;
	uint32_t byteRate;
	uint16_t blockAlign;
	uint16_t bitsPerSample;
} WAVFmtChunk;

typedef struct {
	char subchunkID[4]; // data
	uint32_t subchunkSize;
} WAVDataChunk;
#pragma pack(pop)

void read_wav_header(FILE *file) {
	WAVHeader header;
	fread(&header, sizeof(WAVHeader), 1, file);

	printf("Chunk ID: %.4s\n", header.chunkID);
	printf("Format: %.4s\n", header.format);
	printf("Chunk Size: %u\n", header.chunkSize);
}

void read_fmt_chunk(FILE *file) {
	WAVFmtChunk fmt;
	fread(&fmt, sizeof(WAVFmtChunk), 1, file);

	printf("Audio Format: %u\n", fmt.audioFormat);
	printf("Num Channels: %u\n", fmt.numChannels);
	printf("Sample Rate: %u\n", fmt.sampleRate);
	printf("Byte Rate: %u\n", fmt.byteRate);
	printf("Block Align: %u\n", fmt.blockAlign);
	printf("Bits Per Sample: %u\n", fmt.bitsPerSample);
}

void read_data_chunk(FILE *file) {
	WAVDataChunk data;
	fread(&data, sizeof(WAVDataChunk), 1, file);

	printf("Data Chunk Size: %u\n", data.subchunkSize);
}

int main() {
	FILE *file = fopen("test.wav", "rb");
	if (!file) {
		perror("Failed to open file.\n");
		return 1;
	}

	read_wav_header(file);
	read_fmt_chunk(file);
	read_data_chunk(file);

	fclose(file);
	return 0;
}
