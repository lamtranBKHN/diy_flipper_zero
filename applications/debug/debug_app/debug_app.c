#include <furi.h>
#include <furi_hal.h>

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

static void debug_draw_callback(Canvas* canvas, void* ctx) {
    UNUSED(ctx);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Debug application");
    canvas_draw_str(canvas, 2, 25, "System analysis in progress...");
}

static void debug_input_callback(InputEvent* input_event, void* ctx) {
    furi_assert(ctx);
    FuriMessageQueue* event_queue = ctx;

    DebugEvent event = {.type = DebugEventTypeInput, .input = *input_event};
    furi_message_queue_put(event_queue, &event, FuriWaitForever);
}

// Application entry point
int32_t debug_app(void* p) {
    UNUSED(p);

    // Initialize
    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(DebugEvent));

    // Configure view port
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, debug_draw_callback, NULL);
    view_port_input_callback_set(view_port, debug_input_callback, event_queue);

    // Register view port in GUI
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    // Notification system
    NotificationApp* notifications = furi_record_open(RECORD_NOTIFICATION);

    // Main loop
    DebugEvent event;
    uint8_t color_index = 0;

    while(1) {
        furi_check(furi_message_queue_get(event_queue, &event, FuriWaitForever) == FuriStatusOk);

        if(event.type == DebugEventTypeInput) {
            if((event.input.type == InputTypeShort) && (event.input.key == InputKeyBack)) {
                break;
            }
        } else {
            // Cycle through colors
            const NotificationSequence* colors[] = {
                &sequence_blink_red_100,
                &sequence_blink_green_100,
                &sequence_blink_blue_100,
                &sequence_blink_yellow_100,
                &sequence_blink_cyan_100,
                &sequence_blink_magenta_100,
                &sequence_blink_white_100,
            };

            notification_message(notifications, colors[color_index]);
            color_index++;
            if(color_index >= COUNT_OF(colors)) {
                color_index = 0;
            }
        }
    }

    // Cleanup
    notification_message(notifications, &sequence_blink_stop);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    view_port_enabled_set(view_port, false);
    view_port_free(view_port);
    furi_message_queue_free(event_queue);

    return 0;
}
