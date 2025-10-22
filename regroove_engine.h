#ifndef REGROOVE_H
#define REGROOVE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

typedef struct Regroove Regroove;

// --- Optional UI callback types ---
typedef void (*RegrooveOrderCallback)(int order, int pattern, void *userdata);
typedef void (*RegrooveRowCallback)(int order, int row, void *userdata);
typedef void (*RegrooveLoopPatternCallback)(int order, int pattern, void *userdata);
typedef void (*RegrooveLoopSongCallback)(void *userdata);
// MIDI output callback for note events
// channel: tracker channel (0-63)
// note: tracker note number (0-119, where 48=C-4)
// instrument: instrument number (0-255)
// volume: note volume (0-64, tracker range)
// effect_cmd: effect command (0-255, e.g. 0x0F for 0Fxx)
// effect_param: effect parameter (0-255, e.g. 0xFF for 0FFF)
typedef void (*RegrooveNoteCallback)(int channel, int note, int instrument, int volume,
                                     int effect_cmd, int effect_param, void *userdata);

struct RegrooveCallbacks {
    RegrooveOrderCallback       on_order_change;
    RegrooveRowCallback         on_row_change;
    RegrooveLoopPatternCallback on_loop_pattern;
    RegrooveLoopSongCallback    on_loop_song;
    RegrooveNoteCallback        on_note;
    void *userdata;
};

// Creation & lifetime
Regroove *regroove_create(const char *filename, double samplerate);
void regroove_destroy(Regroove *g);
void regroove_set_callbacks(Regroove *g, struct RegrooveCallbacks *cb);

// Rendering
int regroove_render_audio(Regroove *g, int16_t *buffer, int frames);

// User commands (to be called from main loop or UI)
void regroove_process_commands(Regroove *g);
void regroove_pattern_mode(Regroove *g, int on);
void regroove_queue_next_order(Regroove *g);
void regroove_queue_prev_order(Regroove *g);
void regroove_queue_order(Regroove *g, int order);
void regroove_queue_pattern(Regroove *g, int pattern);
void regroove_jump_to_order(Regroove *g, int order);
void regroove_jump_to_pattern(Regroove *g, int pattern);
void regroove_loop_till_row(Regroove *g, int row);
void regroove_retrigger_pattern(Regroove *g);
void regroove_set_custom_loop_rows(Regroove *g, int rows);
void regroove_toggle_channel_mute(Regroove *g, int ch);
void regroove_mute_all(Regroove *g);
void regroove_unmute_all(Regroove *g);
void regroove_toggle_channel_solo(Regroove *g, int ch);
void regroove_set_channel_volume(Regroove *g, int ch, double vol);
double regroove_get_channel_volume(const Regroove* g, int ch);
void regroove_set_channel_panning(Regroove *g, int ch, double pan);
double regroove_get_channel_panning(const Regroove* g, int ch);

void regroove_set_pitch(Regroove *g, double pitch);

// Interpolation filter control
// filter: 0 = none, 1 = linear, 2 = cubic, 4 = FIR (high quality)
void regroove_set_interpolation_filter(Regroove *g, int filter);
int regroove_get_interpolation_filter(const Regroove *g);

// State queries
int regroove_get_num_orders(const Regroove *g);
int regroove_get_num_patterns(const Regroove *g);
int regroove_get_order_pattern(const Regroove *g, int order);
int regroove_get_current_order(const Regroove *g);
int regroove_get_current_pattern(const Regroove *g);
int regroove_get_current_row(const Regroove *g);
int regroove_get_num_channels(const Regroove *g);
double regroove_get_pitch(const Regroove *g);
int regroove_is_channel_muted(const Regroove *g, int ch);
int regroove_get_pattern_mode(const Regroove *g);
int regroove_get_custom_loop_rows(const Regroove *g);
int regroove_get_full_pattern_rows(const Regroove *g);
double regroove_get_current_bpm(const Regroove *g);

// Get formatted pattern cell data (note, instrument, volume, effects)
// Returns 0 on success, -1 on error
// buffer should be at least 32 bytes
int regroove_get_pattern_cell(const Regroove *g, int pattern, int row, int channel, char *buffer, size_t buffer_size);

// Get number of instruments in module
int regroove_get_num_instruments(const Regroove *g);

// Get instrument name by index
// Returns NULL if instrument doesn't exist or has no name
const char* regroove_get_instrument_name(const Regroove *g, int index);

// Get number of samples in module
int regroove_get_num_samples(const Regroove *g);

// Get sample name by index
// Returns NULL if sample doesn't exist or has no name
const char* regroove_get_sample_name(const Regroove *g, int index);

#ifdef __cplusplus
}
#endif

#endif // REGROOVE_H