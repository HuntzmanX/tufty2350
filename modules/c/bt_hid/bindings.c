#include <string.h>
#include <stdint.h>

#include "py/runtime.h"
#include "py/obj.h"

#include "ble/att_db_util.h"
#include "ble/att_server.h"
#include "ble/gatt-service/hids_device.h"
#include "btstack_defines.h"

#define UUID_HID_SERVICE          0x1812
#define UUID_PROTOCOL_MODE        0x2A4E
#define UUID_REPORT               0x2A4D
#define UUID_REPORT_MAP           0x2A4B
#define UUID_BOOT_KEYBOARD_INPUT  0x2A22
#define UUID_BOOT_KEYBOARD_OUTPUT 0x2A32
#define UUID_BOOT_MOUSE_INPUT     0x2A33
#define UUID_HID_INFORMATION      0x2A4A
#define UUID_HID_CONTROL_POINT    0x2A4C
#define UUID_REPORT_REFERENCE     0x2908

// Native report layout:
//   Report 1 = keyboard input (8 bytes, no report ID in notification body)
//   Report 2 = keyboard LED output
//   Report 4 = mouse input (4 bytes: buttons, X, Y, wheel)
//
// We deliberately keep exactly three Report characteristics because BTstack's
// hids_device_init() generic storage has room for three reports. Boot keyboard
// and Boot mouse are separate HIDS characteristics and do not consume this
// generic Report storage.
static const uint8_t native_hid_report_map[] = {
    // ---------------------------------------------------------------------
    // Keyboard application, input report ID 1.
    // ---------------------------------------------------------------------
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x06,       // Usage (Keyboard)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x01,       //   Report ID (1)

    // Modifier byte.
    0x05, 0x07,       //   Usage Page (Keyboard)
    0x19, 0xE0,       //   Usage Minimum (Left Control)
    0x29, 0xE7,       //   Usage Maximum (Right GUI)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x01,       //   Logical Maximum (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x08,       //   Report Count (8)
    0x81, 0x02,       //   Input (Data, Variable, Absolute)

    // Reserved byte.
    0x95, 0x01,
    0x75, 0x08,
    0x81, 0x01,       //   Input (Constant)

    // Six keyboard usages.
    0x95, 0x06,
    0x75, 0x08,
    0x15, 0x00,
    0x25, 0x65,
    0x05, 0x07,
    0x19, 0x00,
    0x29, 0x65,
    0x81, 0x00,       //   Input (Data, Array)

    // LED output report ID 2.
    0x85, 0x02,
    0x05, 0x08,       //   Usage Page (LEDs)
    0x19, 0x01,
    0x29, 0x05,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x05,
    0x91, 0x02,       //   Output (Data, Variable, Absolute)
    0x75, 0x03,
    0x95, 0x01,
    0x91, 0x01,       //   Output (Constant)

    0xC0,             // End Keyboard Collection

    // ---------------------------------------------------------------------
    // Mouse application, input report ID 4.
    // Body is exactly four bytes: buttons, relative X, relative Y, wheel.
    // ---------------------------------------------------------------------
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x02,       // Usage (Mouse)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x04,       //   Report ID (4)
    0x09, 0x01,       //   Usage (Pointer)
    0xA1, 0x00,       //   Collection (Physical)

    // Three mouse buttons + five padding bits.
    0x05, 0x09,       //   Usage Page (Button)
    0x19, 0x01,       //   Usage Minimum (1)
    0x29, 0x03,       //   Usage Maximum (3)
    0x15, 0x00,
    0x25, 0x01,
    0x95, 0x03,
    0x75, 0x01,
    0x81, 0x02,       //   Input (Data, Variable, Absolute)
    0x95, 0x01,
    0x75, 0x05,
    0x81, 0x01,       //   Input (Constant)

    // Relative X/Y and wheel.
    0x05, 0x01,
    0x09, 0x30,       //   Usage (X)
    0x09, 0x31,       //   Usage (Y)
    0x09, 0x38,       //   Usage (Wheel)
    0x15, 0x81,       //   Logical Minimum (-127)
    0x25, 0x7F,       //   Logical Maximum (127)
    0x75, 0x08,
    0x95, 0x03,
    0x81, 0x06,       //   Input (Data, Variable, Relative)

    0xC0,             // End Physical Collection
    0xC0,             // End Mouse Collection
};

// BTstack's runtime ATT DB helpers take mutable pointers for static attribute
// values, even though these byte arrays are not modified by our code.
static uint8_t report_ref_keyboard_input[2] = {1, 1};
static uint8_t report_ref_output[2]         = {2, 2};
static uint8_t report_ref_mouse_input[2]    = {4, 1};
static uint8_t hid_information[4]           = {0x01, 0x01, 0x00, 0x02};

static uint16_t keyboard_input_handle = 0;
static uint16_t mouse_input_handle = 0;
static uint16_t boot_keyboard_input_handle = 0;
static uint16_t boot_mouse_input_handle = 0;

static uint8_t keyboard_input_enabled = 0;
static uint8_t mouse_input_enabled = 0;
static uint8_t boot_keyboard_enabled = 0;
static uint8_t boot_mouse_enabled = 0;
static uint8_t protocol_mode = 1;

static void native_hid_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (packet_type != HCI_EVENT_PACKET || size < 3 || packet[0] != HCI_EVENT_HIDS_META) {
        return;
    }

    uint8_t subevent = packet[2];

    switch (subevent) {
        case HIDS_SUBEVENT_INPUT_REPORT_ENABLE:
            // BTstack emits (con_handle, report_id, enable) for generic Input
            // Report subscriptions. With v0.4 we accidentally displayed the
            // report ID as the enabled value; v0.5 tracks both reports properly.
            if (size >= 7) {
                uint8_t report_id = packet[5];
                uint8_t enabled = packet[6];

                if (report_id == 1) {
                    keyboard_input_enabled = enabled;
                } else if (report_id == 4) {
                    mouse_input_enabled = enabled;
                }
            }
            break;

        case HIDS_SUBEVENT_BOOT_KEYBOARD_INPUT_REPORT_ENABLE:
            if (size >= 6) {
                boot_keyboard_enabled = packet[5];
            }
            break;

        case HIDS_SUBEVENT_BOOT_MOUSE_INPUT_REPORT_ENABLE:
            if (size >= 6) {
                boot_mouse_enabled = packet[5];
            }
            break;

        case HIDS_SUBEVENT_PROTOCOL_MODE:
            if (size >= 6) {
                protocol_mode = packet[5];
            }
            break;

        default:
            break;
    }
}

// Called from the patched MicroPython BTstack backend while the ATT DB is being
// built, before att_server_init(). This mirrors BTstack's own HIDS GATT layout.
void tufty_native_hid_db_append(void) {
    keyboard_input_handle = 0;
    mouse_input_handle = 0;
    boot_keyboard_input_handle = 0;
    boot_mouse_input_handle = 0;

    att_db_util_add_service_uuid16(UUID_HID_SERVICE);

    // Protocol Mode.
    att_db_util_add_characteristic_uuid16(
        UUID_PROTOCOL_MODE,
        ATT_PROPERTY_READ | ATT_PROPERTY_WRITE_WITHOUT_RESPONSE | ATT_PROPERTY_DYNAMIC,
        ATT_SECURITY_NONE,
        ATT_SECURITY_NONE,
        NULL,
        0
    );

    // Keyboard Input Report: encrypted, notify, Report Reference = ID 1/Input.
    keyboard_input_handle = att_db_util_add_characteristic_uuid16(
        UUID_REPORT,
        ATT_PROPERTY_READ | ATT_PROPERTY_WRITE | ATT_PROPERTY_NOTIFY | ATT_PROPERTY_DYNAMIC,
        ATT_SECURITY_ENCRYPTED,
        ATT_SECURITY_ENCRYPTED,
        NULL,
        0
    );
    att_db_util_add_descriptor_uuid16(
        UUID_REPORT_REFERENCE,
        ATT_PROPERTY_READ,
        ATT_SECURITY_NONE,
        ATT_SECURITY_NONE,
        report_ref_keyboard_input,
        sizeof(report_ref_keyboard_input)
    );

    // Mouse Input Report: encrypted, notify, Report Reference = ID 4/Input.
    mouse_input_handle = att_db_util_add_characteristic_uuid16(
        UUID_REPORT,
        ATT_PROPERTY_READ | ATT_PROPERTY_WRITE | ATT_PROPERTY_NOTIFY | ATT_PROPERTY_DYNAMIC,
        ATT_SECURITY_ENCRYPTED,
        ATT_SECURITY_ENCRYPTED,
        NULL,
        0
    );
    att_db_util_add_descriptor_uuid16(
        UUID_REPORT_REFERENCE,
        ATT_PROPERTY_READ,
        ATT_SECURITY_NONE,
        ATT_SECURITY_NONE,
        report_ref_mouse_input,
        sizeof(report_ref_mouse_input)
    );

    // Keyboard LED Output Report: ID 2/Output.
    att_db_util_add_characteristic_uuid16(
        UUID_REPORT,
        ATT_PROPERTY_READ | ATT_PROPERTY_WRITE | ATT_PROPERTY_NOTIFY | ATT_PROPERTY_DYNAMIC,
        ATT_SECURITY_ENCRYPTED,
        ATT_SECURITY_ENCRYPTED,
        NULL,
        0
    );
    att_db_util_add_descriptor_uuid16(
        UUID_REPORT_REFERENCE,
        ATT_PROPERTY_READ,
        ATT_SECURITY_NONE,
        ATT_SECURITY_NONE,
        report_ref_output,
        sizeof(report_ref_output)
    );

    // Report Map is supplied dynamically by hids_device.c.
    att_db_util_add_characteristic_uuid16(
        UUID_REPORT_MAP,
        ATT_PROPERTY_READ | ATT_PROPERTY_DYNAMIC,
        ATT_SECURITY_NONE,
        ATT_SECURITY_NONE,
        NULL,
        0
    );

    // Boot keyboard/mouse reports.
    boot_keyboard_input_handle = att_db_util_add_characteristic_uuid16(
        UUID_BOOT_KEYBOARD_INPUT,
        ATT_PROPERTY_READ | ATT_PROPERTY_WRITE | ATT_PROPERTY_NOTIFY | ATT_PROPERTY_DYNAMIC,
        ATT_SECURITY_NONE,
        ATT_SECURITY_NONE,
        NULL,
        0
    );
    att_db_util_add_characteristic_uuid16(
        UUID_BOOT_KEYBOARD_OUTPUT,
        ATT_PROPERTY_READ | ATT_PROPERTY_WRITE | ATT_PROPERTY_WRITE_WITHOUT_RESPONSE | ATT_PROPERTY_DYNAMIC,
        ATT_SECURITY_NONE,
        ATT_SECURITY_NONE,
        NULL,
        0
    );
    boot_mouse_input_handle = att_db_util_add_characteristic_uuid16(
        UUID_BOOT_MOUSE_INPUT,
        ATT_PROPERTY_READ | ATT_PROPERTY_WRITE | ATT_PROPERTY_NOTIFY | ATT_PROPERTY_DYNAMIC,
        ATT_SECURITY_NONE,
        ATT_SECURITY_NONE,
        NULL,
        0
    );

    // HID Information is static, exactly like BTstack's hids.gatt.
    att_db_util_add_characteristic_uuid16(
        UUID_HID_INFORMATION,
        ATT_PROPERTY_READ,
        ATT_SECURITY_NONE,
        ATT_SECURITY_NONE,
        hid_information,
        sizeof(hid_information)
    );

    // HID Control Point.
    att_db_util_add_characteristic_uuid16(
        UUID_HID_CONTROL_POINT,
        ATT_PROPERTY_WRITE_WITHOUT_RESPONSE | ATT_PROPERTY_DYNAMIC,
        ATT_SECURITY_NONE,
        ATT_SECURITY_NONE,
        NULL,
        0
    );
}

// Called immediately after MicroPython calls att_server_init().
void tufty_native_hid_start(void) {
    keyboard_input_enabled = 0;
    mouse_input_enabled = 0;
    boot_keyboard_enabled = 0;
    boot_mouse_enabled = 0;
    protocol_mode = 1;

    // Exactly three generic Report characteristics are present:
    // keyboard input, mouse input and keyboard LED output.
    hids_device_init(
        0,
        native_hid_report_map,
        sizeof(native_hid_report_map)
    );
    hids_device_register_packet_handler(native_hid_packet_handler);
}

static void require_native_hid_ready(uint16_t handle) {
    if (handle == 0) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("native HID service not ready"));
    }
}

static mp_obj_t bt_hid_send_input(mp_obj_t conn_obj, mp_obj_t report_obj) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(report_obj, &buf, MP_BUFFER_READ);
    if (buf.len != 8) {
        mp_raise_ValueError(MP_ERROR_TEXT("keyboard report must be 8 bytes"));
    }
    require_native_hid_ready(keyboard_input_handle);

    int err = hids_device_send_input_report_for_id(
        (hci_con_handle_t)mp_obj_get_int(conn_obj),
        1,
        (const uint8_t *)buf.buf,
        (uint16_t)buf.len
    );
    return MP_OBJ_NEW_SMALL_INT(err);
}
static MP_DEFINE_CONST_FUN_OBJ_2(bt_hid_send_input_obj, bt_hid_send_input);

static mp_obj_t bt_hid_send_mouse(mp_obj_t conn_obj, mp_obj_t report_obj) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(report_obj, &buf, MP_BUFFER_READ);
    if (buf.len != 4) {
        mp_raise_ValueError(MP_ERROR_TEXT("mouse report must be 4 bytes"));
    }
    require_native_hid_ready(mouse_input_handle);

    int err = hids_device_send_input_report_for_id(
        (hci_con_handle_t)mp_obj_get_int(conn_obj),
        4,
        (const uint8_t *)buf.buf,
        (uint16_t)buf.len
    );
    return MP_OBJ_NEW_SMALL_INT(err);
}
static MP_DEFINE_CONST_FUN_OBJ_2(bt_hid_send_mouse_obj, bt_hid_send_mouse);

static mp_obj_t bt_hid_send_boot_keyboard(mp_obj_t conn_obj, mp_obj_t report_obj) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(report_obj, &buf, MP_BUFFER_READ);
    if (buf.len != 8) {
        mp_raise_ValueError(MP_ERROR_TEXT("boot keyboard report must be 8 bytes"));
    }
    require_native_hid_ready(boot_keyboard_input_handle);

    int err = hids_device_send_boot_keyboard_input_report(
        (hci_con_handle_t)mp_obj_get_int(conn_obj),
        (const uint8_t *)buf.buf,
        (uint16_t)buf.len
    );
    return MP_OBJ_NEW_SMALL_INT(err);
}
static MP_DEFINE_CONST_FUN_OBJ_2(bt_hid_send_boot_keyboard_obj, bt_hid_send_boot_keyboard);

static mp_obj_t bt_hid_send_boot_mouse(mp_obj_t conn_obj, mp_obj_t report_obj) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(report_obj, &buf, MP_BUFFER_READ);
    if (buf.len != 3) {
        mp_raise_ValueError(MP_ERROR_TEXT("boot mouse report must be 3 bytes"));
    }
    require_native_hid_ready(boot_mouse_input_handle);

    int err = hids_device_send_boot_mouse_input_report(
        (hci_con_handle_t)mp_obj_get_int(conn_obj),
        (const uint8_t *)buf.buf,
        (uint16_t)buf.len
    );
    return MP_OBJ_NEW_SMALL_INT(err);
}
static MP_DEFINE_CONST_FUN_OBJ_2(bt_hid_send_boot_mouse_obj, bt_hid_send_boot_mouse);

// Backwards-compatible v0.4 status tuple.
static mp_obj_t bt_hid_status(void) {
    mp_obj_t items[4] = {
        MP_OBJ_NEW_SMALL_INT(keyboard_input_enabled),
        MP_OBJ_NEW_SMALL_INT(boot_keyboard_enabled),
        MP_OBJ_NEW_SMALL_INT(boot_mouse_enabled),
        MP_OBJ_NEW_SMALL_INT(protocol_mode),
    };
    return mp_obj_new_tuple(4, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(bt_hid_status_obj, bt_hid_status);

// Extended v0.5 status tuple: keyboard, mouse, boot keyboard, boot mouse, mode.
static mp_obj_t bt_hid_status_ex(void) {
    mp_obj_t items[5] = {
        MP_OBJ_NEW_SMALL_INT(keyboard_input_enabled),
        MP_OBJ_NEW_SMALL_INT(mouse_input_enabled),
        MP_OBJ_NEW_SMALL_INT(boot_keyboard_enabled),
        MP_OBJ_NEW_SMALL_INT(boot_mouse_enabled),
        MP_OBJ_NEW_SMALL_INT(protocol_mode),
    };
    return mp_obj_new_tuple(5, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(bt_hid_status_ex_obj, bt_hid_status_ex);

static mp_obj_t bt_hid_report_map_length(void) {
    return MP_OBJ_NEW_SMALL_INT(sizeof(native_hid_report_map));
}
static MP_DEFINE_CONST_FUN_OBJ_0(bt_hid_report_map_length_obj, bt_hid_report_map_length);

static const mp_rom_map_elem_t bt_hid_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_bt_hid) },
    { MP_ROM_QSTR(MP_QSTR_send_input), MP_ROM_PTR(&bt_hid_send_input_obj) },
    { MP_ROM_QSTR(MP_QSTR_send_mouse), MP_ROM_PTR(&bt_hid_send_mouse_obj) },
    { MP_ROM_QSTR(MP_QSTR_send_boot_keyboard), MP_ROM_PTR(&bt_hid_send_boot_keyboard_obj) },
    { MP_ROM_QSTR(MP_QSTR_send_boot_mouse), MP_ROM_PTR(&bt_hid_send_boot_mouse_obj) },
    { MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&bt_hid_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_status_ex), MP_ROM_PTR(&bt_hid_status_ex_obj) },
    { MP_ROM_QSTR(MP_QSTR_report_map_length), MP_ROM_PTR(&bt_hid_report_map_length_obj) },
};
static MP_DEFINE_CONST_DICT(bt_hid_globals, bt_hid_globals_table);

const mp_obj_module_t bt_hid_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&bt_hid_globals,
};

MP_REGISTER_MODULE(MP_QSTR_bt_hid, bt_hid_module);
