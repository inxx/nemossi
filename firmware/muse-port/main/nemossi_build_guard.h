#pragma once
#include "sdkconfig.h"

/* This port deliberately preserves a board whose eFuses and original
 * bootloader are unchanged. Stop compilation before an unsafe image exists.
 */
#if defined(CONFIG_HOMEHUB_NVS_ENCRYPTION) || defined(CONFIG_NVS_ENCRYPTION) || \
    defined(CONFIG_HOMEHUB_PAIRING_EFUSE_AUTH) || defined(CONFIG_SECURE_BOOT) || \
    defined(CONFIG_SECURE_FLASH_ENC_ENABLED)
#error "Nemossi port forbids automatic eFuse, NVS encryption and hardware security changes"
#endif
#if defined(CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT) || \
    defined(CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT) || \
    defined(CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES)
#error "Nemossi app-only port uses an unsigned application with the original bootloader"
#endif
#if CONFIG_PARTITION_TABLE_OFFSET != 0x8000
#error "Nemossi must use the original partition table at 0x8000"
#endif
#if defined(CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION)
#error "Original Nemossi partition table has no PHY partition"
#endif
#if !defined(CONFIG_ESPTOOLPY_FLASHSIZE_16MB) || CONFIG_MMU_PAGE_SIZE != 0x10000
#error "Nemossi requires 16 MiB flash and original 64 KiB MMU pages"
#endif
#if defined(CONFIG_HOMEHUB_OTA_ENABLED) || defined(CONFIG_HOMEHUB_TUNNEL)
#error "Nemossi initial PTT port does not enable OTA or a home-network tunnel"
#endif
#if defined(CONFIG_HOMEHUB_SUPPORT_BUG_REPORT) || defined(CONFIG_HOMEHUB_DEV_BUILD)
#error "Nemossi initial port does not enable remote bug reports or development data logs"
#endif
