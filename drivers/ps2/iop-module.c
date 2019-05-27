// SPDX-License-Identifier: GPL-2.0
/*
 * PlayStation 2 I/O processor (IOP) IRX module operations
 *
 * Copyright (C) 2018 Fredrik Noring
 */

#include <linux/init.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/firmware.h>
#include <linux/dma-mapping.h>

#include <asm/mach-ps2/sif.h>
#include <asm/mach-ps2/iop-heap.h>
#include <asm/mach-ps2/iop-memory.h>

#include "iop-module.h"
#include "sif.h"

#define LF_PATH_MAX 252
#define LF_ARG_MAX 252

enum iop_module_rpc_ops {
	rpo_mod_load = 0,
	rpo_elf_load = 1,
	rpo_set_addr = 2,
	rpo_get_addr = 3,
	rpo_mg_mod_load = 4,
	rpo_mg_elf_load = 5,
	rpo_mod_buf_load = 6,
	rpo_mod_stop = 7,
	rpo_mod_unload = 8,
	rpo_search_mod_by_name = 9,
	rpo_search_mod_by_address = 10
};

static struct device *iopmodules_device;	// FIXME: Naming
static struct sif_rpc_client cd_loadfile_rpc;	// FIXME: Naming

int iop_module_load_rom_arg(const char *filepath, const char *arg_)
{
	const char * const arg = arg_ ? arg_ : "";
	const size_t arg_size = strlen(arg) + 1;
	const size_t filepath_size = strlen(filepath) + 1;
	struct {
		u32 addr;
		u32 arg_size;
		char filepath[LF_PATH_MAX];
		char arg[LF_ARG_MAX];
	} load = {
		.arg_size = arg_size
	};
	struct {
		s32 status;
		u32 modres;
	} result;
	int err;

	if (arg_size >= sizeof(load.arg)) {
		err = -EOVERFLOW;
		goto failure;
	}
	memcpy(load.arg, arg, arg_size);

	if (filepath_size >= sizeof(load.filepath)) {
		err = -ENAMETOOLONG;
		goto failure;
	}
	memcpy(load.filepath, filepath, filepath_size);

	err = sif_rpc(&cd_loadfile_rpc, rpo_mod_load,
		&load, sizeof(load), &result, sizeof(result));
	if (err < 0)
		goto failure;

	if (result.status < 0) {
#if 1
		err = -EIO;
#else
		err = result.status == -SIF_RPC_E_GETP  ||	/* FIXME */
		      result.status == -E_SIF_PKT_ALLOC ||
		      result.status == -E_IOP_NO_MEMORY     ? -ENOMEM :
		      result.status == -SIF_RPC_E_SENDP ||
		      result.status == -E_LF_FILE_IO_ERROR  ? -EIO :
		      result.status == -E_LF_FILE_NOT_FOUND ? -ENOENT :
		      result.status == -E_LF_NOT_IRX        ? -ENOEXEC : -EIO;
#endif
		goto failure;
	}

	return result.status;

failure:
	return err;
}
EXPORT_SYMBOL(iop_module_load_rom_arg);

int iop_module_load_rom(const char *filepath)
{
	return iop_module_load_rom_arg(filepath, NULL);
}
EXPORT_SYMBOL(iop_module_load_rom);

int iop_module_load_buffer(const void *buf, size_t nbyte, const char *arg_)
{
	const char * const arg = arg_ ? arg_ : "";
	const size_t arg_size = strlen(arg) + 1;
	struct {
		u32 addr;
		u32 arg_size;
		char filepath[LF_PATH_MAX];
		char arg[LF_ARG_MAX];
	} load = {
		.addr = iop_alloc(nbyte),
		.arg_size = arg_size
	};
	struct {
		s32 status;
		u32 modres;
	} result;
	int err;

	if (!load.addr)
		return -ENOMEM;

	err = iop_write_memory(load.addr, buf, nbyte);
	if (err < 0)
		goto failure;

	if (arg_size >= sizeof(load.arg)) {
		err = -EOVERFLOW;
		goto failure;
	}
	memcpy(load.arg, arg, arg_size);

	err = sif_rpc(&cd_loadfile_rpc, rpo_mod_buf_load,
		&load, sizeof(load), &result, sizeof(result));
	if (err < 0)
		goto failure;

	if (result.status < 0) {
#if 1
		err = -EIO;
#else
		err = result.status == -SIF_RPC_E_GETP  ||	/* FIXME */
		      result.status == -E_SIF_PKT_ALLOC ||
		      result.status == -E_IOP_NO_MEMORY     ? -ENOMEM :
		      result.status == -SIF_RPC_E_SENDP ||
		      result.status == -E_LF_FILE_IO_ERROR  ? -EIO :
		      result.status == -E_LF_FILE_NOT_FOUND ? -ENOENT :
		      result.status == -E_LF_NOT_IRX        ? -ENOEXEC : -EIO;
#endif
		goto failure;
	}

	iop_free(load.addr);
	return result.status;

failure:
	iop_free(load.addr);
	return err;
}
EXPORT_SYMBOL(iop_module_load_buffer);

int iop_module_load_firmware_arg(const char *filepath, const char *arg)
{
	const struct firmware *fw;
	int err;
	int id;

	err = request_firmware(&fw, filepath, iopmodules_device);
	if (err < 0)
		return err;

	id = iop_module_load_buffer(fw->data, fw->size, arg);

	release_firmware(fw);

	return id;
}
EXPORT_SYMBOL(iop_module_load_firmware_arg);

int iop_module_load_firmware(const char *filepath)
{
	return iop_module_load_firmware_arg(filepath, NULL);
}
EXPORT_SYMBOL(iop_module_load_firmware);

static int __init iop_module_init(void)
{
	int err;
	int id;

	iopmodules_device = root_device_register("iop-module"); /* FIXME */
	if (!iopmodules_device) {
		printk(KERN_ERR "iop-module: Failed to register iopmodules root device.\n");
		return -ENOMEM;
	}

	err = sif_rpc_bind(&cd_loadfile_rpc, SIF_SID_LOAD_MODULE);
	if (err < 0) {
		printk(KERN_ERR "iop-module: bind err = %d\n", err);
		return err;
	}

	id = iop_module_load_firmware("ps2/intrelay-direct.irx");
	if (id < 0) {
		printk(KERN_ERR "iop-module: Loading ps2/intrelay-direct.irx failed with err = %d\n", id);
		return id;
	}

	id = iop_module_load_rom("rom0:ADDDRV");
	if (id < 0) {
		printk(KERN_ERR "iop-module: Loading rom0:ADDDRV failed with err = %d\n", id);
		return id;
	}

	return 0;
}

static void __exit iop_module_exit(void)
{
	/* FIXME */

	sif_rpc_unbind(&cd_loadfile_rpc);
}

module_init(iop_module_init);
module_exit(iop_module_exit);

MODULE_LICENSE("GPL");
