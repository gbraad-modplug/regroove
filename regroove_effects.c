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

// Helper: Foldback distortion for aggressive harmonics
static inline float foldback(float x) {
    const float threshold = 1.0f;
    if (x > threshold) {
        x = threshold - fmodf(x - threshold, threshold * 2.0f);
    } else if (x < -threshold) {
        x = -threshold + fmodf(-threshold - x, threshold * 2.0f);
    }
    return x;
}

// Helper: RB338-style aggressive asymmetric distortion for 909 kicks
static inline float rb338_shaper(float x) {
    // Asymmetric waveshaping - emphasizes attack on positive side
    // More aggressive on positive (kick transients), softer on negative
    if (x > 0.0f) {
        return tanhf(x * 1.5f);  // Aggressive positive
    } else {
        return tanhf(x * 0.5f);  // Softer negative
    }
}

// Helper: Simple one-pole highpass filter for pre-emphasis
static inline float highpass_tick(float input, float *state, float cutoff_norm) {
    float alpha = 1.0f - expf(-2.0f * 3.14159f * cutoff_norm);
    *state += alpha * (input - *state);
    return input - *state;
}

// Helper: Simple resonant bandpass bump (for punch at 120Hz)
static inline float bandpass_bump(float input, float *lp_state, float *bp_state, float freq_norm, float q) {
    // State-variable filter bandpass output
    float f = 2.0f * sinf(3.14159f * freq_norm);
    *lp_state += f * *bp_state;
    float hp = input - *lp_state - q * *bp_state;
    *bp_state += f * hp;
    return *bp_state;
}

// Helper: Envelope follower for dynamic drive
static inline float envelope_follower(float input, float *state, float attack, float release) {
    float level = fabsf(input);
    float coeff = (level > *state) ? attack : release;
    *state += coeff * (level - *state);
    return *state;
}

RegrooveEffects* regroove_effects_create(void) {
    RegrooveEffects* fx = (RegrooveEffects*)calloc(1, sizeof(RegrooveEffects));
    if (!fx) return NULL;

    // Allocate delay buffers
    fx->delay_buffer[0] = (float*)calloc(MAX_DELAY_SAMPLES, sizeof(float));
    fx->delay_buffer[1] = (float*)calloc(MAX_DELAY_SAMPLES, sizeof(float));
    if (!fx->delay_buffer[0] || !fx->delay_buffer[1]) {
        free(fx->delay_buffer[0]);
        free(fx->delay_buffer[1]);
        free(fx);
        return NULL;
    }

    // Default parameters
    fx->distortion_enabled = 0;
    fx->distortion_drive = 0.5f;
    fx->distortion_mix = 0.5f;

    fx->filter_enabled = 0;
    fx->filter_cutoff = 1.0f;
    fx->filter_resonance = 0.0f;

    fx->eq_enabled = 0;
    fx->eq_low = 0.5f;
    fx->eq_mid = 0.5f;
    fx->eq_high = 0.5f;

    fx->compressor_enabled = 0;
    fx->compressor_threshold = 0.4f;  // ~0.20 linear = ~-14dB (moderate)
    fx->compressor_ratio = 0.4f;      // ~8:1 (noticeable but not crazy)
    fx->compressor_attack = 0.05f;    // Fast attack for transients
    fx->compressor_release = 0.5f;    // Slower release to prevent pumping
    fx->compressor_makeup = 0.65f;    // ~2x gain (gentle boost)

    fx->phaser_enabled = 0;
    fx->phaser_rate = 0.3f;
    fx->phaser_depth = 0.5f;
    fx->phaser_feedback = 0.3f;

    fx->reverb_enabled = 0;
    fx->reverb_room_size = 0.5f;
    fx->reverb_damping = 0.5f;
    fx->reverb_mix = 0.3f;

    fx->delay_enabled = 0;
    fx->delay_time = 0.375f;  // ~375ms
    fx->delay_feedback = 0.4f;
    fx->delay_mix = 0.3f;

    return fx;
}

void regroove_effects_destroy(RegrooveEffects* fx) {
    if (fx) {
        free(fx->delay_buffer[0]);
        free(fx->delay_buffer[1]);
        free(fx);
    }
}

void regroove_effects_reset(RegrooveEffects* fx) {
    if (!fx) return;

    // Clear filter state
    memset(fx->filter_lp, 0, sizeof(fx->filter_lp));
    memset(fx->filter_bp, 0, sizeof(fx->filter_bp));

    // Clear distortion state
    memset(fx->distortion_hp, 0, sizeof(fx->distortion_hp));
    memset(fx->distortion_bp_lp, 0, sizeof(fx->distortion_bp_lp));
    memset(fx->distortion_bp_bp, 0, sizeof(fx->distortion_bp_bp));
    memset(fx->distortion_env, 0, sizeof(fx->distortion_env));
    memset(fx->distortion_lp, 0, sizeof(fx->distortion_lp));

    // Clear EQ state
    memset(fx->eq_lp1, 0, sizeof(fx->eq_lp1));
    memset(fx->eq_lp2, 0, sizeof(fx->eq_lp2));
    memset(fx->eq_bp1, 0, sizeof(fx->eq_bp1));
    memset(fx->eq_bp2, 0, sizeof(fx->eq_bp2));
    memset(fx->eq_hp1, 0, sizeof(fx->eq_hp1));
    memset(fx->eq_hp2, 0, sizeof(fx->eq_hp2));

    // Clear compressor state
    memset(fx->compressor_envelope, 0, sizeof(fx->compressor_envelope));
    memset(fx->compressor_rms, 0, sizeof(fx->compressor_rms));

    // Clear delay buffers and reset write position
    if (fx->delay_buffer[0]) {
        memset(fx->delay_buffer[0], 0, MAX_DELAY_SAMPLES * sizeof(float));
    }
    if (fx->delay_buffer[1]) {
        memset(fx->delay_buffer[1], 0, MAX_DELAY_SAMPLES * sizeof(float));
    }
    fx->delay_write_pos = 0;
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

        // --- DISTORTION (RB338-style aggressive overdrive for 909 kicks) ---
        if (fx->distortion_enabled) {
            float dry_left = left;
            float dry_right = right;

            // Pre-emphasis EQ chain:
            // 1. Highpass at 80Hz to remove sub-rumble
            float hp_cutoff = 80.0f / sample_rate;
            float emphasized_left = highpass_tick(left, &fx->distortion_hp[0], hp_cutoff);
            float emphasized_right = highpass_tick(right, &fx->distortion_hp[1], hp_cutoff);

            // 2. Add resonant bandpass bump at 120Hz for punch (909 kick fundamental)
            float bp_freq = 120.0f / sample_rate;
            float bp_q = 0.5f;  // Resonance for punch
            float bp_left = bandpass_bump(emphasized_left, &fx->distortion_bp_lp[0],
                                         &fx->distortion_bp_bp[0], bp_freq, bp_q);
            float bp_right = bandpass_bump(emphasized_right, &fx->distortion_bp_lp[1],
                                          &fx->distortion_bp_bp[1], bp_freq, bp_q);

            // Mix in the punch bump
            emphasized_left += bp_left * 0.5f;
            emphasized_right += bp_right * 0.5f;

            // Dynamic envelope detection for transient emphasis
            float attack_coeff = 0.9f;   // Fast attack
            float release_coeff = 0.001f; // Slow release
            float env_l = envelope_follower(emphasized_left, &fx->distortion_env[0], attack_coeff, release_coeff);
            float env_r = envelope_follower(emphasized_right, &fx->distortion_env[1], attack_coeff, release_coeff);

            // Dynamic drive: more aggressive on transients (kicks, snares)
            // Drive amount: 0.0 = 1x, 1.0 = 8x
            float base_drive = 1.0f + fx->distortion_drive * 7.0f;
            float dynamic_drive_l = base_drive * (0.7f + env_l * 0.6f);
            float dynamic_drive_r = base_drive * (0.7f + env_r * 0.6f);

            // Apply drive gain
            float driven_left = emphasized_left * dynamic_drive_l;
            float driven_right = emphasized_right * dynamic_drive_r;

            // Aggressive distortion chain: foldback -> rb338_shaper
            float folded_left = foldback(driven_left);
            float folded_right = foldback(driven_right);

            float shaped_left = rb338_shaper(folded_left);
            float shaped_right = rb338_shaper(folded_right);

            // Post-EQ: lowpass at 8kHz to tame harshness, add warmth
            float lp_cutoff = 8000.0f / sample_rate;
            float lp_alpha = 1.0f - expf(-2.0f * 3.14159f * lp_cutoff);
            fx->distortion_lp[0] += lp_alpha * (shaped_left - fx->distortion_lp[0]);
            fx->distortion_lp[1] += lp_alpha * (shaped_right - fx->distortion_lp[1]);

            float wet_left = fx->distortion_lp[0];
            float wet_right = fx->distortion_lp[1];

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

        // --- 3-BAND EQ ---
        if (fx->eq_enabled) {
            // 3-band EQ using stable cascaded filters
            // Low shelf (~250Hz), Mid band (~1kHz), High shelf (~6kHz)
            // Gain range: 0.5 = neutral, 0.0 = -12dB cut, 1.0 = +12dB boost
            float low_gain = fx->eq_low;   // 0.0 to 1.0
            float mid_gain = fx->eq_mid;   // 0.0 to 1.0
            float high_gain = fx->eq_high; // 0.0 to 1.0

            // Convert to linear gain (0.25x to 4x, with 1.0x at 0.5)
            float low_mult = powf(4.0f, (low_gain - 0.5f) * 2.0f);   // 0.25 to 4.0
            float mid_mult = powf(4.0f, (mid_gain - 0.5f) * 2.0f);
            float high_mult = powf(4.0f, (high_gain - 0.5f) * 2.0f);

            for (int ch = 0; ch < 2; ch++) {
                float sample = (ch == 0) ? left : right;

                // Low shelf: one-pole lowpass filter for bass (below 250Hz)
                float low_freq = 250.0f / sample_rate;
                float low_alpha = 1.0f - expf(-2.0f * 3.14159f * low_freq);
                fx->eq_lp1[ch] += low_alpha * (sample - fx->eq_lp1[ch]);
                float low_out = fx->eq_lp1[ch] * low_mult + (sample - fx->eq_lp1[ch]);

                // Mid band: bandpass (250Hz to 6kHz) - what's left after low and high
                float mid_freq = 6000.0f / sample_rate;
                float mid_alpha = 1.0f - expf(-2.0f * 3.14159f * mid_freq);
                fx->eq_lp2[ch] += mid_alpha * (low_out - fx->eq_lp2[ch]);
                float mid_band = fx->eq_lp2[ch] - fx->eq_lp1[ch];
                float mid_out = low_out + mid_band * (mid_mult - 1.0f);

                // High shelf: boost/cut high frequencies (above 6kHz)
                float high_band = mid_out - fx->eq_lp2[ch];
                float final_out = mid_out + high_band * (high_mult - 1.0f);

                if (ch == 0) left = final_out;
                else right = final_out;
            }
        }

        // --- COMPRESSOR (Professional RMS with soft knee and makeup gain) ---
        if (fx->compressor_enabled) {
            for (int ch = 0; ch < 2; ch++) {
                float input = (ch == 0) ? left : right;

                // 1. Compute RMS level (smoother than peak for musical compression)
                float squared = input * input;
                float rms_alpha = 0.01f;  // Smoothing coefficient for RMS
                fx->compressor_rms[ch] += rms_alpha * (squared - fx->compressor_rms[ch]);
                float rms_level = sqrtf(fmaxf(fx->compressor_rms[ch], 0.0f));

                // 2. Attack/release envelope follower
                // Attack: 0.5ms to 50ms (0.0-1.0 maps to fast to slow)
                // Release: 10ms to 500ms (0.0-1.0 maps to fast to slow)
                float attack_time = 0.0005f + fx->compressor_attack * 0.0495f;
                float release_time = 0.01f + fx->compressor_release * 0.49f;
                float attack_coeff = 1.0f - expf(-1.0f / (sample_rate * attack_time));
                float release_coeff = 1.0f - expf(-1.0f / (sample_rate * release_time));

                if (rms_level > fx->compressor_envelope[ch]) {
                    fx->compressor_envelope[ch] += attack_coeff * (rms_level - fx->compressor_envelope[ch]);
                } else {
                    fx->compressor_envelope[ch] += release_coeff * (rms_level - fx->compressor_envelope[ch]);
                }

                // 3. Threshold (0.0-1.0 maps to -40dB to -6dB, linear domain: 0.01 to 0.5)
                float threshold = 0.01f + fx->compressor_threshold * 0.49f;

                // 4. Ratio (0.0-1.0 maps to 1:1 to 20:1)
                float ratio = 1.0f + fx->compressor_ratio * 19.0f;

                // 5. Soft knee (0.1 = ±10% threshold for smooth transition)
                float knee_width = 0.1f;
                float gain = 1.0f;
                float envelope = fx->compressor_envelope[ch];

                if (envelope > threshold) {
                    float delta = envelope - threshold;
                    float knee_range = threshold * knee_width;

                    if (delta < knee_range) {
                        // Soft knee: smooth polynomial transition
                        float x = delta / knee_range;  // 0.0 to 1.0
                        float curve = x * x * (3.0f - 2.0f * x);  // Smoothstep
                        float hard_gain = (threshold + delta / ratio) / envelope;
                        gain = 1.0f - curve * (1.0f - hard_gain);
                    } else {
                        // Hard compression above knee
                        gain = (threshold + delta / ratio) / envelope;
                    }
                }

                // 6. Makeup gain (0.0-1.0 maps to 1x to 8x, compensates for level loss)
                // At 0.5 (neutral), makeup is 1x. At 1.0, makeup is 8x.
                float makeup = powf(8.0f, (fx->compressor_makeup - 0.5f) * 2.0f);

                // 7. Apply compression and makeup gain
                float compressed = input * gain * makeup;

                if (ch == 0) left = compressed;
                else right = compressed;
            }
        }

        // --- DELAY/ECHO ---
        if (fx->delay_enabled && fx->delay_buffer[0] && fx->delay_buffer[1]) {
            // Delay time in samples (0-1000ms)
            int delay_samples = (int)(fx->delay_time * sample_rate);
            if (delay_samples > MAX_DELAY_SAMPLES - 1) delay_samples = MAX_DELAY_SAMPLES - 1;

            // Read from delay buffer
            int read_pos = fx->delay_write_pos - delay_samples;
            if (read_pos < 0) read_pos += MAX_DELAY_SAMPLES;

            float delayed_left = fx->delay_buffer[0][read_pos];
            float delayed_right = fx->delay_buffer[1][read_pos];

            // Write to delay buffer (input + feedback)
            fx->delay_buffer[0][fx->delay_write_pos] = left + delayed_left * fx->delay_feedback;
            fx->delay_buffer[1][fx->delay_write_pos] = right + delayed_right * fx->delay_feedback;

            // Mix dry/wet
            left = left * (1.0f - fx->delay_mix) + delayed_left * fx->delay_mix;
            right = right * (1.0f - fx->delay_mix) + delayed_right * fx->delay_mix;

            // Advance write position
            fx->delay_write_pos = (fx->delay_write_pos + 1) % MAX_DELAY_SAMPLES;
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

// EQ setters/getters
void regroove_effects_set_eq_enabled(RegrooveEffects* fx, int enabled) {
    if (fx) fx->eq_enabled = enabled;
}
void regroove_effects_set_eq_low(RegrooveEffects* fx, float gain) {
    if (fx) fx->eq_low = clampf(gain, 0.0f, 1.0f);
}
void regroove_effects_set_eq_mid(RegrooveEffects* fx, float gain) {
    if (fx) fx->eq_mid = clampf(gain, 0.0f, 1.0f);
}
void regroove_effects_set_eq_high(RegrooveEffects* fx, float gain) {
    if (fx) fx->eq_high = clampf(gain, 0.0f, 1.0f);
}
int regroove_effects_get_eq_enabled(RegrooveEffects* fx) {
    return fx ? fx->eq_enabled : 0;
}
float regroove_effects_get_eq_low(RegrooveEffects* fx) {
    return fx ? fx->eq_low : 0.5f;
}
float regroove_effects_get_eq_mid(RegrooveEffects* fx) {
    return fx ? fx->eq_mid : 0.5f;
}
float regroove_effects_get_eq_high(RegrooveEffects* fx) {
    return fx ? fx->eq_high : 0.5f;
}

// Compressor setters/getters
void regroove_effects_set_compressor_enabled(RegrooveEffects* fx, int enabled) {
    if (fx) fx->compressor_enabled = enabled;
}
void regroove_effects_set_compressor_threshold(RegrooveEffects* fx, float threshold) {
    if (fx) fx->compressor_threshold = clampf(threshold, 0.0f, 1.0f);
}
void regroove_effects_set_compressor_ratio(RegrooveEffects* fx, float ratio) {
    if (fx) fx->compressor_ratio = clampf(ratio, 0.0f, 1.0f);
}
void regroove_effects_set_compressor_attack(RegrooveEffects* fx, float attack) {
    if (fx) fx->compressor_attack = clampf(attack, 0.0f, 1.0f);
}
void regroove_effects_set_compressor_release(RegrooveEffects* fx, float release) {
    if (fx) fx->compressor_release = clampf(release, 0.0f, 1.0f);
}
void regroove_effects_set_compressor_makeup(RegrooveEffects* fx, float makeup) {
    if (fx) fx->compressor_makeup = clampf(makeup, 0.0f, 1.0f);
}
int regroove_effects_get_compressor_enabled(RegrooveEffects* fx) {
    return fx ? fx->compressor_enabled : 0;
}
float regroove_effects_get_compressor_threshold(RegrooveEffects* fx) {
    return fx ? fx->compressor_threshold : 0.7f;
}
float regroove_effects_get_compressor_ratio(RegrooveEffects* fx) {
    return fx ? fx->compressor_ratio : 0.5f;
}
float regroove_effects_get_compressor_attack(RegrooveEffects* fx) {
    return fx ? fx->compressor_attack : 0.1f;
}
float regroove_effects_get_compressor_release(RegrooveEffects* fx) {
    return fx ? fx->compressor_release : 0.3f;
}
float regroove_effects_get_compressor_makeup(RegrooveEffects* fx) {
    return fx ? fx->compressor_makeup : 0.5f;
}

// Phaser setters/getters
void regroove_effects_set_phaser_enabled(RegrooveEffects* fx, int enabled) {
    if (fx) fx->phaser_enabled = enabled;
}
void regroove_effects_set_phaser_rate(RegrooveEffects* fx, float rate) {
    if (fx) fx->phaser_rate = clampf(rate, 0.0f, 1.0f);
}
void regroove_effects_set_phaser_depth(RegrooveEffects* fx, float depth) {
    if (fx) fx->phaser_depth = clampf(depth, 0.0f, 1.0f);
}
void regroove_effects_set_phaser_feedback(RegrooveEffects* fx, float feedback) {
    if (fx) fx->phaser_feedback = clampf(feedback, 0.0f, 1.0f);
}
int regroove_effects_get_phaser_enabled(RegrooveEffects* fx) {
    return fx ? fx->phaser_enabled : 0;
}
float regroove_effects_get_phaser_rate(RegrooveEffects* fx) {
    return fx ? fx->phaser_rate : 0.3f;
}
float regroove_effects_get_phaser_depth(RegrooveEffects* fx) {
    return fx ? fx->phaser_depth : 0.5f;
}
float regroove_effects_get_phaser_feedback(RegrooveEffects* fx) {
    return fx ? fx->phaser_feedback : 0.3f;
}

// Reverb setters/getters
void regroove_effects_set_reverb_enabled(RegrooveEffects* fx, int enabled) {
    if (fx) fx->reverb_enabled = enabled;
}
void regroove_effects_set_reverb_room_size(RegrooveEffects* fx, float size) {
    if (fx) fx->reverb_room_size = clampf(size, 0.0f, 1.0f);
}
void regroove_effects_set_reverb_damping(RegrooveEffects* fx, float damping) {
    if (fx) fx->reverb_damping = clampf(damping, 0.0f, 1.0f);
}
void regroove_effects_set_reverb_mix(RegrooveEffects* fx, float mix) {
    if (fx) fx->reverb_mix = clampf(mix, 0.0f, 1.0f);
}
int regroove_effects_get_reverb_enabled(RegrooveEffects* fx) {
    return fx ? fx->reverb_enabled : 0;
}
float regroove_effects_get_reverb_room_size(RegrooveEffects* fx) {
    return fx ? fx->reverb_room_size : 0.5f;
}
float regroove_effects_get_reverb_damping(RegrooveEffects* fx) {
    return fx ? fx->reverb_damping : 0.5f;
}
float regroove_effects_get_reverb_mix(RegrooveEffects* fx) {
    return fx ? fx->reverb_mix : 0.3f;
}

// Delay setters/getters
void regroove_effects_set_delay_enabled(RegrooveEffects* fx, int enabled) {
    if (fx) fx->delay_enabled = enabled;
}
void regroove_effects_set_delay_time(RegrooveEffects* fx, float time) {
    if (fx) fx->delay_time = clampf(time, 0.0f, 1.0f);
}
void regroove_effects_set_delay_feedback(RegrooveEffects* fx, float feedback) {
    if (fx) fx->delay_feedback = clampf(feedback, 0.0f, 1.0f);
}
void regroove_effects_set_delay_mix(RegrooveEffects* fx, float mix) {
    if (fx) fx->delay_mix = clampf(mix, 0.0f, 1.0f);
}
int regroove_effects_get_delay_enabled(RegrooveEffects* fx) {
    return fx ? fx->delay_enabled : 0;
}
float regroove_effects_get_delay_time(RegrooveEffects* fx) {
    return fx ? fx->delay_time : 0.375f;
}
float regroove_effects_get_delay_feedback(RegrooveEffects* fx) {
    return fx ? fx->delay_feedback : 0.4f;
}
float regroove_effects_get_delay_mix(RegrooveEffects* fx) {
    return fx ? fx->delay_mix : 0.3f;
}
