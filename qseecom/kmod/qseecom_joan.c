// SPDX-License-Identifier: GPL-2.0-only
/*
 * Android /dev/qseecom driver for Qualcomm QSEE 4.x (msm8998 / LG V30) on
 * mainline Linux 7.3.
 *
 * Bridges the legacy Qualcomm Android libQSEEComAPI.so / qseecomd UAPI onto
 * mainline qcom_scm (qcom_scm_qseecom_raw_call) and Linux dma-bufs (backed by
 * ion_legacy + cma_heap).
 */

#define pr_fmt(fmt) "qseecom: " fmt

#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/elf.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/iosys-map.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

/* ---- UAPI definitions matching LG msm8998 uapi/linux/qseecom.h ---- */

#define MAX_ION_FD		4
#define MAX_APP_NAME_SIZE	64
#define QSEECOM_MAX_SG_ENTRY	512

struct qseecom_register_listener_req {
	u32 listener_id;
	s32 ifd_data_fd;
	void __user *virt_sb_base;
	u32 sb_size;
};

struct qseecom_send_cmd_req {
	void __user *cmd_req_buf;
	u32 cmd_req_len;
	void __user *resp_buf;
	u32 resp_len;
};

struct qseecom_ion_fd_info {
	s32 fd;
	u32 cmd_buf_offset;
};

struct qseecom_send_modfd_cmd_req {
	void __user *cmd_req_buf;
	u32 cmd_req_len;
	void __user *resp_buf;
	u32 resp_len;
	struct qseecom_ion_fd_info ifd_data[MAX_ION_FD];
};

struct qseecom_load_img_req {
	u32 mdt_len;
	u32 img_len;
	s32 ifd_data_fd;
	char img_name[MAX_APP_NAME_SIZE];
	u32 app_arch;
	u32 app_id;
};

struct qseecom_set_sb_mem_param_req {
	s32 ifd_data_fd;
	void __user *virt_sb_base;
	u32 sb_len;
};

struct qseecom_qseos_version_req {
	u32 qseos_version;
};

struct qseecom_qseos_app_load_query {
	char app_name[MAX_APP_NAME_SIZE];
	u32 app_id;
	u32 app_arch;
};

struct qseecom_send_modfd_listener_resp {
	void __user *resp_buf_ptr;
	u32 resp_len;
	struct qseecom_ion_fd_info ifd_data[MAX_ION_FD];
};

struct qseecom_sg_entry {
	u32 phys_addr;
	u32 len;
};

struct qseecom_sg_entry_64bit {
	u64 phys_addr;
	u32 len;
} __packed;

#define QSEECOM_IOC_MAGIC	0x97

#define QSEECOM_IOCTL_REGISTER_LISTENER_REQ \
	_IOWR(QSEECOM_IOC_MAGIC, 1, struct qseecom_register_listener_req)
#define QSEECOM_IOCTL_UNREGISTER_LISTENER_REQ \
	_IO(QSEECOM_IOC_MAGIC, 2)
#define QSEECOM_IOCTL_SEND_CMD_REQ \
	_IOWR(QSEECOM_IOC_MAGIC, 3, struct qseecom_send_cmd_req)
#define QSEECOM_IOCTL_SEND_MODFD_CMD_REQ \
	_IOWR(QSEECOM_IOC_MAGIC, 4, struct qseecom_send_modfd_cmd_req)
#define QSEECOM_IOCTL_RECEIVE_REQ \
	_IO(QSEECOM_IOC_MAGIC, 5)
#define QSEECOM_IOCTL_SEND_RESP_REQ \
	_IO(QSEECOM_IOC_MAGIC, 6)
#define QSEECOM_IOCTL_LOAD_APP_REQ \
	_IOWR(QSEECOM_IOC_MAGIC, 7, struct qseecom_load_img_req)
#define QSEECOM_IOCTL_SET_MEM_PARAM_REQ \
	_IOWR(QSEECOM_IOC_MAGIC, 8, struct qseecom_set_sb_mem_param_req)
#define QSEECOM_IOCTL_UNLOAD_APP_REQ \
	_IO(QSEECOM_IOC_MAGIC, 9)
#define QSEECOM_IOCTL_GET_QSEOS_VERSION_REQ \
	_IOWR(QSEECOM_IOC_MAGIC, 10, struct qseecom_qseos_version_req)
#define QSEECOM_IOCTL_PERF_ENABLE_REQ \
	_IO(QSEECOM_IOC_MAGIC, 11)
#define QSEECOM_IOCTL_PERF_DISABLE_REQ \
	_IO(QSEECOM_IOC_MAGIC, 12)
#define QSEECOM_IOCTL_LOAD_EXTERNAL_ELF_REQ \
	_IOWR(QSEECOM_IOC_MAGIC, 13, struct qseecom_load_img_req)
#define QSEECOM_IOCTL_UNLOAD_EXTERNAL_ELF_REQ \
	_IO(QSEECOM_IOC_MAGIC, 14)
#define QSEECOM_IOCTL_APP_LOADED_QUERY_REQ \
	_IOWR(QSEECOM_IOC_MAGIC, 15, struct qseecom_qseos_app_load_query)
#define QSEECOM_IOCTL_SEND_MODFD_RESP \
	_IOWR(QSEECOM_IOC_MAGIC, 21, struct qseecom_send_modfd_listener_resp)
#define QSEECOM_IOCTL_SET_BUS_SCALING_REQ \
	_IOWR(QSEECOM_IOC_MAGIC, 23, int)
#define QSEECOM_IOCTL_SEND_MODFD_CMD_64_REQ \
	_IOWR(QSEECOM_IOC_MAGIC, 35, struct qseecom_send_modfd_cmd_req)
#define QSEECOM_IOCTL_SEND_MODFD_RESP_64 \
	_IOWR(QSEECOM_IOC_MAGIC, 36, struct qseecom_send_modfd_listener_resp)

/* ---- QSEE SMC IDs & arginfo encoding (QSEOS 4.x / msm8998) ---- */

#define TZ_OWNER_SIP			2
#define TZ_OWNER_TZ_APPS		48
#define TZ_OWNER_QSEE_OS		50

#define TZ_SVC_APP_ID_PLACEHOLDER	0
#define TZ_SVC_APP_MGR			1
#define TZ_SVC_LISTENER			2
#define TZ_SVC_EXTERNAL			3
#define TZ_SVC_INFO			6

#define QSEOS_RESULT_SUCCESS		0ULL
#define QSEOS_RESULT_INCOMPLETE		1ULL
#define QSEOS_RESULT_BLOCKED_ON_LISTENER 2ULL
#define QSEOS_RESULT_FAILURE		0xFFFFFFFFULL
#define QSEOS_APP_ID_RESP		0xEE01ULL

#define SCM_VAL				0U
#define SCM_RO				1U
#define SCM_RW				2U

#define SCM_ARGS1(a)			(1U | ((a) << 4))
#define SCM_ARGS2(a, b)			(2U | ((a) << 4) | ((b) << 6))
#define SCM_ARGS3(a, b, c)		(3U | ((a) << 4) | ((b) << 6) | ((c) << 8))
#define SCM_ARGS5(a, b, c, d, e)	(5U | ((a) << 4) | ((b) << 6) | \
					 ((c) << 8) | ((d) << 10) | ((e) << 12))

/* ---- Internal state ---- */

struct qsee_dmabuf {
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	struct iosys_map map;
	bool mapped_vmap;
	phys_addr_t pa;
	size_t len;
};

struct qsee_app_entry {
	struct list_head list;
	u32 app_id;
	u32 app_arch;
	u32 ref_cnt;
	bool preloaded;
	char app_name[MAX_APP_NAME_SIZE];
};

struct qsee_listener_entry {
	struct list_head list;
	u32 listener_id;
	struct qsee_dmabuf sb;
	void __user *virt_sb_base;
	u32 sb_len;
	wait_queue_head_t rcv_req_wq;
	wait_queue_head_t send_resp_wq;
	int rcv_req_flag;
	int send_resp_flag;
	bool abort;
	/* Pending modfd buffers for listener responses (cleaned after SMC) */
	struct qsee_dmabuf modfd_bufs[MAX_ION_FD];
	u32 modfd_offsets[MAX_ION_FD];
	u32 modfd_lens[MAX_ION_FD];
	bool modfd_active[MAX_ION_FD];
};

enum qsee_client_type {
	QSEE_GENERIC = 0,
	QSEE_CLIENT_APP,
	QSEE_LISTENER_SERVICE,
	QSEE_UNAVAILABLE_CLIENT_APP,
};

struct qsee_dev_handle {
	enum qsee_client_type type;
	u32 app_id;
	u32 app_arch;
	char app_name[MAX_APP_NAME_SIZE];
	struct qsee_dmabuf sb;
	void __user *virt_sb_base;
	u32 sb_len;
	u32 listener_id;
	bool released;
	struct mutex lock;
};

static struct platform_device *qsee_pdev;
static struct device *qsee_dma_dev;
static u32 qsee_version;
static bool qsee_cmnlib32_loaded;

static DEFINE_MUTEX(qsee_app_call_lock);
static DEFINE_MUTEX(qsee_app_list_lock);
static DEFINE_MUTEX(qsee_listener_lock);

static LIST_HEAD(qsee_app_list);
static LIST_HEAD(qsee_listener_list);

/* ---- DMA-BUF pinning helpers ---- */

static int qsee_pin_dmabuf(s32 fd, bool need_contig, bool need_vmap,
			   struct qsee_dmabuf *out)
{
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	int ret;

	memset(out, 0, sizeof(*out));
	if (fd < 0)
		return -EINVAL;

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	attach = dma_buf_attach(dmabuf, qsee_dma_dev);
	if (IS_ERR(attach)) {
		ret = PTR_ERR(attach);
		goto err_put;
	}

	sgt = dma_buf_map_attachment_unlocked(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto err_detach;
	}

	if (sgt->nents == 0) {
		ret = -EINVAL;
		goto err_unmap;
	}

	if (need_contig && sgt->nents != 1) {
		pr_err("buffer fd=%d is non-contiguous (nents=%u)\n",
		       fd, sgt->nents);
		ret = -EINVAL;
		goto err_unmap;
	}

	if (need_vmap) {
		ret = dma_buf_vmap_unlocked(dmabuf, &out->map);
		if (ret) {
			pr_err("dma_buf_vmap failed for fd=%d: %d\n", fd, ret);
			goto err_unmap;
		}
		out->mapped_vmap = true;
	}

	out->dmabuf = dmabuf;
	out->attach = attach;
	out->sgt = sgt;
	out->pa = sg_phys(sgt->sgl);
	out->len = dmabuf->size;
	return 0;

err_unmap:
	dma_buf_unmap_attachment_unlocked(attach, sgt, DMA_BIDIRECTIONAL);
err_detach:
	dma_buf_detach(dmabuf, attach);
err_put:
	dma_buf_put(dmabuf);
	memset(out, 0, sizeof(*out));
	return ret;
}

static void qsee_unpin_dmabuf(struct qsee_dmabuf *buf)
{
	if (!buf->dmabuf)
		return;
	if (buf->mapped_vmap)
		dma_buf_vunmap_unlocked(buf->dmabuf, &buf->map);
	if (buf->attach && buf->sgt)
		dma_buf_unmap_attachment_unlocked(buf->attach, buf->sgt,
						  DMA_BIDIRECTIONAL);
	if (buf->attach)
		dma_buf_detach(buf->dmabuf, buf->attach);
	dma_buf_put(buf->dmabuf);
	memset(buf, 0, sizeof(*buf));
}

static void qsee_sync_for_tz(struct qsee_dmabuf *buf)
{
	if (buf && buf->dmabuf)
		dma_buf_end_cpu_access(buf->dmabuf, DMA_BIDIRECTIONAL);
}

static void qsee_sync_for_cpu(struct qsee_dmabuf *buf)
{
	if (buf && buf->dmabuf)
		dma_buf_begin_cpu_access(buf->dmabuf, DMA_BIDIRECTIONAL);
}

/* ---- Listener helpers ---- */

static struct qsee_listener_entry *qsee_find_listener_locked(u32 id)
{
	struct qsee_listener_entry *e;

	list_for_each_entry(e, &qsee_listener_list, list) {
		if (e->listener_id == id)
			return e;
	}
	return NULL;
}

static void qsee_cleanup_listener_modfd(struct qsee_listener_entry *lstnr)
{
	int i;

	for (i = 0; i < MAX_ION_FD; i++) {
		if (!lstnr->modfd_active[i])
			continue;
		if (lstnr->sb.mapped_vmap && !lstnr->sb.map.is_iomem &&
		    lstnr->modfd_offsets[i] + lstnr->modfd_lens[i] <= lstnr->sb.len) {
			memset((u8 *)lstnr->sb.map.vaddr + lstnr->modfd_offsets[i],
			       0, lstnr->modfd_lens[i]);
		}
		qsee_sync_for_cpu(&lstnr->modfd_bufs[i]);
		qsee_unpin_dmabuf(&lstnr->modfd_bufs[i]);
		lstnr->modfd_active[i] = false;
	}
}

/*
 * Drive the QSEE incomplete / blocked-on-listener handshake loop until TZ
 * completes the transaction or fails. Caller must hold qsee_app_call_lock.
 */
static int qsee_process_incomplete(u64 res[3])
{
	int ret;

	while (res[0] == QSEOS_RESULT_INCOMPLETE ||
	       res[0] == QSEOS_RESULT_BLOCKED_ON_LISTENER) {
		u32 lstnr_id = (u32)res[2];
		struct qsee_listener_entry *lstnr;
		u64 args[10] = {};
		u32 status = 0;

		mutex_lock(&qsee_listener_lock);
		lstnr = qsee_find_listener_locked(lstnr_id);
		if (!lstnr || lstnr->abort) {
			mutex_unlock(&qsee_listener_lock);
			pr_err("listener %u not registered or aborted\n",
			       lstnr_id);
			status = (u32)-1;
			args[0] = lstnr_id;
			args[1] = status;
			ret = qcom_scm_qseecom_raw_call(
				TZ_OWNER_QSEE_OS, TZ_SVC_LISTENER, 0x03,
				SCM_ARGS2(SCM_VAL, SCM_VAL), args, res);
			return ret ? ret : -EINVAL;
		}
		qsee_sync_for_cpu(&lstnr->sb);
		lstnr->send_resp_flag = 0;
		lstnr->rcv_req_flag = 1;
		wake_up_interruptible(&lstnr->rcv_req_wq);
		mutex_unlock(&qsee_listener_lock);

		/* Wait for userspace listener to finish servicing request */
		ret = wait_event_interruptible(lstnr->send_resp_wq,
					       lstnr->send_resp_flag ||
					       lstnr->abort);
		if (ret || lstnr->abort) {
			status = (u32)-1;
			pr_warn("listener %u wait interrupted/aborted (%d)\n",
				lstnr_id, ret);
		}

		mutex_lock(&qsee_listener_lock);
		lstnr->send_resp_flag = 0;
		qsee_sync_for_tz(&lstnr->sb);
		mutex_unlock(&qsee_listener_lock);

		args[0] = lstnr_id;
		args[1] = status;
		ret = qcom_scm_qseecom_raw_call(
			TZ_OWNER_QSEE_OS, TZ_SVC_LISTENER, 0x03,
			SCM_ARGS2(SCM_VAL, SCM_VAL), args, res);

		mutex_lock(&qsee_listener_lock);
		qsee_cleanup_listener_modfd(lstnr);
		mutex_unlock(&qsee_listener_lock);

		if (ret) {
			pr_err("listener %u response SCM failed: %d\n",
			       lstnr_id, ret);
			return ret;
		}
	}

	return 0;
}

/* ---- QSEE SCM wrappers ---- */

static int qsee_lookup_app(const char *app_name, u32 *app_id_out)
{
	size_t name_len = strnlen(app_name, MAX_APP_NAME_SIZE - 1);
	void *buf;
	dma_addr_t pa;
	u64 args[10] = {};
	u64 res[3] = {};
	int ret;

	*app_id_out = 0;
	if (name_len == 0)
		return -EINVAL;

	buf = dma_alloc_coherent(qsee_dma_dev, PAGE_SIZE, &pa, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	memcpy(buf, app_name, name_len);
	((char *)buf)[name_len] = '\0';

	args[0] = pa;
	args[1] = name_len;
	ret = qcom_scm_qseecom_raw_call(TZ_OWNER_QSEE_OS, TZ_SVC_APP_MGR, 0x03,
					SCM_ARGS2(SCM_RW, SCM_VAL), args, res);
	dma_free_coherent(qsee_dma_dev, PAGE_SIZE, buf, pa);

	if (ret)
		return ret;

	if (res[0] == QSEOS_RESULT_SUCCESS && res[1] == QSEOS_APP_ID_RESP &&
	    (s32)res[2] > 0)
		*app_id_out = (u32)res[2];

	return 0;
}

static int __maybe_unused qsee_load_commonlib(const char *name)
{
	const struct firmware *mdt_fw = NULL;
	char fname[64];
	struct elf32_hdr *ehdr;
	struct elf32_phdr *phdrs;
	size_t total_len = 0, mdt_len = 0;
	void *img = NULL;
	dma_addr_t pa = 0;
	u64 args[10] = {};
	u64 res[3] = {};
	int i, ret;

	snprintf(fname, sizeof(fname), "%s.mdt", name);
	ret = request_firmware(&mdt_fw, fname, qsee_dma_dev);
	if (ret) {
		pr_warn("cmnlib %s not found via request_firmware (%d)\n",
			fname, ret);
		return ret;
	}
	if (mdt_fw->size < sizeof(*ehdr)) {
		ret = -EINVAL;
		goto out_mdt;
	}
	mdt_len = mdt_fw->size;
	ehdr = (struct elf32_hdr *)mdt_fw->data;
	if (ehdr->e_ident[EI_CLASS] != ELFCLASS32) {
		ret = 0;
		goto out_mdt;
	}

	phdrs = (struct elf32_phdr *)(mdt_fw->data + ehdr->e_phoff);
	total_len = mdt_len;
	for (i = 0; i < ehdr->e_phnum; i++) {
		if (phdrs[i].p_filesz && phdrs[i].p_offset + phdrs[i].p_filesz > total_len)
			total_len = phdrs[i].p_offset + phdrs[i].p_filesz;
	}
	total_len = PAGE_ALIGN(total_len);

	img = dma_alloc_coherent(qsee_dma_dev, total_len, &pa, GFP_KERNEL);
	if (!img) {
		ret = -ENOMEM;
		goto out_mdt;
	}
	memcpy(img, mdt_fw->data, mdt_len);

	for (i = 0; i < ehdr->e_phnum; i++) {
		const struct firmware *seg = NULL;

		if (!phdrs[i].p_filesz || i == 0 || i == 1)
			continue;
		snprintf(fname, sizeof(fname), "%s.b%02d", name, i);
		ret = request_firmware(&seg, fname, qsee_dma_dev);
		if (ret)
			goto out_free;
		if (phdrs[i].p_offset + seg->size <= total_len)
			memcpy((u8 *)img + phdrs[i].p_offset, seg->data, seg->size);
		release_firmware(seg);
	}

	args[0] = mdt_len;
	args[1] = total_len;
	args[2] = pa;
	mutex_lock(&qsee_app_call_lock);
	ret = qcom_scm_qseecom_raw_call(TZ_OWNER_QSEE_OS, TZ_SVC_APP_MGR, 0x07,
					SCM_ARGS3(SCM_VAL, SCM_VAL, SCM_VAL),
					args, res);
	if (!ret && (res[0] == QSEOS_RESULT_INCOMPLETE ||
		     res[0] == QSEOS_RESULT_BLOCKED_ON_LISTENER))
		ret = qsee_process_incomplete(res);
	mutex_unlock(&qsee_app_call_lock);

	if (ret || res[0] != QSEOS_RESULT_SUCCESS) {
		pr_err("load %s failed: ret=%d res=0x%llx\n", name, ret, res[0]);
		ret = ret ? ret : -EIO;
	} else {
		pr_info("loaded %s into QSEE\n", name);
	}

out_free:
	dma_free_coherent(qsee_dma_dev, total_len, img, pa);
out_mdt:
	release_firmware(mdt_fw);
	return ret;
}

static struct qsee_app_entry *qsee_find_app_locked(u32 app_id)
{
	struct qsee_app_entry *e;

	list_for_each_entry(e, &qsee_app_list, list) {
		if (e->app_id == app_id)
			return e;
	}
	return NULL;
}

/* ---- ModFD buffer patching ---- */

static int qsee_patch_modfd(struct qsee_dmabuf *sb, u8 *base_kaddr,
			    size_t msg_offset, size_t msg_len,
			    const struct qseecom_ion_fd_info ifd[MAX_ION_FD],
			    bool is_64bit,
			    struct qsee_dmabuf pinned[MAX_ION_FD],
			    u32 patched_off[MAX_ION_FD],
			    u32 patched_len[MAX_ION_FD],
			    bool active[MAX_ION_FD])
{
	int i, ret;

	for (i = 0; i < MAX_ION_FD; i++) {
		struct scatterlist *sg;
		u32 off = ifd[i].cmd_buf_offset;
		size_t write_len;
		u8 *dst;

		active[i] = false;
		if (ifd[i].fd <= 0)
			continue;

		ret = qsee_pin_dmabuf(ifd[i].fd, false, false, &pinned[i]);
		if (ret) {
			pr_err("modfd[%d] pin fd=%d failed: %d\n",
			       i, ifd[i].fd, ret);
			goto err_unwind;
		}
		qsee_sync_for_tz(&pinned[i]);

		if (pinned[i].sgt->nents == 1) {
			write_len = is_64bit ? sizeof(u64) : sizeof(u32);
			if ((size_t)off + write_len > msg_len ||
			    msg_offset + off + write_len > sb->len) {
				ret = -EINVAL;
				goto err_unwind_cur;
			}
			dst = base_kaddr + msg_offset + off;
			if (is_64bit) {
				u64 pa64 = (u64)sg_phys(pinned[i].sgt->sgl);
				memcpy(dst, &pa64, sizeof(pa64));
			} else {
				u32 pa32 = (u32)sg_phys(pinned[i].sgt->sgl);
				memcpy(dst, &pa32, sizeof(pa32));
			}
		} else {
			unsigned int n = pinned[i].sgt->nents;
			unsigned int j;

			if (n > QSEECOM_MAX_SG_ENTRY) {
				ret = -EINVAL;
				goto err_unwind_cur;
			}
			write_len = n * (is_64bit ? sizeof(struct qseecom_sg_entry_64bit)
						  : sizeof(struct qseecom_sg_entry));
			if ((size_t)off + write_len > msg_len ||
			    msg_offset + off + write_len > sb->len) {
				ret = -EINVAL;
				goto err_unwind_cur;
			}
			dst = base_kaddr + msg_offset + off;
			for_each_sg(pinned[i].sgt->sgl, sg, n, j) {
				if (is_64bit) {
					struct qseecom_sg_entry_64bit e = {
						.phys_addr = sg_phys(sg),
						.len = sg->length,
					};
					memcpy(dst, &e, sizeof(e));
					dst += sizeof(e);
				} else {
					struct qseecom_sg_entry e = {
						.phys_addr = (u32)sg_phys(sg),
						.len = sg->length,
					};
					memcpy(dst, &e, sizeof(e));
					dst += sizeof(e);
				}
			}
		}

		patched_off[i] = (u32)(msg_offset + off);
		patched_len[i] = (u32)write_len;
		active[i] = true;
		continue;

err_unwind_cur:
		qsee_unpin_dmabuf(&pinned[i]);
err_unwind:
		while (--i >= 0) {
			if (!active[i])
				continue;
			memset(base_kaddr + patched_off[i], 0, patched_len[i]);
			qsee_unpin_dmabuf(&pinned[i]);
			active[i] = false;
		}
		return ret;
	}
	return 0;
}

static void qsee_unpatch_modfd(u8 *base_kaddr,
			       struct qsee_dmabuf pinned[MAX_ION_FD],
			       const u32 patched_off[MAX_ION_FD],
			       const u32 patched_len[MAX_ION_FD],
			       bool active[MAX_ION_FD])
{
	int i;

	for (i = 0; i < MAX_ION_FD; i++) {
		if (!active[i])
			continue;
		if (base_kaddr)
			memset(base_kaddr + patched_off[i], 0, patched_len[i]);
		qsee_sync_for_cpu(&pinned[i]);
		qsee_unpin_dmabuf(&pinned[i]);
		active[i] = false;
	}
}

/* ---- IOCTL handlers ---- */

static int qsee_ioctl_get_version(void __user *argp)
{
	struct qseecom_qseos_version_req req;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	req.qseos_version = qsee_version;
	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;
	return 0;
}

static int qsee_ioctl_query_app_loaded(struct qsee_dev_handle *data,
				       void __user *argp)
{
	struct qseecom_qseos_app_load_query q;
	struct qsee_app_entry *entry;
	u32 app_id = 0;
	int ret;

	if (copy_from_user(&q, argp, sizeof(q)))
		return -EFAULT;
	q.app_name[MAX_APP_NAME_SIZE - 1] = '\0';

	ret = qsee_lookup_app(q.app_name, &app_id);
	if (ret)
		return ret;

	if (!app_id)
		return 0; /* not loaded */

	mutex_lock(&qsee_app_list_lock);
	entry = qsee_find_app_locked(app_id);
	if (entry) {
		entry->ref_cnt++;
		q.app_arch = entry->app_arch;
	} else {
		entry = kzalloc(sizeof(*entry), GFP_KERNEL);
		if (!entry) {
			mutex_unlock(&qsee_app_list_lock);
			return -ENOMEM;
		}
		entry->app_id = app_id;
		entry->ref_cnt = 1;
		entry->preloaded = true;
		entry->app_arch = ELFCLASS64;
		strscpy(entry->app_name, q.app_name, sizeof(entry->app_name));
		list_add_tail(&entry->list, &qsee_app_list);
		q.app_arch = entry->app_arch;
	}
	mutex_unlock(&qsee_app_list_lock);

	data->type = QSEE_CLIENT_APP;
	data->app_id = app_id;
	data->app_arch = q.app_arch;
	strscpy(data->app_name, q.app_name, sizeof(data->app_name));
	q.app_id = app_id;

	if (copy_to_user(argp, &q, sizeof(q)))
		return -EFAULT;

	pr_info("query '%s' -> loaded (app_id=%u)\n", q.app_name, app_id);
	return -EEXIST; /* CAF ABI: -EEXIST means app is loaded */
}

static int qsee_ioctl_set_mem_param(struct qsee_dev_handle *data,
				    void __user *argp)
{
	struct qseecom_set_sb_mem_param_req req;
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	if (!req.virt_sb_base || !req.sb_len || req.ifd_data_fd < 0)
		return -EINVAL;

	qsee_unpin_dmabuf(&data->sb);
	ret = qsee_pin_dmabuf(req.ifd_data_fd, true, true, &data->sb);
	if (ret)
		return ret;
	if (req.sb_len > data->sb.len) {
		qsee_unpin_dmabuf(&data->sb);
		return -EINVAL;
	}
	data->virt_sb_base = req.virt_sb_base;
	data->sb_len = req.sb_len;
	return 0;
}

static int qsee_ioctl_load_app(struct qsee_dev_handle *data, void __user *argp)
{
	struct qseecom_load_img_req req;
	struct qsee_app_entry *entry;
	struct qsee_dmabuf img_buf;
	u32 app_id = 0;
	u64 args[10] = {};
	u64 res[3] = {};
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	req.img_name[MAX_APP_NAME_SIZE - 1] = '\0';

	if (req.app_arch == ELFCLASS32 && !qsee_cmnlib32_loaded) {
		qsee_cmnlib32_loaded = true; /* Preloaded by UEFI on joan */
	}

	ret = qsee_lookup_app(req.img_name, &app_id);
	if (ret)
		return ret;

	if (app_id) {
		mutex_lock(&qsee_app_list_lock);
		entry = qsee_find_app_locked(app_id);
		if (entry) {
			entry->ref_cnt++;
		} else {
			entry = kzalloc(sizeof(*entry), GFP_KERNEL);
			if (entry) {
				entry->app_id = app_id;
				entry->app_arch = req.app_arch;
				entry->ref_cnt = 1;
				entry->preloaded = true;
				strscpy(entry->app_name, req.img_name,
					sizeof(entry->app_name));
				list_add_tail(&entry->list, &qsee_app_list);
			}
		}
		mutex_unlock(&qsee_app_list_lock);
	} else {
		ret = qsee_pin_dmabuf(req.ifd_data_fd, true, false, &img_buf);
		if (ret)
			return ret;

		if (req.mdt_len > img_buf.len || req.img_len > img_buf.len) {
			qsee_unpin_dmabuf(&img_buf);
			return -EINVAL;
		}

		qsee_sync_for_tz(&img_buf);

		args[0] = req.mdt_len;
		args[1] = req.img_len;
		args[2] = img_buf.pa;

		pr_info("loading TA '%s' (mdt=%u img=%u pa=0x%pa arch=%u)\n",
			req.img_name, req.mdt_len, req.img_len,
			&img_buf.pa, req.app_arch);

		mutex_lock(&qsee_app_call_lock);
		ret = qcom_scm_qseecom_raw_call(
			TZ_OWNER_QSEE_OS, TZ_SVC_APP_MGR, 0x01,
			SCM_ARGS3(SCM_VAL, SCM_VAL, SCM_VAL), args, res);
		if (!ret && (res[0] == QSEOS_RESULT_INCOMPLETE ||
			     res[0] == QSEOS_RESULT_BLOCKED_ON_LISTENER))
			ret = qsee_process_incomplete(res);
		mutex_unlock(&qsee_app_call_lock);

		qsee_unpin_dmabuf(&img_buf);

		if (ret) {
			pr_err("load TA '%s' SCM error %d\n", req.img_name, ret);
			return ret;
		}
		if (res[0] != QSEOS_RESULT_SUCCESS ||
		    res[1] != QSEOS_APP_ID_RESP || (s32)res[2] <= 0) {
			pr_err("load TA '%s' QSEE failed: res=0x%llx type=0x%llx data=0x%llx\n",
			       req.img_name, res[0], res[1], res[2]);
			return -EFAULT;
		}
		app_id = (u32)res[2];

		entry = kzalloc(sizeof(*entry), GFP_KERNEL);
		if (!entry)
			return -ENOMEM;
		entry->app_id = app_id;
		entry->app_arch = req.app_arch;
		entry->ref_cnt = 1;
		strscpy(entry->app_name, req.img_name, sizeof(entry->app_name));
		mutex_lock(&qsee_app_list_lock);
		list_add_tail(&entry->list, &qsee_app_list);
		mutex_unlock(&qsee_app_list_lock);

		pr_info("TA '%s' started (app_id=%u)\n", req.img_name, app_id);
	}

	data->type = QSEE_CLIENT_APP;
	data->app_id = app_id;
	data->app_arch = req.app_arch;
	strscpy(data->app_name, req.img_name, sizeof(data->app_name));
	req.app_id = app_id;

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;
	return 0;
}

static int qsee_unload_app(struct qsee_dev_handle *data)
{
	struct qsee_app_entry *entry;
	bool do_shutdown = false;
	u32 app_id = data->app_id;
	u64 args[10] = {};
	u64 res[3] = {};
	int ret = 0;

	if (!app_id)
		return 0;

	mutex_lock(&qsee_app_list_lock);
	entry = qsee_find_app_locked(app_id);
	if (entry) {
		if (entry->ref_cnt > 1) {
			entry->ref_cnt--;
		} else if (entry->preloaded) {
			entry->ref_cnt = 0;
		} else {
			list_del(&entry->list);
			kfree(entry);
			do_shutdown = true;
		}
	}
	mutex_unlock(&qsee_app_list_lock);

	data->app_id = 0;

	if (do_shutdown) {
		args[0] = app_id;
		mutex_lock(&qsee_app_call_lock);
		ret = qcom_scm_qseecom_raw_call(
			TZ_OWNER_QSEE_OS, TZ_SVC_APP_MGR, 0x02,
			SCM_ARGS1(SCM_VAL), args, res);
		if (!ret && (res[0] == QSEOS_RESULT_INCOMPLETE ||
			     res[0] == QSEOS_RESULT_BLOCKED_ON_LISTENER))
			ret = qsee_process_incomplete(res);
		mutex_unlock(&qsee_app_call_lock);
		pr_info("TA '%s' (app_id=%u) shutdown (ret=%d res=0x%llx)\n",
			data->app_name, app_id, ret, res[0]);
	}
	return ret;
}

static int qsee_send_cmd_common(struct qsee_dev_handle *data,
				void __user *req_uptr, u32 req_len,
				void __user *rsp_uptr, u32 rsp_len,
				const struct qseecom_ion_fd_info *ifd,
				bool is_64bit)
{
	uintptr_t sb_base = (uintptr_t)data->virt_sb_base;
	uintptr_t req_addr = (uintptr_t)req_uptr;
	uintptr_t rsp_addr = (uintptr_t)rsp_uptr;
	size_t req_off, rsp_off;
	struct qsee_dmabuf modfd_bufs[MAX_ION_FD] = {};
	u32 modfd_off[MAX_ION_FD] = {};
	u32 modfd_len[MAX_ION_FD] = {};
	bool modfd_act[MAX_ION_FD] = {};
	u8 *kaddr = NULL;
	u64 args[10] = {};
	u64 res[3] = {};
	int ret;

	if (!data->app_id || !data->sb.dmabuf)
		return -EINVAL;
	if (!req_len || !rsp_len)
		return -EINVAL;
	if (req_addr < sb_base || rsp_addr < sb_base)
		return -EINVAL;

	req_off = req_addr - sb_base;
	rsp_off = rsp_addr - sb_base;
	if (req_off > data->sb_len || req_len > data->sb_len - req_off ||
	    rsp_off > data->sb_len || rsp_len > data->sb_len - rsp_off)
		return -EINVAL;

	if (data->sb.mapped_vmap && !data->sb.map.is_iomem)
		kaddr = data->sb.map.vaddr;

	if (ifd) {
		if (!kaddr)
			return -EIO;
		qsee_sync_for_cpu(&data->sb);
		ret = qsee_patch_modfd(&data->sb, kaddr, req_off, req_len,
				       ifd, is_64bit, modfd_bufs,
				       modfd_off, modfd_len, modfd_act);
		if (ret)
			return ret;
	}

	qsee_sync_for_tz(&data->sb);

	args[0] = data->app_id;
	args[1] = data->sb.pa + req_off;
	args[2] = req_len;
	args[3] = data->sb.pa + rsp_off;
	args[4] = rsp_len;

	mutex_lock(&qsee_app_call_lock);
	ret = qcom_scm_qseecom_raw_call(
		TZ_OWNER_TZ_APPS, TZ_SVC_APP_ID_PLACEHOLDER, 0x01,
		SCM_ARGS5(SCM_VAL, SCM_RW, SCM_VAL, SCM_RW, SCM_VAL),
		args, res);
	if (!ret && (res[0] == QSEOS_RESULT_INCOMPLETE ||
		     res[0] == QSEOS_RESULT_BLOCKED_ON_LISTENER))
		ret = qsee_process_incomplete(res);
	mutex_unlock(&qsee_app_call_lock);

	qsee_sync_for_cpu(&data->sb);

	if (ifd)
		qsee_unpatch_modfd(kaddr, modfd_bufs, modfd_off, modfd_len,
				   modfd_act);

	if (ret)
		return ret;
	if (res[0] != QSEOS_RESULT_SUCCESS) {
		pr_err("send_cmd app_id=%u (%s) failed: res=0x%llx\n",
		       data->app_id, data->app_name, res[0]);
		return -EIO;
	}
	return 0;
}

static int qsee_ioctl_send_cmd(struct qsee_dev_handle *data, void __user *argp)
{
	struct qseecom_send_cmd_req req;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	return qsee_send_cmd_common(data, req.cmd_req_buf, req.cmd_req_len,
				    req.resp_buf, req.resp_len, NULL, false);
}

static int qsee_ioctl_send_modfd_cmd(struct qsee_dev_handle *data,
				     void __user *argp, bool is_64bit)
{
	struct qseecom_send_modfd_cmd_req req;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	return qsee_send_cmd_common(data, req.cmd_req_buf, req.cmd_req_len,
				    req.resp_buf, req.resp_len,
				    req.ifd_data, is_64bit);
}

static int qsee_ioctl_register_listener(struct qsee_dev_handle *data,
					void __user *argp)
{
	struct qseecom_register_listener_req req;
	struct qsee_listener_entry *lstnr;
	u64 args[10] = {};
	u64 res[3] = {};
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	if (!req.sb_size || req.ifd_data_fd < 0)
		return -EINVAL;

	mutex_lock(&qsee_listener_lock);
	if (qsee_find_listener_locked(req.listener_id)) {
		mutex_unlock(&qsee_listener_lock);
		return -EBUSY;
	}
	mutex_unlock(&qsee_listener_lock);

	lstnr = kzalloc(sizeof(*lstnr), GFP_KERNEL);
	if (!lstnr)
		return -ENOMEM;

	ret = qsee_pin_dmabuf(req.ifd_data_fd, true, true, &lstnr->sb);
	if (ret) {
		kfree(lstnr);
		return ret;
	}
	if (req.sb_size > lstnr->sb.len) {
		qsee_unpin_dmabuf(&lstnr->sb);
		kfree(lstnr);
		return -EINVAL;
	}

	lstnr->listener_id = req.listener_id;
	lstnr->virt_sb_base = req.virt_sb_base;
	lstnr->sb_len = req.sb_size;
	init_waitqueue_head(&lstnr->rcv_req_wq);
	init_waitqueue_head(&lstnr->send_resp_wq);

	args[0] = req.listener_id;
	args[1] = lstnr->sb.pa;
	args[2] = req.sb_size;

	/* Try SMCINVOKE listener register (0x06), fallback to legacy (0x01) */
	ret = qcom_scm_qseecom_raw_call(
		TZ_OWNER_QSEE_OS, TZ_SVC_LISTENER, 0x06,
		SCM_ARGS3(SCM_VAL, SCM_RW, SCM_VAL), args, res);
	if (ret || res[0] != QSEOS_RESULT_SUCCESS) {
		ret = qcom_scm_qseecom_raw_call(
			TZ_OWNER_QSEE_OS, TZ_SVC_LISTENER, 0x01,
			SCM_ARGS3(SCM_VAL, SCM_RW, SCM_VAL), args, res);
	}
	if (ret || res[0] != QSEOS_RESULT_SUCCESS) {
		pr_err("register listener %u failed: ret=%d res=0x%llx\n",
		       req.listener_id, ret, res[0]);
		qsee_unpin_dmabuf(&lstnr->sb);
		kfree(lstnr);
		return ret ? ret : -EINVAL;
	}

	mutex_lock(&qsee_listener_lock);
	list_add_tail(&lstnr->list, &qsee_listener_list);
	mutex_unlock(&qsee_listener_lock);

	data->type = QSEE_LISTENER_SERVICE;
	data->listener_id = req.listener_id;
	pr_info("registered listener 0x%x (pa=0x%pa len=%u)\n",
		req.listener_id, &lstnr->sb.pa, req.sb_size);
	return 0;
}

static int qsee_unregister_listener(struct qsee_dev_handle *data)
{
	struct qsee_listener_entry *lstnr;
	u64 args[10] = {};
	u64 res[3] = {};
	u32 id = data->listener_id;

	if (data->type != QSEE_LISTENER_SERVICE || !id)
		return 0;

	mutex_lock(&qsee_listener_lock);
	lstnr = qsee_find_listener_locked(id);
	if (lstnr) {
		lstnr->abort = true;
		wake_up_interruptible(&lstnr->rcv_req_wq);
		wake_up_interruptible(&lstnr->send_resp_wq);
		list_del(&lstnr->list);
	}
	mutex_unlock(&qsee_listener_lock);

	data->listener_id = 0;

	if (lstnr) {
		args[0] = id;
		qcom_scm_qseecom_raw_call(TZ_OWNER_QSEE_OS, TZ_SVC_LISTENER,
					  0x02, SCM_ARGS1(SCM_VAL), args, res);
		qsee_cleanup_listener_modfd(lstnr);
		qsee_unpin_dmabuf(&lstnr->sb);
		kfree(lstnr);
		pr_info("unregistered listener 0x%x\n", id);
	}
	return 0;
}

static int qsee_ioctl_receive_req(struct qsee_dev_handle *data)
{
	struct qsee_listener_entry *lstnr;
	int ret;

	mutex_lock(&qsee_listener_lock);
	lstnr = qsee_find_listener_locked(data->listener_id);
	mutex_unlock(&qsee_listener_lock);
	if (!lstnr)
		return -ENODATA;

	ret = wait_event_interruptible(lstnr->rcv_req_wq,
				       lstnr->rcv_req_flag || lstnr->abort);
	if (ret)
		return -ERESTARTSYS;
	if (lstnr->abort)
		return -ENODEV;

	mutex_lock(&qsee_listener_lock);
	lstnr->rcv_req_flag = 0;
	mutex_unlock(&qsee_listener_lock);
	return 0;
}

static int qsee_ioctl_send_resp(struct qsee_dev_handle *data)
{
	struct qsee_listener_entry *lstnr;

	mutex_lock(&qsee_listener_lock);
	lstnr = qsee_find_listener_locked(data->listener_id);
	if (lstnr) {
		lstnr->send_resp_flag = 1;
		wake_up_interruptible(&lstnr->send_resp_wq);
	}
	mutex_unlock(&qsee_listener_lock);
	return lstnr ? 0 : -EINVAL;
}

static int qsee_ioctl_send_modfd_resp(struct qsee_dev_handle *data,
				      void __user *argp, bool is_64bit)
{
	struct qseecom_send_modfd_listener_resp req;
	struct qsee_listener_entry *lstnr;
	uintptr_t sb_base, resp_addr;
	size_t resp_off;
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	mutex_lock(&qsee_listener_lock);
	lstnr = qsee_find_listener_locked(data->listener_id);
	if (!lstnr || !lstnr->sb.mapped_vmap || lstnr->sb.map.is_iomem) {
		mutex_unlock(&qsee_listener_lock);
		return -EINVAL;
	}

	sb_base = (uintptr_t)lstnr->virt_sb_base;
	resp_addr = (uintptr_t)req.resp_buf_ptr;
	if (resp_addr < sb_base || req.resp_len == 0) {
		mutex_unlock(&qsee_listener_lock);
		return -EINVAL;
	}
	resp_off = resp_addr - sb_base;
	if (resp_off > lstnr->sb_len || req.resp_len > lstnr->sb_len - resp_off) {
		mutex_unlock(&qsee_listener_lock);
		return -EINVAL;
	}

	qsee_sync_for_cpu(&lstnr->sb);
	ret = qsee_patch_modfd(&lstnr->sb, lstnr->sb.map.vaddr,
			       resp_off, req.resp_len, req.ifd_data, is_64bit,
			       lstnr->modfd_bufs, lstnr->modfd_offsets,
			       lstnr->modfd_lens, lstnr->modfd_active);
	if (!ret) {
		lstnr->send_resp_flag = 1;
		wake_up_interruptible(&lstnr->send_resp_wq);
	}
	mutex_unlock(&qsee_listener_lock);
	return ret;
}

static int qsee_ioctl_load_external_elf(void __user *argp)
{
	struct qseecom_load_img_req req;
	struct qsee_dmabuf img_buf;
	u64 args[10] = {};
	u64 res[3] = {};
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	ret = qsee_pin_dmabuf(req.ifd_data_fd, true, false, &img_buf);
	if (ret)
		return ret;
	qsee_sync_for_tz(&img_buf);

	args[0] = req.mdt_len;
	args[1] = req.img_len;
	args[2] = img_buf.pa;

	mutex_lock(&qsee_app_call_lock);
	ret = qcom_scm_qseecom_raw_call(TZ_OWNER_QSEE_OS, TZ_SVC_EXTERNAL, 0x01,
					SCM_ARGS3(SCM_VAL, SCM_VAL, SCM_VAL),
					args, res);
	if (!ret && (res[0] == QSEOS_RESULT_INCOMPLETE ||
		     res[0] == QSEOS_RESULT_BLOCKED_ON_LISTENER))
		ret = qsee_process_incomplete(res);
	mutex_unlock(&qsee_app_call_lock);

	qsee_unpin_dmabuf(&img_buf);
	return (ret || res[0] != QSEOS_RESULT_SUCCESS) ? -EFAULT : 0;
}

static int qsee_ioctl_unload_external_elf(void)
{
	u64 res[3] = {};
	int ret;

	mutex_lock(&qsee_app_call_lock);
	ret = qcom_scm_qseecom_raw_call(TZ_OWNER_QSEE_OS, TZ_SVC_EXTERNAL, 0x02,
					0, NULL, res);
	mutex_unlock(&qsee_app_call_lock);
	return ret;
}

/* ---- File operations ---- */

static int qsee_open(struct inode *inode, struct file *file)
{
	struct qsee_dev_handle *data;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	mutex_init(&data->lock);
	file->private_data = data;
	return 0;
}

static int qsee_release(struct inode *inode, struct file *file)
{
	struct qsee_dev_handle *data = file->private_data;

	if (!data)
		return 0;

	if (data->type == QSEE_LISTENER_SERVICE)
		qsee_unregister_listener(data);
	else if (data->type == QSEE_CLIENT_APP)
		qsee_unload_app(data);

	qsee_unpin_dmabuf(&data->sb);
	kfree(data);
	return 0;
}

static long qsee_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct qsee_dev_handle *data = file->private_data;
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case QSEECOM_IOCTL_GET_QSEOS_VERSION_REQ:
		return qsee_ioctl_get_version(argp);
	case QSEECOM_IOCTL_APP_LOADED_QUERY_REQ:
		return qsee_ioctl_query_app_loaded(data, argp);
	case QSEECOM_IOCTL_SET_MEM_PARAM_REQ:
		return qsee_ioctl_set_mem_param(data, argp);
	case QSEECOM_IOCTL_LOAD_APP_REQ:
		return qsee_ioctl_load_app(data, argp);
	case QSEECOM_IOCTL_UNLOAD_APP_REQ:
		return qsee_unload_app(data);
	case QSEECOM_IOCTL_SEND_CMD_REQ:
		return qsee_ioctl_send_cmd(data, argp);
	case QSEECOM_IOCTL_SEND_MODFD_CMD_REQ:
		return qsee_ioctl_send_modfd_cmd(data, argp, false);
	case QSEECOM_IOCTL_SEND_MODFD_CMD_64_REQ:
		return qsee_ioctl_send_modfd_cmd(data, argp, true);
	case QSEECOM_IOCTL_REGISTER_LISTENER_REQ:
		return qsee_ioctl_register_listener(data, argp);
	case QSEECOM_IOCTL_UNREGISTER_LISTENER_REQ:
		return qsee_unregister_listener(data);
	case QSEECOM_IOCTL_RECEIVE_REQ:
		return qsee_ioctl_receive_req(data);
	case QSEECOM_IOCTL_SEND_RESP_REQ:
		return qsee_ioctl_send_resp(data);
	case QSEECOM_IOCTL_SEND_MODFD_RESP:
		return qsee_ioctl_send_modfd_resp(data, argp, false);
	case QSEECOM_IOCTL_SEND_MODFD_RESP_64:
		return qsee_ioctl_send_modfd_resp(data, argp, true);
	case QSEECOM_IOCTL_LOAD_EXTERNAL_ELF_REQ:
		return qsee_ioctl_load_external_elf(argp);
	case QSEECOM_IOCTL_UNLOAD_EXTERNAL_ELF_REQ:
		return qsee_ioctl_unload_external_elf();
	case QSEECOM_IOCTL_SET_BUS_SCALING_REQ:
	case QSEECOM_IOCTL_PERF_ENABLE_REQ:
	case QSEECOM_IOCTL_PERF_DISABLE_REQ:
		return 0;
	default:
		pr_warn_ratelimited("unsupported ioctl 0x%x (nr=%u)\n",
				    cmd, _IOC_NR(cmd));
		return -ENOTTY;
	}
}

static const struct file_operations qsee_fops = {
	.owner		= THIS_MODULE,
	.open		= qsee_open,
	.release	= qsee_release,
	.unlocked_ioctl	= qsee_ioctl,
	.compat_ioctl	= qsee_ioctl,
};

static struct miscdevice qsee_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "qseecom",
	.fops	= &qsee_fops,
	.mode	= 0666,
};

static int __init qseecom_joan_init(void)
{
	u64 args[10] = { 10 };
	u64 res[3] = {};
	int ret;

	ret = qcom_scm_qseecom_raw_call(TZ_OWNER_SIP, TZ_SVC_INFO, 0x03,
					SCM_ARGS1(SCM_VAL), args, res);
	if (ret) {
		pr_err("failed to query QSEE version: %d\n", ret);
		return ret;
	}
	qsee_version = (u32)res[0];

	qsee_pdev = platform_device_register_simple("qseecom_dma", -1, NULL, 0);
	if (IS_ERR(qsee_pdev))
		return PTR_ERR(qsee_pdev);

	qsee_dma_dev = &qsee_pdev->dev;
	ret = dma_coerce_mask_and_coherent(qsee_dma_dev, DMA_BIT_MASK(32));
	if (ret) {
		platform_device_unregister(qsee_pdev);
		return ret;
	}

	ret = misc_register(&qsee_misc);
	if (ret) {
		platform_device_unregister(qsee_pdev);
		return ret;
	}

	pr_info("/dev/qseecom ready (QSEE version 0x%x)\n", qsee_version);
	return 0;
}

static void __exit qseecom_joan_exit(void)
{
	misc_deregister(&qsee_misc);
	platform_device_unregister(qsee_pdev);
}

module_init(qseecom_joan_init);
module_exit(qseecom_joan_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Android /dev/qseecom driver over mainline qcom_scm for LG V30");
MODULE_IMPORT_NS("DMA_BUF");
