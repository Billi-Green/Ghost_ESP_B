#include "sdkconfig.h"

#ifdef CONFIG_WITH_STATUS_DISPLAY

#include "managers/status_display_animations.h"

#include <string.h>

static const char *s_text_line1 = "GhostESP:";
static const char *s_text_line2 = "Revival";
static int s_text_x = 0;
static int s_text_y = 0;
static int s_vx = 2;
static int s_vy = 1;
static bool s_initialized = false;

void status_anim_bouncing_text_reset(void)
{
    s_initialized = false;
}

void status_anim_bouncing_text_step(TickType_t now, int frame, const StatusAnimGfx *gfx)
{
    (void)now;
    (void)frame;
    if (!gfx) return;

    // Initialize position on first frame
    if (!s_initialized) {
        s_initialized = true;
        // Calculate text dimensions (each character is 6 pixels wide: 5 for char + 1 spacing)
        int line1_width = strlen(s_text_line1) * 6;
        int line2_width = strlen(s_text_line2) * 6;
        int text_width = (line1_width > line2_width) ? line1_width : line2_width;
        int line_height = gfx->font_char_height * gfx->scale_y;
        int gap = 6;
        int text_height = line_height * 2 + gap; // Two lines with gap
        
        // Start at center of screen
        s_text_x = (gfx->width - text_width) / 2;
        s_text_y = (gfx->height - text_height) / 2;
        
        // Set initial velocity (diagonal movement)
        s_vx = 2;
        s_vy = 1;
    }

    // Calculate text dimensions (each character is 6 pixels wide: 5 for char + 1 spacing)
    int line1_width = strlen(s_text_line1) * 6;
    int line2_width = strlen(s_text_line2) * 6;
    int text_width = (line1_width > line2_width) ? line1_width : line2_width;
    int line_height = gfx->font_char_height * gfx->scale_y;
    int gap = 6;
    int text_height = line_height * 2 + gap; // Two lines with gap

    // Check for collisions before updating position
    int next_x = s_text_x + s_vx;
    int next_y = s_text_y + s_vy;

    // Calculate maximum allowed positions
    int max_y = gfx->height - text_height;

    // Bounce off left and right edges
    // Use the wider line for bounce detection to ensure both lines stay on screen
    if (next_x < 0) {
        s_text_x = 0;
        s_vx = -s_vx;
    } else {
        // Check if the rightmost pixel would exceed screen width
        // For n characters, the rightmost pixel is at: x + (n-1)*6 + 4 = x + n*6 - 2
        // To keep it on screen (width pixels, indexed 0 to width-1), we need: x + n*6 - 2 < width
        // Which means: x <= width - n*6 + 1
        // But to be safe and account for the last character's 5px width, we subtract 1 more
        int max_x_line1 = gfx->width - line1_width - 1;
        int max_x_line2 = gfx->width - line2_width - 1;
        int max_x = (max_x_line1 < max_x_line2) ? max_x_line1 : max_x_line2;
        if (next_x > max_x) {
            s_text_x = max_x;
            s_vx = -s_vx;
        } else {
            s_text_x = next_x;
        }
    }

    // Bounce off top and bottom edges
    if (next_y < 0) {
        s_text_y = 0;
        s_vy = -s_vy;
    } else if (next_y > max_y) {
        s_text_y = max_y;
        s_vy = -s_vy;
    } else {
        s_text_y = next_y;
    }

    // Draw both lines of text with proper spacing (matching status_display_render_locked)
    gfx->draw_text(gfx->user, s_text_x, s_text_y, s_text_line1);
    gfx->draw_text(gfx->user, s_text_x, s_text_y + line_height + gap, s_text_line2);
}

#endif // CONFIG_WITH_STATUS_DISPLAY

