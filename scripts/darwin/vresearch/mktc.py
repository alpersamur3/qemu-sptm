"""Build one version-1 trust cache out of others and CDHash lists.

    mktc.py OUT [IN ...]

IN is a trust cache (raw or IM4P-wrapped, version 1: 22-byte entries) or a text
file with one 40-hex-digit CDHash per line. Entries are merged, deduplicated and
sorted (the kernel binary-searches them).
"""
import struct, sys


def im4p_payload(buf):
    """Payload OCTET STRING of an IM4P (SEQUENCE { "IM4P", type, desc, payload, ... })."""
    def hdr(o):
        tag, ln = buf[o], buf[o + 1]
        o += 2
        if ln & 0x80:
            n = ln & 0x7f
            ln = int.from_bytes(buf[o:o + n], 'big')
            o += n
        return tag, ln, o
    tag, ln, o = hdr(0)
    assert tag == 0x30
    for _ in range(3):		# "IM4P", type, description
        _, l, o2 = hdr(o)
        o = o2 + l
    tag, ln, o = hdr(o)
    assert tag == 0x04
    return buf[o:o + ln]


def read_tc(buf):
    if b'IM4P' in buf[:16]:
        buf = im4p_payload(buf)
    ver, = struct.unpack_from('<I', buf)
    if ver != 1:
        raise SystemExit(f'trust cache version {ver}, only 1 is supported')
    n, = struct.unpack_from('<I', buf, 20)
    return {buf[24 + 22 * i:24 + 22 * i + 20]: buf[24 + 22 * i + 20:24 + 22 * i + 22] for i in range(n)}


def main():
    entries = {}
    for path in sys.argv[2:]:
        buf = open(path, 'rb').read()
        if buf[:1] in (b'\x30', b'\x01'):
            entries.update(read_tc(buf))
        else:
            for line in buf.decode().split():
                entries.setdefault(bytes.fromhex(line), bytes([2, 0]))	# hashType 2 (SHA-256 truncated), no flags
    out = struct.pack('<I', 1) + b'A' * 16 + struct.pack('<I', len(entries))
    out += b''.join(h + entries[h] for h in sorted(entries))
    open(sys.argv[1], 'wb').write(out)
    print(f'{sys.argv[1]}: {len(entries)} entries')


if __name__ == '__main__':
    main()
