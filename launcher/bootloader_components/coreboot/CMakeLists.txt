# Bootloader component: boots an installed core from the pool partition.
#
# The stock bootloader only boots from app partitions, and the partition table
# is static, so a dynamic pool cannot be expressed in it. Instead we hook
# bootloader_after_init(), verify the selected core, and hand it to the stock
# loader by rewriting the launcher slot's offset in a copy of the partition
# state. If anything is wrong we simply return and the stock path boots the
# launcher, so the device always has a way back in.
idf_component_register(SRCS "coreboot.c"
                       INCLUDE_DIRS "."
                       REQUIRES bootloader_support spi_flash)

# bootloader_utility.h and bootloader_flash_priv.h live in private include dirs.
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${IDF_PATH}/components/bootloader_support/private_include"
    "${IDF_PATH}/components/bootloader_support/bootloader_flash/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../main")
