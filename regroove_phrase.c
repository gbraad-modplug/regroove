#include "regroove_phrase.h"
#include <stdlib.h>
#include <string.h>

// Phrase engine structure
struct RegroovePhrase {
    RegrooveMetadata* metadata;                 // Where phrases are defined (not owned)
    ActivePhraseSlot slots[PHRASE_MAX_ACTIVE];  // Active phrase playback slots
    PhraseActionCallback action_callback;       // Callback to execute actions
    void* action_userdata;                      // User data for action callback
    PhraseCompletionCallback completion_callback; // Callback when phrase completes
    void* completion_userdata;                  // User data for completion callback
    PhraseResetCallback reset_callback;         // Callback before phrase starts (for UI reset)
    void* reset_userdata;                       // User data for reset callback
    int executing_action;                       // Flag to prevent recursion
};

// Create a new phrase engine
RegroovePhrase* regroove_phrase_create(void) {
    RegroovePhrase* phrase = calloc(1, sizeof(RegroovePhrase));
    if (!phrase) return NULL;

    // Initialize all slots as inactive
    for (int i = 0; i < PHRASE_MAX_ACTIVE; i++) {
        phrase->slots[i].phrase_index = -1;
        phrase->slots[i].current_step = 0;
        phrase->slots[i].playback_position = 0;
    }

    phrase->executing_action = 0;
    return phrase;
}

// Destroy phrase engine
void regroove_phrase_destroy(RegroovePhrase* phrase) {
    if (!phrase) return;
    free(phrase);
}

// Set the metadata source (where phrases are defined)
void regroove_phrase_set_metadata(RegroovePhrase* phrase, RegrooveMetadata* metadata) {
    if (!phrase) return;
    phrase->metadata = metadata;
}

// Set the callback that will be used to execute phrase actions
void regroove_phrase_set_action_callback(RegroovePhrase* phrase,
                                          PhraseActionCallback callback,
                                          void* userdata) {
    if (!phrase) return;
    phrase->action_callback = callback;
    phrase->action_userdata = userdata;
}

// Set the callback that will be called when a phrase completes
void regroove_phrase_set_completion_callback(RegroovePhrase* phrase,
                                               PhraseCompletionCallback callback,
                                               void* userdata) {
    if (!phrase) return;
    phrase->completion_callback = callback;
    phrase->completion_userdata = userdata;
}

// Set the callback that will be called before a phrase starts (for UI reset)
void regroove_phrase_set_reset_callback(RegroovePhrase* phrase,
                                          PhraseResetCallback callback,
                                          void* userdata) {
    if (!phrase) return;
    phrase->reset_callback = callback;
    phrase->reset_userdata = userdata;
}

// Trigger a phrase to start playing
int regroove_phrase_trigger(RegroovePhrase* phrase, int phrase_index) {
    if (!phrase || !phrase->metadata) return -1;
    if (phrase_index < 0 || phrase_index >= phrase->metadata->phrase_count) return -1;

    const Phrase* p = &phrase->metadata->phrases[phrase_index];
    if (p->step_count == 0) return -1;

    // Call reset callback to allow UI to reset visual state
    if (phrase->reset_callback) {
        phrase->reset_callback(phrase->reset_userdata);
    }

    // Cancel all currently active phrases to avoid conflicts
    for (int i = 0; i < PHRASE_MAX_ACTIVE; i++) {
        if (phrase->slots[i].phrase_index != -1) {
            phrase->slots[i].phrase_index = -1;
        }
    }

    // Use slot 0 for the new phrase
    phrase->slots[0].phrase_index = phrase_index;
    phrase->slots[0].current_step = 0;
    phrase->slots[0].playback_position = 0;  // Start at position 0

    return 0;
}

// Update phrase playback (call from row callback)
void regroove_phrase_update(RegroovePhrase* phrase) {
    if (!phrase || !phrase->metadata) return;

    for (int i = 0; i < PHRASE_MAX_ACTIVE; i++) {
        ActivePhraseSlot* slot = &phrase->slots[i];
        if (slot->phrase_index == -1) continue;

        const Phrase* p = &phrase->metadata->phrases[slot->phrase_index];

        // Execute all steps that should happen at current position
        while (slot->current_step < p->step_count) {
            const PhraseStep* step = &p->steps[slot->current_step];

            // Check if this step should execute at current position
            if (step->position_rows > slot->playback_position) {
                break;  // Wait for future position
            }

            // Execute current step via callback
            if (phrase->action_callback) {
                phrase->executing_action = 1;
                phrase->action_callback(step->action, step->parameter, step->value,
                                        phrase->action_userdata);
                phrase->executing_action = 0;
            }

            // Move to next step
            slot->current_step++;
        }

        // Check if phrase is complete
        if (slot->current_step >= p->step_count) {
            int completed_phrase_index = slot->phrase_index;
            slot->phrase_index = -1;  // Mark as inactive

            // Call completion callback
            if (phrase->completion_callback) {
                phrase->completion_callback(completed_phrase_index, phrase->completion_userdata);
            }
        } else {
            // Increment playback position for next update
            slot->playback_position++;
        }
    }
}

// Stop all active phrases
void regroove_phrase_stop_all(RegroovePhrase* phrase) {
    if (!phrase) return;

    for (int i = 0; i < PHRASE_MAX_ACTIVE; i++) {
        phrase->slots[i].phrase_index = -1;
        phrase->slots[i].current_step = 0;
        phrase->slots[i].playback_position = 0;
    }
}

// Check if any phrases are currently playing
int regroove_phrase_is_active(const RegroovePhrase* phrase) {
    if (!phrase) return 0;

    for (int i = 0; i < PHRASE_MAX_ACTIVE; i++) {
        if (phrase->slots[i].phrase_index != -1) {
            return 1;
        }
    }
    return 0;
}

// Get number of active phrases
int regroove_phrase_get_active_count(const RegroovePhrase* phrase) {
    if (!phrase) return 0;

    int count = 0;
    for (int i = 0; i < PHRASE_MAX_ACTIVE; i++) {
        if (phrase->slots[i].phrase_index != -1) {
            count++;
        }
    }
    return count;
}

// Get direct access to active phrase slot (for debugging/display)
const ActivePhraseSlot* regroove_phrase_get_slot(const RegroovePhrase* phrase, int slot_index) {
    if (!phrase || slot_index < 0 || slot_index >= PHRASE_MAX_ACTIVE) return NULL;
    return &phrase->slots[slot_index];
}

// Check if currently executing an action (to prevent recursion)
int regroove_phrase_is_executing(const RegroovePhrase* phrase) {
    if (!phrase) return 0;
    return phrase->executing_action;
}
