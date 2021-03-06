/*
 * Copyright (c) 2015, Intel Corporation
 * All rights reserved.
 *
 * Author: Jeremy Compostella <jeremy.compostella@intel.com>
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
 */

#include <efi.h>
#include <efiapi.h>
#include <efilib.h>
#include <stdio.h>

#include "lib.h"
#include "uefi_utils.h"
#include "protocol.h"
#include "flash.h"
#include "gpt.h"
#include "sparse.h"
#include "sparse_format.h"
#include "fastboot.h"
#include "fastboot_oem.h"
#include "fastboot_usb.h"
#include "text_parser.h"

static BOOLEAN last_cmd_succeeded;
static fastboot_handle fastboot_flash_cmd;
static EFI_FILE_IO_INTERFACE *file_io_interface;
static data_callback_t fastboot_rx_cb, fastboot_tx_cb;
static CHAR8 DEFAULT_OPTIONS[] = "--batch installer.cmd";
static BOOLEAN need_tx_cb;
static char *fastboot_cmd_buf;
static UINTN fastboot_cmd_buf_len;
static char command_buffer[256]; /* Large enough to fit long filename
				    on flash command.  */

#define inst_perror(ret, x, ...) do { \
	fastboot_fail(x ": %r", ##__VA_ARGS__, ret); \
} while (0)

static void flush_tx_buffer(void)
{
	while (need_tx_cb) {
		need_tx_cb = FALSE;
		fastboot_tx_cb(NULL, 0);
	}
}

static void installer_flash_buffer(void *data, unsigned size,
				   INTN argc, CHAR8 **argv)
{
	fastboot_set_dlbuffer(data, size);
	fastboot_flash_cmd(argc, argv);
	flush_tx_buffer();
	fastboot_set_dlbuffer(NULL, 0);
}

static EFI_STATUS read_file(EFI_FILE *file, UINTN size, void *data)
{
	EFI_STATUS ret;
	UINTN nsize = size;

	ret = uefi_call_wrapper(file->Read, 3, file, &nsize, data);
	if (EFI_ERROR(ret)) {
		inst_perror(ret, "Failed to read file");
		return ret;
	}
	if (size != nsize) {
		fastboot_fail("Failed to read %d bytes (only %d read)",
			      size, nsize);
		return EFI_INVALID_PARAMETER;
	}

	return ret;
}

/* This function splits a huge sparse file into smaller ones and flash
   them. */
static void installer_split_and_flash(CHAR16 *filename, UINTN size,
				      UINTN argc, CHAR8 **argv)
{
	EFI_STATUS ret;
	struct sparse_header sph, *new_sph;
	struct chunk_header *ckh, *skip_ckh;
	void *buf, *data;
	UINTN remaining_data = size;
	UINTN data_size, read_size, flash_size, header_size, already_read;
	void *read_ptr;
	INTN nb_chunks;
	EFI_FILE *file;
	__le32 blk_count;

	ret = uefi_open_file(file_io_interface, filename, &file);
	if (EFI_ERROR(ret)) {
		inst_perror(ret, "Failed to open %s file", filename);
		return;
	}

	ret = read_file(file, sizeof(sph), &sph);
	if (EFI_ERROR(ret))
		return;
	remaining_data -= sizeof(sph);

	if (!is_sparse_image((void *) &sph, sizeof(sph))) {
		fastboot_fail("sparse file expected");
		return;
	}

	buf = AllocatePool(MAX_DOWNLOAD_SIZE);
	if (!buf) {
		fastboot_fail("Failed to allocate %d bytes", MAX_DOWNLOAD_SIZE);
		return;
	}
	data = buf;

	/* New sparse header. */
	memcpy(data, &sph, sizeof(sph));
	new_sph = data;
	data += sizeof(*new_sph);

	/* Sparse skip chunk. */
	skip_ckh = data;
	skip_ckh->chunk_type = CHUNK_TYPE_DONT_CARE;
	skip_ckh->total_sz = sizeof(*skip_ckh);
	data += sizeof(*skip_ckh);

	header_size = data - buf;
	data_size = MAX_DOWNLOAD_SIZE - header_size;
	nb_chunks = sph.total_chunks;
	read_size = data_size;
	read_ptr = data;
	blk_count = 0;

	while (nb_chunks > 0 && remaining_data > 0) {
		new_sph->total_chunks = 1;
		new_sph->total_blks = skip_ckh->chunk_sz = blk_count;

		if (remaining_data < read_size)
			read_size = remaining_data;

		/* Read a new piece of the input sparse file. */
		ret = read_file(file, read_size, read_ptr);
		if (EFI_ERROR(ret))
			goto exit;
		remaining_data -= read_size;

		/* Process the loaded chunks to build the new header
		   and the skip chunk. */
		flash_size = header_size;
		ckh = data;
		while ((void *)ckh + sizeof(*ckh) <= read_ptr + read_size &&
		       (void *)ckh + ckh->total_sz <= read_ptr + read_size) {
			if (nb_chunks == 0) {
				fastboot_fail("Corrupted sparse file: too many chunks");
				goto exit;
			}
			flash_size += ckh->total_sz;
			new_sph->total_blks += ckh->chunk_sz;
			blk_count += ckh->chunk_sz;
			new_sph->total_chunks++;
			nb_chunks--;
			ckh = (void *)ckh + ckh->total_sz;
		}

		/* Handle the inconsistencies. */
		if (flash_size == header_size) {
			if ((void *)ckh + sizeof(*ckh) < read_ptr + read_size) {
				fastboot_fail("Corrupted sparse file");
				goto exit;
			} else {
				fastboot_fail("Found a too big chunk");
				goto exit;
			}
		}

		installer_flash_buffer(buf, flash_size, argc, argv);
		if (!last_cmd_succeeded)
			goto exit;

		/* Move the incomplete chunk from the end to the
		   beginning of the buffer. */
		if (buf + flash_size < read_ptr + read_size) {
			already_read = read_ptr + read_size - (void *)ckh;
			memcpy(data, ckh, already_read);
			read_size = data_size - already_read;
			read_ptr = data + already_read;
		} else {
			read_size = data_size;
			read_ptr = data;
		}
	}

exit:
	FreePool(buf);
}

static void installer_flash_cmd(INTN argc, CHAR8 **argv)
{
	EFI_STATUS ret;
	CHAR16 *filename;
	void *data;
	UINTN size;

	if (argc != 3) {
		fastboot_fail("Flash command requires exactly 3 arguments");
		return;
	}

	/* The fastboot flash command does not want the file parameter. */
	argc--;

	filename = stra_to_str(argv[2]);
	if (!filename) {
		fastboot_fail("Failed to convert CHAR8 filename to CHAR16");
		return;
	}

	ret = uefi_get_file_size(file_io_interface, filename, &size);
	if (EFI_ERROR(ret)) {
		inst_perror(ret, "Failed to get %s file size", filename);
		goto exit;
	}

	if (size > MAX_DOWNLOAD_SIZE) {
		installer_split_and_flash(filename, size, argc, argv);
		goto exit;
	}

	ret = uefi_read_file(file_io_interface, filename, &data, &size);
	if (EFI_ERROR(ret)) {
		inst_perror(ret, "Unable to read file %s", filename);
		goto exit;
	}

	installer_flash_buffer(data, size, argc, argv);
	FreePool(data);

exit:
	FreePool(filename);
}

static CHAR16 *get_format_image_filename(CHAR8 *label)
{
	CHAR8 *filename;
	CHAR16 *filename16;
	UINTN label_length;

	if (!strcmp(label, (CHAR8 *)"data"))
		label = (CHAR8 *)"userdata";

	label_length = strlena(label);
	filename = AllocateZeroPool(label_length + 5);
	if (!filename) {
		fastboot_fail("Unable to allocate CHAR8 filename buffer");
		return NULL;
	}
	memcpy(filename, label, label_length);
	memcpy(filename + label_length, ".img", 4);
	filename16 = stra_to_str(filename);
	FreePool(filename);
	if (!filename16) {
		fastboot_fail("Unable to allocate CHAR16 filename buffer");
		return NULL;
	}

	return filename16;
}

/* Simulate the fastboot host format command:
   1. get a filesystem image from a file;
   2. erase the partition;
   3. flash the filesystem image; */
static void installer_format(INTN argc, CHAR8 **argv)
{
	EFI_STATUS ret;
	void *data = NULL;
	UINTN size;
	CHAR16 *filename;

	filename = get_format_image_filename(argv[1]);
	if (!filename)
		return;

	ret = uefi_read_file(file_io_interface, filename, &data, &size);
	if (ret == EFI_NOT_FOUND && !StrCmp(L"userdata.img", filename)) {
		fastboot_info("userdata.img is missing, cannot format %a", argv[1]);
		fastboot_info("Android fs_mgr will manage this");
	} else if (EFI_ERROR(ret)) {
		inst_perror(ret, "Unable to read file %s", filename);
		goto free_filename;
	}

	fastboot_run_root_cmd("erase", argc, argv);
	flush_tx_buffer();
	if (!last_cmd_succeeded)
		goto free_data;

	if (data)
		installer_flash_buffer(data, size, argc, argv);

free_data:
	FreePool(data);
free_filename:
	FreePool(filename);
}

static char **commands;
static UINTN command_nb;
static UINTN current_command;

static void free_commands(void)
{
	UINTN i;

	if (!commands)
		return;

	for (i = 0; i < command_nb; i++)
		if (commands[i])
			FreePool(commands);

	FreePool(commands);
	commands = NULL;
	command_nb = 0;
	current_command = 0;
}

static EFI_STATUS store_command(char *command, VOID *context _unused)
{
	char **new_commands;

	new_commands = AllocatePool((command_nb + 1) * sizeof(*new_commands));
	if (!new_commands) {
		free_commands();
		return EFI_OUT_OF_RESOURCES;
	}

	memcpy(new_commands, commands, command_nb * sizeof(*commands));
	new_commands[command_nb] = strdup(command);
	if (!new_commands[command_nb]) {
		free_commands();
		return EFI_OUT_OF_RESOURCES;
	}
	if (commands)
		FreePool(commands);
	commands = new_commands;
	command_nb++;

	return EFI_SUCCESS;
}

static char *next_command()
{
	if (command_nb == current_command) {
		free_commands();
		return NULL;
	}

	return commands[current_command++];
}

static void batch(__attribute__((__unused__)) INTN argc,
		  __attribute__((__unused__)) CHAR8 **argv)
{
	EFI_STATUS ret;
	void *data;
	UINTN size;
	CHAR16 *filename;

	if (argc != 2) {
		fastboot_fail("Batch command takes one parameter");
		return;
	}

	filename = stra_to_str(argv[1]);
	if (!filename) {
		fastboot_fail("Failed to convert CHAR8 filename to CHAR16");
		return;
	}

	ret = uefi_read_file(file_io_interface, filename, &data, &size);
	if (EFI_ERROR(ret)) {
		inst_perror(ret, "Failed to read %s file", filename);
		FreePool(filename);
		return;
	}
	FreePool(filename);

	ret = parse_text_buffer(data, size, store_command, NULL);
	FreePool(data);
	if (EFI_ERROR(ret))
		inst_perror(ret, "Failed to parse batch file");
	else
		fastboot_okay("");
}

static void usage(__attribute__((__unused__)) INTN argc,
		  __attribute__((__unused__)) CHAR8 **argv)
{
	Print(L"Usage: installer [OPTIONS | COMMANDS]\n");
	Print(L"  installer is an EFI application acting like the fastboot command.\n\n");
	Print(L" COMMANDS               fastboot commands (cf. the fastboot manual page)\n");
	Print(L" --help, -h             print this help and exit\n");
	Print(L" --batch, -b FILE       run all the fastboot commands of FILE\n");
	Print(L"If no option is provided, the installer assumes '%a'\n", DEFAULT_OPTIONS);
	Print(L"Note: 'boot', 'update', 'flash-raw' and 'flashall' commands are NOT supported\n");

	fastboot_okay("");
}

static void unsupported_cmd(__attribute__((__unused__)) INTN argc,
			    CHAR8 **argv)
{
	fastboot_fail("installer does not the support the '%a' command", argv[0]);
}

static struct replacements {
	struct fastboot_cmd cmd;
	fastboot_handle *save_handle;
} REPLACEMENTS[] = {
	/* Fastboot changes. */
	{ { "flash",	UNKNOWN_STATE,	installer_flash_cmd },	&fastboot_flash_cmd },
	{ { "format",	VERIFIED,	installer_format    },	NULL },
	/* Unsupported commands. */
	{ { "update",	UNKNOWN_STATE, unsupported_cmd	    },	NULL },
	{ { "flashall",	UNKNOWN_STATE, unsupported_cmd	    },	NULL },
	{ { "boot",	UNKNOWN_STATE, unsupported_cmd	    },	NULL },
	{ { "devices",	UNKNOWN_STATE, unsupported_cmd	    },	NULL },
	{ { "download",	UNKNOWN_STATE, unsupported_cmd	    },	NULL },
	/* Installer specific commands. */
	{ { "--help",	LOCKED,	usage			    },	NULL },
	{ { "-h",	LOCKED,	usage			    },	NULL },
	{ { "--batch",	LOCKED,	batch			    },	NULL },
	{ { "-b",	LOCKED,	batch			    },	NULL }
};

static EFI_STATUS installer_replace_functions()
{
	EFI_STATUS ret;
	struct fastboot_cmd *cmd;
	UINTN i;

	for (i = 0; i < ARRAY_SIZE(REPLACEMENTS); i++) {
		cmd = fastboot_get_root_cmd(REPLACEMENTS[i].cmd.name);

		if (cmd && REPLACEMENTS[i].save_handle)
			*(REPLACEMENTS[i].save_handle) = cmd->handle;

		if (cmd && REPLACEMENTS[i].cmd.handle)
			cmd->handle = REPLACEMENTS[i].cmd.handle;

		if (!cmd && REPLACEMENTS[i].cmd.handle) {
			ret = fastboot_register(&REPLACEMENTS[i].cmd);
			if (EFI_ERROR(ret))
				return ret;
		}
	}

	return EFI_SUCCESS;
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *_table)
{
	EFI_STATUS ret;
	EFI_LOADED_IMAGE *loaded_img = NULL;
	CHAR8 *options, *buf;
	UINTN i;
	void *bootimage;
	void *efiimage;
	UINTN imagesize;
	enum boot_target target;

	InitializeLib(image, _table);
	g_parent_image = image;

	ret = handle_protocol(image, &LoadedImageProtocol, (void **)&loaded_img);
	if (ret != EFI_SUCCESS) {
		inst_perror(ret, "LoadedImageProtocol error");
		return ret;
	}

	/* Initialize File IO interface. */
	ret = uefi_call_wrapper(BS->HandleProtocol, 3, loaded_img->DeviceHandle,
				&FileSystemProtocol, (void *)&file_io_interface);
	if (EFI_ERROR(ret)) {
		inst_perror(ret, "Failed to get FileSystemProtocol");
		return ret;
	}

	/* Prepare parameters. */
	UINTN size = StrLen(loaded_img->LoadOptions) + 1;
	buf = options = AllocatePool(size);
	if (!options) {
		fastboot_fail("Unable to allocate buffer for parameters");
		return EFI_OUT_OF_RESOURCES;
	}
	str_to_stra(options, loaded_img->LoadOptions, size);
	/* Snip control and space characters. */
	for (i = size - 1; options[i] <= ' '; i--)
		options[i] = '\0';
	/* Drop the first parameter.  */
	options = strchr(options, ' ');
	skip_whitespace((char **)&options);

	store_command(*options != '\0' ? (char *)options : (char *)DEFAULT_OPTIONS,
		      NULL);

	/* Run the fastboot library. */
	ret = fastboot_start(&bootimage, &efiimage, &imagesize, &target);
	if (EFI_ERROR(ret))
		goto exit;

	if (target != UNKNOWN_TARGET)
		reboot_to_target(target);

exit:
	FreePool(buf);
	if (EFI_ERROR(ret))
		return ret;
	return last_cmd_succeeded ? EFI_SUCCESS : EFI_INVALID_PARAMETER;
}

/* USB wrapper functions. */
EFI_STATUS fastboot_usb_init_and_connect(start_callback_t start_cb,
					 data_callback_t rx_cb,
					 data_callback_t tx_cb)
{
	EFI_STATUS ret;
	ret = fastboot_set_command_buffer(command_buffer,
					  sizeof(command_buffer));
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to set fastboot command buffer");
		return ret;
	}

	fastboot_tx_cb = tx_cb;
	fastboot_rx_cb = rx_cb;
	start_cb();

	if (!fastboot_cmd_buf)
		return EFI_INVALID_PARAMETER;

	return EFI_SUCCESS;
}

EFI_STATUS fastboot_usb_stop(void)
{
	return EFI_SUCCESS;
}

EFI_STATUS fastboot_usb_disconnect_and_unbind(void)
{
	return EFI_SUCCESS;
}

EFI_STATUS fastboot_usb_run(void)
{
	static BOOLEAN initialized = FALSE;
	EFI_STATUS ret;
	char *cmd;
	UINTN cmd_len;

	if (!initialized) {
		ret = installer_replace_functions();
		if (EFI_ERROR(ret))
			return ret;
		if (!fastboot_flash_cmd) {
			fastboot_fail("Failed to get the flash handle");
			return ret;
		}
		initialized = TRUE;
	}

	if (current_command > 0) {
		flush_tx_buffer();
		if (!last_cmd_succeeded)
			goto stop;
		Print(L"Command successfully executed\n");
	}

	cmd = next_command();
	if (!cmd)
		goto stop;

	cmd_len = strlena((CHAR8 *)cmd);
	if (cmd_len > fastboot_cmd_buf_len) {
		inst_perror(EFI_BUFFER_TOO_SMALL,
			    "command too long for fastboot command buffer");
		goto stop;
	}

	memcpy(fastboot_cmd_buf, cmd, cmd_len);

	Print(L"Starting command: '%a'\n", cmd);
	fastboot_rx_cb(fastboot_cmd_buf, cmd_len);

	return EFI_SUCCESS;

stop:
	fastboot_stop(NULL, NULL, 0, EXIT_SHELL);
	return EFI_SUCCESS;
}

int usb_read(void *buf, unsigned len)
{
	fastboot_cmd_buf = buf;
	fastboot_cmd_buf_len = len;

	return 0;
}

int usb_write(void *pBuf, uint32_t size)
{
#define PREFIX_LEN 4

	if (size < PREFIX_LEN)
		return 0;

	if (!memcmp((CHAR8 *)"INFO", pBuf, PREFIX_LEN)) {
		Print(L"(bootloader) %a\n", pBuf + PREFIX_LEN);
		need_tx_cb = TRUE;
	} if (!memcmp((CHAR8 *)"OKAY", pBuf, PREFIX_LEN)) {
		if (((char *)pBuf)[PREFIX_LEN] != '\0')
			Print(L"%a\n", pBuf + PREFIX_LEN);
		last_cmd_succeeded = TRUE;
		fastboot_tx_cb(NULL, 0);
	} else if (!memcmp((CHAR8 *)"FAIL", pBuf, PREFIX_LEN)) {
		error(L"%a", pBuf + PREFIX_LEN);
		last_cmd_succeeded = FALSE;
		fastboot_tx_cb(NULL, 0);
	}

	return 0;
}

/* UI wrapper functions. */
void fastboot_ui_destroy(void)
{
}

void fastboot_ui_refresh(void)
{
}

EFI_STATUS fastboot_ui_init(void)
{
	return EFI_SUCCESS;
}

enum boot_target fastboot_ui_event_handler()
{
	return UNKNOWN_TARGET;
}

/* Installer does not support UI.  It is intended to be used in
   factory or for engineering purpose only.  */
BOOLEAN fastboot_ui_confirm_for_state(__attribute__((__unused__)) enum device_state target)
{
	return TRUE;
}
