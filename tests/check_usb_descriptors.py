#!/usr/bin/env python3
"""Validate the actual compiled ESP32-S3 UAC2 microphone + CDC descriptors."""
import argparse
import struct
from pathlib import Path


def sections(path):
    blob = Path(path).read_bytes()
    assert blob[:6] == b"\x7fELF\x01\x01", "expected little-endian ELF32 object"
    e_shoff, = struct.unpack_from("<I", blob, 32)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", blob, 46)
    headers = [struct.unpack_from("<10I", blob, e_shoff + i * e_shentsize)
               for i in range(e_shnum)]
    names_header = headers[e_shstrndx]
    names = blob[names_header[4]:names_header[4] + names_header[5]]
    out = {}
    for header in headers:
        end = names.index(0, header[0])
        name = names[header[0]:end].decode()
        out[name] = blob[header[4]:header[4] + header[5]]
    return out


def descriptors(data):
    pos = 0
    while pos < len(data):
        length = data[pos]
        assert length >= 2 and pos + length <= len(data), f"bad descriptor at {pos}"
        yield data[pos:pos + length]
        pos += length
    assert pos == len(data)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("object", help="compiled usb_descriptors.c.obj")
    parser.add_argument("--sdkconfig", help="build config header containing the fixed sample rate")
    args = parser.parse_args()
    parts = sections(args.object)
    config = parts[".rodata.config_desc"]
    device = parts[".rodata.device_desc"]
    assert len(device) == 18 and device[1] == 1 and device[4:7] == bytes([0xEF, 2, 1])
    assert config[0:2] == bytes([9, 2])
    assert int.from_bytes(config[2:4], "little") == len(config)
    assert config[4] == 4, "UAC AC+AS and CDC control+data interfaces"

    desc = list(descriptors(config))
    interfaces = [(d[2], d[3], d[5:8]) for d in desc if d[1] == 4]
    assert interfaces == [
        (0, 0, bytes([1, 1, 0x20])),
        (1, 0, bytes([1, 2, 0x20])),
        (1, 1, bytes([1, 2, 0x20])),
        (2, 0, bytes([2, 2, 0])),
        (3, 0, bytes([10, 0, 0])),
    ], interfaces
    iads = [(d[2], d[3]) for d in desc if d[1] == 11]
    assert iads == [(0, 2), (2, 2)], iads
    endpoints = [(d[2], d[3] & 3, int.from_bytes(d[4:6], "little"))
                 for d in desc if d[1] == 5]
    assert endpoints == [(0x81, 1, 34), (0x82, 3, 8),
                         (0x01, 2, 64), (0x83, 2, 64)], endpoints
    # UAC2 input terminal -> feature unit -> output terminal path, mic only.
    entities = {d[3]: d for d in desc if d[1] == 0x24 and d[2] in (2, 3, 6) and len(d) >= 12}
    assert entities[1][2] == 2 and entities[2][2] == 6 and entities[3][2] == 3
    assert entities[2][4] == 1 and entities[3][7] == 2
    assert bytes([6, 0x24, 2, 1, 2, 16]) in desc, "PCM 16-bit 1-channel format"
    if args.sdkconfig:
        config_text = Path(args.sdkconfig).read_text()
        assert "#define CONFIG_UAC_SAMPLE_RATE 16000" in config_text
    print("USB descriptor PASS: UAC2 16kHz/16-bit mono IN + CDC ACM, 4 interfaces")


if __name__ == "__main__":
    main()
