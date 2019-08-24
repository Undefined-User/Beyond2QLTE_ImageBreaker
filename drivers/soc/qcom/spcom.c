/*
 * Copyright (c) 2015-2018, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * Secure-Processor-Communication (SPCOM).
 *
 * This driver provides communication to Secure Processor (SP)
 * over RPMSG framework.
 *
 * It provides interface to userspace spcomlib.
 *
 * Userspace application shall use spcomlib for communication with SP. Userspace
 * application can be either client or server. spcomlib shall use write() file
 * operation to send data, and read() file operation to read data.
 *
 * This driver uses RPMSG with glink-spss as a transport layer.
 * This driver exposes "/dev/<sp-channel-name>" file node for each rpmsg logical
 * channel.
 * This driver exposes "/dev/spcom" file node for some debug/control command.
 * The predefined channel "/dev/sp_kernel" is used for loading SP application
 * from HLOS.
 * This driver exposes "/dev/sp_ssr" file node to allow user space poll for SSR.
 * After the remote SP App is loaded, this driver exposes a new file node
 * "/dev/<ch-name>" for the matching HLOS App to use.
 * The access to predefined file nodes and dynamically allocated file nodes is
 * restricted by using unix group and SELinux.
 *
 * No message routing is used, but using the rpmsg/G-Link "multiplexing" feature
 * to use a dedicated logical channel for HLOS and SP Application-Pair.
 *
 * Each HLOS/SP Application can be either Client or Server or both,
 * Messaging is allways point-to-point between 2 HLOS<=>SP applications.
 * Each channel exclusevly used by single Client or Server.
 *
 * User Space Request & Response are synchronous.
 * read() & write() operations are blocking until completed or terminated.
 */
#define pr_fmt(fmt)	KBUILD_MODNAME ": %s: " fmt, __func__

#include <linux/kernel.h>	/* min()             */
#include <linux/module.h>	/* MODULE_LICENSE    */
#include <linux/device.h>	/* class_create()    */
#include <linux/slab.h>	        /* kzalloc()         */
#include <linux/fs.h>		/* file_operations   */
#include <linux/cdev.h>	        /* cdev_add()        */
#include <linux/errno.h>	/* EINVAL, ETIMEDOUT */
#include <linux/printk.h>	/* pr_err()          */
#include <linux/bitops.h>	/* BIT(x)            */
#include <linux/completion.h>	/* wait_for_completion_timeout() */
#include <linux/poll.h>	/* POLLOUT */
#include <linux/platform_device.h>
#include <linux/of.h>		/* of_property_count_strings() */
#include <linux/workqueue.h>
#include <linux/delay.h>	/* msleep() */
#include <linux/dma-buf.h>
#include <linux/limits.h>
#include <linux/rpmsg.h>
#include <linux/atomic.h>
#include <linux/list.h>
#include <uapi/linux/spcom.h>
#include <soc/qcom/subsystem_restart.h>

/**
 * Request buffer size.
 * Any large data (multiply of 4KB) is provided by temp buffer in DDR.
 * Request shall provide the temp buffer physical address (align to 4KB).
 * Maximum request/response size of 268 is used to accommodate APDU size.
 * From kernel spcom driver perspective a PAGE_SIZE of 4K
 * is the actual maximum size for a single read/write file operation.
 */
#define SPCOM_MAX_RESPONSE_SIZE		268

/* SPCOM driver name */
#define DEVICE_NAME	"spcom"

/* maximum ION buffers should be >= SPCOM_MAX_CHANNELS  */
#define SPCOM_MAX_ION_BUF_PER_CH (SPCOM_MAX_CHANNELS + 4)

/* maximum ION buffer per send request/response command */
#define SPCOM_MAX_ION_BUF_PER_CMD SPCOM_MAX_ION_BUF

/* Maximum command size */
#define SPCOM_MAX_COMMAND_SIZE	(PAGE_SIZE)

/* Maximum input size */
#define SPCOM_MAX_READ_SIZE	(PAGE_SIZE)

/* Current Process ID */
#define current_pid() ((u32)(current->pid))

/*
 * After both sides get CONNECTED,
 * there is a race between one side queueing rx buffer and the other side
 * trying to call glink_tx() , this race is only on the 1st tx.
 * Do tx retry with some delay to allow the other side to queue rx buffer.
 */
#define TX_RETRY_DELAY_MSEC	100

/* SPCOM_MAX_REQUEST_SIZE-or-SPCOM_MAX_RESPONSE_SIZE + header */
#define SPCOM_RX_BUF_SIZE	300

/*
 * Initial transaction id, use non-zero nonce for debug.
 * Incremented by client on request, and copied back by server on response.
 */
#define INITIAL_TXN_ID	0x12345678

/**
 * struct spcom_msg_hdr - Request/Response message header between HLOS and SP.
 *
 * This header is proceeding any request specific parameters.
 * The transaction id is used to match request with response.
 * Note: rpmsg API provides the rx/tx data size, so user payload size is
 * calculated by reducing the header size.
 */
struct spcom_msg_hdr {
	uint32_t reserved;	/* for future use */
	uint32_t txn_id;	/* transaction id */
	char buf[0];		/* Variable buffer size, must be last field */
} __packed;

/**
 * struct spcom_client - Client handle
 */
struct spcom_client {
	struct spcom_channel *ch;
};

/**
 * struct spcom_server - Server handle
 */
struct spcom_server {
	struct spcom_channel *ch;
};

/**
 * struct spcom_channel - channel context
 */
struct spcom_channel {
	char name[SPCOM_CHANNEL_NAME_SIZE];
	struct mutex lock;
	uint32_t txn_id;           /* incrementing nonce per client request */
	bool is_server;            /* for txn_id and response_timeout_msec  */
	bool comm_role_undefined; /*  is true on channel creation, before */ 
	/*  first tx/rx on channel */
	uint32_t response_timeout_msec; /* for client only */

	/* char dev */
	struct cdev *cdev;
	struct device *dev;
	struct device_attribute attr;

	/* rpmsg */
	struct rpmsg_driver *rpdrv;
	struct rpmsg_device *rpdev;

	/* Events notification */
	struct completion rx_done;
	struct completion connect;

	/*
	 * Only one client or server per channel.
	 * Only one rx/tx transaction at a time (request + response).
	 */
	bool is_busy;

	u32 pid; /* debug only to find user space application */

	/* abort flags */
	bool rpmsg_abort;

	/* rx data info */
	size_t actual_rx_size;	/* actual data size received */
	void *rpmsg_rx_buf;

	/* shared buffer lock/unlock support */
	int dmabuf_fd_table[SPCOM_MAX_ION_BUF_PER_CH];
	struct dma_buf *dmabuf_handle_table[SPCOM_MAX_ION_BUF_PER_CH];
};

/**
 * struct rx_buff_list - holds rx rpmsg data, before it will be consumed
 * by spcom_signal_rx_done worker, item per rx packet
 */
struct rx_buff_list {
	struct list_head list;

	void *rpmsg_rx_buf;
	int   rx_buf_size;
	struct spcom_channel *ch;
};

/**
 * struct spcom_device - device state structure.
 */
struct spcom_device {
	char predefined_ch_name[SPCOM_MAX_CHANNELS][SPCOM_CHANNEL_NAME_SIZE];

	/* char device info */
	struct cdev cdev;
	dev_t device_no;
	struct class *driver_class;
	struct device *class_dev;
	struct platform_device *pdev;

	/* rpmsg channels */
	struct spcom_channel channels[SPCOM_MAX_CHANNELS];
	atomic_t chdev_count;

	struct completion rpmsg_state_change;
	atomic_t rpmsg_dev_count;

	/* rx data path */
	struct list_head    rx_list_head;
	spinlock_t          rx_lock;
};

/* Device Driver State */
static struct spcom_device *spcom_dev;

/* static functions declaration */
static int spcom_create_channel_chardev(const char *name);
static struct spcom_channel *spcom_find_channel_by_name(const char *name);
static int spcom_register_rpmsg_drv(struct spcom_channel *ch);
static int spcom_unregister_rpmsg_drv(struct spcom_channel *ch);

/**
 * spcom_is_channel_open() - channel is open on this side.
 *
 * Channel is fully connected, when rpmsg driver is registered and
 * rpmsg device probed
 */
static inline bool spcom_is_channel_open(struct spcom_channel *ch)
{
	return ch->rpdrv != NULL;
}

/**
 * spcom_is_channel_connected() - channel is fully connected by both sides.
 */
static inline bool spcom_is_channel_connected(struct spcom_channel *ch)
{
	/* Channel must be open before it gets connected */
	if (!spcom_is_channel_open(ch))
		return false;

	return ch->rpdev != NULL;
}

/**
 * spcom_create_predefined_channels_chardev() - expose predefined channels to
 * user space.
 *
 * Predefined channels list is provided by device tree.  Typically, it is for
 * known servers on remote side that are not loaded by the HLOS
 */
static int spcom_create_predefined_channels_chardev(void)
{
	int i;
	int ret;
	static bool is_predefined_created;

	if (is_predefined_created)
		return 0;

	for (i = 0; i < SPCOM_MAX_CHANNELS; i++) {
		const char *name = spcom_dev->predefined_ch_name[i];

		if (name[0] == 0)
			break;
		ret = spcom_create_channel_chardev(name);
		if (ret) {
			pr_err("failed to create chardev [%s], ret [%d].\n",
			       name, ret);
			return -EFAULT;
		}
	}

	is_predefined_created = true;

	return 0;
}

/*======================================================================*/
/*		UTILITIES						*/
/*======================================================================*/

/**
 * spcom_init_channel() - initialize channel state.
 *
 * @ch: channel state struct pointer
 * @name: channel name
 */
static int spcom_init_channel(struct spcom_channel *ch, const char *name)
{
	if (!ch || !name || !name[0]) {
		pr_err("invalid parameters.\n");
		return -EINVAL;
	}

	strlcpy(ch->name, name, SPCOM_CHANNEL_NAME_SIZE);

	init_completion(&ch->rx_done);
	init_completion(&ch->connect);

	mutex_init(&ch->lock);
	ch->rpdrv = NULL;
	ch->rpdev = NULL;
	ch->actual_rx_size = 0;
	ch->is_busy = false;
	ch->txn_id = INITIAL_TXN_ID; /* use non-zero nonce for debug */
	ch->pid = 0;
	ch->rpmsg_abort = false;
	ch->rpmsg_rx_buf = NULL;
	ch->comm_role_undefined = true;

	return 0;
}

/**
 * spcom_find_channel_by_name() - find a channel by name.
 *
 * @name: channel name
 *
 * Return: a channel state struct.
 */
static struct spcom_channel *spcom_find_channel_by_name(const char *name)
{
	int i;

	for (i = 0 ; i < ARRAY_SIZE(spcom_dev->channels); i++) {
		struct spcom_channel *ch = &spcom_dev->channels[i];

		if (strcmp(ch->name, name) == 0)
			return ch;
	}

	return NULL;
}

/**
 * spcom_rx() - wait for received data until timeout, unless pending rx data is
 *              already ready
 *
 * @ch: channel state struct pointer
 * @buf: buffer pointer
 * @size: buffer size
 *
 * Return: size in bytes on success, negative value on failure.
 */
static int spcom_rx(struct spcom_channel *ch,
		     void *buf,
		     uint32_t size,
		     uint32_t timeout_msec)
{
	unsigned long jiffies = msecs_to_jiffies(timeout_msec);
	long timeleft = 1;
	int ret = 0;

	mutex_lock(&ch->lock);

	/* check for already pending data */
	if (!ch->actual_rx_size) {
		reinit_completion(&ch->rx_done);

		mutex_unlock(&ch->lock); /* unlock while waiting */
		/* wait for rx response */
		pr_debug("wait for rx done, timeout_msec=%d\n", timeout_msec);
		if (timeout_msec)
			timeleft = wait_for_completion_interruptible_timeout(
						     &ch->rx_done, jiffies);
		else
			ret = wait_for_completion_interruptible(&ch->rx_done);

		mutex_lock(&ch->lock);
		if (timeout_msec && timeleft == 0) {
			ch->txn_id++; /*  to drop expired rx packet later */
			pr_err("rx_done timeout expired %d ms, set txn_id=%d\n",
				timeout_msec, ch->txn_id);
			ret = -ETIMEDOUT;
			goto exit_err;
		} else if (ch->rpmsg_abort) {
			pr_warn("rpmsg channel is closing\n");
			ret = -ERESTART;
			goto exit_err;
		} else if (ret < 0 || timeleft == -ERESTARTSYS) {
			pr_debug("wait interrupted: ret=%d, timeleft=%ld\n",
				 ret, timeleft);
			if (timeleft == -ERESTARTSYS)
				ret = -ERESTARTSYS;
			goto exit_err;
		} else if (ch->actual_rx_size) {
			pr_debug("actual_rx_size is [%zu], txn_id %d\n",
				 ch->actual_rx_size, ch->txn_id);
		} else {
			pr_err("actual_rx_size is zero.\n");
			ret = -EFAULT;
			goto exit_err;
		}
	} else {
		pr_debug("pending data size [%zu], requested size [%zu], ch->txn_id %d\n",
			ch->actual_rx_size, size, ch->txn_id);
	}
	if (!ch->rpmsg_rx_buf) {
		pr_err("invalid rpmsg_rx_buf.\n");
		ret = -ENOMEM;
		goto exit_err;
	}

	size = min_t(size_t, ch->actual_rx_size, size);
	memcpy(buf, ch->rpmsg_rx_buf, size);

	pr_debug("copy size [%d].\n", (int) size);

	memset(ch->rpmsg_rx_buf, 0, ch->actual_rx_size);
	kfree((void *)ch->rpmsg_rx_buf);
	ch->rpmsg_rx_buf = NULL;
	ch->actual_rx_size = 0;

	mutex_unlock(&ch->lock);

	return size;
exit_err:
	mutex_unlock(&ch->lock);
	return ret;
}

/**
 * spcom_get_next_request_size() - get request size.
 * already ready
 *
 * @ch: channel state struct pointer
 *
 * Server needs the size of the next request to allocate a request buffer.
 * Initially used intent-request, however this complicated the remote side,
 * so both sides are not using glink_tx() with INTENT_REQ anymore.
 *
 * Return: size in bytes on success, negative value on failure.
 */
static int spcom_get_next_request_size(struct spcom_channel *ch)
{
	int size = -1;
	int ret = 0;

	/* NOTE: Remote clients might not be connected yet.*/
	mutex_lock(&ch->lock);
	reinit_completion(&ch->rx_done);

	/* check if already got it via callback */
	if (ch->actual_rx_size) {
		pr_debug("next-req-size already ready ch [%s] size [%zu]\n",
			 ch->name, ch->actual_rx_size);
		ret = -EFAULT;
		goto exit_ready;
	}
	mutex_unlock(&ch->lock); /* unlock while waiting */

	pr_debug("Wait for Rx Done, ch [%s].\n", ch->name);
	ret = wait_for_completion_interruptible(&ch->rx_done);
	if (ret < 0) {
		pr_debug("ch [%s]:interrupted wait ret=%d\n",
			 ch->name, ret);
		goto exit_error;
	}

	mutex_lock(&ch->lock); /* re-lock after waiting */

	if (ch->actual_rx_size == 0) {
		pr_err("invalid rx size [%zu] ch [%s]\n",
		       ch->actual_rx_size, ch->name);
		mutex_unlock(&ch->lock);
		ret = -EFAULT;
		goto exit_error;
	}

exit_ready:
	/* actual_rx_size not exeeds SPCOM_RX_BUF_SIZE*/
	size = (int)ch->actual_rx_size;
	if (size > sizeof(struct spcom_msg_hdr)) {
		size -= sizeof(struct spcom_msg_hdr);
	} else {
		pr_err("rx size [%d] too small.\n", size);
		ret = -EFAULT;
		mutex_unlock(&ch->lock);
		goto exit_error;
	}

	mutex_unlock(&ch->lock);
	return size;

exit_error:
	return ret;
}

/*======================================================================*/
/*	USER SPACE commands handling					*/
/*======================================================================*/

/**
 * spcom_handle_create_channel_command() - Handle Create Channel command from
 * user space.
 *
 * @cmd_buf:	command buffer.
 * @cmd_size:	command buffer size.
 *
 * Return: 0 on successful operation, negative value otherwise.
 */
static int spcom_handle_create_channel_command(void *cmd_buf, int cmd_size)
{
	int ret = 0;
	struct spcom_user_create_channel_command *cmd = cmd_buf;
	const char *ch_name;
	const size_t maxlen = sizeof(cmd->ch_name);

	if (cmd_size != sizeof(*cmd)) {
		pr_err("cmd_size [%d] , expected [%d].\n",
		       (int) cmd_size,  (int) sizeof(*cmd));
		return -EINVAL;
	}

	ch_name = cmd->ch_name;
	if (strnlen(cmd->ch_name, maxlen) == maxlen) {
		pr_err("channel name is not NULL terminated\n");
		return -EINVAL;
	}

	pr_debug("ch_name [%s].\n", ch_name);

	ret = spcom_create_channel_chardev(ch_name);

	return ret;
}

/**
 * spcom_handle_restart_sp_command() - Handle Restart SP command from
 * user space.
 *
 * Return: 0 on successful operation, negative value otherwise.
 */
static int spcom_handle_restart_sp_command(void)
{
	void *subsystem_get_retval = NULL;

	pr_debug("restart - PIL FW loading process initiated\n");

	subsystem_get_retval = subsystem_get("spss");
	if (!subsystem_get_retval) {
		pr_err("restart - unable to trigger PIL process for FW loading\n");
		return -EINVAL;
	}

	pr_debug("restart - PIL FW loading process is complete\n");
	return 0;
}

/**
 * spcom_handle_send_command() - Handle send request/response from user space.
 *
 * @buf:	command buffer.
 * @buf_size:	command buffer size.
 *
 * Return: 0 on successful operation, negative value otherwise.
 */
static int spcom_handle_send_command(struct spcom_channel *ch,
					     void *cmd_buf, int size)
{
	int ret = 0;
	struct spcom_send_command *cmd = cmd_buf;
	uint32_t buf_size;
	void *buf;
	struct spcom_msg_hdr *hdr;
	void *tx_buf;
	int tx_buf_size;
	uint32_t timeout_msec;
	int time_msec = 0;

	pr_debug("send req/resp ch [%s] size [%d] .\n", ch->name, size);

	/*
	 * check that cmd buf size is at least struct size,
	 * to allow access to struct fields.
	 */
	if (size < sizeof(*cmd)) {
		pr_err("ch [%s] invalid cmd buf.\n",
			ch->name);
		return -EINVAL;
	}

	/* Check if remote side connect */
	if (!spcom_is_channel_connected(ch)) {
		pr_err("ch [%s] remote side not connect.\n", ch->name);
		return -ENOTCONN;
	}

	/* parse command buffer */
	buf = &cmd->buf;
	buf_size = cmd->buf_size;
	timeout_msec = cmd->timeout_msec;

	/* Check param validity */
	if (buf_size > SPCOM_MAX_RESPONSE_SIZE) {
		pr_err("ch [%s] invalid buf size [%d].\n",
			ch->name, buf_size);
		return -EINVAL;
	}
	if (size != sizeof(*cmd) + buf_size) {
		pr_err("ch [%s] invalid cmd size [%d].\n",
			ch->name, size);
		return -EINVAL;
	}

	/* Allocate Buffers*/
	tx_buf_size = sizeof(*hdr) + buf_size;
	tx_buf = kzalloc(tx_buf_size, GFP_KERNEL);
	if (!tx_buf)
		return -ENOMEM;

	/* Prepare Tx Buf */
	hdr = tx_buf;

	mutex_lock(&ch->lock);
	if (ch->comm_role_undefined) {
		pr_debug("ch [%s] send first -> it is client\n", ch->name);
		ch->comm_role_undefined = false;
		ch->is_server = false;
	}

	if (!ch->is_server) {
		ch->txn_id++;   /* client sets the request txn_id */
		ch->response_timeout_msec = timeout_msec;
	}
	hdr->txn_id = ch->txn_id;

	/* user buf */
	memcpy(hdr->buf, buf, buf_size);

	time_msec = 0;
	do {
		if (ch->rpmsg_abort) {
			pr_err("ch [%s] aborted\n", ch->name);
			ret = -ECANCELED;
			break;
		}
		/* may fail when RX intent not queued by SP */
		ret = rpmsg_trysend(ch->rpdev->ept, tx_buf, tx_buf_size);
		if (ret == 0)
			break;
		time_msec += TX_RETRY_DELAY_MSEC;
		mutex_unlock(&ch->lock);
		msleep(TX_RETRY_DELAY_MSEC);
		mutex_lock(&ch->lock);
	} while ((ret == -EBUSY || ret == -EAGAIN) && time_msec < timeout_msec);
	if (ret)
		pr_err("ch [%s] rpmsg_trysend() error (%d), timeout_msec=%d\n",
		       ch->name, ret, timeout_msec);
	mutex_unlock(&ch->lock);

	kfree(tx_buf);
	return ret;
}

/**
 * modify_ion_addr() - replace the ION buffer virtual address with physical
 * address in a request or response buffer.
 *
 * @buf: buffer to modify
 * @buf_size: buffer size
 * @ion_info: ION buffer info such as FD and offset in buffer.
 *
 * Return: 0 on successful operation, negative value otherwise.
 */
static int modify_ion_addr(void *buf,
			    uint32_t buf_size,
			    struct spcom_ion_info ion_info)
{
	struct dma_buf *dma_buf;
	struct dma_buf_attachment *attach;
	struct sg_table *sg = NULL;
	dma_addr_t phy_addr = 0;
	int fd, ret = 0;
	uint32_t buf_offset;
	char *ptr = (char *)buf;

	fd = ion_info.fd;
	buf_offset = ion_info.buf_offset;
	ptr += buf_offset;

	if (fd < 0) {
		pr_err("invalid fd [%d].\n", fd);
		return -ENODEV;
	}

	if (buf_size < sizeof(uint64_t)) {
		pr_err("buf size too small [%d].\n", buf_size);
		return -ENODEV;
	}

	if (buf_offset % sizeof(uint64_t))
		pr_debug("offset [%d] is NOT 64-bit aligned.\n", buf_offset);
	else
		pr_debug("offset [%d] is 64-bit aligned.\n", buf_offset);

	if (buf_offset > buf_size - sizeof(uint64_t)) {
		pr_err("invalid buf_offset [%d].\n", buf_offset);
		return -ENODEV;
	}

	dma_buf = dma_buf_get(fd);
	if (IS_ERR_OR_NULL(dma_buf)) {
		pr_err("fail to get dma buf handle.\n");
		return -EINVAL;
	}
	pr_debug("dma_buf handle ok.\n");
	attach = dma_buf_attach(dma_buf, &spcom_dev->pdev->dev);
	if (IS_ERR_OR_NULL(attach)) {
		ret = PTR_ERR(attach);
		pr_err("fail to attach dma buf %d\n", ret);
		dma_buf_put(dma_buf);
		goto mem_map_table_failed;
	}

	sg = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR_OR_NULL(sg)) {
		ret = PTR_ERR(sg);
		pr_err("fail to get sg table of dma buf %d\n", ret);
		goto mem_map_table_failed;
	}
	if (sg->sgl) {
		phy_addr = sg->sgl->dma_address;
	} else {
		pr_err("sgl is NULL\n");
		ret = -ENOMEM;
		goto mem_map_sg_failed;
	}

	/* Set the physical address at the buffer offset */
	pr_debug("ion phys addr = [0x%lx].\n", (long int) phy_addr);
	memcpy(ptr, &phy_addr, sizeof(phy_addr));

mem_map_sg_failed:
	dma_buf_unmap_attachment(attach, sg, DMA_BIDIRECTIONAL);
mem_map_table_failed:
	dma_buf_detach(dma_buf, attach);
	dma_buf_put(dma_buf);

	return ret;
}

/**
 * spcom_handle_send_modified_command() - send a request/response with ION
 * buffer address. Modify the request/response by replacing the ION buffer
 * virtual address with the physical address.
 *
 * @ch: channel pointer
 * @cmd_buf: User space command buffer
 * @size: size of user command buffer
 *
 * Return: 0 on successful operation, negative value otherwise.
 */
static int spcom_handle_send_modified_command(struct spcom_channel *ch,
					       void *cmd_buf, int size)
{
	int ret = 0;
	struct spcom_user_send_modified_command *cmd = cmd_buf;
	uint32_t buf_size;
	void *buf;
	struct spcom_msg_hdr *hdr;
	void *tx_buf;
	int tx_buf_size;
	struct spcom_ion_info ion_info[SPCOM_MAX_ION_BUF_PER_CMD];
	int i;
	uint32_t timeout_msec;
	int time_msec = 0;

	pr_debug("send req/resp ch [%s] size [%d] .\n", ch->name, size);

	/*
	 * check that cmd buf size is at least struct size,
	 * to allow access to struct fields.
	 */
	if (size < sizeof(*cmd)) {
		pr_err("ch [%s] invalid cmd buf.\n",
			ch->name);
		return -EINVAL;
	}

	/* Check if remote side connect */
	if (!spcom_is_channel_connected(ch)) {
		pr_err("ch [%s] remote side not connect.\n", ch->name);
		return -ENOTCONN;
	}

	/* parse command buffer */
	buf = &cmd->buf;
	buf_size = cmd->buf_size;
	timeout_msec = cmd->timeout_msec;
	memcpy(ion_info, cmd->ion_info, sizeof(ion_info));

	/* Check param validity */
	if (buf_size > SPCOM_MAX_RESPONSE_SIZE) {
		pr_err("ch [%s] invalid buf size [%d].\n",
			ch->name, buf_size);
		return -EINVAL;
	}
	if (size != sizeof(*cmd) + buf_size) {
		pr_err("ch [%s] invalid cmd size [%d].\n",
			ch->name, size);
		return -EINVAL;
	}

	/* Allocate Buffers*/
	tx_buf_size = sizeof(*hdr) + buf_size;
	tx_buf = kzalloc(tx_buf_size, GFP_KERNEL);
	if (!tx_buf)
		return -ENOMEM;

	/* Prepare Tx Buf */
	hdr = tx_buf;

	mutex_lock(&ch->lock);
	if (ch->comm_role_undefined) {
		pr_debug("ch [%s] send first -> it is client\n", ch->name);
		ch->comm_role_undefined = false;
		ch->is_server = false;
	}
	if (!ch->is_server) {
		ch->txn_id++;   /* client sets the request txn_id */
		ch->response_timeout_msec = timeout_msec;
	}
	hdr->txn_id = ch->txn_id;

	/* user buf */
	memcpy(hdr->buf, buf, buf_size);

	for (i = 0 ; i < ARRAY_SIZE(ion_info) ; i++) {
		if (ion_info[i].fd >= 0) {
			ret = modify_ion_addr(hdr->buf, buf_size, ion_info[i]);
			if (ret < 0) {
				mutex_unlock(&ch->lock);
				pr_err("modify_ion_addr() error [%d].\n", ret);
				memset(tx_buf, 0, tx_buf_size);
				kfree(tx_buf);
				return -EFAULT;
			}
		}
	}

	time_msec = 0;
	do {
		if (ch->rpmsg_abort) {
			pr_err("ch [%s] aborted\n", ch->name);
			ret = -ECANCELED;
			break;
		}
		/* may fail when RX intent not queued by SP */
		ret = rpmsg_trysend(ch->rpdev->ept, tx_buf, tx_buf_size);
		if (ret == 0)
			break;
		time_msec += TX_RETRY_DELAY_MSEC;
		mutex_unlock(&ch->lock);
		msleep(TX_RETRY_DELAY_MSEC);
		mutex_lock(&ch->lock);
	} while ((ret == -EBUSY || ret == -EAGAIN) && time_msec < timeout_msec);
	if (ret)
		pr_err("ch [%s] rpmsg_trysend() error (%d), timeout_msec=%d\n",
		       ch->name, ret, timeout_msec);

	mutex_unlock(&ch->lock);
	memset(tx_buf, 0, tx_buf_size);
	kfree(tx_buf);
	return ret;
}


/**
 * spcom_handle_lock_ion_buf_command() - Lock an shared buffer.
 *
 * Lock an shared buffer, prevent it from being free if the userspace App crash,
 * while it is used by the remote subsystem.
 */
static int spcom_handle_lock_ion_buf_command(struct spcom_channel *ch,
					      void *cmd_buf, int size)
{
	struct spcom_user_command *cmd = cmd_buf;
	int fd;
	int i;
	struct dma_buf *dma_buf;

	if (size != sizeof(*cmd)) {
		pr_err("cmd size [%d] , expected [%d].\n",
		       (int) size,  (int) sizeof(*cmd));
		return -EINVAL;
	}

	if (cmd->arg > (unsigned int)INT_MAX) {
		pr_err("int overflow [%u]\n", cmd->arg);
		return -EINVAL;
	}
	fd = cmd->arg;

	dma_buf = dma_buf_get(fd);
	if (IS_ERR_OR_NULL(dma_buf)) {
		pr_err("fail to get dma buf handle.\n");
		return -EINVAL;
	}
	pr_debug("dma_buf referenced ok.\n");

	/* shared buf lock doesn't involve any rx/tx data to SP. */
	mutex_lock(&ch->lock);

	/* Check if this shared buffer is already locked */
	for (i = 0 ; i < ARRAY_SIZE(ch->dmabuf_handle_table) ; i++) {
		if (ch->dmabuf_handle_table[i] == dma_buf) {
			pr_debug("fd [%d] shared buf is already locked.\n", fd);
			/* decrement back the ref count */
			mutex_unlock(&ch->lock);
			dma_buf_put(dma_buf);
			return -EINVAL;
		}
	}

	/* Store the dma_buf handle */
	for (i = 0 ; i < ARRAY_SIZE(ch->dmabuf_handle_table) ; i++) {
		if (ch->dmabuf_handle_table[i] == NULL) {
			ch->dmabuf_handle_table[i] = dma_buf;
			ch->dmabuf_fd_table[i] = fd;
			pr_debug("ch [%s] locked ion buf #%d fd [%d] dma_buf=0x%x\n",
				ch->name, i,
				ch->dmabuf_fd_table[i],
				ch->dmabuf_handle_table[i]);
			mutex_unlock(&ch->lock);
			return 0;
		}
	}

	mutex_unlock(&ch->lock);
	/* decrement back the ref count */
	dma_buf_put(dma_buf);
	pr_err("no free entry to store ion handle of fd [%d].\n", fd);

	return -EFAULT;
}

/**
 * spcom_handle_unlock_ion_buf_command() - Unlock an ION buffer.
 *
 * Unlock an ION buffer, let it be free, when it is no longer being used by
 * the remote subsystem.
 */
static int spcom_handle_unlock_ion_buf_command(struct spcom_channel *ch,
					      void *cmd_buf, int size)
{
	int i;
	struct spcom_user_command *cmd = cmd_buf;
	int fd;
	bool found = false;
	struct dma_buf *dma_buf;

	if (size != sizeof(*cmd)) {
		pr_err("cmd size [%d], expected [%d]\n",
		       (int)size, (int)sizeof(*cmd));
		return -EINVAL;
	}
	if (cmd->arg > (unsigned int)INT_MAX) {
		pr_err("int overflow [%u]\n", cmd->arg);
		return -EINVAL;
	}
	fd = cmd->arg;

	pr_debug("Unlock ion buf ch [%s] fd [%d].\n", ch->name, fd);

	dma_buf = dma_buf_get(fd);
	if (IS_ERR_OR_NULL(dma_buf)) {
		pr_err("fail to get dma buf handle\n");
		return -EINVAL;
	}
	dma_buf_put(dma_buf);
	pr_debug("dma_buf referenced ok\n");

	/* shared buf unlock doesn't involve any rx/tx data to SP. */
	mutex_lock(&ch->lock);
	if (fd == (int) SPCOM_ION_FD_UNLOCK_ALL) {
		pr_debug("unlocked ALL ion buf ch [%s].\n", ch->name);
		found = true;
		/* unlock all buf */
		for (i = 0; i < ARRAY_SIZE(ch->dmabuf_handle_table); i++) {
			if (ch->dmabuf_handle_table[i] != NULL) {
				pr_debug("unlocked ion buf #%d fd [%d]\n",
					i, ch->dmabuf_fd_table[i]);
				dma_buf_put(ch->dmabuf_handle_table[i]);
				ch->dmabuf_handle_table[i] = NULL;
				ch->dmabuf_fd_table[i] = -1;
			}
		}
	} else {
		/* unlock specific buf */
		for (i = 0 ; i < ARRAY_SIZE(ch->dmabuf_handle_table) ; i++) {
			if (!ch->dmabuf_handle_table[i])
				continue;
			if (ch->dmabuf_handle_table[i] == dma_buf) {
				pr_debug("ch [%s] unlocked ion buf #%d fd [%d] dma_buf=0x%x\n",
					ch->name, i,
					ch->dmabuf_fd_table[i],
					ch->dmabuf_handle_table[i]);
				dma_buf_put(ch->dmabuf_handle_table[i]);
				ch->dmabuf_handle_table[i] = NULL;
				ch->dmabuf_fd_table[i] = -1;
				found = true;
				break;
			}
		}
	}
	mutex_unlock(&ch->lock);

	if (!found) {
		pr_err("ch [%s] fd [%d] was not found.\n", ch->name, fd);
		return -ENODEV;
	}

	return 0;
}

/**
 * spcom_handle_write() - Handle user space write commands.
 *
 * @buf:	command buffer.
 * @buf_size:	command buffer size.
 *
 * Return: 0 on successful operation, negative value otherwise.
 */
static int spcom_handle_write(struct spcom_channel *ch,
			       void *buf,
			       int buf_size)
{
	int ret = 0;
	struct spcom_user_command *cmd = NULL;
	int cmd_id = 0;

	/* Minimal command should have command-id and argument */
	if (buf_size < sizeof(struct spcom_user_command)) {
		pr_err("Command buffer size [%d] too small\n", buf_size);
		return -EINVAL;
	}

	cmd = (struct spcom_user_command *)buf;
	cmd_id = (int) cmd->cmd_id;

	pr_debug("cmd_id [0x%x]\n", cmd_id);

	if (!ch && cmd_id != SPCOM_CMD_CREATE_CHANNEL
			&& cmd_id != SPCOM_CMD_RESTART_SP) {
		pr_err("channel context is null\n");
		return -EINVAL;
	}

	switch (cmd_id) {
	case SPCOM_CMD_SEND:
		ret = spcom_handle_send_command(ch, buf, buf_size);
		break;
	case SPCOM_CMD_SEND_MODIFIED:
		ret = spcom_handle_send_modified_command(ch, buf, buf_size);
		break;
	case SPCOM_CMD_LOCK_ION_BUF:
		ret = spcom_handle_lock_ion_buf_command(ch, buf, buf_size);
		break;
	case SPCOM_CMD_UNLOCK_ION_BUF:
		ret = spcom_handle_unlock_ion_buf_command(ch, buf, buf_size);
		break;
	case SPCOM_CMD_CREATE_CHANNEL:
		ret = spcom_handle_create_channel_command(buf, buf_size);
		break;
	case SPCOM_CMD_RESTART_SP:
		ret = spcom_handle_restart_sp_command();
		break;
	default:
		pr_err("Invalid Command Id [0x%x].\n", (int) cmd->cmd_id);
		ret = -EINVAL;
	}

	return ret;
}

/**
 * spcom_handle_get_req_size() - Handle user space get request size command
 *
 * @ch:	channel handle
 * @buf:	command buffer.
 * @size:	command buffer size.
 *
 * Return: size in bytes on success, negative value on failure.
 */
static int spcom_handle_get_req_size(struct spcom_channel *ch,
				      void *buf,
				      uint32_t size)
{
	int ret = -1;
	uint32_t next_req_size = 0;

	if (size < sizeof(next_req_size)) {
		pr_err("buf size [%d] too small.\n", (int) size);
		return -EINVAL;
	}

	ret = spcom_get_next_request_size(ch);
	if (ret < 0)
		return ret;
	next_req_size = (uint32_t) ret;

	memcpy(buf, &next_req_size, sizeof(next_req_size));
	pr_debug("next_req_size [%d].\n", next_req_size);

	return sizeof(next_req_size); /* can't exceed user buffer size */
}

/**
 * spcom_handle_read_req_resp() - Handle user space get request/response command
 *
 * @ch:	channel handle
 * @buf:	command buffer.
 * @size:	command buffer size.
 *
 * Return: size in bytes on success, negative value on failure.
 */
static int spcom_handle_read_req_resp(struct spcom_channel *ch,
				       void *buf,
				       uint32_t size)
{
	int ret;
	struct spcom_msg_hdr *hdr;
	void *rx_buf;
	int rx_buf_size;
	uint32_t timeout_msec = 0; /* client only */

	/* Check if remote side connect */
	if (!spcom_is_channel_connected(ch)) {
		pr_err("ch [%s] remote side not connect.\n", ch->name);
		return -ENOTCONN;
	}

	/* Check param validity */
	if (size > SPCOM_MAX_RESPONSE_SIZE) {
		pr_err("ch [%s] invalid size [%d].\n",
			ch->name, size);
		return -EINVAL;
	}

	/* Allocate Buffers*/
	rx_buf_size = sizeof(*hdr) + size;
	rx_buf = kzalloc(rx_buf_size, GFP_KERNEL);
	if (!rx_buf)
		return -ENOMEM;

	/*
	 * client response timeout depends on the request
	 * handling time on the remote side .
	 */
	if (!ch->is_server) {
		timeout_msec = ch->response_timeout_msec;
		pr_debug("response_timeout_msec = %d.\n", (int) timeout_msec);
	}

	ret = spcom_rx(ch, rx_buf, rx_buf_size, timeout_msec);
	if (ret < 0) {
		pr_err("rx error %d.\n", ret);
		goto exit_err;
	} else {
		size = ret; /* actual_rx_size */
	}

	hdr = rx_buf;

	if (ch->is_server) {
		ch->txn_id = hdr->txn_id;
		pr_debug("request txn_id [0x%x].\n", ch->txn_id);
	}

	/* copy data to user without the header */
	if (size > sizeof(*hdr)) {
		size -= sizeof(*hdr);
		memcpy(buf, hdr->buf, size);
	} else {
		pr_err("rx size [%d] too small.\n", size);
		ret = -EFAULT;
		goto exit_err;
	}

	kfree(rx_buf);
	return size;
exit_err:
	kfree(rx_buf);
	return ret;
}

/**
 * spcom_handle_read() - Handle user space read request/response or
 * request-size command
 *
 * @ch:	channel handle
 * @buf:	command buffer.
 * @size:	command buffer size.
 *
 * A special size SPCOM_GET_NEXT_REQUEST_SIZE, which is bigger than the max
 * response/request tells the kernel that user space only need the size.
 *
 * Return: size in bytes on success, negative value on failure.
 */
static int spcom_handle_read(struct spcom_channel *ch,
			      void *buf,
			      uint32_t size)
{
	int ret = -1;

	if (size == SPCOM_GET_NEXT_REQUEST_SIZE) {
		pr_debug("get next request size, ch [%s].\n", ch->name);
		ch->is_server = true;
		ret = spcom_handle_get_req_size(ch, buf, size);
	} else {
		pr_debug("get request/response, ch [%s].\n", ch->name);
		ret = spcom_handle_read_req_resp(ch, buf, size);
	}

	pr_debug("ch [%s] , size = %d.\n", ch->name, size);

	return ret;
}

/*======================================================================*/
/*		CHAR DEVICE USER SPACE INTERFACE			*/
/*======================================================================*/

/**
 * file_to_filename() - get the filename from file pointer.
 *
 * @filp: file pointer
 *
 * it is used for debug prints.
 *
 * Return: filename string or "unknown".
 */
static char *file_to_filename(struct file *filp)
{
	struct dentry *dentry = NULL;
	char *filename = NULL;

	if (!filp || !filp->f_path.dentry)
		return "unknown";

	dentry = filp->f_path.dentry;
	filename = dentry->d_iname;

	return filename;
}

/**
 * spcom_device_open() - handle channel file open() from user space.
 *
 * @filp: file pointer
 *
 * The file name (without path) is the channel name.
 * Register rpmsg driver matching with channel name.
 * Store the channel context in the file private date pointer for future
 * read/write/close operations.
 */
static int spcom_device_open(struct inode *inode, struct file *filp)
{
	struct spcom_channel *ch;
	int ret;
	const char *name = file_to_filename(filp);
	u32 pid = current_pid();

	pr_debug("open file [%s].\n", name);

	if (strcmp(name, "unknown") == 0) {
		pr_err("name is unknown\n");
		return -EINVAL;
	}

	if (strcmp(name, DEVICE_NAME) == 0) {
		pr_debug("root dir skipped.\n");
		return 0;
	}

	if (strcmp(name, "sp_ssr") == 0) {
		pr_debug("sp_ssr dev node skipped.\n");
		return 0;
	}

	ch = spcom_find_channel_by_name(name);
	if (!ch) {
		pr_err("channel %s doesn't exist, load App first.\n", name);
		return -ENODEV;
	}

	mutex_lock(&ch->lock);
	if (!spcom_is_channel_open(ch)) {
		reinit_completion(&ch->connect);
		/* channel was closed need to register drv again */
		ret = spcom_register_rpmsg_drv(ch);
		if (ret < 0) {
			pr_err("register rpmsg driver failed %d\n", ret);
			mutex_unlock(&ch->lock);
			return ret;
		}
	}
	/* only one client/server may use the channel */
	if (ch->is_busy) {
		pr_err("channel [%s] is BUSY, already in use by pid [%d].\n",
			name, ch->pid);
		mutex_unlock(&ch->lock);
		return -EBUSY;
	}

	ch->is_busy = true;
	ch->pid = pid;
	mutex_unlock(&ch->lock);

	filp->private_data = ch;
	return 0;
}

/**
 * spcom_device_release() - handle channel file close() from user space.
 *
 * @filp: file pointer
 *
 * The file name (without path) is the channel name.
 * Open the relevant glink channel.
 * Store the channel context in the file private
 * date pointer for future read/write/close
 * operations.
 */
static int spcom_device_release(struct inode *inode, struct file *filp)
{
	struct spcom_channel *ch;
	const char *name = file_to_filename(filp);
	int ret = 0;

	if (strcmp(name, "unknown") == 0) {
		pr_err("name is unknown\n");
		return -EINVAL;
	}

	if (strcmp(name, DEVICE_NAME) == 0) {
		pr_debug("root dir skipped.\n");
		return 0;
	}

	if (strcmp(name, "sp_ssr") == 0) {
		pr_debug("sp_ssr dev node skipped.\n");
		return 0;
	}

	ch = filp->private_data;
	if (!ch) {
		pr_debug("ch is NULL, file name %s.\n", file_to_filename(filp));
		return -ENODEV;
	}

	mutex_lock(&ch->lock);
	/* channel might be already closed or disconnected */
	if (!spcom_is_channel_open(ch)) {
		pr_debug("ch [%s] already closed.\n", name);
		mutex_unlock(&ch->lock);
		return 0;
	}

	ch->is_busy = false;
	ch->pid = 0;
	if (ch->rpmsg_rx_buf) {
		pr_debug("ch [%s] discarting unconsumed rx packet actual_rx_size=%d\n",
			name, ch->actual_rx_size);
		kfree(ch->rpmsg_rx_buf);
		ch->rpmsg_rx_buf = NULL;
	} 
	ch->actual_rx_size = 0;
	mutex_unlock(&ch->lock);
	filp->private_data = NULL;

	return ret;
}

/**
 * spcom_device_write() - handle channel file write() from user space.
 *
 * @filp: file pointer
 *
 * Return: On Success - same size as number of bytes to write.
 * On Failure - negative value.
 */
static ssize_t spcom_device_write(struct file *filp,
				   const char __user *user_buff,
				   size_t size, loff_t *f_pos)
{
	int ret;
	char *buf;
	struct spcom_channel *ch;
	const char *name = file_to_filename(filp);
	int buf_size = 0;

	if (!user_buff || !f_pos || !filp) {
		pr_err("invalid null parameters.\n");
		return -EINVAL;
	}

	if (*f_pos != 0) {
		pr_err("offset should be zero, no sparse buffer.\n");
		return -EINVAL;
	}

	if (!name) {
		pr_err("name is NULL\n");
		return -EINVAL;
	}
	pr_debug("write file [%s] size [%d] pos [%d].\n",
		 name, (int) size, (int) *f_pos);

	if (strcmp(name, "unknown") == 0) {
		pr_err("name is unknown\n");
		return -EINVAL;
	}

	ch = filp->private_data;
	if (!ch) {
		if (strcmp(name, DEVICE_NAME) != 0) {
			pr_err("invalid ch pointer, command not allowed.\n");
			return -EINVAL;
		}
		pr_debug("control device - no channel context.\n");
	} else {
		/* Check if remote side connect */
		if (!spcom_is_channel_connected(ch)) {
			pr_err("ch [%s] remote side not connect.\n", ch->name);
			return -ENOTCONN;
		}
	}

	if (size > SPCOM_MAX_COMMAND_SIZE) {
		pr_err("size [%d] > max size [%d].\n",
			   (int) size, (int) SPCOM_MAX_COMMAND_SIZE);
		return -EINVAL;
	}
	buf_size = size; /* explicit casting size_t to int */
	buf = kzalloc(size, GFP_KERNEL);
	if (buf == NULL)
		return -ENOMEM;

	ret = copy_from_user(buf, user_buff, size);
	if (ret) {
		pr_err("Unable to copy from user (err %d).\n", ret);
		kfree(buf);
		return -EFAULT;
	}

	ret = spcom_handle_write(ch, buf, buf_size);
	if (ret) {
		pr_err("handle command error [%d].\n", ret);
		kfree(buf);
		return ret;
	}

	kfree(buf);

	return size;
}

/**
 * spcom_device_read() - handle channel file read() from user space.
 *
 * @filp: file pointer
 *
 * Return: number of bytes to read on success, negative value on
 * failure.
 */
static ssize_t spcom_device_read(struct file *filp, char __user *user_buff,
				 size_t size, loff_t *f_pos)
{
	int ret = 0;
	int actual_size = 0;
	char *buf;
	struct spcom_channel *ch;
	const char *name = file_to_filename(filp);
	uint32_t buf_size = 0;

	pr_debug("read file [%s], size = %d bytes.\n", name, (int) size);

	if (strcmp(name, "unknown") == 0) {
		pr_err("name is unknown\n");
		return -EINVAL;
	}

	if (!user_buff || !f_pos ||
	    (size == 0) || (size > SPCOM_MAX_READ_SIZE)) {
		pr_err("invalid parameters.\n");
		return -EINVAL;
	}
	buf_size = size; /* explicit casting size_t to uint32_t */

	ch = filp->private_data;

	if (ch == NULL) {
		pr_err("invalid ch pointer, file [%s].\n", name);
		return -EINVAL;
	}

	if (!spcom_is_channel_open(ch)) {
		pr_err("ch is not open, file [%s].\n", name);
		return -EINVAL;
	}

	buf = kzalloc(size, GFP_KERNEL);
	if (buf == NULL)
		return -ENOMEM;

	ret = spcom_handle_read(ch, buf, buf_size);
	if (ret < 0) {
		if (ret != -ERESTARTSYS)
			pr_err("read error [%d].\n", ret);
		kfree(buf);
		return ret;
	}
	actual_size = ret;
	if ((actual_size == 0) || (actual_size > size)) {
		pr_err("invalid actual_size [%d].\n", actual_size);
		kfree(buf);
		return -EFAULT;
	}

	ret = copy_to_user(user_buff, buf, actual_size);
	if (ret) {
		pr_err("Unable to copy to user, err = %d.\n", ret);
		kfree(buf);
		return -EFAULT;
	}

	kfree(buf);
	pr_debug("ch [%s] ret [%d].\n", name, (int) actual_size);

	return actual_size;
}

/**
 * spcom_device_poll() - handle channel file poll() from user space.
 *
 * @filp: file pointer
 *
 * This allows user space to wait/check for channel connection,
 * or wait for SSR event.
 *
 * Return: event bitmask on success, set POLLERR on failure.
 */
static unsigned int spcom_device_poll(struct file *filp,
				       struct poll_table_struct *poll_table)
{
	/*
	 * when user call with timeout -1 for blocking mode,
	 * any bit must be set in response
	 */
	unsigned int ret = SPCOM_POLL_READY_FLAG;
	unsigned long mask;
	struct spcom_channel *ch;
	const char *name = file_to_filename(filp);
	bool wait = false;
	bool done = false;
	/* Event types always implicitly polled for */
	unsigned long reserved = POLLERR | POLLHUP | POLLNVAL;
	int ready = 0;

	if (strcmp(name, "unknown") == 0) {
		pr_err("name is unknown\n");
		return -EINVAL;
	}

	if (!poll_table) {
		pr_err("invalid parameters.\n");
		return -EINVAL;
	}

	ch = filp->private_data;
	mask = poll_requested_events(poll_table);

	pr_debug("== ch [%s] mask [0x%x] ==.\n", name, (int) mask);

	/* user space API has poll use "short" and not "long" */
	mask &= 0x0000FFFF;

	wait = mask & SPCOM_POLL_WAIT_FLAG;
	if (wait)
		pr_debug("ch [%s] wait for event flag is ON.\n", name);

	// mask will be used in output, clean input bits
	mask &= (unsigned long)~SPCOM_POLL_WAIT_FLAG;
	mask &= (unsigned long)~SPCOM_POLL_READY_FLAG;
	mask &= (unsigned long)~reserved;

	switch (mask) {
	case SPCOM_POLL_LINK_STATE:
		pr_debug("ch [%s] SPCOM_POLL_LINK_STATE.\n", name);
		if (wait) {
			reinit_completion(&spcom_dev->rpmsg_state_change);
			ready = wait_for_completion_interruptible(
					  &spcom_dev->rpmsg_state_change);
			pr_debug("ch [%s] poll LINK_STATE signaled.\n", name);
		}
		done = atomic_read(&spcom_dev->rpmsg_dev_count) > 0;
		break;
	case SPCOM_POLL_CH_CONNECT:
		/*
		 * ch is not expected to be NULL since user must call open()
		 * to get FD before it can call poll().
		 * open() will fail if no ch related to the char-device.
		 */
		if (ch == NULL) {
			pr_err("invalid ch pointer, file [%s].\n", name);
			return POLLERR;
		}
		pr_debug("ch [%s] SPCOM_POLL_CH_CONNECT.\n", name);
		if (wait) {
			reinit_completion(&ch->connect);
			ready = wait_for_completion_interruptible(&ch->connect);
			pr_debug("ch [%s] poll CH_CONNECT signaled.\n", name);
		}
		mutex_lock(&ch->lock);
		done = completion_done(&ch->connect);
		mutex_unlock(&ch->lock);
		break;
	default:
		pr_err("ch [%s] poll, invalid mask [0x%x].\n",
			 name, (int) mask);
		ret = POLLERR;
		break;
	}

	if (ready < 0) { /* wait was interrupted */
		pr_debug("ch [%s] poll interrupted, ret [%d].\n", name, ready);
		ret = POLLERR | SPCOM_POLL_READY_FLAG | mask;
	}
	if (done)
		ret |= mask;

	pr_debug("ch [%s] poll, mask = 0x%x, ret=0x%x.\n",
		 name, (int) mask, ret);

	return ret;
}

/* file operation supported from user space */
static const struct file_operations fops = {
	.owner = THIS_MODULE,
	.read = spcom_device_read,
	.poll = spcom_device_poll,
	.write = spcom_device_write,
	.open = spcom_device_open,
	.release = spcom_device_release,
};

/**
 * spcom_create_channel_chardev() - Create a channel char-dev node file
 * for user space interface
 */
static int spcom_create_channel_chardev(const char *name)
{
	int ret;
	struct device *dev;
	struct spcom_channel *ch;
	dev_t devt;
	struct class *cls = spcom_dev->driver_class;
	struct device *parent = spcom_dev->class_dev;
	void *priv;
	struct cdev *cdev;

	pr_debug("Add channel [%s].\n", name);

	ch = spcom_find_channel_by_name(name);
	if (ch) {
		pr_err("channel [%s] already exist.\n", name);
		return -EINVAL;
	}

	ch = spcom_find_channel_by_name(""); /* find reserved channel */
	if (!ch) {
		pr_err("no free channel.\n");
		return -ENODEV;
	}

	ret = spcom_init_channel(ch, name);
	if (ret < 0) {
		pr_err("can't init channel %d\n", ret);
		return ret;
	}

	ret = spcom_register_rpmsg_drv(ch);
	if (ret < 0) {
		pr_err("register rpmsg driver failed %d\n", ret);
		goto exit_destroy_channel;
	}

	cdev = kzalloc(sizeof(*cdev), GFP_KERNEL);
	if (!cdev) {
		ret = -ENOMEM;
		goto exit_unregister_drv;
	}

	devt = spcom_dev->device_no + atomic_read(&spcom_dev->chdev_count);
	priv = ch;
	dev = device_create(cls, parent, devt, priv, name);
	if (IS_ERR(dev)) {
		pr_err("device_create failed.\n");
		ret = -ENODEV;
		goto exit_free_cdev;
	}

	cdev_init(cdev, &fops);
	cdev->owner = THIS_MODULE;

	ret = cdev_add(cdev, devt, 1);
	if (ret < 0) {
		pr_err("cdev_add failed %d\n", ret);
		ret = -ENODEV;
		goto exit_destroy_device;
	}
	atomic_inc(&spcom_dev->chdev_count);
	mutex_lock(&ch->lock);
	ch->cdev = cdev;
	ch->dev = dev;
	mutex_unlock(&ch->lock);

	return 0;

exit_destroy_device:
	device_destroy(spcom_dev->driver_class, devt);
exit_free_cdev:
	kfree(cdev);
exit_unregister_drv:
	ret = spcom_unregister_rpmsg_drv(ch);
	if (ret != 0)
		pr_err("can't unregister rpmsg drv %d\n", ret);
exit_destroy_channel:
	// empty channel leaves free slot for next time
	mutex_lock(&ch->lock);
	memset(ch->name, 0, SPCOM_CHANNEL_NAME_SIZE);
	mutex_unlock(&ch->lock);
	return -EFAULT;
}

static int __init spcom_register_chardev(void)
{
	int ret;
	unsigned int baseminor = 0;
	unsigned int count = 1;
	void *priv = spcom_dev;

	ret = alloc_chrdev_region(&spcom_dev->device_no, baseminor, count,
				 DEVICE_NAME);
	if (ret < 0) {
		pr_err("alloc_chrdev_region failed %d\n", ret);
		return ret;
	}

	spcom_dev->driver_class = class_create(THIS_MODULE, DEVICE_NAME);
	if (IS_ERR(spcom_dev->driver_class)) {
		ret = -ENOMEM;
		pr_err("class_create failed %d\n", ret);
		goto exit_unreg_chrdev_region;
	}

	spcom_dev->class_dev = device_create(spcom_dev->driver_class, NULL,
				  spcom_dev->device_no, priv,
				  DEVICE_NAME);

	if (IS_ERR(spcom_dev->class_dev)) {
		pr_err("class_device_create failed %d\n", ret);
		ret = -ENOMEM;
		goto exit_destroy_class;
	}

	cdev_init(&spcom_dev->cdev, &fops);
	spcom_dev->cdev.owner = THIS_MODULE;

	ret = cdev_add(&spcom_dev->cdev,
		       MKDEV(MAJOR(spcom_dev->device_no), 0),
		       SPCOM_MAX_CHANNELS);
	if (ret < 0) {
		pr_err("cdev_add failed %d\n", ret);
		goto exit_destroy_device;
	}

	pr_debug("char device created.\n");

	return 0;

exit_destroy_device:
	device_destroy(spcom_dev->driver_class, spcom_dev->device_no);
exit_destroy_class:
	class_destroy(spcom_dev->driver_class);
exit_unreg_chrdev_region:
	unregister_chrdev_region(spcom_dev->device_no, 1);
	return ret;
}

static void spcom_unregister_chrdev(void)
{
	cdev_del(&spcom_dev->cdev);
	device_destroy(spcom_dev->driver_class, spcom_dev->device_no);
	class_destroy(spcom_dev->driver_class);
	unregister_chrdev_region(spcom_dev->device_no,
				 atomic_read(&spcom_dev->chdev_count));

}

static int spcom_parse_dt(struct device_node *np)
{
	int ret;
	const char *propname = "qcom,spcom-ch-names";
	int num_ch;
	int i;
	const char *name;

	num_ch = of_property_count_strings(np, propname);
	if (num_ch < 0) {
		pr_err("wrong format of predefined channels definition [%d].\n",
		       num_ch);
		return num_ch;
	}
	if (num_ch > ARRAY_SIZE(spcom_dev->predefined_ch_name)) {
		pr_err("too many predefined channels [%d].\n", num_ch);
		return -EINVAL;
	}

	pr_debug("num of predefined channels [%d].\n", num_ch);
	for (i = 0; i < num_ch; i++) {
		ret = of_property_read_string_index(np, propname, i, &name);
		if (ret) {
			pr_err("failed to read DT channel [%d] name .\n", i);
			return -EFAULT;
		}
		strlcpy(spcom_dev->predefined_ch_name[i],
			name,
			sizeof(spcom_dev->predefined_ch_name[i]));

		pr_debug("found ch [%s].\n", name);
	}

	return num_ch;
}

/*
 * the function is running on system workqueue context,
 * processes delayed (by rpmsg rx callback) packets:
 * each paket belong to its destination spcom channel ch
 */
static void spcom_signal_rx_done(struct work_struct *ignored)
{
	struct spcom_channel *ch;
	struct rx_buff_list *rx_item;
	struct spcom_msg_hdr *hdr;
	unsigned long flags;

	spin_lock_irqsave(&spcom_dev->rx_lock, flags);
	while (!list_empty(&spcom_dev->rx_list_head)) {
		/* detach last entry */
		rx_item = list_last_entry(&spcom_dev->rx_list_head,
					  struct rx_buff_list, list);
		list_del(&rx_item->list);
		spin_unlock_irqrestore(&spcom_dev->rx_lock, flags);

		if (!rx_item) {
			pr_err("empty entry in pending rx list\n");
			spin_lock_irqsave(&spcom_dev->rx_lock, flags);
			continue;
		}
		ch = rx_item->ch;
		hdr = (struct spcom_msg_hdr *)rx_item->rpmsg_rx_buf;
		mutex_lock(&ch->lock);

		if (ch->comm_role_undefined) {
			ch->comm_role_undefined = false;
			ch->is_server = true;
			ch->txn_id = hdr->txn_id;
			pr_debug("ch [%s] first packet txn_id=%d, it is server\n",
				ch->name, ch->txn_id);
		}

		if (ch->rpmsg_abort) {
			if (ch->rpmsg_rx_buf) {
				pr_debug("ch [%s] rx aborted free %d bytes\n",
					ch->name, ch->actual_rx_size);
				kfree(ch->rpmsg_rx_buf);
				ch->actual_rx_size = 0;
			}
			goto rx_aborted;
		}
		if (ch->rpmsg_rx_buf) {
			pr_err("ch [%s] previous buffer not consumed %lu bytes\n",
				ch->name, ch->actual_rx_size);
			kfree(ch->rpmsg_rx_buf);
			ch->rpmsg_rx_buf = NULL;
			ch->actual_rx_size = 0;
		} 
		if (!ch->is_server && (hdr->txn_id != ch->txn_id)) {
			pr_err("ch [%s] rx dropped txn_id %d, ch->txn_id %d\n",
				ch->name, hdr->txn_id, ch->txn_id);
			goto rx_aborted;
		}
		ch->rpmsg_rx_buf = rx_item->rpmsg_rx_buf;
		ch->actual_rx_size = rx_item->rx_buf_size;
		complete_all(&ch->rx_done);
		mutex_unlock(&ch->lock);

		kfree(rx_item);

		/* lock for the next list entry */
		spin_lock_irqsave(&spcom_dev->rx_lock, flags);
	}
	spin_unlock_irqrestore(&spcom_dev->rx_lock, flags);
	return;
rx_aborted:
	mutex_unlock(&ch->lock);
	kfree(rx_item->rpmsg_rx_buf);
	kfree(rx_item);
}

static int spcom_rpdev_cb(struct rpmsg_device *rpdev,
			  void *data, int len, void *priv, u32 src)
{
	struct spcom_channel *ch;
	static DECLARE_WORK(rpmsg_rx_consumer, spcom_signal_rx_done);
	struct rx_buff_list *rx_item;
	unsigned long flags;

	if (!rpdev || !data) {
		pr_err("rpdev or data is NULL\n");
		return -EINVAL;
	}
	pr_debug("incoming msg from %s\n", rpdev->id.name);
	ch = dev_get_drvdata(&rpdev->dev);
	if (!ch) {
		pr_err("%s: invalid ch\n", __func__);
		return -EINVAL;
	}
	if (len > SPCOM_RX_BUF_SIZE || len <= 0) {
		pr_err("got msg size %d, max allowed %d\n",
		       len, SPCOM_RX_BUF_SIZE);
		return -EINVAL;
	}

	rx_item = kzalloc(sizeof(*rx_item), GFP_ATOMIC);
	if (!rx_item)
		return -ENOMEM;

	rx_item->rpmsg_rx_buf = kzalloc(len, GFP_ATOMIC);
	if (!rx_item->rpmsg_rx_buf) {
		kfree(rx_item);
		return -ENOMEM;
	}
	memcpy(rx_item->rpmsg_rx_buf, data, len);
	rx_item->rx_buf_size = len;
	rx_item->ch = ch;

	spin_lock_irqsave(&spcom_dev->rx_lock, flags);
	list_add(&rx_item->list, &spcom_dev->rx_list_head);
	spin_unlock_irqrestore(&spcom_dev->rx_lock, flags);
	pr_debug("signaling rx item for %s, received %d bytes\n",
	       rpdev->id.name, len);

	schedule_work(&rpmsg_rx_consumer);
	return 0;
}

static int spcom_rpdev_probe(struct rpmsg_device *rpdev)
{
	const char *name;
	struct spcom_channel *ch;

	if (!rpdev) {
		pr_err("rpdev is NULL\n");
		return -EINVAL;
	}
	name = rpdev->id.name;
	pr_debug("new channel %s rpmsg_device arrived\n", name);
	ch = spcom_find_channel_by_name(name);
	if (!ch) {
		pr_err("channel %s not found\n", name);
		return -ENODEV;
	}
	mutex_lock(&ch->lock);
	ch->rpdev = rpdev;
	ch->rpmsg_abort = false;
	ch->txn_id = INITIAL_TXN_ID;
	complete_all(&ch->connect);
	mutex_unlock(&ch->lock);

	dev_set_drvdata(&rpdev->dev, ch);

	/* used to evaluate underlying transport link up/down */
	atomic_inc(&spcom_dev->rpmsg_dev_count);
	if (atomic_read(&spcom_dev->rpmsg_dev_count) == 1)
		complete_all(&spcom_dev->rpmsg_state_change);

	return 0;
}

static void spcom_rpdev_remove(struct rpmsg_device *rpdev)
{
	struct spcom_channel *ch;
	int i;

	if (!rpdev) {
		pr_err("rpdev is NULL\n");
		return;
	}

	dev_info(&rpdev->dev, "rpmsg device %s removed\n", rpdev->id.name);
	ch = dev_get_drvdata(&rpdev->dev);
	if (!ch) {
		pr_err("channel %s not found\n", rpdev->id.name);
		return;
	}

	mutex_lock(&ch->lock);
	// unlock all ion buffers of sp_kernel channel
	if (strcmp(ch->name, "sp_kernel") == 0) {
		for (i = 0; i < ARRAY_SIZE(ch->dmabuf_handle_table); i++) {
			if (ch->dmabuf_handle_table[i] != NULL) {
				pr_debug("unlocked ion buf #%d fd [%d].\n",
					i, ch->dmabuf_fd_table[i]);
				dma_buf_put(ch->dmabuf_handle_table[i]);
				ch->dmabuf_handle_table[i] = NULL;
				ch->dmabuf_fd_table[i] = -1;
			}
		}
	}

	ch->rpdev = NULL;
	ch->rpmsg_abort = true;
	ch->txn_id = 0;
	complete_all(&ch->rx_done);
	mutex_unlock(&ch->lock);

	/* used to evaluate underlying transport link up/down */
	if (atomic_dec_and_test(&spcom_dev->rpmsg_dev_count))
		complete_all(&spcom_dev->rpmsg_state_change);

}

/* register rpmsg driver to match with channel ch_name */
static int spcom_register_rpmsg_drv(struct spcom_channel *ch)
{
	struct rpmsg_driver *rpdrv;
	struct rpmsg_device_id *match;
	char *drv_name;
	int ret;

	if (ch->rpdrv) {
		pr_err("ch:%s, rpmsg driver %s already registered\n", ch->name,
		       ch->rpdrv->id_table->name);
		return -ENODEV;
	}

	rpdrv = kzalloc(sizeof(*rpdrv), GFP_KERNEL);
	if (!rpdrv)
		return -ENOMEM;

	/* zalloc array of two to NULL terminate the match list */
	match = kzalloc(2 * sizeof(*match), GFP_KERNEL);
	if (!match) {
		kfree(rpdrv);
		return -ENOMEM;
	}
	snprintf(match->name, RPMSG_NAME_SIZE, "%s", ch->name);

	drv_name = kasprintf(GFP_KERNEL, "%s_%s", "spcom_rpmsg_drv", ch->name);
	if (!drv_name) {
		pr_err("can't allocate drv_name for %s\n", ch->name);
		kfree(rpdrv);
		kfree(match);
		return -ENOMEM;
	}

	rpdrv->probe = spcom_rpdev_probe;
	rpdrv->remove = spcom_rpdev_remove;
	rpdrv->callback = spcom_rpdev_cb;
	rpdrv->id_table = match;
	rpdrv->drv.name = drv_name;
	ret = register_rpmsg_driver(rpdrv);
	if (ret) {
		pr_err("can't register rpmsg_driver for %s\n", ch->name);
		kfree(rpdrv);
		kfree(match);
		kfree(drv_name);
		return ret;
	}
	mutex_lock(&ch->lock);
	ch->rpdrv = rpdrv;
	ch->rpmsg_abort = false;
	mutex_unlock(&ch->lock);

	return 0;
}

static int spcom_unregister_rpmsg_drv(struct spcom_channel *ch)
{
	if (!ch->rpdrv)
		return -ENODEV;
	unregister_rpmsg_driver(ch->rpdrv);

	mutex_lock(&ch->lock);
	kfree(ch->rpdrv->drv.name);
	kfree((void *)ch->rpdrv->id_table);
	kfree(ch->rpdrv);
	ch->rpdrv = NULL;
	ch->rpmsg_abort = true; /* will unblock spcom_rx() */
	mutex_unlock(&ch->lock);
	return 0;
}

static int spcom_probe(struct platform_device *pdev)
{
	int ret;
	struct spcom_device *dev = NULL;
	struct device_node *np;

	if (!pdev) {
		pr_err("invalid pdev.\n");
		return -ENODEV;
	}

	np = pdev->dev.of_node;
	if (!np) {
		pr_err("invalid DT node.\n");
		return -EINVAL;
	}

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (dev == NULL)
		return -ENOMEM;

	spcom_dev = dev;
	spcom_dev->pdev = pdev;
	/* start counting exposed channel char devices from 1 */
	atomic_set(&spcom_dev->chdev_count, 1);
	init_completion(&spcom_dev->rpmsg_state_change);
	atomic_set(&spcom_dev->rpmsg_dev_count, 0);

	INIT_LIST_HEAD(&spcom_dev->rx_list_head);
	spin_lock_init(&spcom_dev->rx_lock);

	ret = spcom_register_chardev();
	if (ret) {
		pr_err("create character device failed.\n");
		goto fail_while_chardev_reg;
	}

	ret = spcom_parse_dt(np);
	if (ret < 0)
		goto fail_reg_chardev;

	ret = spcom_create_predefined_channels_chardev();
	if (ret < 0) {
		pr_err("create character device failed.\n");
		goto fail_reg_chardev;
	}
	pr_debug("Driver Initialization ok.\n");
	return 0;

fail_reg_chardev:
	pr_err("failed to init driver\n");
	spcom_unregister_chrdev();
fail_while_chardev_reg:
	kfree(dev);
	spcom_dev = NULL;

	return -ENODEV;
}

static const struct of_device_id spcom_match_table[] = {
	{ .compatible = "qcom,spcom", },
	{ },
};

static struct platform_driver spcom_driver = {
	.probe = spcom_probe,
	.driver = {
		.name = DEVICE_NAME,
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(spcom_match_table),
	},
};

static int __init spcom_init(void)
{
	int ret;

	pr_info("spcom driver version 2.1 23-April-2018.\n");

	ret = platform_driver_register(&spcom_driver);
	if (ret)
		pr_err("spcom_driver register failed %d\n", ret);

	return ret;
}
module_init(spcom_init);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Secure Processor Communication");
