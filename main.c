#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <SDL.h>
#include "regroove.h"

// RtMidi C API
#include <rtmidi_c.h>

static volatile int running = 1;
static struct termios orig_termios;

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
    Regroove *g = (Regroove *)userdata;
    int16_t *buffer = (int16_t *)stream;
    int frames = len / (2 * sizeof(int16_t));
    regroove_render_audio(g, buffer, frames);
}

// --- MIDI HANDLING ---

typedef struct {
    Regroove *player;
    int paused;
    int num_channels;
} MidiContext;

static void midi_cc_action(MidiContext *ctx, unsigned char cc, unsigned char value) {
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
    // ... (other global controls: play, stop, loop, next, prev) ...
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

static void midi_callback(double deltatime, const unsigned char *msg, size_t sz, void *userdata) {
    MidiContext *ctx = (MidiContext *)userdata;
    if (sz < 3) return;
    unsigned char status = msg[0] & 0xF0;
    unsigned char cc = msg[1];
    unsigned char value = msg[2];
    if (status == 0xB0) { // Control Change
        midi_cc_action(ctx, cc, value);
    }
}

// --- MIDI SETUP ---
static RtMidiInPtr open_midi_port(MidiContext *ctx, int which_port) {
    // Defensive: skip MIDI if ALSA sequencer is missing
    if (access("/dev/snd/seq", F_OK) == -1) {
        fprintf(stderr, "No /dev/snd/seq found. Skipping MIDI initialization.\n");
        return NULL;
    }
    RtMidiInPtr midiin = rtmidi_in_create_default();
    if (!midiin) {
        fprintf(stderr, "No MIDI system available (rtmidi_in_create_default failed).\n");
        return NULL;
    }
    unsigned int nports = 0;
    nports = rtmidi_get_port_count(midiin);
    if (nports == 0) {
        fprintf(stderr, "No MIDI input devices found. MIDI disabled.\n");
        rtmidi_in_free(midiin);
        return NULL;
    }
    for (unsigned int i = 0; i < nports; ++i) {
        char name[128];
        int bufsize = sizeof(name);
        rtmidi_get_port_name(midiin, i, name, &bufsize);
        printf("  [%u] %s\n", i, name);
    }
    if (which_port < 0 || which_port >= (int)nports) {
        fprintf(stderr, "Invalid MIDI port %d, using 0.\n", which_port);
        which_port = 0;
    }
    rtmidi_open_port(midiin, which_port, "regroove-midi-in");
    rtmidi_in_set_callback(midiin, midi_callback, ctx);
    rtmidi_in_ignore_types(midiin, 0, 0, 0);
    printf("Opened MIDI port [%d]\n", which_port);
    return midiin;
}

int main(int argc, char *argv[]) {
    int midi_port = -1;
    // Parse arguments for -m <port>
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc) {
            midi_port = atoi(argv[++i]);
        }
    }
    if (argc < 2) {
        fprintf(stderr, "Usage: %s file.mod [-m mididevice]\n", argv[0]);
        return 1;
    }
    Regroove *g = regroove_create(argv[1], 48000.0);
    if (!g) { fprintf(stderr, "Failed to load module\n"); return 1; }

    printf("Song order list (%d entries):\n", regroove_get_num_orders(g));
    for (int ord = 0; ord < regroove_get_num_orders(g); ++ord) {
        int pat = regroove_get_order_pattern(g, ord);
        printf("  Order %2d -> Pattern %2d\n", ord, pat);
    }
    printf("--------------------------------------\n");

    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        regroove_destroy(g);
        return 1;
    }
    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.freq = 48000;
    spec.format = AUDIO_S16SYS;
    spec.channels = 2;
    spec.samples = 256;
    spec.callback = audio_callback;
    spec.userdata = g;
    if (SDL_OpenAudio(&spec, NULL) < 0) {
        fprintf(stderr, "SDL_OpenAudio failed: %s\n", SDL_GetError());
        regroove_destroy(g);
        SDL_Quit();
        return 1;
    }
    signal(SIGINT, handle_sigint);

    tty_make_raw_nonblocking();

    // Setup UI callbacks for feedback
    struct RegrooveCallbacks cbs = {
        .on_order_change = my_order_callback,
        .on_row_change = my_row_callback,
        .on_pattern_loop = my_loop_callback,
        .userdata = NULL
    };
    regroove_set_callbacks(g, &cbs);

    // Setup MIDI
    MidiContext midi_ctx = { .player = g, .paused = 1, .num_channels = regroove_get_num_channels(g) };
    RtMidiInPtr midiin = open_midi_port(&midi_ctx, midi_port);

    if (!midiin)
        printf("No MIDI available. Running with keyboard control only.\n");

    // Start PAUSED
    SDL_PauseAudio(1);

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
    printf("  M switch MIDI port (runtime)\n");
    printf("\nPlayback paused (press SPACE to start)\n");

    while (running) {
        int k = read_key_nonblocking();
        if (k != -1) {
            if (k == 27 || k == 'q' || k == 'Q') {
                running = 0;
            } else if (k == ' ') {
                midi_ctx.paused = !midi_ctx.paused;
                printf("Playback %s\n", midi_ctx.paused ? "paused" : "resumed");
                SDL_PauseAudio(midi_ctx.paused ? 1 : 0);
            } else if (k == 'r' || k == 'R') {
                regroove_retrigger_pattern(g);
                printf("Triggered retrigger.\n");
            } else if (k == 'N' || k == 'n') {
                regroove_queue_next_order(g);
                int next_order = regroove_get_current_order(g) + 1;
                if (next_order < regroove_get_num_orders(g))
                    printf("Next order queued: Order %d (Pattern %d)\n",
                        next_order, regroove_get_order_pattern(g, next_order));
            } else if (k == 'P' || k == 'p') {
                regroove_queue_prev_order(g);
                int prev_order = regroove_get_current_order(g) > 0 ?
                    regroove_get_current_order(g) - 1 : 0;
                printf("Previous order queued: Order %d (Pattern %d)\n",
                    prev_order, regroove_get_order_pattern(g, prev_order));
            } else if (k == 'j' || k == 'J') {
                regroove_loop_till_row(g, regroove_get_current_row(g));
                printf("Loop till row queued: Order %d, Row %d\n",
                       regroove_get_current_order(g), regroove_get_current_row(g));
            } else if (k == 'h' || k == 'H') {
                int rows = regroove_get_custom_loop_rows(g) > 0 ?
                    regroove_get_custom_loop_rows(g) : regroove_get_full_pattern_rows(g);
                int halved = rows / 2 < 1 ? 1 : rows / 2;
                regroove_set_custom_loop_rows(g, halved);
                printf("Loop length halved: %d rows\n", halved);
            } else if (k == 'f' || k == 'F') {
                regroove_set_custom_loop_rows(g, 0);
                printf("Loop length reset to full pattern: %d rows\n", regroove_get_full_pattern_rows(g));
            } else if (k == 'S' || k == 's') {
                int new_mode = !regroove_get_pattern_mode(g);
                regroove_pattern_mode(g, new_mode);
                if (new_mode)
                    printf("Pattern mode ON (looping pattern %d at order %d)\n",
                           regroove_get_current_pattern(g), regroove_get_current_order(g));
                else
                    printf("Song mode ON\n");
            } else if (k >= '1' && k <= '9') {
                int ch = k - '1';
                regroove_toggle_channel_mute(g, ch);
                printf("Channel %d %s\n", ch + 1,
                    regroove_is_channel_muted(g, ch) ? "muted" : "unmuted");
            } else if (k == 'm' || k == 'M') {
                // For runtime MIDI port switch:
                if (midiin) {
                    printf("Switching MIDI port: ");
                    RtMidiInPtr newmidi = NULL;
                    unsigned int nports = rtmidi_get_port_count(midiin);
                    for (unsigned int i = 0; i < nports; ++i) {
                        char name[128];
                        int bufsize = sizeof(name);
                        rtmidi_get_port_name(midiin, i, name, &bufsize);
                        printf("[%u] %s  ", i, name);
                    }
                    printf("\nEnter MIDI port number: ");
                    fflush(stdout);
                    int port = 0;
                    scanf("%d", &port);
                    rtmidi_in_free(midiin);
                    midiin = open_midi_port(&midi_ctx, port);
                } else {
                    regroove_mute_all(g);
                    printf("All channels muted\n");
                }
            } else if (k == 'u' || k == 'U') {
                regroove_unmute_all(g);
                printf("All channels unmuted\n");
            } else if (k == '+' || k == '=') {
                double new_pitch = regroove_get_pitch(g) + .01;
                regroove_set_pitch(g, new_pitch);
                regroove_process_commands(g);
                printf("Pitch factor: %.2f\n", new_pitch);
            } else if (k == '-') {
                double new_pitch = regroove_get_pitch(g) - .01;
                regroove_set_pitch(g, new_pitch);
                regroove_process_commands(g);
                printf("Pitch factor: %.2f\n", new_pitch);
            }
        }
        SDL_Delay(10);
    }

    if (midiin) rtmidi_in_free(midiin);
    SDL_CloseAudio();
    regroove_destroy(g);
    SDL_Quit();
    return 0;
}