/*
 * Copyright (c) 2014, Intel Corporation
 * All rights reserved.
 *
 * Author: Andrew Boie <andrew.p.boie@intel.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer
 *      in the documentation and/or other materials provided with the
 *      distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */


#include <efi.h>
#include <efiapi.h>
#include <efilib.h>

#ifndef USERFASTBOOT
#include <fastboot.h>
#endif

#include "vars.h"
#include "lib.h"
#include "security.h"
#include "android.h"
#include "ux.h"
#include "options.h"
#include "power.h"
#include "targets.h"
#include "unittest.h"
#include "em.h"
#include "storage.h"
#include "version.h"
#ifdef HAL_AUTODETECT
#include "blobstore.h"
#endif
#include "oemvars.h"

/* Ensure this is embedded in the EFI binary somewhere */
static const char __attribute__((used)) magic[] = "### KERNELFLINGER ###";

/* Default max wait time for console reset in units of milliseconds if no EFI
 * variable is set for this platform.
 * You want this value as small as possible as this is added to
 * the boot time for EVERY boot */
#define EFI_RESET_WAIT_MS           200

/* Interval in ms to check on startup for initial press of magic key */
#define DETECT_KEY_STALL_TIME_MS    1

/* How long (in milliseconds) magic key should be held to force
 * Fastboot mode */
#define FASTBOOT_HOLD_DELAY         (2 * 1000)

/* Magic key to enter fastboot mode or revovery console */
#define MAGIC_KEY          EV_DOWN

/* If we find this in the root of the EFI system partition, unconditionally
 * enter Fastboot mode */
#define FASTBOOT_SENTINEL         L"\\force_fastboot"

/* Paths to interesting alternate boot images */
#define FASTBOOT_PATH             L"\\fastboot.img"
#define TDOS_PATH                 L"\\tdos.img"

/* BIOS Capsule update file */
#define FWUPDATE_FILE             L"\\BIOSUPDATE.fv"

/* Crash event menu settings:
 * - Maximum number of watchdog resets in a row before the crash event
 *   menu is displayed. */
#define WATCHDOG_COUNTER_MAX 2
/* - Maximum time between the first and the last watchdog reset.  If
 *   the current difference exceeds this constant, the watchdog
 *   counter is reset to zero. */
#define WATCHDOG_DELAY       (10 * 60)

static EFI_HANDLE g_disk_device;
static EFI_LOADED_IMAGE *g_loaded_image;

extern struct {
        UINT32 oem_keystore_size;
        UINT32 oem_key_size;
        UINT32 oem_keystore_offset;
        UINT32 oem_key_offset;
} oem_keystore_table;

static VOID *oem_keystore;
static UINTN oem_keystore_size;

static VOID *oem_key;
static UINTN oem_key_size;


#ifdef USERDEBUG
/* If a user-provided keystore is present it must be selected for later.
 * If no user-provided keystore is present then the original factory
 * keystore must be selected instead. Selection of a keystore is
 * independent of validation of that keystore. */
static VOID select_keystore(VOID **keystore, UINTN *size)
{
        EFI_STATUS ret;

        ret = get_user_keystore(keystore, size);
        if (EFI_ERROR(ret)) {
                debug(L"selected OEM keystore");
                *keystore = oem_keystore;
                *size = oem_keystore_size;
        } else {
                debug(L"selected User-supplied keystore");
        }
}
#endif


static enum boot_target check_fastboot_sentinel(VOID)
{
        debug(L"checking ESP for %s", FASTBOOT_SENTINEL);
        if (file_exists(g_disk_device, FASTBOOT_SENTINEL))
                return FASTBOOT;
        return NORMAL_BOOT;
}


static enum boot_target check_magic_key(VOID)
{
        unsigned long i;
        EFI_STATUS ret = EFI_NOT_READY;
        EFI_INPUT_KEY key;
#ifdef USERFASTBOOT
        enum boot_target bt;
#endif
        unsigned long wait_ms = EFI_RESET_WAIT_MS;

        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);

        /* Some systems require a short stall before we can be sure there
         * wasn't a keypress at boot. Read the EFI variable which determines
         * that time for this platform */
        ret = get_efi_variable_long_from_str8(&loader_guid,
                                             MAGIC_KEY_TIMEOUT_VAR,
                                             &wait_ms);
        if (EFI_ERROR(ret)) {
                debug(L"Couldn't read timeout variable; assuming default");
        } else {
                if (wait_ms > 1000) {
                        debug(L"pathological magic key timeout, use default");
                        wait_ms = EFI_RESET_WAIT_MS;
                }
        }

        debug(L"Reset wait time: %d", wait_ms);

        /* Check for 'magic' key. Some BIOSes are flaky about this
         * so wait for the ConIn to be ready after reset */
        for (i = 0; i <= wait_ms; i += DETECT_KEY_STALL_TIME_MS) {
                ret = uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2,
                                        ST->ConIn, &key);
                if (ret == EFI_SUCCESS || i == wait_ms)
                        break;
                uefi_call_wrapper(BS->Stall, 1, DETECT_KEY_STALL_TIME_MS * 1000);
        }

        if (EFI_ERROR(ret))
                return NORMAL_BOOT;

        debug(L"ReadKeyStroke: (%d tries) %d %d", i, key.ScanCode, key.UnicodeChar);
        if (ui_keycode_to_event(key.ScanCode) != MAGIC_KEY)
                return NORMAL_BOOT;

#ifdef USERFASTBOOT
        Print(L"Continue holding key for %d second(s) to enter Fastboot mode.\n",
              FASTBOOT_HOLD_DELAY / 1000);
        Print(L"Release key now to load Recovery Console...");
        if (ui_enforce_key_held(FASTBOOT_HOLD_DELAY, MAGIC_KEY)) {
                bt = FASTBOOT;
                Print(L"FASTBOOT\n");
        } else {
                bt = RECOVERY;
                Print(L"RECOVERY\n");
        }
        return bt;
#else
        if (ui_enforce_key_held(FASTBOOT_HOLD_DELAY, MAGIC_KEY))
                return FASTBOOT;
#endif

        return NORMAL_BOOT;
}


static enum boot_target check_bcb(CHAR16 **target_path, BOOLEAN *oneshot)
{
        EFI_STATUS ret;
        struct bootloader_message bcb;
        CHAR16 *target = NULL;
        enum boot_target t;

        *oneshot = FALSE;
        *target_path = NULL;

        ret = read_bcb(MISC_LABEL, &bcb);
        if (EFI_ERROR(ret)) {
                error(L"Unable to read BCB");
                t = NORMAL_BOOT;
                goto out;
        }

        /* We own the status field; clear it in case there is any stale data */
        bcb.status[0] = '\0';

        if (!strncmpa(bcb.command, (CHAR8 *)"boot-", 5)) {
                target = stra_to_str(bcb.command + 5);
                debug(L"BCB boot target: '%s'", target);
        } else if (!strncmpa(bcb.command, (CHAR8 *)"bootonce-", 9)) {
                target = stra_to_str(bcb.command + 9);
                bcb.command[0] = '\0';
                debug(L"BCB oneshot boot target: '%s'", target);
                *oneshot = TRUE;
        }

        ret = write_bcb(MISC_LABEL, &bcb);
        if (EFI_ERROR(ret))
                error(L"Unable to update BCB contents!");

        if (!target) {
                t = NORMAL_BOOT;
                goto out;
        }

        if (target[0] == L'\\') {
                UINTN len;

                if (!file_exists(g_disk_device, target)) {
                        error(L"Specified BCB file '%s' doesn't exist",
                                        target);
                        t = NORMAL_BOOT;
                        goto out;
                }

                len = StrLen(target);
                if (len > 4) {
                        *target_path = StrDuplicate(target);
                        if (!StrCmp(target + (len - 4), L".efi") ||
                                        !StrCmp(target + (len - 4), L".EFI")) {
                                t = ESP_EFI_BINARY;
                        } else {
                                t = ESP_BOOTIMAGE;
                        }
                        goto out;
                }
                error(L"BCB file '%s' appears to be malformed", target);
                t = NORMAL_BOOT;
                goto out;
        }

        t = name_to_boot_target(target);
        if (t != UNKNOWN_TARGET)
                goto out;

        error(L"Unknown boot target in BCB: '%s'", target);
        t = NORMAL_BOOT;

out:
        FreePool(target);
        return t;
}


static enum boot_target check_loader_entry_one_shot(VOID)
{
        CHAR16 *target;
        enum boot_target ret;

        debug(L"checking %s", LOADER_ENTRY_ONESHOT);
        target = get_efi_variable_str(&loader_guid, LOADER_ENTRY_ONESHOT);

        del_efi_variable(&loader_guid, LOADER_ENTRY_ONESHOT);

        if (!target)
                return NORMAL_BOOT;

        debug(L"target = %s", target);
        ret = name_to_boot_target(target);
        if (ret == UNKNOWN_TARGET) {
                error(L"Unknown oneshot boot target: '%s'", target);
                ret = NORMAL_BOOT;
        } else if (ret == CHARGER && !get_current_off_mode_charge()) {
                debug(L"Off mode charge is not set, powering off.");
                ret = POWER_OFF;
        }

        FreePool(target);
        return ret;
}

static BOOLEAN is_a_leap_year(INTN year)
{
        return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static INTN efi_time_to_ctime(EFI_TIME *time)
{
        UINT8 DAY_OF_MONTH[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        UINTN i;
        INTN days = 0;

        for (i = 1970; i < time->Year; i++)
                days += is_a_leap_year(i) ? 365 : 366;

        if (is_a_leap_year(time->Year))
                DAY_OF_MONTH[1] = 29;

        for (i = 0; i + 1 < time->Month; i++)
                days += DAY_OF_MONTH[i];

        return (days * 24 * 3600) + (time->Hour * 3600)
                + (time->Minute * 60) + time->Second;
}

/* If more than WATCHDOG_COUNTER_MAX watchdog resets in a row happened
 * in less than WATCHDOG_DELAY seconds, the crash event menu is
 * displayed.  This menu informs the user of the situation and let him
 * choose which boot target he wants. */
static enum boot_target check_watchdog(VOID)
{
        EFI_STATUS ret;
        enum reset_sources reset_source;
        UINT8 counter;
        EFI_TIME time_ref, now;
        INTN time_diff;

        if (!get_current_crash_event_menu())
                return NORMAL_BOOT;

        ret = get_watchdog_status(&counter, &time_ref);
        if (EFI_ERROR(ret)) {
                efi_perror(ret, L"Failed to get the watchdog status");
                return NORMAL_BOOT;
        }

        reset_source = rsci_get_reset_source();
        if (reset_source != RESET_KERNEL_WATCHDOG
            && reset_source != RESET_SECURITY_WATCHDOG) {
                if (counter != 0) {
                        ret = reset_watchdog_status();
                        if (EFI_ERROR(ret)) {
                                efi_perror(ret, L"Failed to reset the watchdog status");
                                goto error;
                        }
                }
                return NORMAL_BOOT;
        }
        debug(L"Reset source = %d", reset_source);

        ret = uefi_call_wrapper(RT->GetTime, 2, &now, NULL);
        if (EFI_ERROR(ret)) {
                efi_perror(ret, L"Failed to get the current time");
                goto error;
        }

        if (counter > 0) {
                time_diff = efi_time_to_ctime(&now) - efi_time_to_ctime(&time_ref);
                if (time_diff < 0 || time_diff > WATCHDOG_DELAY)
                        counter = 0;
        }

        if (counter == 0) {
                time_ref = now;
                ret = set_watchdog_time_reference(&now);
                if (EFI_ERROR(ret)) {
                        efi_perror(ret, L"Failed to set the watchdog time reference");
                        goto error;
                }
        }

        counter++;
        debug(L"Reset source = %d : incrementing watchdog counter (%d)", reset_source, counter);

        if (counter <= WATCHDOG_COUNTER_MAX) {
                        ret = set_watchdog_counter(counter);
                        if (EFI_ERROR(ret))
                                efi_perror(ret, L"Failed to set the watchdog counter");
                        goto error;
        }

        ret = reset_watchdog_status();
        if (EFI_ERROR(ret))
                efi_perror(ret, L"Failed to reset the watchdog status");

        return ux_crash_event_prompt_user_for_boot_target();

error:
        return NORMAL_BOOT;
}

static enum boot_target check_command_line(VOID **address)
{
        UINTN argc, pos;
        CHAR16 **argv;
        enum boot_target bt;

        *address = NULL;
        bt = NORMAL_BOOT;

        if (EFI_ERROR(get_argv(g_loaded_image, &argc, &argv)))
                return NORMAL_BOOT;

        for (pos = 0; pos < argc; pos++) {
                debug(L"Argument %d: %s", pos, argv[pos]);

#ifndef USERFASTBOOT
                if (!StrCmp(argv[pos], L"-f")) {
                        bt = FASTBOOT;
                        continue;
                }
#endif
#ifndef USER
                if (!StrCmp(argv[pos], L"-U")) {
                        pos++;
                        unittest_main(pos >= argc ? NULL : argv[pos]);
                        FreePool(argv);
                        return EXIT_SHELL;
                }
#endif
                if (!StrCmp(argv[pos], L"-a")) {
                        pos++;
                        if (pos >= argc) {
                                error(L"-a requires a memory address");
                                goto out;
                        }

#ifdef USERFASTBOOT
                        *address = (VOID *)strtoul16(argv[pos], NULL, 0);
                        bt = MEMORY;
#else
                        /* For compatibility...just ignore the supplied address
                         * and enter Fastboot mode */
                        bt = FASTBOOT;
#endif
                        continue;
                }

                /* If we get here the argument isn't recognized */
                if (pos == 0) {
                        /* EFI is inconsistent and only seems to populate the image
                         * name as argv[0] when called from a shell. Do nothing. */
                        continue;
                } else {
                        error(L"unexpected argument %s", argv[pos]);
                        goto out;
                }
        }

out:
        FreePool(argv);
        return bt;
}

static enum boot_target check_battery_inserted()
{
        enum wake_sources wake_source;

        wake_source = rsci_get_wake_source();
        if (wake_source == WAKE_BATTERY_INSERTED)
                return POWER_OFF;

        return NORMAL_BOOT;
}

static enum boot_target check_charge_mode()
{
        enum wake_sources wake_source;

        if (!get_current_off_mode_charge())
                return NORMAL_BOOT;

        wake_source = rsci_get_wake_source();
        if ((wake_source == WAKE_USB_CHARGER_INSERTED) ||
            (wake_source == WAKE_ACDC_CHARGER_INSERTED)) {
                debug(L"Wake source = %d", wake_source);
                return CHARGER;
        }

        return NORMAL_BOOT;
}

enum boot_target check_battery()
{
        if (is_battery_below_boot_OS_threshold()) {
                BOOLEAN charger_plugged = is_charger_plugged_in();
                debug(L"Battery is below boot OS threshold");
                debug(L"Charger is%s plugged", charger_plugged ? L"" : L" not");
                return charger_plugged ? CHARGER : POWER_OFF;
        }

        return NORMAL_BOOT;
}

/* Policy:
 * 1. Check if we had multiple watchdog reported in a short period of
 *    time.  If so, let the user choose the boot target.
 * 2. Check if the "-a xxxxxxxxx" command line was passed in, if so load an
 *    android boot image from RAM at that location.
 * 3. Check if the fastboot sentinel file \force_fastboot is present, and if
 *    so, force fastboot mode. Use in bootable media.
 * 4. Check for "magic key" being held. Short press loads Recovery. Long press
 *    loads Fastboot.
 * 5. Check if wake source is battery inserted, if so power off
 * 6. Check bootloader control block for a boot target, which could be
 *    the name of a boot image that we know how to read from a partition,
 *    or a boot image file in the ESP. BCB can specify oneshot or persistent
 *    targets.
 * 7. Check LoaderEntryOneShot for a boot target
 * 8. Check if we should go into charge mode or normal boot
 *
 * target_address - If MEMORY returned, physical address to load data
 * target_path - If ESP_EFI_BINARY or ESP_BOOTIMAGE returned, path to the
 *               image on the EFI System Partition
 * oneshot - Whether this is a one-shot boot, indicating that the image at
 *           target_path should be deleted before chainloading
 *
 */
static enum boot_target choose_boot_target(VOID **target_address,
                CHAR16 **target_path, BOOLEAN *oneshot)
{
        enum boot_target ret;

        *target_path = NULL;
        *target_address = NULL;
        *oneshot = TRUE;

        debug(L"Bootlogic: Choosing boot target");

        debug(L"Bootlogic: Check watchdog...");
        ret = check_watchdog();
        if (ret != NORMAL_BOOT)
                goto out;

        debug(L"Bootlogic: Check osloader command line...");
        ret = check_command_line(target_address);
        if (ret != NORMAL_BOOT)
                goto out;

        debug(L"Bootlogic: Check fastboot sentinel...");
        ret = check_fastboot_sentinel();
        if (ret != NORMAL_BOOT) {
                goto out;
        }

        debug(L"Bootlogic: Check magic key...");
        ret = check_magic_key();
        if (ret != NORMAL_BOOT)
                goto out;

        debug(L"Bootlogic: Check battery insertion...");
        ret = check_battery_inserted();
        if (ret != NORMAL_BOOT)
                goto out;

        debug(L"Bootlogic: Check BCB...");
        ret = check_bcb(target_path, oneshot);
        if (ret != NORMAL_BOOT)
                goto out;

        debug(L"Bootlogic: Check reboot target...");
        ret = check_loader_entry_one_shot();
        if (ret != NORMAL_BOOT)
                goto out;

        debug(L"Bootlogic: Check battery level...");
        ret = check_battery();
        if (ret == POWER_OFF)
                ux_display_low_battery(3);
        if (ret != NORMAL_BOOT)
                goto out;

        debug(L"Bootlogic: Check charger insertion...");
        ret = check_charge_mode();

out:
        debug(L"Bootlogic: selected '%s'",  boot_target_description(ret));
        return ret;
}

/* Validate an image against a keystore.
 *
 * boot_target - Boot image to load. Values supported are NORMAL_BOOT, RECOVERY,
 *               and ESP_BOOTIMAGE (for 'fastboot boot')
 * bootimage   - bootimage to validate against the keystore.
 * keystore    - Keystore to validate image with.
 * keystore_size - Size of keystore in bytes
 *
 * Return values:
 * EFI_ACCESS_DENIED - Validation failed against supplied keystore
 */
static EFI_STATUS validate_bootimage(
                IN enum boot_target boot_target,
                IN VOID *bootimage,
                IN VOID *keystore,
                IN UINTN keystore_size)
{
        CHAR16 target[BOOT_TARGET_SIZE];
        CHAR16 *expected;
        CHAR16 *expected2 = NULL;
        EFI_STATUS ret;

        ret = verify_android_boot_image(bootimage, keystore,
                                        keystore_size, target);

        if (EFI_ERROR(ret)) {
                debug(L"boot image doesn't verify");
                return EFI_ACCESS_DENIED;
        }

        switch (boot_target) {
        case NORMAL_BOOT:
                expected = L"/boot";
                /* in case of multistage ota */
                expected2 = L"/recovery";
                break;
        case CHARGER:
                expected = L"/boot";
                break;
        case RECOVERY:
                expected = L"/recovery";
                break;
        case ESP_BOOTIMAGE:
                /* "live" bootable image */
                expected = L"/boot";
#ifdef USERFASTBOOT
                /* Bootable Fastboot image */
                expected2 = L"/fastboot";
#endif
                break;
        default:
                expected = NULL;
        }

        if ((!expected || StrCmp(expected, target)) &&
                        (!expected2 || StrCmp(expected2, target))) {
                debug(L"boot image has unexpected target name");
                return EFI_ACCESS_DENIED;
        }

        return EFI_SUCCESS;
}

/* Load a boot image into RAM. If a keystore is supplied, validate the image
 * against it.
 *
 * boot_target - Boot image to load. Values supported are NORMAL_BOOT, RECOVERY,
 *               and ESP_BOOTIMAGE (for 'fastboot boot')
 * keystore    - Keystore to validate image with. If null, no validation
 *               done.
 * keystore_size - Size of keystore in bytes
 * target_path - Path to load boot image from for ESP_BOOTIMAGE case, ignored
 *               otherwise.
 * bootimage   - Returned allocated pointer value for the loaded boot image.
 * oneshot     - For ESP_BOOTIMAGE case, flag indicating that the image should
 *               be deleted.
 *
 * Return values:
 * EFI_INVALID_PARAMETER - Unsupported boot target type, keystore is not well-formed,
 * or loaded boot image was missing or corrupt
 * EFI_ACCESS_DENIED - Validation failed against supplied keystore, boot image
 * still usable
 */
static EFI_STATUS load_boot_image(
                IN enum boot_target boot_target,
                IN VOID *keystore,
                IN UINTN keystore_size,
                IN CHAR16 *target_path,
                OUT VOID **bootimage,
                IN BOOLEAN oneshot)
{
        EFI_STATUS ret;

        switch (boot_target) {
        case NORMAL_BOOT:
        case CHARGER:
                ret = android_image_load_partition(BOOT_LABEL, bootimage);
                break;
        case RECOVERY:
                ret = android_image_load_partition(RECOVERY_LABEL, bootimage);
                break;
        case ESP_BOOTIMAGE:
                /* "fastboot boot" case */
                ret = android_image_load_file(g_disk_device, target_path, oneshot,
                        bootimage);
                break;
        default:
                *bootimage = NULL;
                return EFI_INVALID_PARAMETER;
        }

        if (EFI_ERROR(ret))
                return ret;

        debug(L"boot image loaded");
        if (keystore)
                ret = validate_bootimage(boot_target, *bootimage, keystore, keystore_size);

        return ret;
}


/* Chainload another EFI application on the ESP with the specified path,
 * optionally deleting the file before entering */
static EFI_STATUS enter_efi_binary(CHAR16 *path, BOOLEAN delete)
{
        EFI_DEVICE_PATH *edp;
        EFI_STATUS ret;
        EFI_HANDLE image;

        edp = FileDevicePath(g_disk_device, path);
        if (!edp) {
                error(L"Couldn't generate a path");
                return EFI_INVALID_PARAMETER;
        }

        ret = uefi_call_wrapper(BS->LoadImage, 6, FALSE, g_parent_image,
                        edp, NULL, 0, &image);
        if (EFI_ERROR(ret)) {
                efi_perror(ret, L"BS->LoadImage '%s'", path);
        } else {
                if (delete) {
                        ret = file_delete(g_disk_device, path);
                        if (EFI_ERROR(ret))
                                efi_perror(ret, L"Couldn't delete %s", path);
                }
                ret = uefi_call_wrapper(BS->StartImage, 3, image, NULL, NULL);
                uefi_call_wrapper(BS->UnloadImage, 1, image);
        }
        FreePool(edp);
        return ret;
}


#define OEMVARS_MAGIC           "#OEMVARS\n"
#define OEMVARS_MAGIC_SZ        9

static EFI_STATUS set_image_oemvars_nocheck(VOID *bootimage)
{
        VOID *oemvars;
        UINT32 osz;
        EFI_STATUS ret;

        ret = get_bootimage_2nd(bootimage, &oemvars, &osz);
        if (ret == EFI_SUCCESS && osz > OEMVARS_MAGIC_SZ &&
            !memcmp(oemvars, OEMVARS_MAGIC, OEMVARS_MAGIC_SZ)) {
                debug(L"secondstage contains raw oemvars");
                return flash_oemvars((CHAR8*)oemvars + OEMVARS_MAGIC_SZ,
                                osz - OEMVARS_MAGIC_SZ);
        }

#ifdef HAL_AUTODETECT
        ret = get_bootimage_blob(bootimage, BLOB_TYPE_OEMVARS, &oemvars, &osz);
        if (EFI_ERROR(ret)) {
                if (ret == EFI_UNSUPPORTED || ret == EFI_NOT_FOUND) {
                        debug(L"No blobstore in this boot image");
                        return EFI_SUCCESS;
                }
                return ret;
        }

        return flash_oemvars(oemvars, osz);
#else
        return EFI_NOT_FOUND;
#endif
}

static EFI_STATUS set_image_oemvars(VOID *bootimage)
{
        if (!get_oemvars_update()) {
                debug(L"OEM vars should be up-to-date");
                return EFI_SUCCESS;
        }
        debug(L"OEM vars may need to be updated");
        set_oemvars_update(FALSE);

        return set_image_oemvars_nocheck(bootimage);
}

static EFI_STATUS load_image(VOID *bootimage, UINT8 boot_state,
                             enum boot_target boot_target)
{
        EFI_STATUS ret;

        /* per bootloaderequirements.pdf */
        if (boot_state != BOOT_STATE_GREEN)
                android_clear_memory();

        set_efi_variable(&fastboot_guid, BOOT_STATE_VAR, sizeof(boot_state),
                        &boot_state, FALSE, TRUE);

        debug(L"chainloading boot image, boot state is %s",
                        boot_state_to_string(boot_state));
        ret = android_image_start_buffer(g_parent_image, bootimage,
                                         boot_target, boot_state, NULL);
        if (EFI_ERROR(ret))
                efi_perror(ret, L"Couldn't load Boot image");

        return ret;
}

static VOID enter_tdos(UINT8 boot_state) __attribute__ ((noreturn));

static VOID enter_tdos(UINT8 boot_state)
{
        EFI_STATUS ret;
        VOID *bootimage;

        ret = android_image_load_file(g_disk_device, TDOS_PATH,
                        FALSE, &bootimage);
        if (EFI_ERROR(ret)) {
                error(L"Couldn't load TDOS image");
                goto die;
        }

#ifdef USERDEBUG
        debug(L"verify TDOS boot image");
        CHAR16 target[BOOT_TARGET_SIZE];
        ret = verify_android_boot_image(bootimage, oem_keystore,
                        oem_keystore_size, target);
        if (EFI_ERROR(ret)) {
                error(L"tdos image not verified");
                goto die;
        }

        if (StrCmp(target, L"/tdos")) {
                error(L"This does not appear to be a tdos image");
                goto die;
        }
#endif
        load_image(bootimage, boot_state, TDOS);
        error(L"Couldn't chainload TDOS image");
die:
        /* Allow plenty of time for the error to be visible before the
         * screen goes blank */
        pause(30);
        halt_system();
}

static VOID enter_fastboot_mode(UINT8 boot_state, VOID *bootimage)
        __attribute__ ((noreturn));


#ifdef USERFASTBOOT

/* Enter Fastboot mode. If bootimage is NULL, load it from the file on the
 * EFI system partition */
static VOID enter_fastboot_mode(UINT8 boot_state, VOID *bootimage)
{
        /* Fastboot is conceptually part of the bootloader itself. That it
         * happens to currently be an Android Boot Image, and not part of the
         * kernelflinger EFI binary, is an implementation detail. Fastboot boot
         * image is not independently replaceable by end user without also
         * replacing the bootloader.  On an ARM device the bootloader/fastboot
         * are a single binary.
         *
         * Entering Fastboot is ALWAYS verified by the OEM Keystore, regardless
         * of the device's current boot state/selected keystore/etc. If it
         * doesn't verify we unconditionally halt the system. */
        EFI_STATUS ret;

        /* Publish the OEM key in a volatile EFI variable so that
         * Userfastboot can use it to validate flashed bootloader images */
        set_efi_variable(&fastboot_guid, OEM_KEY_VAR,
                         oem_key_size, oem_key, FALSE, TRUE);
        set_oemvars_update(TRUE);

        if (!bootimage) {
                ret = android_image_load_file(g_disk_device, FASTBOOT_PATH,
                                FALSE, &bootimage);
                if (EFI_ERROR(ret)) {
                        error(L"Couldn't load Fastboot image");
                        goto die;
                }
        }

#ifdef USERDEBUG
        debug(L"verify Fastboot boot image");
        CHAR16 target[BOOT_TARGET_SIZE];
        ret = verify_android_boot_image(bootimage, oem_keystore,
                        oem_keystore_size, target);
        if (EFI_ERROR(ret)) {
                error(L"Fastboot image not verified");
                goto die;
        }

        if (StrCmp(target, L"/fastboot")) {
                error(L"This does not appear to be a Fastboot image");
                goto die;
        }
#endif
        debug(L"chainloading fastboot, boot state is %s",
                        boot_state_to_string(boot_state));
        load_image(bootimage, boot_state, FASTBOOT);
        error(L"Couldn't chainload Fastboot image");
die:
        /* Allow plenty of time for the error to be visible before the
         * screen goes blank */
        pause(30);
        halt_system();
}

#else


/* Enter Fastboot mode. If fastboot_start() returns a valid pointer,
 * try to start the bootimage pointed to. */
static VOID enter_fastboot_mode(UINT8 boot_state, VOID *bootimage)
{
        EFI_STATUS ret = EFI_SUCCESS;
        enum boot_target target;
        EFI_HANDLE image;
        void *efiimage = NULL;
        UINTN imagesize;

        set_efi_variable(&fastboot_guid, BOOT_STATE_VAR, sizeof(boot_state),
                         &boot_state, FALSE, TRUE);
        set_oemvars_update(TRUE);

        for (;;) {
                target = UNKNOWN_TARGET;

                ret = fastboot_start(&bootimage, &efiimage, &imagesize, &target);
                if (EFI_ERROR(ret)) {
                        efi_perror(ret, L"Fastboot mode failed");
                        break;
                }

                if (bootimage) {
                        /* 'fastboot boot' case, only allowed on unlocked devices.
                         * check just to make sure */
                        if (device_is_unlocked()) {
                                set_image_oemvars_nocheck(bootimage);
                                load_image(bootimage, BOOT_STATE_ORANGE, FALSE);
                        }
                        FreePool(bootimage);
                        bootimage = NULL;
                        continue;
                }

                if (efiimage) {
                        ret = uefi_call_wrapper(BS->LoadImage, 6, FALSE, g_parent_image,
                                                NULL, efiimage, imagesize, &image);
                        FreePool(efiimage);
                        efiimage = NULL;
                        if (EFI_ERROR(ret)) {
                                efi_perror(ret, L"Unable to load the received EFI image");
                                continue;
                        }
                        ret = uefi_call_wrapper(BS->StartImage, 3, image, NULL, NULL);
                        if (EFI_ERROR(ret))
                                efi_perror(ret, L"Unable to start the received EFI image");

                        uefi_call_wrapper(BS->UnloadImage, 1, image);
                        continue;
                }

                if (target != UNKNOWN_TARGET)
                        reboot_to_target(target);
        }

        /* Allow plenty of time for the error to be visible before the
         * screen goes blank */
        pause(30);
        halt_system();
}
#endif

static EFI_STATUS push_capsule(
                IN EFI_FILE *root_dir,
                IN CHAR16 *name,
                OUT EFI_RESET_TYPE *resetType)
{
        UINTN len = 0;
        UINT64 max = 0;
        EFI_CAPSULE_HEADER *capHeader = NULL;
        EFI_CAPSULE_HEADER **capHeaderArray;
        EFI_CAPSULE_BLOCK_DESCRIPTOR *scatterList;
        CHAR8 *content = NULL;
        EFI_STATUS ret;

        debug(L"Trying to load capsule: %s", name);
        ret = file_read(root_dir, name, &content, &len);
        if (EFI_SUCCESS == ret) {
                if (len <= 0) {
                        debug(L"Couldn't load capsule data from disk");
                        FreePool(content);
                        return EFI_LOAD_ERROR;
                }
                /* Some capsules might invoke reset during UpdateCapsule
                so delete the file now */
                ret = file_delete(g_disk_device, name);
                if (ret != EFI_SUCCESS) {
                        efi_perror(ret, L"Couldn't delete %s", name);
                        FreePool(content);
                        return ret;
                }
        }
        else {
                debug(L"Error in reading file");
                return ret;
        }

        capHeader = (EFI_CAPSULE_HEADER *) content;
        capHeaderArray = AllocatePool(2*sizeof(EFI_CAPSULE_HEADER*));
        if (!capHeaderArray) {
                FreePool(content);
                return EFI_OUT_OF_RESOURCES;
        }
        capHeaderArray[0] = capHeader;
        capHeaderArray[1] = NULL;
        debug(L"Querying capsule capabilities");
        ret = uefi_call_wrapper(RT->QueryCapsuleCapabilities, 4,
                        capHeaderArray, 1,  &max, resetType);
        if (EFI_SUCCESS == ret) {
                if (len > max) {
                        FreePool(content);
                        FreePool(capHeaderArray);
                        return EFI_BAD_BUFFER_SIZE;
                }
                scatterList = AllocatePool(2*sizeof(EFI_CAPSULE_BLOCK_DESCRIPTOR));
                if (!scatterList) {
                        FreePool(content);
                        FreePool(capHeaderArray);
                        return EFI_OUT_OF_RESOURCES;
                }
                memset((CHAR8*)scatterList, 0x0,
                        2*sizeof(EFI_CAPSULE_BLOCK_DESCRIPTOR));
                scatterList->Length = len;
                scatterList->Union.DataBlock = (EFI_PHYSICAL_ADDRESS) (UINTN) capHeader;

                debug(L"Calling RT->UpdateCapsule");
                ret = uefi_call_wrapper(RT->UpdateCapsule, 3, capHeaderArray, 1,
                        (EFI_PHYSICAL_ADDRESS) (UINTN) scatterList);
                if (ret != EFI_SUCCESS) {
                        FreePool(content);
                        FreePool(capHeaderArray);
                        FreePool(scatterList);
                        return ret;
                }
        }
        return ret;
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *sys_table)
{
        EFI_STATUS ret;
        CHAR16 *target_path = NULL;
        VOID *target_address = NULL;
        VOID *bootimage = NULL;
        BOOLEAN oneshot = FALSE;
        BOOLEAN lock_prompted = FALSE;
        VOID *selected_keystore = NULL;
        UINTN selected_keystore_size = 0;
        enum boot_target boot_target = NORMAL_BOOT;
        UINT8 boot_state = BOOT_STATE_GREEN;
        CHAR16 *loader_version = KERNELFLINGER_VERSION;
        UINT8 hash[KEYSTORE_HASH_SIZE];
        CHAR16 *name = NULL;
        EFI_RESET_TYPE resetType;

        /* gnu-efi initialization */
        InitializeLib(image, sys_table);
        ux_init();

        debug(L"%s", loader_version);
        set_efi_variable_str(&loader_guid, LOADER_VERSION_VAR,
                        FALSE, TRUE, loader_version);

        /* populate globals */
        g_parent_image = image;
        ret = uefi_call_wrapper(BS->OpenProtocol, 6, image,
                        &LoadedImageProtocol, (VOID **)&g_loaded_image,
                        image, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(ret)) {
                efi_perror(ret, L"OpenProtocol: LoadedImageProtocol");
                return ret;
        }
        g_disk_device = g_loaded_image->DeviceHandle;

        /* loaded from mass storage (not DnX) */
        if (g_disk_device) {
                ret = storage_set_boot_device(g_disk_device);
                if (EFI_ERROR(ret))
                        error(L"Failed to set boot device");
        }

        oem_keystore = (UINT8 *)&oem_keystore_table +
                        oem_keystore_table.oem_keystore_offset;
        oem_keystore_size = oem_keystore_table.oem_keystore_size;
        oem_key = (UINT8 *)&oem_keystore_table +
                        oem_keystore_table.oem_key_offset;
        oem_key_size = oem_keystore_table.oem_key_size;
        debug(L"oem key size %d keystore size %d", oem_key_size,
                        oem_keystore_size);

        if (file_exists(g_disk_device, FWUPDATE_FILE)) {
                name = FWUPDATE_FILE;
                push_capsule(g_disk_device, name, &resetType);

                debug(L"I am about to reset the system");

                uefi_call_wrapper(RT->ResetSystem, 4, resetType, EFI_SUCCESS, 0,
                                NULL);
        }

        /* No UX prompts before this point, do not want to interfere
         * with magic key detection */
        boot_target = choose_boot_target(&target_address, &target_path, &oneshot);
        if (boot_target == EXIT_SHELL)
                return EFI_SUCCESS;

        if (boot_target == POWER_OFF)
                halt_system();

        if (boot_target == CHARGER)
                ux_display_empty_battery();

#ifdef USERDEBUG
        debug(L"checking device state");

        if (!is_efi_secure_boot_enabled() && !device_is_provisioning()) {
                debug(L"uefi secure boot is disabled");
                boot_state = BOOT_STATE_ORANGE;
                lock_prompted = TRUE;

                /* Need to warn early, before we even enter Fastboot
                 * or run EFI binaries. Set lock_prompted to true so
                 * we don't ask again later */
                ux_prompt_user_secure_boot_off();
#ifdef NO_DEVICE_UNLOCK
                halt_system();
#else
                debug(L"User accepted UEFI secure boot disabled warning");
#endif
        } else  if (device_is_unlocked()) {
                boot_state = BOOT_STATE_ORANGE;
                debug(L"Device is unlocked");
        } else {
                debug(L"examining keystore");

                select_keystore(&selected_keystore, &selected_keystore_size);
                if (EFI_ERROR(verify_android_keystore(selected_keystore,
                                        selected_keystore_size,
                                        oem_key, oem_key_size, hash))) {
                        debug(L"keystore not validated");
                        boot_state = BOOT_STATE_YELLOW;
                }
        }

#ifdef USER
        if (device_is_provisioning()) {
                debug(L"device is provisioning, force Fastboot mode");
                enter_fastboot_mode(boot_state, target_address);
        }
#endif
#else /* !USERDEBUG */
        /* Make sure it's abundantly clear! */
        error(L"INSECURE BOOTLOADER - SYSTEM SECURITY IN RED STATE");
        pause(1);
        boot_state = BOOT_STATE_RED;
#endif

        /* EFI binaries are validated by the BIOS */
        if (boot_target == ESP_EFI_BINARY) {
                debug(L"entering EFI binary");
                ret = enter_efi_binary(target_path, oneshot);
                if (EFI_ERROR(ret)) {
                        efi_perror(ret, L"EFI Application exited abnormally");
                        pause(3);
                }
                FreePool(target_path);
                reboot(NULL);
        }

        /* Fastboot is always validated by the OEM keystore baked into
         * the kernelflinger binary */
        if (boot_target == FASTBOOT || boot_target == MEMORY) {
                debug(L"entering Fastboot mode");
                enter_fastboot_mode(boot_state, target_address);
        }

        if (boot_target == TDOS) {
                debug(L"entering TDOS");
                enter_tdos(boot_state);
        }

        /* Past this point is where we start to care if the keystore isn't
         * validated or the device is unlocked via Fastboot, start to prompt
         * the user if we aren't GREEN */

        /* If the user keystore is bad the only way to fix it is via
         * fastboot */
        if (boot_state == BOOT_STATE_YELLOW) {
                ux_prompt_user_keystore_unverified(hash);
#ifdef NO_DEVICE_UNLOCK
                halt_system();
#else
                debug(L"User accepted unverified keystore warning");
#endif
        }

        /* If the device is unlocked the only way to re-lock it is
         * via fastboot. Skip this UX if we already prompted earlier
         * about EFI secure boot being turned off */
        if (boot_state == BOOT_STATE_ORANGE && !lock_prompted) {
                ux_prompt_user_device_unlocked();
#ifdef NO_DEVICE_UNLOCK
                halt_system();
#else
                debug(L"User accepted unlocked device warning");
#endif
        }

        debug(L"loading boot image");
        ret = load_boot_image(boot_target, selected_keystore,
                        selected_keystore_size, target_path,
                        &bootimage, oneshot);
        FreePool(target_path);

        if (EFI_ERROR(ret)) {
                debug(L"issue loading boot image: %r", ret);
                boot_state = BOOT_STATE_RED;

                if (boot_target == RECOVERY)
                        ux_warn_user_unverified_recovery();
                else
                        ux_prompt_user_bootimage_unverified();

#ifdef NO_DEVICE_UNLOCK
                halt_system();
#else
                debug(L"User accepted bad boot image warning");
#endif

                if (bootimage == NULL) {
                        error(L"Unable to load boot image at all; stop.");
                        pause(5);
                        halt_system();
                }
        }

        switch (boot_target) {
        case RECOVERY:
        case ESP_BOOTIMAGE:
                /* We're either about to do an OTA update, or doing a one-shot
                 * boot into an alternate boot image from 'fastboot boot'.
                 * Load the OEM vars in this new boot image, but ensure that
                 * we'll read them again on the next normal boot */
                set_image_oemvars_nocheck(bootimage);
                set_oemvars_update(TRUE);
                break;
        case NORMAL_BOOT:
        case CHARGER:
                set_image_oemvars(bootimage);
                break;
        default:
                break;
        }

        return load_image(bootimage, boot_state, boot_target);
}

/* vim: softtabstop=8:shiftwidth=8:expandtab
 */
