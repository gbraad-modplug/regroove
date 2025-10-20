#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/stat.h>
#include <SDL.h>
#include "regroove_common.h"
#include "midi.h"
#include "midi_output.h"

static volatile int running = 1;
static struct termios orig_termios;

// --- Shared state ---
static RegrooveCommonState *common_state = NULL;
static SDL_AudioDeviceID audio_device_id = 0;

// MIDI output state
static int midi_output_device = -1;  // -1 = disabled
static int midi_output_enabled = 0;

// Phrase playback state
#define MAX_ACTIVE_PHRASES 16
struct ActivePhrase {
    int phrase_index;           // Which phrase is playing (-1 = inactive)
    int current_step;           // Current step index
    int delay_counter;          // Rows remaining before executing current step
};
static struct ActivePhrase active_phrases[MAX_ACTIVE_PHRASES];
static int active_phrase_count = 0;
static int executing_phrase_action = 0;  // Track when executing phrase steps


static void handle_sigint(int sig) { (void)sig; running = 0; }
static void tty_restore(void) {
    if (orig_termios.c_cflag) {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
    }
}
static int tty_make_raw_nonblocking(void) {
    if (!isatty(STDIN_FILENO)) return -1;
    if (tcgetattr(STDIN_FILENO, &orig_termios) < 0) return -1;
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags != -1) fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    atexit(tty_restore);
    return 0;
}
static int read_key_nonblocking(void) {
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 1) return (int)c;
    return -1;
}

static void print_song_order(Regroove *g) {
    printf("Song order list (%d entries):\n", regroove_get_num_orders(g));
    for (int ord = 0; ord < regroove_get_num_orders(g); ++ord) {
        int pat = regroove_get_order_pattern(g, ord);
        printf("  Order %2d -> Pattern %2d\n", ord, pat);
    }
    printf("--------------------------------------\n");
}

// Forward declarations
static void handle_input_event(InputEvent *event);
static void update_phrases(void);

// Trigger phrase playback
static void trigger_phrase(int phrase_index) {
    printf("trigger_phrase called with index %d\n", phrase_index);

    if (!common_state || !common_state->metadata) {
        printf("  ERROR: common_state or metadata is NULL\n");
        return;
    }
    if (phrase_index < 0 || phrase_index >= common_state->metadata->phrase_count) {
        printf("  ERROR: phrase_index %d out of range (count=%d)\n",
               phrase_index, common_state->metadata->phrase_count);
        return;
    }

    const Phrase *phrase = &common_state->metadata->phrases[phrase_index];
    if (phrase->step_count == 0) {
        printf("  ERROR: phrase has 0 steps\n");
        return;
    }

    // Cancel all currently active phrases to avoid conflicts
    // Only one phrase should run at a time for predictable behavior
    for (int i = 0; i < MAX_ACTIVE_PHRASES; i++) {
        if (active_phrases[i].phrase_index != -1) {
            printf("Cancelling Phrase %d (was on step %d)\n",
                   active_phrases[i].phrase_index + 1,
                   active_phrases[i].current_step + 1);
            active_phrases[i].phrase_index = -1;
        }
    }

    // Use slot 0 for the new phrase
    int slot = 0;

    // Initialize the active phrase
    active_phrases[slot].phrase_index = phrase_index;
    active_phrases[slot].current_step = 0;
    active_phrases[slot].delay_counter = phrase->steps[0].delay_rows;

    // Auto-start playback if not already playing
    // Phrases need playback to be active for their timing to work
    if (common_state->paused && common_state->player) {
        if (audio_device_id) SDL_PauseAudioDevice(audio_device_id, 0);
        common_state->paused = 0;
        printf("Auto-started playback for Phrase %d (audio unpaused)\n", phrase_index + 1);
    }

    printf("Triggered Phrase %d (%d steps)\n", phrase_index + 1, phrase->step_count);
}

// Update active phrases (called on every row)
static void update_phrases() {
    if (!common_state || !common_state->metadata) return;
    if (!common_state->player) return;

    // Only update phrases when module is playing
    // This ensures delay_counter decrements sync with pattern rows
    if (common_state->paused) return;

    for (int i = 0; i < MAX_ACTIVE_PHRASES; i++) {
        struct ActivePhrase *ap = &active_phrases[i];
        if (ap->phrase_index == -1) continue;

        const Phrase *phrase = &common_state->metadata->phrases[ap->phrase_index];
        if (ap->current_step >= phrase->step_count) {
            // Phrase complete
            ap->phrase_index = -1;
            continue;
        }

        const PhraseStep *step = &phrase->steps[ap->current_step];

        // Decrement delay counter
        if (ap->delay_counter > 0) {
            ap->delay_counter--;
            printf("Phrase %d step %d: waiting (%d rows left)\n", ap->phrase_index + 1, ap->current_step + 1, ap->delay_counter);
            continue;
        }

        // Execute current step
        printf("Phrase %d step %d: EXECUTING %s (delay was %d)\n", ap->phrase_index + 1, ap->current_step + 1, input_action_name(step->action), step->delay_rows);

        InputEvent evt;
        evt.action = step->action;
        evt.parameter = step->parameter;
        evt.value = step->value;
        // Set flag so execute_action knows this is from a phrase, not performance playback
        executing_phrase_action = 1;
        handle_input_event(&evt);
        executing_phrase_action = 0;

        // Move to next step
        ap->current_step++;
        if (ap->current_step < phrase->step_count) {
            ap->delay_counter = phrase->steps[ap->current_step].delay_rows;
            printf("Phrase %d: moved to step %d, delay=%d\n", ap->phrase_index + 1, ap->current_step + 1, ap->delay_counter);
        } else {
            // Phrase complete - reset playback position to order 0
            printf("Phrase %d: COMPLETE - resetting to order 0\n", ap->phrase_index + 1);
            if (common_state->player) {
                regroove_jump_to_order(common_state->player, 0);
            }
            ap->phrase_index = -1;
        }
    }
}

// --- CALLBACKS for UI feedback ---
static void my_order_callback(int order, int pattern, void *userdata) {
    printf("[ORDER] Now at Order %d (Pattern %d)\n", order, pattern);
}
static void my_row_callback(int order, int row, void *userdata) {
    //printf("[ROW] Order %d, Row %d\n", order, row);

    // Update performance timeline
    if (common_state && common_state->performance) {
        // Check for events to playback at current performance row BEFORE incrementing
        if (regroove_performance_is_playing(common_state->performance)) {
            PerformanceEvent events[16];  // Max events per row
            int event_count = regroove_performance_get_events(common_state->performance, events, 16);

            // Trigger all events at this performance row
            for (int i = 0; i < event_count; i++) {
                printf("Playback: Triggering %s (param=%d, value=%.0f) at PR:%d\n",
                       input_action_name(events[i].action), events[i].parameter,
                       events[i].value, regroove_performance_get_row(common_state->performance));

                InputEvent evt;
                evt.action = events[i].action;
                evt.parameter = events[i].parameter;
                evt.value = (int)events[i].value;

                // Trigger playback event (from_playback=1)
                if (common_state->performance) {
                    regroove_performance_handle_action(common_state->performance,
                                                        evt.action,
                                                        evt.parameter,
                                                        evt.value,
                                                        1);  // from_playback=1
                }
            }
        }

        // Now increment the performance row for the next callback
        regroove_performance_tick(common_state->performance);
    }

    // Update active phrases on every row
    update_phrases();
}
static void my_loop_callback(int order, int pattern, void *userdata) {
    printf("[LOOP] Pattern looped at Order %d (Pattern %d)\n", order, pattern);
    (void)userdata;
}

static void my_song_callback(void *userdata) {
    printf("[SONG] looped back to start\n");
}

static void my_note_callback(int channel, int note, int instrument, int volume,
                             int effect_cmd, int effect_param, void *userdata) {
    (void)userdata;
    if (!midi_output_enabled) return;

    // Check for note-off effect commands (0FFF or EC0)
    if (effect_cmd == 0x0F && effect_param == 0xFF) {
        // 0FFF = Note OFF in OctaMED
        midi_output_stop_channel(channel);
        return;
    }
    if (effect_cmd == 0x0E && effect_param == 0xC0) {
        // EC0 = Note cut
        midi_output_stop_channel(channel);
        return;
    }

    // Handle note events
    if (note == -2) {
        // Explicit note-off (=== or OFF in pattern)
        midi_output_stop_channel(channel);
    } else if (note >= 0) {
        // New note triggered
        // Use default volume if not specified
        int vel = (volume >= 0) ? volume : 64;
        midi_output_handle_note(channel, note, instrument, vel);
    }
}

// --- SDL audio callback ---
static void audio_callback(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    if (!common_state || !common_state->player) return;
    int16_t *buffer = (int16_t *)stream;
    int frames = len / (2 * sizeof(int16_t));
    regroove_render_audio(common_state->player, buffer, frames);
}

// --- Only one set of global callbacks ---
struct RegrooveCallbacks global_cbs;

// --- Centralized module loader ---
static int load_module(const char *path, struct RegrooveCallbacks *cbs) {
    if (regroove_common_load_module(common_state, path, cbs) != 0) {
        printf("Failed to load: %s\n", path);
        return -1;
    }

    print_song_order(common_state->player);

    // Debug: Check performance events after load
    if (common_state && common_state->performance) {
        int event_count = regroove_performance_get_event_count(common_state->performance);
        printf("DEBUG: After load, performance event_count = %d\n", event_count);
    }

    printf("\nPlayback paused (press SPACE or MIDI Play to start)\n");
    return 0;
}


// --- Performance Action Executor (Callback for performance engine) ---
static void execute_action(InputAction action, int parameter, float value, void* userdata) {
    (void)userdata;  // Not needed

    switch (action) {
        case ACTION_PLAY_PAUSE:
            if (common_state->paused) {
                // Starting playback - check for performance mode
                if (common_state && common_state->performance && !executing_phrase_action) {
                    int event_count = regroove_performance_get_event_count(common_state->performance);
                    printf("ACTION_PLAY_PAUSE (starting): event_count=%d, executing_phrase_action=%d\n",
                           event_count, executing_phrase_action);
                    if (event_count > 0) {
                        // Reset song position to order 0 when starting performance playback
                        if (common_state->player) {
                            regroove_jump_to_order(common_state->player, 0);
                        }
                        // Enable performance playback only if there are events
                        regroove_performance_set_playback(common_state->performance, 1);
                        printf("Performance playback ENABLED\n");
                    }
                }
            } else {
                // Stopping playback
                if (common_state && common_state->performance) {
                    regroove_performance_set_playback(common_state->performance, 0);
                    regroove_performance_reset(common_state->performance);
                }
            }
            regroove_common_play_pause(common_state, common_state->paused);
            printf("Playback %s\n", common_state->paused ? "paused" : "resumed");
            break;
        case ACTION_PLAY:
            if (common_state->paused) {
                // In performance mode, always start from the beginning
                // BUT: Don't enable performance playback if this is from a phrase
                if (common_state && common_state->performance && !executing_phrase_action) {
                    int event_count = regroove_performance_get_event_count(common_state->performance);
                    printf("ACTION_PLAY: event_count=%d, executing_phrase_action=%d\n",
                           event_count, executing_phrase_action);
                    if (event_count > 0) {
                        // Reset song position to order 0 when starting performance playback
                        if (common_state->player) {
                            regroove_jump_to_order(common_state->player, 0);
                        }
                        // Enable performance playback only if there are events
                        regroove_performance_set_playback(common_state->performance, 1);
                        printf("Performance playback ENABLED\n");
                    }
                }
                regroove_common_play_pause(common_state, 1);
                printf("Playback resumed\n");
            }
            break;
        case ACTION_STOP:
            if (!common_state->paused) {
                regroove_common_play_pause(common_state, 0);
                printf("Playback paused\n");
                // Notify performance system that playback stopped AND reset to beginning
                if (common_state && common_state->performance) {
                    regroove_performance_set_playback(common_state->performance, 0);
                    regroove_performance_reset(common_state->performance);
                }
            }
            break;
        case ACTION_RETRIGGER:
            regroove_common_retrigger(common_state);
            printf("Triggered retrigger.\n");
            break;
        case ACTION_NEXT_ORDER:
            regroove_common_next_order(common_state);
            if (common_state->player) {
                int next_order = regroove_get_current_order(common_state->player) + 1;
                if (next_order < regroove_get_num_orders(common_state->player))
                    printf("Next order queued: Order %d (Pattern %d)\n",
                        next_order, regroove_get_order_pattern(common_state->player, next_order));
            }
            break;
        case ACTION_PREV_ORDER:
            regroove_common_prev_order(common_state);
            if (common_state->player) {
                int prev_order = regroove_get_current_order(common_state->player) > 0 ?
                        regroove_get_current_order(common_state->player) - 1 : 0;
                printf("Previous order queued: Order %d (Pattern %d)\n",
                    prev_order, regroove_get_order_pattern(common_state->player, prev_order));
            }
            break;
        case ACTION_LOOP_TILL_ROW:
            regroove_common_loop_till_row(common_state);
            if (common_state->player) {
                printf("Loop till row queued: Order %d, Row %d\n",
                    regroove_get_current_order(common_state->player),
                    regroove_get_current_row(common_state->player));
            }
            break;
        case ACTION_HALVE_LOOP:
            regroove_common_halve_loop(common_state);
            if (common_state->player) {
                int rows = regroove_get_custom_loop_rows(common_state->player) > 0 ?
                    regroove_get_custom_loop_rows(common_state->player) :
                    regroove_get_full_pattern_rows(common_state->player);
                printf("Loop length halved: %d rows\n", rows);
            }
            break;
        case ACTION_FULL_LOOP:
            regroove_common_full_loop(common_state);
            if (common_state->player) {
                printf("Loop length reset to full pattern: %d rows\n",
                    regroove_get_full_pattern_rows(common_state->player));
            }
            break;
        case ACTION_PATTERN_MODE_TOGGLE:
            if (common_state->player) {
                // Get current mode BEFORE toggling
                int old_mode = regroove_get_pattern_mode(common_state->player);
                regroove_common_pattern_mode_toggle(common_state);
                // Print message based on what mode we're switching TO (opposite of old_mode)
                if (!old_mode) // Was in song mode (0), now switching to pattern mode (1)
                    printf("Pattern mode ON (looping pattern %d at order %d)\n",
                           regroove_get_current_pattern(common_state->player),
                           regroove_get_current_order(common_state->player));
                else // Was in pattern mode (1), now switching to song mode (0)
                    printf("Song mode ON\n");
            }
            break;
        case ACTION_MUTE_ALL:
            regroove_common_mute_all(common_state);
            printf("All channels muted\n");
            break;
        case ACTION_UNMUTE_ALL:
            regroove_common_unmute_all(common_state);
            printf("All channels unmuted\n");
            break;
        case ACTION_PITCH_UP:
            regroove_common_pitch_up(common_state);
            printf("Pitch factor: %.2f\n", common_state->pitch);
            break;
        case ACTION_PITCH_DOWN:
            regroove_common_pitch_down(common_state);
            printf("Pitch factor: %.2f\n", common_state->pitch);
            break;
        case ACTION_PITCH_SET:
            // Map MIDI value (0-127) to pitch range (0.25 to 3.0, center at 1.0)
            // MIDI 0 = 0.25, MIDI 64 = 1.0, MIDI 127 = 3.0
            if (common_state->player) {
                double pitch = 0.25 + (value / 127.0) * (3.0 - 0.25);
                regroove_common_set_pitch(common_state, pitch);
                printf("Pitch factor: %.2f\n", common_state->pitch);
            }
            break;
        case ACTION_QUIT:
            running = 0;
            break;
        case ACTION_FILE_PREV:
            if (common_state->file_list) {
                regroove_filelist_prev(common_state->file_list);
                printf("File: %s\n",
                    common_state->file_list->filenames[common_state->file_list->current_index]);
            }
            break;
        case ACTION_FILE_NEXT:
            if (common_state->file_list) {
                regroove_filelist_next(common_state->file_list);
                printf("File: %s\n",
                    common_state->file_list->filenames[common_state->file_list->current_index]);
            }
            break;
        case ACTION_FILE_LOAD:
            if (common_state->file_list) {
                char path[COMMON_MAX_PATH * 2];
                regroove_filelist_get_current_path(common_state->file_list, path, sizeof(path));
                load_module(path, &global_cbs);
            }
            break;
        case ACTION_CHANNEL_MUTE:
            if (parameter < common_state->num_channels) {
                regroove_common_channel_mute(common_state, parameter);
                if (common_state->player) {
                    int muted = regroove_is_channel_muted(common_state->player, parameter);
                    printf("Channel %d %s\n", parameter + 1, muted ? "muted" : "unmuted");
                }
            }
            break;
        case ACTION_CHANNEL_SOLO:
            if (common_state->player && parameter < common_state->num_channels) {
                regroove_toggle_channel_solo(common_state->player, parameter);
                printf("Channel %d solo toggled\n", parameter + 1);
            }
            break;
        case ACTION_CHANNEL_VOLUME:
            if (common_state->player && parameter < common_state->num_channels) {
                double vol = value / 127.0;
                regroove_set_channel_volume(common_state->player, parameter, vol);
            }
            break;
        case ACTION_TRIGGER_PAD:
            // Handle both application pads (0-15) and song pads (16-31)
            if (parameter >= 0 && parameter < MAX_TRIGGER_PADS) {
                // Application pad (A1-A16)
                if (common_state && common_state->input_mappings) {
                    TriggerPadConfig *pad = &common_state->input_mappings->trigger_pads[parameter];
                    // Execute the trigger pad's configured action
                    if (pad->action != ACTION_NONE) {
                        InputEvent pad_event;
                        pad_event.action = pad->action;
                        pad_event.parameter = pad->parameter;
                        pad_event.value = (int)value;
                        handle_input_event(&pad_event);
                    }
                }
            } else if (parameter >= MAX_TRIGGER_PADS && parameter < MAX_TRIGGER_PADS + MAX_SONG_TRIGGER_PADS) {
                // Song pad (S1-S16)
                int song_pad_idx = parameter - MAX_TRIGGER_PADS;
                if (common_state && common_state->metadata) {
                    TriggerPadConfig *pad = &common_state->metadata->song_trigger_pads[song_pad_idx];
                    // Execute the trigger pad's configured action
                    if (pad->action != ACTION_NONE) {
                        InputEvent pad_event;
                        pad_event.action = pad->action;
                        pad_event.parameter = pad->parameter;
                        pad_event.value = (int)value;
                        handle_input_event(&pad_event);
                    }
                }
            }
            break;
        case ACTION_JUMP_TO_ORDER:
            if (common_state->player && parameter >= 0) {
                int num_orders = regroove_get_num_orders(common_state->player);
                if (parameter < num_orders) {
                    regroove_jump_to_order(common_state->player, parameter);
                    int pat = regroove_get_order_pattern(common_state->player, parameter);
                    printf("Hot cue jump to Order %d (Pattern %d)\n", parameter, pat);
                }
            }
            break;
        case ACTION_JUMP_TO_PATTERN:
            if (common_state->player && parameter >= 0) {
                int num_patterns = regroove_get_num_patterns(common_state->player);
                if (parameter < num_patterns) {
                    regroove_jump_to_pattern(common_state->player, parameter);
                    printf("Jump to Pattern %d\n", parameter);
                }
            }
            break;
        case ACTION_QUEUE_ORDER:
            if (common_state->player && parameter >= 0) {
                int num_orders = regroove_get_num_orders(common_state->player);
                if (parameter < num_orders) {
                    regroove_queue_order(common_state->player, parameter);
                    int pat = regroove_get_order_pattern(common_state->player, parameter);
                    printf("Queued jump to Order %d (Pattern %d) at pattern end\n", parameter, pat);
                }
            }
            break;
        case ACTION_QUEUE_PATTERN:
            if (common_state->player && parameter >= 0) {
                int num_patterns = regroove_get_num_patterns(common_state->player);
                if (parameter < num_patterns) {
                    regroove_queue_pattern(common_state->player, parameter);
                    printf("Queued jump to Pattern %d at pattern end\n", parameter);
                }
            }
            break;
        default:
            break;
    }
}

// --- Unified Input Event Handler (Simplified - routes to performance engine) ---
static void handle_input_event(InputEvent *event) {
    if (!event || event->action == ACTION_NONE) return;

    // Handle phrase triggers directly (bypass performance engine)
    // Phrases are user-initiated only, not part of performance recording/playback
    if (event->action == ACTION_TRIGGER_PHRASE) {
        printf("handle_input_event: ACTION_TRIGGER_PHRASE, parameter=%d, executing_phrase_action=%d\n",
               event->parameter, executing_phrase_action);
        // Only execute phrase triggers from user input, not from playback
        if (!executing_phrase_action) {
            trigger_phrase(event->parameter);
        } else {
            printf("  SKIPPED: executing_phrase_action is true\n");
        }
        // Don't route to performance engine (no recording/playback)
        return;
    }

    // Route everything else through the performance engine
    // It will handle recording and execute via the callback we set up
    if (common_state && common_state->performance) {
        regroove_performance_handle_action(common_state->performance,
                                            event->action,
                                            event->parameter,
                                            event->value,
                                            0);  // from_playback=0 (user input)
    }
}

// --- MIDI HANDLING: uses unified control functions and InputMappings ---
void my_midi_mapping(unsigned char status, unsigned char cc_or_note, unsigned char value, int device_id, void *userdata) {
    (void)userdata;

    unsigned char msg_type = status & 0xF0;

    // Handle Note-On messages for trigger pads
    if (msg_type == 0x90 && value > 0) { // Note-On with velocity > 0
        int note = cc_or_note;
        int triggered = 0;

        // Check application trigger pads (A1-A16)
        if (common_state && common_state->input_mappings) {
            for (int i = 0; i < MAX_TRIGGER_PADS; i++) {
                TriggerPadConfig *pad = &common_state->input_mappings->trigger_pads[i];
                // Skip if disabled
                if (pad->midi_device == -2) continue;

                // Match device (if specified) and note
                if (pad->midi_note == note &&
                    (pad->midi_device == -1 || pad->midi_device == device_id)) {

                    // Execute the configured action
                    if (pad->action != ACTION_NONE) {
                        InputEvent event;
                        event.action = pad->action;
                        event.parameter = pad->parameter;
                        event.value = value;
                        handle_input_event(&event);
                    }
                    triggered = 1;
                    break; // Only trigger the first matching pad
                }
            }
        }

        // If not triggered by application pad, check song trigger pads (S1-S16)
        if (!triggered && common_state && common_state->metadata) {
            for (int i = 0; i < MAX_SONG_TRIGGER_PADS; i++) {
                TriggerPadConfig *pad = &common_state->metadata->song_trigger_pads[i];
                // Skip if disabled
                if (pad->midi_device == -2) continue;

                // Match device (if specified) and note
                if (pad->midi_note == note &&
                    (pad->midi_device == -1 || pad->midi_device == device_id)) {

                    // Execute the configured action
                    if (pad->action != ACTION_NONE) {
                        InputEvent event;
                        event.action = pad->action;
                        event.parameter = pad->parameter;
                        event.value = value;
                        handle_input_event(&event);
                    }
                    break; // Only trigger the first matching pad
                }
            }
        }
        return;
    }

    // Handle Control Change messages for input mappings
    if (msg_type == 0xB0) {
        // Query input mappings
        InputEvent event;
        if (common_state && common_state->input_mappings &&
            input_mappings_get_midi_event(common_state->input_mappings, device_id, cc_or_note, value, &event)) {
            handle_input_event(&event);
        }
    }
}

int is_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return 1;
    return 0;
}

int main(int argc, char *argv[]) {
    int midi_port = -1;
    const char *config_file = "regroove.ini";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc) {
            midi_port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            config_file = argv[++i];
        } else if (!strcmp(argv[i], "--dump-config")) {
            if (regroove_common_save_default_config("regroove_default.ini") == 0) {
                printf("Default configuration saved to regroove_default.ini\n");
                return 0;
            } else {
                fprintf(stderr, "Failed to save default configuration\n");
                return 1;
            }
        }
    }
    if (argc < 2) {
        fprintf(stderr, "Usage: %s file.mod|dir [-m mididevice] [-c config.ini] [--dump-config]\n", argv[0]);
        fprintf(stderr, "  --dump-config  Generate default config file and exit\n");
        return 1;
    }

    // Create common state
    common_state = regroove_common_create();
    if (!common_state) {
        fprintf(stderr, "Failed to create common state\n");
        return 1;
    }

    // Initialize phrase playback state
    for (int i = 0; i < MAX_ACTIVE_PHRASES; i++) {
        active_phrases[i].phrase_index = -1;
        active_phrases[i].current_step = 0;
        active_phrases[i].delay_counter = 0;
    }

    // Set up performance action callback (routes actions through the performance engine)
    if (common_state->performance) {
        regroove_performance_set_action_callback(common_state->performance, execute_action, NULL);
    }

    char *initial_path = argv[1];
    if (is_directory(initial_path)) {
        common_state->file_list = regroove_filelist_create();
        if (!common_state->file_list ||
            regroove_filelist_load(common_state->file_list, initial_path) <= 0) {
            printf("Could not load directory or no files found: %s\n", initial_path);
            regroove_common_destroy(common_state);
            return 1;
        }
        printf("Loaded %d files from directory: %s\n",
               common_state->file_list->count,
               common_state->file_list->directory);
        printf("Use CC61/CC62 or [ and ] to select, CC60 or ENTER to load\n");
    }

    // Print help first (before loading any module)
    printf("Controls:\n");
    printf("  SPACE start/stop playback\n");
    printf("  r retrigger current pattern\n");
    printf("  N/n next order, P/p previous order\n");
    printf("  j loop pattern till current row\n");
    printf("  h halve loop, f reset loop\n");
    printf("  S/s toggle pattern mode\n");
    printf("  1–9 mute channels, m mute all, u unmute all\n");
    printf("  +/- pitch\n");
    printf("  q/ESC quit\n");
    if (common_state->file_list) {
        printf("  [ and ] to select file, ENTER to load\n");
        printf("  (or CC61/CC62/CC60 via MIDI)\n");
    }
    printf("\n");

    // Try to load custom mappings from config file
    if (regroove_common_load_mappings(common_state, config_file) != 0) {
        printf("No %s found, using default mappings\n", config_file);
    } else {
        printf("Loaded input mappings from %s\n", config_file);
    }

    struct RegrooveCallbacks cbs = {
        .on_order_change = my_order_callback,
        .on_row_change = my_row_callback,
        .on_loop_pattern = my_loop_callback,
        .on_loop_song = my_song_callback,
        .on_note = my_note_callback,
        .userdata = NULL
    };
    global_cbs = cbs;

    if (!common_state->file_list) {
        if (load_module(initial_path, &global_cbs) != 0) {
            regroove_common_destroy(common_state);
            return 1;
        }
    }

    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.freq = 48000;
    spec.format = AUDIO_S16SYS;
    spec.channels = 2;
    spec.samples = 256;
    spec.callback = audio_callback;
    spec.userdata = NULL;

    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        regroove_common_destroy(common_state);
        return 1;
    }

    // Open audio device (use selected device or NULL for default)
    const char* device_name = NULL;
    int selected_audio_device = common_state->device_config.audio_device;
    if (selected_audio_device >= 0) {
        device_name = SDL_GetAudioDeviceName(selected_audio_device, 0);
    }
    audio_device_id = SDL_OpenAudioDevice(device_name, 0, &spec, NULL, 0);
    if (audio_device_id == 0) {
        fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        regroove_common_destroy(common_state);
        SDL_Quit();
        return 1;
    }
    // Store audio device ID in common state for use by common functions
    common_state->audio_device_id = audio_device_id;
    signal(SIGINT, handle_sigint);

    tty_make_raw_nonblocking();

    int midi_ports = midi_list_ports();
    if (midi_ports > 0) {
        // Use configured MIDI devices from INI, with command-line override for device 0
        int ports[MIDI_MAX_DEVICES];
        ports[0] = (midi_port >= 0) ? midi_port : common_state->device_config.midi_device_0;
        ports[1] = common_state->device_config.midi_device_1;

        // Count how many devices to open
        int num_devices = 0;
        if (ports[0] >= 0) num_devices = 1;
        if (ports[1] >= 0) num_devices = 2;

        if (num_devices > 0) {
            if (midi_init_multi(my_midi_mapping, NULL, ports, num_devices) != 0) {
                printf("No MIDI available. Running with keyboard control only.\n");
            }
        } else {
            printf("No MIDI devices configured. Running with keyboard control only.\n");
        }
    } else {
        printf("No MIDI available. Running with keyboard control only.\n");
    }

    // Initialize MIDI output if configured
    if (common_state->device_config.midi_output_device >= 0) {
        if (midi_output_init(common_state->device_config.midi_output_device) == 0) {
            midi_output_device = common_state->device_config.midi_output_device;
            midi_output_enabled = 1;
            printf("MIDI output enabled on device %d\n", midi_output_device);
        } else {
            fprintf(stderr, "Failed to initialize MIDI output on device %d\n",
                    common_state->device_config.midi_output_device);
        }
    }

    if (audio_device_id) SDL_PauseAudioDevice(audio_device_id, 1);

    while (running) {
        int k = read_key_nonblocking();
        if (k != -1) {
            // Query input mappings for keyboard event
            InputEvent event;
            if (common_state->input_mappings &&
                input_mappings_get_keyboard_event(common_state->input_mappings, k, &event)) {
                handle_input_event(&event);
            }
        }
        if (common_state->player) regroove_process_commands(common_state->player);
        SDL_Delay(10);
    }

    midi_deinit();

    // Safely stop audio and destroy module
    if (audio_device_id) {
        SDL_PauseAudioDevice(audio_device_id, 1);
        SDL_CloseAudioDevice(audio_device_id);
    }

    regroove_common_destroy(common_state);

    SDL_Quit();
    return 0;
}