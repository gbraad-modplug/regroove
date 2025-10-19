#include "lcd.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct LCD {
    int cols;
    int rows;
    char* buffer;      // Internal buffer for formatted text
    size_t buffer_size;
};

LCD* lcd_init(int cols, int rows) {
    if (cols <= 0 || rows <= 0) return NULL;

    LCD* lcd = (LCD*)calloc(1, sizeof(LCD));
    if (!lcd) return NULL;

    lcd->cols = cols;
    lcd->rows = rows;

    // Allocate buffer: (cols * rows) + (rows - 1) newlines + 1 null terminator
    lcd->buffer_size = (cols * rows) + rows;
    lcd->buffer = (char*)calloc(lcd->buffer_size, sizeof(char));

    if (!lcd->buffer) {
        free(lcd);
        return NULL;
    }

    return lcd;
}

void lcd_write(LCD* lcd, const char* text) {
    if (!lcd || !text) return;

    // Clear buffer
    memset(lcd->buffer, 0, lcd->buffer_size);

    // Copy text to buffer, respecting LCD dimensions
    // Input text should be newline-separated lines
    const char* src = text;
    char* dst = lcd->buffer;
    size_t remaining = lcd->buffer_size - 1; // Reserve 1 for null terminator

    int current_row = 0;
    int current_col = 0;

    while (*src && current_row < lcd->rows && remaining > 0) {
        if (*src == '\n') {
            // Move to next line
            current_row++;
            current_col = 0;
            if (current_row < lcd->rows && remaining > 0) {
                *dst++ = '\n';
                remaining--;
            }
            src++;
        } else if (current_col < lcd->cols) {
            // Copy character
            *dst++ = *src++;
            current_col++;
            remaining--;
        } else {
            // Skip characters beyond column limit until newline
            while (*src && *src != '\n') src++;
        }
    }

    *dst = '\0';
}

const char* lcd_get_buffer(const LCD* lcd) {
    if (!lcd) return NULL;
    return lcd->buffer;
}

void lcd_clear(LCD* lcd) {
    if (!lcd || !lcd->buffer) return;
    memset(lcd->buffer, 0, lcd->buffer_size);
}

void lcd_destroy(LCD* lcd) {
    if (!lcd) return;
    if (lcd->buffer) free(lcd->buffer);
    free(lcd);
}
