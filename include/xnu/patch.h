#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

bool kc_uses_mte(uint8_t *macho);
void patch_kc(uint8_t *macho);
void patch_kc_unauthenticated_root(uint8_t *macho);
void patch_kc_aks_stub(uint8_t *macho);
void patch_kc_aks_crypt(uint8_t *macho);
void patch_kc_amfi_lc_off(uint8_t *macho);
void patch_kc_acm_stub(uint8_t *macho);
void patch_kc_hfs_dp_stub(uint8_t *macho);
