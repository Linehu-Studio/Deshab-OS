// SPDX-License-Identifier: GPL-2.0
/*
 * UTSM hypercall driver — Linux guest side.
 *
 * Provides:
 *   1. VMCALL hypercall interface to communicate with the UTSM monitor kernel
 *   2. /dev/utsm character device for userspace IPC message passing
 *   3. Shared memory ring buffer mapping (queried via UTSM_HCALL_SHM_INFO)
 *
 * The driver is platform-agnostic: it probes for UTSM by issuing a PING
 * hypercall at init. If UTSM is not present (e.g. running on bare metal
 * or a different hypervisor), the driver exits gracefully.
 *
 * Build: configured via CONFIG_UTSM_HCALL (built-in or module).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/mm.h>

/* Shared IPC protocol header (included via build.sh -I flag) */
#include <ipc_proto.h>

#define UTSM_DEV_NAME "utsm"

/* ===== Hypercall ABI =====
 *
 * Linux calls VMCALL with:
 *   RAX = UTSM_HCALL_MAGIC | op
 *   RDI = arg0, RSI = arg1, RDX = arg2
 *
 * UTSM handles the VMCALL VM-Exit and returns:
 *   RAX = return value
 *
 * Since we run in the guest, the VMCALL instruction triggers a VM-Exit
 * to UTSM. The inline assembly must use the same register conventions.
 */

static inline long utsm_hcall(u64 op, u64 a0, u64 a1, u64 a2)
{
	long ret;
	register u64 rax asm("rax") = UTSM_HCALL_MAGIC | op;
	register u64 rdi asm("rdi") = a0;
	register u64 rsi asm("rsi") = a1;
	register u64 rdx asm("rdx") = a2;

	asm volatile("vmcall"
		     : "+r"(rax)
		     : "r"(rdi), "r"(rsi), "r"(rdx)
		     : "memory");

	return (long)rax;
}

static inline long utsm_hcall_ping(void)
{
	return utsm_hcall(UTSM_HCALL_PING, 0, 0, 0);
}

static inline long utsm_hcall_hello(void)
{
	return utsm_hcall(UTSM_HCALL_HELLO, 0, 0, 0);
}

static inline long utsm_hcall_shm_info(u64 *gpa_out, u64 *size_out)
{
	u64 gpa = 0, size = 0;
	long ret;

	/* Pass stack addresses (GVA=GPA due to identity mapping in early boot).
	 * UTSM translates GPA→HPA via EPT walk and writes the values there. */
	ret = utsm_hcall(UTSM_HCALL_SHM_INFO, (u64)&gpa, (u64)&size, 0);
	if (ret == 0) {
		*gpa_out = gpa;
		*size_out = size;
	}
	return ret;
}

static inline long utsm_hcall_console_write(const char *buf, u64 len)
{
	return utsm_hcall(UTSM_HCALL_CONSOLE_WRITE, (u64)buf, len, 0);
}

/* ===== Shared memory state ===== */

static struct utsm_ipc_shm *g_shm;        /* kernel virtual address of SHM */
static u64 g_shm_gpa;                      /* guest physical address of SHM */
static u64 g_shm_size;                     /* size of SHM region */
static int g_utsm_present;                 /* 1 = UTSM detected */
static int g_shm_mapped;

/* ===== /dev/utsm character device ===== */

static ssize_t utsm_dev_write(struct file *f, const char __user *buf,
			      size_t len, loff_t *off)
{
	struct utsm_ipc_msg msg;
	size_t copy_len;
	ssize_t ret;

	if (!g_shm_mapped)
		return -ENODEV;

	if (len == 0)
		return 0;

	/* Cap data to inline message size */
	copy_len = min_t(size_t, len, UTSM_IPC_MSG_DATA_SIZE);

	memset(&msg, 0, sizeof(msg));
	msg.type = UTSM_MSG_DATA;
	msg.data_len = (u32)copy_len;

	if (copy_from_user(msg.data, buf, copy_len)) {
		pr_err("[utsm] copy_from_user failed\n");
		return -EFAULT;
	}

	/* Enqueue into linux_to_utsm ring (Linux produces, UTSM consumes) */
	if (utsm_ipc_ring_push(&g_shm->linux_to_utsm, &msg) != 0) {
		pr_warn("[utsm] linux_to_utsm ring full\n");
		return -EAGAIN;
	}

	ret = (ssize_t)copy_len;

	/* Also relay the data to the UTSM serial console via hypercall.
	 * This gives immediate visibility into Linux userspace output. */
	if (copy_len > 0) {
		char kbuf[UTSM_IPC_MSG_DATA_SIZE + 1];
		memcpy(kbuf, msg.data, copy_len);
		kbuf[copy_len] = '\0';
		utsm_hcall_console_write(kbuf, copy_len);
	}

	return ret;
}

static ssize_t utsm_dev_read(struct file *f, char __user *buf,
			     size_t len, loff_t *off)
{
	struct utsm_ipc_msg msg;

	if (!g_shm_mapped)
		return -ENODEV;

	/* Dequeue from utsm_to_linux ring (UTSM produces, Linux consumes) */
	if (utsm_ipc_ring_pop(&g_shm->utsm_to_linux, &msg) != 0) {
		/* No message available */
		return -EAGAIN;  /* Non-blocking; userspace can poll */
	}

	if (msg.data_len == 0)
		return 0;

	if (len < msg.data_len)
		return -EINVAL;

	if (copy_to_user(buf, msg.data, msg.data_len))
		return -EFAULT;

	return (ssize_t)msg.data_len;
}

static const struct file_operations utsm_fops = {
	.owner  = THIS_MODULE,
	.write  = utsm_dev_write,
	.read   = utsm_dev_read,
};

static struct miscdevice utsm_miscdev = {
	.minor  = MISC_DYNAMIC_MINOR,
	.name   = UTSM_DEV_NAME,
	.fops   = &utsm_fops,
	.mode   = 0666,
};

/* ===== Early console redirect (optional) =====
 *
 * Redirect kernel printk output to UTSM serial console via hypercall.
 * This is useful for debugging when the guest's own serial is not set up.
 * Currently disabled; can be enabled via CONFIG_UTSM_EARLY_CONSOLE.
 */

#ifdef CONFIG_UTSM_EARLY_CONSOLE
static void utsm_console_write(struct console *con, const char *s, unsigned int n)
{
	if (g_utsm_present) {
		char buf[256];
		unsigned int chunk;

		while (n > 0) {
			chunk = min_t(unsigned int, n, sizeof(buf) - 1);
			memcpy(buf, s, chunk);
			buf[chunk] = '\0';
			utsm_hcall_console_write(buf, chunk);
			s += chunk;
			n -= chunk;
		}
	}
}

static struct console utsm_console = {
	.name   = "utsmcon",
	.write  = utsm_console_write,
	.flags  = CON_PRINTBUFFER | CON_ENABLED,
	.index  = -1,
};
#endif

/* ===== Initialization ===== */

/* Forward declaration: used by init before /dev/utsm is registered */
static inline int ipc_shm_send_to_utsm(u32 type, const void *data, u32 len);

static int __init utsm_hcall_init(void)
{
	long ping_ret;
	u64 shm_gpa = 0, shm_size = 0;
	int ret;

	pr_info("[utsm] probing for UTSM monitor...\n");

	/* 1. Ping UTSM to verify it's present */
	ping_ret = utsm_hcall_ping();
	if (ping_ret != 0x504F4E47) {  /* "PONG" */
		pr_info("[utsm] UTSM not detected (ping returned 0x%lx)\n", ping_ret);
		pr_info("[utsm] Driver loaded in passive mode (no /dev/utsm)\n");
		return 0;  /* Not an error: we might be running on bare metal */
	}

	g_utsm_present = 1;
	pr_info("[utsm] UTSM monitor detected (PONG=0x%lx)\n", ping_ret);

	/* 2. Say hello */
	utsm_hcall_hello();

	/* 3. Query shared memory info */
	ret = utsm_hcall_shm_info(&shm_gpa, &shm_size);
	if (ret != 0) {
		pr_err("[utsm] SHM_INFO hypercall failed: %ld\n", ret);
		goto err_no_shm;
	}

	pr_info("[utsm] shared memory: gpa=0x%llx size=%llu KB\n",
		shm_gpa, shm_size / 1024);

	if (shm_gpa == 0 || shm_size == 0) {
		pr_err("[utsm] invalid SHM info\n");
		goto err_no_shm;
	}

	/* 4. Map the shared memory region.
	 * The GPA is a physical address from Linux's perspective.
	 * Use ioremap_cache for cacheable memory (the SHM region is
	 * configured as WB in EPT). */
	g_shm = (struct utsm_ipc_shm *)ioremap_cache(shm_gpa, shm_size);
	if (!g_shm) {
		pr_err("[utsm] failed to ioremap SHM at 0x%llx\n", shm_gpa);
		goto err_ioremap;
	}

	g_shm_gpa = shm_gpa;
	g_shm_size = shm_size;
	g_shm_mapped = 1;

	/* 5. Verify SHM header */
	if (g_shm->header.magic != UTSM_IPC_MAGIC) {
		pr_err("[utsm] SHM magic mismatch: 0x%llx\n", g_shm->header.magic);
		goto err_bad_magic;
	}

	pr_info("[utsm] SHM header: magic=OK version=%u utsm_ready=%u\n",
		g_shm->header.version, g_shm->header.utsm_ready);

	/* 6. Set linux_ready flag */
	g_shm->header.linux_ready = 1;
	mb();  /* Ensure visible to UTSM */

	/* 7. Register /dev/utsm */
	ret = misc_register(&utsm_miscdev);
	if (ret) {
		pr_err("[utsm] misc_register failed: %d\n", ret);
		goto err_misc;
	}

#ifdef CONFIG_UTSM_EARLY_CONSOLE
	register_console(&utsm_console);
#endif

	pr_info("[utsm] /dev/%s registered (IPC ready)\n", UTSM_DEV_NAME);

	/* 8. Send a HELLO message via the ring buffer */
	{
		const char *hello_msg = "Linux booted OK";
		ipc_shm_send_to_utsm(UTSM_MSG_DATA, hello_msg, 15);
	}

	return 0;

err_misc:
	g_shm->header.linux_ready = 0;
err_bad_magic:
	iounmap(g_shm);
	g_shm = NULL;
	g_shm_mapped = 0;
err_ioremap:
err_no_shm:
	g_utsm_present = 0;
	return -ENODEV;
}

/* Helper to send a message (used by init before /dev/utsm is available) */
static inline int ipc_shm_send_to_utsm(u32 type, const void *data, u32 len)
{
	struct utsm_ipc_msg msg;

	if (!g_shm_mapped || !g_shm)
		return -1;

	if (len > UTSM_IPC_MSG_DATA_SIZE)
		len = UTSM_IPC_MSG_DATA_SIZE;

	memset(&msg, 0, sizeof(msg));
	msg.type = type;
	msg.data_len = len;
	if (data && len > 0)
		memcpy(msg.data, data, len);

	return utsm_ipc_ring_push(&g_shm->linux_to_utsm, &msg);
}

static void __exit utsm_hcall_exit(void)
{
	if (g_shm_mapped) {
		/* Clear linux_ready flag */
		g_shm->header.linux_ready = 0;
		mb();

		/* Send shutdown notification */
		ipc_shm_send_to_utsm(UTSM_MSG_SHUTDOWN, NULL, 0);

		misc_deregister(&utsm_miscdev);

#ifdef CONFIG_UTSM_EARLY_CONSOLE
		unregister_console(&utsm_console);
#endif

		iounmap(g_shm);
		g_shm = NULL;
		g_shm_mapped = 0;
	}

	pr_info("[utsm] driver unloaded\n");
}

module_init(utsm_hcall_init);
module_exit(utsm_hcall_exit);

MODULE_AUTHOR("Deshab Project");
MODULE_DESCRIPTION("UTSM dual-kernel hypercall and IPC driver");
MODULE_LICENSE("GPL");
