#include "audio_input.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <SDL2/SDL.h>

// Ring buffer for audio input (to handle different buffer sizes and timing)
// Maximum buffer size: 500ms at 48kHz stereo
#define MAX_RING_BUFFER_SIZE (48000 / 2 * 2)  // 500ms of stereo audio at 48kHz

static int16_t *ring_buffer = NULL;
static int ring_buffer_size = 0;
static volatile int write_pos = 0;
static volatile int read_pos = 0;
static SDL_mutex *ring_mutex = NULL;

void audio_input_init(int buffer_ms) {
    if (!ring_mutex) {
        ring_mutex = SDL_CreateMutex();
    }

    // Clamp buffer size to reasonable range (10ms - 500ms)
    if (buffer_ms < 10) buffer_ms = 10;
    if (buffer_ms > 500) buffer_ms = 500;

    // Calculate buffer size: (48000 samples/sec * buffer_ms / 1000) * 2 channels
    ring_buffer_size = (48000 * buffer_ms / 1000) * 2;

    // Allocate buffer
    if (ring_buffer) {
        free(ring_buffer);
    }
    ring_buffer = (int16_t*)malloc(ring_buffer_size * sizeof(int16_t));
    if (!ring_buffer) {
        ring_buffer_size = 0;
        printf("Failed to allocate audio input ring buffer!\n");
        return;
    }

    write_pos = 0;
    read_pos = 0;
    memset(ring_buffer, 0, ring_buffer_size * sizeof(int16_t));

    printf("Audio input buffer initialized: %d ms (%d samples)\n", buffer_ms, ring_buffer_size);
}

void audio_input_cleanup(void) {
    if (ring_buffer) {
        free(ring_buffer);
        ring_buffer = NULL;
    }
    ring_buffer_size = 0;
    if (ring_mutex) {
        SDL_DestroyMutex(ring_mutex);
        ring_mutex = NULL;
    }
}

void audio_input_write(const int16_t *samples, int num_samples) {
    if (!ring_mutex || !ring_buffer || ring_buffer_size == 0) return;

    SDL_LockMutex(ring_mutex);

    for (int i = 0; i < num_samples; i++) {
        ring_buffer[write_pos] = samples[i];
        write_pos = (write_pos + 1) % ring_buffer_size;

        // If we're about to overwrite unread data, advance read position
        if (write_pos == read_pos) {
            read_pos = (read_pos + 1) % ring_buffer_size;
        }
    }

    SDL_UnlockMutex(ring_mutex);
}

int audio_input_read(int16_t *output, int num_samples) {
    if (!ring_mutex || !ring_buffer || ring_buffer_size == 0) return 0;

    SDL_LockMutex(ring_mutex);

    // Calculate available samples
    int available;
    if (write_pos >= read_pos) {
        available = write_pos - read_pos;
    } else {
        available = ring_buffer_size - read_pos + write_pos;
    }

    // Read at most what's requested or available
    int to_read = (num_samples < available) ? num_samples : available;

    for (int i = 0; i < to_read; i++) {
        output[i] = ring_buffer[read_pos];
        read_pos = (read_pos + 1) % ring_buffer_size;
    }

    SDL_UnlockMutex(ring_mutex);

    return to_read;
}

int audio_input_available(void) {
    if (!ring_mutex || !ring_buffer || ring_buffer_size == 0) return 0;

    SDL_LockMutex(ring_mutex);

    int available;
    if (write_pos >= read_pos) {
        available = write_pos - read_pos;
    } else {
        available = ring_buffer_size - read_pos + write_pos;
    }

    SDL_UnlockMutex(ring_mutex);

    return available;
}

void audio_input_reset(void) {
    if (!ring_mutex || !ring_buffer || ring_buffer_size == 0) return;

    SDL_LockMutex(ring_mutex);
    write_pos = 0;
    read_pos = 0;
    memset(ring_buffer, 0, ring_buffer_size * sizeof(int16_t));
    SDL_UnlockMutex(ring_mutex);
}
