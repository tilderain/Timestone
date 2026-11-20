#!/usr/bin/env python3
"""Builds the lobby backdrop's star tile from its source picture.

    tools/make_space_stars.py [source.png] [out.dxt1] [--gain 1.45] [--preview out.png]

The source (default UMVC3Hook/assets/space_stars_source.png) must tile
seamlessly in both directions. It is scaled to 512x512 without breaking the
seam, brightened by --gain, and written as raw DXT1 blocks with no header
(128 x 128 blocks of 8 bytes), which is what UMVC3Hook/code/space_bg.cpp
decodes and the Makefile embeds as the SPACE_STARS resource. Rebuild the
plugin afterwards.

Needs numpy and Pillow.
"""
import argparse
import os

import numpy as np
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
ASSETS = os.path.join(ROOT, 'UMVC3Hook', 'assets')
TILE = 512  # kSpaceStarsTile in space_bg.h


def seamless_resize(rgb, size):
    """Lanczos resize of a tiling image that keeps its edges matching."""
    height, width = rgb.shape[:2]
    pad_y, pad_x = height // 16, width // 16
    padded = np.pad(rgb, ((pad_y, pad_y), (pad_x, pad_x), (0, 0)), mode='wrap')
    out_pad = size // 16
    scaled = Image.fromarray(padded).resize((size + 2 * out_pad, size + 2 * out_pad), Image.LANCZOS)
    return np.asarray(scaled.crop((out_pad, out_pad, out_pad + size, out_pad + size)))


def pack565(colour):
    return ((colour[:, 0] >> 3) << 11) | ((colour[:, 1] >> 2) << 5) | (colour[:, 2] >> 3)


def unpack565(value):
    r, g, b = (value >> 11) & 31, (value >> 5) & 63, value & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1)


def encode_dxt1(rgb):
    """Range-fit DXT1, always in four-colour mode (c0 >= c1), as the plugin's decoder expects."""
    height, width = rgb.shape[:2]
    pixels = rgb.astype(np.int32)
    blocks = pixels.reshape(height // 4, 4, width // 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 3)
    c0, c1 = pack565(blocks.max(1)), pack565(blocks.min(1))
    swap = c0 < c1
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    e0, e1 = unpack565(c0), unpack565(c1)
    palette = np.stack([e0, e1, (2 * e0 + e1) // 3, (e0 + 2 * e1) // 3], 1)
    index = ((blocks[:, :, None, :] - palette[:, None, :, :]) ** 2).sum(-1).argmin(-1).astype(np.uint32)
    index[c0 == c1] = 0
    bits = (index << (2 * np.arange(16, dtype=np.uint32))[None, :]).sum(1).astype(np.uint32)
    out = np.zeros((len(blocks), 8), np.uint8)
    out[:, 0], out[:, 1], out[:, 2], out[:, 3] = c0 & 255, c0 >> 8, c1 & 255, c1 >> 8
    for byte in range(4):
        out[:, 4 + byte] = (bits >> (8 * byte)) & 255
    return out, palette, index


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('source', nargs='?', default=os.path.join(ASSETS, 'space_stars_source.png'))
    parser.add_argument('output', nargs='?', default=os.path.join(ASSETS, 'space_stars_512.dxt1'))
    parser.add_argument('--gain', type=float, default=1.45, help='brightness multiplier (default 1.45)')
    parser.add_argument('--preview', help='also write the tile as the plugin will decode it, as a PNG')
    args = parser.parse_args()

    source = np.asarray(Image.open(args.source).convert('RGB'))
    if source.shape[0] % 16 or source.shape[1] % 16:
        parser.error('source sides must be multiples of 16')
    tile = seamless_resize(source, TILE)
    tile = np.clip(tile.astype(np.float32) * args.gain, 0, 255).astype(np.uint8)
    blocks, palette, index = encode_dxt1(tile)
    with open(args.output, 'wb') as file:
        file.write(blocks.tobytes())
    print('wrote %s (%d bytes) from %s' % (os.path.relpath(args.output), blocks.nbytes,
                                           os.path.relpath(args.source)))
    if args.preview:
        decoded = np.take_along_axis(palette, index[:, :, None].astype(np.int64), 1)
        image = decoded.reshape(TILE // 4, TILE // 4, 4, 4, 3).transpose(0, 2, 1, 3, 4)
        Image.fromarray(image.reshape(TILE, TILE, 3).astype(np.uint8)).save(args.preview)
        print('wrote', args.preview)


if __name__ == '__main__':
    main()
