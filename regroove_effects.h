#ifndef REGROOVE_EFFECTS_H
#define REGROOVE_EFFECTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Effects chain structure
typedef struct {
    // Distortion parameters
    int distortion_enabled;
    float distortion_drive;    // 1.0 - 10.0 (amount of distortion)
    float distortion_mix;      // 0.0 - 1.0 (dry/wet)

    // Filter parameters (simple resonant low-pass)
    int filter_enabled;
    float filter_cutoff;       // 0.0 - 1.0 (normalized frequency)
    float filter_resonance;    // 0.0 - 1.0 (Q factor)

    // Internal filter state (per channel)
    float filter_lp[2];        // Low-pass state (L, R)
    float filter_bp[2];        // Band-pass state (L, R)
} RegrooveEffects;

// Initialize effects with default parameters
RegrooveEffects* regroove_effects_create(void);

// Free effects
void regroove_effects_destroy(RegrooveEffects* fx);

// Reset effect state (clear filter memory, etc.)
void regroove_effects_reset(RegrooveEffects* fx);

// Process audio buffer through effects chain
// buffer: interleaved stereo int16 samples (L, R, L, R, ...)
// frames: number of stereo frames
// sample_rate: sample rate in Hz
void regroove_effects_process(RegrooveEffects* fx, int16_t* buffer, int frames, int sample_rate);

// Parameter setters (normalized 0.0 - 1.0 for MIDI mapping)
void regroove_effects_set_distortion_enabled(RegrooveEffects* fx, int enabled);
void regroove_effects_set_distortion_drive(RegrooveEffects* fx, float drive);   // 0.0 - 1.0
void regroove_effects_set_distortion_mix(RegrooveEffects* fx, float mix);       // 0.0 - 1.0

void regroove_effects_set_filter_enabled(RegrooveEffects* fx, int enabled);
void regroove_effects_set_filter_cutoff(RegrooveEffects* fx, float cutoff);     // 0.0 - 1.0
void regroove_effects_set_filter_resonance(RegrooveEffects* fx, float resonance); // 0.0 - 1.0

// Parameter getters (normalized 0.0 - 1.0)
int regroove_effects_get_distortion_enabled(RegrooveEffects* fx);
float regroove_effects_get_distortion_drive(RegrooveEffects* fx);
float regroove_effects_get_distortion_mix(RegrooveEffects* fx);

int regroove_effects_get_filter_enabled(RegrooveEffects* fx);
float regroove_effects_get_filter_cutoff(RegrooveEffects* fx);
float regroove_effects_get_filter_resonance(RegrooveEffects* fx);

#ifdef __cplusplus
}
#endif

#endif // REGROOVE_EFFECTS_H
