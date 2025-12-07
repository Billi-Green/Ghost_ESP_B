#include "sdkconfig.h"

#ifdef CONFIG_WITH_STATUS_DISPLAY

#include "managers/status_display_animations.h"

#include <stdint.h>
#include <stdbool.h>
#include <math.h>

// Hypnotic spiral animation
// Draws a rotating spiral that starts from the center and spirals outward

void status_anim_spiral_reset(void)
{
    // Nothing to reset for this animation
}

void status_anim_spiral_step(TickType_t now, int frame, const StatusAnimGfx *gfx)
{
    (void)now;
    if (!gfx || !gfx->plot_pixel) return;

    int center_x = gfx->width / 2;
    int center_y = gfx->height / 2;
    
    // Maximum radius is the distance to the corner
    int max_radius = (int)sqrt(center_x * center_x + center_y * center_y) + 1;
    
    // Rotation speed - complete rotation every ~120 frames
    float rotation_speed = 2.0f * 3.14159265f / 120.0f;
    float current_angle = frame * rotation_speed;
    
    // Spiral parameters
    float spiral_tightness = 0.25f; // How tight the spiral is (lower = tighter, higher = wider)
    int spiral_thickness = 3; // Thickness of the spiral line
    
    // Draw the spiral
    for (int r = 0; r < max_radius; r++) {
        // Calculate angle for this radius (spiral equation: angle = k * radius)
        float angle = current_angle + r * spiral_tightness;
        
        // Convert polar to cartesian coordinates
        int x = center_x + (int)(r * cos(angle));
        int y = center_y + (int)(r * sin(angle));
        
        // Draw a thick line by drawing multiple pixels around the point
        for (int dx = -spiral_thickness; dx <= spiral_thickness; dx++) {
            for (int dy = -spiral_thickness; dy <= spiral_thickness; dy++) {
                int px = x + dx;
                int py = y + dy;
                
                // Check bounds
                if (px >= 0 && px < gfx->width && py >= 0 && py < gfx->height) {
                    // Only draw pixels within the thickness radius
                    if (dx * dx + dy * dy <= spiral_thickness * spiral_thickness) {
                        gfx->plot_pixel(gfx->user, px, py, true);
                    }
                }
            }
        }
    }
}

#endif // CONFIG_WITH_STATUS_DISPLAY

