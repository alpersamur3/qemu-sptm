/*
 * vresearch PVG: Apple's paravirtualised GPU (/arm-io/gfx) and its IOSurface
 * mapper (/arm-io/iosurface-mapper), as the display-only model (pvg=on,
 * vresearch_pvg.c) or over the reims-vgpu library (pvg=reims,
 * vresearch_pvg_reims.c, CONFIG_REIMS_VGPU). Both have the same shape: two
 * 0x10000 MMIO regions, two interrupts, 'width' and 'height' properties.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_VRESEARCH_PVG_H
#define HW_ARM_VRESEARCH_PVG_H

#define TYPE_VRESEARCH_PVG "vresearch-pvg"
#define TYPE_VRESEARCH_PVG_REIMS "vresearch-pvg-reims"

#endif
