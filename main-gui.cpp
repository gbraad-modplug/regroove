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

extern "C" {
#include "regroove_common.h"
#include "midi.h"
}

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
    UI_MODE_SETTINGS = 2
};
static UIMode ui_mode = UI_MODE_VOLUME;

// Trigger pad configuration
#define MAX_TRIGGER_PADS 16
struct TriggerPad {
    InputAction action = ACTION_NONE;
    int parameter = 0;
    int midi_note = -1;  // MIDI note number that triggers this pad (-1 = not mapped)
    int midi_device = -1; // Which MIDI device (-1 = any)
    float fade = 0.0f;   // Visual feedback fade
};
static TriggerPad trigger_pads[MAX_TRIGGER_PADS];

// Shared state
static RegrooveCommonState *common_state = NULL;
static const char *current_config_file = "regroove.ini"; // Track config file for saving

// Audio device state
static std::vector<std::string> audio_device_names;
static int selected_audio_device = -1;
static SDL_AudioDeviceID audio_device_id = 0;

void refresh_audio_devices() {
    audio_device_names.clear();
    int n = SDL_GetNumAudioDevices(0); // 0 = output devices
    for (int i = 0; i < n; i++) {
        audio_device_names.push_back(SDL_GetAudioDeviceName(i, 0));
    }
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

// -----------------------------------------------------------------------------
// Trigger Pad Initialization
// -----------------------------------------------------------------------------

static void init_trigger_pads_defaults() {
    // Initialize all pads to no action
    for (int i = 0; i < MAX_TRIGGER_PADS; i++) {
        trigger_pads[i].action = ACTION_NONE;
        trigger_pads[i].parameter = 0;
        trigger_pads[i].midi_note = -1;
        trigger_pads[i].midi_device = -1;
        trigger_pads[i].fade = 0.0f;
    }

    // Set up default bindings for common actions
    trigger_pads[0].action = ACTION_PLAY_PAUSE;    // P1: Play/Pause
    trigger_pads[1].action = ACTION_STOP;          // P2: Stop
    trigger_pads[2].action = ACTION_RETRIGGER;     // P3: Retrigger
    trigger_pads[3].action = ACTION_PATTERN_MODE_TOGGLE; // P4: Loop toggle

    trigger_pads[4].action = ACTION_PREV_ORDER;    // P5: Previous order
    trigger_pads[5].action = ACTION_NEXT_ORDER;    // P6: Next order
    trigger_pads[6].action = ACTION_HALVE_LOOP;    // P7: Halve loop
    trigger_pads[7].action = ACTION_FULL_LOOP;     // P8: Full loop

    // Pads 9-12: Channel mutes for first 4 channels
    for (int i = 0; i < 4; i++) {
        trigger_pads[8 + i].action = ACTION_CHANNEL_MUTE;
        trigger_pads[8 + i].parameter = i;
    }

    // Pads 13-16: Reserved for user configuration
    trigger_pads[12].action = ACTION_MUTE_ALL;     // P13: Mute all
    trigger_pads[13].action = ACTION_UNMUTE_ALL;   // P14: Unmute all
    trigger_pads[14].action = ACTION_LOOP_TILL_ROW; // P15: Loop till row
    // P16 is unassigned (ACTION_NONE)
}

// -----------------------------------------------------------------------------
// Module Loading
// -----------------------------------------------------------------------------

static constexpr int MAX_FILENAME_LEN = 16;

static int load_module(const char *path) {
    struct RegrooveCallbacks cbs = {
        .on_order_change = my_order_callback,
        .on_row_change = my_row_callback,
        .on_loop_pattern = my_loop_pattern_callback,
        .on_loop_song = my_loop_song_callback,
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

    if (audio_device_id) SDL_PauseAudioDevice(audio_device_id, 1);
    playing = false;
    for (int i = 0; i < 16; i++) step_fade[i] = 0.0f;
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
    ACT_UNMUTE_ALL
};

void dispatch_action(GuiAction act, int arg1 = -1, float arg2 = 0.0f) {
    Regroove *mod = common_state ? common_state->player : NULL;

    switch (act) {
        case ACT_PLAY:
            if (mod) {
                if (audio_device_id) SDL_PauseAudioDevice(audio_device_id, 0);
                playing = true;
            }
            break;
        case ACT_STOP:
            if (mod) {
                if (audio_device_id) SDL_PauseAudioDevice(audio_device_id, 1);
                playing = false;
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
            dispatch_action(ACT_SET_PITCH, -1, 0.0f);
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
    }
}

// -----------------------------------------------------------------------------
// Input Mapping - Convert InputAction to GuiAction
// -----------------------------------------------------------------------------
static void handle_input_event(InputEvent *event) {
    if (!event || event->action == ACTION_NONE) return;

    switch (event->action) {
        case ACTION_PLAY_PAUSE:
            dispatch_action(playing ? ACT_STOP : ACT_PLAY);
            break;
        case ACTION_PLAY:
            dispatch_action(ACT_PLAY);
            break;
        case ACTION_STOP:
            dispatch_action(ACT_STOP);
            break;
        case ACTION_RETRIGGER:
            dispatch_action(ACT_RETRIGGER);
            break;
        case ACTION_NEXT_ORDER:
            dispatch_action(ACT_NEXT_ORDER);
            break;
        case ACTION_PREV_ORDER:
            dispatch_action(ACT_PREV_ORDER);
            break;
        case ACTION_LOOP_TILL_ROW:
            dispatch_action(ACT_LOOP_TILL_ROW);
            break;
        case ACTION_HALVE_LOOP:
            dispatch_action(ACT_HALVE_LOOP);
            break;
        case ACTION_FULL_LOOP:
            dispatch_action(ACT_FULL_LOOP);
            break;
        case ACTION_PATTERN_MODE_TOGGLE:
            dispatch_action(ACT_TOGGLE_LOOP);
            break;
        case ACTION_MUTE_ALL:
            dispatch_action(ACT_MUTE_ALL);
            break;
        case ACTION_UNMUTE_ALL:
            dispatch_action(ACT_UNMUTE_ALL);
            break;
        case ACTION_PITCH_UP:
            dispatch_action(ACT_PITCH_UP);
            break;
        case ACTION_PITCH_DOWN:
            dispatch_action(ACT_PITCH_DOWN);
            break;
        case ACTION_PITCH_SET:
            // Map MIDI value (0-127) to pitch slider range (-1.0 to 1.0)
            {
                float pitch_value = (event->value / 127.0f) * 2.0f - 1.0f; // Maps 0-127 to -1.0 to 1.0
                dispatch_action(ACT_SET_PITCH, -1, pitch_value);
            }
            break;
        case ACTION_PITCH_RESET:
            dispatch_action(ACT_PITCH_RESET);
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
            dispatch_action(ACT_MUTE_CHANNEL, event->parameter);
            break;
        case ACTION_CHANNEL_SOLO:
            dispatch_action(ACT_SOLO_CHANNEL, event->parameter);
            break;
        case ACTION_CHANNEL_VOLUME:
            dispatch_action(ACT_VOLUME_CHANNEL, event->parameter, event->value / 127.0f);
            break;
        case ACTION_TRIGGER_PAD:
            if (event->parameter >= 0 && event->parameter < MAX_TRIGGER_PADS) {
                // Trigger visual feedback
                trigger_pads[event->parameter].fade = 1.0f;
                // Execute the trigger pad's configured action
                if (trigger_pads[event->parameter].action != ACTION_NONE) {
                    InputEvent pad_event;
                    pad_event.action = trigger_pads[event->parameter].action;
                    pad_event.parameter = trigger_pads[event->parameter].parameter;
                    pad_event.value = event->value;
                    handle_input_event(&pad_event);
                }
            }
            break;
        default:
            break;
    }
}

// Save current mappings to config file
static void save_mappings_to_config() {
    if (!common_state || !common_state->input_mappings) return;

    // Save the current input mappings
    if (input_mappings_save(common_state->input_mappings, current_config_file) == 0) {
        printf("Saved mappings to %s\n", current_config_file);
    } else {
        fprintf(stderr, "Failed to save mappings to %s\n", current_config_file);
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

    if (learn_target_type == LEARN_TRIGGER_PAD) {
        // Remove MIDI note mapping from trigger pad
        if (learn_target_pad_index >= 0 && learn_target_pad_index < MAX_TRIGGER_PADS) {
            if (trigger_pads[learn_target_pad_index].midi_note != -1) {
                trigger_pads[learn_target_pad_index].midi_note = -1;
                trigger_pads[learn_target_pad_index].midi_device = -1;
                printf("Unlearned MIDI note mapping for Pad %d\n", learn_target_pad_index + 1);
                removed_count++;
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
        // Save the updated mappings to config
        save_mappings_to_config();
        printf("Removed %d mapping(s)\n", removed_count);
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
    printf("Learn mode: Waiting for input for Pad %d... (Click LEARN again to unlearn)\n", pad_index + 1);
}

// Learn MIDI mapping for current target
static void learn_midi_mapping(int device_id, int cc_or_note, bool is_note) {
    if (!common_state || !common_state->input_mappings) return;
    if (learn_target_type == LEARN_NONE) return;

    if (is_note && learn_target_type == LEARN_TRIGGER_PAD) {
        // Map MIDI note to trigger pad
        if (learn_target_pad_index >= 0 && learn_target_pad_index < MAX_TRIGGER_PADS) {
            trigger_pads[learn_target_pad_index].midi_note = cc_or_note;
            trigger_pads[learn_target_pad_index].midi_device = device_id;
            printf("Learned MIDI note mapping: Note %d (device %d) -> Pad %d\n",
                   cc_or_note, device_id, learn_target_pad_index + 1);
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

            // Set continuous mode for volume and pitch controls
            if (learn_target_type == LEARN_ACTION &&
                (learn_target_action == ACTION_CHANNEL_VOLUME || learn_target_action == ACTION_PITCH_SET)) {
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

        // Check if any trigger pad is mapped to this MIDI note
        for (int i = 0; i < MAX_TRIGGER_PADS; i++) {
            // Match device (if specified) and note
            if (trigger_pads[i].midi_note == note &&
                (trigger_pads[i].midi_device == -1 || trigger_pads[i].midi_device == device_id)) {

                // Trigger visual feedback
                trigger_pads[i].fade = 1.0f;

                // Execute the configured action
                if (trigger_pads[i].action != ACTION_NONE) {
                    InputEvent event;
                    event.action = trigger_pads[i].action;
                    event.parameter = trigger_pads[i].parameter;
                    event.value = value;
                    handle_input_event(&event);
                }
                break; // Only trigger the first matching pad
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
        char lcd[64];

        // Include truncated file name
        const char* file_disp = "";
        if (common_state && common_state->file_list && common_state->file_list->count > 0) {
            static char truncated[MAX_FILENAME_LEN + 1];
            const char* current_file = common_state->file_list->filenames[common_state->file_list->current_index];
            std::strncpy(truncated, current_file, MAX_FILENAME_LEN);
            truncated[MAX_FILENAME_LEN] = 0;
            file_disp = truncated;
        }

        std::snprintf(lcd, sizeof(lcd),
            "SO:%02d PT:%02d Loop:%s\nPitch:%.2f\nFile:%.*s",
            pattern, order, loop_enabled ? "ON" : "OFF",
            MapPitchFader(pitch_slider),
            MAX_FILENAME_LEN, file_disp);

        DrawLCD(lcd, LEFT_PANEL_WIDTH - 16.0f, LCD_HEIGHT);
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

    // Performance recording
    //ImGui::Button("O", ImVec2(BUTTON_SIZE, BUTTON_SIZE));

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

    // PADS button with active state highlighting
    ImVec4 padsCol = (ui_mode == UI_MODE_PADS) ? ImVec4(0.70f, 0.60f, 0.20f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, padsCol);
    if (ImGui::Button("PADS", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
        ui_mode = UI_MODE_PADS;
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

    ImGui::Dummy(ImVec2(0, TRANSPORT_GAP));

    ImGui::BeginGroup();
    // Input learning mode button
    ImVec4 learnCol = learn_mode_active ? ImVec4(0.90f, 0.16f, 0.18f, 1.0f) : ImVec4(0.26f, 0.27f, 0.30f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, learnCol);
    if (ImGui::Button("LEARN", ImVec2(BUTTON_SIZE * 2 + 8, BUTTON_SIZE))) {
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
        // PADS MODE: Show trigger pad grid

        // Fade trigger pads
        for (int i = 0; i < MAX_TRIGGER_PADS; i++)
            trigger_pads[i].fade = fmaxf(trigger_pads[i].fade - 0.02f, 0.0f);

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
                float brightness = trigger_pads[idx].fade;
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
                snprintf(label, sizeof(label), "P%d", idx + 1);
                if (ImGui::Button(label, ImVec2(padSize, padSize))) {
                    if (learn_mode_active) {
                        start_learn_for_pad(idx);
                    } else {
                        trigger_pads[idx].fade = 1.0f;
                        // Execute the configured action for this pad
                        if (trigger_pads[idx].action != ACTION_NONE) {
                            InputEvent event;
                            event.action = trigger_pads[idx].action;
                            event.parameter = trigger_pads[idx].parameter;
                            event.value = 127; // Full value for trigger pads
                            handle_input_event(&event);
                        }
                    }
                }

                ImGui::PopStyleColor(3);
            }
        }
    }
    else if (ui_mode == UI_MODE_SETTINGS) {
        // SETTINGS MODE: Show device configuration

        ImGui::SetCursorPos(ImVec2(origin.x + 16.0f, origin.y + 16.0f));
        ImGui::BeginGroup();

        ImGui::Text("MIDI Device Configuration");
        ImGui::Dummy(ImVec2(0, 12.0f));

        // Get available MIDI ports
        int num_midi_ports = midi_list_ports();

        // MIDI Device 0 selection
        ImGui::Text("MIDI Device 0:");
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
                    }
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Dummy(ImVec2(0, 8.0f));

        // MIDI Device 1 selection
        ImGui::Text("MIDI Device 1:");
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
                    }
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Dummy(ImVec2(0, 20.0f));

        // Audio Device Configuration
        ImGui::Text("Audio Device Configuration");
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
                printf("Audio device set to: Default\n");
                // Note: Audio device hot-swap would require SDL_CloseAudio() and SDL_OpenAudio()
                // which is more complex than MIDI hot-swap. For now, just save the preference.
            }

            // List all available audio devices
            for (int i = 0; i < (int)audio_device_names.size(); i++) {
                if (ImGui::Selectable(audio_device_names[i].c_str(), selected_audio_device == i)) {
                    selected_audio_device = i;
                    printf("Audio device set to: %s\n", audio_device_names[i].c_str());
                    // Note: Audio device hot-swap would require SDL_CloseAudio() and SDL_OpenAudio()
                    // which is more complex than MIDI hot-swap. For now, just save the preference.
                }
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        if (ImGui::Button("Refresh", ImVec2(80.0f, 0.0f))) {
            refresh_audio_devices();
            printf("Refreshed audio device list (%d devices found)\n", (int)audio_device_names.size());
        }

        ImGui::Dummy(ImVec2(0, 20.0f));

        // Save button to persist settings
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

        ImGui::EndGroup();
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
        if (ImGui::Button((std::string("##step")+std::to_string(i)).c_str(), ImVec2(stepWidth, stepWidth)))
            dispatch_action(ACT_SET_LOOP_ROWS, i);
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

    // Initialize trigger pads with defaults
    init_trigger_pads_defaults();

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

    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}