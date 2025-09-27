#include <furi.h>
#include <storage/storage.h>
#include <toolbox/saved_struct.h>
#include <gui/gui_i.h>
#include <u8g2_glue.h>
#include "notification.h"
#include "notification_messages.h"
#include "notification_app.h"

#define TAG "NotificationSrv"

// --- Save settings API (preserved) ---
void notification_message_save_settings(NotificationApp* app) {
    NotificationAppMessage m = {
        .type = SaveSettingsMessage,
        .back_event = furi_event_flag_alloc()
    };
    furi_check(furi_message_queue_put(app->queue, &m, FuriWaitForever) == FuriStatusOk);
    furi_event_flag_wait(
        m.back_event, NOTIFICATION_EVENT_COMPLETE, FuriFlagWaitAny, FuriWaitForever);
    furi_event_flag_free(m.back_event);
}

// --- Display contrast ---
static void notification_apply_lcd_contrast(NotificationApp* app) {
    Gui* gui = furi_record_open(RECORD_GUI);
    u8x8_d_st756x_set_contrast(&gui->canvas->fb.u8x8, app->settings.contrast);
    furi_record_close(RECORD_GUI);
}

// --- Input callbacks (mark dirty only) ---
static void input_event_callback(const void* value, void* context) {
    const InputEvent* event = value;
    NotificationApp* app = context;
    if(event->sequence_source == INPUT_SEQUENCE_SOURCE_HARDWARE) {
        app->settings_dirty = true;
    }
}

static void ascii_event_callback(const void* value, void* context) {
    UNUSED(value);
    NotificationApp* app = context;
    app->settings_dirty = true;
}

// --- Process messages ---
static void notification_process_notification_message(NotificationApp* app, NotificationAppMessage* message) {
    uint32_t idx = 0;
    const NotificationMessage* notif = (*message->sequence)[idx];

    while(notif != NULL) {
        switch(notif->type) {
        case NotificationMessageTypeLcdContrastUpdate:
            notification_apply_lcd_contrast(app);
            break;
        case NotificationMessageTypeDelay:
            furi_delay_ms(notif->data.delay.length);
            break;
        default:
            break;
        }
        idx++;
        notif = (*message->sequence)[idx];
    }
}

static void notification_process_internal_message(NotificationApp* app, NotificationAppMessage* message) {
    uint32_t idx = 0;
    const NotificationMessage* notif = (*message->sequence)[idx];

    while(notif != NULL) {
        if(notif->type == NotificationMessageTypeLcdContrastUpdate) {
            notification_apply_lcd_contrast(app);
        }
        idx++;
        notif = (*message->sequence)[idx];
    }
}

// --- Settings storage ---
static bool notification_load_settings(NotificationApp* app) {
    return saved_struct_load(
        NOTIFICATION_SETTINGS_PATH,
        &app->settings,
        sizeof(NotificationSettings),
        NOTIFICATION_SETTINGS_MAGIC,
        NOTIFICATION_SETTINGS_VERSION);
}

static bool notification_save_settings(NotificationApp* app) {
    bool res = saved_struct_save(
        NOTIFICATION_SETTINGS_PATH,
        &app->settings,
        sizeof(NotificationSettings),
        NOTIFICATION_SETTINGS_MAGIC,
        NOTIFICATION_SETTINGS_VERSION);
    return res;
}

static void notification_apply_settings(NotificationApp* app) {
    notification_load_settings(app);
    notification_apply_lcd_contrast(app);
}

// --- App allocation ---
static NotificationApp* notification_app_alloc(void) {
    NotificationApp* app = malloc(sizeof(NotificationApp));
    furi_check(app != NULL);

    app->queue = furi_message_queue_alloc(8, sizeof(NotificationAppMessage));
    app->settings.version = NOTIFICATION_SETTINGS_VERSION;
    app->settings_dirty = false;

    // Input subscription for buttons
    app->event_record = furi_record_open(RECORD_INPUT_EVENTS);
    furi_pubsub_subscribe(app->event_record, input_event_callback, app);
    app->ascii_record = furi_record_open(RECORD_ASCII_EVENTS);
    furi_pubsub_subscribe(app->ascii_record, ascii_event_callback, app);

    notification_apply_settings(app);

    furi_record_create(RECORD_NOTIFICATION, app);
    return app;
}
#define NOTIFICATION_SAVE_DELAY_MS 500

static uint32_t last_dirty_time = 0;

int32_t notification_srv(void* p) {
    UNUSED(p);
    NotificationApp* app = notification_app_alloc();

    NotificationAppMessage message;
    while(1) {
        uint32_t now = furi_get_tick(); // current tick in ms
        FuriStatus status = furi_message_queue_get(app->queue, &message, 50); // short wait
        if(status == FuriStatusOk) {
            switch(message.type) {
            case NotificationLayerMessage:
                notification_process_notification_message(app, &message);
                break;
            case InternalLayerMessage:
                notification_process_internal_message(app, &message);
                break;
            case SaveSettingsMessage:
                notification_save_settings(app);
                app->settings_dirty = false;
                break;
            case LoadSettingsMessage:
                notification_load_settings(app);
                break;
            }

            if(message.back_event != NULL) {
                furi_event_flag_set(message.back_event, NOTIFICATION_EVENT_COMPLETE);
            }
        }

        // Flush dirty settings if enough time passed
        if(app->settings_dirty && (now - last_dirty_time > NOTIFICATION_SAVE_DELAY_MS)) {
            notification_save_settings(app);
            app->settings_dirty = false;
        }

        if(app->settings_dirty) last_dirty_time = now;
    }

    return 0;
}
