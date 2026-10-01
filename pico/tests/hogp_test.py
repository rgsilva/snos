"""Host checks for LE advertising and HID-over-GATT keyboard conversion."""
import ctypes
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/hogp_test"
BUILD.mkdir(parents=True, exist_ok=True)
SO = BUILD / "libhogp.so"

subprocess.run([
    "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", "-I", str(ROOT),
    str(ROOT / "hogp.c"), str(ROOT / "hidkeys.c"), "-o", str(SO),
], check=True)


class Advertisement(ctypes.Structure):
    _fields_ = [
        ("hid_service", ctypes.c_bool),
        ("keyboard_appearance", ctypes.c_bool),
        ("complete_name", ctypes.c_bool),
        ("name", ctypes.c_char * 64),
    ]


class KeyboardMap(ctypes.Structure):
    _fields_ = [
        ("valid", ctypes.c_bool),
        ("report_id", ctypes.c_uint8),
        ("modifiers_offset", ctypes.c_uint16),
        ("keys_offset", ctypes.c_uint16),
        ("key_count", ctypes.c_uint8),
    ]


class Keys(ctypes.Structure):
    _fields_ = [
        ("modifiers", ctypes.c_uint8), ("keys", ctypes.c_uint8 * 6),
        ("repeat_key", ctypes.c_uint8), ("repeat_modifiers", ctypes.c_uint8),
        ("repeat_byte", ctypes.c_uint8), ("repeat_at", ctypes.c_uint32),
    ]


lib = ctypes.CDLL(str(SO))
lib.hogp_advertisement_parse.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                         ctypes.POINTER(Advertisement)]
lib.hogp_keyboard_map_parse.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                         ctypes.POINTER(KeyboardMap)]
lib.hogp_keyboard_map_parse.restype = ctypes.c_bool
lib.hogp_keyboard_report_to_boot.argtypes = [ctypes.POINTER(KeyboardMap), ctypes.c_uint8,
                                             ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                             ctypes.POINTER(ctypes.c_uint8)]
lib.hogp_keyboard_report_to_boot.restype = ctypes.c_bool
lib.hidkeys_init.argtypes = [ctypes.POINTER(Keys)]
lib.hidkeys_report.argtypes = [ctypes.POINTER(Keys), ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint8),
                               ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t]
lib.hidkeys_report.restype = ctypes.c_size_t


def raw(data):
    return (ctypes.c_uint8 * len(data))(*data)


def parse_ad(data, previous=None):
    ad = previous or Advertisement()
    value = raw(data)
    lib.hogp_advertisement_parse(value, len(value), ctypes.byref(ad))
    return ad


# Complete names override shortened names received in a later scan response.
ad = parse_ad([4, 8, *b"ERG"])
assert bytes(ad.name).split(b"\0")[0] == b"ERG"
ad = parse_ad([10, 9, *b"ERGO Keys"], ad)
assert bytes(ad.name).split(b"\0")[0] == b"ERGO Keys" and ad.complete_name
assert parse_ad([3, 3, 0x12, 0x18]).hid_service
assert parse_ad([3, 0x19, 0xc1, 3]).keyboard_appearance
assert parse_ad([5, 9, ord("x")]).name == b""  # Truncated AD never reads past its buffer.

# Logitech-style boot layout: report ID, modifiers, reserved byte, six usages.
WITH_ID = bytes([
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01, 0x05, 0x07,
    0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
    0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x19, 0x00,
    0x29, 0x65, 0x81, 0x00, 0xc0,
])
WITHOUT_ID = WITH_ID[:6] + WITH_ID[8:]

for descriptor, report_id in ((WITH_ID, 1), (WITHOUT_ID, 0)):
    desc = raw(descriptor)
    mapping = KeyboardMap()
    assert lib.hogp_keyboard_map_parse(desc, len(desc), ctypes.byref(mapping))
    assert mapping.report_id == report_id
    report = raw([2, 0, 4, 5, 0, 0, 0, 0])
    boot = (ctypes.c_uint8 * 8)()
    assert lib.hogp_keyboard_report_to_boot(ctypes.byref(mapping), report_id, report, len(report), boot)
    assert bytes(boot) == b"\x02\0\x04\x05\0\0\0\0"
    keys, out = Keys(), (ctypes.c_uint8 * 8)()
    lib.hidkeys_init(ctypes.byref(keys))
    count = lib.hidkeys_report(ctypes.byref(keys), 0, boot, out, len(out))
    assert bytes(out[:count]) == b"AB"

no_keyboard = raw([0x05, 0x0c, 0x09, 0x01, 0xa1, 1, 0x75, 8, 0x95, 1, 0x81, 2, 0xc0])
mapping = KeyboardMap()
assert not lib.hogp_keyboard_map_parse(no_keyboard, len(no_keyboard), ctypes.byref(mapping))

print("hogp: advertising parsing, report maps, conversion, and HID translation passed")
