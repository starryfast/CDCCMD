# -*- coding: utf-8 -*-
"""make_icon.py -- 把 64x64 的 CDC 图标升级成多尺寸 .ico

Windows 在资源管理器/任务栏/Alt-Tab 会按需取不同尺寸，
只有一个 64x64 时小尺寸会糊、大尺寸会拉花。
这里用纯 GDI+ 把源图重采样出 16/24/32/48/64/128/256 七档,
全部以 32bpp BGRA(带 alpha) 写入一个标准 .ico。

用法:
    python iss/make_icon.py <源.ico> <输出.ico>
"""
import os
import struct
import sys

SIZES = [16, 24, 32, 48, 64, 128, 256]


def load_ico_first_image(path):
    """读 .ico 里第一个图像, 返回 (w, h, rgba_bytes)。支持 BMP(DIB) 与 PNG 两种。"""
    d = open(path, "rb").read()
    if d[:4] != b"\x00\x00\x01\x00":
        raise ValueError("不是 .ico 文件")
    count = struct.unpack("<H", d[4:6])[0]
    if count == 0:
        raise ValueError("ico 里没有图像")
    off = 6
    w = d[off] or 256
    h = d[off + 1] or 256
    bpp = struct.unpack("<H", d[off + 6:off + 8])[0]
    datasize = struct.unpack("<I", d[off + 8:off + 12])[0]
    dataoff = struct.unpack("<I", d[off + 12:off + 16])[0]
    payload = d[dataoff:dataoff + datasize]

    if payload[:8] == b"\x89PNG\r\n\x1a\n":
        # PNG 压缩: 交给 GDI+ 处理
        return ("png", payload, w, h)

    # BMP/DIB: BITMAPINFOHEADER(40) + 像素 + AND 掩码
    hdr_size = struct.unpack("<I", payload[0:4])[0]
    bw = struct.unpack("<i", payload[4:8])[0]
    bh = struct.unpack("<i", payload[8:12])[0]
    bitcount = struct.unpack("<H", payload[14:16])[0]
    if bitcount != 32:
        raise ValueError("只支持 32bpp 源图, 当前 %d" % bitcount)

    real_h = bh // 2 if bh == bw * 2 else bh  # ico 的高度含掩码, 是真实高度的两倍
    stride = bw * 4
    pixels = bytearray(bw * real_h * 4)
    base = hdr_size
    for y in range(real_h):
        # DIB 是自下而上存的, 需要翻转
        src = base + (real_h - 1 - y) * stride
        pixels[y * stride:(y + 1) * stride] = payload[src:src + stride]
    # BGRA -> RGBA
    for i in range(0, len(pixels), 4):
        pixels[i], pixels[i + 2] = pixels[i + 2], pixels[i]
    return ("raw", bytes(pixels), bw, real_h)


def resample_rgba(rgba, sw, sh, dw, dh):
    """双线性重采样 (带 alpha 预乘, 避免边缘发黑)。"""
    out = bytearray(dw * dh * 4)
    for y in range(dh):
        sy = (y + 0.5) * sh / dh - 0.5
        y0 = max(0, int(sy))
        y1 = min(sh - 1, y0 + 1)
        fy = sy - y0
        if sy < 0:
            y0 = y1 = 0
            fy = 0.0
        for x in range(dw):
            sx = (x + 0.5) * sw / dw - 0.5
            x0 = max(0, int(sx))
            x1 = min(sw - 1, x0 + 1)
            fx = sx - x0
            if sx < 0:
                x0 = x1 = 0
                fx = 0.0

            acc = [0.0, 0.0, 0.0, 0.0]
            for (px, py, wgt) in ((x0, y0, (1 - fx) * (1 - fy)),
                                  (x1, y0, fx * (1 - fy)),
                                  (x0, y1, (1 - fx) * fy),
                                  (x1, y1, fx * fy)):
                if wgt <= 0:
                    continue
                o = (py * sw + px) * 4
                a = rgba[o + 3] / 255.0
                # 预乘后插值
                acc[0] += rgba[o + 0] * a * wgt
                acc[1] += rgba[o + 1] * a * wgt
                acc[2] += rgba[o + 2] * a * wgt
                acc[3] += rgba[o + 3] * wgt

            a_out = acc[3]
            if a_out > 0.001:
                af = a_out / 255.0
                r = min(255, max(0, int(acc[0] / af + 0.5)))
                g = min(255, max(0, int(acc[1] / af + 0.5)))
                b = min(255, max(0, int(acc[2] / af + 0.5)))
            else:
                r = g = b = 0
            o = (y * dw + x) * 4
            out[o + 0] = r
            out[o + 1] = g
            out[o + 2] = b
            out[o + 3] = min(255, max(0, int(a_out + 0.5)))
    return bytes(out)


def rgba_to_png(rgba, w, h):
    """手写最小 PNG 编码 (zlib + 无过滤), 避免依赖 Pillow。"""
    import zlib

    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter type 0 (None)
        raw += rgba[y * w * 4:(y + 1) * w * 4]

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    out += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    out += chunk(b"IEND", b"")
    return out


def build_ico(src, dst):
    kind, payload, sw, sh = load_ico_first_image(src)
    if kind == "png":
        raise SystemExit("源图标是 PNG 压缩格式, 请先用别的方式转成 32bpp DIB")
    print("源图: %dx%d 32bpp DIB" % (sw, sh))

    entries = []
    for size in SIZES:
        if size > max(sw, sh) * 2:
            continue
        rgba = resample_rgba(payload, sw, sh, size, size)
        entries.append((size, rgba))
        print("  生成 %dx%d" % (size, size))

    # 全部用 PNG 编码写入 (Vista+ 全支持, 体积也小)
    images = []
    for size, rgba in entries:
        png = rgba_to_png(rgba, size, size)
        images.append((size, png))

    header = struct.pack("<HHH", 0, 1, len(images))
    dirsz = 6 + 16 * len(images)
    offset = dirsz
    dirs = b""
    blob = b""
    for size, png in images:
        b = 0 if size >= 256 else size
        dirs += struct.pack("<BBBBHHII", b, b, 0, 0, 1, 32, len(png), offset)
        offset += len(png)
        blob += png

    open(dst, "wb").write(header + dirs + blob)
    print("写出: %s (%d 字节, %d 档尺寸)" % (dst, 6 + 16 * len(images) + len(blob), len(images)))


if __name__ == "__main__":
    src = sys.argv[1] if len(sys.argv) > 1 else "assets/cdccmd.ico"
    dst = sys.argv[2] if len(sys.argv) > 2 else "assets/cdccmd_multi.ico"
    build_ico(src, dst)
