/*
 * aks-crypt: a SEP-less, reversible software keystore crypto model for the
 * vresearch machine. The guest kernel's AppleSEPKeyStore wrap/unwrap handlers
 * are patched (aks-crypt=on, see xnu_patch.c) to issue an HVC with one of the
 * function ids below instead of talking to a SEP; QEMU answers it here with
 * real AES over a fixed, machine-local key.
 *
 * This is emulator-only: there is no SEP isolation and the wrapping key lives
 * in the VMM, not in secure hardware. It exists so a research VM with no SEP
 * can create the user persona (keybag class-key wrap/unwrap) and reach the UI.
 */
#ifndef HW_ARM_VRESEARCH_AKSCRYPT_H
#define HW_ARM_VRESEARCH_AKSCRYPT_H

#include "hw/core/cpu.h"

/*
 * HVC function ids placed in x0 by the patched keystore code. Chosen clear of
 * the PSCI function ids QEMU implements and of the kernel's own HVCs; only our
 * code-cave ever issues them, so there is no collision in practice.
 *   x0 = fn, x1 = in VA, x2 = in len, x3 = out VA, x4 = out capacity,
 *   x5 = key class, x6 = keybag handle.  Returns the output length in x0
 *   (0 on any failure, so the guest copies out nothing and fails gracefully).
 */
#define VRESEARCH_AKSCRYPT_FN_WRAP    0xC1000010ULL
#define VRESEARCH_AKSCRYPT_FN_UNWRAP  0xC1000011ULL

/*
 * The HVC/PSCI path calls this hook if it is non-NULL. It returns true when it
 * recognized x0 as an aks-crypt call and handled it (result already in x0),
 * false to let normal HVC handling continue. Defined in target/arm (always
 * linked), set by vresearch_akscrypt_enable() so target/arm needs no link-time
 * dependency on the vresearch machine.
 */
extern bool (*arm_akscrypt_hvc_hook)(CPUState *cs);

/* Enable the model (machine option aks-crypt=on). Idempotent. */
void vresearch_akscrypt_enable(void);

#endif /* HW_ARM_VRESEARCH_AKSCRYPT_H */
