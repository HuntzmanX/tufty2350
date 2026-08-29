from pathlib import Path
import sys

if len(sys.argv) != 2:
    raise SystemExit("Usage: enable_ble_pairing.py <micropython-root>")

root = Path(sys.argv[1])
path = root / "extmod" / "btstack" / "modbluetooth_btstack.c"
text = path.read_text(encoding="utf-8")
original = text

# ---------------------------------------------------------------------------
# 1) Pairing/bonding: deliver Security Manager events and accept Just Works.
# ---------------------------------------------------------------------------
anchor_1 = '''    } else if (event_type == HCI_EVENT_VENDOR_SPECIFIC) {\n        DEBUG_printf("  --> hci vendor specific\\n");\n    } else if (event_type == SM_EVENT_AUTHORIZATION_RESULT ||\n'''
replace_1 = '''    } else if (event_type == HCI_EVENT_VENDOR_SPECIFIC) {\n        DEBUG_printf("  --> hci vendor specific\\n");\n    #if MICROPY_PY_BLUETOOTH_ENABLE_PAIRING_BONDING\n    } else if (event_type == SM_EVENT_JUST_WORKS_REQUEST) {\n        uint16_t conn_handle = sm_event_just_works_request_get_handle(packet);\n        sm_just_works_confirm(conn_handle);\n    #endif\n    } else if (event_type == SM_EVENT_AUTHORIZATION_RESULT ||\n'''

anchor_2 = '''static btstack_packet_callback_registration_t hci_event_callback_registration = {\n    .callback = &btstack_packet_handler_generic\n};\n\n#if MICROPY_PY_BLUETOOTH_ENABLE_GATT_CLIENT\n'''
replace_2 = '''static btstack_packet_callback_registration_t hci_event_callback_registration = {\n    .callback = &btstack_packet_handler_generic\n};\n\n#if MICROPY_PY_BLUETOOTH_ENABLE_PAIRING_BONDING\nstatic btstack_packet_callback_registration_t sm_event_callback_registration = {\n    .callback = &btstack_packet_handler_generic\n};\n#endif\n\n#if MICROPY_PY_BLUETOOTH_ENABLE_GATT_CLIENT\n'''

anchor_3 = '''    // Register for HCI events.\n    hci_add_event_handler(&hci_event_callback_registration);\n\n    // Register for ATT server events.\n'''
replace_3 = '''    // Register for HCI events.\n    hci_add_event_handler(&hci_event_callback_registration);\n\n    #if MICROPY_PY_BLUETOOTH_ENABLE_PAIRING_BONDING\n    // Security Manager events (including Just Works) need their own handler.\n    sm_add_event_handler(&sm_event_callback_registration);\n    #endif\n\n    // Register for ATT server events.\n'''

# ---------------------------------------------------------------------------
# 2) Native HIDS hooks. The Tufty user C module supplies these functions.
#    The DB hook runs before att_server_init; the start hook runs afterwards.
# ---------------------------------------------------------------------------
anchor_4 = '''#include "lib/btstack/src/btstack.h"\n\n#define DEBUG_printf'''
replace_4 = '''#include "lib/btstack/src/btstack.h"\n\n// Tufty native HID hooks, supplied by modules/c/bt_hid/bindings.c.\nextern void tufty_native_hid_db_append(void);\nextern void tufty_native_hid_start(void);\n\n#define DEBUG_printf'''

anchor_5 = '''        att_db_util_add_service_uuid16(0x1801);\n        att_db_util_add_characteristic_uuid16(0x2a05, ATT_PROPERTY_READ, ATT_SECURITY_NONE, ATT_SECURITY_NONE, NULL, 0);\n    }\n\n    return 0;\n}\n'''
replace_5 = '''        att_db_util_add_service_uuid16(0x1801);\n        att_db_util_add_characteristic_uuid16(0x2a05, ATT_PROPERTY_READ, ATT_SECURITY_NONE, ATT_SECURITY_NONE, NULL, 0);\n\n        // Append a standards-shaped, native BTstack HID-over-GATT service.\n        tufty_native_hid_db_append();\n    }\n\n    return 0;\n}\n'''

anchor_6 = '''int mp_bluetooth_gatts_register_service_end(void) {\n    DEBUG_printf("mp_bluetooth_gatts_register_service_end\\n");\n    att_server_init(att_db_util_get_address(), &att_read_callback, &att_write_callback);\n    return 0;\n}\n'''
replace_6 = '''int mp_bluetooth_gatts_register_service_end(void) {\n    DEBUG_printf("mp_bluetooth_gatts_register_service_end\\n");\n    att_server_init(att_db_util_get_address(), &att_read_callback, &att_write_callback);\n\n    // Let BTstack's native HIDS implementation own the HID service range.\n    tufty_native_hid_start();\n    return 0;\n}\n'''

patches = (
    ("pairing event", anchor_1, replace_1),
    ("SM registration", anchor_2, replace_2),
    ("SM handler install", anchor_3, replace_3),
    ("native HID externs", anchor_4, replace_4),
    ("native HID DB hook", anchor_5, replace_5),
    ("native HID start hook", anchor_6, replace_6),
)

for name, anchor, replacement in patches:
    count = text.count(anchor)
    if count != 1:
        raise SystemExit(f"{name} patch anchor expected once, found {count}")
    text = text.replace(anchor, replacement, 1)

if text == original:
    raise SystemExit("BLE/native HID patch made no changes")

path.write_text(text, encoding="utf-8")
print("Applied BLE Just Works + native BTstack HIDS hooks to", path)
