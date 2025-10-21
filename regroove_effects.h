#ifndef REGROOVE_EFFECTS_H
#define REGROOVE_EFFECTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Delay line size (1 second at 48kHz)
#define MAX_DELAY_SAMPLES 48000

// Effects chain structure
typedef struct {
    // Distortion parameters
    int distortion_enabled;
    float distortion_drive;    // 0.0 - 1.0
    float distortion_mix;      // 0.0 - 1.0 (dry/wet)

    // Filter parameters (simple resonant low-pass)
    int filter_enabled;
    float filter_cutoff;       // 0.0 - 1.0 (normalized frequency)
    float filter_resonance;    // 0.0 - 1.0 (Q factor)

    // 3-band EQ parameters
    int eq_enabled;
    float eq_low;              // 0.0 - 1.0 (100Hz boost/cut)
    float eq_mid;              // 0.0 - 1.0 (1kHz boost/cut)
    float eq_high;             // 0.0 - 1.0 (10kHz boost/cut)

    // Compressor parameters
    int compressor_enabled;
    float compressor_threshold; // 0.0 - 1.0
    float compressor_ratio;     // 0.0 - 1.0 (maps to 1:1 to 10:1)
    float compressor_attack;    // 0.0 - 1.0 (fast to slow)
    float compressor_release;   // 0.0 - 1.0 (fast to slow)
    float compressor_makeup;    // 0.0 - 1.0 (makeup gain)

    // Phaser parameters
    int phaser_enabled;
    float phaser_rate;         // 0.0 - 1.0 (LFO speed)
    float phaser_depth;        // 0.0 - 1.0 (modulation depth)
    float phaser_feedback;     // 0.0 - 1.0

    // Reverb parameters
    int reverb_enabled;
    float reverb_room_size;    // 0.0 - 1.0
    float reverb_damping;      // 0.0 - 1.0
    float reverb_mix;          // 0.0 - 1.0 (dry/wet)

    // Delay/Echo parameters
    int delay_enabled;
    float delay_time;          // 0.0 - 1.0 (maps to 0-1000ms)
    float delay_feedback;      // 0.0 - 1.0
    float delay_mix;           // 0.0 - 1.0 (dry/wet)

    // Internal state
    float filter_lp[2];        // Low-pass state (L, R)
    float filter_bp[2];        // Band-pass state (L, R)

    float distortion_hp[2];    // Distortion pre-emphasis highpass state
    float distortion_bp_lp[2]; // Distortion bandpass lowpass state
    float distortion_bp_bp[2]; // Distortion bandpass state
    float distortion_env[2];   // Distortion envelope follower state
    float distortion_lp[2];    // Distortion post-filter state

    float eq_lp1[2], eq_lp2[2]; // EQ filter states
    float eq_bp1[2], eq_bp2[2];
    float eq_hp1[2], eq_hp2[2];

    float compressor_envelope[2]; // Compressor envelope followers
    float compressor_rms[2];      // RMS state for smoother detection

    float phaser_lfo_phase;    // Phaser LFO phase
    float phaser_ap[4][2];     // Phaser all-pass filter states (4 stages, stereo)

    float reverb_comb[8][2];   // Reverb comb filter states (8 combs, stereo)
    int reverb_comb_pos[8];    // Comb filter read positions

    float *delay_buffer[2];    // Delay buffers (L, R)
    int delay_write_pos;       // Delay write position
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

void regroove_effects_set_eq_enabled(RegrooveEffects* fx, int enabled);
void regroove_effects_set_eq_low(RegrooveEffects* fx, float gain);     // 0.0 - 1.0
void regroove_effects_set_eq_mid(RegrooveEffects* fx, float gain);     // 0.0 - 1.0
void regroove_effects_set_eq_high(RegrooveEffects* fx, float gain);    // 0.0 - 1.0

void regroove_effects_set_compressor_enabled(RegrooveEffects* fx, int enabled);
void regroove_effects_set_compressor_threshold(RegrooveEffects* fx, float threshold);
void regroove_effects_set_compressor_ratio(RegrooveEffects* fx, float ratio);
void regroove_effects_set_compressor_attack(RegrooveEffects* fx, float attack);
void regroove_effects_set_compressor_release(RegrooveEffects* fx, float release);
void regroove_effects_set_compressor_makeup(RegrooveEffects* fx, float makeup);

void regroove_effects_set_phaser_enabled(RegrooveEffects* fx, int enabled);
void regroove_effects_set_phaser_rate(RegrooveEffects* fx, float rate);
void regroove_effects_set_phaser_depth(RegrooveEffects* fx, float depth);
void regroove_effects_set_phaser_feedback(RegrooveEffects* fx, float feedback);

void regroove_effects_set_reverb_enabled(RegrooveEffects* fx, int enabled);
void regroove_effects_set_reverb_room_size(RegrooveEffects* fx, float size);
void regroove_effects_set_reverb_damping(RegrooveEffects* fx, float damping);
void regroove_effects_set_reverb_mix(RegrooveEffects* fx, float mix);

void regroove_effects_set_delay_enabled(RegrooveEffects* fx, int enabled);
void regroove_effects_set_delay_time(RegrooveEffects* fx, float time);
void regroove_effects_set_delay_feedback(RegrooveEffects* fx, float feedback);
void regroove_effects_set_delay_mix(RegrooveEffects* fx, float mix);

// Parameter getters (normalized 0.0 - 1.0)
int regroove_effects_get_distortion_enabled(RegrooveEffects* fx);
float regroove_effects_get_distortion_drive(RegrooveEffects* fx);
float regroove_effects_get_distortion_mix(RegrooveEffects* fx);

int regroove_effects_get_filter_enabled(RegrooveEffects* fx);
float regroove_effects_get_filter_cutoff(RegrooveEffects* fx);
float regroove_effects_get_filter_resonance(RegrooveEffects* fx);

int regroove_effects_get_eq_enabled(RegrooveEffects* fx);
float regroove_effects_get_eq_low(RegrooveEffects* fx);
float regroove_effects_get_eq_mid(RegrooveEffects* fx);
float regroove_effects_get_eq_high(RegrooveEffects* fx);

int regroove_effects_get_compressor_enabled(RegrooveEffects* fx);
float regroove_effects_get_compressor_threshold(RegrooveEffects* fx);
float regroove_effects_get_compressor_ratio(RegrooveEffects* fx);
float regroove_effects_get_compressor_attack(RegrooveEffects* fx);
float regroove_effects_get_compressor_release(RegrooveEffects* fx);
float regroove_effects_get_compressor_makeup(RegrooveEffects* fx);

int regroove_effects_get_phaser_enabled(RegrooveEffects* fx);
float regroove_effects_get_phaser_rate(RegrooveEffects* fx);
float regroove_effects_get_phaser_depth(RegrooveEffects* fx);
float regroove_effects_get_phaser_feedback(RegrooveEffects* fx);

int regroove_effects_get_reverb_enabled(RegrooveEffects* fx);
float regroove_effects_get_reverb_room_size(RegrooveEffects* fx);
float regroove_effects_get_reverb_damping(RegrooveEffects* fx);
float regroove_effects_get_reverb_mix(RegrooveEffects* fx);

int regroove_effects_get_delay_enabled(RegrooveEffects* fx);
float regroove_effects_get_delay_time(RegrooveEffects* fx);
float regroove_effects_get_delay_feedback(RegrooveEffects* fx);
float regroove_effects_get_delay_mix(RegrooveEffects* fx);

#ifdef __cplusplus
}
#endif

#endif // REGROOVE_EFFECTS_H
