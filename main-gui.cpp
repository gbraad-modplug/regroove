#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl2.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
    #include "regroove.h"
    #include "midi.h"
}

// Clamp helper
template<typename T>
static inline T Clamp(T v, T lo, T hi) { return (v < lo ? lo : (v > hi ? hi : v)); }

// -----------------------------------------------------------------------------
// App State
// -----------------------------------------------------------------------------
struct Channel {
    float volume = 0.7f;
    bool mute = false;
    bool solo = false;
};
static Channel channels[8];
static float pitch_slider = 0.0f; // -1.0 to 1.0, 0 = 1.0x pitch
static bool sequencer_pos[16] = {false};
static int current_step = 0;
static float step_fade[16] = {0.0f};
static bool loop_enabled = false;
static bool playing = false;
static int pattern = 1;
static int order = 1;
static int total_rows = 64;

// Global Regroove instance
static Regroove *g_regroove = NULL;

// -----------------------------------------------------------------------------
// Callbacks
// -----------------------------------------------------------------------------
static void my_row_callback(int ord, int row, void *userdata) {
    (void)userdata;
    if (total_rows <= 0) return;
    
    // Calculate which step indicator should light up (0-15)
    int rows_per_step = total_rows / 16;
    if (rows_per_step < 1) rows_per_step = 1;
    
    current_step = row / rows_per_step;
    if (current_step >= 16) current_step = 15;
    
    // Set current step to full brightness
    step_fade[current_step] = 1.0f;
}

static void my_order_callback(int ord, int pat, void *userdata) {
    (void)userdata;
    order = ord;
    pattern = pat;
    if (g_regroove) {
        total_rows = regroove_get_full_pattern_rows(g_regroove);
    }
}

// -----------------------------------------------------------------------------
// Control Functions
// -----------------------------------------------------------------------------
void control_play_pause(int play) {
    if (!g_regroove) return;
    SDL_PauseAudio(!play ? 1 : 0);
    playing = play;
    printf("Playback %s\n", play ? "resumed" : "paused");
}

void control_retrigger() {
    if (!g_regroove) return;
    regroove_retrigger_pattern(g_regroove);
    printf("Triggered retrigger.\n");
}

void control_next_order() {
    if (!g_regroove) return;
    regroove_queue_next_order(g_regroove);
}

void control_prev_order() {
    if (!g_regroove) return;
    regroove_queue_prev_order(g_regroove);
}

void control_pattern_mode_toggle() {
    if (!g_regroove) return;
    int new_mode = !regroove_get_pattern_mode(g_regroove);
    regroove_pattern_mode(g_regroove, new_mode);
    loop_enabled = new_mode;
}

void control_channel_mute(int ch) {
    if (!g_regroove || ch < 0 || ch >= 8) return;
    regroove_toggle_channel_mute(g_regroove, ch);
    channels[ch].mute = regroove_is_channel_muted(g_regroove, ch);
}

void control_channel_solo(int ch) {
    if (!g_regroove || ch < 0 || ch >= 8) return;
    regroove_toggle_channel_solo(g_regroove, ch);
    channels[ch].solo = !channels[ch].solo;
}

void control_channel_volume(int ch, float vol) {
    if (!g_regroove || ch < 0 || ch >= 8) return;
    regroove_set_channel_volume(g_regroove, ch, (double)vol);
    channels[ch].volume = vol;
}

void control_pitch(float slider_val) {
    if (!g_regroove) return;
    // Map -1..1 to 0.5..2.0 pitch range
    double pitch = 1.0 + slider_val; // -1 -> 0, 0 -> 1, 1 -> 2
    if (pitch < 0.5) pitch = 0.5;
    if (pitch > 2.0) pitch = 2.0;
    regroove_set_pitch(g_regroove, pitch);
}

void control_pitch_reset() {
    pitch_slider = 0.0f;
    control_pitch(0.0f);
}

void control_set_loop_rows(int step_index) {
    if (!g_regroove) return;
    
    if (step_index == 15) {
        // Last step = full pattern
        regroove_set_custom_loop_rows(g_regroove, 0);
        printf("Loop reset to full pattern (%d rows)\n", total_rows);
    } else {
        // Calculate rows for this step
        int rows_per_step = total_rows / 16;
        if (rows_per_step < 1) rows_per_step = 1;
        int loop_rows = (step_index + 1) * rows_per_step;
        regroove_set_custom_loop_rows(g_regroove, loop_rows);
        printf("Loop set to %d rows (step %d)\n", loop_rows, step_index + 1);
    }
}

// -----------------------------------------------------------------------------
// Audio Callback
// -----------------------------------------------------------------------------
static void audio_callback(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    if (!g_regroove) {
        memset(stream, 0, len);
        return;
    }
    int16_t *buffer = (int16_t *)stream;
    int frames = len / (2 * sizeof(int16_t));
    regroove_render_audio(g_regroove, buffer, frames);
}

// -----------------------------------------------------------------------------
// Module Loading
// -----------------------------------------------------------------------------
static int load_module(const char *path) {
    SDL_LockAudio();
    if (g_regroove) {
        Regroove *old = g_regroove;
        g_regroove = NULL;
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
    g_regroove = mod;
    SDL_UnlockAudio();

    // Update UI state
    int num_channels = regroove_get_num_channels(mod);
    for (int i = 0; i < 8 && i < num_channels; i++) {
        channels[i].volume = 0.7f;
        channels[i].mute = false;
        channels[i].solo = false;
    }

    order = regroove_get_current_order(mod);
    pattern = regroove_get_current_pattern(mod);
    total_rows = regroove_get_full_pattern_rows(mod);

    // Set callbacks
    struct RegrooveCallbacks cbs = {
        .on_order_change = my_order_callback,
        .on_row_change = my_row_callback,
        .on_pattern_loop = NULL,
        .userdata = NULL
    };
    regroove_set_callbacks(mod, &cbs);

    SDL_PauseAudio(1);
    playing = false;
    printf("Loaded: %s (%d orders, %d channels, %d rows)\n", 
           path, regroove_get_num_orders(mod), num_channels, total_rows);
    return 0;
}

// -----------------------------------------------------------------------------
// Theme
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

// LCD
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

// Square button
static void SquareButton(const char* label, const char* tooltip=nullptr,
                         bool* toggle=nullptr, ImVec2 size=ImVec2(48,48),
                         bool* outClicked=nullptr)
{
    if (toggle) {
        bool active = *toggle;
        ImVec4 btnCol = active ? ImVec4(0.55f,0.08f,0.09f,1.0f) : ImVec4(0.20f,0.20f,0.23f,1.0f);
        ImVec4 hovCol = active ? ImVec4(0.75f,0.18f,0.19f,1.0f) : ImVec4(0.28f,0.28f,0.32f,1.0f);
        ImVec4 actCol = active ? ImVec4(0.43f,0.08f,0.09f,1.0f) : ImVec4(0.15f,0.15f,0.18f,1.0f);
        
        ImGui::PushStyleColor(ImGuiCol_Button, btnCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, actCol);
        bool clicked = ImGui::Button(label, size);
        ImGui::PopStyleColor(3);
        
        if (clicked && outClicked) *outClicked = true;
        if (clicked) *toggle = !*toggle;
    } else {
        bool clicked = ImGui::Button(label, size);
        if (clicked && outClicked) *outClicked = true;
    }
    if (tooltip && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);
}

// -----------------------------------------------------------------------------
// Main UI
// -----------------------------------------------------------------------------
static void ShowMainUI()
{
    ImGuiIO& io = ImGui::GetIO();
    ImGuiStyle& style = ImGui::GetStyle();

    // Fade out step indicators
    for (int i = 0; i < 16; i++) {
        if (step_fade[i] > 0.0f) {
            step_fade[i] -= 0.02f;
            if (step_fade[i] < 0.0f) step_fade[i] = 0.0f;
        }
    }

    ImGui::SetNextWindowPos(ImVec2(0,0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
    ImGuiWindowFlags rootFlags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize   |
        ImGuiWindowFlags_NoMove     |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar|
        ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::Begin("regroove nanokontrol2 UI", nullptr, rootFlags);

    const float SIDE_MARGIN              = 10.0f;
    const float TOP_MARGIN               = 8.0f;
    const float LEFT_PANEL_WIDTH         = 190.0f;
    const float LCD_HEIGHT               = 90.0f;
    const float TRANSPORT_GAP            = 10.0f;
    const float SEQUENCER_HEIGHT         = 70.0f;
    const float GAP_ABOVE_SEQUENCER      = 8.0f;
    const float BOTTOM_MARGIN            = 6.0f;
    const float SOLO_SIZE                = 34.0f;
    const float MUTE_SIZE                = 34.0f;
    const float TOP_CLEARANCE            = 8.0f;
    const float LABEL_TO_SOLO_GAP        = 4.0f;
    const float SOLO_TO_SLIDER_GAP       = 6.0f;
    const float SLIDER_TO_MUTE_GAP       = 8.0f;
    const float MUTE_BOTTOM_CLEARANCE    = 12.0f;
    const float BASE_SLIDER_W            = 44.0f;
    const float BASE_SPACING             = 26.0f;
    const float MIN_SLIDER_W             = 24.0f;
    const float MIN_SPACING              = 5.0f;
    const float MAX_WIDTH_SCALE          = 1.40f;
    const float MIN_SLIDER_HEIGHT        = 140.0f;
    const float STEP_GAP                 = 6.0f;
    const float STEP_MIN                 = 28.0f;
    const float STEP_MAX                 = 60.0f;
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
        std::snprintf(lcd, sizeof(lcd), "Pat:%02d Ord:%02d Loop:%s",
                      pattern, order, loop_enabled ? "ON" : "OFF");
        DrawLCD(lcd, LEFT_PANEL_WIDTH - 16.0f, LCD_HEIGHT);
    }

    bool stopClicked=false, playClicked=false;
    SquareButton("[]", "Stop", nullptr, ImVec2(48,48), &stopClicked);
    if (stopClicked) control_play_pause(0);
    ImGui::SameLine();
    SquareButton("|>", "Play", nullptr, ImVec2(48,48), &playClicked);
    if (playClicked) control_play_pause(1);

    ImGui::Dummy(ImVec2(0, TRANSPORT_GAP));
    SquareButton("<<", "Previous Pattern", nullptr, ImVec2(48,48));
    ImGui::SameLine();
    SquareButton(">>", "Next Pattern", nullptr, ImVec2(48,48));
    ImGui::SameLine();
    SquareButton("O∞", "Toggle Loop", &loop_enabled, ImVec2(48,48));

    ImGui::EndChild();

    // CHANNEL PANEL (now 9 columns: 8 channels + 1 pitch)
    float rightX = SIDE_MARGIN + LEFT_PANEL_WIDTH + 18.0f;
    float rightW = fullW - rightX - SIDE_MARGIN;
    if (rightW < 300.0f) rightW = 300.0f;

    float baseTotal = BASE_SLIDER_W * 9.0f + BASE_SPACING * 8.0f; // 9 columns now
    float widthScale = rightW / baseTotal;
    if (widthScale > MAX_WIDTH_SCALE) widthScale = MAX_WIDTH_SCALE;

    float sliderW = BASE_SLIDER_W * widthScale;
    float spacing = BASE_SPACING * widthScale;
    bool horizontalScroll = false;

    if (sliderW < MIN_SLIDER_W) {
        sliderW = MIN_SLIDER_W;
        float remaining = rightW - sliderW * 9.0f;
        spacing = remaining / 8.0f;
        if (spacing < MIN_SPACING) {
            spacing = MIN_SPACING;
            horizontalScroll = true;
        }
    }

    ImGui::SetCursorPos(ImVec2(rightX, TOP_MARGIN));
    ImGuiWindowFlags chanFlags = ImGuiWindowFlags_NoScrollWithMouse |
        (horizontalScroll ? ImGuiWindowFlags_HorizontalScrollbar : ImGuiWindowFlags_NoScrollbar);

    ImGui::BeginChild("channels_panel", ImVec2(rightW, channelAreaHeight), true, chanFlags);

    float labelH = ImGui::GetTextLineHeight();
    float contentHeight = channelAreaHeight - childPaddingY;
    float sliderTop = TOP_CLEARANCE + labelH + LABEL_TO_SOLO_GAP + SOLO_SIZE + SOLO_TO_SLIDER_GAP;
    float bottomStack = SLIDER_TO_MUTE_GAP + MUTE_SIZE + MUTE_BOTTOM_CLEARANCE;
    float sliderH = contentHeight - sliderTop - bottomStack - IMGUI_LAYOUT_COMPENSATION;
    if (sliderH < MIN_SLIDER_HEIGHT) sliderH = MIN_SLIDER_HEIGHT;

    ImVec2 origin = ImGui::GetCursorPos();

    // 8 channel columns
    for (int i = 0; i < 8; ++i) {
        float colX = origin.x + i * (sliderW + spacing);
        ImGui::SetCursorPos(ImVec2(colX, origin.y + TOP_CLEARANCE));
        ImGui::BeginGroup();

        ImGui::Text("Ch%d", i + 1);
        ImGui::Dummy(ImVec2(0, LABEL_TO_SOLO_GAP));

        // SOLO
        ImGui::PushStyleColor(ImGuiCol_Button,
                              channels[i].solo ? ImVec4(0.80f,0.12f,0.14f,1.0f) : ImVec4(0.26f,0.27f,0.30f,1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              channels[i].solo ? ImVec4(0.95f,0.30f,0.32f,1.0f) : ImVec4(0.32f,0.33f,0.36f,1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              channels[i].solo ? ImVec4(0.60f,0.10f,0.12f,1.0f) : ImVec4(0.20f,0.21f,0.24f,1.0f));
        if (ImGui::Button((std::string("S##solo")+std::to_string(i)).c_str(), ImVec2(sliderW, SOLO_SIZE)))
            control_channel_solo(i);
        ImGui::PopStyleColor(3);

        ImGui::Dummy(ImVec2(0, SOLO_TO_SLIDER_GAP));

        // Volume slider
        float prev_vol = channels[i].volume;
        if (ImGui::VSliderFloat((std::string("##vol")+std::to_string(i)).c_str(),
                                ImVec2(sliderW, sliderH),
                                &channels[i].volume, 0.0f, 1.0f, "")) {
            if (prev_vol != channels[i].volume) {
                control_channel_volume(i, channels[i].volume);
            }
        }

        ImGui::Dummy(ImVec2(0, SLIDER_TO_MUTE_GAP));

        // MUTE
        ImGui::PushStyleColor(ImGuiCol_Button,
                              channels[i].mute ? ImVec4(0.90f,0.16f,0.18f,1.0f) : ImVec4(0.26f,0.27f,0.30f,1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              channels[i].mute ? ImVec4(0.98f,0.28f,0.30f,1.0f) : ImVec4(0.32f,0.33f,0.36f,1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              channels[i].mute ? ImVec4(0.60f,0.12f,0.14f,1.0f) : ImVec4(0.20f,0.21f,0.24f,1.0f));
        if (ImGui::Button((std::string("M##mute")+std::to_string(i)).c_str(), ImVec2(sliderW, MUTE_SIZE)))
            control_channel_mute(i);
        ImGui::PopStyleColor(3);

        ImGui::EndGroup();
    }

    // 9th column: PITCH slider
    {
        float colX = origin.x + 8 * (sliderW + spacing);
        ImGui::SetCursorPos(ImVec2(colX, origin.y + TOP_CLEARANCE));
        ImGui::BeginGroup();

        ImGui::Text("Pitch");
        ImGui::Dummy(ImVec2(0, LABEL_TO_SOLO_GAP));

        // No SOLO button for pitch, just a spacer
        ImGui::Dummy(ImVec2(sliderW, SOLO_SIZE));
        ImGui::Dummy(ImVec2(0, SOLO_TO_SLIDER_GAP));

        // Pitch slider (-1 to +1, centered at 0)
        float prev_pitch = pitch_slider;
        if (ImGui::VSliderFloat("##pitch", ImVec2(sliderW, sliderH),
                                &pitch_slider, -1.0f, 1.0f, "")) {
            if (prev_pitch != pitch_slider) {
                control_pitch(pitch_slider);
            }
        }

        ImGui::Dummy(ImVec2(0, SLIDER_TO_MUTE_GAP));

        // Reset button instead of MUTE
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.26f,0.27f,0.30f,1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f,0.33f,0.36f,1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20f,0.21f,0.24f,1.0f));
        if (ImGui::Button("R##pitch_reset", ImVec2(sliderW, MUTE_SIZE)))
            control_pitch_reset();
        ImGui::PopStyleColor(3);

        ImGui::EndGroup();
    }

    ImGui::EndChild();

    // SEQUENCER (with step indicators and clickable loop control)
    float sequencerTop = TOP_MARGIN + channelAreaHeight + GAP_ABOVE_SEQUENCER;
    ImGui::SetCursorPos(ImVec2(SIDE_MARGIN, sequencerTop));

    ImGui::BeginChild("sequencer_bar", ImVec2(fullW - 2*SIDE_MARGIN, SEQUENCER_HEIGHT),
                      false, ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);

    float availWidth = ImGui::GetContentRegionAvail().x;
    float stepWidth = (availWidth - STEP_GAP * 15.0f) / 16.0f;
    stepWidth = Clamp(stepWidth, STEP_MIN, STEP_MAX);
    float stepHeight = stepWidth;

    for (int i = 0; i < 16; ++i) {
        // Calculate button color with fade
        float brightness = step_fade[i];
        ImVec4 btnCol = ImVec4(0.18f + brightness * 0.24f, 
                               0.27f + brightness * 0.38f, 
                               0.18f + brightness * 0.24f, 1.0f);
        
        ImGui::PushStyleColor(ImGuiCol_Button, btnCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f,0.48f,0.32f,1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.42f,0.65f,0.42f,1.0f));
        
        if (ImGui::Button((std::string("##step")+std::to_string(i)).c_str(), ImVec2(stepWidth, stepHeight))) {
            control_set_loop_rows(i);
        }
        ImGui::PopStyleColor(3);
        
        if (i != 15) ImGui::SameLine(0.0f, STEP_GAP);
    }

    ImGui::EndChild();
    ImGui::End();
}

// -----------------------------------------------------------------------------
// MIDI Callback
// -----------------------------------------------------------------------------
void my_midi_mapping(unsigned char status, unsigned char cc, unsigned char value, void *userdata) {
    (void)userdata;
    
    if ((status & 0xF0) == 0xB0) {
        if (cc >= 32 && cc < 32 + 8) { // SOLO
            int ch = cc - 32;
            if (value >= 64) control_channel_solo(ch);
        } else if (cc >= 48 && cc < 48 + 8) { // MUTE
            int ch = cc - 48;
            if (value >= 64) control_channel_mute(ch);
        } else if (cc >= 0 && cc < 8) { // VOLUME
            double vol = value / 127.0;
            control_channel_volume(cc, (float)vol);
        } else if (cc == 8) { // PITCH (9th fader)
            // Map 0-127 to -1.0 to 1.0
            pitch_slider = (value - 63.5f) / 63.5f;
            control_pitch(pitch_slider);
        } else switch (cc) {
            case 41: // Play
                if (value >= 64) control_play_pause(1);
                break;
            case 42: // Stop
                if (value >= 64) control_play_pause(0);
                break;
            case 46: // Pattern mode toggle
                if (value >= 64) control_pattern_mode_toggle();
                break;
            case 44: // Next order
                if (value >= 64) control_next_order();
                break;
            case 43: // Prev order
                if (value >= 64) control_prev_order();
                break;
        }
    }
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    int midi_port = -1;
    const char *module_path = NULL;

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc) {
            midi_port = atoi(argv[++i]);
        } else if (!module_path) {
            module_path = argv[i];
        }
    }

    if (!module_path) {
        fprintf(stderr, "Usage: %s file.mod [-m mididevice]\n", argv[0]);
        return 1;
    }

    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // Setup window
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window* window = SDL_CreateWindow(
        "regroove nanokontrol2 UI",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1200, 640,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
    );
    if (!window) {
        fprintf(stderr, "Failed to create window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    if (!gl_ctx) {
        fprintf(stderr, "Failed to create GL context: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl_ctx);
    SDL_GL_SetSwapInterval(1);

    // Setup audio
    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.freq = 48000;
    spec.format = AUDIO_S16SYS;
    spec.channels = 2;
    spec.samples = 256;
    spec.callback = audio_callback;
    spec.userdata = NULL;

    if (SDL_OpenAudio(&spec, NULL) < 0) {
        fprintf(stderr, "SDL_OpenAudio failed: %s\n", SDL_GetError());
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ApplyFlatBlackRedSkin();
    ImGui_ImplSDL2_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL2_Init();

    // Load initial module
    if (load_module(module_path) != 0) {
        fprintf(stderr, "Failed to load module: %s\n", module_path);
        ImGui_ImplOpenGL2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        SDL_CloseAudio();
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Setup MIDI
    int midi_ports = midi_list_ports();
    if (midi_ports > 0) {
        if (midi_init(my_midi_mapping, NULL, midi_port) != 0)
            printf("No MIDI available. Running with keyboard/UI control only.\n");
    } else {
        printf("No MIDI available. Running with keyboard/UI control only.\n");
    }

    // Main loop
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) {
                running = false;
            } else if (e.type == SDL_WINDOWEVENT &&
                       e.window.event == SDL_WINDOWEVENT_CLOSE &&
                       e.window.windowID == SDL_GetWindowID(window)) {
                running = false;
            } else if (e.type == SDL_KEYDOWN) {
                switch (e.key.keysym.sym) {
                    case SDLK_SPACE:
                        control_play_pause(!playing);
                        break;
                    case SDLK_r:
                        control_retrigger();
                        break;
                    case SDLK_n:
                        control_next_order();
                        break;
                    case SDLK_p:
                        control_prev_order();
                        break;
                    case SDLK_l:
                        control_pattern_mode_toggle();
                        break;
                    case SDLK_ESCAPE:
                    case SDLK_q:
                        running = false;
                        break;
                }
            }
        }

        if (g_regroove) {
            regroove_process_commands(g_regroove);
        }

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

    // Cleanup
    midi_deinit();

    SDL_PauseAudio(1);
    SDL_LockAudio();
    if (g_regroove) {
        Regroove *tmp = g_regroove;
        g_regroove = NULL;
        SDL_UnlockAudio();
        regroove_destroy(tmp);
    } else {
        SDL_UnlockAudio();
    }

    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_CloseAudio();
    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}