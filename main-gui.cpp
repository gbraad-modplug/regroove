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
struct Channel {
    float volume = 1.0f;
    bool mute = false;
    bool solo = false;
};
static Channel channels[8];
static float pitch_slider = 0.0f; // -1.0 to 1.0, 0 = 1.0x pitch
static float step_fade[16] = {0.0f};
static int current_step = 0;
static bool loop_enabled = false;
static bool playing = false;
static int pattern = 1, order = 1, total_rows = 64;
static float loop_blink = 0.0f;

// Shared state
static RegrooveCommonState *common_state = NULL;

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
    int num_channels = regroove_get_num_channels(common_state->player);
    for (int i = 0; i < 8 && i < num_channels; ++i) {
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
    int num_channels = regroove_get_num_channels(mod);

    for (int i = 0; i < 16; ++i) step_fade[i] = 0.0f;

    for (int i = 0; i < 8 && i < num_channels; i++) {
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

    SDL_PauseAudio(1);
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
                SDL_PauseAudio(0);
                playing = true;
            }
            break;
        case ACT_STOP:
            if (mod) {
                SDL_PauseAudio(1);
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
            if (mod && arg1 >= 0 && arg1 < 8) {
                bool wasSolo = channels[arg1].solo;

                // Clear all solo states
                for (int i = 0; i < 8; ++i) channels[i].solo = false;

                if (!wasSolo) {
                    // New solo: set this channel solo, mute all, unmute this one
                    channels[arg1].solo = true;
                    regroove_mute_all(mod);
                    for (int i = 0; i < 8; ++i) channels[i].mute = true;
                    // Unmute soloed channel
                    regroove_toggle_channel_mute(mod, arg1);
                    channels[arg1].mute = false;
                } else {
                    // Un-solo: unmute all
                    regroove_unmute_all(mod);
                    for (int i = 0; i < 8; ++i) channels[i].mute = false;
                }
            }
            break;
        }
        case ACT_MUTE_CHANNEL: {
            if (mod && arg1 >= 0 && arg1 < 8) {
                // If soloed, un-solo and unmute all
                if (channels[arg1].solo) {
                    channels[arg1].solo = false;
                    regroove_unmute_all(mod);
                    for (int i = 0; i < 8; ++i) channels[i].mute = false;
                } else {
                    // Toggle mute just for this channel
                    channels[arg1].mute = !channels[arg1].mute;
                    regroove_toggle_channel_mute(mod, arg1);
                }
            }
            break;
        }
        case ACT_VOLUME_CHANNEL:
            if (mod && arg1 >= 0 && arg1 < 8) {
                regroove_set_channel_volume(mod, arg1, (double)arg2);
                channels[arg1].volume = arg2;
            }
            break;
        case ACT_MUTE_ALL:
            if (mod) {
                regroove_mute_all(mod);
                for (int i = 0; i < 8; ++i) {
                    channels[i].mute = true;
                    channels[i].solo = false;
                }
            }
            break;
        case ACT_UNMUTE_ALL:
            if (mod) {
                regroove_unmute_all(mod);
                for (int i = 0; i < 8; ++i) {
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
        default:
            break;
    }
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
        // Map numpad 1-9 to character codes
        key = '1' + (key - SDLK_KP_1);
    } else if (key == SDLK_KP_0) {
        // Map numpad 0 separately
        key = '0';
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

    // Query input mappings
    if (common_state && common_state->input_mappings) {
        InputEvent event;
        if (input_mappings_get_keyboard_event(common_state->input_mappings, key, &event)) {
            handle_input_event(&event);
        }
    }
}

void my_midi_mapping(unsigned char status, unsigned char cc, unsigned char value, void *userdata) {
    (void)userdata;

    // Only handle Control Change messages
    if ((status & 0xF0) != 0xB0) return;

    // Query input mappings
    if (common_state && common_state->input_mappings) {
        InputEvent event;
        if (input_mappings_get_midi_event(common_state->input_mappings, cc, value, &event)) {
            handle_input_event(&event);
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
    ImGui::Begin("MP-1210: Direct Interaction Groove Interface", nullptr, rootFlags);

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
            "Pat:%02d Ord:%02d Loop:%s\nPitch:%.2f\nFile:%.*s",
            pattern, order, loop_enabled ? "ON" : "OFF",
            MapPitchFader(pitch_slider),
            MAX_FILENAME_LEN, file_disp);

        DrawLCD(lcd, LEFT_PANEL_WIDTH - 16.0f, LCD_HEIGHT);
    }

    ImGui::Dummy(ImVec2(0, 8.0f));

    // File browser buttons
    ImGui::BeginGroup();
    if (ImGui::Button("<", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
        if (common_state && common_state->file_list) {
            regroove_filelist_prev(common_state->file_list);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("o", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
        if (common_state && common_state->file_list) {
            char path[COMMON_MAX_PATH * 2];
            regroove_filelist_get_current_path(common_state->file_list, path, sizeof(path));
            load_module(path);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(">", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) {
        if (common_state && common_state->file_list) {
            regroove_filelist_next(common_state->file_list);
        }
    }
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, 8.0f));

    // STOP BUTTON
    if (!playing) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.25f, 0.20f, 1.0f)); // red
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.35f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.50f, 0.15f, 0.15f, 1.0f));
        if (ImGui::Button("[]", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) dispatch_action(ACT_STOP);
        ImGui::PopStyleColor(3);
    } else {
        if (ImGui::Button("[]", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) dispatch_action(ACT_STOP);
    }

    ImGui::SameLine();
    
    // PLAY BUTTON
    if (playing) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.65f, 0.25f, 1.0f)); // green
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.80f, 0.35f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.50f, 0.20f, 1.0f));
        if (ImGui::Button("|>", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) dispatch_action(ACT_RETRIGGER);
        ImGui::PopStyleColor(3);
    } else {
        if (ImGui::Button("|>", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) dispatch_action(ACT_PLAY);
    }

    ImGui::Dummy(ImVec2(0, TRANSPORT_GAP));
    if (ImGui::Button("<<", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) dispatch_action(ACT_PREV_ORDER);
    ImGui::SameLine();
    if (ImGui::Button(">>", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) dispatch_action(ACT_NEXT_ORDER);
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
        if (ImGui::Button("O*", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) dispatch_action(ACT_TOGGLE_LOOP);
        ImGui::PopStyleColor(3);
    } else {
        if (ImGui::Button("O∞", ImVec2(BUTTON_SIZE, BUTTON_SIZE))) dispatch_action(ACT_TOGGLE_LOOP);
    }

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

    // 8 channel columns
    for (int i = 0; i < 8; ++i) {
        float colX = origin.x + i * (sliderW + spacing);
        ImGui::SetCursorPos(ImVec2(colX, origin.y + 8.0f));
        ImGui::BeginGroup();
        ImGui::Text("Ch%d", i + 1);
        ImGui::Dummy(ImVec2(0, 4.0f));

        // SOLO BUTTON
        ImVec4 soloCol = channels[i].solo ? ImVec4(0.80f,0.12f,0.14f,1.0f) : ImVec4(0.26f,0.27f,0.30f,1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, soloCol);
        if (ImGui::Button((std::string("S##solo")+std::to_string(i)).c_str(), ImVec2(sliderW, SOLO_SIZE)))
            dispatch_action(ACT_SOLO_CHANNEL, i);
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 6.0f));
        float prev_vol = channels[i].volume;
        if (ImGui::VSliderFloat((std::string("##vol")+std::to_string(i)).c_str(),
                                ImVec2(sliderW, sliderH),
                                &channels[i].volume, 0.0f, 1.0f, "")) {
            if (prev_vol != channels[i].volume)
                dispatch_action(ACT_VOLUME_CHANNEL, i, channels[i].volume);
        }

        ImGui::Dummy(ImVec2(0, 8.0f));

        // MUTE BUTTON with color feedback
        ImVec4 muteCol = channels[i].mute ? ImVec4(0.90f,0.16f,0.18f,1.0f) : ImVec4(0.26f,0.27f,0.30f,1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, muteCol);
        if (ImGui::Button((std::string("M##mute")+std::to_string(i)).c_str(), ImVec2(sliderW, MUTE_SIZE)))
            dispatch_action(ACT_MUTE_CHANNEL, i);
        ImGui::PopStyleColor();

        ImGui::EndGroup();
    }

    // 9th column: PITCH slider
    {
        float colX = origin.x + 8 * (sliderW + spacing);
        ImGui::SetCursorPos(ImVec2(colX, origin.y + 8.0f));
        ImGui::BeginGroup();
        ImGui::Text("Pitch");
        ImGui::Dummy(ImVec2(0, 4.0f));
        ImGui::Dummy(ImVec2(sliderW, SOLO_SIZE));
        ImGui::Dummy(ImVec2(0, 6.0f));
        float prev_pitch = pitch_slider;
        if (ImGui::VSliderFloat("##pitch", ImVec2(sliderW, sliderH),
                                &pitch_slider, -1.0f, 1.0f, "")) {
            if (prev_pitch != pitch_slider)
                dispatch_action(ACT_SET_PITCH, -1, pitch_slider);
        }
        ImGui::Dummy(ImVec2(0, 8.0f));
        if (ImGui::Button("R##pitch_reset", ImVec2(sliderW, MUTE_SIZE)))
            dispatch_action(ACT_PITCH_RESET);
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
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc)
            midi_port = atoi(argv[++i]);
        else if (!module_path) module_path = argv[i];
    }
    if (!module_path) {
        fprintf(stderr, "Usage: %s directory|file.mod [-m mididevice]\n", argv[0]);
        return 1;
    }

    // Create common state
    common_state = regroove_common_create();
    if (!common_state) {
        fprintf(stderr, "Failed to create common state\n");
        return 1;
    }

    // Load input mappings from regroove.ini
    if (regroove_common_load_mappings(common_state, "regroove.ini") != 0) {
        printf("No regroove.ini found, using default mappings\n");
    } else {
        printf("Loaded input mappings from regroove.ini\n");
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
        "regroove nanokontrol2 UI", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
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
    if (SDL_OpenAudio(&spec, NULL) < 0) return 1;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ApplyFlatBlackRedSkin();
    ImGui_ImplSDL2_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL2_Init();
    //if (load_module(module_path) != 0) return 1;
    int midi_ports = midi_list_ports();
    if (midi_ports > 0) midi_init(my_midi_mapping, NULL, midi_port);
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
    SDL_PauseAudio(1);
    SDL_CloseAudio();

    regroove_common_destroy(common_state);

    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}