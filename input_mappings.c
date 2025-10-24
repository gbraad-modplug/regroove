#include "input_mappings.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define INITIAL_CAPACITY 128

// Helper: Parse action name to enum (exposed for performance loading)
InputAction parse_action(const char *str) {
    if (!str) return ACTION_NONE;
    if (strcmp(str, "play_pause") == 0) return ACTION_PLAY_PAUSE;
    if (strcmp(str, "play") == 0) return ACTION_PLAY;
    if (strcmp(str, "stop") == 0) return ACTION_STOP;
    if (strcmp(str, "retrigger") == 0) return ACTION_RETRIGGER;
    if (strcmp(str, "jump_next_order") == 0) return ACTION_JUMP_NEXT_ORDER;
    if (strcmp(str, "jump_prev_order") == 0) return ACTION_JUMP_PREV_ORDER;
    if (strcmp(str, "queue_next_order") == 0) return ACTION_QUEUE_NEXT_ORDER;
    if (strcmp(str, "queue_prev_order") == 0) return ACTION_QUEUE_PREV_ORDER;
    // Legacy support (old configs used "next_order" / "prev_order" which were queued)
    if (strcmp(str, "next_order") == 0) return ACTION_QUEUE_NEXT_ORDER;
    if (strcmp(str, "prev_order") == 0) return ACTION_QUEUE_PREV_ORDER;
    if (strcmp(str, "halve_loop") == 0) return ACTION_HALVE_LOOP;
    if (strcmp(str, "full_loop") == 0) return ACTION_FULL_LOOP;
    if (strcmp(str, "pattern_mode_toggle") == 0) return ACTION_PATTERN_MODE_TOGGLE;
    if (strcmp(str, "mute_all") == 0) return ACTION_MUTE_ALL;
    if (strcmp(str, "unmute_all") == 0) return ACTION_UNMUTE_ALL;
    if (strcmp(str, "pitch_up") == 0) return ACTION_PITCH_UP;
    if (strcmp(str, "pitch_down") == 0) return ACTION_PITCH_DOWN;
    if (strcmp(str, "pitch_set") == 0) return ACTION_PITCH_SET;
    if (strcmp(str, "pitch_reset") == 0) return ACTION_PITCH_RESET;
    if (strcmp(str, "quit") == 0) return ACTION_QUIT;
    if (strcmp(str, "file_prev") == 0) return ACTION_FILE_PREV;
    if (strcmp(str, "file_next") == 0) return ACTION_FILE_NEXT;
    if (strcmp(str, "file_load") == 0) return ACTION_FILE_LOAD;
    if (strcmp(str, "channel_mute") == 0) return ACTION_CHANNEL_MUTE;
    if (strcmp(str, "channel_solo") == 0) return ACTION_CHANNEL_SOLO;
    if (strcmp(str, "queue_channel_mute") == 0) return ACTION_QUEUE_CHANNEL_MUTE;
    if (strcmp(str, "queue_channel_solo") == 0) return ACTION_QUEUE_CHANNEL_SOLO;
    if (strcmp(str, "channel_volume") == 0) return ACTION_CHANNEL_VOLUME;
    if (strcmp(str, "trigger_pad") == 0) return ACTION_TRIGGER_PAD;
    if (strcmp(str, "jump_to_order") == 0) return ACTION_JUMP_TO_ORDER;
    if (strcmp(str, "jump_to_pattern") == 0) return ACTION_JUMP_TO_PATTERN;
    if (strcmp(str, "queue_order") == 0) return ACTION_QUEUE_ORDER;
    if (strcmp(str, "queue_pattern") == 0) return ACTION_QUEUE_PATTERN;
    if (strcmp(str, "record_toggle") == 0) return ACTION_RECORD_TOGGLE;
    if (strcmp(str, "set_loop_step") == 0) return ACTION_SET_LOOP_STEP;
    if (strcmp(str, "trigger_phrase") == 0) return ACTION_TRIGGER_PHRASE;
    if (strcmp(str, "trigger_loop") == 0) return ACTION_TRIGGER_LOOP;
    if (strcmp(str, "play_to_loop") == 0) return ACTION_PLAY_TO_LOOP;
    if (strcmp(str, "fx_distortion_drive") == 0) return ACTION_FX_DISTORTION_DRIVE;
    if (strcmp(str, "fx_distortion_mix") == 0) return ACTION_FX_DISTORTION_MIX;
    if (strcmp(str, "fx_filter_cutoff") == 0) return ACTION_FX_FILTER_CUTOFF;
    if (strcmp(str, "fx_filter_resonance") == 0) return ACTION_FX_FILTER_RESONANCE;
    if (strcmp(str, "fx_eq_low") == 0) return ACTION_FX_EQ_LOW;
    if (strcmp(str, "fx_eq_mid") == 0) return ACTION_FX_EQ_MID;
    if (strcmp(str, "fx_eq_high") == 0) return ACTION_FX_EQ_HIGH;
    if (strcmp(str, "fx_compressor_threshold") == 0) return ACTION_FX_COMPRESSOR_THRESHOLD;
    if (strcmp(str, "fx_compressor_ratio") == 0) return ACTION_FX_COMPRESSOR_RATIO;
    if (strcmp(str, "fx_delay_time") == 0) return ACTION_FX_DELAY_TIME;
    if (strcmp(str, "fx_delay_feedback") == 0) return ACTION_FX_DELAY_FEEDBACK;
    if (strcmp(str, "fx_delay_mix") == 0) return ACTION_FX_DELAY_MIX;
    if (strcmp(str, "fx_distortion_toggle") == 0) return ACTION_FX_DISTORTION_TOGGLE;
    if (strcmp(str, "fx_filter_toggle") == 0) return ACTION_FX_FILTER_TOGGLE;
    if (strcmp(str, "fx_eq_toggle") == 0) return ACTION_FX_EQ_TOGGLE;
    if (strcmp(str, "fx_compressor_toggle") == 0) return ACTION_FX_COMPRESSOR_TOGGLE;
    if (strcmp(str, "fx_delay_toggle") == 0) return ACTION_FX_DELAY_TOGGLE;
    if (strcmp(str, "master_volume") == 0) return ACTION_MASTER_VOLUME;
    if (strcmp(str, "playback_volume") == 0) return ACTION_PLAYBACK_VOLUME;
    if (strcmp(str, "input_volume") == 0) return ACTION_INPUT_VOLUME;
    if (strcmp(str, "master_pan") == 0) return ACTION_MASTER_PAN;
    if (strcmp(str, "playback_pan") == 0) return ACTION_PLAYBACK_PAN;
    if (strcmp(str, "input_pan") == 0) return ACTION_INPUT_PAN;
    if (strcmp(str, "channel_pan") == 0) return ACTION_CHANNEL_PAN;
    if (strcmp(str, "master_mute") == 0) return ACTION_MASTER_MUTE;
    if (strcmp(str, "playback_mute") == 0) return ACTION_PLAYBACK_MUTE;
    if (strcmp(str, "input_mute") == 0) return ACTION_INPUT_MUTE;
    return ACTION_NONE;
}

// Helper: Convert action enum to string
const char* input_action_name(InputAction action) {
    switch (action) {
        case ACTION_PLAY_PAUSE: return "play_pause";
        case ACTION_PLAY: return "play";
        case ACTION_STOP: return "stop";
        case ACTION_RETRIGGER: return "retrigger";
        case ACTION_JUMP_NEXT_ORDER: return "jump_next_order";
        case ACTION_JUMP_PREV_ORDER: return "jump_prev_order";
        case ACTION_QUEUE_NEXT_ORDER: return "queue_next_order";
        case ACTION_QUEUE_PREV_ORDER: return "queue_prev_order";
        case ACTION_HALVE_LOOP: return "halve_loop";
        case ACTION_FULL_LOOP: return "full_loop";
        case ACTION_PATTERN_MODE_TOGGLE: return "pattern_mode_toggle";
        case ACTION_MUTE_ALL: return "mute_all";
        case ACTION_UNMUTE_ALL: return "unmute_all";
        case ACTION_PITCH_UP: return "pitch_up";
        case ACTION_PITCH_DOWN: return "pitch_down";
        case ACTION_PITCH_SET: return "pitch_set";
        case ACTION_PITCH_RESET: return "pitch_reset";
        case ACTION_QUIT: return "quit";
        case ACTION_FILE_PREV: return "file_prev";
        case ACTION_FILE_NEXT: return "file_next";
        case ACTION_FILE_LOAD: return "file_load";
        case ACTION_CHANNEL_MUTE: return "channel_mute";
        case ACTION_CHANNEL_SOLO: return "channel_solo";
        case ACTION_QUEUE_CHANNEL_MUTE: return "queue_channel_mute";
        case ACTION_QUEUE_CHANNEL_SOLO: return "queue_channel_solo";
        case ACTION_CHANNEL_VOLUME: return "channel_volume";
        case ACTION_TRIGGER_PAD: return "trigger_pad";
        case ACTION_JUMP_TO_ORDER: return "jump_to_order";
        case ACTION_JUMP_TO_PATTERN: return "jump_to_pattern";
        case ACTION_QUEUE_ORDER: return "queue_order";
        case ACTION_QUEUE_PATTERN: return "queue_pattern";
        case ACTION_RECORD_TOGGLE: return "record_toggle";
        case ACTION_SET_LOOP_STEP: return "set_loop_step";
        case ACTION_TRIGGER_PHRASE: return "trigger_phrase";
        case ACTION_TRIGGER_LOOP: return "trigger_loop";
        case ACTION_PLAY_TO_LOOP: return "play_to_loop";
        case ACTION_FX_DISTORTION_DRIVE: return "fx_distortion_drive";
        case ACTION_FX_DISTORTION_MIX: return "fx_distortion_mix";
        case ACTION_FX_FILTER_CUTOFF: return "fx_filter_cutoff";
        case ACTION_FX_FILTER_RESONANCE: return "fx_filter_resonance";
        case ACTION_FX_EQ_LOW: return "fx_eq_low";
        case ACTION_FX_EQ_MID: return "fx_eq_mid";
        case ACTION_FX_EQ_HIGH: return "fx_eq_high";
        case ACTION_FX_COMPRESSOR_THRESHOLD: return "fx_compressor_threshold";
        case ACTION_FX_COMPRESSOR_RATIO: return "fx_compressor_ratio";
        case ACTION_FX_DELAY_TIME: return "fx_delay_time";
        case ACTION_FX_DELAY_FEEDBACK: return "fx_delay_feedback";
        case ACTION_FX_DELAY_MIX: return "fx_delay_mix";
        case ACTION_FX_DISTORTION_TOGGLE: return "fx_distortion_toggle";
        case ACTION_FX_FILTER_TOGGLE: return "fx_filter_toggle";
        case ACTION_FX_EQ_TOGGLE: return "fx_eq_toggle";
        case ACTION_FX_COMPRESSOR_TOGGLE: return "fx_compressor_toggle";
        case ACTION_FX_DELAY_TOGGLE: return "fx_delay_toggle";
        case ACTION_MASTER_VOLUME: return "master_volume";
        case ACTION_PLAYBACK_VOLUME: return "playback_volume";
        case ACTION_INPUT_VOLUME: return "input_volume";
        case ACTION_MASTER_PAN: return "master_pan";
        case ACTION_PLAYBACK_PAN: return "playback_pan";
        case ACTION_INPUT_PAN: return "input_pan";
        case ACTION_CHANNEL_PAN: return "channel_pan";
        case ACTION_MASTER_MUTE: return "master_mute";
        case ACTION_PLAYBACK_MUTE: return "playback_mute";
        case ACTION_INPUT_MUTE: return "input_mute";
        default: return "none";
    }
}

// Helper: Trim whitespace
static char* trim(char *str) {
    while (isspace(*str)) str++;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace(*end)) end--;
    *(end + 1) = '\0';
    return str;
}

InputMappings* input_mappings_create(void) {
    InputMappings *m = calloc(1, sizeof(InputMappings));
    if (!m) return NULL;

    m->midi_capacity = INITIAL_CAPACITY;
    m->midi_mappings = calloc(m->midi_capacity, sizeof(MidiMapping));

    m->keyboard_capacity = INITIAL_CAPACITY;
    m->keyboard_mappings = calloc(m->keyboard_capacity, sizeof(KeyboardMapping));

    if (!m->midi_mappings || !m->keyboard_mappings) {
        input_mappings_destroy(m);
        return NULL;
    }

    input_mappings_reset_defaults(m);
    return m;
}

void input_mappings_destroy(InputMappings *mappings) {
    if (!mappings) return;
    free(mappings->midi_mappings);
    free(mappings->keyboard_mappings);
    free(mappings);
}

void input_mappings_reset_defaults(InputMappings *mappings) {
    if (!mappings) return;

    mappings->midi_count = 0;
    mappings->keyboard_count = 0;

    // Initialize trigger pads with default configuration
    for (int i = 0; i < MAX_TRIGGER_PADS; i++) {
        mappings->trigger_pads[i].action = ACTION_NONE;
        mappings->trigger_pads[i].parameter = 0;
        mappings->trigger_pads[i].midi_note = -1;
        mappings->trigger_pads[i].midi_device = -1;
        mappings->trigger_pads[i].phrase_index = -1;
    }

    // Set up default bindings for trigger pads
    mappings->trigger_pads[0].action = ACTION_PLAY_PAUSE;           // P1: Play/Pause
    mappings->trigger_pads[1].action = ACTION_STOP;                 // P2: Stop
    mappings->trigger_pads[2].action = ACTION_RETRIGGER;            // P3: Retrigger
    mappings->trigger_pads[3].action = ACTION_PATTERN_MODE_TOGGLE;  // P4: Loop toggle

    mappings->trigger_pads[4].action = ACTION_QUEUE_PREV_ORDER;     // P5: Previous order (queued)
    mappings->trigger_pads[5].action = ACTION_QUEUE_NEXT_ORDER;     // P6: Next order (queued)
    mappings->trigger_pads[6].action = ACTION_HALVE_LOOP;           // P7: Halve loop
    mappings->trigger_pads[7].action = ACTION_FULL_LOOP;            // P8: Full loop

    // Pads 9-12: Channel mutes for first 4 channels
    for (int i = 0; i < 4; i++) {
        mappings->trigger_pads[8 + i].action = ACTION_CHANNEL_MUTE;
        mappings->trigger_pads[8 + i].parameter = i;
    }

    // Pads 13-16: Reserved for user configuration
    mappings->trigger_pads[12].action = ACTION_MUTE_ALL;            // P13: Mute all
    mappings->trigger_pads[13].action = ACTION_UNMUTE_ALL;          // P14: Unmute all
    // P15-P16 are unassigned (ACTION_NONE)

    // Default MIDI mappings (based on current implementation)
    // device_id = -1 means any device, 0 = device 0, 1 = device 1
    MidiMapping default_midi[] = {
        {-1, 41, ACTION_PLAY, 0, 64, 0},
        {-1, 42, ACTION_STOP, 0, 64, 0},
        {-1, 46, ACTION_PATTERN_MODE_TOGGLE, 0, 64, 0},
        {-1, 44, ACTION_JUMP_NEXT_ORDER, 0, 64, 0},
        {-1, 43, ACTION_JUMP_PREV_ORDER, 0, 64, 0},
        {-1, 60, ACTION_FILE_LOAD, 0, 64, 0},
        {-1, 61, ACTION_FILE_PREV, 0, 64, 0},
        {-1, 62, ACTION_FILE_NEXT, 0, 64, 0},
        // Channel solo (CC 32-39)
        {-1, 32, ACTION_CHANNEL_SOLO, 0, 64, 0},
        {-1, 33, ACTION_CHANNEL_SOLO, 1, 64, 0},
        {-1, 34, ACTION_CHANNEL_SOLO, 2, 64, 0},
        {-1, 35, ACTION_CHANNEL_SOLO, 3, 64, 0},
        {-1, 36, ACTION_CHANNEL_SOLO, 4, 64, 0},
        {-1, 37, ACTION_CHANNEL_SOLO, 5, 64, 0},
        {-1, 38, ACTION_CHANNEL_SOLO, 6, 64, 0},
        {-1, 39, ACTION_CHANNEL_SOLO, 7, 64, 0},
        // Channel mute (CC 48-55)
        {-1, 48, ACTION_CHANNEL_MUTE, 0, 64, 0},
        {-1, 49, ACTION_CHANNEL_MUTE, 1, 64, 0},
        {-1, 50, ACTION_CHANNEL_MUTE, 2, 64, 0},
        {-1, 51, ACTION_CHANNEL_MUTE, 3, 64, 0},
        {-1, 52, ACTION_CHANNEL_MUTE, 4, 64, 0},
        {-1, 53, ACTION_CHANNEL_MUTE, 5, 64, 0},
        {-1, 54, ACTION_CHANNEL_MUTE, 6, 64, 0},
        {-1, 55, ACTION_CHANNEL_MUTE, 7, 64, 0},
        // Channel volume (CC 0-7) - continuous controls
        {-1, 0, ACTION_CHANNEL_VOLUME, 0, 0, 1},
        {-1, 1, ACTION_CHANNEL_VOLUME, 1, 0, 1},
        {-1, 2, ACTION_CHANNEL_VOLUME, 2, 0, 1},
        {-1, 3, ACTION_CHANNEL_VOLUME, 3, 0, 1},
        {-1, 4, ACTION_CHANNEL_VOLUME, 4, 0, 1},
        {-1, 5, ACTION_CHANNEL_VOLUME, 5, 0, 1},
        {-1, 6, ACTION_CHANNEL_VOLUME, 6, 0, 1},
        {-1, 7, ACTION_CHANNEL_VOLUME, 7, 0, 1},
    };

    int default_midi_count = sizeof(default_midi) / sizeof(default_midi[0]);
    for (int i = 0; i < default_midi_count && i < mappings->midi_capacity; i++) {
        mappings->midi_mappings[i] = default_midi[i];
    }
    mappings->midi_count = default_midi_count;

    // Default keyboard mappings (based on current implementation)
    KeyboardMapping default_keyboard[] = {
        {' ', ACTION_PLAY_PAUSE, 0},
        {'r', ACTION_RETRIGGER, 0},
        {'R', ACTION_RETRIGGER, 0},
        {'N', ACTION_QUEUE_NEXT_ORDER, 0},
        {'n', ACTION_QUEUE_NEXT_ORDER, 0},
        {'P', ACTION_QUEUE_PREV_ORDER, 0},
        {'p', ACTION_QUEUE_PREV_ORDER, 0},
        {'h', ACTION_HALVE_LOOP, 0},
        {'H', ACTION_HALVE_LOOP, 0},
        {'f', ACTION_FULL_LOOP, 0},
        {'F', ACTION_FULL_LOOP, 0},
        {'S', ACTION_PATTERN_MODE_TOGGLE, 0},
        {'s', ACTION_PATTERN_MODE_TOGGLE, 0},
        {'m', ACTION_MUTE_ALL, 0},
        {'M', ACTION_MUTE_ALL, 0},
        {'u', ACTION_UNMUTE_ALL, 0},
        {'U', ACTION_UNMUTE_ALL, 0},
        {'+', ACTION_PITCH_UP, 0},
        {'=', ACTION_PITCH_UP, 0},
        {'-', ACTION_PITCH_DOWN, 0},
        {'q', ACTION_QUIT, 0},
        {'Q', ACTION_QUIT, 0},
        {27, ACTION_QUIT, 0}, // ESC
        {'[', ACTION_FILE_PREV, 0},
        {']', ACTION_FILE_NEXT, 0},
        {'\n', ACTION_FILE_LOAD, 0},
        {'\r', ACTION_FILE_LOAD, 0},
        // Channel mute keys 1-9
        {'1', ACTION_CHANNEL_MUTE, 0},
        {'2', ACTION_CHANNEL_MUTE, 1},
        {'3', ACTION_CHANNEL_MUTE, 2},
        {'4', ACTION_CHANNEL_MUTE, 3},
        {'5', ACTION_CHANNEL_MUTE, 4},
        {'6', ACTION_CHANNEL_MUTE, 5},
        {'7', ACTION_CHANNEL_MUTE, 6},
        {'8', ACTION_CHANNEL_MUTE, 7},
        // Numpad keys for trigger pads (GUI only - codes 159-168)
        {159, ACTION_TRIGGER_PAD, 0},  // KP0 -> Pad 1
        {160, ACTION_TRIGGER_PAD, 1},  // KP1 -> Pad 2
        {161, ACTION_TRIGGER_PAD, 2},  // KP2 -> Pad 3
        {162, ACTION_TRIGGER_PAD, 3},  // KP3 -> Pad 4
        {163, ACTION_TRIGGER_PAD, 4},  // KP4 -> Pad 5
        {164, ACTION_TRIGGER_PAD, 5},  // KP5 -> Pad 6
        {165, ACTION_TRIGGER_PAD, 6},  // KP6 -> Pad 7
        {166, ACTION_TRIGGER_PAD, 7},  // KP7 -> Pad 8
        {167, ACTION_TRIGGER_PAD, 8},  // KP8 -> Pad 9
        {168, ACTION_TRIGGER_PAD, 9},  // KP9 -> Pad 10
    };

    int default_keyboard_count = sizeof(default_keyboard) / sizeof(default_keyboard[0]);
    for (int i = 0; i < default_keyboard_count && i < mappings->keyboard_capacity; i++) {
        mappings->keyboard_mappings[i] = default_keyboard[i];
    }
    mappings->keyboard_count = default_keyboard_count;
}

int input_mappings_load(InputMappings *mappings, const char *filepath) {
    if (!mappings || !filepath) return -1;

    FILE *f = fopen(filepath, "r");
    if (!f) return -1;

    char line[512];
    enum { SECTION_NONE, SECTION_MIDI, SECTION_KEYBOARD, SECTION_TRIGGER_PADS } section = SECTION_NONE;

    // Clear existing mappings
    mappings->midi_count = 0;
    mappings->keyboard_count = 0;

    // Reset trigger pads to defaults
    for (int i = 0; i < MAX_TRIGGER_PADS; i++) {
        mappings->trigger_pads[i].action = ACTION_NONE;
        mappings->trigger_pads[i].parameter = 0;
        mappings->trigger_pads[i].midi_note = -1;
        mappings->trigger_pads[i].midi_device = -1;
    }

    while (fgets(line, sizeof(line), f)) {
        char *trimmed = trim(line);

        // Skip empty lines and comments
        if (trimmed[0] == '\0' || trimmed[0] == '#' || trimmed[0] == ';') continue;

        // Check for section headers
        if (trimmed[0] == '[') {
            if (strstr(trimmed, "[midi]")) section = SECTION_MIDI;
            else if (strstr(trimmed, "[keyboard]")) section = SECTION_KEYBOARD;
            else if (strstr(trimmed, "[trigger_pads]")) section = SECTION_TRIGGER_PADS;
            else section = SECTION_NONE;
            continue;
        }

        // Parse key=value pairs
        char *eq = strchr(trimmed, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = trim(trimmed);
        char *value = trim(eq + 1);

        if (section == SECTION_MIDI) {
            // Format: cc<number> = action[,parameter[,continuous[,device_id]]]
            if (strncmp(key, "cc", 2) == 0) {
                int cc = atoi(key + 2);
                char action_str[64];
                int param = 0, continuous = 0, device_id = -1;

                strncpy(action_str, value, sizeof(action_str) - 1);
                action_str[sizeof(action_str) - 1] = '\0';

                char *tok = strtok(action_str, ",");
                if (!tok) continue;

                char trimmed_tok[64];
                strncpy(trimmed_tok, tok, sizeof(trimmed_tok) - 1);
                trimmed_tok[sizeof(trimmed_tok) - 1] = '\0';
                InputAction action = parse_action(trim(trimmed_tok));

                tok = strtok(NULL, ",");
                if (tok) param = atoi(tok);

                tok = strtok(NULL, ",");
                if (tok) continuous = atoi(tok);

                tok = strtok(NULL, ",");
                if (tok) device_id = atoi(tok);

                // Threshold is automatically set based on continuous flag
                int threshold = continuous ? 0 : 64;

                // Add mapping if we have capacity
                if (mappings->midi_count < mappings->midi_capacity) {
                    mappings->midi_mappings[mappings->midi_count++] = (MidiMapping){
                        device_id, cc, action, param, threshold, continuous
                    };
                }
            }
        } else if (section == SECTION_KEYBOARD) {
            // Format: key<char/code> = action[,parameter]
            if (strncmp(key, "key", 3) == 0) {
                int keycode;
                if (key[3] == '_') {
                    // Special keys: key_space, key_esc, key_enter, etc.
                    if (strcmp(key + 4, "space") == 0) keycode = ' ';
                    else if (strcmp(key + 4, "esc") == 0) keycode = 27;
                    else if (strcmp(key + 4, "enter") == 0) keycode = '\n';
                    else if (strcmp(key + 4, "plus") == 0) keycode = '+';
                    else if (strcmp(key + 4, "minus") == 0) keycode = '-';
                    else if (strcmp(key + 4, "equals") == 0) keycode = '=';
                    else if (strcmp(key + 4, "lbracket") == 0) keycode = '[';
                    else if (strcmp(key + 4, "rbracket") == 0) keycode = ']';
                    else if (strcmp(key + 4, "pipe") == 0) keycode = '|';
                    else if (strcmp(key + 4, "backslash") == 0) keycode = '\\';
                    else if (strcmp(key + 4, "slash") == 0) keycode = '/';
                    else if (strcmp(key + 4, "comma") == 0) keycode = ',';
                    else if (strcmp(key + 4, "semicolon") == 0) keycode = ';';
                    else if (strcmp(key + 4, "hash") == 0) keycode = '#';
                    // Numpad keys (using special codes 159-168, GUI only)
                    else if (strncmp(key + 4, "kp", 2) == 0) {
                        int kpnum = atoi(key + 6);
                        if (kpnum >= 0 && kpnum <= 9) {
                            keycode = (kpnum == 0) ? 159 : (159 + kpnum); // KP0=159, KP1=160, ..., KP9=168
                        } else continue;
                    }
                    else continue;
                } else {
                    // Regular keys: key<char>
                    keycode = key[3];
                }

                char action_str[64];
                int param = 0;

                strncpy(action_str, value, sizeof(action_str) - 1);
                action_str[sizeof(action_str) - 1] = '\0';

                char *tok = strtok(action_str, ",");
                if (!tok) continue;

                char trimmed_tok[64];
                strncpy(trimmed_tok, tok, sizeof(trimmed_tok) - 1);
                trimmed_tok[sizeof(trimmed_tok) - 1] = '\0';
                InputAction action = parse_action(trim(trimmed_tok));

                tok = strtok(NULL, ",");
                if (tok) param = atoi(tok);

                // Add mapping if we have capacity
                if (mappings->keyboard_count < mappings->keyboard_capacity) {
                    mappings->keyboard_mappings[mappings->keyboard_count++] = (KeyboardMapping){
                        keycode, action, param
                    };
                }
            }
        } else if (section == SECTION_TRIGGER_PADS) {
            // Format: pad<number> = action[,parameter[,midi_note[,midi_device]]]
            if (strncmp(key, "pad", 3) == 0) {
                int pad_num = atoi(key + 3);
                if (pad_num < 1 || pad_num > MAX_TRIGGER_PADS) continue;
                int pad_idx = pad_num - 1; // Convert to 0-based index

                char action_str[64];
                int param = 0, midi_note = -1, midi_device = -1;

                strncpy(action_str, value, sizeof(action_str) - 1);
                action_str[sizeof(action_str) - 1] = '\0';

                char *tok = strtok(action_str, ",");
                if (!tok) continue;

                char trimmed_tok[64];
                strncpy(trimmed_tok, tok, sizeof(trimmed_tok) - 1);
                trimmed_tok[sizeof(trimmed_tok) - 1] = '\0';
                InputAction action = parse_action(trim(trimmed_tok));

                tok = strtok(NULL, ",");
                if (tok) param = atoi(tok);

                tok = strtok(NULL, ",");
                if (tok) midi_note = atoi(tok);

                tok = strtok(NULL, ",");
                if (tok) midi_device = atoi(tok);

                // Set trigger pad configuration
                mappings->trigger_pads[pad_idx].action = action;
                mappings->trigger_pads[pad_idx].parameter = param;
                mappings->trigger_pads[pad_idx].midi_note = midi_note;
                mappings->trigger_pads[pad_idx].midi_device = midi_device;
            }
        }
    }

    fclose(f);
    return 0;
}

int input_mappings_save(InputMappings *mappings, const char *filepath) {
    if (!mappings || !filepath) return -1;

    FILE *f = fopen(filepath, "w");
    if (!f) return -1;

    fprintf(f, "# Regroove Input Mappings Configuration\n\n");

    fprintf(f, "[midi]\n");
    fprintf(f, "# Format: cc<number> = action[,parameter[,continuous[,device_id]]]\n");
    fprintf(f, "# continuous: 1 for continuous controls (faders/knobs), 0 for buttons (default)\n");
    fprintf(f, "# device_id: -1 for any device (default), 0 for device 0, 1 for device 1\n");
    fprintf(f, "# Buttons trigger at MIDI value >= 64, continuous controls respond to all values\n\n");

    for (int i = 0; i < mappings->midi_count; i++) {
        MidiMapping *m = &mappings->midi_mappings[i];
        fprintf(f, "cc%d = %s,%d,%d,%d\n",
                m->cc_number,
                input_action_name(m->action),
                m->parameter,
                m->continuous,
                m->device_id);
    }

    fprintf(f, "\n[keyboard]\n");
    fprintf(f, "# Format: key<char> = action[,parameter]\n");
    fprintf(f, "# Special keys use key_<name> format (key_space, key_esc, key_enter)\n\n");

    for (int i = 0; i < mappings->keyboard_count; i++) {
        KeyboardMapping *k = &mappings->keyboard_mappings[i];
        const char *key_name;
        char key_buf[32];

        if (k->key == ' ') key_name = "key_space";
        else if (k->key == 27) key_name = "key_esc";
        else if (k->key == '\n' || k->key == '\r') key_name = "key_enter";
        else if (k->key == '+') key_name = "key_plus";
        else if (k->key == '-') key_name = "key_minus";
        else if (k->key == '=') key_name = "key_equals";
        else if (k->key == '[') key_name = "key_lbracket";
        else if (k->key == ']') key_name = "key_rbracket";
        else if (k->key == '|') key_name = "key_pipe";
        else if (k->key == '\\') key_name = "key_backslash";
        else if (k->key == '/') key_name = "key_slash";
        else if (k->key == ',') key_name = "key_comma";
        else if (k->key == ';') key_name = "key_semicolon";
        else if (k->key == '#') key_name = "key_hash";
        else {
            snprintf(key_buf, sizeof(key_buf), "key%c", k->key);
            key_name = key_buf;
        }

        fprintf(f, "%s = %s,%d\n",
                key_name,
                input_action_name(k->action),
                k->parameter);
    }

    fprintf(f, "\n[trigger_pads]\n");
    fprintf(f, "# Format: pad<number> = action[,parameter[,midi_note[,midi_device]]]\n");
    fprintf(f, "# midi_note: -1 = not mapped, 0-127 = MIDI note number\n");
    fprintf(f, "# midi_device: -1 = any device (default), 0 = device 0, 1 = device 1\n\n");

    for (int i = 0; i < MAX_TRIGGER_PADS; i++) {
        TriggerPadConfig *p = &mappings->trigger_pads[i];
        fprintf(f, "pad%d = %s,%d,%d,%d\n",
                i + 1,
                input_action_name(p->action),
                p->parameter,
                p->midi_note,
                p->midi_device);
    }

    fclose(f);
    return 0;
}

int input_mappings_get_midi_event(InputMappings *mappings, int device_id, int cc, int value, InputEvent *out_event) {
    if (!mappings || !out_event) return 0;

    for (int i = 0; i < mappings->midi_count; i++) {
        MidiMapping *m = &mappings->midi_mappings[i];
        // Match if CC matches and either device matches or mapping is for any device (-1)
        if (m->cc_number == cc && (m->device_id == -1 || m->device_id == device_id)) {
            // For continuous controls, always trigger
            // For buttons, check threshold
            if (m->continuous || value >= m->threshold) {
                out_event->action = m->action;
                out_event->parameter = m->parameter;
                out_event->value = value;
                return 1;
            }
        }
    }

    return 0;
}

int input_mappings_get_keyboard_event(InputMappings *mappings, int key, InputEvent *out_event) {
    if (!mappings || !out_event) return 0;

    for (int i = 0; i < mappings->keyboard_count; i++) {
        KeyboardMapping *k = &mappings->keyboard_mappings[i];
        if (k->key == key) {
            out_event->action = k->action;
            out_event->parameter = k->parameter;
            out_event->value = 0;
            return 1;
        }
    }

    return 0;
}
