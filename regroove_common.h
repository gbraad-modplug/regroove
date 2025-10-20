#ifndef REGROOVE_COMMON_H
#define REGROOVE_COMMON_H

#include <stddef.h>
#include "regroove_engine.h"
#include "input_mappings.h"
#include "regroove_metadata.h"
#include "regroove_performance.h"

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

// Device configuration
typedef struct {
    int midi_device_0;      // MIDI device 0 port (-1 = not configured)
    int midi_device_1;      // MIDI device 1 port (-1 = not configured)
    int audio_device;       // Audio device index (-1 = default)
    int midi_output_device; // MIDI output device port (-1 = disabled)
} RegrooveDeviceConfig;

// Phrase playback state
#define MAX_ACTIVE_PHRASES 16
typedef struct {
    int phrase_index;           // Which phrase is playing (-1 = inactive)
    int current_step;           // Current step index
    int delay_counter;          // Rows remaining before executing current step
} ActivePhrase;

// Forward declaration for phrase action callback
typedef void (*PhraseActionCallback)(InputAction action, int parameter, int value, void* userdata);

// Common playback state
typedef struct {
    Regroove *player;
    InputMappings *input_mappings;
    RegrooveFileList *file_list;
    RegrooveMetadata *metadata;
    RegroovePerformance *performance;
    RegrooveDeviceConfig device_config;
    int paused;
    int num_channels;
    double pitch;
    unsigned int audio_device_id;  // SDL_AudioDeviceID for device-specific audio control
    char current_module_path[COMMON_MAX_PATH];  // Track current module for .rgx saving
    int executing_phrase_action;  // Flag to prevent phrase PLAY from enabling performance playback
    ActivePhrase active_phrases[MAX_ACTIVE_PHRASES];  // Active phrase playback state
    PhraseActionCallback phrase_action_callback;  // Callback to execute phrase actions
    void *phrase_callback_userdata;
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
void regroove_common_loop_till_row(RegrooveCommonState *state);
void regroove_common_halve_loop(RegrooveCommonState *state);
void regroove_common_full_loop(RegrooveCommonState *state);
void regroove_common_pattern_mode_toggle(RegrooveCommonState *state);
void regroove_common_channel_mute(RegrooveCommonState *state, int channel);
void regroove_common_mute_all(RegrooveCommonState *state);
void regroove_common_unmute_all(RegrooveCommonState *state);
void regroove_common_pitch_up(RegrooveCommonState *state);
void regroove_common_pitch_down(RegrooveCommonState *state);
void regroove_common_set_pitch(RegrooveCommonState *state, double pitch);

// Phrase playback functions
void regroove_common_set_phrase_callback(RegrooveCommonState *state, PhraseActionCallback callback, void *userdata);
void regroove_common_trigger_phrase(RegrooveCommonState *state, int phrase_index);
void regroove_common_update_phrases(RegrooveCommonState *state);

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
