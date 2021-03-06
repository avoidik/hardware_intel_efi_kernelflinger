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

#ifndef _VARS_H_
#define _VARS_H_
#include <efi.h>
#include <efiapi.h>

/* Gummiboot's loader GUID, for compatibility we honor some of the
 * same variables */
extern const EFI_GUID loader_guid;

extern const EFI_GUID fastboot_guid;

/* TODO get rid of the rest of these _VAR definitions here and write
 * accessor functions for them */

#define LOADER_ENTRY_ONESHOT    L"LoaderEntryOneShot"
/* Report bootloader version */
#define LOADER_VERSION_VAR      L"LoaderVersion"

#define SERIAL_PORT_VAR         L"SerialPort"

/* EFI variable which stores the max timeout for checking whether the
 * magic key was pressed at startup */
#define MAGIC_KEY_TIMEOUT_VAR   L"MagicKeyTimeout"

/* EFI variable which stores the time in milliseconds to wait between
 * two key events for a hold key */
#define HOLD_KEY_STALL_TIME_VAR   L"HoldKeyStallTime"

/* Boot state that we report before exiting boot services, per
 * Google's verified boot spec */
#define BOOT_STATE_VAR		L"BootState"
#define BOOT_STATE_GREEN	0
#define BOOT_STATE_YELLOW	1
#define BOOT_STATE_ORANGE	2
#define BOOT_STATE_RED		3

#define OEM_KEY_VAR		L"OEMKey"

/* EFI variable to store the kernelflinger logs.  */
#define LOG_VAR			L"KernelflingerLogs"

#ifndef USER
#define CMDLINE_PREPEND_VAR     L"PrependCmdline"
#define CMDLINE_APPEND_VAR      L"AppendCmdline"
#define CMDLINE_REPLACE_VAR     L"ReplaceCmdline"
#endif

/* Various interesting partition labels */
extern const CHAR16 *BOOT_LABEL;
extern const CHAR16 *RECOVERY_LABEL;
extern const CHAR16 *MISC_LABEL;

BOOLEAN device_is_unlocked(void);
BOOLEAN device_is_locked(void);
BOOLEAN device_is_verified(void);
BOOLEAN get_current_off_mode_charge(void);
EFI_STATUS set_off_mode_charge(BOOLEAN enabled);
BOOLEAN get_current_crash_event_menu(void);
EFI_STATUS set_crash_event_menu(BOOLEAN enabled);
BOOLEAN get_oemvars_update(void);
EFI_STATUS set_oemvars_update(BOOLEAN updated);

enum device_state {
	UNKNOWN_STATE = -1,
	LOCKED = 0,
	VERIFIED = 1,
	UNLOCKED = 2
};
char *get_current_state_string(void);
EFI_GRAPHICS_OUTPUT_BLT_PIXEL *get_current_state_color();
EFI_STATUS set_current_state(enum device_state state);
enum device_state get_current_state();
EFI_STATUS set_user_keystore(VOID *keystore, UINTN size);
EFI_STATUS get_user_keystore(VOID **keystorep, UINTN *sizep);
BOOLEAN device_is_provisioning(void);
EFI_STATUS get_watchdog_status(UINT8 *counter, EFI_TIME *time);
EFI_STATUS reset_watchdog_status(VOID);
EFI_STATUS set_watchdog_counter(UINT8 counter);
EFI_STATUS set_watchdog_time_reference(EFI_TIME *time);
char *get_serial_number(void);
BOOLEAN get_display_splash(void);
char *get_property_bootloader(void);
#ifdef HAL_AUTODETECT
char *get_property_device(void);
char *get_property_brand(void);
char *get_property_name(void);
char *get_property_model(void);
#endif
char *get_device_id(void);
CHAR16 *boot_state_to_string(UINT8 boot_state);
#ifndef USER
EFI_STATUS reprovision_state_vars(VOID);
#endif
#endif /* _VARS_H_ */

