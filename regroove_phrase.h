#ifndef REGROOVE_PHRASE_H
#define REGROOVE_PHRASE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "input_mappings.h"
#include "regroove_metadata.h"

// Maximum number of phrases that can be playing simultaneously
#define PHRASE_MAX_ACTIVE 16

// Active phrase playback state
typedef struct {
    int phrase_index;           // Which phrase is playing (-1 = inactive)
    int current_step;           // Current step index
    int playback_position;      // Current playback position in rows (increments each tick)
} ActivePhraseSlot;

// Phrase engine state
typedef struct RegroovePhrase RegroovePhrase;

// Callback function type for executing phrase actions
// This decouples the phrase engine from GUI/TUI - they provide a callback to execute actions
typedef void (*PhraseActionCallback)(InputAction action, int parameter, int value, void* userdata);

// Callback function type for phrase completion
// Called when a phrase finishes playback (all steps executed)
typedef void (*PhraseCompletionCallback)(int phrase_index, void* userdata);

// Callback function type for pre-trigger reset
// Called before a phrase starts, to reset UI state
typedef void (*PhraseResetCallback)(void* userdata);

// Create a new phrase engine
RegroovePhrase* regroove_phrase_create(void);

// Destroy phrase engine
void regroove_phrase_destroy(RegroovePhrase* phrase);

// Set the metadata source (where phrases are defined)
void regroove_phrase_set_metadata(RegroovePhrase* phrase, RegrooveMetadata* metadata);

// Set the callback that will be used to execute phrase actions
void regroove_phrase_set_action_callback(RegroovePhrase* phrase,
                                          PhraseActionCallback callback,
                                          void* userdata);

// Set the callback that will be called when a phrase completes
void regroove_phrase_set_completion_callback(RegroovePhrase* phrase,
                                               PhraseCompletionCallback callback,
                                               void* userdata);

// Set the callback that will be called before a phrase starts (for UI reset)
void regroove_phrase_set_reset_callback(RegroovePhrase* phrase,
                                          PhraseResetCallback callback,
                                          void* userdata);

// Trigger a phrase to start playing
// - Cancels all currently active phrases to avoid conflicts
// - Returns 0 on success, -1 on error (invalid phrase_index, no steps, etc.)
int regroove_phrase_trigger(RegroovePhrase* phrase, int phrase_index);

// Update phrase playback (call from row callback)
// - Executes pending actions via the callback
// - Advances phrase playback state
// - Should only be called when playback is active
void regroove_phrase_update(RegroovePhrase* phrase);

// Stop all active phrases
void regroove_phrase_stop_all(RegroovePhrase* phrase);

// Check if any phrases are currently playing
int regroove_phrase_is_active(const RegroovePhrase* phrase);

// Get number of active phrases
int regroove_phrase_get_active_count(const RegroovePhrase* phrase);

// Get direct access to active phrase slot (for debugging/display)
// Returns NULL if index is out of bounds
const ActivePhraseSlot* regroove_phrase_get_slot(const RegroovePhrase* phrase, int slot_index);

#ifdef __cplusplus
}
#endif

#endif // REGROOVE_PHRASE_H
