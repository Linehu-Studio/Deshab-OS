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
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/poll.h>

/* Shared IPC protocol header (included via build.sh -I flag).
 * We provide our own hypercall wrappers, so skip the inline ones. */
#define UTSM_NO_INLINE_HCALL
#include <ipc_proto.h>

#define UTSM_DEV_NAME "utsm"

/* ===== Ioctl interface for exec daemon =====
 *
 * The exec daemon (userspace) uses these ioctls to:
 *   1. RECV_MSG: poll utsm_to_linux ring for EXEC_REQUEST (non-blocking)
 *   2. SEND_MSG: push STDOUT/STDERR/EXIT to linux_to_utsm ring
 *   3. PARK: execute HLT to trigger VM-Exit → UTSM parks guest,
 *            returning control to the host. vmresume causes the
 *            ioctl to return, at which point the daemon re-checks
 *            the ring for new requests.
 *
 * This explicit-park model avoids the need for timer emulation or
 * IRQ injection: the daemon HLTs only when it has nothing to do,
 * and UTSM vmresumes only when there is work. */

#define UTSM_IOCTL_RECV_MSG  _IOWR('U', 1, struct utsm_ioctl_msg)
#define UTSM_IOCTL_SEND_MSG  _IOWR('U', 2, struct utsm_ioctl_msg)
#define UTSM_IOCTL_PARK      _IO('U', 3)
#define UTSM_IOCTL_GET_READY _IOR('U', 4, int)
#define UTSM_IOCTL_WRITE_POOL _IOW('U', 5, struct utsm_ioctl_pool)
#define UTSM_IOCTL_READ_POOL  _IOWR('U', 6, struct utsm_ioctl_pool)
#define UTSM_IOCTL_HOST_READ  _IOWR('U', 7, struct utsm_ioctl_hostfile)
#define UTSM_IOCTL_HOST_WRITE _IOW('U', 8, struct utsm_ioctl_hostfile)

/* Ioctl message wrapper: carries type + data in a single call */
struct utsm_ioctl_msg {
    u32 type;          /* message type (in/out) */
    u32 data_len;      /* actual bytes in data (in/out) */
    u32 reserved;
    u32 buf_size;      /* capacity of data buffer (in) */
    u8  data[UTSM_IPC_MSG_DATA_SIZE]; /* payload */
};

/* Pool transfer descriptor for WRITE_POOL / READ_POOL ioctls.
 * Used by the exec daemon to stage large file-transfer payloads into the
 * shared payload_pool (bulk data channel for UTSM_MSG_FILE_RESPONSE). */
struct utsm_ioctl_pool {
    u32 offset;        /* byte offset into payload_pool */
    u32 len;           /* bytes to transfer */
    u64 user_buf;      /* userspace buffer pointer */
};

/* Host-file transfer descriptor for HOST_READ / HOST_WRITE ioctls.
 * Gives Linux userspace access to files on the UTSM-side FAT32 system disk
 * (root directory, 8.3-convertible names, whole-file semantics, max 256KB)
 * via the UTSM_HCALL_FILE_READ / UTSM_HCALL_FILE_WRITE hypercalls.
 * payload_pool at offset 0 is the bulk data buffer in both directions. */
struct utsm_ioctl_hostfile {
    u64 offset;        /* in: file offset (HOST_READ; must be 0 for HOST_WRITE) */
    u32 len;           /* in: requested length; out: bytes transferred */
    u32 reserved;
    u64 total;         /* out: total file size (HOST_READ) */
    u64 user_buf;      /* userspace buffer pointer */
    char name[64];     /* UTSM FAT32 root file name (NUL-terminated) */
};

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

/* UTSM FAT32 file read: stages file[offset..offset+len) into payload_pool.
 * Returns positive packed value (low 20 bits = staged bytes, >> 20 = total
 * file size) or a negative UTSM_HCALL_* error. The name pointer follows the
 * same GVA==GPA identity-mapping convention as the other hypercalls. */
static inline long utsm_hcall_file_read(const char *name, u64 offset, u64 len)
{
	return utsm_hcall(UTSM_HCALL_FILE_READ, (u64)name, offset, len);
}

/* UTSM FAT32 file write: writes len bytes staged at payload_pool[0] as the
 * whole new content of the named root file (create or replace).
 * Returns bytes written or a negative UTSM_HCALL_* error. */
static inline long utsm_hcall_file_write(const char *name, u64 len)
{
	return utsm_hcall(UTSM_HCALL_FILE_WRITE, (u64)name, 0, len);
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

/* ===== Ioctl handlers for exec daemon ===== */

static long utsm_dev_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    if (!g_shm_mapped)
        return -ENODEV;

    switch (cmd) {
    case UTSM_IOCTL_RECV_MSG: {
        /* Non-blocking dequeue from utsm_to_linux ring */
        struct utsm_ioctl_msg umsg;
        struct utsm_ipc_msg msg;
        int ret;

        if (copy_from_user(&umsg, (void __user *)arg, sizeof(umsg)))
            return -EFAULT;

        ret = utsm_ipc_ring_pop(&g_shm->utsm_to_linux, &msg);
        if (ret != 0)
            return -EAGAIN;  /* ring empty */

        umsg.type = msg.type;
        umsg.data_len = msg.data_len;
        if (umsg.buf_size < msg.data_len)
            return -EINVAL;
        if (msg.data_len > 0) {
            if (copy_to_user(umsg.data, msg.data, msg.data_len))
                return -EFAULT;
        }
        if (copy_to_user((void __user *)arg, &umsg, sizeof(umsg)))
            return -EFAULT;
        return 0;
    }

    case UTSM_IOCTL_SEND_MSG: {
        /* Enqueue to linux_to_utsm ring */
        struct utsm_ioctl_msg umsg;
        struct utsm_ipc_msg msg;

        if (copy_from_user(&umsg, (void __user *)arg, sizeof(umsg)))
            return -EFAULT;

        memset(&msg, 0, sizeof(msg));
        msg.type = umsg.type;
        msg.data_len = min_t(u32, umsg.data_len, UTSM_IPC_MSG_DATA_SIZE);
        if (msg.data_len > 0) {
            if (copy_from_user(msg.data, umsg.data, msg.data_len))
                return -EFAULT;
        }

        if (utsm_ipc_ring_push(&g_shm->linux_to_utsm, &msg) != 0) {
            pr_warn("[utsm] linux_to_utsm ring full\n");
            return -EAGAIN;
        }
        return 0;
    }

    case UTSM_IOCTL_PARK: {
        /* Execute HLT to trigger VM-Exit → UTSM parks guest.
         * Interrupts must be enabled so that on vmresume the guest
         * can continue (HLT with interrupts disabled would hang).
         *
         * Flow:
         *   1. daemon calls ioctl(PARK) → driver executes HLT
         *   2. HLT → VM-Exit (CPU_BASED_HLT_EXITING) → UTSM handle_hlt
         *   3. UTSM advances RIP past HLT, exits to host (park)
         *   4. UTSM writes request + vmresume
         *   5. Guest continues from after HLT → ioctl returns
         *   6. Daemon re-checks ring for new EXEC_REQUEST
         */
        mb();  /* ensure prior ring writes are visible before HLT */
        asm volatile("hlt" ::: "memory");
        return 0;
    }

    case UTSM_IOCTL_GET_READY: {
        int ready = (g_shm->header.utsm_ready && g_shm->header.linux_ready) ? 1 : 0;
        if (copy_to_user((void __user *)arg, &ready, sizeof(ready)))
            return -EFAULT;
        return 0;
    }

    case UTSM_IOCTL_WRITE_POOL: {
        /* Copy userspace data into payload_pool (daemon → UTSM bulk data) */
        struct utsm_ioctl_pool p;

        if (copy_from_user(&p, (void __user *)arg, sizeof(p)))
            return -EFAULT;
        if (p.offset > UTSM_IPC_PAYLOAD_SIZE ||
            p.len > UTSM_IPC_PAYLOAD_SIZE - p.offset)
            return -EINVAL;
        if (p.len == 0)
            return 0;
        if (copy_from_user(g_shm->payload_pool + p.offset,
                           (void __user *)(uintptr_t)p.user_buf, p.len))
            return -EFAULT;
        mb();  /* ensure pool data visible to UTSM before response msg */
        return 0;
    }

    case UTSM_IOCTL_READ_POOL: {
        /* Copy payload_pool data to userspace (UTSM → daemon bulk data) */
        struct utsm_ioctl_pool p;

        if (copy_from_user(&p, (void __user *)arg, sizeof(p)))
            return -EFAULT;
        if (p.offset > UTSM_IPC_PAYLOAD_SIZE ||
            p.len > UTSM_IPC_PAYLOAD_SIZE - p.offset)
            return -EINVAL;
        if (p.len == 0)
            return 0;
        if (copy_to_user((void __user *)(uintptr_t)p.user_buf,
                         g_shm->payload_pool + p.offset, p.len))
            return -EFAULT;
        return 0;
    }

    case UTSM_IOCTL_HOST_READ: {
        /* Read a file from the UTSM FAT32 disk into userspace.
         * Hypercall stages the chunk into payload_pool, we relay it out. */
        struct utsm_ioctl_hostfile hf;
        char kname[64];
        long ret;
        u32 n;

        if (copy_from_user(&hf, (void __user *)arg, sizeof(hf)))
            return -EFAULT;
        if (hf.len == 0 || hf.len > UTSM_IPC_PAYLOAD_SIZE)
            return -EINVAL;
        if (!hf.user_buf)
            return -EINVAL;
        memcpy(kname, hf.name, sizeof(kname));
        kname[sizeof(kname) - 1] = '\0';

        ret = utsm_hcall_file_read(kname, hf.offset, hf.len);
        if (ret < 0) {
            if (ret == UTSM_HCALL_NOENT) return -ENOENT;
            if (ret == UTSM_HCALL_INVAL) return -EINVAL;
            if (ret == UTSM_HCALL_NOMEM) return -ENOMEM;
            return -EIO;
        }

        n = (u32)(ret & 0xFFFFF);
        hf.total = (u64)ret >> 20;
        if (n > hf.len)
            n = hf.len;  /* defensive: never overflow the user buffer */
        if (n > 0 &&
            copy_to_user((void __user *)(uintptr_t)hf.user_buf,
                         g_shm->payload_pool, n))
            return -EFAULT;
        hf.len = n;
        if (copy_to_user((void __user *)arg, &hf, sizeof(hf)))
            return -EFAULT;
        return 0;
    }

    case UTSM_IOCTL_HOST_WRITE: {
        /* Write userspace data as the whole content of a UTSM FAT32 file.
         * Stage into payload_pool first, then hypercall to commit. */
        struct utsm_ioctl_hostfile hf;
        char kname[64];
        long ret;

        if (copy_from_user(&hf, (void __user *)arg, sizeof(hf)))
            return -EFAULT;
        if (hf.len > UTSM_IPC_PAYLOAD_SIZE)
            return -EINVAL;
        if (hf.len > 0 && !hf.user_buf)
            return -EINVAL;
        memcpy(kname, hf.name, sizeof(kname));
        kname[sizeof(kname) - 1] = '\0';

        if (hf.len > 0 &&
            copy_from_user(g_shm->payload_pool,
                           (void __user *)(uintptr_t)hf.user_buf, hf.len))
            return -EFAULT;
        mb();  /* ensure pool data visible to UTSM before the hypercall */

        ret = utsm_hcall_file_write(kname, hf.len);
        if (ret < 0) {
            if (ret == UTSM_HCALL_INVAL) return -EINVAL;
            if (ret == UTSM_HCALL_NOMEM) return -ENOMEM;
            return -EIO;
        }
        return 0;
    }

    default:
        return -ENOTTY;
    }
}

static __poll_t utsm_dev_poll(struct file *f, struct poll_table_struct *wait)
{
    __poll_t mask = 0;

    if (!g_shm_mapped)
        return EPOLLERR;

    /* Report readable if utsm_to_linux ring has messages */
    if (!utsm_ipc_ring_empty(&g_shm->utsm_to_linux))
        mask |= EPOLLIN | EPOLLRDNORM;

    /* Always writable (ring push may still fail with -EAGAIN) */
    mask |= EPOLLOUT | EPOLLWRNORM;

    return mask;
}

static const struct file_operations utsm_fops = {
    .owner          = THIS_MODULE,
    .write          = utsm_dev_write,
    .read           = utsm_dev_read,
    .unlocked_ioctl = utsm_dev_ioctl,
    .poll           = utsm_dev_poll,
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
		pr_err("[utsm] SHM_INFO hypercall failed: %d\n", ret);
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
