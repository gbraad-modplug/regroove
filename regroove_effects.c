#include "regroove_effects.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Helper: clamp float value
static inline float clampf(float v, float min, float max) {
    if (v < min) return min;
    if (v > max) return max;
    return v;
}

// Helper: RB338-style overdrive with tube-like saturation
static inline float overdrive_saturate(float x) {
    // Smooth saturation curve (similar to tube overdrive)
    // Uses a cubic soft-clip that transitions to hard limit
    float ax = fabsf(x);

    if (ax < 0.33f) {
        // Clean zone - linear
        return x;
    } else if (ax < 1.0f) {
        // Soft saturation zone (tube-like)
        float sign = (x > 0.0f) ? 1.0f : -1.0f;
        float t = ax - 0.33f;
        // Smooth compression curve
        return sign * (0.33f + t * (1.0f - t * 0.5f));
    } else {
        // Hard clipping at ±1
        return (x > 0.0f) ? 1.0f : -1.0f;
    }
}

RegrooveEffects* regroove_effects_create(void) {
    RegrooveEffects* fx = (RegrooveEffects*)calloc(1, sizeof(RegrooveEffects));
    if (!fx) return NULL;

    // Default parameters
    fx->distortion_enabled = 0;
    fx->distortion_drive = 0.5f;  // 50% drive by default
    fx->distortion_mix = 0.5f;

    fx->filter_enabled = 0;
    fx->filter_cutoff = 1.0f;    // Fully open by default
    fx->filter_resonance = 0.0f; // No resonance

    // Clear filter state
    memset(fx->filter_lp, 0, sizeof(fx->filter_lp));
    memset(fx->filter_bp, 0, sizeof(fx->filter_bp));

    return fx;
}

void regroove_effects_destroy(RegrooveEffects* fx) {
    if (fx) {
        free(fx);
    }
}

void regroove_effects_reset(RegrooveEffects* fx) {
    if (!fx) return;

    // Clear filter state
    memset(fx->filter_lp, 0, sizeof(fx->filter_lp));
    memset(fx->filter_bp, 0, sizeof(fx->filter_bp));
}

void regroove_effects_process(RegrooveEffects* fx, int16_t* buffer, int frames, int sample_rate) {
    if (!fx || !buffer || frames <= 0) return;

    // Convert to float for processing
    const float scale_to_float = 1.0f / 32768.0f;
    const float scale_to_int16 = 32767.0f;

    for (int i = 0; i < frames; i++) {
        // Get stereo samples
        float left = (float)buffer[i * 2] * scale_to_float;
        float right = (float)buffer[i * 2 + 1] * scale_to_float;

        // --- DISTORTION (RB338-style overdrive) ---
        if (fx->distortion_enabled) {
            float dry_left = left;
            float dry_right = right;

            // Drive amount: 0.0 = 1x (clean), 1.0 = 10x (heavy overdrive)
            float drive_amount = 1.0f + fx->distortion_drive * 9.0f;

            // Apply pre-gain
            float driven_left = left * drive_amount;
            float driven_right = right * drive_amount;

            // Apply saturation/clipping
            float saturated_left = overdrive_saturate(driven_left);
            float saturated_right = overdrive_saturate(driven_right);

            // Makeup gain to compensate for drive (prevents output from getting quieter)
            // Higher drive needs less makeup (signal is compressed/clipped)
            float makeup = 1.0f / (1.0f + fx->distortion_drive * 0.5f);
            float wet_left = saturated_left * makeup;
            float wet_right = saturated_right * makeup;

            // Mix dry/wet
            left = dry_left * (1.0f - fx->distortion_mix) + wet_left * fx->distortion_mix;
            right = dry_right * (1.0f - fx->distortion_mix) + wet_right * fx->distortion_mix;
        }

        // --- RESONANT LOW-PASS FILTER ---
        if (fx->filter_enabled) {
            // Simple state-variable filter (Chamberlin)
            // Normalized cutoff to actual frequency (linear mapping)
            float nyquist = sample_rate * 0.5f;
            float freq = fx->filter_cutoff * nyquist * 0.48f; // Linear for predictable response
            float f = 2.0f * sinf(3.14159265f * freq / (float)sample_rate);

            // Resonance (Q) - limit range for stability
            // 0.0 resonance = q of 0.7 (gentle)
            // 1.0 resonance = q of 0.1 (strong but stable)
            float q = 0.7f - fx->filter_resonance * 0.6f;
            if (q < 0.1f) q = 0.1f;

            // Process left channel
            fx->filter_lp[0] += f * fx->filter_bp[0];
            float hp = left - fx->filter_lp[0] - q * fx->filter_bp[0];
            fx->filter_bp[0] += f * hp;
            left = fx->filter_lp[0];

            // Process right channel
            fx->filter_lp[1] += f * fx->filter_bp[1];
            hp = right - fx->filter_lp[1] - q * fx->filter_bp[1];
            fx->filter_bp[1] += f * hp;
            right = fx->filter_lp[1];
        }

        // Convert back to int16 with clamping
        buffer[i * 2] = (int16_t)clampf(left * scale_to_int16, -32768.0f, 32767.0f);
        buffer[i * 2 + 1] = (int16_t)clampf(right * scale_to_int16, -32768.0f, 32767.0f);
    }
}

// Parameter setters
void regroove_effects_set_distortion_enabled(RegrooveEffects* fx, int enabled) {
    if (fx) fx->distortion_enabled = enabled;
}

void regroove_effects_set_distortion_drive(RegrooveEffects* fx, float drive) {
    if (fx) {
        // Store normalized 0.0-1.0 directly
        fx->distortion_drive = clampf(drive, 0.0f, 1.0f);
    }
}

void regroove_effects_set_distortion_mix(RegrooveEffects* fx, float mix) {
    if (fx) fx->distortion_mix = clampf(mix, 0.0f, 1.0f);
}

void regroove_effects_set_filter_enabled(RegrooveEffects* fx, int enabled) {
    if (fx) fx->filter_enabled = enabled;
}

void regroove_effects_set_filter_cutoff(RegrooveEffects* fx, float cutoff) {
    if (fx) fx->filter_cutoff = clampf(cutoff, 0.0f, 1.0f);
}

void regroove_effects_set_filter_resonance(RegrooveEffects* fx, float resonance) {
    if (fx) fx->filter_resonance = clampf(resonance, 0.0f, 1.0f);
}

// Parameter getters
int regroove_effects_get_distortion_enabled(RegrooveEffects* fx) {
    return fx ? fx->distortion_enabled : 0;
}

float regroove_effects_get_distortion_drive(RegrooveEffects* fx) {
    return fx ? fx->distortion_drive : 0.0f;
}

float regroove_effects_get_distortion_mix(RegrooveEffects* fx) {
    return fx ? fx->distortion_mix : 0.0f;
}

int regroove_effects_get_filter_enabled(RegrooveEffects* fx) {
    return fx ? fx->filter_enabled : 0;
}

float regroove_effects_get_filter_cutoff(RegrooveEffects* fx) {
    return fx ? fx->filter_cutoff : 0.0f;
}

float regroove_effects_get_filter_resonance(RegrooveEffects* fx) {
    return fx ? fx->filter_resonance : 0.0f;
}
