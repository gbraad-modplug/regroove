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

static volatile int running = 1;
static struct termios orig_termios;

static Regroove *g_active = NULL;

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

#define MAX_FILES 4096
#define MAX_PATH 1024

typedef struct {
    char **names;
    int count;
    int current;
    char dirpath[MAX_PATH];
} FileList;

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

// --- SDL audio callback ---
static void audio_callback(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    Regroove *g = g_active;
    if (!g) return;
    int16_t *buffer = (int16_t *)stream;
    int frames = len / (2 * sizeof(int16_t));
    regroove_render_audio(g, buffer, frames);
}

typedef struct {
    Regroove *player;
    int paused;
    int num_channels;
    FileList *files;
    int files_active;
} MidiContext;

// --- Centralized module loader ---
static int load_module(const char *path, Regroove **g_ptr, SDL_AudioSpec *spec, struct RegrooveCallbacks *cbs, MidiContext *midi_ctx) {
    if (*g_ptr) { regroove_destroy(*g_ptr); *g_ptr = NULL; }
    Regroove *g = regroove_create(path, 48000.0);
    *g_ptr = g;
    if (midi_ctx) {
        midi_ctx->player = g;
        midi_ctx->num_channels = g ? regroove_get_num_channels(g) : 0;
        midi_ctx->paused = 1;
    }
    if (!g) {
        printf("Failed to load: %s\n", path);
        return -1;
    }
    printf("Loaded: %s\n", path);
    regroove_set_callbacks(g, cbs);
    SDL_LockAudio();
    g_active = g;
    SDL_UnlockAudio();
    SDL_PauseAudio(1);
    print_song_order(g);
    printf("\nPlayback paused (press SPACE to start)\n");
    return 0;
}

// --- MIDI HANDLING ---
void my_midi_mapping(unsigned char status, unsigned char cc, unsigned char value, void *userdata) {
    MidiContext *ctx = (MidiContext *)userdata;
    struct RegrooveCallbacks *cbs = NULL; // See note below
    SDL_AudioSpec *spec = NULL;
    // NOTE: if you want to update callbacks and spec from MIDI, pass them via MidiContext or make them global/static
    extern struct RegrooveCallbacks global_cbs;
    extern SDL_AudioSpec global_spec;
    cbs = &global_cbs;
    spec = &global_spec;

    if ((status & 0xF0) == 0xB0) { // Control Change
        // FILE BROWSER: CC61=prev, CC62=next, CC60=load
        if (ctx->files_active) {
            if (cc == 61 && value >= 64) {
                ctx->files->current--;
                if (ctx->files->current < 0) ctx->files->current = ctx->files->count - 1;
                printf("[MIDI] File: %s\n", ctx->files->names[ctx->files->current]);
            } else if (cc == 62 && value >= 64) {
                ctx->files->current++;
                if (ctx->files->current >= ctx->files->count) ctx->files->current = 0;
                printf("[MIDI] File: %s\n", ctx->files->names[ctx->files->current]);
            } else if (cc == 60 && value >= 64) {
                char path[MAX_PATH * 2];
                snprintf(path, sizeof(path), "%s/%s", ctx->files->dirpath, ctx->files->names[ctx->files->current]);
                load_module(path, &ctx->player, spec, cbs, ctx);
            }
            return;
        }
        // SOLO: CC32+ch (channels 0..7)
        if (cc >= 32 && cc < 32 + 8) {
            int ch = cc - 32;
            if (ch < ctx->num_channels && value >= 64) {
                regroove_toggle_channel_single(ctx->player, ch);
                printf("[MIDI] SOLO ch %d\n", ch);
            }
        }
        // MUTE: CC48+ch (channels 0..7)
        else if (cc >= 48 && cc < 48 + ctx->num_channels) {
            int ch = cc - 48;
            if (ch < ctx->num_channels && value >= 64) {
                regroove_toggle_channel_mute(ctx->player, ch);
                printf("[MIDI] MUTE toggle ch %d\n", ch);
            }
        }
        // VOLUME: CC0+ch (channels 0..7)
        else if (cc >= 0 && cc < ctx->num_channels) {
            double vol = value / 127.0;
            regroove_set_channel_volume(ctx->player, cc, vol);
            printf("[MIDI] Vol ch %d = %.2f\n", cc, vol);
        }
        // Global controls
        else switch (cc) {
            case 41: // Play
                if (ctx->paused) {
                    ctx->paused = 0;
                    SDL_PauseAudio(0);
                    printf("[MIDI] Play\n");
                }
                break;
            case 42: // Stop
                if (!ctx->paused) {
                    ctx->paused = 1;
                    SDL_PauseAudio(1);
                    printf("[MIDI] Stop\n");
                }
                break;
            case 46: // Loop pattern/mode toggle
                if (value >= 64) {
                    int mode = !regroove_get_pattern_mode(ctx->player);
                    regroove_pattern_mode(ctx->player, mode);
                    printf("[MIDI] Pattern mode %s\n", mode ? "ON" : "OFF");
                }
                break;
            case 44: // Next order
                if (value >= 64) {
                    regroove_queue_next_order(ctx->player);
                    printf("[MIDI] Next order\n");
                }
                break;
            case 43: // Prev order
                if (value >= 64) {
                    regroove_queue_prev_order(ctx->player);
                    printf("[MIDI] Prev order\n");
                }
                break;
        }
    }
}

struct RegrooveCallbacks global_cbs;
SDL_AudioSpec global_spec;

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
    Regroove *g = NULL;
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
    spec.userdata = g;
    global_spec = spec;

    if (!files_active) {
        if (load_module(initial_path, &g, &global_spec, &global_cbs, NULL) != 0) {
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

    struct RegrooveCallbacks cbs = {
        .on_order_change = my_order_callback,
        .on_row_change = my_row_callback,
        .on_pattern_loop = my_loop_callback,
        .userdata = NULL
    };
    global_cbs = cbs;
    if (g) regroove_set_callbacks(g, &global_cbs);

    MidiContext midi_ctx = { .player = g, .paused = 1, .num_channels = g ? regroove_get_num_channels(g) : 0, .files = &files, .files_active = files_active };
    int midi_ports = midi_list_ports();
    if (midi_ports > 0) {
        if (midi_init(my_midi_mapping, &midi_ctx, midi_port) != 0)
            printf("No MIDI available. Running with keyboard control only.\n");
    } else {
        printf("No MIDI available. Running with keyboard control only.\n");
    }

    SDL_PauseAudio(1);
    double pitch = g ? regroove_get_pitch(g) : 1.0;

    while (running) {
        int k = read_key_nonblocking();
        if (files_active) {
            // Directory navigation via keyboard: [ and ]
            if (k == '[') { // prev
                files.current--;
                if (files.current < 0) files.current = files.count - 1;
                printf("File: %s\n", files.names[files.current]);
            } else if (k == ']') { // next
                files.current++;
                if (files.current >= files.count) files.current = 0;
                printf("File: %s\n", files.names[files.current]);
            } else if (k == '\n' || k == '\r') { // ENTER = load
                char path[MAX_PATH * 2];
                snprintf(path, sizeof(path), "%s/%s", files.dirpath, files.names[files.current]);
                load_module(path, &g, &global_spec, &global_cbs, &midi_ctx);
            }
        }
        if (k != -1) {
            if (k == 27 || k == 'q' || k == 'Q') {
                running = 0;
            } else if (k == ' ') {
                if (g) {
                    midi_ctx.paused = !midi_ctx.paused;
                    printf("Playback %s\n", midi_ctx.paused ? "paused" : "resumed");
                    SDL_PauseAudio(midi_ctx.paused ? 1 : 0);
                }
            } else if (k == 'r' || k == 'R') {
                if (g) {
                    regroove_retrigger_pattern(g);
                    printf("Triggered retrigger.\n");
                }
            } else if (k == 'N' || k == 'n') {
                if (g) {
                    regroove_queue_next_order(g);
                    int next_order = regroove_get_current_order(g) + 1;
                    if (next_order < regroove_get_num_orders(g))
                        printf("Next order queued: Order %d (Pattern %d)\n",
                            next_order, regroove_get_order_pattern(g, next_order));
                }
            } else if (k == 'P' || k == 'p') {
                if (g) {
                    regroove_queue_prev_order(g);
                    int prev_order = regroove_get_current_order(g) > 0 ?
                        regroove_get_current_order(g) - 1 : 0;
                    printf("Previous order queued: Order %d (Pattern %d)\n",
                        prev_order, regroove_get_order_pattern(g, prev_order));
                }
            } else if (k == 'j' || k == 'J') {
                if (g) {
                    regroove_loop_till_row(g, regroove_get_current_row(g));
                    printf("Loop till row queued: Order %d, Row %d\n",
                        regroove_get_current_order(g), regroove_get_current_row(g));
                }
            } else if (k == 'h' || k == 'H') {
                if (g) {
                    int rows = regroove_get_custom_loop_rows(g) > 0 ?
                        regroove_get_custom_loop_rows(g) : regroove_get_full_pattern_rows(g);
                    int halved = rows / 2 < 1 ? 1 : rows / 2;
                    regroove_set_custom_loop_rows(g, halved);
                    printf("Loop length halved: %d rows\n", halved);
                }
            } else if (k == 'f' || k == 'F') {
                if (g) {
                    regroove_set_custom_loop_rows(g, 0);
                    printf("Loop length reset to full pattern: %d rows\n", regroove_get_full_pattern_rows(g));
                }
            } else if (k == 'S' || k == 's') {
                if (g) {
                    int new_mode = !regroove_get_pattern_mode(g);
                    regroove_pattern_mode(g, new_mode);
                    if (new_mode)
                        printf("Pattern mode ON (looping pattern %d at order %d)\n",
                            regroove_get_current_pattern(g), regroove_get_current_order(g));
                    else
                        printf("Song mode ON\n");
                }
            } else if (k >= '1' && k <= '9') {
                if (g) {
                    int ch = k - '1';
                    regroove_toggle_channel_mute(g, ch);
                    printf("Channel %d %s\n", ch + 1,
                        regroove_is_channel_muted(g, ch) ? "muted" : "unmuted");
                }
            } else if (k == 'm' || k == 'M') {
                if (g) {
                    regroove_mute_all(g);
                    printf("All channels muted\n");
                }
            } else if (k == 'u' || k == 'U') {
                if (g) {
                    regroove_unmute_all(g);
                    printf("All channels unmuted\n");
                }
            } else if (k == '+' || k == '=') {
                if (g) {
                    pitch += 1.01;
                    regroove_set_pitch(g, pitch);
                    printf("Pitch factor: %.2f\n", pitch);
                }
            } else if (k == '-') {
                if (g) {
                    pitch -= 1.01;
                    regroove_set_pitch(g, pitch);
                    printf("Pitch factor: %.2f\n", pitch);
                }
            }
        }
        if (g) regroove_process_commands(g);
        SDL_Delay(10);
    }

    midi_deinit();
    
    // Safely stop audio and destroy module
    SDL_PauseAudio(1);
    SDL_LockAudio();
    if (g_active) {
        Regroove *tmp = g_active;
        g_active = NULL;
        SDL_UnlockAudio();
        regroove_destroy(tmp);
    } else {
        SDL_UnlockAudio();
    }

    SDL_CloseAudio();
    free_filelist(&files);
    SDL_Quit();
    return 0;
}