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
    RG_CMD_QUEUE_NEXT_ORDER,
    RG_CMD_QUEUE_PREV_ORDER,
    RG_CMD_QUEUE_PATTERN,
    RG_CMD_JUMP_TO_PATTERN,
    RG_CMD_SET_LOOP_RANGE,      // Set loop start/end points
    RG_CMD_TRIGGER_LOOP,        // Jump to loop start and begin looping (ACTIVE)
    RG_CMD_PLAY_TO_LOOP,        // Toggle: OFF→ARMED, ARMED→OFF, ACTIVE→OFF
    RG_CMD_SET_PATTERN_MODE,
    RG_CMD_RETRIGGER_PATTERN,
    RG_CMD_SET_CUSTOM_LOOP_ROWS,
    RG_CMD_TOGGLE_CHANNEL_MUTE,
    RG_CMD_TOGGLE_CHANNEL_SINGLE,   // NEW
    RG_CMD_MUTE_ALL,
    RG_CMD_UNMUTE_ALL,
    RG_CMD_SET_PITCH,
    RG_CMD_SET_CHANNEL_VOLUME,
    RG_CMD_SET_CHANNEL_PANNING,
    RG_CMD_QUEUE_CHANNEL_MUTE,      // Queued mute toggle
    RG_CMD_QUEUE_CHANNEL_SOLO       // Queued solo toggle
} RegrooveCommandType;

typedef struct {
    RegrooveCommandType type;
    int arg1;
    int arg2;
    double dval; // For volume
    int arg3;    // For loop range (end_order)
    int arg4;    // For loop range (end_row)
} RegrooveCommand;

#define RG_MAX_COMMANDS 8

struct Regroove {
    openmpt_module_ext* modext;
    openmpt_module* mod;
    openmpt_module_ext_interface_interactive interactive;
    int interactive_ok;
    openmpt_module_ext_interface_interactive2 *interactive2;
    int interactive2_ok;
    double samplerate;
    double pitch_factor;
    int interpolation_filter;  // 0=none, 1=linear, 2=cubic, 4=FIR
    int stereo_separation;     // 0-200, percentage
    int dither;                // 0=none, 1=default, 2=rect 0.5bit, 3=rect 1bit
    int amiga_resampler;       // 0=disabled, 1=enabled
    int amiga_filter_type;     // 0=auto, 1=a500, 2=a1200, 3=unfiltered
    int num_channels;
    int* mute_states;
    double* channel_volumes;
    double* channel_pannings;  // 0.0 = full left, 0.5 = center, 1.0 = full right

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
    int queued_jump_type;  // 0=none, 1=next, 2=prev, 3=specific order, 4=pattern

    // Loop range system (replaces loop_till_row)
    int loop_range_enabled;    // 0=OFF, 1=ARMED (play-to-loop), 2=ACTIVE (looping)
    int loop_start_order;      // -1 = use current pattern only
    int loop_start_row;
    int loop_end_order;        // -1 = use current pattern only
    int loop_end_row;

    int pending_pattern_mode_order; // -1 = none

    int custom_loop_rows; // 0 = use full_loop_rows
    int full_loop_rows;

    int prev_row;
    int prev_order;

    // Pending mute/solo state (queued until pattern boundary)
    int* pending_mute_states;      // NULL if no pending changes
    int has_pending_mute_changes;  // Flag to indicate pending changes exist

    // Track the specific queued action per channel (so UI knows which buttons to highlight)
    int* queued_action_per_channel; // 0=none, 1=mute, 2=solo for each channel

    // --- UI callback hooks ---
    RegrooveOrderCallback        on_order_change;
    RegrooveRowCallback          on_row_change;
    RegrooveLoopPatternCallback  on_loop_pattern;
    RegrooveLoopSongCallback     on_loop_song;
    RegroovePatternModeCallback  on_pattern_mode_change;
    RegrooveNoteCallback         on_note;
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
        // Use proper mute function instead of volume=0 to prevent pattern data from unmuting
        g->interactive.set_channel_mute_status(g->modext, ch, g->mute_states[ch]);
    }
}

static void reapply_volumes(struct Regroove* g) {
    if (!g->interactive_ok) return;
    for (int ch = 0; ch < g->num_channels; ++ch) {
        // Reapply channel volume (unless muted)
        if (!g->mute_states[ch]) {
            g->interactive.set_channel_volume(g->modext, ch, g->channel_volumes[ch]);
        }
    }
}

static void reapply_pannings(struct Regroove* g) {
    if (!g->interactive2_ok || !g->interactive2) return;
    for (int ch = 0; ch < g->num_channels; ++ch) {
        // Convert 0.0..1.0 to -1.0..1.0 for libopenmpt
        double libopenmpt_pan = (g->channel_pannings[ch] * 2.0) - 1.0;
        g->interactive2->set_channel_panning(g->modext, ch, libopenmpt_pan);
    }
}

static void apply_pending_mute_changes(struct Regroove* g) {
    if (!g->has_pending_mute_changes || !g->pending_mute_states) return;

    // Copy pending states to current states
    memcpy(g->mute_states, g->pending_mute_states, g->num_channels * sizeof(int));

    // Apply to engine
    if (g->interactive_ok) {
        reapply_mutes(g);
        reapply_volumes(g);
        reapply_pannings(g);
    }

    // Clear pending state
    free(g->pending_mute_states);
    g->pending_mute_states = NULL;
    g->has_pending_mute_changes = 0;

    // Clear all queued action flags
    if (g->queued_action_per_channel) {
        memset(g->queued_action_per_channel, 0, g->num_channels * sizeof(int));
    }
}

static void enqueue_command(struct Regroove* g, RegrooveCommandType type, int arg1, int arg2) {
    int next_tail = (g->command_queue_tail + 1) % RG_MAX_COMMANDS;
    if (next_tail != g->command_queue_head) {
        g->command_queue[g->command_queue_tail].type = type;
        g->command_queue[g->command_queue_tail].arg1 = arg1;
        g->command_queue[g->command_queue_tail].arg2 = arg2;
        g->command_queue[g->command_queue_tail].dval = 0.0;
        g->command_queue[g->command_queue_tail].arg3 = 0;
        g->command_queue[g->command_queue_tail].arg4 = 0;
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
        g->command_queue[g->command_queue_tail].arg3 = 0;
        g->command_queue[g->command_queue_tail].arg4 = 0;
        g->command_queue_tail = next_tail;
    }
}
static void enqueue_command_range(struct Regroove* g, RegrooveCommandType type,
                                   int start_order, int start_row, int end_order, int end_row) {
    int next_tail = (g->command_queue_tail + 1) % RG_MAX_COMMANDS;
    if (next_tail != g->command_queue_head) {
        g->command_queue[g->command_queue_tail].type = type;
        g->command_queue[g->command_queue_tail].arg1 = start_order;
        g->command_queue[g->command_queue_tail].arg2 = start_row;
        g->command_queue[g->command_queue_tail].arg3 = end_order;
        g->command_queue[g->command_queue_tail].arg4 = end_row;
        g->command_queue[g->command_queue_tail].dval = 0.0;
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
                        g->interactive.set_channel_mute_status(g->modext, cmd->arg1, g->mute_states[cmd->arg1]);
                }
                break;
            case RG_CMD_TOGGLE_CHANNEL_SINGLE:
                if (cmd->arg1 >= 0 && cmd->arg1 < g->num_channels) {
                    // Check if this channel is already soloed (unmuted while all others are muted)
                    int is_soloed = (g->mute_states[cmd->arg1] == 0);
                    if (is_soloed) {
                        for (int i = 0; i < g->num_channels; i++) {
                            if (i != cmd->arg1 && g->mute_states[i] == 0) {
                                is_soloed = 0;
                                break;
                            }
                        }
                    }

                    if (is_soloed) {
                        // Un-solo: unmute all channels
                        for (int i = 0; i < g->num_channels; ++i) {
                            g->mute_states[i] = 0;
                            if (g->interactive_ok)
                                g->interactive.set_channel_mute_status(g->modext, i, 0);
                        }
                    } else {
                        // Solo: mute all except this channel
                        for (int i = 0; i < g->num_channels; ++i) {
                            int mute = (i != cmd->arg1);
                            g->mute_states[i] = mute;
                            if (g->interactive_ok)
                                g->interactive.set_channel_mute_status(g->modext, i, mute);
                        }
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
                    if (g->interactive_ok && !g->mute_states[ch])
                        g->interactive.set_channel_volume(g->modext, ch, vol);
                }
                break;
            }
            case RG_CMD_SET_CHANNEL_PANNING: {
                int ch = cmd->arg1;
                double pan = cmd->dval;
                if (ch >= 0 && ch < g->num_channels) {
                    if (pan < 0.0) pan = 0.0;
                    if (pan > 1.0) pan = 1.0;
                    g->channel_pannings[ch] = pan;
                    if (g->interactive2_ok && g->interactive2) {
                        // Convert 0.0..1.0 to -1.0..1.0 for libopenmpt
                        double libopenmpt_pan = (pan * 2.0) - 1.0;
                        g->interactive2->set_channel_panning(g->modext, ch, libopenmpt_pan);
                    }
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
            case RG_CMD_QUEUE_NEXT_ORDER: {
                int cur_order = openmpt_module_get_current_order(g->mod);
                int next_order = cur_order + 1;
                if (next_order < g->num_orders) {
                    if (g->pattern_mode) {
                        g->pending_pattern_mode_order = next_order;
                        g->queued_jump_type = 1; // next
                    } else {
                        g->queued_order = next_order;
                        g->queued_row = 0;
                        g->has_queued_jump = 1;
                        g->queued_jump_type = 1; // next
                    }
                }
                break;
            }
            case RG_CMD_QUEUE_PREV_ORDER: {
                int cur_order = openmpt_module_get_current_order(g->mod);
                int prev_order = cur_order > 0 ? cur_order - 1 : 0;
                if (g->pattern_mode) {
                    g->pending_pattern_mode_order = prev_order;
                    g->queued_jump_type = 2; // prev
                } else {
                    g->queued_order = prev_order;
                    g->queued_row = 0;
                    g->has_queued_jump = 1;
                    g->queued_jump_type = 2; // prev
                }
                break;
            }
            case RG_CMD_QUEUE_ORDER:
                if (g->pattern_mode) {
                    g->pending_pattern_mode_order = cmd->arg1;
                    g->queued_jump_type = 3; // specific order
                } else {
                    g->queued_order = cmd->arg1;
                    g->queued_row = cmd->arg2;
                    g->has_queued_jump = 1;
                    g->queued_jump_type = 3; // specific order
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
                    g->queued_jump_type = 4; // pattern
                } else {
                    g->queued_order = target_order;
                    g->queued_row = 0;
                    g->has_queued_jump = 1;
                    g->queued_jump_type = 4; // pattern
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
                    printf("RG_CMD_JUMP_TO_PATTERN: pattern %d, searched and found order %d\n", pattern_index, target_order);
                } else {
                    printf("RG_CMD_JUMP_TO_PATTERN: pattern %d, explicit order %d\n", pattern_index, target_order);
                }

                // Update loop state for pattern mode
                g->loop_order = target_order;
                g->loop_pattern = pattern_index;
                g->full_loop_rows = openmpt_module_get_pattern_num_rows(g->mod, pattern_index);
                g->custom_loop_rows = 0;
                g->prev_row = -1;

                // Jump to the position immediately
                openmpt_module_set_position_order_row(g->mod, target_order, 0);
                printf("RG_CMD_JUMP_TO_PATTERN: Executed jump to order %d, row 0\n", target_order);
                if (g->interactive_ok) {
                    // reapply_mutes(g);  // Testing if this is still needed
                    reapply_volumes(g);
                    reapply_pannings(g);
                }
                break;
            }
            case RG_CMD_SET_LOOP_RANGE:
                // Set loop range: arg1=start_order, arg2=start_row, arg3=end_order, arg4=end_row
                g->loop_start_order = cmd->arg1;
                g->loop_start_row = cmd->arg2;
                g->loop_end_order = cmd->arg3;
                g->loop_end_row = cmd->arg4;
                // Don't activate loop yet (that's done by TRIGGER_LOOP or PLAY_TO_LOOP)
                break;
            case RG_CMD_TRIGGER_LOOP:
                // Jump to loop start immediately and begin looping
                if (g->loop_start_order >= 0 && g->loop_start_order < g->num_orders) {
                    openmpt_module_set_position_order_row(g->mod, g->loop_start_order, g->loop_start_row);
                } else {
                    // Single pattern mode: use current order
                    int cur_order = openmpt_module_get_current_order(g->mod);
                    openmpt_module_set_position_order_row(g->mod, cur_order, g->loop_start_row);
                }
                g->loop_range_enabled = 2; // ACTIVE
                apply_pending_mute_changes(g);
                if (g->interactive_ok) {
                    // reapply_mutes(g);  // Testing if this is still needed
                    reapply_volumes(g);
                    reapply_pannings(g);
                }
                g->prev_row = -1;
                break;
            case RG_CMD_PLAY_TO_LOOP:
                // Toggle: OFF→ARMED, ARMED→OFF, ACTIVE→OFF
                if (g->loop_range_enabled == 0) {
                    g->loop_range_enabled = 1; // OFF → ARMED
                } else {
                    g->loop_range_enabled = 0; // ARMED or ACTIVE → OFF
                }
                break;
            case RG_CMD_SET_PATTERN_MODE:
                g->pattern_mode = cmd->arg1;
                if (g->pattern_mode) {
                    g->loop_order   = openmpt_module_get_current_order(g->mod);
                    g->loop_pattern = openmpt_module_get_current_pattern(g->mod);
                    g->full_loop_rows = openmpt_module_get_pattern_num_rows(g->mod, g->loop_pattern);
                    g->custom_loop_rows = 0;
                    g->pending_pattern_mode_order = -1;
                    g->queued_jump_type = 0;  // Clear any queued jumps when toggling mode
                    g->prev_row = -1;
                }
                break;
            case RG_CMD_RETRIGGER_PATTERN: {
                int cur_order = openmpt_module_get_current_order(g->mod);
                openmpt_module_set_position_order_row(g->mod, cur_order, 0);
                if (g->interactive_ok) {
                    // reapply_mutes(g);  // Testing if this is still needed
                    reapply_volumes(g);
                    reapply_pannings(g);
                }
                g->prev_row = -1;
                break;
            }
            case RG_CMD_SET_CUSTOM_LOOP_ROWS:
                g->custom_loop_rows = cmd->arg1;
                if (g->custom_loop_rows < 0) g->custom_loop_rows = 0;
                g->prev_row = -1;
                break;
            case RG_CMD_QUEUE_CHANNEL_MUTE:
                if (cmd->arg1 >= 0 && cmd->arg1 < g->num_channels) {
                    // Allocate pending states if not yet allocated
                    if (!g->pending_mute_states) {
                        g->pending_mute_states = (int*)malloc(g->num_channels * sizeof(int));
                        // Copy current mute states as starting point
                        memcpy(g->pending_mute_states, g->mute_states, g->num_channels * sizeof(int));
                    }
                    // Toggle pending mute state for this channel
                    g->pending_mute_states[cmd->arg1] = !g->pending_mute_states[cmd->arg1];

                    // Track that this specific channel had MUTE queued
                    // BUT: if pending state == current state, that means we're canceling the change
                    if (g->queued_action_per_channel) {
                        if (g->pending_mute_states[cmd->arg1] != g->mute_states[cmd->arg1]) {
                            // There IS a pending change - mark it
                            g->queued_action_per_channel[cmd->arg1] = 1; // mute
                        } else {
                            // No change - cancel the queued action
                            g->queued_action_per_channel[cmd->arg1] = 0;
                        }
                    }

                    // Check if there are ANY pending changes left across all channels
                    int has_changes = 0;
                    for (int i = 0; i < g->num_channels; i++) {
                        if (g->pending_mute_states[i] != g->mute_states[i]) {
                            has_changes = 1;
                            break;
                        }
                    }
                    g->has_pending_mute_changes = has_changes;
                }
                break;
            case RG_CMD_QUEUE_CHANNEL_SOLO:
                if (cmd->arg1 >= 0 && cmd->arg1 < g->num_channels) {
                    // Allocate pending states if not yet allocated
                    if (!g->pending_mute_states) {
                        g->pending_mute_states = (int*)malloc(g->num_channels * sizeof(int));
                        // Copy current mute states as starting point
                        memcpy(g->pending_mute_states, g->mute_states, g->num_channels * sizeof(int));
                    }
                    // Check if this channel is currently pending solo (all others muted in pending)
                    int is_pending_solo = g->pending_mute_states[cmd->arg1] == 0;
                    if (is_pending_solo) {
                        // Check if all other channels are muted in pending
                        for (int i = 0; i < g->num_channels; i++) {
                            if (i != cmd->arg1 && g->pending_mute_states[i] == 0) {
                                is_pending_solo = 0;
                                break;
                            }
                        }
                    }

                    if (!is_pending_solo) {
                        // Set to solo: mute all, unmute this one
                        for (int i = 0; i < g->num_channels; i++) {
                            g->pending_mute_states[i] = (i != cmd->arg1) ? 1 : 0;
                        }
                        // Track that this specific channel had SOLO queued
                        // Clear all previous queued actions and set only this channel to SOLO
                        if (g->queued_action_per_channel) {
                            memset(g->queued_action_per_channel, 0, g->num_channels * sizeof(int));
                            g->queued_action_per_channel[cmd->arg1] = 2; // solo
                        }
                    } else {
                        // Un-solo: unmute all
                        for (int i = 0; i < g->num_channels; i++) {
                            g->pending_mute_states[i] = 0;
                        }
                        // Clear all queued action flags
                        if (g->queued_action_per_channel) {
                            memset(g->queued_action_per_channel, 0, g->num_channels * sizeof(int));
                        }
                    }
                    g->has_pending_mute_changes = 1;
                }
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
    g->interpolation_filter = 1;  // Default to linear
    g->stereo_separation = 100;   // Default to 100%
    g->dither = 1;                // Default to library default
    g->amiga_resampler = 0;       // Default to disabled
    g->amiga_filter_type = 0;     // Default to auto
    g->pattern_mode = 0;
    g->pending_pattern_mode_order = -1;
    g->queued_jump_type = 0;

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
    g->channel_pannings = (double*)calloc(g->num_channels, sizeof(double));
    for (int i = 0; i < g->num_channels; ++i) {
        g->channel_volumes[i] = 1.0;
        g->channel_pannings[i] = 0.5;  // Center
    }

    g->interactive_ok = 0;
    if (openmpt_module_ext_get_interface(
            g->modext, LIBOPENMPT_EXT_C_INTERFACE_INTERACTIVE,
            &g->interactive, sizeof(g->interactive)) != 0) {
        g->interactive_ok = 1;
        reapply_mutes(g);
        reapply_volumes(g);
        reapply_pannings(g);
    }

    g->interactive2_ok = 0;
    g->interactive2 = NULL;
    openmpt_module_ext_interface_interactive2 interactive2_temp;
    if (openmpt_module_ext_get_interface(
            g->modext, "interactive2",
            &interactive2_temp, sizeof(interactive2_temp)) != 0) {
        g->interactive2 = (openmpt_module_ext_interface_interactive2*)malloc(sizeof(openmpt_module_ext_interface_interactive2));
        if (g->interactive2) {
            memcpy(g->interactive2, &interactive2_temp, sizeof(interactive2_temp));
            g->interactive2_ok = 1;

            // Read module's default panning for each channel
            // libopenmpt returns -1.0 to 1.0, convert to 0.0 to 1.0
            for (int i = 0; i < g->num_channels; ++i) {
                double libopenmpt_pan = g->interactive2->get_channel_panning(g->modext, i);
                g->channel_pannings[i] = (libopenmpt_pan + 1.0) / 2.0;  // Convert -1.0..1.0 to 0.0..1.0
                printf("Channel %d initial panning from libopenmpt: %f -> converted to %f\n", i, libopenmpt_pan, g->channel_pannings[i]);
            }
        }
    }

    // Set interpolation filter (OPENMPT_MODULE_RENDER_INTERPOLATIONFILTER_LENGTH = 3)
    openmpt_module_set_render_param(g->mod, 3, g->interpolation_filter);

    // Set stereo separation (OPENMPT_MODULE_RENDER_STEREOSEPARATION_PERCENT = 2)
    openmpt_module_set_render_param(g->mod, 2, g->stereo_separation);

    // Set dither
    openmpt_module_ctl_set_integer(g->mod, "dither", g->dither);

    // Set Amiga resampler (only affects 4-channel Amiga modules)
    openmpt_module_ctl_set_boolean(g->mod, "render.resampler.emulate_amiga", g->amiga_resampler);

    // Set Amiga filter type
    const char* amiga_filter_names[] = {"auto", "a500", "a1200", "unfiltered"};
    if (g->amiga_filter_type >= 0 && g->amiga_filter_type <= 3) {
        openmpt_module_ctl_set_text(g->mod, "render.resampler.emulate_amiga_type", amiga_filter_names[g->amiga_filter_type]);
    }

    // Disable automatic looping - we'll handle it manually for cleaner loop points
    openmpt_module_set_repeat_count(g->mod, 0);

    g->loop_order = openmpt_module_get_current_order(g->mod);
    g->loop_pattern = openmpt_module_get_current_pattern(g->mod);
    g->full_loop_rows = openmpt_module_get_pattern_num_rows(g->mod, g->loop_pattern);
    g->custom_loop_rows = 0;
    g->prev_row = -1;
    g->prev_order = g->loop_order;

    // Initialize loop range system
    g->loop_range_enabled = 0;  // OFF
    g->loop_start_order = -1;   // Use current pattern
    g->loop_start_row = 0;
    g->loop_end_order = -1;     // Use current pattern
    g->loop_end_row = 0;

    // Initialize pending mute/solo state
    g->pending_mute_states = NULL;
    g->has_pending_mute_changes = 0;
    g->queued_action_per_channel = (int*)calloc(g->num_channels, sizeof(int));

    g->on_order_change = NULL;
    g->on_row_change = NULL;
    g->on_loop_pattern = NULL;
    g->on_loop_song = NULL;
    g->on_note = NULL;
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
    if (g->channel_pannings) free(g->channel_pannings);
    if (g->interactive2) free(g->interactive2);
    if (g->pending_mute_states) free(g->pending_mute_states);
    if (g->queued_action_per_channel) free(g->queued_action_per_channel);
    free(g);
}

void regroove_set_callbacks(Regroove *g, struct RegrooveCallbacks *cb) {
    if (!g || !cb) return;
    g->on_order_change = cb->on_order_change;
    g->on_row_change = cb->on_row_change;
    g->on_loop_pattern = cb->on_loop_pattern;
    g->on_loop_song = cb->on_loop_song;
    g->on_note = cb->on_note;
    g->callback_userdata = cb->userdata;
}

int regroove_render_audio(Regroove* g, int16_t* buffer, int frames) {
    process_commands(g);

    // Note: Queued jumps are now handled at pattern boundaries in song playback mode
    // (see below in the normal playback section)

    // Store position BEFORE rendering
    int prev_order_before = openmpt_module_get_current_order(g->mod);
    int prev_row_before = openmpt_module_get_current_row(g->mod);

    int count = openmpt_module_read_interleaved_stereo(
        g->mod, g->samplerate * g->pitch_factor, frames, buffer);

    // Get position AFTER rendering
    int cur_order = openmpt_module_get_current_order(g->mod);
    int cur_pattern = openmpt_module_get_current_pattern(g->mod);
    int cur_row = openmpt_module_get_current_row(g->mod);
    int loop_rows = g->custom_loop_rows > 0 ? g->custom_loop_rows : g->full_loop_rows;

    // Detect if libopenmpt looped during this buffer (order decreased or wrapped to 0)
    // COMMENTED OUT: Not necessary in song mode for now
    /*
    int num_orders = openmpt_module_get_num_orders(g->mod);
    int loop_occurred_in_buffer = 0;

    if (!g->pattern_mode && g->loop_range_enabled == 0) {
        // Check if we wrapped from a high order back to order 0
        if (prev_order_before > 0 && cur_order == 0) {
            loop_occurred_in_buffer = 1;
        }
        // Also detect if order decreased (shouldn't happen in normal playback)
        else if (cur_order < prev_order_before && cur_order >= 0) {
            loop_occurred_in_buffer = 1;
        }
    }

    // If loop occurred mid-buffer (song mode), discard this buffer and render fresh from order 0
    if (loop_occurred_in_buffer) {
        // Jump back to order 0, row 0
        openmpt_module_set_position_order_row(g->mod, 0, 0);
        if (g->on_loop_song) {
            g->on_loop_song(g->callback_userdata);
        }
        // Re-render a clean buffer starting from the beginning
        count = openmpt_module_read_interleaved_stereo(
            g->mod, g->samplerate * g->pitch_factor, frames, buffer);
        cur_order = 0;
        cur_pattern = openmpt_module_get_current_pattern(g->mod);
        cur_row = openmpt_module_get_current_row(g->mod);
        g->prev_row = -1;
    }
    */

    // Pattern mode: detect if order escaped during render (pattern break/jump in the pattern data)
    // BUT: Don't treat it as an escape if we have a pending pattern mode order change queued
    if (g->pattern_mode && prev_order_before == g->loop_order && cur_order != g->loop_order &&
        g->pending_pattern_mode_order == -1) {
        // Jump back to the loop pattern start
        openmpt_module_set_position_order_row(g->mod, g->loop_order, 0);
        apply_pending_mute_changes(g);
        if (g->interactive_ok) {
            // reapply_mutes(g);  // Testing if this is still needed
            reapply_volumes(g);
            reapply_pannings(g);
        }
        // Re-render a clean buffer from pattern start
        count = openmpt_module_read_interleaved_stereo(
            g->mod, g->samplerate * g->pitch_factor, frames, buffer);
        cur_order = g->loop_order;
        cur_pattern = openmpt_module_get_current_pattern(g->mod);
        cur_row = openmpt_module_get_current_row(g->mod);
        g->prev_row = -1;
        if (g->on_loop_pattern) {
            g->on_loop_pattern(g->loop_order, g->loop_pattern, g->callback_userdata);
        }
    }

    // Loop range system: check if we should activate or loop back
    if (g->loop_range_enabled > 0) {
        // Determine loop boundaries
        int loop_start_order = (g->loop_start_order >= 0) ? g->loop_start_order : cur_order;
        int loop_end_order = (g->loop_end_order >= 0) ? g->loop_end_order : cur_order;

        // Check if we're at the loop end position
        int at_loop_end = 0;
        if (cur_order == loop_end_order && cur_row >= g->loop_end_row) {
            at_loop_end = 1;
        } else if (cur_order > loop_end_order) {
            at_loop_end = 1;
        }

        // Check if we're at or past the loop start position (for ARMED state)
        int at_loop_start = 0;
        if (g->loop_range_enabled == 1) { // ARMED
            if (cur_order == loop_start_order && cur_row >= g->loop_start_row) {
                at_loop_start = 1;
            } else if (cur_order > loop_start_order && cur_order <= loop_end_order) {
                at_loop_start = 1; // We're between start and end
            }
        }

        // State transitions
        if (g->loop_range_enabled == 1 && at_loop_start) {
            // ARMED → ACTIVE when loop start is reached
            g->loop_range_enabled = 2;
        }

        if (g->loop_range_enabled == 2 && at_loop_end) {
            // ACTIVE: Jump back to loop start
            openmpt_module_set_position_order_row(g->mod, loop_start_order, g->loop_start_row);
            apply_pending_mute_changes(g);
            if (g->interactive_ok) {
                // reapply_mutes(g);  // Testing if this is still needed
                reapply_volumes(g);
                reapply_pannings(g);
            }
            if (g->on_loop_pattern) {
                int loop_pattern = openmpt_module_get_order_pattern(g->mod, loop_start_order);
                g->on_loop_pattern(loop_start_order, loop_pattern, g->callback_userdata);
            }
            g->prev_row = -1;
        } else {
            g->prev_row = cur_row;
        }
    }
    else if (g->pattern_mode) {
        int at_custom_loop_end = (g->custom_loop_rows > 0 && cur_row >= loop_rows);
        int at_full_pattern_end = (g->custom_loop_rows == 0 && g->prev_row == loop_rows - 1 && cur_row == 0);

        // Detect early pattern exit (pattern break/jump command) in the MIDDLE of a pattern
        // Only detect if we're not at a normal boundary AND the order changed
        int escaped_loop_order = (cur_order != g->loop_order) &&
                                 !at_custom_loop_end &&
                                 !at_full_pattern_end &&
                                 (g->prev_row != -1);

        // Pattern boundary = normal end only (NOT escape - we handle that separately)
        int at_pattern_boundary = at_custom_loop_end || at_full_pattern_end;

        // --- CRITICAL: process pending pattern jump first and return ---
        if (at_pattern_boundary && g->pending_pattern_mode_order != -1) {
            // Check if we're already at the target order (natural advance already happened)
            int is_same_order = (g->pending_pattern_mode_order == cur_order);

            if (is_same_order) {
                // Already at target order - just update loop state without re-rendering
                g->loop_order = cur_order;
                g->loop_pattern = cur_pattern;
                g->full_loop_rows = openmpt_module_get_pattern_num_rows(g->mod, g->loop_pattern);
                g->custom_loop_rows = 0;
                g->prev_row = -1;
                g->prev_order = g->loop_order;
                apply_pending_mute_changes(g);
                if (g->interactive_ok) {
                    // reapply_mutes(g);  // Testing if this is still needed
                    reapply_volumes(g);
                    reapply_pannings(g);
                }
                if (g->on_loop_pattern)
                    g->on_loop_pattern(g->loop_order, g->loop_pattern, g->callback_userdata);
            } else {
                // Different order - do the jump and re-render
                g->loop_order = g->pending_pattern_mode_order;
                g->loop_pattern = openmpt_module_get_order_pattern(g->mod, g->loop_order);
                g->full_loop_rows = openmpt_module_get_pattern_num_rows(g->mod, g->loop_pattern);
                g->custom_loop_rows = 0;

                // Validate the new pattern has valid rows
                if (g->full_loop_rows > 0) {
                    openmpt_module_set_position_order_row(g->mod, g->loop_order, 0);
                    apply_pending_mute_changes(g);
                    if (g->interactive_ok) {
                        // reapply_mutes(g);  // Testing if this is still needed
                        reapply_volumes(g);
                        reapply_pannings(g);
                    }
                    // Re-render a clean buffer from the new pattern start to avoid glitches
                    count = openmpt_module_read_interleaved_stereo(
                        g->mod, g->samplerate * g->pitch_factor, frames, buffer);
                    cur_order = g->loop_order;
                    cur_pattern = g->loop_pattern;
                    cur_row = openmpt_module_get_current_row(g->mod);
                    if (g->on_loop_pattern)
                        g->on_loop_pattern(g->loop_order, g->loop_pattern, g->callback_userdata);
                }
                g->prev_row = -1;
                g->prev_order = g->loop_order;  // Update prev_order to avoid false escape detection
            }

            // Clear pending state (whether jump happened or not)
            g->pending_pattern_mode_order = -1;
            g->queued_jump_type = 0;  // Clear visual feedback

            // Return regardless of whether jump happened
            return count;
        }

        // Then do standard wrap/loop logic
        if (at_pattern_boundary) {
            // At normal pattern end - wrap to beginning
            openmpt_module_set_position_order_row(g->mod, g->loop_order, 0);
            apply_pending_mute_changes(g);
            if (g->interactive_ok) {
                // reapply_mutes(g);  // Testing if this is still needed
                reapply_volumes(g);
                reapply_pannings(g);
            }
            if (g->on_loop_pattern)
                g->on_loop_pattern(g->loop_order, g->loop_pattern, g->callback_userdata);
            g->prev_row = -1;
        } else if (escaped_loop_order) {
            // Pattern break/jump command detected in middle of pattern
            // Switch the loop to the new pattern instead of snapping back
            g->loop_order = cur_order;
            g->loop_pattern = cur_pattern;
            g->full_loop_rows = openmpt_module_get_pattern_num_rows(g->mod, g->loop_pattern);
            g->custom_loop_rows = 0;
            g->prev_row = cur_row;

            // Notify UI that loop switched to new pattern
            if (g->on_loop_pattern)
                g->on_loop_pattern(g->loop_order, g->loop_pattern, g->callback_userdata);
        } else {
            // Normal playback - just update row tracking
            g->prev_row = cur_row;
        }
    }
    else {
        // Normal song playback mode - apply pending changes at pattern boundaries
        // Detect pattern boundary: order changed OR row wrapped to 0
        int boundary_crossed = 0;
        if (g->prev_order != -1 && cur_order != g->prev_order) {
            // Order changed - definitely a pattern boundary
            boundary_crossed = 1;
        } else if (g->prev_row != -1 && g->prev_row != 0 && cur_row == 0) {
            // Row wrapped to 0 from a non-zero row - pattern boundary
            boundary_crossed = 1;
        }

        if (boundary_crossed) {
            // Apply pending mute changes at pattern boundary
            if (g->has_pending_mute_changes) {
                apply_pending_mute_changes(g);
                if (g->interactive_ok) {
                    // reapply_mutes(g);  // Testing if this is still needed
                    reapply_volumes(g);
                    reapply_pannings(g);
                }
            }

            // Execute queued jump at pattern boundary
            if (g->has_queued_jump) {
                openmpt_module_set_position_order_row(g->mod, g->queued_order, g->queued_row);
                if (g->interactive_ok) {
                    // reapply_mutes(g);  // Testing if this is still needed
                    reapply_volumes(g);
                    reapply_pannings(g);
                }
                g->has_queued_jump = 0;
                g->queued_jump_type = 0;
                g->prev_row = -1;
            }
        }

        // Track previous row for next boundary detection
        g->prev_row = cur_row;
    }

    g->prev_order = cur_order;

    // --- CALL UI CALLBACKS AFTER ALL JUMP LOGIC ---
    int final_order = openmpt_module_get_current_order(g->mod);
    int final_pattern = openmpt_module_get_current_pattern(g->mod);
    int final_row = openmpt_module_get_current_row(g->mod);

    if (g->on_order_change && g->last_msg_order != final_order) {
        // Update full_loop_rows to reflect the current pattern's row count
        // This is needed for MPTM files where patterns have different lengths
        g->full_loop_rows = openmpt_module_get_pattern_num_rows(g->mod, final_pattern);

        g->on_order_change(final_order, final_pattern, g->callback_userdata);
        g->last_msg_order = final_order;
    }
    // --- Call note callback for MIDI output (if registered) ---
    // Do this BEFORE updating last_msg_row so it triggers on row changes
    if (g->on_note && g->last_msg_row != final_row) {
        // Process all channels for note events at the current row
        for (int ch = 0; ch < g->num_channels; ch++) {
            // Skip muted channels - don't send MIDI notes for muted channels
            if (g->mute_states && g->mute_states[ch]) {
                continue;
            }

            // Get pattern cell data for this channel
            const char *note_str = openmpt_module_format_pattern_row_channel(
                g->mod, final_pattern, final_row, ch, 0, 1);

            if (note_str && strlen(note_str) > 0) {
                // Parse note, instrument, volume, and effect from formatted string
                // Format is typically: "C-5 01 .. ..."
                int note = -1;
                int instrument = -1;
                int volume = -1;
                int effect_cmd = 0;
                int effect_param = 0;

                // Parse note (first 3 chars)
                if (strlen(note_str) >= 3) {
                    if (note_str[0] >= 'A' && note_str[0] <= 'G') {
                        // Calculate MIDI note number from tracker note
                        // C-4 should be MIDI note 48 (tracker's middle C)
                        int octave = note_str[2] - '0';
                        int base_note = 0;
                        switch (note_str[0]) {
                            case 'C': base_note = 0; break;
                            case 'D': base_note = 2; break;
                            case 'E': base_note = 4; break;
                            case 'F': base_note = 5; break;
                            case 'G': base_note = 7; break;
                            case 'A': base_note = 9; break;
                            case 'B': base_note = 11; break;
                        }
                        if (note_str[1] == '#' || note_str[1] == '-') {
                            if (note_str[1] == '#') base_note++;
                        }
                        note = octave * 12 + base_note;
                    } else if (strncmp(note_str, "===", 3) == 0 ||
                               strncmp(note_str, "OFF", 3) == 0) {
                        // Note-off or key-off indicator
                        note = -2; // Special value for note-off
                    }
                }

                // Parse instrument (chars 4-5, hex)
                if (strlen(note_str) >= 6 && note_str[4] != '.' && note_str[5] != '.') {
                    char inst_str[3] = {note_str[4], note_str[5], 0};
                    instrument = (int)strtol(inst_str, NULL, 16);
                }

                // Parse volume (chars 7-8)
                if (strlen(note_str) >= 9 && note_str[7] != '.' && note_str[8] != '.') {
                    char vol_str[3] = {note_str[7], note_str[8], 0};
                    volume = (int)strtol(vol_str, NULL, 16);
                }

                // Parse effect command and parameter (chars 10-12)
                if (strlen(note_str) >= 13) {
                    if (note_str[10] != '.' && note_str[11] != '.' && note_str[12] != '.') {
                        char cmd_str[2] = {note_str[10], 0};
                        char param_str[3] = {note_str[11], note_str[12], 0};
                        effect_cmd = (int)strtol(cmd_str, NULL, 16);
                        effect_param = (int)strtol(param_str, NULL, 16);
                    }
                }

                // Only call callback if there's a note or an effect command
                if (note >= -2 || effect_cmd != 0) {
                    // Apply channel volume slider to the velocity
                    int adjusted_volume = volume;
                    if (g->channel_volumes) {
                        if (volume >= 0) {
                            // Apply channel volume slider (0.0-1.0) directly to tracker volume
                            adjusted_volume = (int)(volume * g->channel_volumes[ch]);
                        } else {
                            // No volume specified in pattern - use default max (64) and apply slider
                            adjusted_volume = (int)(64 * g->channel_volumes[ch]);
                        }
                    }
                    g->on_note(ch, note, instrument, adjusted_volume, effect_cmd, effect_param,
                              g->callback_userdata);
                }
            }
        }
    }

    // --- Call row change callback (after note callback so notes are processed first) ---
    if (g->on_row_change && g->last_msg_row != final_row) {
        g->on_row_change(final_order, final_row, g->callback_userdata);
        g->last_msg_row = final_row;
    }

    // --- Manual song looping - COMMENTED OUT: Song should play once and stop, not loop ---
    /*
    // Detect if we've reached the end (after last order's last row)
    // This happens when openmpt stops because repeat_count=0
    if (count == 0 && !g->pattern_mode && g->loop_range_enabled == 0) {
        // Song ended - manually loop back to start
        openmpt_module_set_position_order_row(g->mod, 0, 0);
        if (g->on_loop_song) {
            g->on_loop_song(g->callback_userdata);
        }
        // Re-render to get audio for the loop start
        count = openmpt_module_read_interleaved_stereo(
            g->mod, g->samplerate * g->pitch_factor, frames, buffer);
        cur_order = 0;
        cur_row = 0;
        g->prev_row = -1;
    }
    */

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
    enqueue_command(g, RG_CMD_QUEUE_NEXT_ORDER, 0, 0);
}
void regroove_queue_prev_order(Regroove* g) {
    enqueue_command(g, RG_CMD_QUEUE_PREV_ORDER, 0, 0);
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
        printf("regroove_jump_to_order: Jumping to order %d (pattern %d)\n", target_order, target_pattern);
        enqueue_command(g, RG_CMD_JUMP_TO_PATTERN, target_pattern, target_order);
    } else {
        printf("regroove_jump_to_order: FAILED - order %d out of range (0-%d)\n", order, g->num_orders - 1);
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
void regroove_set_position_row(Regroove* g, int row) {
    if (!g || !g->mod) return;

    // Get current order and pattern
    int current_order = openmpt_module_get_current_order(g->mod);
    int current_pattern = openmpt_module_get_current_pattern(g->mod);
    int num_rows = openmpt_module_get_pattern_num_rows(g->mod, current_pattern);

    // Validate row
    if (row < 0) row = 0;
    if (row >= num_rows) row = num_rows - 1;

    // Set position immediately (not queued)
    openmpt_module_set_position_order_row(g->mod, current_order, row);
}
void regroove_clear_pending_jump(Regroove* g) {
    if (!g) return;
    // Clear pending jump state without affecting pattern mode
    g->pending_pattern_mode_order = -1;
    g->queued_jump_type = 0;
    g->has_queued_jump = 0;
}
// Loop range system
void regroove_set_loop_range(Regroove* g, int start_order, int start_row, int end_order, int end_row) {
    enqueue_command_range(g, RG_CMD_SET_LOOP_RANGE, start_order, start_row, end_order, end_row);
}
void regroove_set_loop_start_here(Regroove* g) {
    int cur_order = openmpt_module_get_current_order(g->mod);
    int cur_row = openmpt_module_get_current_row(g->mod);
    enqueue_command_range(g, RG_CMD_SET_LOOP_RANGE, cur_order, cur_row, g->loop_end_order, g->loop_end_row);
}
void regroove_set_loop_end_here(Regroove* g) {
    int cur_order = openmpt_module_get_current_order(g->mod);
    int cur_row = openmpt_module_get_current_row(g->mod);
    enqueue_command_range(g, RG_CMD_SET_LOOP_RANGE, g->loop_start_order, g->loop_start_row, cur_order, cur_row);
}
void regroove_trigger_loop(Regroove* g) {
    enqueue_command(g, RG_CMD_TRIGGER_LOOP, 0, 0);
}
void regroove_play_to_loop(Regroove* g) {
    enqueue_command(g, RG_CMD_PLAY_TO_LOOP, 0, 0);
}
int regroove_get_loop_state(const Regroove* g) {
    return g ? g->loop_range_enabled : 0;
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

void regroove_queue_channel_mute(Regroove *g, int ch) {
    enqueue_command(g, RG_CMD_QUEUE_CHANNEL_MUTE, ch, 0);
}

void regroove_toggle_channel_solo(Regroove* g, int ch) {
    enqueue_command(g, RG_CMD_TOGGLE_CHANNEL_SINGLE, ch, 0);
}

void regroove_queue_channel_solo(Regroove *g, int ch) {
    enqueue_command(g, RG_CMD_QUEUE_CHANNEL_SOLO, ch, 0);
}

void regroove_set_channel_volume(Regroove *g, int ch, double vol) {
    enqueue_command_d(g, RG_CMD_SET_CHANNEL_VOLUME, ch, vol);
}

double regroove_get_channel_volume(const Regroove* g, int ch) {
    if (!g || ch < 0 || ch >= g->num_channels) return 0.0;
    return g->channel_volumes[ch];
}

void regroove_set_channel_panning(Regroove *g, int ch, double pan) {
    enqueue_command_d(g, RG_CMD_SET_CHANNEL_PANNING, ch, pan);
}

double regroove_get_channel_panning(const Regroove* g, int ch) {
    if (!g || ch < 0 || ch >= g->num_channels) return 0.5;
    return g->channel_pannings[ch];
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

void regroove_set_interpolation_filter(Regroove* g, int filter) {
    if (!g || !g->mod) return;
    // Validate filter value: 0, 1, 2, or 4
    if (filter != 0 && filter != 1 && filter != 2 && filter != 4) return;
    g->interpolation_filter = filter;
    // OPENMPT_MODULE_RENDER_INTERPOLATIONFILTER_LENGTH = 3
    openmpt_module_set_render_param(g->mod, 3, filter);
}

int regroove_get_interpolation_filter(const Regroove* g) {
    if (!g) return 1;  // Default to linear
    return g->interpolation_filter;
}

void regroove_set_stereo_separation(Regroove* g, int separation) {
    if (!g || !g->mod) return;
    // Clamp to valid range 0-200
    if (separation < 0) separation = 0;
    if (separation > 200) separation = 200;
    g->stereo_separation = separation;
    // OPENMPT_MODULE_RENDER_STEREOSEPARATION_PERCENT = 2
    openmpt_module_set_render_param(g->mod, 2, separation);
}

int regroove_get_stereo_separation(const Regroove* g) {
    if (!g) return 100;  // Default
    return g->stereo_separation;
}

void regroove_set_dither(Regroove* g, int dither) {
    if (!g || !g->mod) return;
    // Validate dither value: 0-3
    if (dither < 0 || dither > 3) return;
    g->dither = dither;
    openmpt_module_ctl_set_integer(g->mod, "dither", dither);
}

int regroove_get_dither(const Regroove* g) {
    if (!g) return 1;  // Default to library default
    return g->dither;
}

void regroove_set_amiga_resampler(Regroove* g, int enabled) {
    if (!g || !g->mod) return;
    g->amiga_resampler = enabled ? 1 : 0;
    openmpt_module_ctl_set_boolean(g->mod, "render.resampler.emulate_amiga", enabled);
}

int regroove_get_amiga_resampler(const Regroove* g) {
    if (!g) return 0;  // Default to disabled
    return g->amiga_resampler;
}

void regroove_set_amiga_filter_type(Regroove* g, int filter_type) {
    if (!g || !g->mod) return;
    // Validate filter type: 0-3
    if (filter_type < 0 || filter_type > 3) return;
    g->amiga_filter_type = filter_type;
    const char* amiga_filter_names[] = {"auto", "a500", "a1200", "unfiltered"};
    openmpt_module_ctl_set_text(g->mod, "render.resampler.emulate_amiga_type", amiga_filter_names[filter_type]);
}

int regroove_get_amiga_filter_type(const Regroove* g) {
    if (!g) return 0;  // Default to auto
    return g->amiga_filter_type;
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
int regroove_has_pending_mute_changes(const Regroove *g) {
    return g ? g->has_pending_mute_changes : 0;
}
int regroove_get_pending_channel_mute(const Regroove *g, int ch) {
    if (!g || ch < 0 || ch >= g->num_channels) return 0;
    if (!g->pending_mute_states) return g->mute_states[ch];  // No pending changes, return current state
    return g->pending_mute_states[ch];
}
int regroove_get_queued_action_for_channel(const Regroove *g, int ch) {
    if (!g || ch < 0 || ch >= g->num_channels || !g->queued_action_per_channel) return 0;
    return g->queued_action_per_channel[ch];
}
int regroove_get_queued_jump_type(const Regroove *g) {
    return g ? g->queued_jump_type : 0;
}
int regroove_get_queued_order(const Regroove *g) {
    if (!g) return -1;
    // In pattern mode, queued order is stored in pending_pattern_mode_order
    if (g->pattern_mode && g->queued_jump_type > 0) {
        return g->pending_pattern_mode_order;
    }
    return g->queued_order;
}
int regroove_get_pattern_mode(const Regroove* g) { return g->pattern_mode; }
int regroove_get_custom_loop_rows(const Regroove* g) { return g->custom_loop_rows; }
int regroove_get_full_pattern_rows(const Regroove* g) { return g->full_loop_rows; }
int regroove_get_pattern_num_rows(const Regroove* g, int pattern) {
    if (!g || !g->mod) return 0;
    return openmpt_module_get_pattern_num_rows(g->mod, pattern);
}
double regroove_get_current_bpm(const Regroove* g) {
    if (!g || !g->mod) return 0.0;
    // Use tempo2 instead of estimated_bpm to get the actual tempo value
    // estimated_bpm accounts for speed (ticks/row) which makes MIDI Clock timing wrong
    // MIDI Clock should represent the musical tempo, not the effective playback speed
    return openmpt_module_get_current_tempo2(g->mod);
}

int regroove_get_current_speed(const Regroove* g) {
    if (!g || !g->mod) return 6;  // Default to 6 ticks/row
    return openmpt_module_get_current_speed(g->mod);
}

int regroove_get_pattern_cell(const Regroove *g, int pattern, int row, int channel, char *buffer, size_t buffer_size) {
    if (!g || !g->mod || !buffer || buffer_size < 32) return -1;

    // Use openmpt API to get formatted pattern cell
    const char *cell_text = openmpt_module_format_pattern_row_channel(g->mod, pattern, row, channel, buffer_size, 1);
    if (!cell_text) {
        buffer[0] = '\0';
        return -1;
    }

    // Copy to buffer
    snprintf(buffer, buffer_size, "%s", cell_text);
    openmpt_free_string(cell_text);
    return 0;
}

int regroove_get_num_instruments(const Regroove *g) {
    if (!g || !g->mod) return 0;
    return openmpt_module_get_num_instruments(g->mod);
}

const char* regroove_get_instrument_name(const Regroove *g, int index) {
    if (!g || !g->mod) return NULL;
    return openmpt_module_get_instrument_name(g->mod, index);
}

int regroove_get_num_samples(const Regroove *g) {
    if (!g || !g->mod) return 0;
    return openmpt_module_get_num_samples(g->mod);
}

const char* regroove_get_sample_name(const Regroove *g, int index) {
    if (!g || !g->mod) return NULL;
    return openmpt_module_get_sample_name(g->mod, index);
}
