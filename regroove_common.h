#ifndef REGROOVE_COMMON_H
#define REGROOVE_COMMON_H

#include <stddef.h>
#include "regroove_engine.h"
#include "input_mappings.h"
#include "regroove_metadata.h"
#include "regroove_performance.h"
#include "regroove_phrase.h"

#ifdef __cplusplus
extern "C" {
#endif

// Maximum path length
#define COMMON_MAX_PATH 1024
#define COMMON_MAX_FILES 4096

// File list management
typedef struct {
    char **filenames;     // Array of filenames (not full paths)
    int count;
    int current_index;
    char directory[COMMON_MAX_PATH];  // Directory path (normalized, no trailing slash)
} RegrooveFileList;

// Initialize file list
RegrooveFileList* regroove_filelist_create(void);

// Load files from directory (handles trailing slash automatically)
int regroove_filelist_load(RegrooveFileList *list, const char *dir_path);

// Get current file's full path
const char* regroove_filelist_get_current_path(RegrooveFileList *list, char *buffer, size_t bufsize);

// Navigate file list
void regroove_filelist_next(RegrooveFileList *list);
void regroove_filelist_prev(RegrooveFileList *list);

// Free file list
void regroove_filelist_destroy(RegrooveFileList *list);

// Device configuration and effect defaults
typedef struct {
    int midi_device_0;      // MIDI device 0 port (-1 = not configured)
    int midi_device_1;      // MIDI device 1 port (-1 = not configured)
    int audio_device;       // Audio output device index (-1 = default)
    int audio_input_device; // Audio input device index (-1 = disabled)
    int midi_output_device; // MIDI output device port (-1 = disabled)
    int midi_output_note_duration; // 0 = immediate off, 1 = hold until next note/off command
    int midi_clock_sync;    // 0 = disabled, 1 = sync tempo to incoming MIDI clock (default: 0)
    float midi_clock_sync_threshold; // Tempo change threshold % to apply pitch adjustment (0.1-5.0, default: 0.5)
    int midi_clock_master;  // 0 = disabled, 1 = send MIDI clock as master (default: 0)
    int midi_clock_send_transport; // 0 = disabled, 1 = send MIDI Start/Stop/Continue when master (default: 0)
    int midi_clock_send_spp; // 0 = disabled, 1 = on stop only (standard MIDI), 2 = during playback (regroove-to-regroove) (default: 2)
    int midi_clock_spp_interval; // SPP interval in rows when sending during playback: 64=pattern, 16, 8, 4 (default: 64)
    int midi_spp_receive; // 0 = disabled (ignore incoming SPP), 1 = enabled (sync to incoming SPP) (default: 1)
    int midi_transport_control; // 0 = disabled, 1 = respond to MIDI Start/Stop/Continue (default: 0)
    int interpolation_filter; // 0=none, 1=linear, 2=cubic, 4=FIR (default: 2)
    int stereo_separation;    // 0-200, stereo separation percentage (default: 100)
    int dither;               // 0=none, 1=default, 2=rectangular 0.5bit, 3=rectangular 1bit (default: 1)
    int amiga_resampler;      // 0=disabled, 1=enabled (default: 0)
    int amiga_filter_type;    // 0=auto, 1=a500, 2=a1200, 3=unfiltered (default: 0)
    int expanded_pads;      // 0 = combined 8+8 layout, 1 = separate 16+16 panels (default: 0)

    // Default effect parameters (applied on song load)
    float fx_distortion_drive;      // 0.0 - 1.0
    float fx_distortion_mix;        // 0.0 - 1.0
    float fx_filter_cutoff;         // 0.0 - 1.0
    float fx_filter_resonance;      // 0.0 - 1.0
    float fx_eq_low;                // 0.0 - 1.0
    float fx_eq_mid;                // 0.0 - 1.0
    float fx_eq_high;               // 0.0 - 1.0
    float fx_compressor_threshold;  // 0.0 - 1.0
    float fx_compressor_ratio;      // 0.0 - 1.0
    float fx_compressor_attack;     // 0.0 - 1.0
    float fx_compressor_release;    // 0.0 - 1.0
    float fx_compressor_makeup;     // 0.0 - 1.0
    float fx_delay_time;            // 0.0 - 1.0
    float fx_delay_feedback;        // 0.0 - 1.0
    float fx_delay_mix;             // 0.0 - 1.0
} RegrooveDeviceConfig;

// Common playback state
typedef struct {
    Regroove *player;
    InputMappings *input_mappings;
    RegrooveFileList *file_list;
    RegrooveMetadata *metadata;
    RegroovePerformance *performance;
    RegroovePhrase *phrase;
    RegrooveDeviceConfig device_config;
    int paused;
    int num_channels;
    double pitch;
    unsigned int audio_device_id;  // SDL_AudioDeviceID for device-specific audio control
    char current_module_path[COMMON_MAX_PATH];  // Track current module for .rgx saving
} RegrooveCommonState;

// Initialize common state
RegrooveCommonState* regroove_common_create(void);

// Load input mappings from .ini file (with fallback to defaults)
int regroove_common_load_mappings(RegrooveCommonState *state, const char *ini_path);

// Load a module file safely (handles audio locking)
int regroove_common_load_module(RegrooveCommonState *state, const char *path,
                                struct RegrooveCallbacks *callbacks);

// Free common state
void regroove_common_destroy(RegrooveCommonState *state);

// Common control functions (using centralized state)
void regroove_common_play_pause(RegrooveCommonState *state, int play);
void regroove_common_retrigger(RegrooveCommonState *state);
void regroove_common_next_order(RegrooveCommonState *state);
void regroove_common_prev_order(RegrooveCommonState *state);
void regroove_common_halve_loop(RegrooveCommonState *state);
void regroove_common_full_loop(RegrooveCommonState *state);
void regroove_common_pattern_mode_toggle(RegrooveCommonState *state);
void regroove_common_channel_mute(RegrooveCommonState *state, int channel);
void regroove_common_mute_all(RegrooveCommonState *state);
void regroove_common_unmute_all(RegrooveCommonState *state);
void regroove_common_pitch_up(RegrooveCommonState *state);
void regroove_common_pitch_down(RegrooveCommonState *state);
void regroove_common_set_pitch(RegrooveCommonState *state, double pitch);

// Phrase playback functions (wrappers around phrase engine)
void regroove_common_set_phrase_callback(RegrooveCommonState *state, PhraseActionCallback callback, void *userdata);
void regroove_common_trigger_phrase(RegrooveCommonState *state, int phrase_index);
void regroove_common_update_phrases(RegrooveCommonState *state);
int regroove_common_phrase_is_active(const RegrooveCommonState *state);

// Save device configuration to existing INI file
int regroove_common_save_device_config(RegrooveCommonState *state, const char *filepath);

// Save default configuration to INI file
int regroove_common_save_default_config(const char *filepath);

// Save metadata and performance to .rgx file
int regroove_common_save_rgx(RegrooveCommonState *state);

#ifdef __cplusplus
}
#endif

#endif // REGROOVE_COMMON_H
