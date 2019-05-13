// SPDX-License-Identifier: GPL-2.0
/*
 * PlayStation 2 sub-system interface (SIF)
 *
 * The SIF is an interface unit to the I/O processor (IOP).
 *
 * Copyright (C) 2019 Fredrik Noring
 */

#include <linux/build_bug.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/types.h>

#include <asm/io.h>

#include <asm/mach-ps2/dmac.h>
#include <asm/mach-ps2/irq.h>
#include <asm/mach-ps2/sif.h>

#define IOP_RESET_ARGS "rom0:UDNL rom0:OSDCNF"

#define CMD_PACKET_MAX		128
#define CMD_PACKET_DATA_MAX 	112

#define SIF0_BUFFER_SIZE	PAGE_SIZE
#define SIF1_BUFFER_SIZE	PAGE_SIZE

struct sif_rpc_packet_header {
	u32 rec_id;
	void *pkt_addr;
	u32 rpc_id;
};

struct sif_rpc_request_end_packet {
	struct sif_rpc_packet_header header;
	struct sif_rpc_client *client;
	u32 client_id;

	iop_addr_t server;
	iop_addr_t server_buffer;

	void *client_buff;
};

struct sif_rpc_bind_packet {
	struct sif_rpc_packet_header header;
	struct sif_rpc_client *client;
	u32 server_id;
};

struct sif_cmd_header
{
	u32 packet_size : 8;	/* Min 1x16 (header only), max 7*16 bytes */
	u32 data_size : 24;	/* IOP data size in bytes */
	u32 dst;		/* IOP data address or NULL */
	u32 cmd_id;		/* Command id */
	u32 opt;
};

typedef void (*sif_cmd_func)(void *data, void *arg);

struct sif_cmd_handler
{
	sif_cmd_func func;
	void *arg;
};

static DEFINE_SPINLOCK(sregs_lock);
static s32 sregs[32];

static iop_addr_t iop_buffer; /* Address of IOP SIF DMA receive address */
static void *sif0_buffer;
static void *sif1_buffer;

static void cmd_write_sreg(void *data, void *arg)
{
	unsigned long flags;
	const struct {
		u32 reg;
		s32 val;
	} *packet = data;

	BUG_ON(packet->reg >= ARRAY_SIZE(sregs));

	spin_lock_irqsave(&sregs_lock, flags);
	sregs[packet->reg] = packet->val;
	spin_unlock_irqrestore(&sregs_lock, flags);
}

static void sif_write_msflag(u32 value)
{
	outl(value, SIF_MSFLAG);
}

static void sif_write_smflag(u32 value)
{
	outl(value, SIF_SMFLAG);
}

static u32 sif_read_smflag(void)
{
	return inl(SIF_SMFLAG);
}

static bool completed(bool (*condition)(void))
{
	const unsigned long timeout = jiffies + 5*HZ;

	do {
		if (condition())
			return true;

		msleep(1);
	} while (time_is_after_jiffies(timeout));

	return false;
}

static bool sif_smflag_cmdinit(void)
{
	return (sif_read_smflag() & SIF_STATUS_CMDINIT) != 0;
}

static bool sif_smflag_bootend(void)
{
	return (sif_read_smflag() & SIF_STATUS_BOOTEND) != 0;
}

static bool sif1_busy(void)
{
	return (inl(DMAC_SIF1_CHCR) & DMAC_CHCR_BUSY) != 0;
}

static bool sif1_ready(void)
{
	size_t countout = 50000;	/* About 5 s */

	while (sif1_busy() && countout > 0) {
		udelay(100);
		countout--;
	}

	return countout > 0;
}

/* Bytes to 32-bit word count. */
static u32 nbytes_to_wc(size_t nbytes)
{
	const u32 wc = nbytes / 4;

	BUG_ON(nbytes & 0x3);	/* Word count must align */
	BUG_ON(nbytes != (size_t)wc * 4);

	return wc;
}

/* Bytes to 128-bit quadword count. */
static u32 nbytes_to_qwc(size_t nbytes)
{
	const size_t qwc = nbytes / 16;

	BUG_ON(nbytes & 0xf);	/* Quadword count must align */
	BUG_ON(qwc > 0xffff);	/* QWC DMA field is only 16 bits */

	return qwc;
}

static int sif1_write_ert_int_0(const struct sif_cmd_header *header,
	bool ert, bool int_0, iop_addr_t dst, const void *src, size_t nbytes)
{
	const size_t header_size = header != NULL ? sizeof(*header) : 0;
	const size_t aligned_size = (header_size + nbytes + 15) & ~(size_t)15;
	const struct iop_dma_tag iop_dma_tag = {
		.ert	= ert,
		.int_0	= int_0,
		.addr	= dst,
		.wc	= nbytes_to_wc(aligned_size)
	};
	const size_t dma_nbytes = sizeof(iop_dma_tag) + aligned_size;
	u8 *dma_buffer = sif1_buffer;
	dma_addr_t madr;

	if (!aligned_size)
		return 0;
	if (dma_nbytes > SIF1_BUFFER_SIZE)
		return -EINVAL;
	if (!sif1_ready())
		return -EBUSY;

	memcpy(&dma_buffer[0], &iop_dma_tag, sizeof(iop_dma_tag));
	memcpy(&dma_buffer[sizeof(iop_dma_tag)], header, header_size);
	memcpy(&dma_buffer[sizeof(iop_dma_tag) + header_size], src, nbytes);

	madr = virt_to_phys(dma_buffer);
	dma_cache_wback((unsigned long)dma_buffer, dma_nbytes);

	outl(madr, DMAC_SIF1_MADR);
	outl(nbytes_to_qwc(dma_nbytes), DMAC_SIF1_QWC);
	outl(DMAC_CHCR_SENDN_TIE, DMAC_SIF1_CHCR);

	return 0;
}

static int sif1_write(const struct sif_cmd_header *header,
	iop_addr_t dst, const void *src, size_t nbytes)
{
	return sif1_write_ert_int_0(header, false, false, dst, src, nbytes);
}

static int sif1_write_irq(const struct sif_cmd_header *header,
	iop_addr_t dst, const void *src, size_t nbytes)
{
	return sif1_write_ert_int_0(header, true, true, dst, src, nbytes);
}

static int sif_cmd_opt_copy(u32 cmd_id, u32 opt, const void *pkt,
	size_t pktsize, iop_addr_t dst, const void *src, size_t nbytes)
{
	const struct sif_cmd_header header = {
		.cmd_id      = cmd_id,
		.packet_size = sizeof(header) + pktsize,
		.data_size   = nbytes,
		.dst         = dst,
		.opt         = opt
	};
	int err;

	if (pktsize > CMD_PACKET_DATA_MAX)
		return -EINVAL;

	err = sif1_write(NULL, dst, src, nbytes);
	if (!err)
		err = sif1_write_irq(&header, iop_buffer, pkt, pktsize);

	return err;
}

static int sif_cmd_copy(u32 cmd_id, const void *pkt, size_t pktsize,
	iop_addr_t dst, const void *src, size_t nbytes)
{
	return sif_cmd_opt_copy(cmd_id, 0, pkt, pktsize, dst, src, nbytes);
}

static int sif_cmd(u32 cmd_id, const void *pkt, size_t pktsize)
{
	return sif_cmd_copy(cmd_id, pkt, pktsize, 0, NULL, 0);
}

static struct sif_cmd_handler *handler_from_cid(u32 cmd_id)
{
	enum { CMD_HANDLER_MAX = 64 };

	static struct sif_cmd_handler sys_cmds[CMD_HANDLER_MAX];
	static struct sif_cmd_handler usr_cmds[CMD_HANDLER_MAX];

	const u32 id = cmd_id & ~SIF_CMD_ID_SYS;
	struct sif_cmd_handler *cmd_handlers =
		(cmd_id & SIF_CMD_ID_SYS) != 0 ?  sys_cmds : usr_cmds;

	return id < CMD_HANDLER_MAX ? &cmd_handlers[id] : NULL;
}

static void cmd_rpc_end(void *data, void *arg)
{
	const struct sif_rpc_request_end_packet *packet = data;
	struct sif_rpc_client *client = packet->client;

	switch (packet->client_id) {
	case SIF_CMD_RPC_CALL:
		break;

	case SIF_CMD_RPC_BIND:
		client->server = packet->server;
		client->server_buffer = packet->server_buffer;
		break;

	default:
		BUG();
	}

	complete_all(&client->done);
}

static void cmd_rpc_bind(void *data, void *arg)
{
	const struct sif_rpc_bind_packet *bind = data;
	const struct sif_rpc_request_end_packet packet = {
		.client    = bind->client,
		.client_id = SIF_CMD_RPC_BIND,
	};
	int err;

	err = sif_cmd(SIF_CMD_RPC_END, &packet, sizeof(packet));
	if (err)
		pr_err_once("sif: cmd_rpc_bind failed (%d)\n", err);
}

static int sif_request_cmd(u32 cmd_id, sif_cmd_func func, void *arg)
{
	struct sif_cmd_handler *handler = handler_from_cid(cmd_id);

	if (handler == NULL)
		return -EINVAL;

	handler->func = func;
	handler->arg = arg;

	return 0;
}

static void cmd_rpc_irq(void *data, void *arg)
{
	const struct sif_rpc_request_end_packet *packet = data;

	intc_sif_irq(packet->header.rec_id);
}

static int iop_reset_arg(const char *arg)
{
	const size_t arglen = strlen(arg) + 1;
	struct {
		u32 arglen;
		u32 mode;
		char arg[79 + 1];	/* Including NUL */
	} reset_pkt = {
		.arglen = arglen,
		.mode = 0
	};
	int err;

	if (arglen > sizeof(reset_pkt.arg))
		return -EINVAL;
	memcpy(reset_pkt.arg, arg, arglen);

	sif_write_smflag(SIF_STATUS_BOOTEND);

	err = sif_cmd(SIF_CMD_RESET_CMD, &reset_pkt, sizeof(reset_pkt));
	if (err)
		return err;

	sif_write_smflag(SIF_STATUS_SIFINIT);
	sif_write_smflag(SIF_STATUS_CMDINIT);

	return completed(sif_smflag_bootend) ? 0 : -EIO;
}

static int iop_reset(void)
{
	return iop_reset_arg(IOP_RESET_ARGS);
}

static int sif_read_subaddr(dma_addr_t *subaddr)
{
	if (!completed(sif_smflag_cmdinit))
		return -EIO;

	*subaddr = inl(SIF_SUBADDR);

	return 0;
}

static void sif_write_mainaddr_bootend(dma_addr_t mainaddr)
{
	outl(0xff, SIF_UNKNF260);
	outl(mainaddr, SIF_MAINADDR);
	sif_write_msflag(SIF_STATUS_CMDINIT);
	sif_write_msflag(SIF_STATUS_BOOTEND);
}

static void put_dma_buffers(void)
{
	free_page((unsigned long)sif1_buffer);
	free_page((unsigned long)sif0_buffer);
}

static int get_dma_buffers(void)
{
	sif0_buffer = (void *)__get_free_page(GFP_DMA);
	sif1_buffer = (void *)__get_free_page(GFP_DMA);

	if (sif0_buffer == NULL ||
	    sif1_buffer == NULL) {
		put_dma_buffers();
		return -ENOMEM;
	}

	return 0;
}

static int sif_request_cmds(void)
{
	const struct {
		u32 cmd_id;
		sif_cmd_func func;
		struct cmd_data *arg;
	} cmds[] = {
		{ SIF_CMD_WRITE_SREG, cmd_write_sreg, NULL },

		{ SIF_CMD_RPC_END,    cmd_rpc_end,    NULL },
		{ SIF_CMD_RPC_BIND,   cmd_rpc_bind,   NULL },
		{ SIF_CMD_RPC_IRQ,    cmd_rpc_irq,    NULL },
	};
	int err = 0;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(cmds) && err == 0; i++)
		err = sif_request_cmd(cmds[i].cmd_id,
			cmds[i].func, cmds[i].arg);

	return err;
}

static void sif_disable_dma(void)
{
	outl(DMAC_CHCR_STOP, DMAC_SIF0_CHCR);
	outl(0, DMAC_SIF0_MADR);
	outl(0, DMAC_SIF0_QWC);
	inl(DMAC_SIF0_QWC);

	outl(DMAC_CHCR_STOP, DMAC_SIF1_CHCR);
}

static int __init sif_init(void)
{
	int err;

	BUILD_BUG_ON(sizeof(struct sif_rpc_packet_header) != 12);
	BUILD_BUG_ON(sizeof(struct sif_rpc_request_end_packet) != 32);
	BUILD_BUG_ON(sizeof(struct sif_rpc_bind_packet) != 20);
	BUILD_BUG_ON(sizeof(struct sif_cmd_header) != 16);

	sif_disable_dma();

	err = get_dma_buffers();
	if (err) {
		pr_err("sif: Failed to allocate DMA buffers (%d)\n", err);
		goto err_dma_buffers;
	}

	/* Read provisional subaddr in preparation for the IOP reset. */
	err = sif_read_subaddr(&iop_buffer);
	if (err) {
		pr_err("sif: Failed to read provisional subaddr (%d)\n", err);
		goto err_provisional_subaddr;
	}

	/* Write provisional mainaddr in preparation for the IOP reset. */
	sif_write_mainaddr_bootend(virt_to_phys(sif0_buffer));

	err = iop_reset();
	if (err) {
		pr_err("sif: Failed to reset the IOP (%d)\n", err);
		goto err_iop_reset;
	}

	/* Write final mainaddr and indicate end of boot. */
	sif_write_mainaddr_bootend(virt_to_phys(sif0_buffer));

	/* Read final subaddr. */
	err = sif_read_subaddr(&iop_buffer);
	if (err) {
		pr_err("sif: Failed to read final subaddr (%d)\n", err);
		goto err_final_subaddr;
	}

	err = sif_request_cmds();
	if (err) {
		pr_err("sif: Failed to request commands (%d)\n", err);
		goto err_request_commands;
	}

	return 0;

err_request_commands:
err_final_subaddr:
err_iop_reset:
err_provisional_subaddr:
	put_dma_buffers();

err_dma_buffers:
	return err;
}

static void __exit sif_exit(void)
{
	sif_disable_dma();

	put_dma_buffers();
}

module_init(sif_init);
module_exit(sif_exit);

MODULE_DESCRIPTION("PlayStation 2 sub-system interface (SIF) driver");
MODULE_AUTHOR("Fredrik Noring");
MODULE_LICENSE("GPL");
