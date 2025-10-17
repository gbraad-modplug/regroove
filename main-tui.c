#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include <SDL.h>
#include "regroove.h"
#include "midi.h"
#include "input_mappings.h"

#define MAX_FILES 4096
#define MAX_PATH 1024

typedef struct {
    char **names;
    int count;
    int current;
    char dirpath[MAX_PATH];
} FileList;

static volatile int running = 1;
static struct termios orig_termios;

// --- Only one Regroove instance in the app ---
static Regroove *g = NULL;

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

void free_filelist(FileList *fl) {
    if (!fl) return;
    if (fl->names) {
        for (int i = 0; i < fl->count; ++i) free(fl->names[i]);
        free(fl->names);
    }
    fl->names = NULL;
    fl->count = 0;
    fl->current = 0;
}

int ends_with_module(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot) return 0;
    char ext[16];
    snprintf(ext, sizeof(ext), "%s", dot);
    for (char *p = ext; *p; ++p) *p = tolower(*p);
    return (
        strcmp(ext, ".mod") == 0  || strcmp(ext, ".xm") == 0   ||
        strcmp(ext, ".s3m") == 0  || strcmp(ext, ".it") == 0   ||
        strcmp(ext, ".med") == 0  || strcmp(ext, ".mmd") == 0  ||
        strcmp(ext, ".mmd0") == 0 || strcmp(ext, ".mmd1") == 0 ||
        strcmp(ext, ".mmd2") == 0 || strcmp(ext, ".mmd3") == 0 ||
        strcmp(ext, ".mmdc") == 0
    );
}

int load_filelist(FileList *fl, const char *dirpath) {
    DIR *dir = opendir(dirpath);
    if (!dir) return -1;
    struct dirent *entry;
    fl->names = (char**)malloc(MAX_FILES * sizeof(char*));
    fl->count = 0;
    fl->current = 0;
    snprintf(fl->dirpath, MAX_PATH, "%s", dirpath);
    while ((entry = readdir(dir)) != NULL && fl->count < MAX_FILES) {
        if ((entry->d_type == DT_REG || entry->d_type == DT_UNKNOWN) && ends_with_module(entry->d_name)) {
            fl->names[fl->count++] = strdup(entry->d_name);
        }
    }
    closedir(dir);
    return fl->count;
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
    if (!g) return;
    int16_t *buffer = (int16_t *)stream;
    int frames = len / (2 * sizeof(int16_t));
    regroove_render_audio(g, buffer, frames);
}

// --- Only ONE MidiContext, only referencing g and not its own player ---
typedef struct {
    int paused;
    int num_channels;
    FileList *files;
    int files_active;
    InputMappings *input_mappings;
} MidiContext;
static MidiContext midi_ctx = {0};

// --- Only one set of global callbacks and spec ---
struct RegrooveCallbacks global_cbs;
SDL_AudioSpec global_spec;

// --- Global pitch control ---
static double global_pitch = 1.0;

// --- Centralized module loader ---
static int load_module(const char *path, SDL_AudioSpec *spec, struct RegrooveCallbacks *cbs) {
    SDL_LockAudio();
    if (g) {
        Regroove *old = g;
        g = NULL;
        SDL_UnlockAudio();
        regroove_destroy(old);
    } else {
        SDL_UnlockAudio();
    }
    Regroove *mod = regroove_create(path, 48000.0);
    if (!mod) {
        printf("Failed to load: %s\n", path);
        return -1;
    }
    SDL_LockAudio();
    g = mod;
    SDL_UnlockAudio();

    midi_ctx.num_channels = regroove_get_num_channels(mod);
    midi_ctx.paused = 1;

    regroove_set_callbacks(mod, cbs);
    SDL_PauseAudio(1);
    print_song_order(mod);
    printf("\nPlayback paused (press SPACE or MIDI Play to start)\n");
    return 0;
}

// --- Unified playback/command control using g only ---
void control_play_pause(int play) {
    if (!g) return;

    midi_ctx.paused = !play ? 1 : 0;
    SDL_PauseAudio(!play ? 1 : 0);
    printf("Playback %s\n", play ? "resumed" : "paused");
}
void control_retrigger(void) {
    if (!g) return;

    regroove_retrigger_pattern(g);
    printf("Triggered retrigger.\n");
}
void control_next_order(void) {
    if (!g) return;
    
    regroove_queue_next_order(g);
    int next_order = regroove_get_current_order(g) + 1;
    if (next_order < regroove_get_num_orders(g))
        printf("Next order queued: Order %d (Pattern %d)\n",
            next_order, regroove_get_order_pattern(g, next_order));
}
void control_prev_order(void) {
    if (!g) return;

    regroove_queue_prev_order(g);
    int prev_order = regroove_get_current_order(g) > 0 ?
            regroove_get_current_order(g) - 1 : 0;
        printf("Previous order queued: Order %d (Pattern %d)\n",
            prev_order, regroove_get_order_pattern(g, prev_order));
}
void control_loop_till_row(void) {
    if (!g) return;

    regroove_loop_till_row(g, regroove_get_current_row(g));
    printf("Loop till row queued: Order %d, Row %d\n",
        regroove_get_current_order(g), regroove_get_current_row(g));
}
void control_halve_loop(void) {
    if (!g) return;

    int rows = regroove_get_custom_loop_rows(g) > 0 ?
        regroove_get_custom_loop_rows(g) : regroove_get_full_pattern_rows(g);
    int halved = rows / 2 < 1 ? 1 : rows / 2;
    regroove_set_custom_loop_rows(g, halved);
    printf("Loop length halved: %d rows\n", halved);
}
void control_full_loop(void) {
    if (!g) return;

    regroove_set_custom_loop_rows(g, 0);
    printf("Loop length reset to full pattern: %d rows\n", regroove_get_full_pattern_rows(g));
}
void control_pattern_mode_toggle(void) {
    if (!g) return;

    int new_mode = !regroove_get_pattern_mode(g);
    regroove_pattern_mode(g, new_mode);
    if (new_mode)
        printf("Pattern mode ON (looping pattern %d at order %d)\n",
               regroove_get_current_pattern(g), regroove_get_current_order(g));
    else
        printf("Song mode ON\n");
}
void control_channel_mute(int ch) {
    if (!g) return;

    regroove_toggle_channel_mute(g, ch);
    int muted = regroove_is_channel_muted(g, ch); // always ask the engine!
    printf("Channel %d %s\n", ch + 1, muted ? "muted" : "unmuted");
}
void control_mute_all(void) {
    if (!g) return;

    regroove_mute_all(g);
    printf("All channels muted\n");
}
void control_unmute_all(void) {
    if (!g) return
    regroove_unmute_all(g);
    printf("All channels unmuted\n");
}
void control_pitch_up(void) {
    if (!g) return;

    if (global_pitch < 3.0) global_pitch += 0.01;
    regroove_set_pitch(g, global_pitch);
    printf("Pitch factor: %.2f\n", global_pitch);
}
void control_pitch_down(void) {
    if (!g) return;

    if (global_pitch > 0.25) global_pitch -= 0.01;
    regroove_set_pitch(g, global_pitch);
    printf("Pitch factor: %.2f\n", global_pitch);
}

// --- Unified Input Event Handler ---
void handle_input_event(InputEvent *event) {
    if (!event || event->action == ACTION_NONE) return;

    switch (event->action) {
        case ACTION_PLAY_PAUSE:
            control_play_pause(midi_ctx.paused);
            break;
        case ACTION_PLAY:
            if (midi_ctx.paused) control_play_pause(1);
            break;
        case ACTION_STOP:
            if (!midi_ctx.paused) control_play_pause(0);
            break;
        case ACTION_RETRIGGER:
            control_retrigger();
            break;
        case ACTION_NEXT_ORDER:
            control_next_order();
            break;
        case ACTION_PREV_ORDER:
            control_prev_order();
            break;
        case ACTION_LOOP_TILL_ROW:
            control_loop_till_row();
            break;
        case ACTION_HALVE_LOOP:
            control_halve_loop();
            break;
        case ACTION_FULL_LOOP:
            control_full_loop();
            break;
        case ACTION_PATTERN_MODE_TOGGLE:
            control_pattern_mode_toggle();
            break;
        case ACTION_MUTE_ALL:
            control_mute_all();
            break;
        case ACTION_UNMUTE_ALL:
            control_unmute_all();
            break;
        case ACTION_PITCH_UP:
            control_pitch_up();
            break;
        case ACTION_PITCH_DOWN:
            control_pitch_down();
            break;
        case ACTION_QUIT:
            running = 0;
            break;
        case ACTION_FILE_PREV:
            if (midi_ctx.files_active) {
                midi_ctx.files->current--;
                if (midi_ctx.files->current < 0)
                    midi_ctx.files->current = midi_ctx.files->count - 1;
                printf("File: %s\n", midi_ctx.files->names[midi_ctx.files->current]);
            }
            break;
        case ACTION_FILE_NEXT:
            if (midi_ctx.files_active) {
                midi_ctx.files->current++;
                if (midi_ctx.files->current >= midi_ctx.files->count)
                    midi_ctx.files->current = 0;
                printf("File: %s\n", midi_ctx.files->names[midi_ctx.files->current]);
            }
            break;
        case ACTION_FILE_LOAD:
            if (midi_ctx.files_active) {
                char path[MAX_PATH * 2];
                snprintf(path, sizeof(path), "%s/%s",
                        midi_ctx.files->dirpath,
                        midi_ctx.files->names[midi_ctx.files->current]);
                load_module(path, &global_spec, &global_cbs);
            }
            break;
        case ACTION_CHANNEL_MUTE:
            if (event->parameter < midi_ctx.num_channels) {
                control_channel_mute(event->parameter);
            }
            break;
        case ACTION_CHANNEL_SOLO:
            if (g && event->parameter < midi_ctx.num_channels) {
                regroove_toggle_channel_solo(g, event->parameter);
                printf("Channel %d solo toggled\n", event->parameter + 1);
            }
            break;
        case ACTION_CHANNEL_VOLUME:
            if (g && event->parameter < midi_ctx.num_channels) {
                double vol = event->value / 127.0;
                regroove_set_channel_volume(g, event->parameter, vol);
            }
            break;
        default:
            break;
    }
}

// --- MIDI HANDLING: uses unified control functions and InputMappings ---
void my_midi_mapping(unsigned char status, unsigned char cc, unsigned char value, void *userdata) {
    MidiContext *ctx = (MidiContext *)userdata;

    // Only handle Control Change messages
    if ((status & 0xF0) != 0xB0) return;

    // Query input mappings
    InputEvent event;
    if (ctx->input_mappings && input_mappings_get_midi_event(ctx->input_mappings, cc, value, &event)) {
        handle_input_event(&event);
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
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc) {
            midi_port = atoi(argv[++i]);
        }
    }
    if (argc < 2) {
        fprintf(stderr, "Usage: %s file.mod|dir [-m mididevice]\n", argv[0]);
        return 1;
    }

    FileList files = {0};
    int files_active = 0;
    char *initial_path = argv[1];
    if (is_directory(initial_path)) {
        if (load_filelist(&files, initial_path) > 0) {
            files_active = 1;
            printf("Loaded %d files from directory: %s\n", files.count, files.dirpath);
            printf("Use CC61/CC62 or [ and ] to select, CC60 or ENTER to load\n");
        } else {
            printf("Could not load directory or no files found: %s\n", initial_path);
            free_filelist(&files);
            return 1;
        }
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
    if (files_active) {
        printf("  [ and ] to select file, ENTER to load\n");
        printf("  (or CC61/CC62/CC60 via MIDI)\n");
    }
    printf("\n");

    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.freq = 48000;
    spec.format = AUDIO_S16SYS;
    spec.channels = 2;
    spec.samples = 256;
    spec.callback = audio_callback;
    spec.userdata = NULL;
    global_spec = spec;

    // Initialize input mappings
    InputMappings *input_mappings = input_mappings_create();
    if (!input_mappings) {
        fprintf(stderr, "Failed to create input mappings\n");
        return 1;
    }

    // Try to load custom mappings from regroove.ini
    if (input_mappings_load(input_mappings, "regroove.ini") != 0) {
        printf("No regroove.ini found, using default mappings\n");
        input_mappings_reset_defaults(input_mappings);
    } else {
        printf("Loaded input mappings from regroove.ini\n");
    }

    midi_ctx.files = &files;
    midi_ctx.files_active = files_active;
    midi_ctx.paused = 1;
    midi_ctx.num_channels = 0;
    midi_ctx.input_mappings = input_mappings;

    struct RegrooveCallbacks cbs = {
        .on_order_change = my_order_callback,
        .on_row_change = my_row_callback,
        .on_loop_pattern = my_loop_callback,
        .on_loop_song = my_song_callback,
        .userdata = NULL
    };
    global_cbs = cbs;

    if (!files_active) {
        if (load_module(initial_path, &global_spec, &global_cbs) != 0) {
            return 1;
        }
    }

    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        if (g) regroove_destroy(g);
        return 1;
    }
    if (SDL_OpenAudio(&global_spec, NULL) < 0) {
        fprintf(stderr, "SDL_OpenAudio failed: %s\n", SDL_GetError());
        if (g) regroove_destroy(g);
        SDL_Quit();
        return 1;
    }
    signal(SIGINT, handle_sigint);

    tty_make_raw_nonblocking();

    int midi_ports = midi_list_ports();
    if (midi_ports > 0) {
        if (midi_init(my_midi_mapping, &midi_ctx, midi_port) != 0)
            printf("No MIDI available. Running with keyboard control only.\n");
    } else {
        printf("No MIDI available. Running with keyboard control only.\n");
    }

    SDL_PauseAudio(1);

    while (running) {
        int k = read_key_nonblocking();
        if (k != -1) {
            // Query input mappings for keyboard event
            InputEvent event;
            if (input_mappings_get_keyboard_event(input_mappings, k, &event)) {
                handle_input_event(&event);
            }
        }
        if (g) regroove_process_commands(g);
        SDL_Delay(10);
    }

    midi_deinit();

    // Safely stop audio and destroy module
    SDL_PauseAudio(1);
    SDL_LockAudio();
    if (g) {
        Regroove *tmp = g;
        g = NULL;
        SDL_UnlockAudio();
        regroove_destroy(tmp);
    } else {
        SDL_UnlockAudio();
    }

    SDL_CloseAudio();
    input_mappings_destroy(input_mappings);
    free_filelist(&files);
    SDL_Quit();
    return 0;
}