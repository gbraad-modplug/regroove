#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <SDL.h>
#include "regroove.h"

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
    printf("[ROW] Order %d, Row %d\n", order, row);
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

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s file.mod\n", argv[0]);
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

    // Start PAUSED
    SDL_PauseAudio(1);
    int paused = 1;

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
    printf("\nPlayback paused (press SPACE to start)\n");

    while (running) {
        int k = read_key_nonblocking();
        if (k != -1) {
            if (k == 27 || k == 'q' || k == 'Q') {
                running = 0;
            } else if (k == ' ') {
                paused = !paused;
                printf("Playback %s\n", paused ? "paused" : "resumed");
                SDL_PauseAudio(paused ? 1 : 0);
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
                regroove_mute_all(g);
                printf("All channels muted\n");
            } else if (k == 'u' || k == 'U') {
                regroove_unmute_all(g);
                printf("All channels unmuted\n");
            } else if (k == '+' || k == '=') {
                double new_pitch = regroove_get_pitch(g) * 1.05;
                regroove_set_pitch(g, new_pitch);
                printf("Pitch factor: %.2f\n", new_pitch);
            } else if (k == '-') {
                double new_pitch = regroove_get_pitch(g) / 1.05;
                regroove_set_pitch(g, new_pitch);
                printf("Pitch factor: %.2f\n", new_pitch);
            }
        }
        SDL_Delay(10);
    }

    SDL_CloseAudio();
    regroove_destroy(g);
    SDL_Quit();
    return 0;
}