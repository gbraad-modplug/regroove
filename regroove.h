#ifndef REGROOVE_H
#define REGROOVE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct Regroove Regroove;

// --- Optional UI callback types ---
typedef void (*RegrooveOrderCallback)(int order, int pattern, void *userdata);
typedef void (*RegrooveRowCallback)(int order, int row, void *userdata);
typedef void (*RegrooveLoopCallback)(int order, int pattern, void *userdata);

struct RegrooveCallbacks {
    RegrooveOrderCallback on_order_change;
    RegrooveRowCallback   on_row_change;
    RegrooveLoopCallback  on_pattern_loop;
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
void regroove_loop_till_row(Regroove *g, int row);
void regroove_retrigger_pattern(Regroove *g);
void regroove_set_custom_loop_rows(Regroove *g, int rows);
void regroove_toggle_channel_mute(Regroove *g, int ch);
void regroove_mute_all(Regroove *g);
void regroove_unmute_all(Regroove *g);
void regroove_toggle_channel_solo(Regroove *g, int ch);
void regroove_set_channel_volume(Regroove *g, int ch, double vol);
double regroove_get_channel_volume(const Regroove* g, int ch);

void regroove_set_pitch(Regroove *g, double pitch);

// State queries
int regroove_get_num_orders(const Regroove *g);
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

#ifdef __cplusplus
}
#endif

#endif // REGROOVE_H