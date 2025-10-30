#ifndef AUDIO_INPUT_H
#define AUDIO_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// Initialize audio input ring buffer with specified buffer size in milliseconds
// buffer_ms: Buffer size in milliseconds (recommended: 50-200ms, default: 100ms)
void audio_input_init(int buffer_ms);

// Cleanup audio input resources
void audio_input_cleanup(void);

// Write samples from input device to ring buffer (called by SDL input callback)
void audio_input_write(const int16_t *samples, int num_samples);

// Read samples from ring buffer for mixing (called by output callback)
// Returns number of samples actually read (may be less than requested if buffer underrun)
int audio_input_read(int16_t *output, int num_samples);

// Get number of samples available in ring buffer
int audio_input_available(void);

// Reset ring buffer (clear all data)
void audio_input_reset(void);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_INPUT_H
