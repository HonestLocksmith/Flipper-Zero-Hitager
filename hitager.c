#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_serial.h>
#include <furi_hal_serial_control.h>
#include <gui/gui.h>
#include <input/input.h>

#define HITAGER_BAUD 115200
#define HITAGER_CMD_TIMEOUT_MS 2500
#define HITAGER_RX_STREAM_SIZE 1024

/* Replace this with a real r... sequence when you want a canned VVDI/SuperChip raw command. */
#define HITAGER_RAW_PRESET "r050000"

typedef enum {
    HitagerEventTypeInput,
} HitagerEventType;

typedef struct {
    HitagerEventType type;
    InputEvent input;
} HitagerEvent;

typedef struct {
    FuriMessageQueue* event_queue;
    FuriStreamBuffer* rx_stream;
    FuriHalSerialHandle* serial;
    ViewPort* view_port;
    Gui* gui;

    FuriString* last_cmd;
    FuriString* last_resp;
    FuriString* status;
    FuriString* log1;
    FuriString* log2;
    FuriString* log3;

    bool rf_on;
    bool uart_ok;
    bool exit;
} HitagerApp;

static void hitager_log_line(HitagerApp* app, const char* line) {
    furi_string_set(app->log3, furi_string_get_cstr(app->log2));
    furi_string_set(app->log2, furi_string_get_cstr(app->log1));
    furi_string_set(app->log1, line);
}

static void hitager_serial_rx_callback(
    FuriHalSerialHandle* handle,
    FuriHalSerialRxEvent event,
    void* context) {
    UNUSED(handle);
    HitagerApp* app = context;

    if(event == FuriHalSerialRxEventData) {
        uint8_t data = furi_hal_serial_async_rx(app->serial);
        furi_stream_buffer_send(app->rx_stream, &data, 1, 0);
    }
}

static void hitager_input_callback(InputEvent* input_event, void* context) {
    HitagerApp* app = context;
    HitagerEvent event = {.type = HitagerEventTypeInput, .input = *input_event};
    furi_message_queue_put(app->event_queue, &event, 0);
}

static void hitager_draw_callback(Canvas* canvas, void* context) {
    HitagerApp* app = context;

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Hitager UART");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 22, furi_string_get_cstr(app->status));

    char line[64];
    snprintf(line, sizeof(line), "RF:%s CMD:%s", app->rf_on ? "ON" : "OFF", furi_string_get_cstr(app->last_cmd));
    canvas_draw_str(canvas, 2, 34, line);

    snprintf(line, sizeof(line), "RESP:%s", furi_string_get_cstr(app->last_resp));
    canvas_draw_str(canvas, 2, 44, line);

    canvas_draw_str(canvas, 2, 54, furi_string_get_cstr(app->log1));
    canvas_draw_str(canvas, 2, 63, "OK ID  UP RFON  DN RFOFF");
}

static void hitager_drain_rx(HitagerApp* app) {
    uint8_t b;
    while(furi_stream_buffer_receive(app->rx_stream, &b, 1, 0) == 1) {
    }
}

static bool hitager_read_line(HitagerApp* app, FuriString* line, uint32_t timeout_ms) {
    furi_string_reset(line);
    uint32_t start = furi_get_tick();

    while((furi_get_tick() - start) < furi_ms_to_ticks(timeout_ms)) {
        uint8_t b = 0;
        if(furi_stream_buffer_receive(app->rx_stream, &b, 1, 25) == 1) {
            if(b == '\r') continue;
            if(b == '\n') {
                if(furi_string_size(line) > 0) return true;
            } else if(furi_string_size(line) < 120) {
                char tmp[2] = {(char)b, 0};
                furi_string_cat(line, tmp);
            }
        }
    }
    return false;
}

bool hitager_cmd(HitagerApp* app, const char* cmd, FuriString* out_resp) {
    if(!app->uart_ok || app->serial == NULL) {
        furi_string_set(app->status, "UART not available");
        return false;
    }

    furi_string_set(app->last_cmd, cmd);
    furi_string_reset(out_resp);
    hitager_drain_rx(app);

    furi_hal_serial_tx(app->serial, (const uint8_t*)cmd, strlen(cmd));
    furi_hal_serial_tx_wait_complete(app->serial);

    bool ok = false;
    bool saw_eof = false;
    FuriString* line = furi_string_alloc();

    while(hitager_read_line(app, line, HITAGER_CMD_TIMEOUT_MS)) {
        const char* s = furi_string_get_cstr(line);
        hitager_log_line(app, s);

        if(strncmp(s, "RESP:", 5) == 0) {
            furi_string_set(out_resp, s + 5);
            ok = true;
        } else if(strstr(s, "RFON")) {
            app->rf_on = true;
            furi_string_set(app->status, "RF on");
            ok = true;
        } else if(strstr(s, "RFOFF")) {
            app->rf_on = false;
            furi_string_set(app->status, "RF off");
            ok = true;
        } else if(strstr(s, "EOF")) {
            saw_eof = true;
            break;
        }

        view_port_update(app->view_port);
    }

    if(!ok && !saw_eof) {
        furi_string_set(app->status, "TIMEOUT / no EOF");
        furi_string_set(out_resp, "TIMEOUT");
    } else if(ok && furi_string_size(app->status) == 0) {
        furi_string_set(app->status, "OK");
    } else if(ok && !strstr(furi_string_get_cstr(app->status), "RF")) {
        furi_string_set(app->status, "OK");
    }

    furi_string_free(line);
    view_port_update(app->view_port);
    return ok;
}

bool hitager_rf_on(HitagerApp* app) {
    return hitager_cmd(app, "o", app->last_resp);
}

bool hitager_rf_off(HitagerApp* app) {
    return hitager_cmd(app, "f", app->last_resp);
}

bool hitager_read_id(HitagerApp* app) {
    return hitager_cmd(app, "i05C0", app->last_resp);
}

bool hitager_version(HitagerApp* app) {
    return hitager_cmd(app, "v", app->last_resp);
}

bool hitager_raw(HitagerApp* app, const char* rcmd) {
    return hitager_cmd(app, rcmd, app->last_resp);
}

static HitagerApp* hitager_app_alloc(void) {
    HitagerApp* app = malloc(sizeof(HitagerApp));
    memset(app, 0, sizeof(HitagerApp));

    app->event_queue = furi_message_queue_alloc(8, sizeof(HitagerEvent));
    app->rx_stream = furi_stream_buffer_alloc(HITAGER_RX_STREAM_SIZE, 1);
    app->last_cmd = furi_string_alloc_set("-");
    app->last_resp = furi_string_alloc_set("-");
    app->status = furi_string_alloc_set("Starting");
    app->log1 = furi_string_alloc_set("");
    app->log2 = furi_string_alloc_set("");
    app->log3 = furi_string_alloc_set("");

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, hitager_draw_callback, app);
    view_port_input_callback_set(app->view_port, hitager_input_callback, app);

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    if(furi_hal_serial_control_is_busy(FuriHalSerialIdUsart)) {
        furi_string_set(app->status, "USART busy");
        app->uart_ok = false;
    } else {
        app->serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
        if(app->serial) {
            furi_hal_serial_init(app->serial, HITAGER_BAUD);
            furi_hal_serial_async_rx_start(app->serial, hitager_serial_rx_callback, app, false);
            app->uart_ok = true;
            furi_string_set(app->status, "UART 115200 ready");
        } else {
            furi_string_set(app->status, "No USART handle");
            app->uart_ok = false;
        }
    }

    view_port_update(app->view_port);
    return app;
}

static void hitager_app_free(HitagerApp* app) {
    if(app->serial) {
        furi_hal_serial_async_rx_stop(app->serial);
        furi_hal_serial_deinit(app->serial);
        furi_hal_serial_control_release(app->serial);
    }

    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);

    furi_string_free(app->last_cmd);
    furi_string_free(app->last_resp);
    furi_string_free(app->status);
    furi_string_free(app->log1);
    furi_string_free(app->log2);
    furi_string_free(app->log3);
    furi_stream_buffer_free(app->rx_stream);
    furi_message_queue_free(app->event_queue);
    free(app);
}

int32_t hitager_app(void* p) {
    UNUSED(p);
    HitagerApp* app = hitager_app_alloc();

    HitagerEvent event;
    while(!app->exit) {
        if(furi_message_queue_get(app->event_queue, &event, FuriWaitForever) == FuriStatusOk) {
            if(event.type == HitagerEventTypeInput && event.input.type == InputTypeShort) {
                switch(event.input.key) {
                case InputKeyBack:
                    app->exit = true;
                    break;
                case InputKeyOk:
                    hitager_read_id(app);
                    break;
                case InputKeyUp:
                    hitager_rf_on(app);
                    break;
                case InputKeyDown:
                    hitager_rf_off(app);
                    break;
                case InputKeyRight:
                    hitager_version(app);
                    break;
                case InputKeyLeft:
                    hitager_raw(app, HITAGER_RAW_PRESET);
                    break;
                default:
                    break;
                }
                view_port_update(app->view_port);
            }
        }
    }

    hitager_app_free(app);
    return 0;
}
