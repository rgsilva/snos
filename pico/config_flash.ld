SECTIONS { .config_guard : { } }
INSERT AFTER .flash_end;
ASSERT(__flash_binary_end <= 0x101fd000, "Firmware overlaps Bluetooth/config flash storage")
