/*
 * NSEvent → kr_input_event, shared by the viewer's panes (src/viewer/main.mm) and the Python app
 * shell's windows (src/python/app.mm). Objective-C++ only. Positions are in the view's coordinates
 * (a flipped view: top-left origin) times its scale, i.e. the kr_view's device pixels.
 */
#pragma once

#import <AppKit/AppKit.h>

#include <karouka/karouka.h>

namespace Karouka::EventInput {

inline uint32_t modifiers_of(NSEvent* event)
{
    uint32_t modifiers = 0;
    if (event.modifierFlags & NSEventModifierFlagShift)
        modifiers |= KR_MOD_SHIFT;
    if (event.modifierFlags & NSEventModifierFlagControl)
        modifiers |= KR_MOD_CTRL;
    if (event.modifierFlags & NSEventModifierFlagOption)
        modifiers |= KR_MOD_ALT;
    if (event.modifierFlags & NSEventModifierFlagCommand)
        modifiers |= KR_MOD_SUPER;
    return modifiers;
}

// mouse_move, mouse_down or mouse_up at the event's position in `view`; button 0 primary, 1 secondary.
inline kr_input_event mouse(NSView* view, NSEvent* event, kr_input_type type, uint32_t button, float scale)
{
    NSPoint point = [view convertPoint:event.locationInWindow fromView:nil];
    kr_input_event input {};
    input.struct_size = sizeof(input);
    input.type = type;
    input.x = point.x * scale;
    input.y = point.y * scale;
    input.button = button;
    input.modifiers = modifiers_of(event);
    return input;
}

// A wheel: precise (trackpad) deltas are points, line deltas 16 points a line; positive scrolls down.
inline kr_input_event wheel(NSView* view, NSEvent* event, float scale)
{
    kr_input_event input = mouse(view, event, KR_INPUT_WHEEL, 0, scale);
    float factor = event.hasPreciseScrollingDeltas ? 1.0f : 16.0f;
    input.delta_x = -event.scrollingDeltaX * factor * scale;
    input.delta_y = -event.scrollingDeltaY * factor * scale;
    return input;
}

// key_down or key_up: the virtual key code, and the one character it types (none for controls and
// the function-key range).
inline kr_input_event key(NSEvent* event, kr_input_type type)
{
    kr_input_event input {};
    input.struct_size = sizeof(input);
    input.type = type;
    input.key_code = event.keyCode;
    input.modifiers = modifiers_of(event);
    NSString* characters = event.characters;
    if (characters.length == 1) {
        unichar c = [characters characterAtIndex:0];
        if (c >= 0x20 && c != 0x7F && (c < 0xF700 || c > 0xF8FF))
            input.code_point = c;
    }
    return input;
}

}
