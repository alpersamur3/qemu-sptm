#!/usr/bin/env python3
"""Prepare a VResearch1 device tree (vphone600ap, vresearch101ap) for -M vresearch.

iBoot normally fills /memory, /chosen and /chosen/memory-map. Without iBoot this
script does it, the way darwin-vm's dt_fixup.py does for real SoC device trees:
the loader (hw/arm/xnuboot_sptm.c) then writes the SPTM/TXM/kernelcache regions
into the named /chosen/memory-map entries.

    vresearch_dt_fixup.py IN OUT [--dram-base A] [--dram-size N] [--cpus N]
                                 [--keep-sep] [--nvram FILE]

The input is the unwrapped (im4p payload, decompressed) DeviceTree.*.im4p.
"""
import argparse
import struct
import zlib

DRAM_BASE = 0x80000000      # above arm-io's 0x10000000-0x80000000 window
DRAM_SIZE = 4 << 30
FIRMWARE_NAME = 'qemu-sptm'
PLATFORM_NAME = 'vresearch1'

# Filled by the loader. iBoot leaves unused regions at (-1, -1).
MEMORY_MAP_ENTRIES = [
    'TXM-ro', 'TXM-rx', 'TXM-bx', 'TXM-rw', 'TXM-le', 'TXM-entry', 'TXM-virt',
    'TrustCache',
    'BootKC-rx', 'BootKC-bx', 'BootKC-ro', 'BootKC-rs', 'BootKC-rw', 'BootKC-le',
    'BootKC-virt', 'BootKC-entry',
    'DeviceTree',
    'SPTM-ro', 'SPTM-rm', 'SPTM-rx', 'SPTM-rw', 'SPTM-le', 'SPTM-entry', 'SPTM-virt',
    'BootArgs', 'slide',
    'CL4-rx', 'CL4-ro', 'CL4-rw', 'CL4-le', 'CL4-dummypage', 'CL4-entry', 'CL4-virt',
    'RAMDisk',
]


class Node:
    def __init__(self):
        self.props = {}     # name -> [placeholder flag, raw bytes]
        self.children = []

    @property
    def name(self):
        return self.props['name'][1].split(b'\0')[0].decode()

    def child(self, name):
        for c in self.children:
            if c.name == name:
                return c
        raise KeyError(f'{self.name} has no child {name}')

    def remove(self, name):
        self.children = [c for c in self.children if c.name != name]

    def set(self, name, val):
        if isinstance(val, str):
            val = val.encode() + b'\0'
        self.props[name] = [0, val]     # a real value, no longer an iBoot placeholder


def u32(v):
    return struct.pack('<I', v)


def u64(v):
    return struct.pack('<Q', v)


def padded(s, size):
    b = s.encode()
    assert len(b) < size
    return b.ljust(size, b'\0')


def parse(buf, off=0):
    n = Node()
    nprops, nchildren = struct.unpack_from('<II', buf, off)
    off += 8
    for _ in range(nprops):
        name = buf[off:off + 32].split(b'\0')[0].decode()
        ln, = struct.unpack_from('<I', buf, off + 32)
        flags, ln = ln & 0x80000000, ln & 0x7fffffff
        n.props[name] = [flags, bytes(buf[off + 36:off + 36 + ln])]
        off += 36 + ((ln + 3) & ~3)
    for _ in range(nchildren):
        c, off = parse(buf, off)
        n.children.append(c)
    return n, off


def encode(n):
    out = [struct.pack('<II', len(n.props), len(n.children))]
    for name, (flags, val) in n.props.items():
        if len(name) >= 32:
            raise ValueError(f'property name too long: {name}')
        out.append(name.encode().ljust(32, b'\0'))
        out.append(struct.pack('<I', len(val) | flags))
        out.append(val.ljust((len(val) + 3) & ~3, b'\0'))
    for c in n.children:
        out.append(encode(c))
    return b''.join(out)


def chrp_partition(sig, name, size, body=b''):
    """CHRP NVRAM partition: sig, checksum, length in 16-byte units, 12-byte name."""
    ln = size // 16
    hdr = struct.pack('<BBH12s', sig, 0, ln, name.encode())
    ck = sig + (ln & 0xff) + (ln >> 8) + sum(hdr[4:])
    while ck >> 8:
        ck = (ck & 0xff) + (ck >> 8)
    return hdr[:1] + bytes([ck]) + hdr[2:] + body.ljust(size - 16, b'\0')


def empty_nvram(size=0x2000):
    """Apple NVRAM image with an empty 'common' partition."""
    img = chrp_partition(0x5a, 'nvram', 0x20)
    img += chrp_partition(0x70, 'common', 0x800)
    img += chrp_partition(0x7f, 'wwwwwwwwwwww', size - len(img))
    img = bytearray(img)
    struct.pack_into('<I', img, 0x14, 1)        # generation
    struct.pack_into('<I', img, 0x10, zlib.adler32(bytes(img[0x14:])))
    return bytes(img)


def resolve_placeholders(n):
    """
    iBoot fills placeholder properties (bit 31 of the length) from syscfg or the
    MAC address store, e.g. 'macaddr/wifiaddr,syscfg/WMac/6' or
    'syscfg/SrNm/0x20,zeroes/0x20'. SPTM's device tree walker doesn't mask the
    bit, so none may be left. Without syscfg: sized ones become zeroes, the rest
    go away (as when the syscfg key is missing).
    """
    for name, (flags, val) in list(n.props.items()):
        if not flags:
            continue
        size = None
        for alt in val.rstrip(b'\0').decode().split(','):
            parts = alt.split('/')
            if parts[0] == 'zeroes' or len(parts) >= 3:
                size = int(parts[-1], 0)
                break
        if size is None:
            del n.props[name]
        else:
            n.props[name] = [0, bytes(size)]
    for c in n.children:
        resolve_placeholders(c)


def fixup(root, dram_base, dram_size, ncpus, keep_sep, nvram):
    chosen = root.child('chosen')
    arm_io = root.child('arm-io')

    resolve_placeholders(root)

    root.set('platform-name', padded(PLATFORM_NAME, len(root.props['platform-name'][1])))

    # /memory and the /chosen entries SPTM and the kernel look up
    root.child('memory').set('reg', u64(dram_base) + u64(dram_size))
    chosen.set('dram-base', u64(dram_base))
    chosen.set('dram-size', u64(dram_size))
    chosen.set('firmware-version', padded(FIRMWARE_NAME, len(chosen.props['firmware-version'][1])))
    chosen.set('system-firmware-version', FIRMWARE_NAME)
    chosen.set('random-seed', b'A' * len(chosen.props['random-seed'][1]))
    chosen.set('kernel-ctrr-to-be-enabled', u32(0))

    chosen.set('nvram-proxy-data', nvram)
    chosen.set('nvram-total-size', u32(len(nvram)))
    chosen.set('nvram-bank-size', u32(len(nvram)))
    chosen.set('nvram-bank-count', u32(1))
    chosen.set('nvram-current-bank', u32(1))

    mm = chosen.child('memory-map')
    for name in MEMORY_MAP_ENTRIES:
        mm.set(name, u64(0xffffffffffffffff) * 2)
    mm.set('slide', u64(0) * 2)

    # /product/util: a block split by its 'regions' (id, offset, size) triplets,
    # presumably memory shared with the host. iBoot fills 'reg'; put it right
    # after DRAM, outside the memory the kernel manages. The machine backs it
    # with zeroed RAM.
    util = root.child('product').child('util')
    regions = struct.unpack(f'<{len(util.props["regions"][1]) // 4}I', util.props['regions'][1])
    util_size = max(regions[i + 1] + regions[i + 2] for i in range(0, len(regions), 3))
    util.set('reg', u64(dram_base + dram_size) + u64(util_size))

    # One CPU: SMP needs PSCI CPU_ON under SPTM, not done yet
    cpus = root.child('cpus')
    keep = {f'cpu{i}' for i in range(ncpus)}
    cpus.children = [c for c in cpus.children if c.name in keep]
    cpus.child('cpu0').set('state', 'running')

    # No SEP: the driver doesn't match without the node
    if not keep_sep:
        arm_io.remove('sep')
        chosen.set('sepfw-load-at-boot', u32(0))

    # serial console on uart0 (already so in the vphone600ap tree)
    root.child('defaults').set('serial-device', arm_io.child('uart0').props['AAPL,phandle'][1])


def main():
    p = argparse.ArgumentParser(prog='vresearch_dt_fixup')
    p.add_argument('dtree', type=argparse.FileType('rb'))
    p.add_argument('out', type=argparse.FileType('wb'))
    p.add_argument('--dram-base', type=lambda x: int(x, 0), default=DRAM_BASE)
    p.add_argument('--dram-size', type=lambda x: int(x, 0), default=DRAM_SIZE)
    p.add_argument('--cpus', type=int, default=1)
    p.add_argument('--keep-sep', action='store_true', help='keep /arm-io/sep')
    p.add_argument('--nvram', type=argparse.FileType('rb'),
                   help='NVRAM image (default: an empty one)')
    args = p.parse_args()

    root, _ = parse(args.dtree.read())
    if root.child('arm-io').props['compatible'][1].split(b'\0')[0] != b'arm-io,vmapple1':
        raise SystemExit('not a VResearch1 device tree (arm-io isn\'t arm-io,vmapple1)')
    nvram = args.nvram.read() if args.nvram else empty_nvram()
    fixup(root, args.dram_base, args.dram_size, args.cpus, args.keep_sep, nvram)
    args.out.write(encode(root))


if __name__ == '__main__':
    main()
