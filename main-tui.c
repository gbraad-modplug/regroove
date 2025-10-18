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

static volatile int running = 1;
static struct termios orig_termios;

// --- Shared state ---
static RegrooveCommonState *common_state = NULL;

// --- Trigger pad configuration ---
#define MAX_TRIGGER_PADS 16
typedef struct {
    InputAction action;
    int parameter;
    int midi_note;   // MIDI note number that triggers this pad (-1 = not mapped)
    int midi_device; // Which MIDI device (-1 = any)
} TriggerPad;

static TriggerPad trigger_pads[MAX_TRIGGER_PADS];

// --- Trigger Pad Initialization ---
static void init_trigger_pads_defaults(void) {
    // Initialize all pads to no action
    for (int i = 0; i < MAX_TRIGGER_PADS; i++) {
        trigger_pads[i].action = ACTION_NONE;
        trigger_pads[i].parameter = 0;
        trigger_pads[i].midi_note = -1;
        trigger_pads[i].midi_device = -1;
    }

    // Set up default bindings (same as GUI)
    trigger_pads[0].action = ACTION_PLAY_PAUSE;         // P1: Play/Pause
    trigger_pads[1].action = ACTION_STOP;               // P2: Stop
    trigger_pads[2].action = ACTION_RETRIGGER;          // P3: Retrigger
    trigger_pads[3].action = ACTION_PATTERN_MODE_TOGGLE; // P4: Loop toggle

    trigger_pads[4].action = ACTION_PREV_ORDER;         // P5: Previous order
    trigger_pads[5].action = ACTION_NEXT_ORDER;         // P6: Next order
    trigger_pads[6].action = ACTION_HALVE_LOOP;         // P7: Halve loop
    trigger_pads[7].action = ACTION_FULL_LOOP;          // P8: Full loop

    // Pads 9-12: Channel mutes for first 4 channels
    for (int i = 0; i < 4; i++) {
        trigger_pads[8 + i].action = ACTION_CHANNEL_MUTE;
        trigger_pads[8 + i].parameter = i;
    }

    // Pads 13-16: Reserved for user configuration
    trigger_pads[12].action = ACTION_MUTE_ALL;          // P13: Mute all
    trigger_pads[13].action = ACTION_UNMUTE_ALL;        // P14: Unmute all
    trigger_pads[14].action = ACTION_LOOP_TILL_ROW;     // P15: Loop till row
    // P16 is unassigned (ACTION_NONE)
}

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

// --- CALLBACKS for UI feedback ---
static void my_order_callback(int order, int pattern, void *userdata) {
    printf("[ORDER] Now at Order %d (Pattern %d)\n", order, pattern);
}
static void my_row_callback(int order, int row, void *userdata) {
    //printf("[ROW] Order %d, Row %d\n", order, row);
}
static void my_loop_callback(int order, int pattern, void *userdata) {
    printf("[LOOP] Loop/retrigger at Order %d (Pattern %d)\n", order, pattern);
}

static void my_song_callback(void *userdata) {
    printf("[SONG] looped back to start\n");
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
    printf("\nPlayback paused (press SPACE or MIDI Play to start)\n");
    return 0;
}


// --- Unified Input Event Handler ---
void handle_input_event(InputEvent *event) {
    if (!event || event->action == ACTION_NONE) return;

    switch (event->action) {
        case ACTION_PLAY_PAUSE:
            regroove_common_play_pause(common_state, common_state->paused);
            printf("Playback %s\n", common_state->paused ? "paused" : "resumed");
            break;
        case ACTION_PLAY:
            if (common_state->paused) {
                regroove_common_play_pause(common_state, 1);
                printf("Playback resumed\n");
            }
            break;
        case ACTION_STOP:
            if (!common_state->paused) {
                regroove_common_play_pause(common_state, 0);
                printf("Playback paused\n");
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
            regroove_common_pattern_mode_toggle(common_state);
            if (common_state->player) {
                int new_mode = regroove_get_pattern_mode(common_state->player);
                if (new_mode)
                    printf("Pattern mode ON (looping pattern %d at order %d)\n",
                           regroove_get_current_pattern(common_state->player),
                           regroove_get_current_order(common_state->player));
                else
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
            if (event->parameter < common_state->num_channels) {
                regroove_common_channel_mute(common_state, event->parameter);
                if (common_state->player) {
                    int muted = regroove_is_channel_muted(common_state->player, event->parameter);
                    printf("Channel %d %s\n", event->parameter + 1, muted ? "muted" : "unmuted");
                }
            }
            break;
        case ACTION_CHANNEL_SOLO:
            if (common_state->player && event->parameter < common_state->num_channels) {
                regroove_toggle_channel_solo(common_state->player, event->parameter);
                printf("Channel %d solo toggled\n", event->parameter + 1);
            }
            break;
        case ACTION_CHANNEL_VOLUME:
            if (common_state->player && event->parameter < common_state->num_channels) {
                double vol = event->value / 127.0;
                regroove_set_channel_volume(common_state->player, event->parameter, vol);
            }
            break;
        case ACTION_TRIGGER_PAD:
            if (event->parameter >= 0 && event->parameter < MAX_TRIGGER_PADS) {
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

// --- MIDI HANDLING: uses unified control functions and InputMappings ---
void my_midi_mapping(unsigned char status, unsigned char cc_or_note, unsigned char value, int device_id, void *userdata) {
    (void)userdata;

    unsigned char msg_type = status & 0xF0;

    // Handle Note-On messages for trigger pads
    if (msg_type == 0x90 && value > 0) { // Note-On with velocity > 0
        int note = cc_or_note;

        // Check if any trigger pad is mapped to this MIDI note
        for (int i = 0; i < MAX_TRIGGER_PADS; i++) {
            // Match device (if specified) and note
            if (trigger_pads[i].midi_note == note &&
                (trigger_pads[i].midi_device == -1 || trigger_pads[i].midi_device == device_id)) {

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

    // Initialize trigger pads with defaults
    init_trigger_pads_defaults();

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
    if (SDL_OpenAudio(&spec, NULL) < 0) {
        fprintf(stderr, "SDL_OpenAudio failed: %s\n", SDL_GetError());
        regroove_common_destroy(common_state);
        SDL_Quit();
        return 1;
    }
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

    SDL_PauseAudio(1);

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
    SDL_PauseAudio(1);
    SDL_CloseAudio();

    regroove_common_destroy(common_state);

    SDL_Quit();
    return 0;
}