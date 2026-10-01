/* hypercall.c — UTSM-side VMCALL hypercall handler.
 *
 * When the Linux guest executes a VMCALL instruction, it triggers a VM-Exit
 * with exit reason EXIT_VCALL (18). The guest places:
 *   RAX = UTSM_HCALL_MAGIC | op    (identifies this as a UTSM hypercall)
 *   RDI = arg0
 *   RSI = arg1
 *   RDX = arg2
 *
 * The VM-Exit assembly stub saves all guest GPRs into g_guest_regs before
 * calling the C dispatcher. This handler reads arguments from g_guest_regs,
 * dispatches to the operation handler, writes the return value into
 * g_guest_regs.rax, and advances GUEST_RIP so the guest resumes after
 * the VMCALL instruction.
 *
 * Protocol details in CODE/utsm-ipc/ipc_proto.h.
 */

#include <utsm/hypercall.h>
#include <utsm/vmx.h>
#include <utsm/ept.h>
#include <utsm/vmm.h>
#include <utsm/ipc_shm.h>
#include <utsm/linux_compat.h>
#include <utsm/linux_loader.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include <ipc_proto.h>
#include "../arch/x86_64/limine.h"

extern volatile struct limine_hhdm_request g_hhdm_request;

static u64 g_hypercall_count;

/* ===== Guest memory access helpers =====
 *
 * The Linux guest uses identity-mapped page tables (GVA == GPA), so
 * pointers passed via hypercall arguments are GPAs. We translate GPA→HPA
 * via EPT walk, then add HHDM offset to get the host virtual address. */

static void *gpa_to_hva(u64 gpa) {
    u64 hpa = ept_gpa_to_hpa(gpa);
    if (hpa == 0) return (void *)0;
    if (!g_hhdm_request.response) return (void *)0;
    u64 hhdm = g_hhdm_request.response->offset;
    return (void *)(hhdm + hpa);
}

static void *guest_ptr_to_hva(u64 guest_ptr) {
    u64 gpa = vmx_guest_gva_to_gpa(guest_ptr);
    if (gpa == 0) gpa = guest_ptr;
    return gpa_to_hva(gpa);
}

/* Write a u64 to guest memory at the given GPA.
 * Returns 0 on success, -1 on failure. */
static int guest_write_u64(u64 gpa, u64 value) {
    void *hva = guest_ptr_to_hva(gpa);
    if (!hva) return -1;
    *(volatile u64 *)hva = value;
    return 0;
}

/* Copy bytes from guest memory (GVA=GPA) to host buffer.
 * Handles cross-page boundaries by translating each page separately.
 * Returns 0 on success, -1 on failure. */
static int guest_read_buf(u64 gva, void *host_dst, u64 len) {
    u8 *dst = (u8 *)host_dst;
    u64 copied = 0;
    while (copied < len) {
        void *hva = guest_ptr_to_hva(gva + copied);
        if (!hva) return -1;
        u64 page_off = (gva + copied) & 0xFFFULL;
        u64 chunk = 4096 - page_off;
        if (chunk > len - copied) chunk = len - copied;
        const u8 *src = (const u8 *)hva;
        for (u64 i = 0; i < chunk; i++) {
            dst[copied + i] = src[i];
        }
        copied += chunk;
    }
    return 0;
}

/* ===== Individual hypercall handlers ===== */

static long hcall_ping(u64 a0, u64 a1, u64 a2) {
    (void)a0; (void)a1; (void)a2;
    /* Return a recognizable PONG value */
    return 0x504F4E47;  /* "PONG" */
}

static long hcall_hello(u64 a0, u64 a1, u64 a2) {
    (void)a0; (void)a1; (void)a2;
    log_info("[HCALL] HELLO from Linux guest");

    /* Linux is announcing itself; the IPC shared memory's linux_ready
     * flag is set separately when Linux maps the shared memory. */
    if (ipc_shm_is_ready()) {
        /* Send a HELLO_ACK message via the shared memory ring */
        const char *msg = "HELLO from UTSM";
        ipc_shm_send(UTSM_MSG_HELLO_ACK, msg, 15);
    }
    return UTSM_HCALL_OK;
}

static long hcall_shm_info(u64 a0, u64 a1, u64 a2) {
    /* a0 = GPA of gpa_out (u64), a1 = GPA of size_out (u64) */
    (void)a2;
    if (!ipc_shm_is_ready()) {
        return UTSM_HCALL_NOMEM;
    }

    u64 shm_gpa = ipc_shm_get_gpa();
    u64 shm_size = ipc_shm_get_size();

    /* Write results to guest memory */
    if (a0 != 0 && guest_write_u64(a0, shm_gpa) != 0) {
        log_error("[HCALL] SHM_INFO: failed to write gpa to guest");
        log_hex64("[HCALL] SHM_INFO: a0=", a0);
        log_hex64("[HCALL] SHM_INFO: walk=", vmx_guest_gva_to_gpa(a0));
        return UTSM_HCALL_INVAL;
    }
    if (a1 != 0 && guest_write_u64(a1, shm_size) != 0) {
        log_error("[HCALL] SHM_INFO: failed to write size to guest");
        return UTSM_HCALL_INVAL;
    }

    log_hex64("[HCALL] SHM_INFO: gpa=", shm_gpa);
    log_hex64("[HCALL] SHM_INFO: size=", shm_size);
    return UTSM_HCALL_OK;
}

/* VSCode integration Phase 1: query graphics surface pool.
 * Mirrors hcall_shm_info: writes gpa + size into guest-provided out-pointers.
 * Linux user-space then mmaps /dev/utsm at the returned GPA to obtain a
 * writable scanout buffer (Phase 3 virtio-gpu shadow buffer). */
static long hcall_surface_info(u64 a0, u64 a1, u64 a2) {
    /* a0 = GPA of gpa_out (u64), a1 = GPA of size_out (u64) */
    (void)a2;

    u64 surface_gpa = 0, surface_size = 0;
    if (linux_get_surface_info(0, &surface_gpa, &surface_size) != 0) {
        /* Surface pool not initialized (Linux guest not loaded, or alloc failed) */
        return UTSM_HCALL_NOMEM;
    }

    /* Write results to guest memory */
    if (a0 != 0 && guest_write_u64(a0, surface_gpa) != 0) {
        log_error("[HCALL] SURFACE_INFO: failed to write gpa to guest");
        return UTSM_HCALL_INVAL;
    }
    if (a1 != 0 && guest_write_u64(a1, surface_size) != 0) {
        log_error("[HCALL] SURFACE_INFO: failed to write size to guest");
        return UTSM_HCALL_INVAL;
    }

    log_hex64("[HCALL] SURFACE_INFO: gpa=", surface_gpa);
    log_hex64("[HCALL] SURFACE_INFO: size=", surface_size);
    return UTSM_HCALL_OK;
}

static long hcall_cap_validate(u64 a0, u64 a1, u64 a2) {
    /* Phase 1.3: stub — capability validation not yet implemented */
    (void)a0; (void)a1; (void)a2;
    log_info("[HCALL] CAP_VALIDATE (stub)");
    return UTSM_HCALL_NOSYS;
}

static long hcall_utrw_read(u64 a0, u64 a1, u64 a2) {
    /* Phase 1.3: stub — sealed memory read not yet implemented */
    (void)a0; (void)a1; (void)a2;
    log_info("[HCALL] UTRW_READ (stub)");
    return UTSM_HCALL_NOSYS;
}

static long hcall_utrw_write(u64 a0, u64 a1, u64 a2) {
    /* Phase 1.3: stub — sealed memory write not yet implemented */
    (void)a0; (void)a1; (void)a2;
    log_info("[HCALL] UTRW_WRITE (stub)");
    return UTSM_HCALL_NOSYS;
}

static long hcall_drr_checkpoint(u64 a0, u64 a1, u64 a2) {
    /* Phase 1.3: stub — DRR checkpoint trigger not yet implemented */
    (void)a0; (void)a1; (void)a2;
    log_info("[HCALL] DRR_CHECKPOINT (stub)");
    return UTSM_HCALL_NOSYS;
}

static long hcall_console_write(u64 a0, u64 a1, u64 a2) {
    /* a0 = GPA of string buffer, a1 = length */
    (void)a2;
    if (a0 == 0 || a1 == 0) {
        return UTSM_HCALL_INVAL;
    }

    /* Cap length to prevent runaway reads */
    if (a1 > 4096) a1 = 4096;

    /* Read the string from guest memory */
    char buf[4097];
    if (guest_read_buf(a0, buf, a1) != 0) {
        log_error("[HCALL] CONSOLE_WRITE: failed to read guest buffer");
        return UTSM_HCALL_INVAL;
    }
    buf[a1] = '\0';

    /* Output to UTSM serial console with a [LINUX] prefix */
    serial_write("[LINUX] ");
    serial_write(buf);
    return (long)a1;
}

static long hcall_console_read(u64 a0, u64 a1, u64 a2) {
    /* Phase 1.3: stub — console read not yet implemented */
    (void)a0; (void)a1; (void)a2;
    return UTSM_HCALL_NOSYS;
}

/* ===== Phase 3: Linux guest ↔ UTSM FAT32 file transfer =====
 *
 * payload_pool at offset 0 is the bulk data buffer. ABI details are
 * documented in ipc_proto.h ("Linux ↔ UTSM FAT32 hypercalls").
 *
 * Return packing for FILE_READ (positive RAX):
 *   low 20 bits = staged byte count (pool capacity < 1MB → fits)
 *   RAX >> 20   = total file size (FAT32 root files ≤ 256KB → fits) */

/* Read a NUL-terminated file name (max 63 chars) from guest memory. */
static int hcall_read_name(u64 gpa, char out[64]) {
    if (guest_read_buf(gpa, out, 64) != 0) return -1;
    out[63] = '\0';
    return 0;
}

/* Map lxc_f32_* error codes to hypercall return codes. */
static long hcall_f32_err(int rc) {
    if (rc == -10) return UTSM_HCALL_INVAL;  /* illegal file name */
    if (rc == -11) return UTSM_HCALL_NOENT;  /* not found */
    if (rc == -9)  return UTSM_HCALL_IO;     /* no block device */
    return UTSM_HCALL_IO;
}

static long hcall_file_read(u64 a0, u64 a1, u64 a2) {
    /* a0 = name GPA, a1 = file offset, a2 = max bytes to stage */
    if (a0 == 0) return UTSM_HCALL_INVAL;

    char name[64];
    if (hcall_read_name(a0, name) != 0) return UTSM_HCALL_INVAL;

    u8 *fdata = (u8 *)0;
    u32 fsize = 0;
    int rc = lxc_f32_read_file(name, &fdata, &fsize);
    if (rc != 0) return hcall_f32_err(rc);

    u64 cap = ipc_shm_payload_capacity();
    u64 want = a2;
    if (want > cap) want = cap;

    u64 n = 0;
    if (a1 < (u64)fsize) {
        n = (u64)fsize - a1;
        if (n > want) n = want;
    }

    if (n > 0) {
        u8 *pool = (u8 *)ipc_shm_payload_ptr(0);
        if (!pool) return UTSM_HCALL_NOMEM;
        for (u64 i = 0; i < n; i++) pool[i] = fdata[a1 + i];
    }

    return (long)(((u64)fsize << 20) | n);
}

static long hcall_file_write(u64 a0, u64 a1, u64 a2) {
    /* a0 = name GPA, a1 = reserved (0), a2 = bytes staged at pool[0] */
    (void)a1;
    if (a0 == 0) return UTSM_HCALL_INVAL;
    if (a2 > ipc_shm_payload_capacity() || a2 > 262144) {
        return UTSM_HCALL_INVAL;   /* fat32_io 单文件上限 256KB */
    }

    char name[64];
    if (hcall_read_name(a0, name) != 0) return UTSM_HCALL_INVAL;

    const u8 *pool = (const u8 *)ipc_shm_payload_ptr(0);
    if (!pool && a2 > 0) return UTSM_HCALL_NOMEM;

    int rc = lxc_f32_write_file(name, pool, (u32)a2);
    if (rc != 0) return hcall_f32_err(rc);
    return (long)a2;
}

/* ===== Main hypercall dispatcher ===== */

int hypercall_handle(u64 guest_rax, u64 guest_rdi, u64 guest_rsi, u64 guest_rdx,
                     u64 guest_rip, u64 instr_len) {
    g_hypercall_count++;

    /* Validate magic: the high bits of RAX must be UTSM_HCALL_MAGIC */
    if ((guest_rax & ~0xFFFFULL) != UTSM_HCALL_MAGIC) {
        log_hex64("[HCALL] bad magic: ", guest_rax);
        /* Return error in guest RAX */
        g_guest_regs.rax = (u64)UTSM_HCALL_INVAL;
        /* Still advance RIP to avoid infinite loop */
        vmx_vmcs_write(VMCS_GUEST_RIP, guest_rip + instr_len);
        return 1;  /* resume guest */
    }

    /* Extract operation from low 16 bits */
    u64 op = guest_rax & 0xFFFFULL;
    long result = UTSM_HCALL_NOSYS;

    switch (op) {
    case UTSM_HCALL_PING:
        result = hcall_ping(guest_rdi, guest_rsi, guest_rdx);
        break;
    case UTSM_HCALL_HELLO:
        result = hcall_hello(guest_rdi, guest_rsi, guest_rdx);
        break;
    case UTSM_HCALL_PARK:
        /* Daemon park: return to DSK. Idle HLT must not use this path. */
        g_guest_regs.rax = (u64)UTSM_HCALL_OK;
        vmx_vmcs_write(VMCS_GUEST_RIP, guest_rip + instr_len);
        g_guest_parked = 1;
        log_info("[HCALL] PARK - Linux guest parked");
        return 0;
    case UTSM_HCALL_SHM_INFO:
        result = hcall_shm_info(guest_rdi, guest_rsi, guest_rdx);
        break;
    case UTSM_HCALL_SURFACE_INFO:
        result = hcall_surface_info(guest_rdi, guest_rsi, guest_rdx);
        break;
    case UTSM_HCALL_CAP_VALIDATE:
        result = hcall_cap_validate(guest_rdi, guest_rsi, guest_rdx);
        break;
    case UTSM_HCALL_UTRW_READ:
        result = hcall_utrw_read(guest_rdi, guest_rsi, guest_rdx);
        break;
    case UTSM_HCALL_UTRW_WRITE:
        result = hcall_utrw_write(guest_rdi, guest_rsi, guest_rdx);
        break;
    case UTSM_HCALL_DRR_CHECKPOINT:
        result = hcall_drr_checkpoint(guest_rdi, guest_rsi, guest_rdx);
        break;
    case UTSM_HCALL_CONSOLE_WRITE:
        result = hcall_console_write(guest_rdi, guest_rsi, guest_rdx);
        break;
    case UTSM_HCALL_CONSOLE_READ:
        result = hcall_console_read(guest_rdi, guest_rsi, guest_rdx);
        break;
    case UTSM_HCALL_FILE_READ:
        result = hcall_file_read(guest_rdi, guest_rsi, guest_rdx);
        break;
    case UTSM_HCALL_FILE_WRITE:
        result = hcall_file_write(guest_rdi, guest_rsi, guest_rdx);
        break;
    default:
        log_hex64("[HCALL] unknown op: ", op);
        result = UTSM_HCALL_NOSYS;
        break;
    }

    /* Write return value into guest RAX (restored by assembly before vmresume) */
    g_guest_regs.rax = (u64)result;

    /* Advance guest RIP past the VMCALL instruction */
    vmx_vmcs_write(VMCS_GUEST_RIP, guest_rip + instr_len);

    return 1;  /* resume guest */
}

u64 hypercall_get_count(void) {
    return g_hypercall_count;
}
