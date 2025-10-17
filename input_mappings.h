#ifndef INPUT_MAPPINGS_H
#define INPUT_MAPPINGS_H

// Action types that can be triggered by inputs
typedef enum {
    ACTION_NONE = 0,
    ACTION_PLAY_PAUSE,
    ACTION_PLAY,
    ACTION_STOP,
    ACTION_RETRIGGER,
    ACTION_NEXT_ORDER,
    ACTION_PREV_ORDER,
    ACTION_LOOP_TILL_ROW,
    ACTION_HALVE_LOOP,
    ACTION_FULL_LOOP,
    ACTION_PATTERN_MODE_TOGGLE,
    ACTION_MUTE_ALL,
    ACTION_UNMUTE_ALL,
    ACTION_PITCH_UP,
    ACTION_PITCH_DOWN,
    ACTION_QUIT,
    ACTION_FILE_PREV,
    ACTION_FILE_NEXT,
    ACTION_FILE_LOAD,
    // Parameterized actions
    ACTION_CHANNEL_MUTE,     // parameter = channel index
    ACTION_CHANNEL_SOLO,     // parameter = channel index
    ACTION_CHANNEL_VOLUME,   // parameter = channel index, uses MIDI value for volume
    ACTION_MAX
} InputAction;

// Input event with action and parameter
typedef struct {
    InputAction action;
    int parameter;           // Generic parameter (channel index, etc.)
    int value;               // For continuous controls (MIDI CC value, etc.)
} InputEvent;

// MIDI mapping entry
typedef struct {
    int cc_number;           // MIDI CC number (0-127, -1 = unused)
    InputAction action;      // Action to trigger
    int parameter;           // Action parameter (channel index, etc.)
    int threshold;           // Trigger threshold (default 64 for buttons, 0 for continuous)
    int continuous;          // 1 = continuous control (volume), 0 = button/trigger
} MidiMapping;

// Keyboard mapping entry
typedef struct {
    int key;                 // ASCII key code (-1 = unused)
    InputAction action;      // Action to trigger
    int parameter;           // Action parameter (channel index, etc.)
} KeyboardMapping;

// Input mappings configuration
typedef struct {
    MidiMapping *midi_mappings;
    int midi_count;
    int midi_capacity;
    KeyboardMapping *keyboard_mappings;
    int keyboard_count;
    int keyboard_capacity;
} InputMappings;

// Initialize input mappings system
InputMappings* input_mappings_create(void);

// Destroy input mappings and free resources
void input_mappings_destroy(InputMappings *mappings);

// Load mappings from .ini file
int input_mappings_load(InputMappings *mappings, const char *filepath);

// Save mappings to .ini file
int input_mappings_save(InputMappings *mappings, const char *filepath);

// Reset to default mappings
void input_mappings_reset_defaults(InputMappings *mappings);

// Query mappings - returns 1 if action found, 0 otherwise
int input_mappings_get_midi_event(InputMappings *mappings, int cc, int value, InputEvent *out_event);
int input_mappings_get_keyboard_event(InputMappings *mappings, int key, InputEvent *out_event);

// Get action name (for debugging/display)
const char* input_action_name(InputAction action);

#endif // INPUT_MAPPINGS_H
