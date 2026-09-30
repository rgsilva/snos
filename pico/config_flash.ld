SECTIONS { .config_guard : { } }
INSERT AFTER .flash_end;
ASSERT(__flash_binary_end <= 0x101ff000, "Firmware overlaps config sector")
