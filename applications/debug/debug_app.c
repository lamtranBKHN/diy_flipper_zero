#include "furi.h"
#include "furi_hal.h"

#include <gui/gui.h>
#include <input/input.h>

#include <notification/notification_messages.h>

typedef enum {
    DebugEventTypeTick,
    DebugEventTypeInput,
} DebugEventType;

typedef struct {
    DebugEventType type;
    InputEvent input;
} DebugEvent;

// Debug sequence for LED colors
static const NotificationSequence debug_sequence_red = {
    &message_red_255,
    NULL,
};

static const NotificationSequence debug_sequence_green = {
    &message_green_255,
    NULL,
};

static const NotificationSequence debug_sequence_blue = {
    &message_blue_255,
    NULL,
};

static const NotificationSequence* debug_sequences[] = {
    &debug_sequence_red,
    &debug_sequence_green,
    &debug_sequence_blue,
};

static void debug_update(void* ctx) {
    furi_assert(ctx);
    FuriMessageQueue* event_queue = ctx;
    DebugEvent event = {.type = DebugEventTypeTick};
    // It's OK to loose this event if system overloaded
    furi_message_queue_put(event_queue, &event, 0);
}

static void debug_draw_callback(Canvas* canvas, void* ctx) {
    UNUSED(ctx);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Debug application");
    canvas_draw_str(canvas, 2, 25, "Running system analysis...");
}

static void debug_input_callback(InputEvent* input_event, void* ctx) {
    furi_assert(ctx);
    FuriMessageQueue* event_queue = ctx;

    DebugEvent event = {.type = DebugEventTypeInput, .input = *input_event};
    furi_message_queue_put(event_queue, &event, FuriWaitForever);
}

int32_t debug_app(void* p) {
    UNUSED(p);
    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(DebugEvent));

    // Configure view port
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, debug_draw_callback, NULL);
    view_port_input_callback_set(view_port, debug_input_callback, event_queue);
    FuriTimer* timer = furi_timer_alloc(debug_update, FuriTimerTypePeriodic, event_queue);
    furi_timer_start(timer, furi_kernel_get_tick_frequency());

    // Register view port in GUI
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    NotificationApp* notifications = furi_record_open(RECORD_NOTIFICATION);

    uint8_t state = 0;
    DebugEvent event;

    while(1) {
        furi_check(furi_message_queue_get(event_queue, &event, FuriWaitForever) == FuriStatusOk);
        if(event.type == DebugEventTypeInput) {
            if((event.input.type == InputTypeShort) && (event.input.key == InputKeyBack)) {
                break;
            }
        } else {
            notification_message(notifications, debug_sequences[state]);
            state++;
            if(state >= COUNT_OF(debug_sequences)) {
                state = 0;
            }
        }
    }

    notification_message(notifications, &sequence_blink_stop);

    furi_timer_free(timer);

    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_message_queue_free(event_queue);

    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);

    return 0;
}
