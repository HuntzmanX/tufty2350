from pathlib import Path
import sys

if len(sys.argv) != 2:
    raise SystemExit("Usage: enable_ble_pairing.py <micropython-root>")

root = Path(sys.argv[1])
path = root / "extmod" / "btstack" / "modbluetooth_btstack.c"
text = path.read_text(encoding="utf-8")
original = text

anchor_1 = '''    } else if (event_type == HCI_EVENT_VENDOR_SPECIFIC) {\n        DEBUG_printf("  --> hci vendor specific\\n");\n    } else if (event_type == SM_EVENT_AUTHORIZATION_RESULT ||\n'''
replace_1 = '''    } else if (event_type == HCI_EVENT_VENDOR_SPECIFIC) {\n        DEBUG_printf("  --> hci vendor specific\\n");\n    #if MICROPY_PY_BLUETOOTH_ENABLE_PAIRING_BONDING\n    } else if (event_type == SM_EVENT_JUST_WORKS_REQUEST) {\n        uint16_t conn_handle = sm_event_just_works_request_get_handle(packet);\n        sm_just_works_confirm(conn_handle);\n    #endif\n    } else if (event_type == SM_EVENT_AUTHORIZATION_RESULT ||\n'''

anchor_2 = '''static btstack_packet_callback_registration_t hci_event_callback_registration = {\n    .callback = &btstack_packet_handler_generic\n};\n\n#if MICROPY_PY_BLUETOOTH_ENABLE_GATT_CLIENT\n'''
replace_2 = '''static btstack_packet_callback_registration_t hci_event_callback_registration = {\n    .callback = &btstack_packet_handler_generic\n};\n\n#if MICROPY_PY_BLUETOOTH_ENABLE_PAIRING_BONDING\nstatic btstack_packet_callback_registration_t sm_event_callback_registration = {\n    .callback = &btstack_packet_handler_generic\n};\n#endif\n\n#if MICROPY_PY_BLUETOOTH_ENABLE_GATT_CLIENT\n'''

anchor_3 = '''    // Register for HCI events.\n    hci_add_event_handler(&hci_event_callback_registration);\n\n    // Register for ATT server events.\n'''
replace_3 = '''    // Register for HCI events.\n    hci_add_event_handler(&hci_event_callback_registration);\n\n    #if MICROPY_PY_BLUETOOTH_ENABLE_PAIRING_BONDING\n    // Security Manager events (including Just Works) need their own handler.\n    sm_add_event_handler(&sm_event_callback_registration);\n    #endif\n\n    // Register for ATT server events.\n'''

for i, (anchor, replacement) in enumerate(((anchor_1, replace_1), (anchor_2, replace_2), (anchor_3, replace_3)), 1):
    count = text.count(anchor)
    if count != 1:
        raise SystemExit(f"BLE pairing patch anchor {i} expected once, found {count}")
    text = text.replace(anchor, replacement, 1)

if text == original:
    raise SystemExit("BLE pairing patch made no changes")

path.write_text(text, encoding="utf-8")
print("Applied BLE Just Works pairing support to", path)
