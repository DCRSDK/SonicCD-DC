#!/usr/bin/env python3
"""Extract an RSDKv3 (Sonic CD 2011) Data.rsdk into a directory tree."""
import os, struct, sys

E_A = b"4RaS9D7KaEbxcp2o5r6t"   # 20 bytes
E_B = b"3tRaUxLmEaSn"           # 12 bytes

def decrypt(buf, size):
    no = (size & 0x1FC) >> 2
    pb = (no % 9) + 1
    pa = (no % pb) + 1
    swap = False
    out = bytearray(len(buf))
    for i, b in enumerate(buf):
        b ^= E_B[pb] ^ no; pb += 1
        if swap:
            b = ((b & 0xF) << 4) | (b >> 4)
        b ^= E_A[pa]; pa += 1
        if pa <= 19 or pb <= 11:
            if pa > 19: pa = 1; swap = not swap
            if pb > 11: pb = 1; swap = not swap
        else:
            no = (no + 1) & 0x7F
            if swap:
                swap = False; pa = (no % 12) + 6; pb = (no % 5) + 4
            else:
                swap = True;  pa = (no % 15) + 3; pb = (no % 7) + 1
        out[i] = b & 0xFF
    return bytes(out)

def main(rsdk, outdir):
    data = open(rsdk, 'rb').read()
    total = len(data)
    hdr, = struct.unpack_from('<I', data, 0)
    ndir, = struct.unpack_from('<H', data, 4)
    p = 6
    dirs = []
    for _ in range(ndir):
        n = data[p]; p += 1
        name = bytes(c ^ ((-1 - n) & 0xFF) for c in data[p:p+n]).decode('ascii'); p += n
        off, = struct.unpack_from('<I', data, p); p += 4
        dirs.append((name, off))

    count = 0
    for i, (name, off) in enumerate(dirs):
        start = hdr + off
        end = hdr + dirs[i+1][1] if i + 1 < len(dirs) else total
        q = start
        while q < end:
            n = data[q]; q += 1
            if n == 0 or q + n > end: break
            fname = bytes((~c) & 0xFF for c in data[q:q+n]).decode('ascii'); q += n
            fsize, = struct.unpack_from('<I', data, q); q += 4
            if q + fsize > total: break
            blob = decrypt(data[q:q+fsize], fsize); q += fsize
            dest = os.path.join(outdir, name.replace('/', os.sep), fname)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            open(dest, 'wb').write(blob)
            count += 1
    print(f"extracted {count} files from {ndir} directories")

if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit("usage: rsdkv3_extract.py <Data.rsdk> <outdir>")
    main(sys.argv[1], sys.argv[2])
