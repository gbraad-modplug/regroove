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
    ACTION_HALVE_LOOP,
    ACTION_FULL_LOOP,
    ACTION_PATTERN_MODE_TOGGLE,
    ACTION_MUTE_ALL,
    ACTION_UNMUTE_ALL,
    ACTION_PITCH_UP,
    ACTION_PITCH_DOWN,
    ACTION_PITCH_SET,        // uses MIDI value for pitch (continuous control)
    ACTION_PITCH_RESET,      // reset pitch to 1.0x
    ACTION_QUIT,
    ACTION_FILE_PREV,
    ACTION_FILE_NEXT,
    ACTION_FILE_LOAD,
    // Parameterized actions
    ACTION_CHANNEL_MUTE,     // parameter = channel index
    ACTION_CHANNEL_SOLO,     // parameter = channel index
    ACTION_QUEUE_CHANNEL_MUTE,  // parameter = channel index (queued at pattern boundary)
    ACTION_QUEUE_CHANNEL_SOLO,  // parameter = channel index (queued at pattern boundary)
    ACTION_CHANNEL_VOLUME,   // parameter = channel index, uses MIDI value for volume
    ACTION_TRIGGER_PAD,      // parameter = pad index (0-15)
    ACTION_JUMP_TO_ORDER,    // parameter = order index (immediate jump)
    ACTION_JUMP_TO_PATTERN,  // parameter = pattern index (immediate jump)
    ACTION_QUEUE_ORDER,      // parameter = order index (queued jump at pattern end)
    ACTION_QUEUE_PATTERN,    // parameter = pattern index (queued jump at pattern end)
    ACTION_RECORD_TOGGLE,    // toggle performance recording
    ACTION_SET_LOOP_STEP,    // parameter = step index (0-15), sets loop length
    ACTION_TRIGGER_PHRASE,   // parameter = phrase index (triggers phrase sequence)
    ACTION_TRIGGER_LOOP,     // parameter = loop range index (triggers saved loop range)
    ACTION_PLAY_TO_LOOP,     // parameter = loop range index (arms loop, waits to reach start)
    // Effects actions (continuous, use MIDI value 0-127)
    ACTION_FX_DISTORTION_DRIVE,    // distortion drive amount
    ACTION_FX_DISTORTION_MIX,      // distortion dry/wet mix
    ACTION_FX_FILTER_CUTOFF,       // filter cutoff frequency
    ACTION_FX_FILTER_RESONANCE,    // filter resonance/Q
    ACTION_FX_EQ_LOW,              // EQ low band gain
    ACTION_FX_EQ_MID,              // EQ mid band gain
    ACTION_FX_EQ_HIGH,             // EQ high band gain
    ACTION_FX_COMPRESSOR_THRESHOLD, // compressor threshold
    ACTION_FX_COMPRESSOR_RATIO,    // compressor ratio
    ACTION_FX_DELAY_TIME,          // delay time
    ACTION_FX_DELAY_FEEDBACK,      // delay feedback
    ACTION_FX_DELAY_MIX,           // delay dry/wet mix
    // Effects toggles (button/trigger)
    ACTION_FX_DISTORTION_TOGGLE,   // toggle distortion on/off
    ACTION_FX_FILTER_TOGGLE,       // toggle filter on/off
    ACTION_FX_EQ_TOGGLE,           // toggle EQ on/off
    ACTION_FX_COMPRESSOR_TOGGLE,   // toggle compressor on/off
    ACTION_FX_DELAY_TOGGLE,        // toggle delay on/off
    // Mixer actions (continuous, use MIDI value 0-127)
    ACTION_MASTER_VOLUME,          // master output volume
    ACTION_PLAYBACK_VOLUME,        // playback engine volume
    ACTION_INPUT_VOLUME,           // audio input volume
    ACTION_MASTER_PAN,             // master pan (0=left, 64=center, 127=right)
    ACTION_PLAYBACK_PAN,           // playback pan
    ACTION_INPUT_PAN,              // input pan
    ACTION_CHANNEL_PAN,            // channel pan (requires parameter = channel index)
    // Mixer toggles (button/trigger)
    ACTION_MASTER_MUTE,            // toggle master mute
    ACTION_PLAYBACK_MUTE,          // toggle playback mute
    ACTION_INPUT_MUTE,             // toggle input mute
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
    int device_id;           // MIDI device ID (0 or 1, -1 = any device)
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

// Trigger pad configuration
#define MAX_TRIGGER_PADS 16           // Application pads (A1-A16)
#define MAX_SONG_TRIGGER_PADS 16      // Song-specific pads (S1-S16)
#define MAX_TOTAL_TRIGGER_PADS (MAX_TRIGGER_PADS + MAX_SONG_TRIGGER_PADS)

typedef struct {
    InputAction action;      // Action to trigger (ACTION_NONE if using phrase)
    int parameter;           // Action parameter (channel index, etc.)
    int midi_note;           // MIDI note number that triggers this pad (-1 = not mapped)
    int midi_device;         // Which MIDI device (-1 = any)
    int phrase_index;        // Index into phrases array (-1 = not using phrase, use action instead)
} TriggerPadConfig;

// Input mappings configuration (application-wide from regroove.ini)
typedef struct {
    MidiMapping *midi_mappings;
    int midi_count;
    int midi_capacity;
    KeyboardMapping *keyboard_mappings;
    int keyboard_count;
    int keyboard_capacity;
    TriggerPadConfig trigger_pads[MAX_TRIGGER_PADS];  // A1-A16 only
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
int input_mappings_get_midi_event(InputMappings *mappings, int device_id, int cc, int value, InputEvent *out_event);
int input_mappings_get_keyboard_event(InputMappings *mappings, int key, InputEvent *out_event);

// Get action name (for debugging/display)
const char* input_action_name(InputAction action);

// Parse action name to enum (for loading from files)
InputAction parse_action(const char *str);

#endif // INPUT_MAPPINGS_H
