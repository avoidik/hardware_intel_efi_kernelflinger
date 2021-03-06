/*
 * Copyright (c) 2013, Intel Corporation
 * All rights reserved.
 *
 * Author: Andrew Boie <andrew.p.boie@intel.com>
 * Some Linux bootstrapping code adapted from efilinux by
 * Matt Fleming <matt.fleming@intel.com>
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
#include <efilib.h>
#include <ui.h>

#include "android.h"
#include "efilinux.h"
#include "lib.h"
#include "security.h"
#include "vars.h"
#include "power.h"
#include "targets.h"
#include "gpt.h"
#include "storage.h"
#include "text_parser.h"
#ifdef HAL_AUTODETECT
#include "blobstore.h"
#endif

struct setup_header {
        UINT8 setup_secs;        /* Sectors for setup code */
        UINT16 root_flags;
        UINT32 sys_size;
        UINT16 ram_size;
        UINT16 video_mode;
        UINT16 root_dev;
        UINT16 signature;        /* Boot signature */
        UINT16 jump;
        UINT32 header;
        UINT16 version;
        UINT16 su_switch;
        UINT16 setup_seg;
        UINT16 start_sys;
        UINT16 kernel_ver;
        UINT8 loader_id;
        UINT8 load_flags;
        UINT16 movesize;
        UINT32 code32_start;        /* Start of code loaded high */
        UINT32 ramdisk_start;        /* Start of initial ramdisk */
        UINT32 ramdisk_len;        /* Lenght of initial ramdisk */
        UINT32 bootsect_kludge;
        UINT16 heap_end;
        UINT8 ext_loader_ver;  /* Extended boot loader version */
        UINT8 ext_loader_type; /* Extended boot loader ID */
        UINT32 cmd_line_ptr;   /* 32-bit pointer to the kernel command line */
        UINT32 ramdisk_max;    /* Highest legal initrd address */
        UINT32 kernel_alignment; /* Physical addr alignment required for kernel */
        UINT8 relocatable_kernel; /* Whether kernel is relocatable or not */
        UINT8 min_alignment;
        UINT16 xloadflags;
        UINT32 cmdline_size;
        UINT32 hardware_subarch;
        UINT64 hardware_subarch_data;
        UINT32 payload_offset;
        UINT32 payload_length;
        UINT64 setup_data;
        UINT64 pref_address;
        UINT32 init_size;
        UINT32 handover_offset;
} __attribute__((packed));

struct efi_info {
        UINT32 efi_loader_signature;
        UINT32 efi_systab;
        UINT32 efi_memdesc_size;
        UINT32 efi_memdesc_version;
        UINT32 efi_memmap;
        UINT32 efi_memmap_size;
        UINT32 efi_systab_hi;
        UINT32 efi_memmap_hi;
};

struct e820_entry {
        UINT64 addr;                /* start of memory segment */
        UINT64 size;                /* size of memory segment */
        UINT32 type;                /* type of memory segment */
} __attribute__((packed));

struct screen_info {
        UINT8  orig_x;           /* 0x00 */
        UINT8  orig_y;           /* 0x01 */
        UINT16 ext_mem_k;        /* 0x02 */
        UINT16 orig_video_page;  /* 0x04 */
        UINT8  orig_video_mode;  /* 0x06 */
        UINT8  orig_video_cols;  /* 0x07 */
        UINT8  flags;            /* 0x08 */
        UINT8  unused2;          /* 0x09 */
        UINT16 orig_video_ega_bx;/* 0x0a */
        UINT16 unused3;          /* 0x0c */
        UINT8  orig_video_lines; /* 0x0e */
        UINT8  orig_video_isVGA; /* 0x0f */
        UINT16 orig_video_points;/* 0x10 */

        /* VESA graphic mode -- linear frame buffer */
        UINT16 lfb_width;        /* 0x12 */
        UINT16 lfb_height;       /* 0x14 */
        UINT16 lfb_depth;        /* 0x16 */
        UINT32 lfb_base;         /* 0x18 */
        UINT32 lfb_size;         /* 0x1c */
        UINT16 cl_magic, cl_offset; /* 0x20 */
        UINT16 lfb_linelength;   /* 0x24 */
        UINT8  red_size;         /* 0x26 */
        UINT8  red_pos;          /* 0x27 */
        UINT8  green_size;       /* 0x28 */
        UINT8  green_pos;        /* 0x29 */
        UINT8  blue_size;        /* 0x2a */
        UINT8  blue_pos;         /* 0x2b */
        UINT8  rsvd_size;        /* 0x2c */
        UINT8  rsvd_pos;         /* 0x2d */
        UINT16 vesapm_seg;       /* 0x2e */
        UINT16 vesapm_off;       /* 0x30 */
        UINT16 pages;            /* 0x32 */
        UINT16 vesa_attributes;  /* 0x34 */
        UINT32 capabilities;     /* 0x36 */
        UINT8  _reserved[6];     /* 0x3a */
} __attribute__((packed));

struct boot_params {
        struct screen_info screen_info;
        UINT8 apm_bios_info[0x14];
        UINT8 _pad2[4];
        UINT64 tboot_addr;
        UINT8 ist_info[0x10];
        UINT8 _pad3[16];
        UINT8 hd0_info[16];
        UINT8 hd1_info[16];
        UINT8 sys_desc_table[0x10];
        UINT8 olpc_ofw_header[0x10];
        UINT8 _pad4[128];
        UINT8 edid_info[0x80];
        struct efi_info efi_info;
        UINT32 alt_mem_k;
        UINT32 scratch;
        UINT8 e820_entries;
        UINT8 eddbuf_entries;
        UINT8 edd_mbr_sig_buf_entries;
        UINT8 _pad6[6];
        struct setup_header hdr;
        UINT8 _pad7[0x290-0x1f1-sizeof(struct setup_header)];
        UINT32 edd_mbr_sig_buffer[16];
        struct e820_entry e820_map[128];
        UINT8 _pad8[48];
        UINT8 eddbuf[0x1ec];
        UINT8 _pad9[276];
};

typedef void(*handover_func)(void *, EFI_SYSTEM_TABLE *, struct boot_params *) \
            __attribute__((regparm(0)));

static inline void handover_jump(EFI_HANDLE image, struct boot_params *bp,
                                 EFI_PHYSICAL_ADDRESS kernel_start)
{
        UINTN offset = bp->hdr.handover_offset;
        handover_func hf;

        asm volatile ("cli");

#if __LP64__
        /* The 64-bit kernel entry is 512 bytes after the start. */
        kernel_start += 512;
#endif

        hf = (handover_func)((UINTN)kernel_start + offset);
        hf(image, ST, bp);
}



UINT32 pagealign(struct boot_img_hdr *hdr, UINT32 blob_size)
{
        UINT32 page_mask = hdr->page_size - 1;
        return (blob_size + page_mask) & (~page_mask);
}


UINTN bootimage_size(struct boot_img_hdr *aosp_header)
{
        UINTN size;

        size = pagealign(aosp_header, aosp_header->kernel_size) +
               pagealign(aosp_header, aosp_header->ramdisk_size) +
               pagealign(aosp_header, aosp_header->second_size) +
               aosp_header->page_size;

        return size;
}


struct boot_img_hdr *get_bootimage_header(VOID *bootimage_blob)
{
        struct boot_img_hdr *hdr;

        if (!bootimage_blob)
                return NULL;

        hdr = (struct boot_img_hdr *)bootimage_blob;
        if (strncmpa((CHAR8 *)BOOT_MAGIC, hdr->magic, BOOT_MAGIC_SIZE))
                return NULL;
        return hdr;
}


static EFI_STATUS setup_ramdisk(UINT8 *bootimage)
{
        struct boot_img_hdr *aosp_header;
        struct boot_params *bp;
        UINT32 roffset, rsize;
        EFI_PHYSICAL_ADDRESS ramdisk_addr;
        EFI_STATUS ret;

        aosp_header = (struct boot_img_hdr *)bootimage;
        bp = (struct boot_params *)(bootimage + aosp_header->page_size);

        roffset = aosp_header->page_size + pagealign(aosp_header,
                        aosp_header->kernel_size);
        rsize = aosp_header->ramdisk_size;
        if (!rsize) {
                debug(L"boot image has no ramdisk");
                return EFI_SUCCESS; // no ramdisk, so nothing to do
        }

        bp->hdr.ramdisk_len = rsize;
        debug(L"ramdisk size %d", rsize);
        ret = emalloc(rsize, 0x1000, &ramdisk_addr);
        if (EFI_ERROR(ret))
                return ret;

        if ((UINTN)ramdisk_addr > bp->hdr.ramdisk_max) {
                error(L"Ramdisk address is too high!");
                efree(ramdisk_addr, rsize);
                return EFI_OUT_OF_RESOURCES;
        }
        memcpy((VOID *)(UINTN)ramdisk_addr, bootimage + roffset, rsize);
        bp->hdr.ramdisk_start = (UINT32)(UINTN)ramdisk_addr;
        return EFI_SUCCESS;
}


static CHAR16 *get_serial_port(void)
{
        CHAR8 *data;
        UINTN size;
        CHAR16 *val, *pos;
        EFI_STATUS ret;

        ret = get_efi_variable(&loader_guid, SERIAL_PORT_VAR,
                        &size, (VOID **)&data, NULL);
        if (EFI_ERROR(ret))
                goto error;

        if (size < 3) {
                FreePool(data);
                goto error;
        }

        /* Historical: older Fastboot versions saved this as a 16-bit
         * string, newer ones as 8-bit. Do a little inspection to
         * see which is the case, and upconvert as necessary */
        if (data[0] && data[1]) {
                /* 16 bit string with 8bit data would have at least one 0*/
                data[size - 1] = '\0';
                val = stra_to_str(data);
                FreePool(data);
        } else {
                if (size % 2 == 0) {
                        data[size - 1] = '\0';
                        data[size - 2] = '\0';
                        val = (CHAR16 *)data;
                } else {
                        FreePool(data);
                        goto error;
                }
        }

        pos = val;

        /* Only [0-9a-zA-Z,] acceptable. Any funny business, give up */
        while (*pos) {
                if ( ! ( (*pos >= L'0' && *pos <= L'9') ||
                         (*pos >= L'a' && *pos <= L'z') ||
                         (*pos >= L'A' && *pos <= L'Z') ||
                         *pos == L',')) {
                        FreePool(val);
                        goto error;
                }
                pos++;
        }
        return val;
error:
        return StrDuplicate(L"tty0");
}


static CHAR16 *get_wake_reason(void)
{
        enum wake_sources wake_source;
        CHAR16 *reason;

        wake_source = rsci_get_wake_source();
        switch(wake_source) {
        case WAKE_BATTERY_INSERTED:
                reason = StrDuplicate(L"battery_inserted");
                break;
        case WAKE_USB_CHARGER_INSERTED:
                reason = StrDuplicate(L"usb_charger_inserted");
                break;
        case WAKE_ACDC_CHARGER_INSERTED:
                reason = StrDuplicate(L"acdc_charger_inserted");
                break;
        case WAKE_POWER_BUTTON_PRESSED:
                reason = StrDuplicate(L"power_button_pressed");
                break;
        case WAKE_RTC_TIMER:
                reason = StrDuplicate(L"rtc_timer");
                break;
        case WAKE_BATTERY_REACHED_IA_THRESHOLD:
                reason = StrDuplicate(L"battery_reached_ia_threshold");
                break;
        default:
                debug(L"wake_source = 0x%02x", wake_source);
                reason = NULL;
        }

        return reason;
}


static CHAR16 *get_reset_reason(void)
{
        enum reset_sources reset_source;
        CHAR16 *reason;

        reset_source = rsci_get_reset_source();
        switch (reset_source) {
        case RESET_KERNEL_WATCHDOG:
                reason = StrDuplicate(L"watchdog");
                break;
        case RESET_SECURITY_WATCHDOG:
                reason = StrDuplicate(L"security_watchdog");
                break;
        case RESET_PMC_WATCHDOG:
                reason = StrDuplicate(L"pmc_watchdog");
                break;
        case RESET_EC_WATCHDOG:
                reason = StrDuplicate(L"ec_watchdog");
                break;
        case RESET_PLATFORM_WATCHDOG:
                reason = StrDuplicate(L"platform_watchdog");
                break;
        case RESET_SECURITY_INITIATED:
                reason = StrDuplicate(L"security_initiated");
                break;
        default:
                debug(L"reset_source = 0x%02x", reset_source);
                reason = NULL;
        }

        return reason;
}


static CHAR16 *get_boot_reason(void)
{
        CHAR16 *bootreason, *pos;

        bootreason = get_wake_reason();
        if (bootreason)
                goto done;

        bootreason = get_reset_reason();
        if (bootreason)
                goto done;

        bootreason = get_efi_variable_str(&loader_guid,
                                          L"LoaderEntryRebootReason");
        if (!bootreason) {
                debug(L"Error while trying to get LoaderEntryRebootReason variable");
                bootreason = StrDuplicate(L"unknown");
                goto done;
        }

        pos = bootreason;
        while (*pos) {
                /* Only allow alphanumeric characters */
                if (!((*pos >= L'0' && *pos <= L'9') ||
                            (*pos >= L'a' && *pos <= L'z') ||
                            *pos == L'_')) {
                        debug(L"Error, LoaderEntryRebootReason contains non-alphanumeric characters");
                        FreePool(bootreason);
                        bootreason = StrDuplicate(L"unknown");
                        break;
                }
                pos++;
        }
done:
        del_efi_variable(&loader_guid, L"LoaderEntryRebootReason");
        return bootreason;
}


static EFI_STATUS prepend_command_line(CHAR16 **cmdline, CHAR16 *fmt, ...)
{
        CHAR16 *old;
        va_list args;
        CHAR16 *string;
        CHAR16 *new;

        old = *cmdline;
        va_start(args, fmt);
        string = VPoolPrint(fmt, args);
        va_end(args);

        if (!string)
                return EFI_OUT_OF_RESOURCES;

        new = PoolPrint(L"%s %s", string, old);
        FreePool(string);
        if (!new)
                return EFI_OUT_OF_RESOURCES;

        FreePool(old);
        *cmdline = new;
        return EFI_SUCCESS;
}


static CHAR16 *get_command_line(IN struct boot_img_hdr *aosp_header,
                                IN enum boot_target boot_target)
{
        CHAR16 *cmdline16 = NULL;
#ifndef USER
        CHAR16 *cmdline_append = NULL;
        CHAR16 *cmdline_prepend = NULL;
        BOOLEAN needs_pause = FALSE;

        if (boot_target == NORMAL_BOOT) {
                cmdline16 = get_efi_variable_str8(&loader_guid, CMDLINE_REPLACE_VAR);
                cmdline_append = get_efi_variable_str8(&loader_guid, CMDLINE_APPEND_VAR);
                cmdline_prepend = get_efi_variable_str8(&loader_guid, CMDLINE_PREPEND_VAR);
        }
#else
        (void)boot_target; /* Get rid of a unused parameter warning */
#endif

        if (!cmdline16) {
                CHAR8 full_cmdline[BOOT_ARGS_SIZE + BOOT_EXTRA_ARGS_SIZE];

                memcpy(full_cmdline, aosp_header->cmdline, (BOOT_ARGS_SIZE - 1));
                if (aosp_header->cmdline[BOOT_ARGS_SIZE - 2]) {
                        memcpy(full_cmdline + (BOOT_ARGS_SIZE - 1),
                               aosp_header->extra_cmdline,
                               BOOT_EXTRA_ARGS_SIZE);
                }

                cmdline16 = stra_to_str(full_cmdline);

                if (!cmdline16)
                        return NULL;
#ifndef USER
        } else {
                error(L"Boot image command line overridden with '%s'", cmdline16);
                needs_pause = TRUE;
#endif
        }

#ifndef USER
        if (cmdline_prepend) {
                EFI_STATUS ret;

                error(L"Prepending '%s' to command line", cmdline_prepend);
                needs_pause = TRUE;

                ret = prepend_command_line(&cmdline16, L"%s", cmdline_prepend);
                FreePool(cmdline_prepend);
                if (EFI_ERROR(ret))
                        error(L"couldn't prepend to command line");
        }

        if (cmdline_append) {
                EFI_STATUS ret;

                error(L"Appending '%s' to command line", cmdline_append);
                needs_pause = TRUE;

                ret = prepend_command_line(&cmdline_append, L"%s", cmdline16);
                if (EFI_ERROR(ret)) {
                        error(L"couldn't prepend to command line");
                        FreePool(cmdline_append);
                } else {
                        FreePool(cmdline16);
                        cmdline16 = cmdline_append;
                }
        }

        if (needs_pause)
                pause(1);
#endif

        return cmdline16;
}

EFI_STATUS get_bootimage_2nd(VOID *bootimage, VOID **second, UINT32 *size)
{
        struct boot_img_hdr *bh;
        UINT32 offset;

        bh = get_bootimage_header(bootimage);
        if (!bh)
                return EFI_INVALID_PARAMETER;

        /* Nothing to do? */
        if (bh->second_size == 0)
                return EFI_NOT_FOUND;

        offset = bh->page_size + pagealign(bh, bh->kernel_size) +
                 pagealign(bh, bh->ramdisk_size);
        *second = (UINT8*)bootimage + offset;
        *size = bh->second_size;
        return EFI_SUCCESS;
}

#ifdef HAL_AUTODETECT
EFI_STATUS get_bootimage_blob(VOID *bootimage, enum blobtype btype, VOID **blob,
                              UINT32 *blobsize)
{
        VOID *second;
        UINT32 second_size;
        struct blobstore *bs;
        char *device_id;
        EFI_STATUS ret;

        device_id = get_device_id();
        debug(L"Lookup blobstore data %a-%d", device_id, btype);

        ret = get_bootimage_2nd(bootimage, &second, &second_size);
        if (EFI_ERROR(ret))
                return EFI_UNSUPPORTED;

        bs = blobstore_get(second, second_size);
        if (!bs)
                return EFI_UNSUPPORTED;

        if (blobstore_get_item(bs, device_id, btype, blob, blobsize))
                return EFI_NOT_FOUND;

        return EFI_SUCCESS;
}

/* File format is a series of lines, which could be a blank line,
 * #<comment> or <key>=<value>. We don't do sanity checking as the
 * blobstore is covered by the verified boot signature and is hence
 * trusted */
static EFI_STATUS parse_bootvars_line(char *line, VOID *ctx)
{
        CHAR16 **cmdline16 = (CHAR16 **)ctx;

        if (strlen((CHAR8 *)line) == 0 || line[0] == '#')
                return EFI_SUCCESS;

        return prepend_command_line(cmdline16, L"%a", line);
}

static EFI_STATUS add_bootvars(VOID *bootimage, CHAR16 **cmdline16)
{
        VOID *bootvars;
        UINT32 bvsize;
        EFI_STATUS ret;

        ret = get_bootimage_blob(bootimage, BLOB_TYPE_BOOTVARS, &bootvars,
                                 &bvsize);
        if (EFI_ERROR(ret)) {
                if (ret == EFI_UNSUPPORTED || ret == EFI_NOT_FOUND) {
                        debug(L"Not setting bootvars: %r", ret);
                        return EFI_SUCCESS;
                }
                efi_perror(ret, L"Couldn't get bootvars");
                return ret;
        }

        return parse_text_buffer(bootvars, bvsize, parse_bootvars_line,
                                 cmdline16);
}
#endif

static EFI_STATUS setup_command_line(
                IN UINT8 *bootimage,
                IN enum boot_target boot_target,
                IN EFI_GUID *swap_guid,
                IN UINT8 boot_state)
{
        CHAR16 *cmdline16 = NULL;
        char   *serialno = NULL;
        CHAR16 *serialport = NULL;
        CHAR16 *bootreason = NULL;

        EFI_PHYSICAL_ADDRESS cmdline_addr;
        CHAR8 *cmdline;
        UINTN cmdlen;
        EFI_STATUS ret;
        struct boot_params *buf;
        struct boot_img_hdr *aosp_header;

        aosp_header = (struct boot_img_hdr *)bootimage;
        buf = (struct boot_params *)(bootimage + aosp_header->page_size);

        cmdline16 = get_command_line(aosp_header, boot_target);
        if (!cmdline16) {
                ret = EFI_OUT_OF_RESOURCES;
                goto out;
        }

        /* Append serial number from DMI */
        serialno = get_serial_number();
        if (serialno) {
                ret = prepend_command_line(&cmdline16,
                                L"androidboot.serialno=%a g_ffs.iSerialNumber=%a",
                                serialno, serialno);
                if (EFI_ERROR(ret))
                        goto out;
        }

        if (boot_target == CHARGER) {
                ret = prepend_command_line(&cmdline16,
                                L"androidboot.mode=charger");
                if (EFI_ERROR(ret))
                        goto out;
        }

        bootreason = get_boot_reason();
        if (!bootreason) {
                ret = EFI_OUT_OF_RESOURCES;
                goto out;
        }

        ret = prepend_command_line(&cmdline16, L"androidboot.bootreason=%s", bootreason);
        if (EFI_ERROR(ret))
                goto out;

        ret = prepend_command_line(&cmdline16, L"androidboot.verifiedbootstate=%s",
                                   boot_state_to_string(boot_state));
        if (EFI_ERROR(ret))
                goto out;

        if (swap_guid) {
                ret = prepend_command_line(&cmdline16, L"resume=PARTUUID=%g",
                        swap_guid);
                if (EFI_ERROR(ret))
                        goto out;
        }

        serialport = get_serial_port();
        if (!serialport) {
                ret = EFI_OUT_OF_RESOURCES;
                goto out;
        }

        ret = prepend_command_line(&cmdline16, L"console=%s", serialport);
        if (EFI_ERROR(ret))
                goto out;

        PCI_DEVICE_PATH *boot_device = get_boot_device();
        if (boot_device) {
                ret = prepend_command_line(&cmdline16,
                                           L"androidboot.diskbus=%02x.%x",
                                           boot_device->Device,
                                           boot_device->Function);
                if (EFI_ERROR(ret))
                        goto out;
        } else
                error(L"Boot device not found, diskbus parameter not set in the commandline!");

        ret = prepend_command_line(&cmdline16, L"androidboot.bootloader=%a",
                                   get_property_bootloader());
        if (EFI_ERROR(ret))
                goto out;

#ifdef HAL_AUTODETECT
        ret = prepend_command_line(&cmdline16, L"androidboot.brand=%a "
                                   "androidboot.name=%a androidboot.device=%a "
                                   "androidboot.model=%a", get_property_brand(),
                                   get_property_name(), get_property_device(),
                                   get_property_model());
        if (EFI_ERROR(ret))
                goto out;

        ret = add_bootvars(bootimage, &cmdline16);
        if (EFI_ERROR(ret))
                goto out;
#endif

        /* Documentation/x86/boot.txt: "The kernel command line can be located
         * anywhere between the end of the setup heap and 0xA0000" */
        cmdline_addr = 0xA0000;
        cmdlen = StrLen(cmdline16);
        ret = allocate_pages(AllocateMaxAddress, EfiLoaderData,
                             EFI_SIZE_TO_PAGES(cmdlen + 1),
                             &cmdline_addr);
        if (EFI_ERROR(ret))
                goto out;

        cmdline = (CHAR8 *)(UINTN)cmdline_addr;
        ret = str_to_stra(cmdline, cmdline16, cmdlen + 1);
        if (EFI_ERROR(ret)) {
                error(L"Non-ascii characters in command line");
                free_pages(cmdline_addr, EFI_SIZE_TO_PAGES(cmdlen + 1));
                goto out;
        }

        buf->hdr.cmd_line_ptr = (UINT32)(UINTN)cmdline;
        ret = EFI_SUCCESS;
out:
        FreePool(cmdline16);
        FreePool(bootreason);
        FreePool(serialport);

        return ret;
}

static EFI_STATUS handover_kernel(CHAR8 *bootimage, EFI_HANDLE parent_image)
{
        EFI_PHYSICAL_ADDRESS kernel_start;
        EFI_PHYSICAL_ADDRESS boot_addr;
        struct boot_params *boot_params;
        UINT64 init_size;
        EFI_STATUS ret;
        struct boot_img_hdr *aosp_header;
        struct boot_params *buf;
        UINT8 setup_sectors;
        UINT32 setup_size;
        UINT32 ksize;
        UINT32 koffset;

        aosp_header = (struct boot_img_hdr *)bootimage;
        buf = (struct boot_params *)(bootimage + aosp_header->page_size);

        koffset = aosp_header->page_size;
        setup_sectors = buf->hdr.setup_secs;
        setup_sectors++; /* Add boot sector */
        setup_size = (UINT32)setup_sectors * 512;
        ksize = aosp_header->kernel_size - setup_size;
        kernel_start = buf->hdr.pref_address;
        init_size = buf->hdr.init_size;
        buf->hdr.loader_id = 0x1;
        memset(&buf->screen_info, 0x0, sizeof(buf->screen_info));

        ret = allocate_pages(AllocateAddress, EfiLoaderData,
                             EFI_SIZE_TO_PAGES(init_size), &kernel_start);
        if (EFI_ERROR(ret)) {
                /*
                 * We failed to allocate the preferred address, so
                 * just allocate some memory and hope for the best.
                 */
                ret = emalloc(init_size, buf->hdr.kernel_alignment, &kernel_start);
                if (EFI_ERROR(ret))
                        return ret;
        }

        memcpy((CHAR8 *)(UINTN)kernel_start, bootimage + koffset + setup_size, ksize);

        boot_addr = 0x3fffffff;
        ret = allocate_pages(AllocateMaxAddress, EfiLoaderData,
                             EFI_SIZE_TO_PAGES(16384), &boot_addr);
        if (EFI_ERROR(ret))
                goto out;

        /* Free UI resources. */
        ui_free();

#ifndef USER
        log_flush_to_var(FALSE);
#endif

        boot_params = (struct boot_params *)(UINTN)boot_addr;
        memset(boot_params, 0x0, 16384);

        /* Copy first two sectors to boot_params */
        memcpy(boot_params, (CHAR8 *)buf, 2 * 512);
        boot_params->hdr.code32_start = (UINT32)((UINT64)kernel_start);

        ret = EFI_LOAD_ERROR;
        handover_jump(parent_image, boot_params, kernel_start);
        /* Shouldn't get here */

        free_pages(boot_addr, EFI_SIZE_TO_PAGES(16384));
out:
        efree(kernel_start, ksize);
        return ret;
}

EFI_STATUS android_image_load_partition(
                IN const CHAR16 *label,
                OUT VOID **bootimage_p)
{
        UINT32 MediaId;
        UINT32 img_size;
        VOID *bootimage;
        EFI_STATUS ret;
        struct boot_img_hdr aosp_header;
        struct gpt_partition_interface gpart;
        UINTN partition_start;

        *bootimage_p = NULL;
        ret = gpt_get_partition_by_label(label, &gpart, LOGICAL_UNIT_USER);
        if (EFI_ERROR(ret)) {
                debug(L"Partition %s not found", label);
                return ret;
        }
        MediaId = gpart.bio->Media->MediaId;
        partition_start = gpart.part.starting_lba * gpart.bio->Media->BlockSize;

        debug(L"Reading boot image header");
        ret = uefi_call_wrapper(gpart.dio->ReadDisk, 5, gpart.dio, MediaId,
                                partition_start,
                                sizeof(aosp_header), &aosp_header);
        if (EFI_ERROR(ret)) {
                efi_perror(ret, L"ReadDisk (header)");
                return ret;
        }
        if (strncmpa((CHAR8 *)BOOT_MAGIC, aosp_header.magic, BOOT_MAGIC_SIZE)) {
                error(L"This partition does not appear to contain an Android boot image");
                return EFI_INVALID_PARAMETER;
        }

        img_size = bootimage_size(&aosp_header) + BOOT_SIGNATURE_MAX_SIZE;
        bootimage = AllocatePool(img_size);
        if (!bootimage)
                return EFI_OUT_OF_RESOURCES;

        debug(L"Reading full boot image (%d bytes)", img_size);
        ret = uefi_call_wrapper(gpart.dio->ReadDisk, 5, gpart.dio, MediaId, partition_start,
                                img_size, bootimage);
        if (EFI_ERROR(ret)) {
                efi_perror(ret, L"ReadDisk");
                FreePool(bootimage);
                return ret;
        }

        *bootimage_p = bootimage;
        return EFI_SUCCESS;
}


EFI_STATUS android_image_load_file(
                IN EFI_HANDLE device,
                IN CHAR16 *loader,
                IN BOOLEAN delete,
                OUT VOID **bootimage_p)
{
        EFI_STATUS ret, ret2;
        VOID *bootimage = NULL;
        EFI_DEVICE_PATH *path;
        EFI_GUID SimpleFileSystemProtocol = SIMPLE_FILE_SYSTEM_PROTOCOL;
        EFI_GUID EfiFileInfoId = EFI_FILE_INFO_ID;
        EFI_FILE_IO_INTERFACE *drive;
        EFI_FILE_INFO *fileinfo = NULL;
        EFI_FILE *imagefile, *root;
        UINTN buffersize = sizeof(EFI_FILE_INFO);
        struct boot_img_hdr *aosp_header;

        *bootimage_p = NULL;
        debug(L"Locating boot image from file %s", loader);
        path = FileDevicePath(device, loader);
        if (!path) {
                error(L"Error getting device path.");
                uefi_call_wrapper(BS->Stall, 1, 3 * 1000 * 1000);
                return EFI_INVALID_PARAMETER;
        }

        /* Open the device */
        ret = uefi_call_wrapper(BS->HandleProtocol, 3, device,
                        &SimpleFileSystemProtocol, (void **)&drive);
        if (EFI_ERROR(ret)) {
                efi_perror(ret, L"HandleProtocol (SimpleFileSystemProtocol)");
                return ret;
        }
        ret = uefi_call_wrapper(drive->OpenVolume, 2, drive, &root);
        if (EFI_ERROR(ret)) {
                efi_perror(ret, L"OpenVolume");
                return ret;
        }

        /* Get information about the boot image file, we need to know
         * how big it is, and allocate a suitable buffer */
        ret = uefi_call_wrapper(root->Open, 5, root, &imagefile, loader,
                        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
        if (EFI_ERROR(ret)) {
                efi_perror(ret, L"Open");
                return ret;
        }
        fileinfo = AllocatePool(buffersize);
        if (!fileinfo)
                return EFI_OUT_OF_RESOURCES;

        ret = uefi_call_wrapper(imagefile->GetInfo, 4, imagefile,
                        &EfiFileInfoId, &buffersize, fileinfo);
        if (ret == EFI_BUFFER_TOO_SMALL) {
                /* buffersize updated with the required space for
                 * the request */
                FreePool(fileinfo);
                fileinfo = AllocatePool(buffersize);
                if (!fileinfo)
                        return EFI_OUT_OF_RESOURCES;
                ret = uefi_call_wrapper(imagefile->GetInfo, 4, imagefile,
                        &EfiFileInfoId, &buffersize, fileinfo);
        }
        if (EFI_ERROR(ret)) {
                efi_perror(ret, L"GetInfo");
                goto out;
        }
        buffersize = fileinfo->FileSize;

        /* Add BOOT_SIGNATURE_MAX_SIZE just in case the image is unsigned */
        bootimage = AllocatePool(buffersize + BOOT_SIGNATURE_MAX_SIZE);
        if (!bootimage) {
                ret = EFI_OUT_OF_RESOURCES;
                goto out;
        }

        /* Read the file into the buffer */
        ret = uefi_call_wrapper(imagefile->Read, 3, imagefile,
                        &buffersize, bootimage);
        if (ret == EFI_BUFFER_TOO_SMALL) {
                /* buffersize updated with the required space for
                 * the request. By the way it doesn't make any
                 * sense to me why this is needed since we supposedly
                 * got the file size from the GetInfo call but
                 * whatever... */
                FreePool(bootimage);
                bootimage = AllocatePool(buffersize);
                if (!fileinfo) {
                        ret = EFI_OUT_OF_RESOURCES;
                        goto out;
                }
                ret = uefi_call_wrapper(imagefile->Read, 3, imagefile,
                        &buffersize, bootimage);
        }
        if (EFI_ERROR(ret)) {
                efi_perror(ret, L"Read");
                goto out;
        }

        debug(L"Read boot image from file (%d bytes)", buffersize);

        aosp_header = (struct boot_img_hdr *)bootimage;
        if (strncmpa((CHAR8 *)BOOT_MAGIC, aosp_header->magic, BOOT_MAGIC_SIZE)) {
                error(L"File does not appear to contain an Android boot image");
                ret = EFI_INVALID_PARAMETER;
        }
out:
        if (delete) {
                //this should close handle and flush FS
                ret2 = uefi_call_wrapper(imagefile->Delete, 1, imagefile);
                if (EFI_ERROR(ret2)) {
                        efi_perror(ret2, L"Couldn't delete source file");
                        goto out_free;
                }
        } else {
                ret2 = uefi_call_wrapper(imagefile->Close, 1, imagefile);
                if (EFI_ERROR(ret2)) {
                        efi_perror(ret2, L"Couldn't close source file");
                        goto out_free;
                }
        }

out_free:
        FreePool(fileinfo);
        if (ret == EFI_SUCCESS) {
                *bootimage_p = bootimage;
        } else {
                FreePool(bootimage);
        }
        return ret;
}


EFI_STATUS android_image_start_buffer(
                IN EFI_HANDLE parent_image,
                IN VOID *bootimage,
                IN enum boot_target boot_target,
                IN UINT8 boot_state,
                IN EFI_GUID *swap_guid)
{
        struct boot_img_hdr *aosp_header;
        struct boot_params *buf;
        EFI_STATUS ret;

        if (!bootimage)
                return EFI_INVALID_PARAMETER;

        aosp_header = (struct boot_img_hdr *)bootimage;
        if (strncmpa((CHAR8 *)BOOT_MAGIC, aosp_header->magic, BOOT_MAGIC_SIZE)) {
                error(L"buffer does not appear to contain an Android boot image");
                return EFI_INVALID_PARAMETER;
        }

        buf = (struct boot_params *)(bootimage + aosp_header->page_size);

        /* Check boot sector signature */
        if (buf->hdr.signature != 0xAA55) {
                error(L"bzImage kernel corrupt");
                return EFI_INVALID_PARAMETER;
        }

        if (buf->hdr.header != SETUP_HDR) {
                error(L"Setup code version is invalid");
                return EFI_INVALID_PARAMETER;
        }

        if (buf->hdr.version < 0x20c) {
                /* Protocol 2.12, kernel 3.8 required */
                error(L"Kernel header version %x too old", buf->hdr.version);
                return EFI_INVALID_PARAMETER;
        }

#if __LP64__
        if (!(buf->hdr.xloadflags & XLF_EFI_HANDOVER_64)) {
                error(L"This kernel does not support 64-bit EFI Handover protocol");
#else
        if (!(buf->hdr.xloadflags & XLF_EFI_HANDOVER_32)) {
                error(L"This kernel does not support 32-bit EFI Handover protocol");
#endif
                return EFI_INVALID_PARAMETER;
        }

        if (!buf->hdr.relocatable_kernel) {
                Print(L"Expected relocatable kernel\n");
                return EFI_INVALID_PARAMETER;
        }

        debug(L"Creating command line");
        ret = setup_command_line(bootimage, boot_target, swap_guid, boot_state);
        if (EFI_ERROR(ret)) {
                efi_perror(ret, L"setup_command_line");
                return ret;
        }

        debug(L"Loading the ramdisk");
        ret = setup_ramdisk(bootimage);
        if (EFI_ERROR(ret)) {
                efi_perror(ret, L"setup_ramdisk");
                goto out_cmdline;
        }

        debug(L"Loading the kernel");
        ret = handover_kernel(bootimage, parent_image);
        efi_perror(ret, L"handover_kernel");

        efree(buf->hdr.ramdisk_start, buf->hdr.ramdisk_len);
        buf->hdr.ramdisk_start = 0;
        buf->hdr.ramdisk_len = 0;
out_cmdline:
        free_pages(buf->hdr.cmd_line_ptr,
                        strlena((CHAR8 *)(UINTN)buf->hdr.cmd_line_ptr) + 1);
        buf->hdr.cmd_line_ptr = 0;
        return ret;
}


#if DEBUG_MESSAGES
VOID dump_bcb(IN struct bootloader_message *bcb)
{
        CHAR16 *cmd16, *stat16;

        cmd16 = stra_to_str(bcb->command);
        stat16 = stra_to_str(bcb->status);
        if (cmd16 && stat16)
                debug(L"BCB: cmd '%s' status '%s'",
                        cmd16, stat16);
        FreePool(cmd16);
        FreePool(stat16);
}
#else
#define dump_bcb(b) (void)0
#endif

EFI_STATUS read_bcb(
                IN const CHAR16 *label,
                OUT struct bootloader_message *bcb)
{
        EFI_STATUS ret;
        struct gpt_partition_interface gpart;
        UINTN partition_start;

        debug(L"Locating BCB");
        ret = gpt_get_partition_by_label(label, &gpart, LOGICAL_UNIT_USER);
        if (EFI_ERROR(ret))
                return EFI_INVALID_PARAMETER;
        partition_start = gpart.part.starting_lba * gpart.bio->Media->BlockSize;

        debug(L"Reading BCB");
        ret = uefi_call_wrapper(gpart.dio->ReadDisk, 5, gpart.dio,
                                gpart.bio->Media->MediaId,
                                partition_start, sizeof(*bcb), bcb);
        if (EFI_ERROR(ret)) {
                efi_perror(ret, L"ReadDisk (bcb)");
                return ret;
        }
        bcb->command[31] = '\0';
        bcb->status[31] = '\0';
        dump_bcb(bcb);

        return EFI_SUCCESS;
}



EFI_STATUS write_bcb(
                IN const CHAR16 *label,
                IN struct bootloader_message *bcb)
{
        EFI_STATUS ret;
        struct gpt_partition_interface gpart;
        UINTN partition_start;

        debug(L"Locating BCB");
        ret = gpt_get_partition_by_label(label, &gpart, LOGICAL_UNIT_USER);
        if (EFI_ERROR(ret))
                return EFI_INVALID_PARAMETER;
        partition_start = gpart.part.starting_lba * gpart.bio->Media->BlockSize;

        debug(L"Writing BCB");
        ret = uefi_call_wrapper(gpart.dio->WriteDisk, 5, gpart.dio,
                                gpart.bio->Media->MediaId,
                                partition_start, sizeof(*bcb), bcb);
        if (EFI_ERROR(ret)) {
                efi_perror(ret, L"WriteDisk (bcb)");
                return ret;
        }
        dump_bcb(bcb);

        return EFI_SUCCESS;
}


EFI_STATUS android_clear_memory()
{
        UINTN nr_entries, key, entry_sz;
        CHAR8 *mem_entries;
        UINT32 entry_ver;
        UINTN i;
        UINTN counter;
        CHAR8 *mem_map;
        EFI_TPL OldTpl;

        OldTpl = uefi_call_wrapper(BS->RaiseTPL, 1, TPL_NOTIFY);
        mem_entries = (CHAR8 *)LibMemoryMap(&nr_entries, &key, &entry_sz, &entry_ver);
        if (!mem_entries) {
                uefi_call_wrapper(BS->RestoreTPL, 1, OldTpl);
                return EFI_OUT_OF_RESOURCES;
        }
        counter = 0;
        mem_map = mem_entries;
        for (i = 0; i < nr_entries; mem_entries += entry_sz, i++) {
                EFI_MEMORY_DESCRIPTOR *entry;
                UINT64 map_sz;

                entry = (EFI_MEMORY_DESCRIPTOR *)mem_entries;
                map_sz = entry->NumberOfPages * EFI_PAGE_SIZE;

                if (entry->Type == EfiConventionalMemory) {
                        ZeroMem((void *) (UINTN)entry->PhysicalStart, map_sz);
                        counter += entry->NumberOfPages;
                }
        }
        uefi_call_wrapper(BS->RestoreTPL, 1, OldTpl);

        FreePool((void *)mem_map);
        return EFI_SUCCESS;
}

/* vim: softtabstop=8:shiftwidth=8:expandtab
 */
