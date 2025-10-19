#include "regroove_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libopenmpt/libopenmpt.h>
#include <libopenmpt/libopenmpt_ext.h>

#define REGROOVE_MIN_PITCH 0.01
#define REGROOVE_MAX_PITCH 4.0

typedef enum {
    RG_CMD_NONE,
    RG_CMD_QUEUE_ORDER,
    RG_CMD_QUEUE_PATTERN,
    RG_CMD_JUMP_TO_PATTERN,
    RG_CMD_LOOP_TILL_ROW,
    RG_CMD_SET_PATTERN_MODE,
    RG_CMD_RETRIGGER_PATTERN,
    RG_CMD_SET_CUSTOM_LOOP_ROWS,
    RG_CMD_TOGGLE_CHANNEL_MUTE,
    RG_CMD_TOGGLE_CHANNEL_SINGLE,   // NEW
    RG_CMD_MUTE_ALL,
    RG_CMD_UNMUTE_ALL,
    RG_CMD_SET_PITCH,
    RG_CMD_SET_CHANNEL_VOLUME       // NEW
} RegrooveCommandType;

typedef struct {
    RegrooveCommandType type;
    int arg1;
    int arg2;
    double dval; // For volume
} RegrooveCommand;

#define RG_MAX_COMMANDS 8

struct Regroove {
    openmpt_module_ext* modext;
    openmpt_module* mod;
    openmpt_module_ext_interface_interactive interactive;
    int interactive_ok;
    double samplerate;
    double pitch_factor;
    int num_channels;
    int* mute_states;
    double* channel_volumes;

    int num_orders;
    int pattern_mode;
    int loop_pattern;
    int loop_order;

    RegrooveCommand command_queue[RG_MAX_COMMANDS];
    int command_queue_head;
    int command_queue_tail;

    int queued_order;
    int queued_row;
    int has_queued_jump;

    int loop_till_row;
    int is_looping_till;

    int pending_pattern_mode_order; // -1 = none

    int custom_loop_rows; // 0 = use full_loop_rows
    int full_loop_rows;

    int prev_row;
    int prev_order;

    // --- UI callback hooks ---
    RegrooveOrderCallback        on_order_change;
    RegrooveRowCallback          on_row_change;
    RegrooveLoopPatternCallback  on_loop_pattern;
    RegrooveLoopSongCallback     on_loop_song;
    void *callback_userdata;

    // --- For feedback ---
    int last_msg_order;
    int last_msg_pattern;
    int last_msg_row;

    int last_playback_order;
    int last_playback_row;
};

static void reapply_mutes(struct Regroove* g) {
    if (!g->interactive_ok) return;
    for (int ch = 0; ch < g->num_channels; ++ch) {
        g->interactive.set_channel_volume(g->modext, ch,
            g->mute_states[ch] ? 0.0 : g->channel_volumes[ch]);
    }
}

static void enqueue_command(struct Regroove* g, RegrooveCommandType type, int arg1, int arg2) {
    int next_tail = (g->command_queue_tail + 1) % RG_MAX_COMMANDS;
    if (next_tail != g->command_queue_head) {
        g->command_queue[g->command_queue_tail].type = type;
        g->command_queue[g->command_queue_tail].arg1 = arg1;
        g->command_queue[g->command_queue_tail].arg2 = arg2;
        g->command_queue[g->command_queue_tail].dval = 0.0;
        g->command_queue_tail = next_tail;
    }
}
static void enqueue_command_d(struct Regroove* g, RegrooveCommandType type, int arg1, double dval) {
    int next_tail = (g->command_queue_tail + 1) % RG_MAX_COMMANDS;
    if (next_tail != g->command_queue_head) {
        g->command_queue[g->command_queue_tail].type = type;
        g->command_queue[g->command_queue_tail].arg1 = arg1;
        g->command_queue[g->command_queue_tail].arg2 = 0;
        g->command_queue[g->command_queue_tail].dval = dval;
        g->command_queue_tail = next_tail;
    }
}

static void process_commands(struct Regroove* g) {
    while (g->command_queue_head != g->command_queue_tail) {
        RegrooveCommand* cmd = &g->command_queue[g->command_queue_head];
        switch (cmd->type) {
            case RG_CMD_TOGGLE_CHANNEL_MUTE:
                if (cmd->arg1 >= 0 && cmd->arg1 < g->num_channels) {
                    g->mute_states[cmd->arg1] = !g->mute_states[cmd->arg1];
                    if (g->interactive_ok)
                        g->interactive.set_channel_volume(
                            g->modext, cmd->arg1, g->mute_states[cmd->arg1] ? 0.0 : g->channel_volumes[cmd->arg1]);
                }
                break;
            case RG_CMD_TOGGLE_CHANNEL_SINGLE:
                if (cmd->arg1 >= 0 && cmd->arg1 < g->num_channels) {
                    for (int i = 0; i < g->num_channels; ++i) {
                        int mute = (i != cmd->arg1);
                        g->mute_states[i] = mute;
                        if (g->interactive_ok)
                            g->interactive.set_channel_volume(
                                g->modext, i, mute ? 0.0 : g->channel_volumes[i]);
                    }
                }
                break;
            case RG_CMD_SET_CHANNEL_VOLUME: {
                int ch = cmd->arg1;
                double vol = cmd->dval;
                if (ch >= 0 && ch < g->num_channels) {
                    if (vol < 0.0) vol = 0.0;
                    if (vol > 1.0) vol = 1.0;
                    g->channel_volumes[ch] = vol;
                    if (g->interactive_ok)
                        g->interactive.set_channel_volume(
                            g->modext, ch, g->mute_states[ch] ? 0.0 : vol);
                }
                break;
            }
            case RG_CMD_MUTE_ALL:
                for (int ch = 0; ch < g->num_channels; ++ch) {
                    g->mute_states[ch] = 1;
                    if (g->interactive_ok)
                        g->interactive.set_channel_volume(g->modext, ch, 0.0);
                }
                break;
            case RG_CMD_UNMUTE_ALL:
                for (int ch = 0; ch < g->num_channels; ++ch) {
                    g->mute_states[ch] = 0;
                    if (g->interactive_ok)
                        g->interactive.set_channel_volume(g->modext, ch, g->channel_volumes[ch]);
                }
                break;
            case RG_CMD_SET_PITCH: {
                double val = cmd->arg1 / 100.0;
                if (val < REGROOVE_MIN_PITCH) val = REGROOVE_MIN_PITCH;
                if (val > REGROOVE_MAX_PITCH) val = REGROOVE_MAX_PITCH;
                g->pitch_factor = val;
                break;
            }
            case RG_CMD_QUEUE_ORDER:
                if (g->pattern_mode) {
                    g->pending_pattern_mode_order = cmd->arg1;
                } else {
                    g->queued_order = cmd->arg1;
                    g->queued_row = cmd->arg2;
                    g->has_queued_jump = 1;
                }
                break;
            case RG_CMD_QUEUE_PATTERN: {
                // Find first order containing this pattern
                int pattern_index = cmd->arg1;
                int target_order = -1;
                for (int i = 0; i < g->num_orders; ++i) {
                    if (openmpt_module_get_order_pattern(g->mod, i) == pattern_index) {
                        target_order = i;
                        break;
                    }
                }
                if (target_order == -1) target_order = 0; // fallback

                // Queue this order (will jump at pattern end)
                if (g->pattern_mode) {
                    g->pending_pattern_mode_order = target_order;
                } else {
                    g->queued_order = target_order;
                    g->queued_row = 0;
                    g->has_queued_jump = 1;
                }
                break;
            }
            case RG_CMD_JUMP_TO_PATTERN: {
                int pattern_index = cmd->arg1;
                int target_order = cmd->arg2; // arg2 now carries explicit order, or -1 to search

                // If order not specified, find first order that contains this pattern
                if (target_order == -1) {
                    for (int i = 0; i < g->num_orders; ++i) {
                        if (openmpt_module_get_order_pattern(g->mod, i) == pattern_index) {
                            target_order = i;
                            break;
                        }
                    }
                    // If pattern not found in order list, use order 0 as fallback
                    if (target_order == -1) {
                        target_order = 0;
                    }
                }

                // Update loop state for pattern mode
                g->loop_order = target_order;
                g->loop_pattern = pattern_index;
                g->full_loop_rows = openmpt_module_get_pattern_num_rows(g->mod, pattern_index);
                g->custom_loop_rows = 0;
                g->prev_row = -1;

                // Jump to the position immediately
                openmpt_module_set_position_order_row(g->mod, target_order, 0);
                if (g->interactive_ok) reapply_mutes(g);
                break;
            }
            case RG_CMD_LOOP_TILL_ROW:
                g->loop_order = cmd->arg1;
                g->loop_pattern = openmpt_module_get_order_pattern(g->mod, cmd->arg1);
                g->full_loop_rows = openmpt_module_get_pattern_num_rows(g->mod, g->loop_pattern);
                g->custom_loop_rows = 0;
                g->loop_till_row = cmd->arg2;
                g->is_looping_till = 1;
                openmpt_module_set_position_order_row(g->mod, cmd->arg1, 0);
                if (g->interactive_ok) reapply_mutes(g);
                g->prev_row = -1;
                break;
            case RG_CMD_SET_PATTERN_MODE:
                g->pattern_mode = cmd->arg1;
                if (g->pattern_mode) {
                    g->loop_order   = openmpt_module_get_current_order(g->mod);
                    g->loop_pattern = openmpt_module_get_current_pattern(g->mod);
                    g->full_loop_rows = openmpt_module_get_pattern_num_rows(g->mod, g->loop_pattern);
                    g->custom_loop_rows = 0;
                    g->pending_pattern_mode_order = -1;
                    g->prev_row = -1;
                }
                break;
            case RG_CMD_RETRIGGER_PATTERN: {
                int cur_order = openmpt_module_get_current_order(g->mod);
                openmpt_module_set_position_order_row(g->mod, cur_order, 0);
                if (g->interactive_ok) reapply_mutes(g);
                g->prev_row = -1;
                break;
            }
            case RG_CMD_SET_CUSTOM_LOOP_ROWS:
                g->custom_loop_rows = cmd->arg1;
                if (g->custom_loop_rows < 0) g->custom_loop_rows = 0;
                g->prev_row = -1;
                break;
            default: break;
        }
        g->command_queue_head = (g->command_queue_head + 1) % RG_MAX_COMMANDS;
    }
}

Regroove *regroove_create(const char *filename, double samplerate) {
    Regroove *g = (Regroove *)calloc(1, sizeof(Regroove));
    size_t size = 0;
    int error = 0;
    void* bytes = NULL;

    g->modext = NULL;
    g->mod = NULL;
    g->samplerate = samplerate;
    g->pitch_factor = 1.0;
    g->pattern_mode = 0;
    g->pending_pattern_mode_order = -1;

    FILE* f = fopen(filename, "rb");
    if (!f) { free(g); return NULL; }
    fseek(f, 0, SEEK_END);
    size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    bytes = malloc(size);
    if (!bytes) { fclose(f); free(g); return NULL; }
    if (fread(bytes, 1, size, f) != size) { free(bytes); fclose(f); free(g); return NULL; }
    fclose(f);

    g->modext = openmpt_module_ext_create_from_memory(
        bytes, size, NULL, NULL, NULL, &error, NULL, NULL, NULL);
    free(bytes);
    if (!g->modext) { free(g); return NULL; }
    g->mod = openmpt_module_ext_get_module(g->modext);
    if (!g->mod) { openmpt_module_ext_destroy(g->modext); free(g); return NULL; }
    g->num_orders = openmpt_module_get_num_orders(g->mod);
    g->num_channels = openmpt_module_get_num_channels(g->mod);

    g->mute_states = (int*)calloc(g->num_channels, sizeof(int));
    g->channel_volumes = (double*)calloc(g->num_channels, sizeof(double));
    for (int i = 0; i < g->num_channels; ++i) g->channel_volumes[i] = 1.0;

    g->interactive_ok = 0;
    if (openmpt_module_ext_get_interface(
            g->modext, LIBOPENMPT_EXT_C_INTERFACE_INTERACTIVE,
            &g->interactive, sizeof(g->interactive)) != 0) {
        g->interactive_ok = 1;
        reapply_mutes(g);
    }

    g->loop_order = openmpt_module_get_current_order(g->mod);
    g->loop_pattern = openmpt_module_get_current_pattern(g->mod);
    g->full_loop_rows = openmpt_module_get_pattern_num_rows(g->mod, g->loop_pattern);
    g->custom_loop_rows = 0;
    g->prev_row = -1;
    g->prev_order = g->loop_order;

    g->on_order_change = NULL;
    g->on_row_change = NULL;
    g->on_loop_pattern = NULL;
    g->on_loop_song = NULL;
    g->callback_userdata = NULL;

    g->last_msg_order = -1;
    g->last_msg_pattern = -1;
    g->last_msg_row = -1;

    g->last_playback_order = -1;
    g->last_playback_row = -1;

    return g;
}

void regroove_destroy(Regroove *g) {
    if (g->modext) openmpt_module_ext_destroy(g->modext);
    if (g->mute_states) free(g->mute_states);
    if (g->channel_volumes) free(g->channel_volumes);
    free(g);
}

void regroove_set_callbacks(Regroove *g, struct RegrooveCallbacks *cb) {
    if (!g || !cb) return;
    g->on_order_change = cb->on_order_change;
    g->on_row_change = cb->on_row_change;
    g->on_loop_pattern = cb->on_loop_pattern;
    g->on_loop_song = cb->on_loop_song;
    g->callback_userdata = cb->userdata;
}

int regroove_render_audio(Regroove* g, int16_t* buffer, int frames) {
    process_commands(g);

    if (g->has_queued_jump) {
        openmpt_module_set_position_order_row(g->mod, g->queued_order, g->queued_row);
        if (g->interactive_ok) reapply_mutes(g);
        g->has_queued_jump = 0;
        g->prev_row = -1;
    }

    int count = openmpt_module_read_interleaved_stereo(
        g->mod, g->samplerate * g->pitch_factor, frames, buffer);

    int cur_order = openmpt_module_get_current_order(g->mod);
    int cur_pattern = openmpt_module_get_current_pattern(g->mod);
    int cur_row = openmpt_module_get_current_row(g->mod);
    int loop_rows = g->custom_loop_rows > 0 ? g->custom_loop_rows : g->full_loop_rows;

    if (g->is_looping_till) {
        int rows = openmpt_module_get_pattern_num_rows(g->mod, g->loop_pattern);
        if (cur_order == g->loop_order) {
            if (cur_row == g->loop_till_row) {
                g->is_looping_till = 0;
                g->prev_row = -1;
            } else if (g->prev_row == rows - 1 && cur_row == 0) {
                openmpt_module_set_position_order_row(g->mod, g->loop_order, 0);
                if (g->interactive_ok) reapply_mutes(g);
                if (g->on_loop_pattern)
                    g->on_loop_pattern(g->loop_order, g->loop_pattern, g->callback_userdata);
                g->prev_row = -1;
            } else {
                g->prev_row = cur_row;
            }
        } else {
            g->prev_row = -1;
        }
    }
    else if (g->pattern_mode) {
        int at_custom_loop_end = (g->custom_loop_rows > 0 && cur_row >= loop_rows);
        int at_full_pattern_end = (g->custom_loop_rows == 0 && g->prev_row == loop_rows - 1 && cur_row == 0);

        // --- CRITICAL: process pending pattern jump first and return ---
        if ((at_custom_loop_end || at_full_pattern_end) &&
            g->pending_pattern_mode_order != -1 && g->pending_pattern_mode_order != g->loop_order) {
            g->loop_order = g->pending_pattern_mode_order;
            g->loop_pattern = openmpt_module_get_order_pattern(g->mod, g->loop_order);
            g->full_loop_rows = openmpt_module_get_pattern_num_rows(g->mod, g->loop_pattern);
            g->custom_loop_rows = 0;
            g->pending_pattern_mode_order = -1;
            openmpt_module_set_position_order_row(g->mod, g->loop_order, 0);
            if (g->interactive_ok) reapply_mutes(g);
            if (g->on_loop_pattern)
                g->on_loop_pattern(g->loop_order, g->loop_pattern, g->callback_userdata);
            g->prev_row = -1;
            return count;
        }

        // Then do standard wrap/loop logic
        if (cur_order == g->loop_order) {
            if (at_custom_loop_end || at_full_pattern_end) {
                openmpt_module_set_position_order_row(g->mod, g->loop_order, 0);
                if (g->interactive_ok)
                    reapply_mutes(g);
                if (g->on_loop_pattern)
                    g->on_loop_pattern(g->loop_order, g->loop_pattern, g->callback_userdata);
                g->prev_row = -1;
            } else {
                g->prev_row = cur_row;
            }
        } else { // If escaped loop order, snap back
            openmpt_module_set_position_order_row(g->mod, g->loop_order, 0);
            if (g->interactive_ok)
                reapply_mutes(g);
            if (g->on_loop_pattern)
                g->on_loop_pattern(g->loop_order, g->loop_pattern, g->callback_userdata);
            g->prev_row = -1;
        }
    }
    else if (g->has_queued_jump) {
        openmpt_module_set_position_order_row(g->mod, g->queued_order, g->queued_row);
        if (g->interactive_ok)
            reapply_mutes(g);
        g->has_queued_jump = 0;
        g->prev_row = -1;
    }

    g->prev_order = cur_order;

    // --- CALL UI CALLBACKS AFTER ALL JUMP LOGIC ---
    int final_order = openmpt_module_get_current_order(g->mod);
    int final_pattern = openmpt_module_get_current_pattern(g->mod);
    int final_row = openmpt_module_get_current_row(g->mod);

    if (g->on_order_change && g->last_msg_order != final_order) {
        g->on_order_change(final_order, final_pattern, g->callback_userdata);
        g->last_msg_order = final_order;
    }
    if (g->on_row_change && g->last_msg_row != final_row) {
        g->on_row_change(final_order, final_row, g->callback_userdata);
        g->last_msg_row = final_row;
    }

    // --- Detect song loop event ---
    int num_orders = openmpt_module_get_num_orders(g->mod);
    if (g->last_playback_order != -1) {
        if (g->last_playback_order == num_orders - 1 && cur_order == 0) {
            if (g->on_loop_song) {
                g->on_loop_song(g->callback_userdata);
            }
        }
    }
    g->last_playback_order = cur_order;
    g->last_playback_row = cur_row;

    return count;
}

// --- API functions ---

void regroove_process_commands(Regroove *g) {
    process_commands(g);
}

void regroove_pattern_mode(Regroove* g, int on) {
    enqueue_command(g, RG_CMD_SET_PATTERN_MODE, !!on, 0);
}
void regroove_queue_next_order(Regroove* g) {
    int cur_order = openmpt_module_get_current_order(g->mod);
    int next_order = cur_order + 1;
    if (next_order < g->num_orders)
        enqueue_command(g, RG_CMD_QUEUE_ORDER, next_order, 0);
}
void regroove_queue_prev_order(Regroove* g) {
    int cur_order = openmpt_module_get_current_order(g->mod);
    int prev_order = cur_order > 0 ? cur_order - 1 : 0;
    enqueue_command(g, RG_CMD_QUEUE_ORDER, prev_order, 0);
}
void regroove_queue_order(Regroove* g, int order) {
    if (order >= 0 && order < g->num_orders) {
        enqueue_command(g, RG_CMD_QUEUE_ORDER, order, 0);
    }
}
void regroove_queue_pattern(Regroove* g, int pattern) {
    if (!g || !g->mod) return;
    int num_patterns = openmpt_module_get_num_patterns(g->mod);
    if (pattern >= 0 && pattern < num_patterns) {
        enqueue_command(g, RG_CMD_QUEUE_PATTERN, pattern, 0);
    }
}
void regroove_jump_to_order(Regroove* g, int order) {
    if (order >= 0 && order < g->num_orders) {
        // Immediate jump - use JUMP_TO_PATTERN command which jumps immediately
        int target_order = order;
        int target_pattern = openmpt_module_get_order_pattern(g->mod, target_order);
        enqueue_command(g, RG_CMD_JUMP_TO_PATTERN, target_pattern, target_order);
    }
}
void regroove_jump_to_pattern(Regroove* g, int pattern) {
    if (!g || !g->mod) return;
    // Get total number of patterns to validate input
    int num_patterns = openmpt_module_get_num_patterns(g->mod);
    if (pattern >= 0 && pattern < num_patterns) {
        enqueue_command(g, RG_CMD_JUMP_TO_PATTERN, pattern, -1); // -1 = auto-find order
    }
}
void regroove_loop_till_row(Regroove* g, int row) {
    int cur_order = openmpt_module_get_current_order(g->mod);
    enqueue_command(g, RG_CMD_LOOP_TILL_ROW, cur_order, row);
}
void regroove_retrigger_pattern(Regroove* g) {
    enqueue_command(g, RG_CMD_RETRIGGER_PATTERN, 0, 0);
}
void regroove_set_custom_loop_rows(Regroove* g, int rows) {
    enqueue_command(g, RG_CMD_SET_CUSTOM_LOOP_ROWS, rows, 0);
}

void regroove_toggle_channel_mute(Regroove *g, int ch) {
    enqueue_command(g, RG_CMD_TOGGLE_CHANNEL_MUTE, ch, 0);
    process_commands(g);
}

void regroove_toggle_channel_solo(Regroove* g, int ch) {
    enqueue_command(g, RG_CMD_TOGGLE_CHANNEL_SINGLE, ch, 0);
}

void regroove_set_channel_volume(Regroove *g, int ch, double vol) {
    enqueue_command_d(g, RG_CMD_SET_CHANNEL_VOLUME, ch, vol);
}

double regroove_get_channel_volume(const Regroove* g, int ch) {
    if (!g || ch < 0 || ch >= g->num_channels) return 0.0;
    return g->channel_volumes[ch];
}

void regroove_mute_all(Regroove* g) {
    enqueue_command(g, RG_CMD_MUTE_ALL, 0, 0);
}
void regroove_unmute_all(Regroove* g) {
    enqueue_command(g, RG_CMD_UNMUTE_ALL, 0, 0);
}
void regroove_set_pitch(Regroove* g, double pitch) {
    enqueue_command(g, RG_CMD_SET_PITCH, (int)(pitch * 100), 0);
}

// --- Info getters ---
int regroove_get_num_orders(const Regroove* g) { return g->num_orders; }
int regroove_get_num_patterns(const Regroove* g) {
    if (!g || !g->mod) return 0;
    return openmpt_module_get_num_patterns(g->mod);
}
int regroove_get_order_pattern(const Regroove* g, int order) {
    if (!g || !g->mod) return -1;
    return openmpt_module_get_order_pattern(g->mod, order);
}
int regroove_get_current_order(const Regroove* g) { return openmpt_module_get_current_order(g->mod); }
int regroove_get_current_pattern(const Regroove* g) { return openmpt_module_get_current_pattern(g->mod); }
int regroove_get_current_row(const Regroove* g) { return openmpt_module_get_current_row(g->mod); }
int regroove_get_num_channels(const Regroove* g) { return g ? g->num_channels : 0; }
double regroove_get_pitch(const Regroove* g) { return g ? g->pitch_factor : 1.0; }
int regroove_is_channel_muted(const Regroove* g, int ch) {
    if (!g || ch < 0 || ch >= g->num_channels) return 0;
    return g->mute_states[ch];
}
int regroove_get_pattern_mode(const Regroove* g) { return g->pattern_mode; }
int regroove_get_custom_loop_rows(const Regroove* g) { return g->custom_loop_rows; }
int regroove_get_full_pattern_rows(const Regroove* g) { return g->full_loop_rows; }
double regroove_get_current_bpm(const Regroove* g) {
    if (!g || !g->mod) return 0.0;
    return openmpt_module_get_current_estimated_bpm(g->mod);
}
