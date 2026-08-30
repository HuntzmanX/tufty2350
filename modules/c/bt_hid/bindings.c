#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "py/runtime.h"
#include "py/obj.h"

#include "ble/att_db_util.h"
#include "ble/att_server.h"
#include "ble/gatt-service/hids_device.h"
#include "btstack.h"

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

// Native report layout is deliberately unchanged from Consumer-HID v0.6.5:
//   Report 1 = keyboard input (8 bytes)
//   Report 3 = Consumer Control input (2-byte usage ID, little-endian)
//   Report 4 = mouse input (4 bytes: buttons, X, Y, wheel)
//
// Keeping the descriptor byte-for-byte compatible avoids forcing already
// bonded hosts to rediscover a different HID profile.
static const uint8_t native_hid_report_map[] = {
    // Keyboard application, input report ID 1.
    0x05, 0x01,
    0x09, 0x06,
    0xA1, 0x01,
    0x85, 0x01,

    0x05, 0x07,
    0x19, 0xE0,
    0x29, 0xE7,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x08,
    0x81, 0x02,

    0x95, 0x01,
    0x75, 0x08,
    0x81, 0x01,

    0x95, 0x06,
    0x75, 0x08,
    0x15, 0x00,
    0x25, 0x65,
    0x05, 0x07,
    0x19, 0x00,
    0x29, 0x65,
    0x81, 0x00,

    0xC0,

    // Consumer Control application, input report ID 3.
    0x05, 0x0C,
    0x09, 0x01,
    0xA1, 0x01,
    0x85, 0x03,
    0x15, 0x00,
    0x26, 0xFF, 0x02,
    0x19, 0x00,
    0x2A, 0xFF, 0x02,
    0x75, 0x10,
    0x95, 0x01,
    0x81, 0x00,
    0xC0,

    // Mouse application, input report ID 4.
    0x05, 0x01,
    0x09, 0x02,
    0xA1, 0x01,
    0x85, 0x04,
    0x09, 0x01,
    0xA1, 0x00,

    0x05, 0x09,
    0x19, 0x01,
    0x29, 0x03,
    0x15, 0x00,
    0x25, 0x01,
    0x95, 0x03,
    0x75, 0x01,
    0x81, 0x02,
    0x95, 0x01,
    0x75, 0x05,
    0x81, 0x01,

    0x05, 0x01,
    0x09, 0x30,
    0x09, 0x31,
    0x09, 0x38,
    0x15, 0x81,
    0x25, 0x7F,
    0x75, 0x08,
    0x95, 0x03,
    0x81, 0x06,

    0xC0,
    0xC0,
};

static uint8_t report_ref_keyboard_input[2] = {1, 1};
static uint8_t report_ref_consumer_input[2] = {3, 1};
static uint8_t report_ref_mouse_input[2]    = {4, 1};
static uint8_t hid_information[4]           = {0x01, 0x01, 0x00, 0x02};

static uint16_t keyboard_input_handle = 0;
static uint16_t consumer_input_handle = 0;
static uint16_t mouse_input_handle = 0;
static uint16_t boot_keyboard_input_handle = 0;
static uint16_t boot_mouse_input_handle = 0;

static uint8_t keyboard_input_enabled = 0;
static uint8_t consumer_input_enabled = 0;
static uint8_t mouse_input_enabled = 0;
static uint8_t boot_keyboard_enabled = 0;
static uint8_t boot_mouse_enabled = 0;
static uint8_t protocol_mode = 1;

// Pairing is locked unless the app explicitly opens a pairing window.
// Re-encryption by already bonded devices does not require Just Works approval.
static bool native_pairing_allowed = false;

// Called by the MicroPython BTstack patch before accepting/declining
// SM_EVENT_JUST_WORKS_REQUEST.
bool tufty_native_pairing_allowed(void) {
    return native_pairing_allowed;
}

static void native_hid_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (packet_type != HCI_EVENT_PACKET || size < 3 || packet[0] != HCI_EVENT_HIDS_META) {
        return;
    }

    uint8_t subevent = packet[2];

    switch (subevent) {
        case HIDS_SUBEVENT_INPUT_REPORT_ENABLE:
            if (size >= 7) {
                uint8_t report_id = packet[5];
                uint8_t enabled = packet[6];

                if (report_id == 1) {
                    keyboard_input_enabled = enabled;
                } else if (report_id == 3) {
                    consumer_input_enabled = enabled;
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

void tufty_native_hid_db_append(void) {
    keyboard_input_handle = 0;
    consumer_input_handle = 0;
    mouse_input_handle = 0;
    boot_keyboard_input_handle = 0;
    boot_mouse_input_handle = 0;

    att_db_util_add_service_uuid16(UUID_HID_SERVICE);

    att_db_util_add_characteristic_uuid16(
        UUID_PROTOCOL_MODE,
        ATT_PROPERTY_READ | ATT_PROPERTY_WRITE_WITHOUT_RESPONSE | ATT_PROPERTY_DYNAMIC,
        ATT_SECURITY_NONE,
        ATT_SECURITY_NONE,
        NULL,
        0
    );

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

    consumer_input_handle = att_db_util_add_characteristic_uuid16(
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
        report_ref_consumer_input,
        sizeof(report_ref_consumer_input)
    );

    att_db_util_add_characteristic_uuid16(
        UUID_REPORT_MAP,
        ATT_PROPERTY_READ | ATT_PROPERTY_DYNAMIC,
        ATT_SECURITY_NONE,
        ATT_SECURITY_NONE,
        NULL,
        0
    );

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

    att_db_util_add_characteristic_uuid16(
        UUID_HID_INFORMATION,
        ATT_PROPERTY_READ,
        ATT_SECURITY_NONE,
        ATT_SECURITY_NONE,
        hid_information,
        sizeof(hid_information)
    );

    att_db_util_add_characteristic_uuid16(
        UUID_HID_CONTROL_POINT,
        ATT_PROPERTY_WRITE_WITHOUT_RESPONSE | ATT_PROPERTY_DYNAMIC,
        ATT_SECURITY_NONE,
        ATT_SECURITY_NONE,
        NULL,
        0
    );
}

void tufty_native_hid_start(void) {
    keyboard_input_enabled = 0;
    consumer_input_enabled = 0;
    mouse_input_enabled = 0;
    boot_keyboard_enabled = 0;
    boot_mouse_enabled = 0;
    protocol_mode = 1;
    native_pairing_allowed = false;

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

    int err = att_server_notify(
        (hci_con_handle_t)mp_obj_get_int(conn_obj),
        keyboard_input_handle,
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

    int err = att_server_notify(
        (hci_con_handle_t)mp_obj_get_int(conn_obj),
        mouse_input_handle,
        (const uint8_t *)buf.buf,
        (uint16_t)buf.len
    );
    return MP_OBJ_NEW_SMALL_INT(err);
}
static MP_DEFINE_CONST_FUN_OBJ_2(bt_hid_send_mouse_obj, bt_hid_send_mouse);

static mp_obj_t bt_hid_send_consumer(mp_obj_t conn_obj, mp_obj_t report_obj) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(report_obj, &buf, MP_BUFFER_READ);
    if (buf.len != 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("consumer report must be 2 bytes"));
    }
    require_native_hid_ready(consumer_input_handle);

    int err = att_server_notify(
        (hci_con_handle_t)mp_obj_get_int(conn_obj),
        consumer_input_handle,
        (const uint8_t *)buf.buf,
        (uint16_t)buf.len
    );
    return MP_OBJ_NEW_SMALL_INT(err);
}
static MP_DEFINE_CONST_FUN_OBJ_2(bt_hid_send_consumer_obj, bt_hid_send_consumer);

static mp_obj_t bt_hid_send_boot_keyboard(mp_obj_t conn_obj, mp_obj_t report_obj) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(report_obj, &buf, MP_BUFFER_READ);
    if (buf.len != 8) {
        mp_raise_ValueError(MP_ERROR_TEXT("boot keyboard report must be 8 bytes"));
    }
    require_native_hid_ready(boot_keyboard_input_handle);

    int err = att_server_notify(
        (hci_con_handle_t)mp_obj_get_int(conn_obj),
        boot_keyboard_input_handle,
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

    int err = att_server_notify(
        (hci_con_handle_t)mp_obj_get_int(conn_obj),
        boot_mouse_input_handle,
        (const uint8_t *)buf.buf,
        (uint16_t)buf.len
    );
    return MP_OBJ_NEW_SMALL_INT(err);
}
static MP_DEFINE_CONST_FUN_OBJ_2(bt_hid_send_boot_mouse_obj, bt_hid_send_boot_mouse);

// -------------------------------------------------------------------------
// Multi-host / Bluetooth identity management
// -------------------------------------------------------------------------

static mp_obj_t bt_hid_set_pairing_enabled(mp_obj_t enabled_obj) {
    native_pairing_allowed = mp_obj_is_true(enabled_obj);
    return mp_obj_new_bool(native_pairing_allowed);
}
static MP_DEFINE_CONST_FUN_OBJ_1(bt_hid_set_pairing_enabled_obj, bt_hid_set_pairing_enabled);

static mp_obj_t bt_hid_pairing_enabled(void) {
    return mp_obj_new_bool(native_pairing_allowed);
}
static MP_DEFINE_CONST_FUN_OBJ_0(bt_hid_pairing_enabled_obj, bt_hid_pairing_enabled);

static mp_obj_t bt_hid_set_identity_public(void) {
    // Identity changes are only safe while not connected. The app always
    // stops advertising and disconnects first; disable advertising here too
    // as a defensive measure.
    gap_advertisements_enable(false);
    gap_random_address_set_mode(GAP_RANDOM_ADDRESS_TYPE_OFF);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(bt_hid_set_identity_public_obj, bt_hid_set_identity_public);

static mp_obj_t bt_hid_set_identity_random(mp_obj_t addr_obj) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(addr_obj, &buf, MP_BUFFER_READ);

    if (buf.len != 6) {
        mp_raise_ValueError(MP_ERROR_TEXT("identity address must be 6 bytes"));
    }

    bd_addr_t addr;
    memcpy(addr, buf.buf, 6);

    // Force Bluetooth Static Random address marker (top two bits = 1).
    addr[0] = (uint8_t)((addr[0] & 0x3f) | 0xc0);

    gap_advertisements_enable(false);
    gap_random_address_set(addr);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(bt_hid_set_identity_random_obj, bt_hid_set_identity_random);

static mp_obj_t bt_hid_identity_info(void) {
    uint8_t addr_type = BD_ADDR_TYPE_UNKNOWN;
    bd_addr_t addr = {0};

    gap_le_get_own_address(&addr_type, addr);

    mp_obj_t items[2] = {
        MP_OBJ_NEW_SMALL_INT(addr_type),
        mp_obj_new_bytes(addr, sizeof(addr)),
    };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(bt_hid_identity_info_obj, bt_hid_identity_info);

static mp_obj_t bt_hid_disconnect(mp_obj_t conn_obj) {
    hci_con_handle_t handle = (hci_con_handle_t)mp_obj_get_int(conn_obj);
    gap_disconnect(handle);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(bt_hid_disconnect_obj, bt_hid_disconnect);

static mp_obj_t bt_hid_connection_info(mp_obj_t conn_obj) {
    hci_con_handle_t handle = (hci_con_handle_t)mp_obj_get_int(conn_obj);
    hci_connection_t *connection = hci_connection_for_handle(handle);

    if (connection == NULL) {
        return mp_const_none;
    }

    sm_connection_t *sm = &connection->sm_connection;
    bool bonded = sm->sm_le_db_index >= 0;

    // (peer_addr_type, peer_addr, encrypted, authenticated, bonded,
    //  key_size, le_db_index, own_addr_type, own_addr)
    mp_obj_t items[9] = {
        MP_OBJ_NEW_SMALL_INT(connection->address_type),
        mp_obj_new_bytes(connection->address, sizeof(connection->address)),
        mp_obj_new_bool(sm->sm_connection_encrypted != 0),
        mp_obj_new_bool(sm->sm_connection_authenticated != 0),
        mp_obj_new_bool(bonded),
        MP_OBJ_NEW_SMALL_INT(sm->sm_actual_encryption_key_size),
        MP_OBJ_NEW_SMALL_INT(sm->sm_le_db_index),
        MP_OBJ_NEW_SMALL_INT(sm->sm_own_addr_type),
        mp_obj_new_bytes(sm->sm_own_address, sizeof(sm->sm_own_address)),
    };
    return mp_obj_new_tuple(9, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(bt_hid_connection_info_obj, bt_hid_connection_info);

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

// Extended v0.5 status tuple remains exactly five items for app compatibility.
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

static mp_obj_t bt_hid_consumer_status(void) {
    return MP_OBJ_NEW_SMALL_INT(consumer_input_enabled);
}
static MP_DEFINE_CONST_FUN_OBJ_0(bt_hid_consumer_status_obj, bt_hid_consumer_status);

static mp_obj_t bt_hid_report_map_length(void) {
    return MP_OBJ_NEW_SMALL_INT(sizeof(native_hid_report_map));
}
static MP_DEFINE_CONST_FUN_OBJ_0(bt_hid_report_map_length_obj, bt_hid_report_map_length);

static const mp_rom_map_elem_t bt_hid_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_bt_hid) },
    { MP_ROM_QSTR(MP_QSTR_send_input), MP_ROM_PTR(&bt_hid_send_input_obj) },
    { MP_ROM_QSTR(MP_QSTR_send_mouse), MP_ROM_PTR(&bt_hid_send_mouse_obj) },
    { MP_ROM_QSTR(MP_QSTR_send_consumer), MP_ROM_PTR(&bt_hid_send_consumer_obj) },
    { MP_ROM_QSTR(MP_QSTR_send_boot_keyboard), MP_ROM_PTR(&bt_hid_send_boot_keyboard_obj) },
    { MP_ROM_QSTR(MP_QSTR_send_boot_mouse), MP_ROM_PTR(&bt_hid_send_boot_mouse_obj) },

    { MP_ROM_QSTR(MP_QSTR_set_pairing_enabled), MP_ROM_PTR(&bt_hid_set_pairing_enabled_obj) },
    { MP_ROM_QSTR(MP_QSTR_pairing_enabled), MP_ROM_PTR(&bt_hid_pairing_enabled_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_identity_public), MP_ROM_PTR(&bt_hid_set_identity_public_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_identity_random), MP_ROM_PTR(&bt_hid_set_identity_random_obj) },
    { MP_ROM_QSTR(MP_QSTR_identity_info), MP_ROM_PTR(&bt_hid_identity_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&bt_hid_disconnect_obj) },
    { MP_ROM_QSTR(MP_QSTR_connection_info), MP_ROM_PTR(&bt_hid_connection_info_obj) },

    { MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&bt_hid_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_status_ex), MP_ROM_PTR(&bt_hid_status_ex_obj) },
    { MP_ROM_QSTR(MP_QSTR_consumer_status), MP_ROM_PTR(&bt_hid_consumer_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_report_map_length), MP_ROM_PTR(&bt_hid_report_map_length_obj) },
};
static MP_DEFINE_CONST_DICT(bt_hid_globals, bt_hid_globals_table);

const mp_obj_module_t bt_hid_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&bt_hid_globals,
};

MP_REGISTER_MODULE(MP_QSTR_bt_hid, bt_hid_module);
