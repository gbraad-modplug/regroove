#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl2.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <cmath>
#include <vector>
#include <sys/stat.h>
#include <ctime>

extern "C" {
#include "regroove_common.h"
#include "midi.h"
#include "midi_output.h"
#include "lcd.h"
#include "regroove_effects.h"
}

// -----------------------------------------------------------------------------
// Forward Declarations
// -----------------------------------------------------------------------------
static void handle_input_event(InputEvent *event, bool from_playback = false);
static void update_phrases(void);

// -----------------------------------------------------------------------------
// State & Helper Types
// -----------------------------------------------------------------------------
static const char* appname = "MP-1210: Direct Interaction Groove Interface";

struct Channel {
    float volume = 1.0f;
    bool mute = false;
    bool solo = false;
};

#define MAX_CHANNELS 64
static Channel channels[MAX_CHANNELS];
static float pitch_slider = 0.0f; // -1.0 to 1.0, 0 = 1.0x pitch
static float step_fade[16] = {0.0f};
static int current_step = 0;
static bool loop_enabled = false;
static bool playing = false;
static int pattern = 1, order = 1, total_rows = 64;
static float loop_blink = 0.0f;

// UI mode state
enum UIMode {
    UI_MODE_VOLUME = 0,
    UI_MODE_PADS = 1,
    UI_MODE_SONG = 2,
    UI_MODE_PERF = 3,
    UI_MODE_INFO = 4,
    UI_MODE_MIDI = 5,
    UI_MODE_TRACKER = 6,
    UI_MODE_EFFECTS = 7,
    UI_MODE_SETTINGS = 8
};
static UIMode ui_mode = UI_MODE_VOLUME;

// Visual feedback for trigger pads (fade effect) - supports both A and S pads
static float trigger_pad_fade[MAX_TOTAL_TRIGGER_PADS] = {0.0f};

// Channel note highlighting (for tracker view and volume faders)
static float channel_note_fade[MAX_CHANNELS] = {0.0f};

// Shared state
static RegrooveCommonState *common_state = NULL;
static const char *current_config_file = "regroove.ini"; // Track config file for saving

// Audio device state
static std::vector<std::string> audio_device_names;
static int selected_audio_device = -1;
static SDL_AudioDeviceID audio_device_id = 0;

// MIDI device cache (refreshed only when settings panel is shown or on refresh button)
static int cached_midi_port_count = -1;
static UIMode last_ui_mode = UI_MODE_VOLUME;

// LCD display (initialized in main)
static LCD* lcd_display = NULL;

// MIDI output state
static int midi_output_device = -1;  // -1 = disabled
static bool midi_output_enabled = false;

// Effects state
static RegrooveEffects* effects = NULL;

// MIDI monitor (circular buffer for recent MIDI messages)
#define MIDI_MONITOR_SIZE 50
struct MidiMonitorEntry {
    char timestamp[16];
    int device_id;
    char type[16];      // "Note On", "Note Off", "CC", etc.
    int number;         // Note number or CC number
    int value;          // Velocity or CC value
    bool is_output;     // true = OUT, false = IN
};
static MidiMonitorEntry midi_monitor[MIDI_MONITOR_SIZE];
static int midi_monitor_head = 0;
static int midi_monitor_count = 0;

void add_to_midi_monitor(int device_id, const char* type, int number, int value, bool is_output) {
    MidiMonitorEntry* entry = &midi_monitor[midi_monitor_head];

    // Get current time
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    strftime(entry->timestamp, sizeof(entry->timestamp), "%H:%M:%S", tm_info);

    entry->device_id = device_id;
    snprintf(entry->type, sizeof(entry->type), "%s", type);
    entry->number = number;
    entry->value = value;
    entry->is_output = is_output;

    midi_monitor_head = (midi_monitor_head + 1) % MIDI_MONITOR_SIZE;
    if (midi_monitor_count < MIDI_MONITOR_SIZE) {
        midi_monitor_count++;
    }
}

// Phrase playback state is now managed by the phrase engine via regroove_common
// No local state needed

void refresh_audio_devices() {
    audio_device_names.clear();
    int n = SDL_GetNumAudioDevices(0); // 0 = output devices
    for (int i = 0; i < n; i++) {
        audio_device_names.push_back(SDL_GetAudioDeviceName(i, 0));
    }
}

void refresh_midi_devices() {
    cached_midi_port_count = midi_list_ports();
}

// Learn mode state
static bool learn_mode_active = false;
enum LearnTarget {
    LEARN_NONE = 0,
    LEARN_ACTION,      // Regular button action (Play, Stop, etc.)
    LEARN_TRIGGER_PAD  // Trigger pad
};
static LearnTarget learn_target_type = LEARN_NONE;
static InputAction learn_target_action = ACTION_NONE;
static int learn_target_parameter = 0;
static int learn_target_pad_index = -1;

// Clamp helper
template<typename T>
static inline T Clamp(T v, T lo, T hi) { return (v < lo ? lo : (v > hi ? hi : v)); }

static float MapPitchFader(float slider_val) {
    // slider_val: -1.0 ... 0.0 ... +1.0
    // output:     0.05 ... 1.0 ... 2.0
    float pitch;
    if (slider_val < 0.0f) {
        pitch = 1.0f + slider_val * (1.0f - 0.05f); // [-1,0] maps to [0.05,1.0]
    } else {
        pitch = 1.0f + slider_val * (2.0f - 1.0f);  // [0,1] maps to [1.0,2.0]
    }
    return Clamp(pitch, 0.05f, 2.0f);
}

void update_channel_mute_states() {
    if (!common_state || !common_state->player) return;
    common_state->num_channels = regroove_get_num_channels(common_state->player);

    for (int i = 0; i < common_state->num_channels; ++i) {
        channels[i].mute = regroove_is_channel_muted(common_state->player, i);
    }
}

// -----------------------------------------------------------------------------
// Callbacks
// -----------------------------------------------------------------------------
static void my_row_callback(int ord, int row, void *userdata) {
    //printf("[ROW] Order %d, Row %d\n", ord, row);

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
                handle_input_event(&evt, true);  // from_playback=true
            }
        }

        // Now increment the performance row for the next callback
        regroove_performance_tick(common_state->performance);
    }

    // Update active phrases on every row
    update_phrases();

    if (total_rows <= 0) return;
    int rows_per_step = total_rows / 16;
    if (rows_per_step < 1) rows_per_step = 1;
    current_step = row / rows_per_step;
    if (current_step >= 16) current_step = 15;
    step_fade[current_step] = 1.0f;
}
static void my_order_callback(int ord, int pat, void *userdata) {
    //printf("[SONG] Now at Order %d (Pattern %d)\n", ord, pat);
    order = ord;
    pattern = pat;
    if (common_state && common_state->player)
        total_rows = regroove_get_full_pattern_rows(common_state->player);
}

static void my_loop_pattern_callback(int order, int pattern, void *userdata) {
    //printf("[LOOP] Loop/retrigger at Order %d (Pattern %d)\n", order, pattern);
    loop_blink = 1.0f;
}

static void my_loop_song_callback(void *userdata) {
    //printf("[SONG] looped back to start\n");
    playing = false;
}

static void my_note_callback(int channel, int note, int instrument, int volume,
                             int effect_cmd, int effect_param, void *userdata) {
    (void)userdata;

    // Trigger channel highlighting for visual feedback
    if (channel >= 0 && channel < MAX_CHANNELS && note >= 0) {
        channel_note_fade[channel] = 1.0f;
    }

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


// -----------------------------------------------------------------------------
// Module Loading
// -----------------------------------------------------------------------------

// LCD Display Configuration (similar to HD44780 initialization)
// Configured for UI panel width of 190px - 20 chars fits nicely
static constexpr int MAX_LCD_TEXTLENGTH = LCD_COLS;

// UI Color Constants
static const ImVec4 COLOR_SECTION_HEADING = ImVec4(1.0f, 0.8f, 0.3f, 1.0f);  // Orange/amber for section headings

static int load_module(const char *path) {
    struct RegrooveCallbacks cbs = {
        .on_order_change = my_order_callback,
        .on_row_change = my_row_callback,
        .on_loop_pattern = my_loop_pattern_callback,
        .on_loop_song = my_loop_song_callback,
        .on_note = my_note_callback,
        .userdata = NULL
    };

    if (regroove_common_load_module(common_state, path, &cbs) != 0) {
        return -1;
    }

    Regroove *mod = common_state->player;
    common_state->num_channels = regroove_get_num_channels(mod);

    for (int i = 0; i < 16; ++i) step_fade[i] = 0.0f;

    for (int i = 0; i < common_state->num_channels; i++) {
        channels[i].volume = 1.0f;
        channels[i].mute = false;
        channels[i].solo = false;
    }
    update_channel_mute_states();

    order = regroove_get_current_order(mod);
    pattern = regroove_get_current_pattern(mod);
    total_rows = regroove_get_full_pattern_rows(mod);

    loop_enabled = false;
    playing = false;
    pitch_slider = 0.0f;
    current_step = 0;

    regroove_set_custom_loop_rows(mod, 0); // 0 disables custom loop
    regroove_set_pitch(mod, MapPitchFader(0.0f)); // Reset pitch

    // Clear effects buffers and reset to default parameters
    if (effects) {
        regroove_effects_reset(effects);

        // Disable all effects
        regroove_effects_set_distortion_enabled(effects, 0);
        regroove_effects_set_filter_enabled(effects, 0);
        regroove_effects_set_eq_enabled(effects, 0);
        regroove_effects_set_compressor_enabled(effects, 0);
        regroove_effects_set_delay_enabled(effects, 0);

        // Reset all parameters to defaults from config
        regroove_effects_set_distortion_drive(effects, common_state->device_config.fx_distortion_drive);
        regroove_effects_set_distortion_mix(effects, common_state->device_config.fx_distortion_mix);
        regroove_effects_set_filter_cutoff(effects, common_state->device_config.fx_filter_cutoff);
        regroove_effects_set_filter_resonance(effects, common_state->device_config.fx_filter_resonance);
        regroove_effects_set_eq_low(effects, common_state->device_config.fx_eq_low);
        regroove_effects_set_eq_mid(effects, common_state->device_config.fx_eq_mid);
        regroove_effects_set_eq_high(effects, common_state->device_config.fx_eq_high);
        regroove_effects_set_compressor_threshold(effects, common_state->device_config.fx_compressor_threshold);
        regroove_effects_set_compressor_ratio(effects, common_state->device_config.fx_compressor_ratio);
        regroove_effects_set_compressor_attack(effects, common_state->device_config.fx_compressor_attack);
        regroove_effects_set_compressor_release(effects, common_state->device_config.fx_compressor_release);
        regroove_effects_set_compressor_makeup(effects, common_state->device_config.fx_compressor_makeup);
        regroove_effects_set_delay_time(effects, common_state->device_config.fx_delay_time);
        regroove_effects_set_delay_feedback(effects, common_state->device_config.fx_delay_feedback);
        regroove_effects_set_delay_mix(effects, common_state->device_config.fx_delay_mix);
    }

    if (audio_device_id) SDL_PauseAudioDevice(audio_device_id, 1);
    playing = false;
    for (int i = 0; i < 16; i++) step_fade[i] = 0.0f;

    // Auto-switch to PERF mode if performance events were loaded, otherwise VOL mode
    if (common_state && common_state->performance) {
        int event_count = regroove_performance_get_event_count(common_state->performance);
        if (event_count > 0) {
            ui_mode = UI_MODE_PERF;
            printf("Auto-switched to PERF mode (%d events loaded)\n", event_count);
        } else {
            ui_mode = UI_MODE_VOLUME;  // Reset to default mode when no performance
        }
    } else {
        ui_mode = UI_MODE_VOLUME;  // Reset to default mode
    }

    return 0;
}

// -----------------------------------------------------------------------------
// Unified Input Actions
// -----------------------------------------------------------------------------
enum GuiAction {
    ACT_PLAY,
    ACT_STOP,
    ACT_TOGGLE_LOOP,
    ACT_NEXT_ORDER,
    ACT_PREV_ORDER,
    ACT_RETRIGGER,
    ACT_SET_PITCH,
    ACT_PITCH_RESET,
    ACT_PITCH_UP,
    ACT_PITCH_DOWN,
    ACT_SET_LOOP_ROWS,
    ACT_LOOP_TILL_ROW,
    ACT_HALVE_LOOP,
    ACT_FULL_LOOP,
    ACT_MUTE_CHANNEL,
    ACT_SOLO_CHANNEL,
    ACT_VOLUME_CHANNEL,
    ACT_MUTE_ALL,
    ACT_UNMUTE_ALL,
    ACT_JUMP_TO_ORDER,
    ACT_JUMP_TO_PATTERN,
    ACT_QUEUE_ORDER,
    ACT_QUEUE_PATTERN
};

void dispatch_action(GuiAction act, int arg1 = -1, float arg2 = 0.0f, bool should_record = true) {
    // Record the action if requested (UI buttons pass true, handle_input_event passes false to avoid double-recording)
    if (should_record && common_state && common_state->performance) {
        if (regroove_performance_is_recording(common_state->performance)) {
            // Convert GuiAction to InputAction for recording
            InputAction input_action = ACTION_NONE;
            int parameter = arg1;
            int value = (int)(arg2 * 127.0f);

            switch (act) {
                case ACT_PLAY: input_action = ACTION_PLAY; break;
                case ACT_STOP: input_action = ACTION_STOP; break;
                case ACT_TOGGLE_LOOP: input_action = ACTION_PATTERN_MODE_TOGGLE; break;
                case ACT_NEXT_ORDER: input_action = ACTION_NEXT_ORDER; break;
                case ACT_PREV_ORDER: input_action = ACTION_PREV_ORDER; break;
                case ACT_RETRIGGER: input_action = ACTION_RETRIGGER; break;
                case ACT_LOOP_TILL_ROW: input_action = ACTION_LOOP_TILL_ROW; break;
                case ACT_HALVE_LOOP: input_action = ACTION_HALVE_LOOP; break;
                case ACT_FULL_LOOP: input_action = ACTION_FULL_LOOP; break;
                case ACT_MUTE_CHANNEL: input_action = ACTION_CHANNEL_MUTE; break;
                case ACT_SOLO_CHANNEL: input_action = ACTION_CHANNEL_SOLO; break;
                case ACT_VOLUME_CHANNEL: input_action = ACTION_CHANNEL_VOLUME; break;
                case ACT_MUTE_ALL: input_action = ACTION_MUTE_ALL; break;
                case ACT_UNMUTE_ALL: input_action = ACTION_UNMUTE_ALL; break;
                case ACT_PITCH_UP: input_action = ACTION_PITCH_UP; break;
                case ACT_PITCH_DOWN: input_action = ACTION_PITCH_DOWN; break;
                case ACT_PITCH_RESET: input_action = ACTION_PITCH_RESET; break;
                case ACT_JUMP_TO_ORDER: input_action = ACTION_JUMP_TO_ORDER; break;
                case ACT_JUMP_TO_PATTERN: input_action = ACTION_JUMP_TO_PATTERN; break;
                case ACT_QUEUE_ORDER: input_action = ACTION_QUEUE_ORDER; break;
                case ACT_QUEUE_PATTERN: input_action = ACTION_QUEUE_PATTERN; break;
                default: break;
            }

            if (input_action != ACTION_NONE) {
                regroove_performance_record_event(common_state->performance,
                                                  input_action,
                                                  parameter,
                                                  value);
            }
        }
    }

    Regroove *mod = common_state ? common_state->player : NULL;

    switch (act) {
        case ACT_PLAY:
            if (mod) {
                // In performance mode, always start from the beginning
                // BUT: Don't enable performance playback if this is from a phrase
                if (common_state && common_state->performance && !regroove_common_phrase_is_active(common_state)) {
                    int event_count = regroove_performance_get_event_count(common_state->performance);
                    if (event_count > 0) {
                        // Reset song position to order 0 when starting performance playback
                        regroove_jump_to_order(mod, 0);
                        // Enable performance playback only if there are events
                        regroove_performance_set_playback(common_state->performance, 1);
                    }
                }
                if (audio_device_id) SDL_PauseAudioDevice(audio_device_id, 0);
                playing = true;
            }
            break;
        case ACT_STOP:
            if (mod) {
                if (audio_device_id) SDL_PauseAudioDevice(audio_device_id, 1);
                playing = false;
                // Notify performance system that playback stopped AND reset to beginning
                if (common_state && common_state->performance) {
                    regroove_performance_set_playback(common_state->performance, 0);
                    regroove_performance_reset(common_state->performance);
                }
            }
            break;
        case ACT_TOGGLE_LOOP:
            if (mod) {
                loop_enabled = !loop_enabled;
                regroove_pattern_mode(mod, loop_enabled ? 1 : 0);
            }
            break;
        case ACT_NEXT_ORDER:
            if (mod) regroove_queue_next_order(mod);
            break;
        case ACT_PREV_ORDER:
            if (mod) regroove_queue_prev_order(mod);
            break;
        case ACT_RETRIGGER:
            if (mod) {
                //SDL_PauseAudio(1);  // TODO: retrigger causes a double free
                regroove_retrigger_pattern(mod);
                //SDL_PauseAudio(0);
                update_channel_mute_states();
            }
            break;
        case ACT_SET_PITCH: {
            if (mod) {
                float mapped_pitch = MapPitchFader(arg2);
                regroove_set_pitch(mod, mapped_pitch);
                pitch_slider = arg2;
            }
            break;
        }
        case ACT_PITCH_RESET:
            pitch_slider = 0.0f;
            dispatch_action(ACT_SET_PITCH, -1, 0.0f, false);  // Don't record SET_PITCH, only PITCH_RESET
            break;
        case ACT_PITCH_UP:
            if (mod) {
                // Increment pitch slider by small amount (0.01 = ~1% of range)
                pitch_slider += 0.01f;
                if (pitch_slider > 1.0f) pitch_slider = 1.0f;
                float mapped_pitch = MapPitchFader(pitch_slider);
                regroove_set_pitch(mod, mapped_pitch);
            }
            break;
        case ACT_PITCH_DOWN:
            if (mod) {
                // Decrement pitch slider by small amount
                pitch_slider -= 0.01f;
                if (pitch_slider < -1.0f) pitch_slider = -1.0f;
                float mapped_pitch = MapPitchFader(pitch_slider);
                regroove_set_pitch(mod, mapped_pitch);
            }
            break;
        case ACT_SET_LOOP_ROWS:
            if (mod && total_rows > 0) {
                int step_index = arg1;
                if (step_index == 15) {
                    regroove_set_custom_loop_rows(mod, 0);
                } else {
                    int rows_per_step = total_rows / 16;
                    if (rows_per_step < 1) rows_per_step = 1;
                    int loop_rows = (step_index + 1) * rows_per_step;
                    regroove_set_custom_loop_rows(mod, loop_rows);
                }
            }
            break;
        case ACT_LOOP_TILL_ROW:
            if (mod) {
                int current_row = regroove_get_current_row(mod);
                regroove_loop_till_row(mod, current_row);
            }
            break;
        case ACT_HALVE_LOOP:
            if (mod && total_rows > 0) {
                int rows = regroove_get_custom_loop_rows(mod) > 0 ?
                    regroove_get_custom_loop_rows(mod) :
                    total_rows;
                int halved = rows / 2 < 1 ? 1 : rows / 2;
                regroove_set_custom_loop_rows(mod, halved);
            }
            break;
        case ACT_FULL_LOOP:
            if (mod) {
                regroove_set_custom_loop_rows(mod, 0);
            }
            break;
        case ACT_SOLO_CHANNEL: {
            if (mod && arg1 >= 0 && arg1 < common_state->num_channels) {
                bool wasSolo = channels[arg1].solo;

                // Clear all solo states
                for (int i = 0; i < common_state->num_channels; ++i) channels[i].solo = false;

                if (!wasSolo) {
                    // New solo: set this channel solo, mute all, unmute this one
                    channels[arg1].solo = true;
                    regroove_mute_all(mod);
                    for (int i = 0; i < common_state->num_channels; ++i) channels[i].mute = true;
                    // Unmute soloed channel
                    regroove_toggle_channel_mute(mod, arg1);
                    channels[arg1].mute = false;
                } else {
                    // Un-solo: unmute all
                    regroove_unmute_all(mod);
                    for (int i = 0; i < common_state->num_channels; ++i) channels[i].mute = false;
                }
            }
            break;
        }
        case ACT_MUTE_CHANNEL: {
            if (mod && arg1 >= 0 && arg1 < common_state->num_channels) {
                // If soloed, un-solo and mute all
                if (channels[arg1].solo) {
                    channels[arg1].solo = false;
                    regroove_mute_all(mod);
                    for (int i = 0; i < common_state->num_channels; ++i) channels[i].mute = true;
                } else {
                    // Toggle mute just for this channel, remove solo
                    channels[arg1].mute = !channels[arg1].mute;
                    regroove_toggle_channel_mute(mod, arg1);
                    for (int i = 0; i < common_state->num_channels; ++i) channels[i].solo = false;
                }
            }
            break;
        }
        case ACT_VOLUME_CHANNEL:
            if (mod && arg1 >= 0 && arg1 < common_state->num_channels) {
                regroove_set_channel_volume(mod, arg1, (double)arg2);
                channels[arg1].volume = arg2;
            }
            break;
        case ACT_MUTE_ALL:
            if (mod) {
                regroove_mute_all(mod);
                for (int i = 0; i < common_state->num_channels; ++i) {
                    channels[i].mute = true;
                    channels[i].solo = false;
                }
            }
            break;
        case ACT_UNMUTE_ALL:
            if (mod) {
                regroove_unmute_all(mod);
                for (int i = 0; i < common_state->num_channels; ++i) {
                    channels[i].mute = false;
                    channels[i].solo = false;
                }
            }
            break;
        case ACT_JUMP_TO_ORDER:
            if (mod && arg1 >= 0) {
                regroove_jump_to_order(mod, arg1);
            }
            break;
        case ACT_JUMP_TO_PATTERN:
            if (mod && arg1 >= 0) {
                regroove_jump_to_pattern(mod, arg1);
            }
            break;
        case ACT_QUEUE_ORDER:
            if (mod && arg1 >= 0) {
                regroove_queue_order(mod, arg1);
            }
            break;
        case ACT_QUEUE_PATTERN:
            if (mod && arg1 >= 0) {
                regroove_queue_pattern(mod, arg1);
            }
            break;
    }
}

// -----------------------------------------------------------------------------
// Phrase Playback System
// -----------------------------------------------------------------------------
static void trigger_phrase(int phrase_index) {
    // Clear effect buffers to prevent clicks/pops from previous state
    if (effects) {
        regroove_effects_reset(effects);
    }

    // Use common library function
    regroove_common_trigger_phrase(common_state, phrase_index);

    // Sync GUI playing state with common_state->paused
    if (!common_state->paused) {
        playing = true;
    }
}

static void update_phrases() {
    // Use common library function
    regroove_common_update_phrases(common_state);
}

// -----------------------------------------------------------------------------
// Performance Action Executor (Callback for performance engine)
// -----------------------------------------------------------------------------

// Forward declaration
static void execute_action(InputAction action, int parameter, float value, void* userdata);

// Wrapper for phrase callback (converts int value to float)
static void phrase_action_callback(InputAction action, int parameter, int value, void* userdata) {
    execute_action(action, parameter, (float)value, userdata);
}

// Phrase reset callback - resets GUI state before phrase starts
static void phrase_reset_callback(void* userdata) {
    (void)userdata;

    // Reset GUI channel visual state to clean slate
    for (int i = 0; i < MAX_CHANNELS; ++i) {
        channels[i].mute = false;
        channels[i].solo = false;
        channels[i].volume = 1.0f;
    }
}

// Phrase completion callback - handles cleanup when phrase finishes
static void phrase_completion_callback(int phrase_index, void* userdata) {
    (void)phrase_index;
    (void)userdata;

    Regroove* mod = common_state ? common_state->player : NULL;
    if (!mod) return;

    // Stop playback
    playing = false;
    if (common_state->audio_device_id) {
        SDL_PauseAudioDevice(common_state->audio_device_id, 1);
    }
    common_state->paused = 1;

    // Reset to order 0
    regroove_jump_to_order(mod, 0);

    // Reset all channels - both engine and GUI state
    regroove_unmute_all(mod);
    phrase_reset_callback(NULL);  // Reuse the reset logic
}

// This function is called by the performance engine to execute actions
// It maps InputAction -> GuiAction -> dispatch_action(should_record=false)
static void execute_action(InputAction action, int parameter, float value, void* userdata) {
    (void)userdata;  // Not needed

    switch (action) {
        case ACTION_PLAY_PAUSE:
            dispatch_action(playing ? ACT_STOP : ACT_PLAY, -1, 0.0f, false);
            break;
        case ACTION_PLAY:
            dispatch_action(ACT_PLAY, -1, 0.0f, false);
            break;
        case ACTION_STOP:
            dispatch_action(ACT_STOP, -1, 0.0f, false);
            break;
        case ACTION_RETRIGGER:
            dispatch_action(ACT_RETRIGGER, -1, 0.0f, false);
            break;
        case ACTION_NEXT_ORDER:
            dispatch_action(ACT_NEXT_ORDER, -1, 0.0f, false);
            break;
        case ACTION_PREV_ORDER:
            dispatch_action(ACT_PREV_ORDER, -1, 0.0f, false);
            break;
        case ACTION_LOOP_TILL_ROW:
            dispatch_action(ACT_LOOP_TILL_ROW, -1, 0.0f, false);
            break;
        case ACTION_HALVE_LOOP:
            dispatch_action(ACT_HALVE_LOOP, -1, 0.0f, false);
            break;
        case ACTION_FULL_LOOP:
            dispatch_action(ACT_FULL_LOOP, -1, 0.0f, false);
            break;
        case ACTION_PATTERN_MODE_TOGGLE:
            dispatch_action(ACT_TOGGLE_LOOP, -1, 0.0f, false);
            break;
        case ACTION_MUTE_ALL:
            dispatch_action(ACT_MUTE_ALL, -1, 0.0f, false);
            break;
        case ACTION_UNMUTE_ALL:
            dispatch_action(ACT_UNMUTE_ALL, -1, 0.0f, false);
            break;
        case ACTION_PITCH_UP:
            dispatch_action(ACT_PITCH_UP, -1, 0.0f, false);
            break;
        case ACTION_PITCH_DOWN:
            dispatch_action(ACT_PITCH_DOWN, -1, 0.0f, false);
            break;
        case ACTION_PITCH_SET:
            // Map MIDI value (0-127) to pitch slider range (-1.0 to 1.0)
            {
                float pitch_value = (value / 127.0f) * 2.0f - 1.0f; // Maps 0-127 to -1.0 to 1.0
                dispatch_action(ACT_SET_PITCH, -1, pitch_value, false);
            }
            break;
        case ACTION_PITCH_RESET:
            dispatch_action(ACT_PITCH_RESET, -1, 0.0f, false);
            break;
        case ACTION_QUIT:
            {
                SDL_Event quit;
                quit.type = SDL_QUIT;
                SDL_PushEvent(&quit);
            }
            break;
        case ACTION_FILE_PREV:
            if (common_state && common_state->file_list) {
                regroove_filelist_prev(common_state->file_list);
            }
            break;
        case ACTION_FILE_NEXT:
            if (common_state && common_state->file_list) {
                regroove_filelist_next(common_state->file_list);
            }
            break;
        case ACTION_FILE_LOAD:
            if (common_state && common_state->file_list) {
                char path[COMMON_MAX_PATH * 2];
                regroove_filelist_get_current_path(common_state->file_list, path, sizeof(path));
                load_module(path);
            }
            break;
        case ACTION_CHANNEL_MUTE:
            dispatch_action(ACT_MUTE_CHANNEL, parameter, 0.0f, false);
            break;
        case ACTION_CHANNEL_SOLO:
            dispatch_action(ACT_SOLO_CHANNEL, parameter, 0.0f, false);
            break;
        case ACTION_CHANNEL_VOLUME:
            dispatch_action(ACT_VOLUME_CHANNEL, parameter, value / 127.0f, false);
            break;
        case ACTION_TRIGGER_PAD:
            // Handle both application pads (0-15) and song pads (16-31)
            if (parameter >= 0 && parameter < MAX_TRIGGER_PADS) {
                // Application pad (A1-A16)
                if (common_state && common_state->input_mappings) {
                    TriggerPadConfig *pad = &common_state->input_mappings->trigger_pads[parameter];
                    // Trigger visual feedback
                    trigger_pad_fade[parameter] = 1.0f;
                    // Execute the trigger pad's configured action
                    if (pad->action != ACTION_NONE) {
                        InputEvent pad_event;
                        pad_event.action = pad->action;
                        pad_event.parameter = pad->parameter;
                        pad_event.value = (int)value;
                        handle_input_event(&pad_event, false);  // from_playback=false
                    }
                }
            } else if (parameter >= MAX_TRIGGER_PADS && parameter < MAX_TRIGGER_PADS + MAX_SONG_TRIGGER_PADS) {
                // Song pad (S1-S16)
                int song_pad_idx = parameter - MAX_TRIGGER_PADS;
                if (common_state && common_state->metadata) {
                    TriggerPadConfig *pad = &common_state->metadata->song_trigger_pads[song_pad_idx];
                    // Trigger visual feedback (offset for song pads)
                    trigger_pad_fade[parameter] = 1.0f;
                    // Execute the trigger pad's configured action
                    if (pad->action != ACTION_NONE) {
                        InputEvent pad_event;
                        pad_event.action = pad->action;
                        pad_event.parameter = pad->parameter;
                        pad_event.value = (int)value;
                        handle_input_event(&pad_event, false);  // from_playback=false
                    }
                }
            }
            break;
        case ACTION_JUMP_TO_ORDER:
            dispatch_action(ACT_JUMP_TO_ORDER, parameter, 0.0f, false);
            break;
        case ACTION_JUMP_TO_PATTERN:
            dispatch_action(ACT_JUMP_TO_PATTERN, parameter, 0.0f, false);
            break;
        case ACTION_QUEUE_ORDER:
            dispatch_action(ACT_QUEUE_ORDER, parameter, 0.0f, false);
            break;
        case ACTION_QUEUE_PATTERN:
            dispatch_action(ACT_QUEUE_PATTERN, parameter, 0.0f, false);
            break;
        case ACTION_RECORD_TOGGLE:
            // Toggle recording state
            if (common_state && common_state->performance) {
                static bool recording = false;
                recording = !recording;
                regroove_performance_set_recording(common_state->performance, recording);
                if (recording) {
                    if (playing) {
                        dispatch_action(ACT_STOP, -1, 0.0f, false);
                    }
                    printf("Performance recording started\n");
                } else {
                    printf("Performance recording stopped\n");
                }
            }
            break;
        case ACTION_SET_LOOP_STEP:
            dispatch_action(ACT_SET_LOOP_ROWS, parameter, 0.0f, false);
            break;
        case ACTION_TRIGGER_PHRASE:
            // Phrases should not be triggered from performance playback
            // They are user-initiated triggers only, to prevent infinite loops
            // during playback (phrase triggers itself from recording)
            printf("Ignoring trigger_phrase during performance playback (param=%d)\n", parameter);
            break;
        case ACTION_FX_DISTORTION_DRIVE:
            if (effects) {
                // Map MIDI value (0-127) to normalized 0.0-1.0
                regroove_effects_set_distortion_drive(effects, value / 127.0f);
            }
            break;
        case ACTION_FX_DISTORTION_MIX:
            if (effects) {
                regroove_effects_set_distortion_mix(effects, value / 127.0f);
            }
            break;
        case ACTION_FX_FILTER_CUTOFF:
            if (effects) {
                regroove_effects_set_filter_cutoff(effects, value / 127.0f);
            }
            break;
        case ACTION_FX_FILTER_RESONANCE:
            if (effects) {
                regroove_effects_set_filter_resonance(effects, value / 127.0f);
            }
            break;
        case ACTION_FX_EQ_LOW:
            if (effects) {
                regroove_effects_set_eq_low(effects, value / 127.0f);
            }
            break;
        case ACTION_FX_EQ_MID:
            if (effects) {
                regroove_effects_set_eq_mid(effects, value / 127.0f);
            }
            break;
        case ACTION_FX_EQ_HIGH:
            if (effects) {
                regroove_effects_set_eq_high(effects, value / 127.0f);
            }
            break;
        case ACTION_FX_COMPRESSOR_THRESHOLD:
            if (effects) {
                regroove_effects_set_compressor_threshold(effects, value / 127.0f);
            }
            break;
        case ACTION_FX_COMPRESSOR_RATIO:
            if (effects) {
                regroove_effects_set_compressor_ratio(effects, value / 127.0f);
            }
            break;
        case ACTION_FX_DELAY_TIME:
            if (effects) {
                regroove_effects_set_delay_time(effects, value / 127.0f);
            }
            break;
        case ACTION_FX_DELAY_FEEDBACK:
            if (effects) {
                regroove_effects_set_delay_feedback(effects, value / 127.0f);
            }
            break;
        case ACTION_FX_DELAY_MIX:
            if (effects) {
                regroove_effects_set_delay_mix(effects, value / 127.0f);
            }
            break;
        case ACTION_FX_DISTORTION_TOGGLE:
            if (effects) {
                int enabled = regroove_effects_get_distortion_enabled(effects);
                regroove_effects_set_distortion_enabled(effects, !enabled);
            }
            break;
        case ACTION_FX_FILTER_TOGGLE:
            if (effects) {
                int enabled = regroove_effects_get_filter_enabled(effects);
                regroove_effects_set_filter_enabled(effects, !enabled);
            }
            break;
        case ACTION_FX_EQ_TOGGLE:
            if (effects) {
                int enabled = regroove_effects_get_eq_enabled(effects);
                regroove_effects_set_eq_enabled(effects, !enabled);
            }
            break;
        case ACTION_FX_COMPRESSOR_TOGGLE:
            if (effects) {
                int enabled = regroove_effects_get_compressor_enabled(effects);
                regroove_effects_set_compressor_enabled(effects, !enabled);
            }
            break;
        case ACTION_FX_DELAY_TOGGLE:
            if (effects) {
                int enabled = regroove_effects_get_delay_enabled(effects);
                regroove_effects_set_delay_enabled(effects, !enabled);
            }
            break;
        default:
            break;
    }
}

// -----------------------------------------------------------------------------
// Input Event Handler (Simplified - just routes to performance engine)
// -----------------------------------------------------------------------------
static void handle_input_event(InputEvent *event, bool from_playback) {
    if (!event || event->action == ACTION_NONE) return;

    // Handle phrase triggers directly (bypass performance engine)
    // Phrases are user-initiated only, not part of performance recording/playback
    if (event->action == ACTION_TRIGGER_PHRASE) {
        if (!from_playback) {
            // User-initiated phrase trigger - execute it
            trigger_phrase(event->parameter);
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
                                            from_playback ? 1 : 0);
    }
}

// Save current mappings to config file
static void save_mappings_to_config() {
    if (!common_state || !common_state->input_mappings) return;

    // Save the current input mappings (includes trigger pads)
    if (input_mappings_save(common_state->input_mappings, current_config_file) == 0) {
        // Also save device configuration
        if (regroove_common_save_device_config(common_state, current_config_file) == 0) {
            printf("Saved mappings and devices to %s\n", current_config_file);
        } else {
            fprintf(stderr, "Failed to save device config to %s\n", current_config_file);
        }
    } else {
        fprintf(stderr, "Failed to save mappings to %s\n", current_config_file);
    }
}

// Save current .rgx metadata
static void save_rgx_metadata() {
    if (!common_state || !common_state->metadata) return;
    if (common_state->current_module_path[0] == '\0') return;

    // Get .rgx path from module path
    char rgx_path[COMMON_MAX_PATH];
    regroove_metadata_get_rgx_path(common_state->current_module_path, rgx_path, sizeof(rgx_path));

    // Save metadata
    if (regroove_metadata_save(common_state->metadata, rgx_path) == 0) {
        printf("Saved metadata to %s\n", rgx_path);
    } else {
        fprintf(stderr, "Failed to save metadata to %s\n", rgx_path);
    }
}

// Learn keyboard mapping for current target
static void learn_keyboard_mapping(int key) {
    if (!common_state || !common_state->input_mappings) return;
    if (learn_target_type == LEARN_NONE) return;

    // Check if this key is already mapped to the current target
    bool already_mapped = false;
    InputAction target_action = (learn_target_type == LEARN_TRIGGER_PAD) ? ACTION_TRIGGER_PAD : learn_target_action;
    int target_param = (learn_target_type == LEARN_TRIGGER_PAD) ? learn_target_pad_index : learn_target_parameter;

    for (int i = 0; i < common_state->input_mappings->keyboard_count; i++) {
        KeyboardMapping *k = &common_state->input_mappings->keyboard_mappings[i];
        if (k->key == key && k->action == target_action && k->parameter == target_param) {
            // Already mapped to this target - unlearn it
            for (int j = i; j < common_state->input_mappings->keyboard_count - 1; j++) {
                common_state->input_mappings->keyboard_mappings[j] =
                    common_state->input_mappings->keyboard_mappings[j + 1];
            }
            common_state->input_mappings->keyboard_count--;
            printf("Unlearned keyboard mapping: key=%d from %s (param=%d)\n",
                   key, input_action_name(target_action), target_param);
            already_mapped = true;
            save_mappings_to_config();
            break;
        }
    }

    if (!already_mapped && common_state->input_mappings->keyboard_count < common_state->input_mappings->keyboard_capacity) {
        // Check if this key is mapped to something else, remove that mapping
        for (int i = 0; i < common_state->input_mappings->keyboard_count; i++) {
            if (common_state->input_mappings->keyboard_mappings[i].key == key) {
                // Remove this mapping by shifting others down
                for (int j = i; j < common_state->input_mappings->keyboard_count - 1; j++) {
                    common_state->input_mappings->keyboard_mappings[j] =
                        common_state->input_mappings->keyboard_mappings[j + 1];
                }
                common_state->input_mappings->keyboard_count--;
                break;
            }
        }

        // Add the new mapping
        KeyboardMapping new_mapping;
        new_mapping.key = key;

        if (learn_target_type == LEARN_TRIGGER_PAD) {
            new_mapping.action = ACTION_TRIGGER_PAD;
            new_mapping.parameter = learn_target_pad_index;
        } else {
            new_mapping.action = learn_target_action;
            new_mapping.parameter = learn_target_parameter;
        }

        common_state->input_mappings->keyboard_mappings[common_state->input_mappings->keyboard_count++] = new_mapping;
        printf("Learned keyboard mapping: key=%d -> %s (param=%d)\n",
               key, input_action_name(new_mapping.action), new_mapping.parameter);

        // Save to config file
        save_mappings_to_config();
    }

    // Exit learn mode
    learn_mode_active = false;
    learn_target_type = LEARN_NONE;
}

// Helper function to unlearn (remove all mappings for current target)
static void unlearn_current_target() {
    if (!common_state || !common_state->input_mappings) return;
    if (learn_target_type == LEARN_NONE) return;

    int removed_count = 0;
    bool song_pad_changed = false;

    if (learn_target_type == LEARN_TRIGGER_PAD) {
        // Remove MIDI note mapping from trigger pad (application or song pad)
        if (learn_target_pad_index >= 0 && learn_target_pad_index < MAX_TRIGGER_PADS) {
            // Application pad
            if (common_state && common_state->input_mappings) {
                TriggerPadConfig *pad = &common_state->input_mappings->trigger_pads[learn_target_pad_index];
                if (pad->midi_note != -1) {
                    pad->midi_note = -1;
                    pad->midi_device = -1;
                    printf("Unlearned MIDI note mapping for Application Pad A%d\n", learn_target_pad_index + 1);
                    removed_count++;
                }
            }
        } else if (learn_target_pad_index >= MAX_TRIGGER_PADS &&
                   learn_target_pad_index < MAX_TRIGGER_PADS + MAX_SONG_TRIGGER_PADS) {
            // Song pad
            int song_pad_idx = learn_target_pad_index - MAX_TRIGGER_PADS;
            if (common_state && common_state->metadata) {
                TriggerPadConfig *pad = &common_state->metadata->song_trigger_pads[song_pad_idx];
                if (pad->midi_note != -1) {
                    pad->midi_note = -1;
                    pad->midi_device = -1;
                    printf("Unlearned MIDI note mapping for Song Pad S%d\n", song_pad_idx + 1);
                    song_pad_changed = true;
                }
            }
        }

        // Remove keyboard mappings for this trigger pad
        for (int i = 0; i < common_state->input_mappings->keyboard_count; i++) {
            KeyboardMapping *k = &common_state->input_mappings->keyboard_mappings[i];
            if (k->action == ACTION_TRIGGER_PAD && k->parameter == learn_target_pad_index) {
                // Remove this mapping by shifting others down
                for (int j = i; j < common_state->input_mappings->keyboard_count - 1; j++) {
                    common_state->input_mappings->keyboard_mappings[j] =
                        common_state->input_mappings->keyboard_mappings[j + 1];
                }
                common_state->input_mappings->keyboard_count--;
                printf("Unlearned keyboard mapping for Pad %d\n", learn_target_pad_index + 1);
                removed_count++;
                i--; // Check this index again since we shifted
            }
        }

        // Remove MIDI CC mappings for this trigger pad
        for (int i = 0; i < common_state->input_mappings->midi_count; i++) {
            MidiMapping *m = &common_state->input_mappings->midi_mappings[i];
            if (m->action == ACTION_TRIGGER_PAD && m->parameter == learn_target_pad_index) {
                // Remove this mapping
                for (int j = i; j < common_state->input_mappings->midi_count - 1; j++) {
                    common_state->input_mappings->midi_mappings[j] =
                        common_state->input_mappings->midi_mappings[j + 1];
                }
                common_state->input_mappings->midi_count--;
                printf("Unlearned MIDI CC mapping for Pad %d\n", learn_target_pad_index + 1);
                removed_count++;
                i--; // Check this index again since we shifted
            }
        }
    } else if (learn_target_type == LEARN_ACTION) {
        // Remove keyboard mappings for this action
        for (int i = 0; i < common_state->input_mappings->keyboard_count; i++) {
            KeyboardMapping *k = &common_state->input_mappings->keyboard_mappings[i];
            if (k->action == learn_target_action && k->parameter == learn_target_parameter) {
                // Remove this mapping by shifting others down
                for (int j = i; j < common_state->input_mappings->keyboard_count - 1; j++) {
                    common_state->input_mappings->keyboard_mappings[j] =
                        common_state->input_mappings->keyboard_mappings[j + 1];
                }
                common_state->input_mappings->keyboard_count--;
                printf("Unlearned keyboard mapping for %s (param=%d)\n",
                       input_action_name(learn_target_action), learn_target_parameter);
                removed_count++;
                i--; // Check this index again since we shifted
            }
        }

        // Remove MIDI mappings for this action
        for (int i = 0; i < common_state->input_mappings->midi_count; i++) {
            MidiMapping *m = &common_state->input_mappings->midi_mappings[i];
            if (m->action == learn_target_action && m->parameter == learn_target_parameter) {
                // Remove this mapping
                for (int j = i; j < common_state->input_mappings->midi_count - 1; j++) {
                    common_state->input_mappings->midi_mappings[j] =
                        common_state->input_mappings->midi_mappings[j + 1];
                }
                common_state->input_mappings->midi_count--;
                printf("Unlearned MIDI mapping for %s (param=%d)\n",
                       input_action_name(learn_target_action), learn_target_parameter);
                removed_count++;
                i--; // Check this index again since we shifted
            }
        }
    }

    if (removed_count > 0) {
        // Save the updated application pad mappings to config
        save_mappings_to_config();
        printf("Removed %d mapping(s)\n", removed_count);
    } else if (song_pad_changed) {
        // Save song pad changes to .rgx file
        regroove_common_save_rgx(common_state);
        printf("Removed song pad mapping\n");
    } else {
        printf("No mappings to remove\n");
    }

    // Exit learn mode
    learn_mode_active = false;
    learn_target_type = LEARN_NONE;
}

// Helper functions to start learn mode for different targets
static void start_learn_for_action(InputAction action, int parameter = 0) {
    learn_mode_active = true;
    learn_target_type = LEARN_ACTION;
    learn_target_action = action;
    learn_target_parameter = parameter;
    learn_target_pad_index = -1;
    printf("Learn mode: Waiting for input for action %s (param=%d)... (Click LEARN again to unlearn)\n",
           input_action_name(action), parameter);
}

static void start_learn_for_pad(int pad_index) {
    if (pad_index < 0 || pad_index >= MAX_TRIGGER_PADS) return;
    learn_mode_active = true;
    learn_target_type = LEARN_TRIGGER_PAD;
    learn_target_action = ACTION_NONE;
    learn_target_parameter = 0;
    learn_target_pad_index = pad_index;
    printf("Learn mode: Waiting for input for Application Pad A%d... (Click LEARN again to unlearn)\n", pad_index + 1);
}

// Start learn mode for song trigger pad
static void start_learn_for_song_pad(int pad_index) {
    if (pad_index < 0 || pad_index >= MAX_SONG_TRIGGER_PADS) return;
    learn_mode_active = true;
    learn_target_type = LEARN_TRIGGER_PAD;
    learn_target_action = ACTION_NONE;
    learn_target_parameter = 0;
    // Use offset to distinguish song pads from application pads
    learn_target_pad_index = MAX_TRIGGER_PADS + pad_index;
    printf("Learn mode: Waiting for input for Song Pad S%d... (Click LEARN again to unlearn)\n", pad_index + 1);
}

// Learn MIDI mapping for current target
static void learn_midi_mapping(int device_id, int cc_or_note, bool is_note) {
    if (!common_state || !common_state->input_mappings) return;
    if (learn_target_type == LEARN_NONE) return;

    if (is_note && learn_target_type == LEARN_TRIGGER_PAD) {
        // Map MIDI note to trigger pad (application or song pad)
        if (learn_target_pad_index >= 0 && learn_target_pad_index < MAX_TRIGGER_PADS) {
            // Application pad (A1-A16)
            if (common_state && common_state->input_mappings) {
                TriggerPadConfig *pad = &common_state->input_mappings->trigger_pads[learn_target_pad_index];
                pad->midi_note = cc_or_note;
                pad->midi_device = device_id;
                printf("Learned MIDI note mapping: Note %d (device %d) -> Application Pad A%d\n",
                       cc_or_note, device_id, learn_target_pad_index + 1);
                // Save to config file
                save_mappings_to_config();
            }
        } else if (learn_target_pad_index >= MAX_TRIGGER_PADS &&
                   learn_target_pad_index < MAX_TRIGGER_PADS + MAX_SONG_TRIGGER_PADS) {
            // Song pad (S1-S16)
            int song_pad_idx = learn_target_pad_index - MAX_TRIGGER_PADS;
            if (common_state && common_state->metadata) {
                TriggerPadConfig *pad = &common_state->metadata->song_trigger_pads[song_pad_idx];
                pad->midi_note = cc_or_note;
                pad->midi_device = device_id;
                printf("Learned MIDI note mapping: Note %d (device %d) -> Song Pad S%d\n",
                       cc_or_note, device_id, song_pad_idx + 1);
                // Save to .rgx file
                regroove_common_save_rgx(common_state);
            }
        }
    } else if (!is_note) {
        // Map MIDI CC to action

        // Check if this CC is already mapped to the current target
        bool already_mapped = false;
        InputAction target_action = (learn_target_type == LEARN_TRIGGER_PAD) ? ACTION_TRIGGER_PAD : learn_target_action;
        int target_param = (learn_target_type == LEARN_TRIGGER_PAD) ? learn_target_pad_index : learn_target_parameter;

        for (int i = 0; i < common_state->input_mappings->midi_count; i++) {
            MidiMapping *m = &common_state->input_mappings->midi_mappings[i];
            if (m->cc_number == cc_or_note &&
                (m->device_id == device_id || m->device_id == -1 || device_id == -1) &&
                m->action == target_action && m->parameter == target_param) {
                // Already mapped to this target - unlearn it
                for (int j = i; j < common_state->input_mappings->midi_count - 1; j++) {
                    common_state->input_mappings->midi_mappings[j] =
                        common_state->input_mappings->midi_mappings[j + 1];
                }
                common_state->input_mappings->midi_count--;
                printf("Unlearned MIDI CC mapping: CC %d (device %d) from %s (param=%d)\n",
                       cc_or_note, device_id, input_action_name(target_action), target_param);
                already_mapped = true;
                save_mappings_to_config();
                break;
            }
        }

        if (!already_mapped && common_state->input_mappings->midi_count < common_state->input_mappings->midi_capacity) {
            // Check if this CC is mapped to something else, remove it
            for (int i = 0; i < common_state->input_mappings->midi_count; i++) {
                MidiMapping *m = &common_state->input_mappings->midi_mappings[i];
                if (m->cc_number == cc_or_note &&
                    (m->device_id == device_id || m->device_id == -1 || device_id == -1)) {
                    // Remove this mapping
                    for (int j = i; j < common_state->input_mappings->midi_count - 1; j++) {
                        common_state->input_mappings->midi_mappings[j] =
                            common_state->input_mappings->midi_mappings[j + 1];
                    }
                    common_state->input_mappings->midi_count--;
                    break;
                }
            }

            // Add the new mapping
            MidiMapping new_mapping;
            new_mapping.device_id = device_id;
            new_mapping.cc_number = cc_or_note;

            // Set continuous mode for volume, pitch, and effects controls
            if (learn_target_type == LEARN_ACTION &&
                (learn_target_action == ACTION_CHANNEL_VOLUME ||
                 learn_target_action == ACTION_PITCH_SET ||
                 learn_target_action == ACTION_FX_DISTORTION_DRIVE ||
                 learn_target_action == ACTION_FX_DISTORTION_MIX ||
                 learn_target_action == ACTION_FX_FILTER_CUTOFF ||
                 learn_target_action == ACTION_FX_FILTER_RESONANCE ||
                 learn_target_action == ACTION_FX_EQ_LOW ||
                 learn_target_action == ACTION_FX_EQ_MID ||
                 learn_target_action == ACTION_FX_EQ_HIGH ||
                 learn_target_action == ACTION_FX_COMPRESSOR_THRESHOLD ||
                 learn_target_action == ACTION_FX_COMPRESSOR_RATIO ||
                 learn_target_action == ACTION_FX_DELAY_TIME ||
                 learn_target_action == ACTION_FX_DELAY_FEEDBACK ||
                 learn_target_action == ACTION_FX_DELAY_MIX)) {
                new_mapping.threshold = 0;
                new_mapping.continuous = 1; // Continuous fader mode
            } else {
                new_mapping.threshold = 64; // Button-style threshold
                new_mapping.continuous = 0; // Button mode
            }

            if (learn_target_type == LEARN_TRIGGER_PAD) {
                new_mapping.action = ACTION_TRIGGER_PAD;
                new_mapping.parameter = learn_target_pad_index;
            } else {
                new_mapping.action = learn_target_action;
                new_mapping.parameter = learn_target_parameter;
            }

            common_state->input_mappings->midi_mappings[common_state->input_mappings->midi_count++] = new_mapping;
            printf("Learned MIDI CC mapping: CC %d (device %d) -> %s (param=%d)\n",
                   cc_or_note, device_id, input_action_name(new_mapping.action), new_mapping.parameter);

            // Save to config file
            save_mappings_to_config();
        }
    }

    // Exit learn mode
    learn_mode_active = false;
    learn_target_type = LEARN_NONE;
}

void handle_keyboard(SDL_Event &e, SDL_Window *window) {
    if (e.type != SDL_KEYDOWN) return;

    // Don't process keyboard shortcuts if user is typing in a text field
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureKeyboard) {
        return;
    }

    // Handle special GUI-only keys first
    if (e.key.keysym.sym == SDLK_F11) {
        if (window) {
            Uint32 flags = SDL_GetWindowFlags(window);
            if (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) {
                SDL_SetWindowFullscreen(window, 0);
            } else {
                SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
            }
        }
        return;
    }

    // Convert SDL key to character for input mappings
    int key = e.key.keysym.sym;

    if (key >= SDLK_a && key <= SDLK_z) {
        // Convert to lowercase character
        key = 'a' + (key - SDLK_a);
    } else if (key >= SDLK_0 && key <= SDLK_9) {
        key = '0' + (key - SDLK_0);
    } else if (key >= SDLK_KP_1 && key <= SDLK_KP_9) {
        // Map numpad 1-9 to unique codes (160-168)
        key = 160 + (key - SDLK_KP_1);
    } else if (key == SDLK_KP_0) {
        // Map numpad 0 to unique code 159
        key = 159;
    } else {
        // Map special keys
        switch (key) {
            case SDLK_SPACE: key = ' '; break;
            case SDLK_ESCAPE: key = 27; break;
            case SDLK_RETURN: key = '\n'; break;
            case SDLK_KP_ENTER: key = '\n'; break;
            case SDLK_LEFTBRACKET: key = '['; break;
            case SDLK_RIGHTBRACKET: key = ']'; break;
            case SDLK_MINUS: key = '-'; break;
            case SDLK_KP_MINUS: key = '-'; break;
            case SDLK_EQUALS: key = '='; break;
            case SDLK_PLUS: key = '+'; break;
            case SDLK_KP_PLUS: key = '+'; break;
            default: return; // Unsupported key
        }
    }

    // If in learn mode, learn the mapping instead of executing
    if (learn_mode_active) {
        learn_keyboard_mapping(key);
        return;
    }

    // Query input mappings
    if (common_state && common_state->input_mappings) {
        InputEvent event;
        if (input_mappings_get_keyboard_event(common_state->input_mappings, key, &event)) {
            handle_input_event(&event);
        }
    }
}

void my_midi_mapping(unsigned char status, unsigned char cc_or_note, unsigned char value, int device_id, void *userdata) {
    (void)userdata;

    unsigned char msg_type = status & 0xF0;

    // Log to MIDI monitor
    if (msg_type == 0x90) {  // Note On
        add_to_midi_monitor(device_id, value > 0 ? "Note On" : "Note Off", cc_or_note, value, false);
    } else if (msg_type == 0x80) {  // Note Off
        add_to_midi_monitor(device_id, "Note Off", cc_or_note, value, false);
    } else if (msg_type == 0xB0) {  // Control Change
        add_to_midi_monitor(device_id, "CC", cc_or_note, value, false);
    }

    // If in learn mode, capture the MIDI input
    if (learn_mode_active) {
        // Only learn on note-on or CC with value > 0
        if ((msg_type == 0x90 && value > 0) || (msg_type == 0xB0 && value >= 64)) {
            bool is_note = (msg_type == 0x90);
            learn_midi_mapping(device_id, cc_or_note, is_note);
        }
        return;
    }

    // Handle Note-On messages for trigger pads
    if (msg_type == 0x90 && value > 0) { // Note-On with velocity > 0
        int note = cc_or_note;
        bool triggered = false;

        // Check application trigger pads (A1-A16)
        if (common_state && common_state->input_mappings) {
            for (int i = 0; i < MAX_TRIGGER_PADS; i++) {
                TriggerPadConfig *pad = &common_state->input_mappings->trigger_pads[i];
                // Skip if disabled
                if (pad->midi_device == -2) continue;

                // Match device (if specified) and note
                if (pad->midi_note == note &&
                    (pad->midi_device == -1 || pad->midi_device == device_id)) {

                    // Trigger visual feedback
                    trigger_pad_fade[i] = 1.0f;

                    // Execute the configured action
                    if (pad->action != ACTION_NONE) {
                        InputEvent event;
                        event.action = pad->action;
                        event.parameter = pad->parameter;
                        event.value = value;
                        handle_input_event(&event, false);
                    }
                    triggered = true;
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

                    // Trigger visual feedback (offset for song pads)
                    int global_idx = MAX_TRIGGER_PADS + i;
                    trigger_pad_fade[global_idx] = 1.0f;

                    // Execute the configured action
                    if (pad->action != ACTION_NONE) {
                        InputEvent event;
                        event.action = pad->action;
                        event.parameter = pad->parameter;
                        event.value = value;
                        handle_input_event(&event, false);
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
        if (common_state && common_state->input_mappings) {
            InputEvent event;
            if (input_mappings_get_midi_event(common_state->input_mappings, device_id, cc_or_note, value, &event)) {
                handle_input_event(&event);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Audio Callback
// -----------------------------------------------------------------------------
static void audio_callback(void *userdata, Uint8 *stream, int len) {
    if (!common_state || !common_state->player) {
        memset(stream, 0, len);
        return;
    }
    int16_t *buffer = (int16_t *)stream;
    int frames = len / (2 * sizeof(int16_t));
    regroove_render_audio(common_state->player, buffer, frames);

    // Apply effects if available
    if (effects) {
        regroove_effects_process(effects, buffer, frames, 48000);
    }
}

// -----------------------------------------------------------------------------
// Main UI
// -----------------------------------------------------------------------------

static void ApplyFlatBlackRedSkin()
{
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding   = 0.0f;
    s.ChildRounding    = 0.0f;
    s.FrameRounding    = 3.0f;
    s.GrabRounding     = 3.0f;
    s.ScrollbarRounding= 3.0f;
    s.WindowPadding    = ImVec2(6,6);
    s.FramePadding     = ImVec2(5,3);
    s.ItemSpacing      = ImVec2(8,6);
    s.ItemInnerSpacing = ImVec2(6,4);
    s.ChildBorderSize  = 1.0f;
    s.WindowBorderSize = 0.0f;
    s.FrameBorderSize  = 0.0f;

    ImVec4* c = s.Colors;
    ImVec4 black = ImVec4(0,0,0,1);
    ImVec4 dark2 = ImVec4(0.12f,0.12f,0.12f,1.0f);

    c[ImGuiCol_WindowBg]        = black;
    c[ImGuiCol_ChildBg]         = black;
    c[ImGuiCol_PopupBg]         = ImVec4(0.07f,0.07f,0.07f,1.0f);
    c[ImGuiCol_Border]          = ImVec4(0.15f,0.15f,0.15f,0.3f);
    c[ImGuiCol_BorderShadow]    = ImVec4(0,0,0,0);
    c[ImGuiCol_FrameBg]         = dark2;
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.18f,0.18f,0.18f,1.0f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.24f,0.24f,0.24f,1.0f);

    ImVec4 red       = ImVec4(0.90f,0.15f,0.18f,1.0f);
    ImVec4 redHover  = ImVec4(0.98f,0.26f,0.30f,1.0f);

    c[ImGuiCol_Button]          = dark2;
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.23f,0.23f,0.23f,1.0f);
    c[ImGuiCol_ButtonActive]    = ImVec4(0.16f,0.16f,0.16f,1.0f);

    c[ImGuiCol_SliderGrab]      = red;
    c[ImGuiCol_SliderGrabActive]= redHover;

    c[ImGuiCol_Text]            = ImVec4(0.88f,0.89f,0.90f,1.0f);
    c[ImGuiCol_TextDisabled]    = ImVec4(0.45f,0.46f,0.48f,1.0f);
}

static void DrawLCD(const char* text, float width, float height)
{
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 end(pos.x + width, pos.y + height);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, end, IM_COL32(25,50,18,255), 6.0f);
    dl->AddRect(pos, end, IM_COL32(95,140,65,255), 6.0f, 0, 2.0f);
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 10, pos.y + 16));
    ImGui::TextColored(ImVec4(0.80f,1.0f,0.70f,1.0f), "%s", text);
    ImGui::SetCursorScreenPos(ImVec2(pos.x, end.y + 8));
}

static void ShowMainUI() {
    ImGuiIO& io = ImGui::GetIO();
    ImGuiStyle& style = ImGui::GetStyle();

    // Fade the sequencer steps
    for (int i = 0; i < 16; i++)
        step_fade[i] = fmaxf(step_fade[i] - 0.02f, 0.0f);

    // Fade the channel note highlights
    for (int i = 0; i < MAX_CHANNELS; i++) {
        channel_note_fade[i] = fmaxf(channel_note_fade[i] - 0.05f, 0.0f);
    }

    ImGui::SetNextWindowPos(ImVec2(0,0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
    ImGuiWindowFlags rootFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin(appname, nullptr, rootFlags);

    // Layout constants
    const float BUTTON_SIZE = 48.0f;
    const float SIDE_MARGIN = 10.0f;
    const float TOP_MARGIN = 8.0f;
    const float LEFT_PANEL_WIDTH = 190.0f;
    const float LCD_HEIGHT = 90.0f;
    const float TRANSPORT_GAP = 10.0f;
    const float SEQUENCER_HEIGHT = 70.0f;
    const float GAP_ABOVE_SEQUENCER = 8.0f;
    const float BOTTOM_MARGIN = 6.0f;
    const float SOLO_SIZE = 34.0f;
    const float MUTE_SIZE = 34.0f;
    const float BASE_SLIDER_W = 44.0f;
    const float BASE_SPACING = 26.0f;
    const float MIN_SLIDER_HEIGHT = 140.0f;
    const float STEP_GAP = 6.0f;
    const float STEP_MIN = 28.0f;
    const float STEP_MAX = 60.0f;
    const float IMGUI_LAYOUT_COMPENSATION = SEQUENCER_HEIGHT / 2;

    float fullW = io.DisplaySize.x;
    float fullH = io.DisplaySize.y;

    float childPaddingY = style.WindowPadding.y * 2.0f;
    float childBorderY = style.ChildBorderSize * 2.0f;
    float channelAreaHeight = fullH - TOP_MARGIN - GAP_ABOVE_SEQUENCER - SEQUENCER_HEIGHT - BOTTOM_MARGIN - childPaddingY - childBorderY;
    if (channelAreaHeight < 280.0f) channelAreaHeight = 280.0f;

    // LEFT PANEL
    ImGui::SetCursorPos(ImVec2(SIDE_MARGIN, TOP_MARGIN));
    ImGui::BeginChild("left_panel", ImVec2(LEFT_PANEL_WIDTH, channelAreaHeight),
                      true, ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    {
        if (lcd_display) {
            char lcd_text[256];

            // Include truncated file name
            const char* file_disp = "";
            if (common_state && common_state->file_list && common_state->file_list->count > 0) {
                static char truncated[MAX_LCD_TEXTLENGTH + 1];
                const char* current_file = common_state->file_list->filenames[common_state->file_list->current_index];
                std::strncpy(truncated, current_file, MAX_LCD_TEXTLENGTH);
                truncated[MAX_LCD_TEXTLENGTH] = 0;
                file_disp = truncated;
            }

            // Get BPM from engine
            char bpm_str[16] = "---";
            if (common_state && common_state->player) {
                double bpm = regroove_get_current_bpm(common_state->player);
                std::snprintf(bpm_str, sizeof(bpm_str), "%.0f", bpm);
            }

            // Get pattern description from metadata
            // Always query the actual current pattern from the engine to avoid stale data
            const char* pattern_desc = "";
            if (common_state && common_state->metadata && common_state->player) {
                int current_pattern = regroove_get_current_pattern(common_state->player);
                const char* desc = regroove_metadata_get_pattern_desc(common_state->metadata, current_pattern);

                if (desc && desc[0] != '\0') {
                    pattern_desc = desc;
                }
            }

            // Determine playback mode display
            const char* mode_str = "----";
            // Only show mode if a module is loaded
            if (common_state && common_state->player) {
                mode_str = "SONG";
                // Check if phrase is active (highest priority)
                if (common_state->phrase && regroove_phrase_is_active(common_state->phrase)) {
                    mode_str = "PHRS";
                }
                // Show PERF whenever performance events are loaded (regardless of playback state)
                else if (common_state->performance) {
                    int event_count = regroove_performance_get_event_count(common_state->performance);
                    if (event_count > 0) {
                        mode_str = "PERF";
                    } else if (loop_enabled) {
                        mode_str = "LOOP";
                    }
                } else if (loop_enabled) {
                    mode_str = "LOOP";
                }
            }

            std::snprintf(lcd_text, sizeof(lcd_text),
                "SO:%02d PT:%02d MD:%s\nPitch:%.2f BPM:%s\n%.*s\n%.*s",
                order, pattern, mode_str,
                MapPitchFader(pitch_slider), bpm_str,
                MAX_LCD_TEXTLENGTH, file_disp,
                MAX_LCD_TEXTLENGTH, pattern_desc);

            // Write to LCD display
            lcd_write(lcd_display, lcd_text);

            // Draw LCD
            DrawLCD(lcd_get_buffer(lcd_display), LEFT_PANEL_WIDTH - 16.0f, LCD_HEIGHT);
        }
    }

    ImGui::Dummy(ImVec2(0, 8.0f));

    // File browser buttons
    ImGui::BeginGroup();
    if (ImGui::Button("<", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
        if (learn_mode_active) {
            start_learn_for_action(ACTION_FILE_PREV);
        } else if (common_state && common_state->file_list) {
            regroove_filelist_prev(common_state->file_list);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("o", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
        if (learn_mode_active) {
            start_learn_for_action(ACTION_FILE_LOAD);
        } else if (common_state && common_state->file_list) {
            char path[COMMON_MAX_PATH * 2];
            regroove_filelist_get_current_path(common_state->file_list, path, sizeof(path));
            load_module(path);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(">", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
        if (learn_mode_active) {
            start_learn_for_action(ACTION_FILE_NEXT);
        } else if (common_state && common_state->file_list) {
            regroove_filelist_next(common_state->file_list);
        }
    }
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, 8.0f));

    ImGui::BeginGroup();
    // STOP BUTTON
    if (!playing) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.25f, 0.20f, 1.0f)); // red
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.35f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.50f, 0.15f, 0.15f, 1.0f));
        if (ImGui::Button("[]", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
            if (learn_mode_active) start_learn_for_action(ACTION_STOP);
            else dispatch_action(ACT_STOP);
        }
        ImGui::PopStyleColor(3);
    } else {
        if (ImGui::Button("[]", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
            if (learn_mode_active) start_learn_for_action(ACTION_STOP);
            else dispatch_action(ACT_STOP);
        }
    }

    ImGui::SameLine();

    // PLAY BUTTON
    if (playing) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.65f, 0.25f, 1.0f)); // green
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.80f, 0.35f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.50f, 0.20f, 1.0f));
        if (ImGui::Button("|>", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
            if (learn_mode_active) start_learn_for_action(ACTION_RETRIGGER);
            else dispatch_action(ACT_RETRIGGER);
        }
        ImGui::PopStyleColor(3);
    } else {
        if (ImGui::Button("|>", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
            if (learn_mode_active) start_learn_for_action(ACTION_PLAY);
            else dispatch_action(ACT_PLAY);
        }
    }

    ImGui::SameLine();

    // Performance recording button
    static bool recording = false;
    ImVec4 recCol = recording ? ImVec4(0.90f, 0.16f, 0.18f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, recCol);
    if (ImGui::Button("O", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
        if (learn_mode_active) {
            start_learn_for_action(ACTION_RECORD_TOGGLE);
        } else if (common_state && common_state->performance) {
            recording = !recording;
            regroove_performance_set_recording(common_state->performance, recording);
            if (recording) {
                // When starting recording, stop playback to avoid re-recording played events
                regroove_performance_set_playback(common_state->performance, 0);
                printf("Performance recording started (playback stopped)\n");
            } else {
                printf("Performance recording stopped (%d events recorded)\n",
                       regroove_performance_get_event_count(common_state->performance));
                // Save to .rgx file when recording stops
                if (regroove_performance_get_event_count(common_state->performance) > 0) {
                    regroove_common_save_rgx(common_state);
                }
            }
        }
    }
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, TRANSPORT_GAP));

    if (ImGui::Button("<<", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
        if (learn_mode_active) start_learn_for_action(ACTION_PREV_ORDER);
        else dispatch_action(ACT_PREV_ORDER);
    }
    ImGui::SameLine();
    if (ImGui::Button(">>", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
        if (learn_mode_active) start_learn_for_action(ACTION_NEXT_ORDER);
        else dispatch_action(ACT_NEXT_ORDER);
    }
    ImGui::SameLine();

    // Fade the blink effect each frame
    loop_blink = fmaxf(loop_blink - 0.05f, 0.0f);

    // LOOP BUTTON
    ImVec4 baseCol = loop_enabled ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
    ImVec4 blinkCol = ImVec4(
        baseCol.x + loop_blink * 0.6f, // brighten R
        baseCol.y + loop_blink * 0.4f, // brighten G
        baseCol.z,                     // keep B
        1.0f
    );

    if (loop_enabled) {
        ImGui::PushStyleColor(ImGuiCol_Button, blinkCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, blinkCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, blinkCol);
        if (ImGui::Button("O*", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
            if (learn_mode_active) start_learn_for_action(ACTION_PATTERN_MODE_TOGGLE);
            else dispatch_action(ACT_TOGGLE_LOOP);
        }
        ImGui::PopStyleColor(3);
    } else {
        if (ImGui::Button("O∞", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
            if (learn_mode_active) start_learn_for_action(ACTION_PATTERN_MODE_TOGGLE);
            else dispatch_action(ACT_TOGGLE_LOOP);
        }
    }

    ImGui::EndGroup();

    ImGui::Dummy(ImVec2(0, TRANSPORT_GAP));

    ImGui::BeginGroup();
    // VOL button with active state highlighting
    ImVec4 volCol = (ui_mode == UI_MODE_VOLUME) ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, volCol);
    if (ImGui::Button("VOL", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
        ui_mode = UI_MODE_VOLUME;
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();

    // EFFECTS button with active state highlighting
    ImVec4 fxCol = (ui_mode == UI_MODE_EFFECTS) ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, fxCol);
    if (ImGui::Button("FX", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
        ui_mode = UI_MODE_EFFECTS;
    }
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 8.0f));

    // SONG button with active state highlighting
    ImVec4 songCol = (ui_mode == UI_MODE_SONG) ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, songCol);
    if (ImGui::Button("SONG", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
        ui_mode = UI_MODE_SONG;
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();

    // PADS button with active state highlighting
    ImVec4 padsCol = (ui_mode == UI_MODE_PADS) ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, padsCol);
    if (ImGui::Button("PADS", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
        ui_mode = UI_MODE_PADS;
    }
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 8.0f));

    // TRACKER button with active state highlighting
    ImVec4 trackCol = (ui_mode == UI_MODE_TRACKER) ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, trackCol);
    if (ImGui::Button("TRACK", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
        ui_mode = UI_MODE_TRACKER;
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();

    // INFO button with active state highlighting
    ImVec4 infoCol = (ui_mode == UI_MODE_INFO) ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, infoCol);
    if (ImGui::Button("INFO", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
        ui_mode = UI_MODE_INFO;
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();

    // PERF button with active state highlighting
    ImVec4 perfCol = (ui_mode == UI_MODE_PERF) ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, perfCol);
    if (ImGui::Button("PERF", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
        ui_mode = UI_MODE_PERF;
    }
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 8.0f));

    // Input learning mode button
    ImVec4 learnCol = learn_mode_active ? ImVec4(0.90f, 0.16f, 0.18f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, learnCol);
    if (ImGui::Button("LEARN", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
        if (learn_mode_active && learn_target_type != LEARN_NONE) {
            // If we're waiting for input, unlearn the current target
            unlearn_current_target();
        } else {
            // Toggle learn mode on/off
            learn_mode_active = !learn_mode_active;
            if (!learn_mode_active) {
                // Cancel learn mode
                learn_target_type = LEARN_NONE;
                learn_target_action = ACTION_NONE;
                learn_target_parameter = 0;
                learn_target_pad_index = -1;
            }
        }
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();

    // MIDI button with active state highlighting
    ImVec4 midiCol = (ui_mode == UI_MODE_MIDI) ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, midiCol);
    if (ImGui::Button("MIDI", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
        ui_mode = UI_MODE_MIDI;
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();

    // SETUP button with active state highlighting
    ImVec4 setupCol = (ui_mode == UI_MODE_SETTINGS) ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, setupCol);
    if (ImGui::Button("SETUP", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
        ui_mode = UI_MODE_SETTINGS;
    }
    ImGui::PopStyleColor();

    ImGui::EndGroup();

    ImGui::EndChild();

    // CHANNEL PANEL (9 columns: 8 channels + 1 pitch)
    float rightX = SIDE_MARGIN + LEFT_PANEL_WIDTH + 18.0f;
    float rightW = fullW - rightX - SIDE_MARGIN;
    if (rightW < 300.0f) rightW = 300.0f;

    float baseTotal = BASE_SLIDER_W * 9.0f + BASE_SPACING * 8.0f;
    float widthScale = rightW / baseTotal;
    if (widthScale > 1.40f) widthScale = 1.40f;
    float sliderW = BASE_SLIDER_W * widthScale;
    float spacing = BASE_SPACING * widthScale;

    ImGui::SetCursorPos(ImVec2(rightX, TOP_MARGIN));
    ImGui::BeginChild("channels_panel", ImVec2(rightW, channelAreaHeight), true, ImGuiWindowFlags_NoScrollbar);

    float labelH = ImGui::GetTextLineHeight();
    float contentHeight = channelAreaHeight - childPaddingY;
    float sliderTop = 8.0f + labelH + 4.0f + SOLO_SIZE + 6.0f;
    float bottomStack = 8.0f + MUTE_SIZE + 12.0f;
    float sliderH = contentHeight - sliderTop - bottomStack - IMGUI_LAYOUT_COMPENSATION;
    if (sliderH < MIN_SLIDER_HEIGHT) sliderH = MIN_SLIDER_HEIGHT;

    ImVec2 origin = ImGui::GetCursorPos();

    // Detect UI mode change to refresh devices only when needed
    if ((ui_mode == UI_MODE_SETTINGS || ui_mode == UI_MODE_MIDI) &&
        (last_ui_mode != UI_MODE_SETTINGS && last_ui_mode != UI_MODE_MIDI)) {
        refresh_midi_devices();
        if (audio_device_names.empty()) {
            refresh_audio_devices();
        }
    }
    last_ui_mode = ui_mode;

    // Conditional rendering based on UI mode
    if (ui_mode == UI_MODE_VOLUME) {
        // VOLUME MODE: Show channel sliders

        // Channel columns (only draw if module is loaded)
        int num_channels = (common_state && common_state->player) ? common_state->num_channels : 0;

        for (int i = 0; i < num_channels; ++i) {
            float colX = origin.x + i * (sliderW + spacing);
            ImGui::SetCursorPos(ImVec2(colX, origin.y + 8.0f));
            ImGui::BeginGroup();
            ImGui::Text("Ch%d", i + 1);
            ImGui::Dummy(ImVec2(0, 4.0f));

            // SOLO BUTTON
            ImVec4 soloCol = channels[i].solo ? ImVec4(0.80f,0.12f,0.14f,1.0f) : ImVec4(0.26f,0.27f,0.30f,1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, soloCol);
            if (ImGui::Button((std::string("S##solo")+std::to_string(i)).c_str(), ImVec2(sliderW, SOLO_SIZE))) {
                if (learn_mode_active) start_learn_for_action(ACTION_CHANNEL_SOLO, i);
                else dispatch_action(ACT_SOLO_CHANNEL, i);
            }
            ImGui::PopStyleColor();

            ImGui::Dummy(ImVec2(0, 6.0f));

            // Get slider position before drawing
            ImVec2 slider_pos = ImGui::GetCursorScreenPos();

            float prev_vol = channels[i].volume;
            if (ImGui::VSliderFloat((std::string("##vol")+std::to_string(i)).c_str(),
                                    ImVec2(sliderW, sliderH),
                                    &channels[i].volume, 0.0f, 1.0f, "")) {
                if (learn_mode_active && ImGui::IsItemActive()) {
                    // User is dragging the slider in learn mode - enter learn mode for this channel volume
                    start_learn_for_action(ACTION_CHANNEL_VOLUME, i);
                } else if (prev_vol != channels[i].volume) {
                    dispatch_action(ACT_VOLUME_CHANNEL, i, channels[i].volume);
                }
            }

            // Add note highlight effect AFTER slider (draw on top)
            if (channel_note_fade[i] > 0.0f) {
                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                // Very subtle cyan glow around the slider
                ImU32 highlight_col = ImGui::ColorConvertFloat4ToU32(ImVec4(
                    0.4f + channel_note_fade[i] * 0.15f,  // Very subtle red brightening
                    0.5f + channel_note_fade[i] * 0.2f,   // Slight green
                    0.6f + channel_note_fade[i] * 0.25f,  // Moderate blue (subtle cyan)
                    0.35f * channel_note_fade[i]          // More transparent
                ));
                // Draw very subtle outline around slider
                draw_list->AddRect(
                    ImVec2(slider_pos.x - 1, slider_pos.y - 1),
                    ImVec2(slider_pos.x + sliderW + 1, slider_pos.y + sliderH + 1),
                    highlight_col,
                    2.0f,
                    0,
                    1.5f + channel_note_fade[i] * 0.5f  // 1.5-2px thickness
                );
            }

            ImGui::Dummy(ImVec2(0, 8.0f));

            // MUTE BUTTON with color feedback
            ImVec4 muteCol = channels[i].mute ? ImVec4(0.90f,0.16f,0.18f,1.0f) : ImVec4(0.26f,0.27f,0.30f,1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, muteCol);
            if (ImGui::Button((std::string("M##mute")+std::to_string(i)).c_str(), ImVec2(sliderW, MUTE_SIZE))) {
                if (learn_mode_active) start_learn_for_action(ACTION_CHANNEL_MUTE, i);
                else dispatch_action(ACT_MUTE_CHANNEL, i);
            }
            ImGui::PopStyleColor();

            ImGui::EndGroup();
        }

        // Pitch slider column (positioned after channels)
        {
            float colX = origin.x + num_channels * (sliderW + spacing);
            ImGui::SetCursorPos(ImVec2(colX, origin.y + 8.0f));
            ImGui::BeginGroup();
            ImGui::Text("Pitch");
            ImGui::Dummy(ImVec2(0, 4.0f));
            ImGui::Dummy(ImVec2(sliderW, SOLO_SIZE));
            ImGui::Dummy(ImVec2(0, 6.0f));
            float prev_pitch = pitch_slider;
            if (ImGui::VSliderFloat("##pitch", ImVec2(sliderW, sliderH),
                                    &pitch_slider, -1.0f, 1.0f, "")) {
                if (learn_mode_active && ImGui::IsItemActive()) {
                    // User is dragging the slider in learn mode - enter learn mode for pitch
                    start_learn_for_action(ACTION_PITCH_SET);
                } else if (prev_pitch != pitch_slider) {
                    dispatch_action(ACT_SET_PITCH, -1, pitch_slider);
                }
            }
            ImGui::Dummy(ImVec2(0, 8.0f));
            if (ImGui::Button("R##pitch_reset", ImVec2(sliderW, MUTE_SIZE))) {
                if (learn_mode_active) start_learn_for_action(ACTION_PITCH_RESET);
                else dispatch_action(ACT_PITCH_RESET);
            }
            ImGui::EndGroup();
        }
    }
    else if (ui_mode == UI_MODE_PADS) {
        // PADS MODE: Show application trigger pads (A1-A16)

        // Fade trigger pads
        for (int i = 0; i < MAX_TRIGGER_PADS; i++)
            trigger_pad_fade[i] = fmaxf(trigger_pad_fade[i] - 0.02f, 0.0f);

        // Calculate pad layout (4x4 grid)
        const int PADS_PER_ROW = 4;
        const int NUM_ROWS = MAX_TRIGGER_PADS / PADS_PER_ROW;
        float padSpacing = 12.0f;
        float availWidth = rightW - 2 * padSpacing;
        float availHeight = contentHeight - 16.0f;

        // Calculate pad size (square buttons)
        float padW = (availWidth - padSpacing * (PADS_PER_ROW - 1)) / PADS_PER_ROW;
        float padH = (availHeight - padSpacing * (NUM_ROWS - 1)) / NUM_ROWS;
        float padSize = fminf(padW, padH);
        if (padSize > 140.0f) padSize = 140.0f; // Max pad size
        if (padSize < 60.0f) padSize = 60.0f;   // Min pad size

        // Center the grid
        float gridW = PADS_PER_ROW * padSize + (PADS_PER_ROW - 1) * padSpacing;
        float gridH = NUM_ROWS * padSize + (NUM_ROWS - 1) * padSpacing;
        float startX = origin.x + (rightW - gridW) * 0.5f;
        float startY = origin.y + (contentHeight - gridH) * 0.5f;

        // Draw trigger pads
        for (int row = 0; row < NUM_ROWS; row++) {
            for (int col = 0; col < PADS_PER_ROW; col++) {
                int idx = row * PADS_PER_ROW + col;
                float posX = startX + col * (padSize + padSpacing);
                float posY = startY + row * (padSize + padSpacing);

                ImGui::SetCursorPos(ImVec2(posX, posY));

                // Pad color with fade effect
                float brightness = trigger_pad_fade[idx];
                ImVec4 padCol = ImVec4(
                    0.18f + brightness * 0.50f,
                    0.27f + brightness * 0.40f,
                    0.18f + brightness * 0.24f,
                    1.0f
                );

                ImGui::PushStyleColor(ImGuiCol_Button, padCol);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.48f, 0.32f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.42f, 0.65f, 0.42f, 1.0f));

                char label[16];
                snprintf(label, sizeof(label), "A%d", idx + 1);
                if (ImGui::Button(label, ImVec2(padSize, padSize))) {
                    if (learn_mode_active) {
                        start_learn_for_pad(idx);
                    } else if (common_state && common_state->input_mappings) {
                        trigger_pad_fade[idx] = 1.0f;
                        // Execute the configured action for this pad
                        TriggerPadConfig *pad = &common_state->input_mappings->trigger_pads[idx];
                        if (pad->action != ACTION_NONE) {
                            InputEvent event;
                            event.action = pad->action;
                            event.parameter = pad->parameter;
                            event.value = 127; // Full value for trigger pads
                            handle_input_event(&event);
                        }
                    }
                }

                ImGui::PopStyleColor(3);
            }
        }
    }
    else if (ui_mode == UI_MODE_SONG) {
        // SONG MODE: Show song-specific trigger pads (S1-S16)

        // Fade trigger pads
        for (int i = 0; i < MAX_SONG_TRIGGER_PADS; i++) {
            int global_idx = MAX_TRIGGER_PADS + i;
            trigger_pad_fade[global_idx] = fmaxf(trigger_pad_fade[global_idx] - 0.02f, 0.0f);
        }

        // Calculate pad layout (4x4 grid)
        const int PADS_PER_ROW = 4;
        const int NUM_ROWS = MAX_SONG_TRIGGER_PADS / PADS_PER_ROW;
        float padSpacing = 12.0f;
        float availWidth = rightW - 2 * padSpacing;
        float availHeight = contentHeight - 16.0f;

        // Calculate pad size (square buttons)
        float padW = (availWidth - padSpacing * (PADS_PER_ROW - 1)) / PADS_PER_ROW;
        float padH = (availHeight - padSpacing * (NUM_ROWS - 1)) / NUM_ROWS;
        float padSize = fminf(padW, padH);
        if (padSize > 140.0f) padSize = 140.0f; // Max pad size
        if (padSize < 60.0f) padSize = 60.0f;   // Min pad size

        // Center the grid
        float gridW = PADS_PER_ROW * padSize + (PADS_PER_ROW - 1) * padSpacing;
        float gridH = NUM_ROWS * padSize + (NUM_ROWS - 1) * padSpacing;
        float startX = origin.x + (rightW - gridW) * 0.5f;
        float startY = origin.y + (contentHeight - gridH) * 0.5f;

        // Draw song trigger pads
        for (int row = 0; row < NUM_ROWS; row++) {
            for (int col = 0; col < PADS_PER_ROW; col++) {
                int idx = row * PADS_PER_ROW + col;
                int global_idx = MAX_TRIGGER_PADS + idx;  // Offset for S pads
                float posX = startX + col * (padSize + padSpacing);
                float posY = startY + row * (padSize + padSpacing);

                ImGui::SetCursorPos(ImVec2(posX, posY));

                // Different color for song pads (bluish tint)
                float brightness = trigger_pad_fade[global_idx];
                ImVec4 padCol = ImVec4(
                    0.18f + brightness * 0.30f,
                    0.27f + brightness * 0.40f,
                    0.28f + brightness * 0.50f,  // More blue
                    1.0f
                );

                ImGui::PushStyleColor(ImGuiCol_Button, padCol);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.38f, 0.52f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.38f, 0.52f, 0.72f, 1.0f));

                char label[16];
                snprintf(label, sizeof(label), "S%d", idx + 1);
                if (ImGui::Button(label, ImVec2(padSize, padSize))) {
                    if (learn_mode_active) {
                        start_learn_for_song_pad(idx);  // Use idx (0-15), not global_idx
                    } else if (common_state && common_state->metadata) {
                        trigger_pad_fade[global_idx] = 1.0f;
                        // Execute the configured action for this song pad
                        TriggerPadConfig *pad = &common_state->metadata->song_trigger_pads[idx];

                        if (pad->action != ACTION_NONE) {
                            InputEvent event;
                            event.action = pad->action;
                            event.parameter = pad->parameter;
                            event.value = 127; // Full value for trigger pads
                            handle_input_event(&event);
                        }
                    }
                }

                ImGui::PopStyleColor(3);
            }
        }
    }
    else if (ui_mode == UI_MODE_PERF) {
        // PERF MODE: Show and edit performance events

        ImGui::SetCursorPos(ImVec2(origin.x + 16.0f, origin.y + 16.0f));

        // Make the entire perf area scrollable
        ImGui::BeginChild("##perf_scroll", ImVec2(rightW - 32.0f, contentHeight - 32.0f), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);

        if (!common_state || !common_state->performance) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Performance system not initialized");
        } else {
            RegroovePerformance* perf = common_state->performance;
            int event_count = regroove_performance_get_event_count(perf);

            ImGui::Text("Performance Events (%d total)", event_count);
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 8.0f));

            // Control buttons
            ImGui::BeginGroup();
            if (ImGui::Button("Clear All Events", ImVec2(150.0f, 30.0f))) {
                regroove_performance_clear_events(perf);
                printf("Cleared all performance events\n");
            }
            ImGui::SameLine();
            if (ImGui::Button("Save to .rgx", ImVec2(150.0f, 30.0f))) {
                if (regroove_common_save_rgx(common_state) == 0) {
                    printf("Performance saved to .rgx file\n");
                } else {
                    fprintf(stderr, "Failed to save performance\n");
                }
            }
            ImGui::EndGroup();

            ImGui::Dummy(ImVec2(0, 12.0f));

            // Event list
            ImGui::TextColored(COLOR_SECTION_HEADING, "EVENT LIST");
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 8.0f));

            // Track which event is being edited (-1 = none)
            static int edit_event_index = -1;

            if (event_count == 0) {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No events recorded. Press the 'O' button and play to record.");
            } else {
                ImGui::BeginChild("##event_list", ImVec2(rightW - 64.0f, contentHeight - 200.0f), true);

                ImGui::Columns(6, "event_columns");
                ImGui::SetColumnWidth(0, 80.0f);  // PO:PR
                ImGui::SetColumnWidth(1, 200.0f); // Action
                ImGui::SetColumnWidth(2, 100.0f); // Parameter
                ImGui::SetColumnWidth(3, 100.0f); // Value
                ImGui::SetColumnWidth(4, 80.0f);  // Edit
                ImGui::SetColumnWidth(5, 80.0f);  // Delete

                ImGui::Text("Position"); ImGui::NextColumn();
                ImGui::Text("Action"); ImGui::NextColumn();
                ImGui::Text("Parameter"); ImGui::NextColumn();
                ImGui::Text("Value"); ImGui::NextColumn();
                ImGui::Text("Edit"); ImGui::NextColumn();
                ImGui::Text("Delete"); ImGui::NextColumn();
                ImGui::Separator();

                int delete_index = -1;
                bool save_needed = false;

                for (int i = 0; i < event_count; i++) {
                    PerformanceEvent* evt = regroove_performance_get_event_at(perf, i);
                    if (!evt) continue;

                    ImGui::PushID(i);
                    bool is_editing = (edit_event_index == i);

                    if (is_editing) {
                        // EDITING MODE - Show editable fields

                        // Position (editable)
                        int po = evt->performance_row / 64;
                        int pr = evt->performance_row % 64;
                        ImGui::SetNextItemWidth(40.0f);
                        if (ImGui::InputInt("##edit_po", &po, 0, 0)) {
                            if (po < 0) po = 0;
                            evt->performance_row = po * 64 + pr;
                            save_needed = true;
                        }
                        ImGui::SameLine();
                        ImGui::Text(":");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(40.0f);
                        if (ImGui::InputInt("##edit_pr", &pr, 0, 0)) {
                            if (pr < 0) pr = 0;
                            if (pr >= 64) pr = 63;
                            evt->performance_row = po * 64 + pr;
                            save_needed = true;
                        }
                        ImGui::NextColumn();

                        // Action (editable dropdown)
                        ImGui::SetNextItemWidth(180.0f);
                        if (ImGui::BeginCombo("##edit_action", input_action_name(evt->action))) {
                            for (int a = ACTION_NONE; a < ACTION_MAX; a++) {
                                InputAction act = (InputAction)a;
                                if (ImGui::Selectable(input_action_name(act), evt->action == act)) {
                                    evt->action = act;
                                    save_needed = true;
                                }
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::NextColumn();

                        // Parameter (editable if applicable)
                        if (evt->action == ACTION_CHANNEL_MUTE || evt->action == ACTION_CHANNEL_SOLO ||
                            evt->action == ACTION_CHANNEL_VOLUME || evt->action == ACTION_TRIGGER_PAD ||
                            evt->action == ACTION_JUMP_TO_ORDER || evt->action == ACTION_JUMP_TO_PATTERN ||
                            evt->action == ACTION_QUEUE_ORDER || evt->action == ACTION_QUEUE_PATTERN) {
                            ImGui::SetNextItemWidth(80.0f);
                            if (ImGui::InputInt("##edit_param", &evt->parameter, 0, 0)) {
                                if (evt->parameter < 0) evt->parameter = 0;
                                save_needed = true;
                            }
                        } else {
                            ImGui::Text("-");
                        }
                        ImGui::NextColumn();

                        // Value (editable if applicable)
                        if (evt->action == ACTION_CHANNEL_VOLUME || evt->action == ACTION_PITCH_SET) {
                            ImGui::SetNextItemWidth(80.0f);
                            if (ImGui::InputFloat("##edit_value", &evt->value, 0, 0, "%.0f")) {
                                if (evt->value < 0.0f) evt->value = 0.0f;
                                if (evt->value > 127.0f) evt->value = 127.0f;
                                save_needed = true;
                            }
                        } else {
                            ImGui::Text("-");
                        }
                        ImGui::NextColumn();

                        // Save/Cancel buttons
                        if (ImGui::Button("Save", ImVec2(60.0f, 0.0f))) {
                            edit_event_index = -1;
                            save_needed = true;
                            printf("Saved changes to event at index %d\n", i);
                        }
                        ImGui::NextColumn();

                        if (ImGui::Button("Cancel", ImVec2(40.0f, 0.0f))) {
                            edit_event_index = -1;
                            // Reload to discard changes (or we could cache original values)
                        }
                        ImGui::NextColumn();

                    } else {
                        // DISPLAY MODE - Show read-only fields

                        // Position (PO:PR format)
                        int po = evt->performance_row / 64;
                        int pr = evt->performance_row % 64;
                        ImGui::Text("%02d:%02d", po, pr);
                        ImGui::NextColumn();

                        // Action
                        ImGui::Text("%s", input_action_name(evt->action));
                        ImGui::NextColumn();

                        // Parameter
                        if (evt->action == ACTION_CHANNEL_MUTE || evt->action == ACTION_CHANNEL_SOLO ||
                            evt->action == ACTION_CHANNEL_VOLUME || evt->action == ACTION_TRIGGER_PAD) {
                            ImGui::Text("%d", evt->parameter);
                        } else if (evt->action == ACTION_JUMP_TO_ORDER || evt->action == ACTION_QUEUE_ORDER) {
                            ImGui::Text("Order %d", evt->parameter);
                        } else if (evt->action == ACTION_JUMP_TO_PATTERN || evt->action == ACTION_QUEUE_PATTERN) {
                            ImGui::Text("Pattern %d", evt->parameter);
                        } else {
                            ImGui::Text("-");
                        }
                        ImGui::NextColumn();

                        // Value
                        if (evt->action == ACTION_CHANNEL_VOLUME) {
                            ImGui::Text("%.0f", evt->value);
                        } else {
                            ImGui::Text("-");
                        }
                        ImGui::NextColumn();

                        // Edit button
                        if (ImGui::Button("Edit", ImVec2(60.0f, 0.0f))) {
                            edit_event_index = i;
                        }
                        ImGui::NextColumn();

                        // Delete button
                        if (ImGui::Button("X", ImVec2(40.0f, 0.0f))) {
                            delete_index = i;
                            edit_event_index = -1; // Cancel any editing
                        }
                        ImGui::NextColumn();
                    }

                    ImGui::PopID();
                }

                // Handle deletion
                if (delete_index >= 0) {
                    if (regroove_performance_delete_event(perf, delete_index) == 0) {
                        printf("Deleted event at index %d\n", delete_index);
                        save_needed = true;
                    }
                }

                // Auto-save if any changes were made
                if (save_needed) {
                    regroove_common_save_rgx(common_state);
                }

                ImGui::Columns(1);
                ImGui::EndChild();
            }

            ImGui::Dummy(ImVec2(0, 12.0f));

            // Add new event UI
            ImGui::TextColored(COLOR_SECTION_HEADING, "ADD NEW EVENT");
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 8.0f));

            static int new_perf_po = 0;
            static int new_perf_pr = 0;
            static InputAction new_perf_action = ACTION_PLAY;
            static int new_perf_parameter = 0;
            static float new_perf_value = 127.0f;

            ImGui::Text("Position:");
            ImGui::SameLine(120.0f);
            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputInt("##new_po", &new_perf_po);
            if (new_perf_po < 0) new_perf_po = 0;
            ImGui::SameLine();
            ImGui::Text(":");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputInt("##new_pr", &new_perf_pr);
            if (new_perf_pr < 0) new_perf_pr = 0;
            if (new_perf_pr >= 64) new_perf_pr = 63;

            ImGui::Text("Action:");
            ImGui::SameLine(120.0f);
            ImGui::SetNextItemWidth(250.0f);
            if (ImGui::BeginCombo("##new_perf_action", input_action_name(new_perf_action))) {
                for (int a = ACTION_NONE; a < ACTION_MAX; a++) {
                    InputAction act = (InputAction)a;
                    if (ImGui::Selectable(input_action_name(act), new_perf_action == act)) {
                        new_perf_action = act;
                        new_perf_parameter = 0;
                    }
                }
                ImGui::EndCombo();
            }

            // Parameter input (conditional based on action)
            if (new_perf_action == ACTION_CHANNEL_MUTE || new_perf_action == ACTION_CHANNEL_SOLO ||
                new_perf_action == ACTION_CHANNEL_VOLUME || new_perf_action == ACTION_TRIGGER_PAD ||
                new_perf_action == ACTION_JUMP_TO_ORDER || new_perf_action == ACTION_JUMP_TO_PATTERN ||
                new_perf_action == ACTION_QUEUE_ORDER || new_perf_action == ACTION_QUEUE_PATTERN) {
                ImGui::Text("Parameter:");
                ImGui::SameLine(120.0f);
                ImGui::SetNextItemWidth(100.0f);
                ImGui::InputInt("##new_perf_param", &new_perf_parameter);
                if (new_perf_parameter < 0) new_perf_parameter = 0;
            }

            // Value input (for volume/pitch actions)
            if (new_perf_action == ACTION_CHANNEL_VOLUME || new_perf_action == ACTION_PITCH_SET) {
                ImGui::Text("Value:");
                ImGui::SameLine(120.0f);
                ImGui::SetNextItemWidth(100.0f);
                ImGui::InputFloat("##new_perf_value", &new_perf_value);
                if (new_perf_value < 0.0f) new_perf_value = 0.0f;
                if (new_perf_value > 127.0f) new_perf_value = 127.0f;
            }

            if (ImGui::Button("Add Event", ImVec2(150.0f, 30.0f))) {
                int performance_row = new_perf_po * 64 + new_perf_pr;
                if (regroove_performance_add_event(perf, performance_row, new_perf_action,
                                                   new_perf_parameter, new_perf_value) == 0) {
                    printf("Added event: %s at %02d:%02d\n",
                           input_action_name(new_perf_action), new_perf_po, new_perf_pr);
                    // Auto-save after adding
                    regroove_common_save_rgx(common_state);
                } else {
                    fprintf(stderr, "Failed to add event (buffer full?)\n");
                }
            }

            ImGui::Dummy(ImVec2(0, 12.0f));
            ImGui::TextWrapped("Events are automatically saved to the .rgx file when modified.");

            // Phrase Editor Section
            ImGui::Dummy(ImVec2(0, 20.0f));
            ImGui::TextColored(COLOR_SECTION_HEADING, "PHRASE EDITOR");
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 8.0f));

            ImGui::TextWrapped("Phrases are sequences of actions that execute in succession. Assign phrases to song pads to trigger complex sequences.");
            ImGui::Dummy(ImVec2(0, 12.0f));

            // Phrase list and editor
            static int selected_phrase_idx = -1;
            static char new_phrase_desc[RGX_MAX_PHRASE_NAME] = "";

            // Phrase list
            ImGui::BeginChild("##phrase_list", ImVec2(300.0f, 300.0f), true);
            ImGui::Text("Phrases (%d/%d)", common_state->metadata->phrase_count, RGX_MAX_PHRASES);
            ImGui::Separator();

            for (int i = 0; i < common_state->metadata->phrase_count; i++) {
                Phrase* phrase = &common_state->metadata->phrases[i];
                ImGui::PushID(i);

                // Display as "Phrase 1: description" or just "Phrase 1" if no description
                char label[128];
                if (phrase->name[0] != '\0') {
                    snprintf(label, sizeof(label), "Phrase %d: %s", i + 1, phrase->name);
                } else {
                    snprintf(label, sizeof(label), "Phrase %d", i + 1);
                }

                bool is_selected = (selected_phrase_idx == i);
                if (ImGui::Selectable(label, is_selected)) {
                    selected_phrase_idx = i;
                }

                ImGui::PopID();
            }

            ImGui::EndChild();

            ImGui::SameLine();

            // Phrase editor (right side)
            ImGui::BeginChild("##phrase_editor", ImVec2(rightW - 400.0f, 300.0f), true);

            if (selected_phrase_idx >= 0 && selected_phrase_idx < common_state->metadata->phrase_count) {
                Phrase* phrase = &common_state->metadata->phrases[selected_phrase_idx];

                ImGui::Text("Editing: Phrase %d", selected_phrase_idx + 1);
                ImGui::Separator();
                ImGui::Dummy(ImVec2(0, 8.0f));

                // Phrase description editor
                char desc_buffer[RGX_MAX_PHRASE_NAME];
                strncpy(desc_buffer, phrase->name, RGX_MAX_PHRASE_NAME - 1);
                desc_buffer[RGX_MAX_PHRASE_NAME - 1] = '\0';
                ImGui::Text("Description:");
                ImGui::SameLine(100.0f);
                ImGui::SetNextItemWidth(200.0f);
                if (ImGui::InputText("##phrase_desc", desc_buffer, RGX_MAX_PHRASE_NAME)) {
                    strncpy(phrase->name, desc_buffer, RGX_MAX_PHRASE_NAME - 1);
                    phrase->name[RGX_MAX_PHRASE_NAME - 1] = '\0';
                    regroove_common_save_rgx(common_state);
                }

                ImGui::Dummy(ImVec2(0, 12.0f));
                ImGui::Text("Steps (%d/%d)", phrase->step_count, RGX_MAX_PHRASE_STEPS);
                ImGui::Separator();

                // Steps list
                ImGui::BeginChild("##phrase_steps", ImVec2(0, 150.0f), true);

                int delete_step_idx = -1;
                for (int i = 0; i < phrase->step_count; i++) {
                    PhraseStep* step = &phrase->steps[i];
                    ImGui::PushID(1000 + i);

                    ImGui::Text("%d.", i + 1);
                    ImGui::SameLine(40.0f);

                    // Action dropdown
                    ImGui::SetNextItemWidth(150.0f);
                    if (ImGui::BeginCombo("##action", input_action_name(step->action))) {
                        for (int a = ACTION_NONE; a < ACTION_MAX; a++) {
                            InputAction act = (InputAction)a;
                            if (ImGui::Selectable(input_action_name(act), step->action == act)) {
                                step->action = act;
                                regroove_common_save_rgx(common_state);
                            }
                        }
                        ImGui::EndCombo();
                    }

                    // Parameter (conditional)
                    if (step->action == ACTION_CHANNEL_MUTE || step->action == ACTION_CHANNEL_SOLO ||
                        step->action == ACTION_CHANNEL_VOLUME || step->action == ACTION_TRIGGER_PAD ||
                        step->action == ACTION_JUMP_TO_ORDER || step->action == ACTION_JUMP_TO_PATTERN ||
                        step->action == ACTION_QUEUE_ORDER || step->action == ACTION_QUEUE_PATTERN) {
                        ImGui::SameLine();
                        ImGui::Text("Param:");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(60.0f);
                        if (ImGui::InputInt("##param", &step->parameter, 0, 0)) {
                            if (step->parameter < 0) step->parameter = 0;
                            regroove_common_save_rgx(common_state);
                        }
                    }

                    // Value (for volume/pitch)
                    if (step->action == ACTION_CHANNEL_VOLUME || step->action == ACTION_PITCH_SET) {
                        ImGui::SameLine();
                        ImGui::Text("Val:");
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(60.0f);
                        if (ImGui::InputInt("##value", &step->value, 0, 0)) {
                            if (step->value < 0) step->value = 0;
                            if (step->value > 127) step->value = 127;
                            regroove_common_save_rgx(common_state);
                        }
                    }

                    // Position
                    ImGui::SameLine();
                    ImGui::Text("Pos:");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(60.0f);
                    if (ImGui::InputInt("##position", &step->position_rows, 0, 0)) {
                        if (step->position_rows < 0) step->position_rows = 0;
                        regroove_common_save_rgx(common_state);
                    }

                    // Delete button
                    ImGui::SameLine();
                    if (ImGui::Button("X", ImVec2(30.0f, 0.0f))) {
                        delete_step_idx = i;
                    }

                    ImGui::PopID();
                }

                // Handle step deletion
                if (delete_step_idx >= 0) {
                    for (int i = delete_step_idx; i < phrase->step_count - 1; i++) {
                        phrase->steps[i] = phrase->steps[i + 1];
                    }
                    phrase->step_count--;
                    regroove_common_save_rgx(common_state);
                }

                ImGui::EndChild();

                // Add step button
                ImGui::Dummy(ImVec2(0, 8.0f));
                if (phrase->step_count < RGX_MAX_PHRASE_STEPS) {
                    if (ImGui::Button("Add Step", ImVec2(120.0f, 0.0f))) {
                        PhraseStep* new_step = &phrase->steps[phrase->step_count];
                        new_step->action = ACTION_PLAY;
                        new_step->parameter = 0;
                        new_step->value = 127;
                        new_step->position_rows = 0;
                        phrase->step_count++;
                        regroove_common_save_rgx(common_state);
                    }
                } else {
                    ImGui::TextDisabled("Max steps reached");
                }

                // Delete phrase button
                ImGui::SameLine();
                if (ImGui::Button("Delete Phrase", ImVec2(120.0f, 0.0f))) {
                    // Remove phrase from list
                    for (int i = selected_phrase_idx; i < common_state->metadata->phrase_count - 1; i++) {
                        common_state->metadata->phrases[i] = common_state->metadata->phrases[i + 1];
                    }
                    common_state->metadata->phrase_count--;

                    // Clear any song pads that referenced this phrase
                    for (int i = 0; i < MAX_SONG_TRIGGER_PADS; i++) {
                        if (common_state->metadata->song_trigger_pads[i].phrase_index == selected_phrase_idx) {
                            common_state->metadata->song_trigger_pads[i].phrase_index = -1;
                        } else if (common_state->metadata->song_trigger_pads[i].phrase_index > selected_phrase_idx) {
                            // Adjust indices for pads that referenced phrases after the deleted one
                            common_state->metadata->song_trigger_pads[i].phrase_index--;
                        }
                    }

                    selected_phrase_idx = -1;
                    regroove_common_save_rgx(common_state);
                }

            } else {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Select a phrase to edit");
            }

            ImGui::EndChild();

            // Create new phrase
            ImGui::Dummy(ImVec2(0, 12.0f));
            ImGui::Text("Create New Phrase:");
            ImGui::SetNextItemWidth(200.0f);
            ImGui::InputText("##new_phrase_desc", new_phrase_desc, RGX_MAX_PHRASE_NAME);
            ImGui::SameLine();
            if (ImGui::Button("Create", ImVec2(80.0f, 0.0f))) {
                if (common_state->metadata->phrase_count < RGX_MAX_PHRASES) {
                    Phrase* new_phrase = &common_state->metadata->phrases[common_state->metadata->phrase_count];
                    // Description is optional, can be empty
                    if (new_phrase_desc[0] != '\0') {
                        strncpy(new_phrase->name, new_phrase_desc, RGX_MAX_PHRASE_NAME - 1);
                        new_phrase->name[RGX_MAX_PHRASE_NAME - 1] = '\0';
                    } else {
                        new_phrase->name[0] = '\0';
                    }
                    new_phrase->step_count = 0;
                    common_state->metadata->phrase_count++;
                    selected_phrase_idx = common_state->metadata->phrase_count - 1;
                    new_phrase_desc[0] = '\0';
                    regroove_common_save_rgx(common_state);
                    printf("Created Phrase %d\n", common_state->metadata->phrase_count);
                }
            }

            ImGui::Dummy(ImVec2(0, 12.0f));
            ImGui::TextWrapped("Phrases are saved automatically to the .rgx file. To trigger a phrase from a song pad, set the pad's action to 'trigger_phrase' and the parameter to the phrase index (Phrase 1 = parameter 0, Phrase 2 = parameter 1, etc.).");
        }

        ImGui::EndChild(); // End perf_scroll child window
    }
    else if (ui_mode == UI_MODE_INFO) {
        // INFO MODE: Show song/module information

        ImGui::SetCursorPos(ImVec2(origin.x + 16.0f, origin.y + 16.0f));

        // Make the entire info area scrollable
        ImGui::BeginChild("##info_scroll", ImVec2(rightW - 32.0f, contentHeight - 32.0f), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);

        Regroove *mod = common_state ? common_state->player : NULL;

        // File Browser Section - always visible (independent of loaded module)
        ImGui::TextColored(COLOR_SECTION_HEADING, "FILE BROWSER");
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 8.0f));

        // Selected file (from browser, not necessarily loaded)
        if (common_state->file_list && common_state->file_list->count > 0) {
            const char* current_file = common_state->file_list->filenames[common_state->file_list->current_index];
            ImGui::Text("Selected File:");
            ImGui::SameLine(150.0f);
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", current_file);
        } else {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No directory loaded");
        }

        ImGui::Dummy(ImVec2(0, 12.0f));

        if (!mod) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No module loaded");
        } else {
            // Loaded Module Information Section
            ImGui::TextColored(COLOR_SECTION_HEADING, "MODULE INFORMATION");
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 8.0f));

            // Actually loaded module file
            if (common_state->current_module_path[0] != '\0') {
                // Extract just the filename from the path
                const char* loaded_file = strrchr(common_state->current_module_path, '/');
                if (!loaded_file) loaded_file = strrchr(common_state->current_module_path, '\\');
                if (!loaded_file) loaded_file = common_state->current_module_path;
                else loaded_file++; // Skip the separator

                ImGui::Text("Loaded Module:");
                ImGui::SameLine(150.0f);
                ImGui::TextColored(ImVec4(0.8f, 0.9f, 1.0f, 1.0f), "%s", loaded_file);
            }

            // Number of channels
            ImGui::Text("Channels:");
            ImGui::SameLine(150.0f);
            ImGui::Text("%d", common_state->num_channels);

            // Number of orders
            int num_orders = regroove_get_num_orders(mod);
            ImGui::Text("Song Length:");
            ImGui::SameLine(150.0f);
            ImGui::Text("%d orders", num_orders);

            // Pattern rows
            ImGui::Text("Pattern Rows:");
            ImGui::SameLine(150.0f);
            ImGui::Text("%d rows", total_rows);

            // Current playback position
            int current_order = regroove_get_current_order(mod);
            int current_pattern = regroove_get_current_pattern(mod);
            int current_row = regroove_get_current_row(mod);

            ImGui::Dummy(ImVec2(0, 12.0f));
            ImGui::TextColored(COLOR_SECTION_HEADING, "PLAYBACK INFORMATION");
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 8.0f));

            ImGui::Text("Current Order:");
            ImGui::SameLine(150.0f);
            ImGui::Text("%d", current_order);

            ImGui::Text("Current Pattern:");
            ImGui::SameLine(150.0f);
            ImGui::Text("%d", current_pattern);

            ImGui::Text("Current Row:");
            ImGui::SameLine(150.0f);
            ImGui::Text("%d", current_row);

            // Determine play mode display
            const char* play_mode_str = "Song Mode";
            bool has_performance = false;
            // Check if phrase is active (highest priority)
            if (common_state && common_state->phrase && regroove_phrase_is_active(common_state->phrase)) {
                play_mode_str = "Phrase Mode";
            }
            else if (common_state && common_state->performance) {
                int event_count = regroove_performance_get_event_count(common_state->performance);
                if (event_count > 0 || regroove_performance_is_playing(common_state->performance)) {
                    play_mode_str = "Performance Mode";
                    has_performance = true;
                } else if (loop_enabled) {
                    play_mode_str = "Pattern Loop";
                }
            } else if (loop_enabled) {
                play_mode_str = "Pattern Loop";
            }

            ImGui::Text("Play Mode:");
            ImGui::SameLine(150.0f);
            ImGui::Text("%s", play_mode_str);

            // Show performance position if in performance mode
            if (has_performance && common_state && common_state->performance) {
                int perf_order, perf_row;
                regroove_performance_get_position(common_state->performance, &perf_order, &perf_row);

                ImGui::Text("Performance Order:");
                ImGui::SameLine(150.0f);
                ImGui::Text("%d", perf_order);

                ImGui::Text("Performance Row:");
                ImGui::SameLine(150.0f);
                ImGui::Text("%d", perf_row);
            }

            double pitch = regroove_get_pitch(mod);
            ImGui::Text("Pitch:");
            ImGui::SameLine(150.0f);
            ImGui::Text("%.2fx", pitch);

            int custom_loop_rows = regroove_get_custom_loop_rows(mod);
            if (custom_loop_rows > 0) {
                ImGui::Text("Custom Loop:");
                ImGui::SameLine(150.0f);
                ImGui::Text("%d rows", custom_loop_rows);
            }

            // Channel status overview
            ImGui::Dummy(ImVec2(0, 12.0f));
            ImGui::TextColored(COLOR_SECTION_HEADING, "CHANNEL STATUS");
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 8.0f));

            // Count muted and solo channels
            int muted_count = 0;
            int solo_count = 0;
            for (int i = 0; i < common_state->num_channels; i++) {
                if (channels[i].mute) muted_count++;
                if (channels[i].solo) solo_count++;
            }

            ImGui::Text("Active Channels:");
            ImGui::SameLine(150.0f);
            ImGui::Text("%d / %d", common_state->num_channels - muted_count, common_state->num_channels);

            if (muted_count > 0) {
                ImGui::Text("Muted:");
                ImGui::SameLine(150.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%d channels", muted_count);
            }

            if (solo_count > 0) {
                ImGui::Text("Solo:");
                ImGui::SameLine(150.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "%d channels", solo_count);
            }

            // Order/Pattern table
            ImGui::Dummy(ImVec2(0, 12.0f));
            ImGui::TextColored(COLOR_SECTION_HEADING, "ORDER LIST");
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 8.0f));

            ImGui::BeginChild("##order_list", ImVec2(rightW - 64.0f, 250.0f), true);

            ImGui::Columns(2, "order_columns");
            ImGui::SetColumnWidth(0, 80.0f);
            ImGui::SetColumnWidth(1, 100.0f);

            ImGui::Text("Order"); ImGui::NextColumn();
            ImGui::Text("Pattern"); ImGui::NextColumn();
            ImGui::Separator();

            for (int i = 0; i < num_orders; i++) {
                int pat = regroove_get_order_pattern(mod, i);

                ImGui::PushID(i);

                // Highlight current order
                bool is_current = (i == current_order);
                if (is_current) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.0f, 1.0f));
                }

                // Make order number clickable (hot cue)
                char order_label[16];
                snprintf(order_label, sizeof(order_label), "%s%d", is_current ? "> " : "  ", i);
                if (ImGui::Selectable(order_label, is_current, ImGuiSelectableFlags_SpanAllColumns)) {
                    // Jump to this order (hot cue)
                    dispatch_action(ACT_JUMP_TO_ORDER, i);
                }

                if (is_current) {
                    ImGui::PopStyleColor();
                }
                ImGui::NextColumn();

                // Display pattern number
                if (is_current) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.0f, 1.0f));
                    ImGui::Text("%d", pat);
                    ImGui::PopStyleColor();
                } else {
                    ImGui::Text("%d", pat);
                }
                ImGui::NextColumn();

                ImGui::PopID();
            }

            ImGui::Columns(1);
            ImGui::EndChild();

            // Pattern Descriptions Section
            ImGui::Dummy(ImVec2(0, 20.0f));
            ImGui::TextColored(COLOR_SECTION_HEADING, "PATTERN DESCRIPTIONS");
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 8.0f));

            // Display pattern descriptions with editable text fields
            int num_patterns = regroove_get_num_patterns(mod);

            ImGui::BeginChild("##pattern_desc_list", ImVec2(rightW - 64.0f, 300.0f), true);

            // Track the currently loaded module to clear buffers when module changes
            static char pattern_desc_buffers[RGX_MAX_PATTERNS][RGX_MAX_PATTERN_DESC] = {0};
            static char last_loaded_module[COMMON_MAX_PATH] = {0};

            // Clear buffers if module changed
            if (common_state && strcmp(last_loaded_module, common_state->current_module_path) != 0) {
                memset(pattern_desc_buffers, 0, sizeof(pattern_desc_buffers));
                strncpy(last_loaded_module, common_state->current_module_path, COMMON_MAX_PATH - 1);
                last_loaded_module[COMMON_MAX_PATH - 1] = '\0';
            }

            for (int p = 0; p < num_patterns; p++) {
                ImGui::PushID(p);

                ImGui::Text("Pattern %d:", p);
                ImGui::SameLine(100.0f);

                // Get current description from metadata
                const char* current_desc = regroove_metadata_get_pattern_desc(common_state->metadata, p);

                // Initialize buffer with current description if empty
                if (pattern_desc_buffers[p][0] == '\0') {
                    if (current_desc && current_desc[0] != '\0') {
                        strncpy(pattern_desc_buffers[p], current_desc, RGX_MAX_PATTERN_DESC - 1);
                        pattern_desc_buffers[p][RGX_MAX_PATTERN_DESC - 1] = '\0';
                    }
                }

                ImGui::SetNextItemWidth(400.0f);
                if (ImGui::InputText("##desc", pattern_desc_buffers[p], RGX_MAX_PATTERN_DESC)) {
                    // Description was edited - update metadata in memory only
                    // File save happens when user leaves the field or on explicit save
                    regroove_metadata_set_pattern_desc(common_state->metadata, p, pattern_desc_buffers[p]);
                }

                // Save to file when user finishes editing (loses focus)
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    save_rgx_metadata();
                }

                ImGui::PopID();
            }

            ImGui::EndChild();

            ImGui::Dummy(ImVec2(0, 12.0f));
            ImGui::TextWrapped("Pattern descriptions are automatically saved to a .rgx file alongside your module file.");
        }

        ImGui::EndChild(); // End info_scroll child window
    }
    else if (ui_mode == UI_MODE_MIDI) {
        // MIDI MODE: Consolidated MIDI configuration panel

        ImGui::SetCursorPos(ImVec2(origin.x + 16.0f, origin.y + 16.0f));

        // Make the entire MIDI area scrollable
        ImGui::BeginChild("##midi_scroll", ImVec2(rightW - 32.0f, contentHeight - 32.0f), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);

        ImGui::BeginGroup();

        // =====================================================================
        // SECTION 1: MIDI DEVICES
        // =====================================================================
        ImGui::TextColored(COLOR_SECTION_HEADING, "MIDI DEVICE CONFIGURATION");
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 12.0f));

        // Use cached MIDI port count (refreshed when panel is first shown)
        int num_midi_ports = cached_midi_port_count >= 0 ? cached_midi_port_count : 0;

        // MIDI Device 0 selection
        ImGui::Text("MIDI Input 0:");
        ImGui::SameLine(150.0f);
        int current_device_0 = common_state ? common_state->device_config.midi_device_0 : -1;
        char device_0_label[128];
        if (current_device_0 == -1) {
            snprintf(device_0_label, sizeof(device_0_label), "None");
        } else {
            char port_name[128];
            if (midi_get_port_name(current_device_0, port_name, sizeof(port_name)) == 0) {
                snprintf(device_0_label, sizeof(device_0_label), "%s", port_name);
            } else {
                snprintf(device_0_label, sizeof(device_0_label), "Port %d", current_device_0);
            }
        }

        if (ImGui::BeginCombo("##midi_device_0", device_0_label)) {
            if (ImGui::Selectable("None", current_device_0 == -1)) {
                if (common_state) {
                    common_state->device_config.midi_device_0 = -1;
                    // Hot-swap MIDI devices
                    midi_deinit();
                    int ports[MIDI_MAX_DEVICES];
                    ports[0] = common_state->device_config.midi_device_0;
                    ports[1] = common_state->device_config.midi_device_1;
                    int num_devices = 0;
                    if (ports[0] >= 0) num_devices = 1;
                    if (ports[1] >= 0) num_devices = 2;
                    if (num_devices > 0) {
                        midi_init_multi(my_midi_mapping, NULL, ports, num_devices);
                    }
                    printf("MIDI Device 0 set to: None\n");
                    regroove_common_save_device_config(common_state, current_config_file);
                }
            }
            for (int i = 0; i < num_midi_ports; i++) {
                char label[128];
                char port_name[128];
                if (midi_get_port_name(i, port_name, sizeof(port_name)) == 0) {
                    snprintf(label, sizeof(label), "%s", port_name);
                } else {
                    snprintf(label, sizeof(label), "Port %d", i);
                }
                if (ImGui::Selectable(label, current_device_0 == i)) {
                    if (common_state) {
                        common_state->device_config.midi_device_0 = i;
                        // Hot-swap MIDI devices
                        midi_deinit();
                        int ports[MIDI_MAX_DEVICES];
                        ports[0] = common_state->device_config.midi_device_0;
                        ports[1] = common_state->device_config.midi_device_1;
                        int num_devices = 0;
                        if (ports[0] >= 0) num_devices = 1;
                        if (ports[1] >= 0) num_devices = 2;
                        if (num_devices > 0) {
                            midi_init_multi(my_midi_mapping, NULL, ports, num_devices);
                        }
                        printf("MIDI Device 0 set to: Port %d\n", i);
                        regroove_common_save_device_config(common_state, current_config_file);
                    }
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Dummy(ImVec2(0, 8.0f));

        // MIDI Device 1 selection
        ImGui::Text("MIDI Input 1:");
        ImGui::SameLine(150.0f);
        int current_device_1 = common_state ? common_state->device_config.midi_device_1 : -1;
        char device_1_label[128];
        if (current_device_1 == -1) {
            snprintf(device_1_label, sizeof(device_1_label), "None");
        } else {
            char port_name[128];
            if (midi_get_port_name(current_device_1, port_name, sizeof(port_name)) == 0) {
                snprintf(device_1_label, sizeof(device_1_label), "%s", port_name);
            } else {
                snprintf(device_1_label, sizeof(device_1_label), "Port %d", current_device_1);
            }
        }

        if (ImGui::BeginCombo("##midi_device_1", device_1_label)) {
            if (ImGui::Selectable("None", current_device_1 == -1)) {
                if (common_state) {
                    common_state->device_config.midi_device_1 = -1;
                    // Hot-swap MIDI devices
                    midi_deinit();
                    int ports[MIDI_MAX_DEVICES];
                    ports[0] = common_state->device_config.midi_device_0;
                    ports[1] = common_state->device_config.midi_device_1;
                    int num_devices = 0;
                    if (ports[0] >= 0) num_devices = 1;
                    if (ports[1] >= 0) num_devices = 2;
                    if (num_devices > 0) {
                        midi_init_multi(my_midi_mapping, NULL, ports, num_devices);
                    }
                    printf("MIDI Device 1 set to: None\n");
                    regroove_common_save_device_config(common_state, current_config_file);
                }
            }
            for (int i = 0; i < num_midi_ports; i++) {
                char label[128];
                char port_name[128];
                if (midi_get_port_name(i, port_name, sizeof(port_name)) == 0) {
                    snprintf(label, sizeof(label), "%s", port_name);
                } else {
                    snprintf(label, sizeof(label), "Port %d", i);
                }
                if (ImGui::Selectable(label, current_device_1 == i)) {
                    if (common_state) {
                        common_state->device_config.midi_device_1 = i;
                        // Hot-swap MIDI devices
                        midi_deinit();
                        int ports[MIDI_MAX_DEVICES];
                        ports[0] = common_state->device_config.midi_device_0;
                        ports[1] = common_state->device_config.midi_device_1;
                        int num_devices = 0;
                        if (ports[0] >= 0) num_devices = 1;
                        if (ports[1] >= 0) num_devices = 2;
                        if (num_devices > 0) {
                            midi_init_multi(my_midi_mapping, NULL, ports, num_devices);
                        }
                        printf("MIDI Device 1 set to: Port %d\n", i);
                        regroove_common_save_device_config(common_state, current_config_file);
                    }
                }
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        if (ImGui::Button("Refresh##midi", ImVec2(80.0f, 0.0f))) {
            refresh_midi_devices();
            printf("Refreshed MIDI device list (%d devices found)\n", cached_midi_port_count);
        }

        ImGui::Dummy(ImVec2(0, 20.0f));

        // MIDI Output Device Configuration
        ImGui::Text("MIDI Output (Experimental)");
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 8.0f));
        ImGui::TextWrapped("Send MIDI notes to external synths based on tracker playback. Effect commands 0FFF and EC0 trigger note-off.");
        ImGui::Dummy(ImVec2(0, 8.0f));

        ImGui::Text("MIDI Output:");
        ImGui::SameLine(150.0f);

        const char* midi_out_label = (midi_output_device == -1) ? "Disabled" : "Port";
        if (midi_output_device >= 0) {
            char port_name[128];
            if (midi_get_port_name(midi_output_device, port_name, sizeof(port_name)) == 0) {
                midi_out_label = port_name;
            }
        }

        if (ImGui::BeginCombo("##midi_output", midi_out_label)) {
            // Disabled option
            if (ImGui::Selectable("Disabled", midi_output_device == -1)) {
                if (midi_output_enabled) {
                    midi_output_deinit();
                    midi_output_enabled = false;
                }
                midi_output_device = -1;
                if (common_state) {
                    common_state->device_config.midi_output_device = -1;
                    regroove_common_save_device_config(common_state, current_config_file);
                }
                printf("MIDI output disabled\n");
            }

            // List MIDI output ports
            for (int i = 0; i < num_midi_ports; i++) {
                char label[128];
                char port_name[128];
                if (midi_get_port_name(i, port_name, sizeof(port_name)) == 0) {
                    snprintf(label, sizeof(label), "%s", port_name);
                } else {
                    snprintf(label, sizeof(label), "Port %d", i);
                }

                if (ImGui::Selectable(label, midi_output_device == i)) {
                    // Reinitialize MIDI output with new device
                    if (midi_output_enabled) {
                        midi_output_deinit();
                    }

                    if (midi_output_init(i) == 0) {
                        midi_output_device = i;
                        midi_output_enabled = true;
                        if (common_state) {
                            common_state->device_config.midi_output_device = i;
                            regroove_common_save_device_config(common_state, current_config_file);
                        }
                        printf("MIDI output enabled on port %d\n", i);
                    } else {
                        midi_output_device = -1;
                        midi_output_enabled = false;
                        fprintf(stderr, "Failed to initialize MIDI output on port %d\n", i);
                    }
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Dummy(ImVec2(0, 20.0f));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 20.0f));

        // =====================================================================
        // SECTION 2: MIDI MONITOR
        // =====================================================================
        ImGui::TextColored(COLOR_SECTION_HEADING, "MIDI MONITOR");
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 8.0f));

        ImGui::TextWrapped("Recent MIDI messages (IN = incoming from devices, OUT = outgoing to synths):");
        ImGui::Dummy(ImVec2(0, 8.0f));

        // MIDI monitor table
        ImGui::BeginChild("##midi_monitor", ImVec2(rightW - 64.0f, 250.0f), true);

        ImGui::Columns(6, "midi_monitor_columns");
        ImGui::SetColumnWidth(0, 80.0f);   // Time
        ImGui::SetColumnWidth(1, 60.0f);   // Dir
        ImGui::SetColumnWidth(2, 70.0f);   // Device
        ImGui::SetColumnWidth(3, 100.0f);  // Type
        ImGui::SetColumnWidth(4, 80.0f);   // Number
        ImGui::SetColumnWidth(5, 80.0f);   // Value

        ImGui::Text("Time"); ImGui::NextColumn();
        ImGui::Text("Dir"); ImGui::NextColumn();
        ImGui::Text("Device"); ImGui::NextColumn();
        ImGui::Text("Type"); ImGui::NextColumn();
        ImGui::Text("Number"); ImGui::NextColumn();
        ImGui::Text("Value"); ImGui::NextColumn();
        ImGui::Separator();

        // Display MIDI monitor entries (newest first)
        for (int i = 0; i < midi_monitor_count; i++) {
            int idx = (midi_monitor_head - 1 - i + MIDI_MONITOR_SIZE) % MIDI_MONITOR_SIZE;
            MidiMonitorEntry* entry = &midi_monitor[idx];

            ImGui::Text("%s", entry->timestamp); ImGui::NextColumn();

            // Direction with color
            if (entry->is_output) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "OUT");
            } else {
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.5f, 1.0f), "IN");
            }
            ImGui::NextColumn();

            ImGui::Text("Dev %d", entry->device_id); ImGui::NextColumn();
            ImGui::Text("%s", entry->type); ImGui::NextColumn();
            ImGui::Text("%d", entry->number); ImGui::NextColumn();
            ImGui::Text("%d", entry->value); ImGui::NextColumn();
        }

        ImGui::Columns(1);
        ImGui::EndChild();

        ImGui::Dummy(ImVec2(0, 8.0f));
        if (ImGui::Button("Clear Monitor", ImVec2(120.0f, 0.0f))) {
            midi_monitor_count = 0;
            midi_monitor_head = 0;
        }

        ImGui::Dummy(ImVec2(0, 20.0f));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 20.0f));

        // =====================================================================
        // SECTION 3: APPLICATION TRIGGER PADS (A1-A16)
        // =====================================================================
        ImGui::Text("Application Trigger Pads (A1-A16)");
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 8.0f));

        ImGui::TextWrapped("Configure application-wide trigger pads. Use LEARN mode on the PADS panel to assign MIDI notes.");
        ImGui::Dummy(ImVec2(0, 12.0f));

        // Application pads configuration table
        ImGui::BeginChild("##app_pads_table", ImVec2(rightW - 64.0f, 400.0f), true);

        if (common_state && common_state->input_mappings) {
            ImGui::Columns(6, "app_pad_columns");
            ImGui::SetColumnWidth(0, 50.0f);   // Pad
            ImGui::SetColumnWidth(1, 160.0f);  // Action
            ImGui::SetColumnWidth(2, 150.0f);  // Parameter
            ImGui::SetColumnWidth(3, 90.0f);   // MIDI Note
            ImGui::SetColumnWidth(4, 100.0f);  // Device
            ImGui::SetColumnWidth(5, 80.0f);   // Actions

            ImGui::Text("Pad"); ImGui::NextColumn();
            ImGui::Text("Action"); ImGui::NextColumn();
            ImGui::Text("Parameter"); ImGui::NextColumn();
            ImGui::Text("MIDI Note"); ImGui::NextColumn();
            ImGui::Text("Device"); ImGui::NextColumn();
            ImGui::Text("Actions"); ImGui::NextColumn();
            ImGui::Separator();

            for (int i = 0; i < MAX_TRIGGER_PADS; i++) {
                TriggerPadConfig *pad = &common_state->input_mappings->trigger_pads[i];
                ImGui::PushID(i);

                // Pad number
                ImGui::Text("A%d", i + 1);
                ImGui::NextColumn();

                // Action dropdown
                ImGui::SetNextItemWidth(180.0f);
                if (ImGui::BeginCombo("##action", input_action_name(pad->action))) {
                    for (int a = ACTION_NONE; a < ACTION_MAX; a++) {
                        InputAction act = (InputAction)a;
                        if (ImGui::Selectable(input_action_name(act), pad->action == act)) {
                            pad->action = act;
                            pad->parameter = 0;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::NextColumn();

                // Parameter with +/- buttons (conditional based on action)
                if (pad->action == ACTION_CHANNEL_MUTE || pad->action == ACTION_CHANNEL_SOLO ||
                    pad->action == ACTION_CHANNEL_VOLUME || pad->action == ACTION_TRIGGER_PAD ||
                    pad->action == ACTION_JUMP_TO_ORDER || pad->action == ACTION_JUMP_TO_PATTERN ||
                    pad->action == ACTION_QUEUE_ORDER || pad->action == ACTION_QUEUE_PATTERN ||
                    pad->action == ACTION_TRIGGER_PHRASE) {

                    if (ImGui::Button("-", ImVec2(30.0f, 0.0f))) {
                        if (pad->parameter > 0) pad->parameter--;
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(60.0f);
                    ImGui::InputInt("##param", &pad->parameter, 0, 0);
                    if (pad->parameter < 0) pad->parameter = 0;
                    ImGui::SameLine();
                    if (ImGui::Button("+", ImVec2(30.0f, 0.0f))) {
                        pad->parameter++;
                    }
                } else {
                    ImGui::Text("-");
                }
                ImGui::NextColumn();

                // MIDI Note display (read-only, set via LEARN mode)
                if (pad->midi_note >= 0) {
                    ImGui::Text("Note %d", pad->midi_note);
                } else {
                    ImGui::TextDisabled("Not set");
                }
                ImGui::NextColumn();

                // Device selection
                if (pad->midi_note >= 0) {
                    const char* device_label = pad->midi_device == -1 ? "Any" :
                                               pad->midi_device == -2 ? "Disabled" :
                                               (pad->midi_device == 0 ? "Dev 0" : "Dev 1");
                    ImGui::SetNextItemWidth(90.0f);
                    if (ImGui::BeginCombo("##device", device_label)) {
                        if (ImGui::Selectable("Any", pad->midi_device == -1)) {
                            pad->midi_device = -1;
                            save_mappings_to_config();
                        }
                        if (ImGui::Selectable("Dev 0", pad->midi_device == 0)) {
                            pad->midi_device = 0;
                            save_mappings_to_config();
                        }
                        if (ImGui::Selectable("Dev 1", pad->midi_device == 1)) {
                            pad->midi_device = 1;
                            save_mappings_to_config();
                        }
                        if (ImGui::Selectable("Disabled", pad->midi_device == -2)) {
                            pad->midi_device = -2;
                            save_mappings_to_config();
                        }
                        ImGui::EndCombo();
                    }
                } else {
                    ImGui::TextDisabled("-");
                }
                ImGui::NextColumn();

                // Unmap button
                if (pad->midi_note >= 0) {
                    if (ImGui::Button("Unmap", ImVec2(70.0f, 0.0f))) {
                        pad->midi_note = -1;
                        pad->midi_device = -1;
                        save_mappings_to_config();
                        printf("Unmapped Application Pad A%d\n", i + 1);
                    }
                } else {
                    ImGui::TextDisabled("-");
                }
                ImGui::NextColumn();

                ImGui::PopID();
            }

            ImGui::Columns(1);
        }

        ImGui::EndChild();

        ImGui::Dummy(ImVec2(0, 12.0f));
        ImGui::TextWrapped("To assign MIDI notes to application pads, use LEARN mode: click the LEARN button, then click a pad on the PADS panel, then press a MIDI note on your controller.");

        ImGui::Dummy(ImVec2(0, 20.0f));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 20.0f));

        // =====================================================================
        // SECTION 4: SONG TRIGGER PADS (S1-S16)
        // =====================================================================
        ImGui::Text("Song Trigger Pads (S1-S16)");
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 8.0f));

        ImGui::TextWrapped("Configure song-specific trigger pads that are saved with this module. Use LEARN mode on the SONG panel to assign MIDI notes.");
        ImGui::Dummy(ImVec2(0, 12.0f));

        // Song pads configuration table
        ImGui::BeginChild("##song_pads_table", ImVec2(rightW - 64.0f, 400.0f), true);

        if (common_state && common_state->metadata) {
            ImGui::Columns(6, "song_pad_columns");
            ImGui::SetColumnWidth(0, 50.0f);   // Pad
            ImGui::SetColumnWidth(1, 160.0f);  // Action
            ImGui::SetColumnWidth(2, 150.0f);  // Parameter
            ImGui::SetColumnWidth(3, 90.0f);   // MIDI Note
            ImGui::SetColumnWidth(4, 100.0f);  // Device
            ImGui::SetColumnWidth(5, 80.0f);   // Actions

            ImGui::Text("Pad"); ImGui::NextColumn();
            ImGui::Text("Action"); ImGui::NextColumn();
            ImGui::Text("Parameter"); ImGui::NextColumn();
            ImGui::Text("MIDI Note"); ImGui::NextColumn();
            ImGui::Text("Device"); ImGui::NextColumn();
            ImGui::Text("Actions"); ImGui::NextColumn();
            ImGui::Separator();

            bool song_pads_changed = false;
            for (int i = 0; i < MAX_SONG_TRIGGER_PADS; i++) {
                TriggerPadConfig *pad = &common_state->metadata->song_trigger_pads[i];
                ImGui::PushID(i + 1000); // Offset to avoid ID collision

                // Pad number
                ImGui::Text("S%d", i + 1);
                ImGui::NextColumn();

                // Action dropdown
                ImGui::SetNextItemWidth(180.0f);
                if (ImGui::BeginCombo("##action", input_action_name(pad->action))) {
                    for (int a = ACTION_NONE; a < ACTION_MAX; a++) {
                        InputAction act = (InputAction)a;
                        if (ImGui::Selectable(input_action_name(act), pad->action == act)) {
                            pad->action = act;
                            song_pads_changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::NextColumn();

                // Parameter with +/- buttons (conditional based on action)
                if (pad->action == ACTION_CHANNEL_MUTE || pad->action == ACTION_CHANNEL_SOLO ||
                    pad->action == ACTION_CHANNEL_VOLUME || pad->action == ACTION_TRIGGER_PAD ||
                    pad->action == ACTION_JUMP_TO_ORDER || pad->action == ACTION_JUMP_TO_PATTERN ||
                    pad->action == ACTION_QUEUE_ORDER || pad->action == ACTION_QUEUE_PATTERN ||
                    pad->action == ACTION_TRIGGER_PHRASE) {

                    if (ImGui::Button("-", ImVec2(30.0f, 0.0f))) {
                        if (pad->parameter > 0) {
                            pad->parameter--;
                            song_pads_changed = true;
                        }
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(60.0f);
                    if (ImGui::InputInt("##param", &pad->parameter, 0, 0)) {
                        if (pad->parameter < 0) pad->parameter = 0;
                        song_pads_changed = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("+", ImVec2(30.0f, 0.0f))) {
                        pad->parameter++;
                        song_pads_changed = true;
                    }
                } else {
                    ImGui::Text("-");
                }
                ImGui::NextColumn();

                // MIDI Note display (read-only, set via LEARN mode)
                if (pad->midi_note >= 0) {
                    ImGui::Text("Note %d", pad->midi_note);
                } else {
                    ImGui::TextDisabled("Not set");
                }
                ImGui::NextColumn();

                // Device selection
                if (pad->midi_note >= 0) {
                    const char* device_label = pad->midi_device == -1 ? "Any" :
                                               pad->midi_device == -2 ? "Disabled" :
                                               (pad->midi_device == 0 ? "Dev 0" : "Dev 1");
                    ImGui::SetNextItemWidth(90.0f);
                    if (ImGui::BeginCombo("##device", device_label)) {
                        if (ImGui::Selectable("Any", pad->midi_device == -1)) {
                            pad->midi_device = -1;
                            song_pads_changed = true;
                        }
                        if (ImGui::Selectable("Dev 0", pad->midi_device == 0)) {
                            pad->midi_device = 0;
                            song_pads_changed = true;
                        }
                        if (ImGui::Selectable("Dev 1", pad->midi_device == 1)) {
                            pad->midi_device = 1;
                            song_pads_changed = true;
                        }
                        if (ImGui::Selectable("Disabled", pad->midi_device == -2)) {
                            pad->midi_device = -2;
                            song_pads_changed = true;
                        }
                        ImGui::EndCombo();
                    }
                } else {
                    ImGui::TextDisabled("-");
                }
                ImGui::NextColumn();

                // Unmap button
                if (pad->midi_note >= 0) {
                    if (ImGui::Button("Unmap", ImVec2(70.0f, 0.0f))) {
                        pad->midi_note = -1;
                        pad->midi_device = -1;
                        song_pads_changed = true;
                        printf("Unmapped Song Pad S%d\n", i + 1);
                    }
                } else {
                    ImGui::TextDisabled("-");
                }
                ImGui::NextColumn();

                ImGui::PopID();
            }

            // Auto-save if any changes were made
            if (song_pads_changed) {
                regroove_common_save_rgx(common_state);
            }

            ImGui::Columns(1);
        }

        ImGui::EndChild();

        ImGui::Dummy(ImVec2(0, 12.0f));
        ImGui::TextWrapped("To assign MIDI notes to song pads, use LEARN mode: click the LEARN button, then click a pad on the SONG panel, then press a MIDI note on your controller.");

        ImGui::Dummy(ImVec2(0, 20.0f));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 20.0f));

        // =====================================================================
        // SECTION 5: MIDI CC MAPPINGS
        // =====================================================================
        ImGui::TextColored(COLOR_SECTION_HEADING, "MIDI CC MAPPINGS");
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 12.0f));

        // Static variables for new MIDI mapping
        static InputAction new_midi_action = ACTION_PLAY_PAUSE;
        static int new_midi_parameter = 0;
        static int new_midi_device = -1; // -1 = any device
        static int new_midi_cc = 1;
        static int new_midi_threshold = 64;
        static int new_midi_continuous = 0;

        if (common_state && common_state->input_mappings) {
            // Display existing MIDI mappings in a table
            ImGui::BeginChild("##midi_mappings_list", ImVec2(rightW - 64.0f, 200.0f), true);

            ImGui::Columns(6, "midi_columns");
            ImGui::SetColumnWidth(0, 80.0f);
            ImGui::SetColumnWidth(1, 80.0f);
            ImGui::SetColumnWidth(2, 180.0f);
            ImGui::SetColumnWidth(3, 80.0f);
            ImGui::SetColumnWidth(4, 100.0f);
            ImGui::SetColumnWidth(5, 80.0f);

            ImGui::Text("Device"); ImGui::NextColumn();
            ImGui::Text("CC"); ImGui::NextColumn();
            ImGui::Text("Action"); ImGui::NextColumn();
            ImGui::Text("Param"); ImGui::NextColumn();
            ImGui::Text("Mode"); ImGui::NextColumn();
            ImGui::Text("Delete"); ImGui::NextColumn();
            ImGui::Separator();

            int delete_midi_index = -1;
            for (int i = 0; i < common_state->input_mappings->midi_count; i++) {
                MidiMapping *mm = &common_state->input_mappings->midi_mappings[i];

                // Display device
                if (mm->device_id == -1) {
                    ImGui::Text("Any");
                } else {
                    ImGui::Text("%d", mm->device_id);
                }
                ImGui::NextColumn();

                // Display CC number
                ImGui::Text("CC%d", mm->cc_number); ImGui::NextColumn();

                // Display action
                ImGui::Text("%s", input_action_name(mm->action)); ImGui::NextColumn();

                // Display parameter
                if (mm->action == ACTION_CHANNEL_MUTE || mm->action == ACTION_CHANNEL_SOLO ||
                    mm->action == ACTION_CHANNEL_VOLUME || mm->action == ACTION_TRIGGER_PAD ||
                    mm->action == ACTION_JUMP_TO_ORDER || mm->action == ACTION_JUMP_TO_PATTERN ||
                    mm->action == ACTION_QUEUE_ORDER || mm->action == ACTION_QUEUE_PATTERN) {
                    ImGui::Text("%d", mm->parameter);
                } else {
                    ImGui::Text("-");
                }
                ImGui::NextColumn();

                // Display mode
                if (mm->continuous) {
                    ImGui::Text("Continuous");
                } else {
                    ImGui::Text("Trigger@%d", mm->threshold);
                }
                ImGui::NextColumn();

                // Delete button
                ImGui::PushID(2000 + i);
                if (ImGui::Button("X", ImVec2(40.0f, 0.0f))) {
                    delete_midi_index = i;
                }
                ImGui::PopID();
                ImGui::NextColumn();
            }

            ImGui::Columns(1);
            ImGui::EndChild();

            // Handle deletion
            if (delete_midi_index >= 0) {
                for (int j = delete_midi_index; j < common_state->input_mappings->midi_count - 1; j++) {
                    common_state->input_mappings->midi_mappings[j] =
                        common_state->input_mappings->midi_mappings[j + 1];
                }
                common_state->input_mappings->midi_count--;
                printf("Deleted MIDI mapping at index %d\n", delete_midi_index);
                save_mappings_to_config();
            }

            ImGui::Dummy(ImVec2(0, 8.0f));

            // Add new MIDI mapping UI
            ImGui::Text("Add MIDI CC Mapping:");
            ImGui::Dummy(ImVec2(0, 4.0f));

            // Device dropdown
            ImGui::Text("Device:");
            ImGui::SameLine(150.0f);
            ImGui::SetNextItemWidth(150.0f);
            const char* device_labels[] = { "Any", "Device 0", "Device 1" };
            int device_combo_idx = new_midi_device == -1 ? 0 : new_midi_device + 1;
            if (ImGui::BeginCombo("##new_midi_device", device_labels[device_combo_idx])) {
                if (ImGui::Selectable("Any", new_midi_device == -1)) new_midi_device = -1;
                if (ImGui::Selectable("Device 0", new_midi_device == 0)) new_midi_device = 0;
                if (ImGui::Selectable("Device 1", new_midi_device == 1)) new_midi_device = 1;
                ImGui::EndCombo();
            }

            // CC number
            ImGui::Text("CC Number:");
            ImGui::SameLine(150.0f);
            ImGui::SetNextItemWidth(100.0f);
            ImGui::InputInt("##new_midi_cc", &new_midi_cc);
            if (new_midi_cc < 0) new_midi_cc = 0;
            if (new_midi_cc > 127) new_midi_cc = 127;

            // Action dropdown
            ImGui::Text("Action:");
            ImGui::SameLine(150.0f);
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::BeginCombo("##new_midi_action", input_action_name(new_midi_action))) {
                for (int a = ACTION_NONE; a < ACTION_MAX; a++) {
                    InputAction act = (InputAction)a;
                    if (ImGui::Selectable(input_action_name(act), new_midi_action == act)) {
                        new_midi_action = act;
                        new_midi_parameter = 0;
                        // Auto-set continuous mode for volume, pitch, and effects controls
                        if (act == ACTION_CHANNEL_VOLUME ||
                            act == ACTION_PITCH_SET ||
                            act == ACTION_FX_DISTORTION_DRIVE ||
                            act == ACTION_FX_DISTORTION_MIX ||
                            act == ACTION_FX_FILTER_CUTOFF ||
                            act == ACTION_FX_FILTER_RESONANCE ||
                            act == ACTION_FX_EQ_LOW ||
                            act == ACTION_FX_EQ_MID ||
                            act == ACTION_FX_EQ_HIGH ||
                            act == ACTION_FX_COMPRESSOR_THRESHOLD ||
                            act == ACTION_FX_COMPRESSOR_RATIO ||
                            act == ACTION_FX_DELAY_TIME ||
                            act == ACTION_FX_DELAY_FEEDBACK ||
                            act == ACTION_FX_DELAY_MIX) {
                            new_midi_continuous = 1;
                            new_midi_threshold = 0;
                        } else {
                            new_midi_continuous = 0;
                            new_midi_threshold = 64;
                        }
                    }
                }
                ImGui::EndCombo();
            }

            // Parameter input (only for actions that need it)
            if (new_midi_action == ACTION_CHANNEL_MUTE || new_midi_action == ACTION_CHANNEL_SOLO ||
                new_midi_action == ACTION_CHANNEL_VOLUME || new_midi_action == ACTION_TRIGGER_PAD) {
                ImGui::Text("Parameter:");
                ImGui::SameLine(150.0f);
                ImGui::SetNextItemWidth(100.0f);
                ImGui::InputInt("##new_midi_param", &new_midi_parameter);
                if (new_midi_parameter < 0) new_midi_parameter = 0;
                if (new_midi_action == ACTION_TRIGGER_PAD && new_midi_parameter >= MAX_TRIGGER_PADS)
                    new_midi_parameter = MAX_TRIGGER_PADS - 1;
                if ((new_midi_action == ACTION_CHANNEL_MUTE || new_midi_action == ACTION_CHANNEL_SOLO ||
                     new_midi_action == ACTION_CHANNEL_VOLUME) && new_midi_parameter >= MAX_CHANNELS)
                    new_midi_parameter = MAX_CHANNELS - 1;
            }

            // Mode selection
            ImGui::Text("Mode:");
            ImGui::SameLine(150.0f);
            ImGui::Checkbox("Continuous", (bool*)&new_midi_continuous);
            if (!new_midi_continuous) {
                ImGui::SameLine();
                ImGui::Text("Threshold:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100.0f);
                ImGui::InputInt("##new_midi_threshold", &new_midi_threshold);
                if (new_midi_threshold < 0) new_midi_threshold = 0;
                if (new_midi_threshold > 127) new_midi_threshold = 127;
            }

            // Add button
            if (ImGui::Button("Add MIDI Mapping", ImVec2(200.0f, 0.0f))) {
                if (common_state->input_mappings->midi_count < common_state->input_mappings->midi_capacity) {
                    // Check if this CC/device combo already exists, remove it
                    for (int i = 0; i < common_state->input_mappings->midi_count; i++) {
                        MidiMapping *m = &common_state->input_mappings->midi_mappings[i];
                        if (m->cc_number == new_midi_cc &&
                            (m->device_id == new_midi_device || m->device_id == -1 || new_midi_device == -1)) {
                            for (int j = i; j < common_state->input_mappings->midi_count - 1; j++) {
                                common_state->input_mappings->midi_mappings[j] =
                                    common_state->input_mappings->midi_mappings[j + 1];
                            }
                            common_state->input_mappings->midi_count--;
                            break;
                        }
                    }

                    // Add new mapping
                    MidiMapping new_mapping;
                    new_mapping.device_id = new_midi_device;
                    new_mapping.cc_number = new_midi_cc;
                    new_mapping.action = new_midi_action;
                    new_mapping.parameter = new_midi_parameter;
                    new_mapping.threshold = new_midi_threshold;
                    new_mapping.continuous = new_midi_continuous;
                    common_state->input_mappings->midi_mappings[common_state->input_mappings->midi_count++] = new_mapping;
                    printf("Added MIDI mapping: CC%d (device %d) -> %s (param=%d, %s)\n",
                           new_midi_cc, new_midi_device, input_action_name(new_midi_action),
                           new_midi_parameter, new_midi_continuous ? "continuous" : "trigger");
                    save_mappings_to_config();
                } else {
                    printf("MIDI mappings capacity reached\n");
                }
            }
        }

        ImGui::Dummy(ImVec2(0, 20.0f));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 20.0f));

        // =====================================================================
        // SECTION 6: SAVE BUTTON
        // =====================================================================
        if (ImGui::Button("Save All MIDI Settings", ImVec2(220.0f, 40.0f))) {
            save_mappings_to_config();
            printf("MIDI settings saved to %s\n", current_config_file);
        }

        ImGui::EndGroup();

        ImGui::EndChild(); // End midi_scroll child window
    }
    else if (ui_mode == UI_MODE_EFFECTS) {
        // EFFECTS MODE: Fader-style effects controls (like volume faders)

        if (!effects) {
            ImGui::SetCursorPos(ImVec2(origin.x + 16.0f, origin.y + 16.0f));
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Effects system not initialized");
        } else {
            // Layout: Each effect group gets vertical faders like volume faders
            // Enable button at top, fader(s) in middle
            // fx_spacing: tight spacing within effect groups (between faders in same group)
            // spacing: wider spacing between effect groups (same as volume panel fader spacing)
            const float fx_spacing = 16.0f;
            int col_index = 0;

            // --- DISTORTION GROUP ---
            ImGui::SetCursorPos(ImVec2(origin.x, origin.y + 8.0f));
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "DISTORTION");

            // Drive (with enable)
            {
                float colX = origin.x + col_index * (sliderW + fx_spacing);
                ImGui::SetCursorPos(ImVec2(colX, origin.y + 24.0f));
                ImGui::BeginGroup();
                ImGui::Text("Drive");
                ImGui::Dummy(ImVec2(0, 4.0f));

                // Enable toggle
                int dist_en = regroove_effects_get_distortion_enabled(effects);
                ImVec4 enCol = dist_en ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, enCol);
                if (ImGui::Button("E##dist_en", ImVec2(sliderW, SOLO_SIZE))) {
                    if (learn_mode_active) start_learn_for_action(ACTION_FX_DISTORTION_TOGGLE);
                    else regroove_effects_set_distortion_enabled(effects, !dist_en);
                }
                ImGui::PopStyleColor();
                ImGui::Dummy(ImVec2(0, 6.0f));

                // Drive fader
                float drive = regroove_effects_get_distortion_drive(effects);
                if (ImGui::VSliderFloat("##fx_drive", ImVec2(sliderW, sliderH), &drive, 0.0f, 1.0f, "")) {
                    if (learn_mode_active && ImGui::IsItemActive()) {
                        start_learn_for_action(ACTION_FX_DISTORTION_DRIVE);
                    } else {
                        regroove_effects_set_distortion_drive(effects, drive);
                    }
                }
                ImGui::Dummy(ImVec2(0, 8.0f));
                if (ImGui::Button("R##dist_drive_reset", ImVec2(sliderW, MUTE_SIZE))) {
                    regroove_effects_set_distortion_drive(effects, 0.5f);
                }
                ImGui::EndGroup();
                col_index++;
            }

            // Mix (with reset button)
            {
                float colX = origin.x + col_index * (sliderW + fx_spacing);
                ImGui::SetCursorPos(ImVec2(colX, origin.y + 24.0f));
                ImGui::BeginGroup();
                ImGui::Text("Mix");
                ImGui::Dummy(ImVec2(0, 4.0f));

                // Spacer to align with faders that have enable buttons
                ImGui::Dummy(ImVec2(sliderW, SOLO_SIZE));
                ImGui::Dummy(ImVec2(0, 6.0f));

                float mix = regroove_effects_get_distortion_mix(effects);
                if (ImGui::VSliderFloat("##fx_dist_mix", ImVec2(sliderW, sliderH), &mix, 0.0f, 1.0f, "")) {
                    if (learn_mode_active && ImGui::IsItemActive()) {
                        start_learn_for_action(ACTION_FX_DISTORTION_MIX);
                    } else {
                        regroove_effects_set_distortion_mix(effects, mix);
                    }
                }
                ImGui::Dummy(ImVec2(0, 8.0f));
                if (ImGui::Button("R##dist_mix_reset", ImVec2(sliderW, MUTE_SIZE))) {
                    regroove_effects_set_distortion_mix(effects, 0.5f); // Reset to 50% mix
                }
                ImGui::EndGroup();
                col_index++;
            }

            // Add group spacing (wider gap between effect groups)
            // Add extra spacing equal to volume panel spacing between faders
            float group_gap_offset = (spacing - fx_spacing);

            // --- FILTER GROUP ---
            float filter_start_x = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
            ImGui::SetCursorPos(ImVec2(filter_start_x, origin.y + 8.0f));
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "FILTER");

            // Cutoff (with enable)
            {
                float colX = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
                ImGui::SetCursorPos(ImVec2(colX, origin.y + 24.0f));
                ImGui::BeginGroup();
                ImGui::Text("Cutoff");
                ImGui::Dummy(ImVec2(0, 4.0f));

                // Enable toggle
                int filt_en = regroove_effects_get_filter_enabled(effects);
                ImVec4 enCol = filt_en ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, enCol);
                if (ImGui::Button("E##filt_en", ImVec2(sliderW, SOLO_SIZE))) {
                    if (learn_mode_active) start_learn_for_action(ACTION_FX_FILTER_TOGGLE);
                    else regroove_effects_set_filter_enabled(effects, !filt_en);
                }
                ImGui::PopStyleColor();
                ImGui::Dummy(ImVec2(0, 6.0f));

                float cutoff = regroove_effects_get_filter_cutoff(effects);
                if (ImGui::VSliderFloat("##fx_cutoff", ImVec2(sliderW, sliderH), &cutoff, 0.0f, 1.0f, "")) {
                    if (learn_mode_active && ImGui::IsItemActive()) {
                        start_learn_for_action(ACTION_FX_FILTER_CUTOFF);
                    } else {
                        regroove_effects_set_filter_cutoff(effects, cutoff);
                    }
                }
                ImGui::Dummy(ImVec2(0, 8.0f));
                if (ImGui::Button("R##filt_cutoff_reset", ImVec2(sliderW, MUTE_SIZE))) {
                    regroove_effects_set_filter_cutoff(effects, 1.0f); // Reset to fully open
                }
                ImGui::EndGroup();
                col_index++;
            }

            // Resonance (with reset button)
            {
                float colX = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
                ImGui::SetCursorPos(ImVec2(colX, origin.y + 24.0f));
                ImGui::BeginGroup();
                ImGui::Text("Resonance");
                ImGui::Dummy(ImVec2(0, 4.0f));

                // Spacer to align with faders that have enable buttons
                ImGui::Dummy(ImVec2(sliderW, SOLO_SIZE));
                ImGui::Dummy(ImVec2(0, 6.0f));

                float reso = regroove_effects_get_filter_resonance(effects);
                if (ImGui::VSliderFloat("##fx_reso", ImVec2(sliderW, sliderH), &reso, 0.0f, 1.0f, "")) {
                    if (learn_mode_active && ImGui::IsItemActive()) {
                        start_learn_for_action(ACTION_FX_FILTER_RESONANCE);
                    } else {
                        regroove_effects_set_filter_resonance(effects, reso);
                    }
                }
                ImGui::Dummy(ImVec2(0, 8.0f));
                if (ImGui::Button("R##filt_reso_reset", ImVec2(sliderW, MUTE_SIZE))) {
                    regroove_effects_set_filter_resonance(effects, 0.0f); // Reset to 0
                }
                ImGui::EndGroup();
                col_index++;
            }

            // Add group spacing (wider gap between effect groups)
            group_gap_offset += (spacing - fx_spacing);

            // --- EQ GROUP ---
            float eq_start_x = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
            ImGui::SetCursorPos(ImVec2(eq_start_x, origin.y + 8.0f));
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "EQ");

            // EQ Low (with enable)
            {
                float colX = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
                ImGui::SetCursorPos(ImVec2(colX, origin.y + 24.0f));
                ImGui::BeginGroup();
                ImGui::Text("Low");
                ImGui::Dummy(ImVec2(0, 4.0f));

                int eq_en = regroove_effects_get_eq_enabled(effects);
                ImVec4 enCol = eq_en ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, enCol);
                if (ImGui::Button("E##eq_en", ImVec2(sliderW, SOLO_SIZE))) {
                    if (learn_mode_active) start_learn_for_action(ACTION_FX_EQ_TOGGLE);
                    else regroove_effects_set_eq_enabled(effects, !eq_en);
                }
                ImGui::PopStyleColor();
                ImGui::Dummy(ImVec2(0, 6.0f));

                float eq_low = regroove_effects_get_eq_low(effects);
                if (ImGui::VSliderFloat("##fx_eq_low", ImVec2(sliderW, sliderH), &eq_low, 0.0f, 1.0f, "")) {
                    if (learn_mode_active && ImGui::IsItemActive()) {
                        start_learn_for_action(ACTION_FX_EQ_LOW);
                    } else {
                        regroove_effects_set_eq_low(effects, eq_low);
                    }
                }
                ImGui::Dummy(ImVec2(0, 8.0f));
                if (ImGui::Button("R##eq_low_reset", ImVec2(sliderW, MUTE_SIZE))) {
                    regroove_effects_set_eq_low(effects, 0.5f); // Reset to 50% (neutral)
                }
                ImGui::EndGroup();
                col_index++;
            }

            // EQ Mid (with reset button)
            {
                float colX = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
                ImGui::SetCursorPos(ImVec2(colX, origin.y + 24.0f));
                ImGui::BeginGroup();
                ImGui::Text("Mid");
                ImGui::Dummy(ImVec2(0, 4.0f));

                // Spacer to align with faders that have enable buttons
                ImGui::Dummy(ImVec2(sliderW, SOLO_SIZE));
                ImGui::Dummy(ImVec2(0, 6.0f));

                float eq_mid = regroove_effects_get_eq_mid(effects);
                if (ImGui::VSliderFloat("##fx_eq_mid", ImVec2(sliderW, sliderH), &eq_mid, 0.0f, 1.0f, "")) {
                    if (learn_mode_active && ImGui::IsItemActive()) {
                        start_learn_for_action(ACTION_FX_EQ_MID);
                    } else {
                        regroove_effects_set_eq_mid(effects, eq_mid);
                    }
                }
                ImGui::Dummy(ImVec2(0, 8.0f));
                if (ImGui::Button("R##eq_mid_reset", ImVec2(sliderW, MUTE_SIZE))) {
                    regroove_effects_set_eq_mid(effects, 0.5f); // Reset to 50% (neutral)
                }
                ImGui::EndGroup();
                col_index++;
            }

            // EQ High (with reset button)
            {
                float colX = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
                ImGui::SetCursorPos(ImVec2(colX, origin.y + 24.0f));
                ImGui::BeginGroup();
                ImGui::Text("High");
                ImGui::Dummy(ImVec2(0, 4.0f));

                // Spacer to align with faders that have enable buttons
                ImGui::Dummy(ImVec2(sliderW, SOLO_SIZE));
                ImGui::Dummy(ImVec2(0, 6.0f));

                float eq_high = regroove_effects_get_eq_high(effects);
                if (ImGui::VSliderFloat("##fx_eq_high", ImVec2(sliderW, sliderH), &eq_high, 0.0f, 1.0f, "")) {
                    if (learn_mode_active && ImGui::IsItemActive()) {
                        start_learn_for_action(ACTION_FX_EQ_HIGH);
                    } else {
                        regroove_effects_set_eq_high(effects, eq_high);
                    }
                }
                ImGui::Dummy(ImVec2(0, 8.0f));
                if (ImGui::Button("R##eq_high_reset", ImVec2(sliderW, MUTE_SIZE))) {
                    regroove_effects_set_eq_high(effects, 0.5f); // Reset to 50% (neutral)
                }
                ImGui::EndGroup();
                col_index++;
            }

            // Add group spacing (wider gap between effect groups)
            group_gap_offset += (spacing - fx_spacing);

            // --- COMPRESSOR GROUP ---
            float comp_start_x = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
            ImGui::SetCursorPos(ImVec2(comp_start_x, origin.y + 8.0f));
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "COMPRESSOR");

            // Threshold (with enable)
            {
                float colX = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
                ImGui::SetCursorPos(ImVec2(colX, origin.y + 24.0f));
                ImGui::BeginGroup();
                ImGui::Text("Threshold");
                ImGui::Dummy(ImVec2(0, 4.0f));

                int comp_en = regroove_effects_get_compressor_enabled(effects);
                ImVec4 enCol = comp_en ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, enCol);
                if (ImGui::Button("E##comp_en", ImVec2(sliderW, SOLO_SIZE))) {
                    if (learn_mode_active) start_learn_for_action(ACTION_FX_COMPRESSOR_TOGGLE);
                    else regroove_effects_set_compressor_enabled(effects, !comp_en);
                }
                ImGui::PopStyleColor();
                ImGui::Dummy(ImVec2(0, 6.0f));

                float thresh = regroove_effects_get_compressor_threshold(effects);
                if (ImGui::VSliderFloat("##fx_comp_thresh", ImVec2(sliderW, sliderH), &thresh, 0.0f, 1.0f, "")) {
                    if (learn_mode_active && ImGui::IsItemActive()) {
                        start_learn_for_action(ACTION_FX_COMPRESSOR_THRESHOLD);
                    } else {
                        regroove_effects_set_compressor_threshold(effects, thresh);
                    }
                }
                ImGui::Dummy(ImVec2(0, 8.0f));
                if (ImGui::Button("R##comp_thresh_reset", ImVec2(sliderW, MUTE_SIZE))) {
                    regroove_effects_set_compressor_threshold(effects, 0.5f);
                }
                ImGui::EndGroup();
                col_index++;
            }

            // Ratio (with reset button)
            {
                float colX = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
                ImGui::SetCursorPos(ImVec2(colX, origin.y + 24.0f));
                ImGui::BeginGroup();
                ImGui::Text("Ratio");
                ImGui::Dummy(ImVec2(0, 4.0f));

                // Spacer to align with faders that have enable buttons
                ImGui::Dummy(ImVec2(sliderW, SOLO_SIZE));
                ImGui::Dummy(ImVec2(0, 6.0f));

                float ratio = regroove_effects_get_compressor_ratio(effects);
                if (ImGui::VSliderFloat("##fx_comp_ratio", ImVec2(sliderW, sliderH), &ratio, 0.0f, 1.0f, "")) {
                    if (learn_mode_active && ImGui::IsItemActive()) {
                        start_learn_for_action(ACTION_FX_COMPRESSOR_RATIO);
                    } else {
                        regroove_effects_set_compressor_ratio(effects, ratio);
                    }
                }
                ImGui::Dummy(ImVec2(0, 8.0f));
                if (ImGui::Button("R##comp_ratio_reset", ImVec2(sliderW, MUTE_SIZE))) {
                    regroove_effects_set_compressor_ratio(effects, 0.0f); // Reset to 1:1 (no compression)
                }
                ImGui::EndGroup();
                col_index++;
            }

            // Add group spacing (wider gap between effect groups)
            group_gap_offset += (spacing - fx_spacing);

            // --- DELAY GROUP ---
            float delay_start_x = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
            ImGui::SetCursorPos(ImVec2(delay_start_x, origin.y + 8.0f));
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "DELAY");

            // Time (with enable)
            {
                float colX = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
                ImGui::SetCursorPos(ImVec2(colX, origin.y + 24.0f));
                ImGui::BeginGroup();
                ImGui::Text("Time");
                ImGui::Dummy(ImVec2(0, 4.0f));

                int delay_en = regroove_effects_get_delay_enabled(effects);
                ImVec4 enCol = delay_en ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, enCol);
                if (ImGui::Button("E##delay_en", ImVec2(sliderW, SOLO_SIZE))) {
                    if (learn_mode_active) start_learn_for_action(ACTION_FX_DELAY_TOGGLE);
                    else regroove_effects_set_delay_enabled(effects, !delay_en);
                }
                ImGui::PopStyleColor();
                ImGui::Dummy(ImVec2(0, 6.0f));

                float time = regroove_effects_get_delay_time(effects);
                if (ImGui::VSliderFloat("##fx_delay_time", ImVec2(sliderW, sliderH), &time, 0.0f, 1.0f, "")) {
                    if (learn_mode_active && ImGui::IsItemActive()) {
                        start_learn_for_action(ACTION_FX_DELAY_TIME);
                    } else {
                        regroove_effects_set_delay_time(effects, time);
                    }
                }
                ImGui::Dummy(ImVec2(0, 8.0f));
                if (ImGui::Button("R##delay_time_reset", ImVec2(sliderW, MUTE_SIZE))) {
                    regroove_effects_set_delay_time(effects, 0.25f); // Reset to 250ms
                }
                ImGui::EndGroup();
                col_index++;
            }

            // Feedback (with reset button)
            {
                float colX = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
                ImGui::SetCursorPos(ImVec2(colX, origin.y + 24.0f));
                ImGui::BeginGroup();
                ImGui::Text("Feedback");
                ImGui::Dummy(ImVec2(0, 4.0f));

                // Spacer to align with faders that have enable buttons
                ImGui::Dummy(ImVec2(sliderW, SOLO_SIZE));
                ImGui::Dummy(ImVec2(0, 6.0f));

                float feedback = regroove_effects_get_delay_feedback(effects);
                if (ImGui::VSliderFloat("##fx_delay_fb", ImVec2(sliderW, sliderH), &feedback, 0.0f, 1.0f, "")) {
                    if (learn_mode_active && ImGui::IsItemActive()) {
                        start_learn_for_action(ACTION_FX_DELAY_FEEDBACK);
                    } else {
                        regroove_effects_set_delay_feedback(effects, feedback);
                    }
                }
                ImGui::Dummy(ImVec2(0, 8.0f));
                if (ImGui::Button("R##delay_fb_reset", ImVec2(sliderW, MUTE_SIZE))) {
                    regroove_effects_set_delay_feedback(effects, 0.0f); // Reset to 0 (no feedback)
                }
                ImGui::EndGroup();
                col_index++;
            }

            // Mix (with reset button)
            {
                float colX = origin.x + col_index * (sliderW + fx_spacing) + group_gap_offset;
                ImGui::SetCursorPos(ImVec2(colX, origin.y + 24.0f));
                ImGui::BeginGroup();
                ImGui::Text("Mix");
                ImGui::Dummy(ImVec2(0, 4.0f));

                // Spacer to align with faders that have enable buttons
                ImGui::Dummy(ImVec2(sliderW, SOLO_SIZE));
                ImGui::Dummy(ImVec2(0, 6.0f));

                float mix = regroove_effects_get_delay_mix(effects);
                if (ImGui::VSliderFloat("##fx_delay_mix", ImVec2(sliderW, sliderH), &mix, 0.0f, 1.0f, "")) {
                    if (learn_mode_active && ImGui::IsItemActive()) {
                        start_learn_for_action(ACTION_FX_DELAY_MIX);
                    } else {
                        regroove_effects_set_delay_mix(effects, mix);
                    }
                }
                ImGui::Dummy(ImVec2(0, 8.0f));
                if (ImGui::Button("R##delay_mix_reset", ImVec2(sliderW, MUTE_SIZE))) {
                    regroove_effects_set_delay_mix(effects, 0.5f); // Reset to 50% mix
                }
                ImGui::EndGroup();
                col_index++;
            }
        }
    }
    else if (ui_mode == UI_MODE_SETTINGS) {
        // SETTINGS MODE: Audio and keyboard configuration

        ImGui::SetCursorPos(ImVec2(origin.x + 16.0f, origin.y + 16.0f));

        // Make the entire settings area scrollable
        ImGui::BeginChild("##settings_scroll", ImVec2(rightW - 32.0f, contentHeight - 32.0f), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);

        ImGui::BeginGroup();

        ImGui::TextColored(COLOR_SECTION_HEADING, "AUDIO DEVICE CONFIGURATION");
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 12.0f));

        // Refresh audio device list if empty
        if (audio_device_names.empty()) {
            refresh_audio_devices();
        }

        ImGui::Text("Audio Output:");
        ImGui::SameLine(150.0f);

        const char* current_audio_label = (selected_audio_device >= 0 && selected_audio_device < (int)audio_device_names.size())
            ? audio_device_names[selected_audio_device].c_str()
            : "Default";

        if (ImGui::BeginCombo("##audio_device", current_audio_label)) {
            // Default device option
            if (ImGui::Selectable("Default", selected_audio_device == -1)) {
                selected_audio_device = -1;
                if (common_state) {
                    common_state->device_config.audio_device = -1;
                    regroove_common_save_device_config(common_state, current_config_file);
                }
                printf("Audio device set to: Default\n");
                // Note: Audio device hot-swap would require SDL_CloseAudio() and SDL_OpenAudio()
                // which is more complex than MIDI hot-swap. For now, just save the preference.
            }

            // List all available audio devices
            for (int i = 0; i < (int)audio_device_names.size(); i++) {
                if (ImGui::Selectable(audio_device_names[i].c_str(), selected_audio_device == i)) {
                    selected_audio_device = i;
                    if (common_state) {
                        common_state->device_config.audio_device = i;
                        regroove_common_save_device_config(common_state, current_config_file);
                    }
                    printf("Audio device set to: %s\n", audio_device_names[i].c_str());
                    // Note: Audio device hot-swap would require SDL_CloseAudio() and SDL_OpenAudio()
                    // which is more complex than MIDI hot-swap. For now, just save the preference.
                }
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        if (ImGui::Button("Refresh##audio", ImVec2(80.0f, 0.0f))) {
            refresh_audio_devices();
            printf("Refreshed audio device list (%d devices found)\n", (int)audio_device_names.size());
        }

        ImGui::Dummy(ImVec2(0, 20.0f));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 20.0f));

        // Keyboard Mappings Section
        ImGui::TextColored(COLOR_SECTION_HEADING, "KEYBOARD MAPPINGS");
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 12.0f));

        // Static variables for new keyboard mapping
        static InputAction new_kb_action = ACTION_PLAY_PAUSE;
        static int new_kb_parameter = 0;
        static int new_kb_key = ' ';
        static char kb_key_buffer[32] = " ";

        if (common_state && common_state->input_mappings) {
            // Display existing keyboard mappings in a table
            ImGui::BeginChild("##kb_mappings_list", ImVec2(rightW - 64.0f, 200.0f), true);

            ImGui::Columns(4, "kb_columns");
            ImGui::SetColumnWidth(0, 100.0f);
            ImGui::SetColumnWidth(1, 200.0f);
            ImGui::SetColumnWidth(2, 100.0f);
            ImGui::SetColumnWidth(3, 80.0f);

            ImGui::Text("Key"); ImGui::NextColumn();
            ImGui::Text("Action"); ImGui::NextColumn();
            ImGui::Text("Parameter"); ImGui::NextColumn();
            ImGui::Text("Delete"); ImGui::NextColumn();
            ImGui::Separator();

            int delete_kb_index = -1;
            for (int i = 0; i < common_state->input_mappings->keyboard_count; i++) {
                KeyboardMapping *km = &common_state->input_mappings->keyboard_mappings[i];

                // Display key
                char key_display[32];
                if (km->key >= 32 && km->key <= 126) {
                    snprintf(key_display, sizeof(key_display), "'%c' (%d)", km->key, km->key);
                } else {
                    snprintf(key_display, sizeof(key_display), "Code %d", km->key);
                }
                ImGui::Text("%s", key_display); ImGui::NextColumn();

                // Display action
                ImGui::Text("%s", input_action_name(km->action)); ImGui::NextColumn();

                // Display parameter
                if (km->action == ACTION_CHANNEL_MUTE || km->action == ACTION_CHANNEL_SOLO ||
                    km->action == ACTION_CHANNEL_VOLUME || km->action == ACTION_TRIGGER_PAD ||
                    km->action == ACTION_JUMP_TO_ORDER || km->action == ACTION_JUMP_TO_PATTERN ||
                    km->action == ACTION_QUEUE_ORDER || km->action == ACTION_QUEUE_PATTERN) {
                    ImGui::Text("%d", km->parameter);
                } else {
                    ImGui::Text("-");
                }
                ImGui::NextColumn();

                // Delete button
                ImGui::PushID(i);
                if (ImGui::Button("X", ImVec2(40.0f, 0.0f))) {
                    delete_kb_index = i;
                }
                ImGui::PopID();
                ImGui::NextColumn();
            }

            ImGui::Columns(1);
            ImGui::EndChild();

            // Handle deletion
            if (delete_kb_index >= 0) {
                for (int j = delete_kb_index; j < common_state->input_mappings->keyboard_count - 1; j++) {
                    common_state->input_mappings->keyboard_mappings[j] =
                        common_state->input_mappings->keyboard_mappings[j + 1];
                }
                common_state->input_mappings->keyboard_count--;
                printf("Deleted keyboard mapping at index %d\n", delete_kb_index);
            }

            ImGui::Dummy(ImVec2(0, 8.0f));

            // Add new keyboard mapping UI
            ImGui::Text("Add Keyboard Mapping:");
            ImGui::Dummy(ImVec2(0, 4.0f));

            // Key input
            ImGui::Text("Key:");
            ImGui::SameLine(150.0f);
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::InputText("##new_kb_key", kb_key_buffer, sizeof(kb_key_buffer))) {
                if (kb_key_buffer[0] != '\0') {
                    new_kb_key = kb_key_buffer[0];
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(Type a single character)");

            // Action dropdown
            ImGui::Text("Action:");
            ImGui::SameLine(150.0f);
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::BeginCombo("##new_kb_action", input_action_name(new_kb_action))) {
                for (int a = ACTION_NONE; a < ACTION_MAX; a++) {
                    InputAction act = (InputAction)a;
                    if (ImGui::Selectable(input_action_name(act), new_kb_action == act)) {
                        new_kb_action = act;
                        new_kb_parameter = 0; // Reset parameter when changing action
                    }
                }
                ImGui::EndCombo();
            }

            // Parameter input (only for actions that need it)
            if (new_kb_action == ACTION_CHANNEL_MUTE || new_kb_action == ACTION_CHANNEL_SOLO ||
                new_kb_action == ACTION_CHANNEL_VOLUME || new_kb_action == ACTION_TRIGGER_PAD) {
                ImGui::Text("Parameter:");
                ImGui::SameLine(150.0f);
                ImGui::SetNextItemWidth(100.0f);
                ImGui::InputInt("##new_kb_param", &new_kb_parameter);
                if (new_kb_parameter < 0) new_kb_parameter = 0;
                if (new_kb_action == ACTION_TRIGGER_PAD && new_kb_parameter >= MAX_TRIGGER_PADS)
                    new_kb_parameter = MAX_TRIGGER_PADS - 1;
                if ((new_kb_action == ACTION_CHANNEL_MUTE || new_kb_action == ACTION_CHANNEL_SOLO ||
                     new_kb_action == ACTION_CHANNEL_VOLUME) && new_kb_parameter >= MAX_CHANNELS)
                    new_kb_parameter = MAX_CHANNELS - 1;
            }

            // Add button
            if (ImGui::Button("Add Keyboard Mapping", ImVec2(200.0f, 0.0f))) {
                if (common_state->input_mappings->keyboard_count < common_state->input_mappings->keyboard_capacity) {
                    // Remove any existing mapping for this key
                    for (int i = 0; i < common_state->input_mappings->keyboard_count; i++) {
                        if (common_state->input_mappings->keyboard_mappings[i].key == new_kb_key) {
                            for (int j = i; j < common_state->input_mappings->keyboard_count - 1; j++) {
                                common_state->input_mappings->keyboard_mappings[j] =
                                    common_state->input_mappings->keyboard_mappings[j + 1];
                            }
                            common_state->input_mappings->keyboard_count--;
                            break;
                        }
                    }

                    // Add new mapping
                    KeyboardMapping new_mapping;
                    new_mapping.key = new_kb_key;
                    new_mapping.action = new_kb_action;
                    new_mapping.parameter = new_kb_parameter;
                    common_state->input_mappings->keyboard_mappings[common_state->input_mappings->keyboard_count++] = new_mapping;
                    printf("Added keyboard mapping: key=%d -> %s (param=%d)\n",
                           new_kb_key, input_action_name(new_kb_action), new_kb_parameter);
                } else {
                    printf("Keyboard mappings capacity reached\n");
                }
            }
        }

        ImGui::Dummy(ImVec2(0, 20.0f));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 20.0f));

        if (ImGui::Button("Save Settings", ImVec2(180.0f, 40.0f))) {
            if (common_state && common_state->input_mappings) {
                // Save input mappings
                if (input_mappings_save(common_state->input_mappings, current_config_file) == 0) {
                    // Save device configuration to the same file
                    FILE *f = fopen(current_config_file, "r+");
                    if (f) {
                        // Check if [devices] section already exists
                        char line[512];
                        int has_devices_section = 0;
                        while (fgets(line, sizeof(line), f)) {
                            if (strstr(line, "[devices]")) {
                                has_devices_section = 1;
                                break;
                            }
                        }

                        if (!has_devices_section) {
                            // Append [devices] section
                            fseek(f, 0, SEEK_END);
                            fprintf(f, "\n[devices]\n");
                            fprintf(f, "midi_device_0 = %d\n", common_state->device_config.midi_device_0);
                            fprintf(f, "midi_device_1 = %d\n", common_state->device_config.midi_device_1);
                            fprintf(f, "audio_device = %d\n", selected_audio_device);
                        }
                        fclose(f);
                    }
                    printf("Settings saved to %s\n", current_config_file);
                } else {
                    fprintf(stderr, "Failed to save settings to %s\n", current_config_file);
                }
            }
        }

        ImGui::Dummy(ImVec2(0, 20.0f));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 20.0f));

        // Effect Default Parameters Section
        ImGui::TextColored(COLOR_SECTION_HEADING, "EFFECT DEFAULT PARAMETERS");
        ImGui::Separator();
        ImGui::TextWrapped("(Applied when loading songs)");
        ImGui::Dummy(ImVec2(0, 12.0f));

        if (common_state) {
            bool config_changed = false;

            // Distortion parameters
            ImGui::TextColored(COLOR_SECTION_HEADING, "DISTORTION");
            ImGui::Separator();

            ImGui::Text("Distortion Drive:");
            ImGui::SameLine(200.0f);
            if (ImGui::SliderFloat("##dist_drive", &common_state->device_config.fx_distortion_drive, 0.0f, 1.0f, "%.2f")) {
                config_changed = true;
            }

            ImGui::Text("Distortion Mix:");
            ImGui::SameLine(200.0f);
            if (ImGui::SliderFloat("##dist_mix", &common_state->device_config.fx_distortion_mix, 0.0f, 1.0f, "%.2f")) {
                config_changed = true;
            }

            ImGui::Dummy(ImVec2(0, 12.0f));

            // Filter parameters
            ImGui::TextColored(COLOR_SECTION_HEADING, "FILTER");
            ImGui::Separator();

            ImGui::Text("Filter Cutoff:");
            ImGui::SameLine(200.0f);
            if (ImGui::SliderFloat("##filt_cutoff", &common_state->device_config.fx_filter_cutoff, 0.0f, 1.0f, "%.2f")) {
                config_changed = true;
            }

            ImGui::Text("Filter Resonance:");
            ImGui::SameLine(200.0f);
            if (ImGui::SliderFloat("##filt_res", &common_state->device_config.fx_filter_resonance, 0.0f, 1.0f, "%.2f")) {
                config_changed = true;
            }

            ImGui::Dummy(ImVec2(0, 12.0f));

            // EQ parameters
            ImGui::TextColored(COLOR_SECTION_HEADING, "EQUALIZER");
            ImGui::Separator();

            ImGui::Text("EQ Low:");
            ImGui::SameLine(200.0f);
            if (ImGui::SliderFloat("##eq_low", &common_state->device_config.fx_eq_low, 0.0f, 1.0f, "%.2f")) {
                config_changed = true;
            }

            ImGui::Text("EQ Mid:");
            ImGui::SameLine(200.0f);
            if (ImGui::SliderFloat("##eq_mid", &common_state->device_config.fx_eq_mid, 0.0f, 1.0f, "%.2f")) {
                config_changed = true;
            }

            ImGui::Text("EQ High:");
            ImGui::SameLine(200.0f);
            if (ImGui::SliderFloat("##eq_high", &common_state->device_config.fx_eq_high, 0.0f, 1.0f, "%.2f")) {
                config_changed = true;
            }

            ImGui::Dummy(ImVec2(0, 12.0f));

            // Compressor parameters
            ImGui::TextColored(COLOR_SECTION_HEADING, "COMPRESSOR");
            ImGui::Separator();

            ImGui::Text("Compressor Threshold:");
            ImGui::SameLine(200.0f);
            if (ImGui::SliderFloat("##comp_thresh", &common_state->device_config.fx_compressor_threshold, 0.0f, 1.0f, "%.2f")) {
                config_changed = true;
            }

            ImGui::Text("Compressor Ratio:");
            ImGui::SameLine(200.0f);
            if (ImGui::SliderFloat("##comp_ratio", &common_state->device_config.fx_compressor_ratio, 0.0f, 1.0f, "%.2f")) {
                config_changed = true;
            }

            ImGui::Text("Compressor Attack:");
            ImGui::SameLine(200.0f);
            if (ImGui::SliderFloat("##comp_attack", &common_state->device_config.fx_compressor_attack, 0.0f, 1.0f, "%.2f")) {
                config_changed = true;
            }

            ImGui::Text("Compressor Release:");
            ImGui::SameLine(200.0f);
            if (ImGui::SliderFloat("##comp_release", &common_state->device_config.fx_compressor_release, 0.0f, 1.0f, "%.2f")) {
                config_changed = true;
            }

            ImGui::Text("Compressor Makeup:");
            ImGui::SameLine(200.0f);
            if (ImGui::SliderFloat("##comp_makeup", &common_state->device_config.fx_compressor_makeup, 0.0f, 1.0f, "%.2f")) {
                config_changed = true;
            }

            ImGui::Dummy(ImVec2(0, 12.0f));

            // Delay parameters
            ImGui::TextColored(COLOR_SECTION_HEADING, "DELAY");
            ImGui::Separator();

            ImGui::Text("Delay Time:");
            ImGui::SameLine(200.0f);
            if (ImGui::SliderFloat("##delay_time", &common_state->device_config.fx_delay_time, 0.0f, 1.0f, "%.2f")) {
                config_changed = true;
            }

            ImGui::Text("Delay Feedback:");
            ImGui::SameLine(200.0f);
            if (ImGui::SliderFloat("##delay_fb", &common_state->device_config.fx_delay_feedback, 0.0f, 1.0f, "%.2f")) {
                config_changed = true;
            }

            ImGui::Text("Delay Mix:");
            ImGui::SameLine(200.0f);
            if (ImGui::SliderFloat("##delay_mix", &common_state->device_config.fx_delay_mix, 0.0f, 1.0f, "%.2f")) {
                config_changed = true;
            }

            if (config_changed) {
                regroove_common_save_device_config(common_state, current_config_file);
            }

            ImGui::Dummy(ImVec2(0, 12.0f));
            ImGui::TextWrapped("These parameters will be applied to all effects when a new song is loaded. Current effect settings are not affected.");
        }

        ImGui::EndGroup();

        ImGui::EndChild(); // End settings_scroll child window
    }
    else if (ui_mode == UI_MODE_TRACKER) {
        // TRACKER MODE: Display tracker lanes with pattern data

        ImGui::SetCursorPos(ImVec2(origin.x + 16.0f, origin.y + 16.0f));

        // Make the entire tracker area scrollable
        ImGui::BeginChild("##tracker_scroll", ImVec2(rightW - 32.0f, contentHeight - 32.0f), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);

        Regroove *mod = common_state ? common_state->player : NULL;

        if (!mod) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No module loaded");
        } else {
            int num_channels = common_state->num_channels;
            int current_pattern = regroove_get_current_pattern(mod);
            int current_row = regroove_get_current_row(mod);
            int num_rows = regroove_get_full_pattern_rows(mod);

            ImGui::Text("Tracker View - Pattern %d (%d rows, %d channels)", current_pattern, num_rows, num_channels);
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 8.0f));

            // Calculate column widths
            const float ROW_COL_WIDTH = 50.0f;
            const float CHANNEL_COL_WIDTH = 140.0f;
            const float MIN_CHANNEL_WIDTH = 100.0f;

            // Adjust channel width based on available space
            float available_width = rightW - 64.0f - ROW_COL_WIDTH;
            float channel_width = CHANNEL_COL_WIDTH;
            if (num_channels > 0) {
                float total_needed = num_channels * CHANNEL_COL_WIDTH;
                if (total_needed > available_width) {
                    channel_width = fmaxf(available_width / num_channels, MIN_CHANNEL_WIDTH);
                }
            }

            // Tracker display area
            ImGui::BeginChild("##tracker_view", ImVec2(rightW - 64.0f, contentHeight - 64.0f), true, ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_AlwaysHorizontalScrollbar);

            // Header row
            ImGui::Columns(num_channels + 1, "tracker_columns", true);
            ImGui::SetColumnWidth(0, ROW_COL_WIDTH);
            for (int i = 0; i < num_channels; i++) {
                ImGui::SetColumnWidth(i + 1, channel_width);
            }

            // Column headers
            ImGui::Text("Row"); ImGui::NextColumn();
            for (int ch = 0; ch < num_channels; ch++) {
                ImGui::Text("Ch%d", ch + 1); ImGui::NextColumn();
            }
            ImGui::Separator();

            // Calculate how many rows fit in the visible area
            float window_height = ImGui::GetWindowHeight();
            float line_height = ImGui::GetTextLineHeightWithSpacing();
            int visible_rows = (int)(window_height / line_height);
            int padding_rows = visible_rows / 2; // Half screen of padding on each side

            // Display pattern rows with leading and trailing blank rows
            int start_row = -padding_rows;
            int end_row = num_rows - 1 + padding_rows;

            for (int row = start_row; row <= end_row; row++) {
                ImGui::PushID(row);

                // Check if this is a valid pattern row
                bool is_valid_row = (row >= 0 && row < num_rows);
                bool is_current = (row == current_row);

                // Highlight current row
                if (is_current) {
                    ImVec2 row_min = ImGui::GetCursorScreenPos();
                    ImVec2 row_max = ImVec2(row_min.x + ROW_COL_WIDTH + num_channels * channel_width, row_min.y + ImGui::GetTextLineHeight());
                    ImGui::GetWindowDrawList()->AddRectFilled(row_min, row_max, IM_COL32(60, 60, 40, 255));
                }

                // Row number (blank for padding rows)
                if (is_current) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.0f, 1.0f));
                }
                if (is_valid_row) {
                    ImGui::Text("%02d", row);
                } else {
                    ImGui::Text("  "); // Empty placeholder for padding rows
                }
                if (is_current) {
                    ImGui::PopStyleColor();
                }
                ImGui::NextColumn();

                // Channel data
                for (int ch = 0; ch < num_channels; ch++) {
                    if (is_valid_row) {
                        // Get pattern data for this cell
                        char cell_text[128];
                        int result = regroove_get_pattern_cell(mod, current_pattern, row, ch, cell_text, sizeof(cell_text));

                        // Apply channel note highlighting
                        bool has_note_highlight = (is_current && channel_note_fade[ch] > 0.0f);
                        if (has_note_highlight) {
                            ImVec4 highlight_color = ImVec4(
                                0.2f + channel_note_fade[ch] * 0.6f,
                                0.8f * channel_note_fade[ch],
                                0.2f + channel_note_fade[ch] * 0.4f,
                                1.0f
                            );
                            ImGui::PushStyleColor(ImGuiCol_Text, highlight_color);
                        } else if (is_current) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.0f, 1.0f));
                        }

                        if (result == 0 && cell_text[0] != '\0') {
                            ImGui::Text("%s", cell_text);
                        } else {
                            ImGui::Text("...");
                        }

                        if (has_note_highlight || is_current) {
                            ImGui::PopStyleColor();
                        }
                    } else {
                        // Empty cell for padding rows
                        ImGui::Text("   ");
                    }

                    ImGui::NextColumn();
                }

                ImGui::PopID();
            }

            ImGui::Columns(1);

            // Auto-scroll to keep current row centered
            if (playing) {
                // Calculate position to center current row (accounting for padding)
                float current_row_y = (current_row - start_row + 1) * line_height;
                float target_scroll = current_row_y - (window_height * 0.5f);
                ImGui::SetScrollY(fmaxf(0.0f, target_scroll));
            }

            ImGui::EndChild(); // End tracker_view
        }

        ImGui::EndChild(); // End tracker_scroll child window
    }

    ImGui::EndChild();

    // SEQUENCER BAR (step indicators)
    float sequencerTop = TOP_MARGIN + channelAreaHeight + GAP_ABOVE_SEQUENCER;
    ImGui::SetCursorPos(ImVec2(SIDE_MARGIN, sequencerTop));
    ImGui::BeginChild("sequencer_bar", ImVec2(fullW - 2*SIDE_MARGIN, SEQUENCER_HEIGHT),
                      false, ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    const int numSteps = 16;
    float gap = STEP_GAP;
    float availWidth = ImGui::GetContentRegionAvail().x;
    float stepWidth = (availWidth - gap * (numSteps - 1)) / numSteps;
    stepWidth = Clamp(stepWidth, STEP_MIN, STEP_MAX);
    float rowWidth = numSteps * stepWidth + (numSteps - 1) * gap;
    float centerOffset = (availWidth - rowWidth) * 0.5f;
    if (centerOffset < 0) centerOffset = 0;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + centerOffset);

    for (int i = 0; i < numSteps; ++i) {
        float brightness = step_fade[i];
        ImVec4 btnCol = ImVec4(0.18f + brightness * 0.24f, 
                            0.27f + brightness * 0.38f, 
                            0.18f + brightness * 0.24f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, btnCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f,0.48f,0.32f,1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.42f,0.65f,0.42f,1.0f));
        if (ImGui::Button((std::string("##step")+std::to_string(i)).c_str(), ImVec2(stepWidth, stepWidth))) {
            if (learn_mode_active) {
                start_learn_for_action(ACTION_SET_LOOP_STEP, i);
            } else {
                dispatch_action(ACT_SET_LOOP_ROWS, i);
            }
        }
        ImGui::PopStyleColor(3);
        if (i != numSteps - 1) ImGui::SameLine(0.0f, gap);
    }
    ImGui::EndChild();
    ImGui::End();
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    int midi_port = -1;
    const char *module_path = NULL;
    const char *config_file = "regroove.ini";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc)
            midi_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc)
            config_file = argv[++i];
        else if (!strcmp(argv[i], "--dump-config")) {
            if (regroove_common_save_default_config("regroove_default.ini") == 0) {
                printf("Default configuration saved to regroove_default.ini\n");
                return 0;
            } else {
                fprintf(stderr, "Failed to save default configuration\n");
                return 1;
            }
        }
        else if (!module_path) module_path = argv[i];
    }
    if (!module_path) {
        fprintf(stderr, "Usage: %s directory|file.mod [-m mididevice] [-c config.ini] [--dump-config]\n", argv[0]);
        fprintf(stderr, "  --dump-config  Generate default config file and exit\n");
        return 1;
    }

    // Create common state
    common_state = regroove_common_create();
    if (!common_state) {
        fprintf(stderr, "Failed to create common state\n");
        return 1;
    }

    // Phrase playback state is already initialized in regroove_common_create()

    // Set up performance action callback (routes actions through the performance engine)
    if (common_state->performance) {
        regroove_performance_set_action_callback(common_state->performance, execute_action, NULL);
    }

    // Set up phrase callbacks (pre-trigger reset, action execution, and completion cleanup)
    if (common_state->phrase) {
        regroove_phrase_set_reset_callback(common_state->phrase, phrase_reset_callback, NULL);
        regroove_phrase_set_action_callback(common_state->phrase, phrase_action_callback, NULL);
        regroove_phrase_set_completion_callback(common_state->phrase, phrase_completion_callback, NULL);
    }

    // Track the config file for saving learned mappings
    current_config_file = config_file;

    // Load input mappings from config file
    if (regroove_common_load_mappings(common_state, config_file) != 0) {
        printf("No %s found, using default mappings\n", config_file);
    } else {
        printf("Loaded input mappings from %s\n", config_file);
    }

    // Apply loaded audio device setting to UI variable
    selected_audio_device = common_state->device_config.audio_device;

    // Initialize MIDI output if configured
    if (common_state->device_config.midi_output_device >= 0) {
        if (midi_output_init(common_state->device_config.midi_output_device) == 0) {
            midi_output_device = common_state->device_config.midi_output_device;
            midi_output_enabled = true;
            printf("MIDI output enabled on device %d\n", midi_output_device);
        } else {
            fprintf(stderr, "Failed to initialize MIDI output on device %d\n",
                    common_state->device_config.midi_output_device);
        }
    }

    // Load file list from directory
    std::string dir_path;
    struct stat st;
    if (stat(module_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        // It's a directory
        dir_path = module_path;
    } else {
        // It's a file, get the parent directory
        size_t last_slash = std::string(module_path).find_last_of("/\\");
        if (last_slash != std::string::npos) {
            dir_path = std::string(module_path).substr(0, last_slash);
        } else {
            dir_path = ".";
        }
    }

    common_state->file_list = regroove_filelist_create();
    if (!common_state->file_list ||
        regroove_filelist_load(common_state->file_list, dir_path.c_str()) <= 0) {
        fprintf(stderr, "Failed to load file list from directory: %s\n", dir_path.c_str());
        regroove_common_destroy(common_state);
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) return 1;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_Window* window = SDL_CreateWindow(
        appname, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1200, 640, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
    );
    if (!window) return 1;
    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_ctx);
    SDL_GL_SetSwapInterval(1);
    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.freq = 48000;
    spec.format = AUDIO_S16SYS;
    spec.channels = 2;
    spec.samples = 256;
    spec.callback = audio_callback;
    spec.userdata = NULL;
    // Open audio device (use selected device or NULL for default)
    const char* device_name = NULL;
    if (selected_audio_device >= 0) {
        device_name = SDL_GetAudioDeviceName(selected_audio_device, 0);
    }
    audio_device_id = SDL_OpenAudioDevice(device_name, 0, &spec, NULL, 0);
    if (audio_device_id == 0) {
        fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return 1;
    }
    // Store audio device ID in common state for use by common functions
    common_state->audio_device_id = audio_device_id;
    // Initialize LCD display
    lcd_display = lcd_init(LCD_COLS, LCD_ROWS);
    if (!lcd_display) {
        fprintf(stderr, "Failed to initialize LCD display\n");
        return 1;
    }

    // Initialize effects
    effects = regroove_effects_create();
    if (!effects) {
        fprintf(stderr, "Failed to initialize effects system\n");
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ApplyFlatBlackRedSkin();
    ImGui_ImplSDL2_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL2_Init();
    //if (load_module(module_path) != 0) return 1;
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
            midi_init_multi(my_midi_mapping, NULL, ports, num_devices);
        }
    }
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) running = false;
            handle_keyboard(e, window); // unified handler!
        }
        if (common_state && common_state->player) regroove_process_commands(common_state->player);
        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        ShowMainUI();
        ImGui::Render();
        ImGuiIO& io = ImGui::GetIO();
        glViewport(0,0,(int)io.DisplaySize.x,(int)io.DisplaySize.y);
        glClearColor(0.0f,0.0f,0.0f,1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
        SDL_Delay(10);
    }
    midi_deinit();
    if (audio_device_id) {
        SDL_PauseAudioDevice(audio_device_id, 1);
        SDL_CloseAudioDevice(audio_device_id);
    }

    regroove_common_destroy(common_state);

    // Cleanup effects
    if (effects) {
        regroove_effects_destroy(effects);
    }

    // Cleanup LCD display
    if (lcd_display) {
        lcd_destroy(lcd_display);
    }

    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
