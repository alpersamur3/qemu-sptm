#ifndef XNU_BOOT_ARGS_H
#define XNU_BOOT_ARGS_H

// from xnu-12377.121.6/pexpert/pexpert/arm64/boot.h

/*
 * Copyright (c) 2007-2009 Apple Inc. All rights reserved.
 * Copyright (c) 2000-2006 Apple Computer, Inc. All rights reserved.
 */
/*
 * @OSF_COPYRIGHT@
 */

#include <stdint.h>

#define BOOT_LINE_LENGTH 1024

/*
 * Video information..
 */

/* unsigned long in XNU, i.e. 64-bit; spelled out so LLP64 hosts (Windows) agree */
struct Boot_Video {
	uint64_t        v_baseAddr;     /* Base address of video memory */
	uint64_t        v_display;      /* Display Code (if Applicable */
	uint64_t        v_rowBytes;     /* Number of bytes per pixel row */
	uint64_t        v_width;        /* Width */
	uint64_t        v_height;       /* Height */
	uint64_t        v_depth;        /* Pixel Depth and other parameters */
};

#define kBootVideoDepthMask             (0xFF)
#define kBootVideoDepthDepthShift       (0)
#define kBootVideoDepthRotateShift      (8)
#define kBootVideoDepthScaleShift       (16)
#define kBootVideoDepthBootRotateShift  (24)

#define kBootFlagsDarkBoot              (1ULL << 0)

typedef struct Boot_Video       Boot_Video;

/* Boot argument structure - passed into Mach kernel at boot time.
 */
#define kBootArgsRevision               1
#define kBootArgsRevision2              2       /* added boot_args.bootFlags */
#define kBootArgsVersion1               1
#define kBootArgsVersion2               2

typedef struct xnu_boot_args {
	uint16_t                Revision;                       /* Revision of boot_args structure */
	uint16_t                Version;                        /* Version of boot_args structure */
	uint64_t                virtBase;                       /* Virtual base of memory */
	uint64_t                physBase;                       /* Physical base of memory */
	uint64_t                memSize;                        /* Size of memory */
	uint64_t                topOfKernelData;        /* Highest physical address used in kernel data area */
	Boot_Video              Video;                          /* Video Information */
	uint32_t                machineType;            /* Machine Type */
	uint64_t                deviceTreeP;           /* Base of flattened device tree */
	uint32_t                deviceTreeLength;       /* Length of flattened tree */
	char                    CommandLine[BOOT_LINE_LENGTH];  /* Passed in command line */
	uint64_t                bootFlags;              /* Additional flags specified by the bootloader */
	uint64_t                memSizeActual;          /* Actual size of memory */
} boot_args;

/* the arm64 XNU layout, whatever the host's long is */
QEMU_BUILD_BUG_ON(offsetof(boot_args, deviceTreeP) != 96);

#define SOC_DEVICE_TYPE_BUFFER_SIZE     32

#define PC_TRACE_BUF_SIZE               1024

#define CDBG_MEM ((sizeof(dbg_registry_t) + PAGE_SIZE - 1) & ~PAGE_MASK)

#define PE_EARLY_BOOT_VA (CDBG_MEM)

#endif
