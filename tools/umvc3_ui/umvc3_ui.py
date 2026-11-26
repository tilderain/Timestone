#!/usr/bin/env python3
"""Offline tools for UMVC3's native UI resources.

The native lobby UI (doc/native_lobby_ui.md) is drawn with the game's own
sprite, frame and font units. These tools read the same resources from a
user's installed game so layouts can be inspected and previewed without
launching it. Nothing extracted here belongs in the repository: the cache
directory defaults to build/ui_cache, which is ignored.

  extract [game-dir] [cache]      unpack the UI archives the lobby uses
  find-game                       print the game folder found in the Steam libraries
  list <file.arc>                 list an archive's entries
  sdl <file.sdl> [frame]          summarise a scheduler layout
  render-sdl <file.sdl> <out.png> [frame] [cache]
  render <commands.json> <out.png> [cache]

Formats (PC, MT Framework 2.x as shipped with UMVC3):
  ARC  'ARC\\0', u16 version, u16 count, then 80-byte entries
       (64-byte path, u32 type hash, u32 packed size, u32 size|flags, u32 offset);
       payloads are zlib streams.
  TEX  'TEX\\0', u32 version word, u32 mips:6|width:13|height:13,
       u32 count:8|format:8, mip offsets, data. Format 0x13 = DXT1,
       0x17 = DXT5, 0x2A = DXT5 holding Y in alpha, Cr in red, Cb in blue
       and the real alpha in green.
  SDL  'SDL\\0', u16 version, u16 trackCount+1, ... u64 stringTable at +0x18,
       48-byte tracks from +0x50. A unit track (kind 2) carries the class DTI
       hash; property tracks carry type, key count, name offset and offsets
       to the key headers (frame:24 | mode:8) and key values.
  FSD  font metrics: 28-byte header, then one u32 advance per glyph. Glyph
       index for U+0020..U+007E is codepoint - 0x20.
"""
import glob
import json
import math
import os
import re
import struct
import sys
import zlib

TYPE_TEX = 0x241F5DEB
TYPE_SDL = 0x4C0DB839
TYPE_FSD = 0x1DB58B09
UNIT_KINDS = {0x7942324B: 'sprite', 0x4AF926B7: 'frame', 0x518E722D: 'text'}
# Archives the lobby draws from, relative to nativePCx64/ui.
LOBBY_ARCHIVES = ('cmn', 'netroom', 'mnmain', 'lic', 'mnchsstg', 'mnchs')
# Of the license archive only the icon set is used (character portraits),
# of the stage select archive only the stage icons, and of the character
# select archive only the stage select's arrow.
ARCHIVE_FILTER = {'lic': 'lic_user_i', 'mnchsstg': 'chs_stgm', 'mnchs': 'chs_opt_sel_b'}
UNDERLAY = 'meku_menu_co00_BM_NOMIP'
DEFAULT_CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'build', 'ui_cache')


# --------------------------------------------------------------------------
# The game folder
# --------------------------------------------------------------------------
STEAM_APP_ID = '357190'
DEFAULT_INSTALL_DIR = 'ULTIMATE MARVEL VS. CAPCOM 3'


def _vdf_values(text, key):
    """Every value written straight after the quoted key, as Steam's text
    KeyValues files have them."""
    return [value.replace('\\\\', '\\') for value in
            re.findall(r'"%s"\s+"((?:[^"\\]|\\.)*)"' % re.escape(key), text)]


def _read_text(path):
    try:
        with open(path, encoding='utf-8', errors='replace') as file:
            return file.read()
    except OSError:
        return ''


def find_game():
    """The folder holding umvc3.exe, or None: looked for the way the Linux
    installer does (tools/timestone_install_linux.cpp), in every Steam
    library, after the library this repository is itself in, if it is in one."""
    libraries = []

    def add(path):
        path = os.path.realpath(path)
        if path not in libraries:
            libraries.append(path)

    here = os.path.dirname(os.path.abspath(__file__))
    while os.path.dirname(here) != here:
        if os.path.basename(here) == 'steamapps':
            add(os.path.dirname(here))
            break
        here = os.path.dirname(here)
    home = os.environ.get('HOME', '')
    if home:
        for place in ('.steam/steam', '.local/share/Steam',
                      '.var/app/com.valvesoftware.Steam/.local/share/Steam',  # Flatpak
                      'snap/steam/common/.local/share/Steam',                 # Snap
                      '.steam/root'):
            steam = os.path.join(home, place)
            add(steam)
            for listing in ('config/libraryfolders.vdf', 'steamapps/libraryfolders.vdf'):
                for path in _vdf_values(_read_text(os.path.join(steam, listing)), 'path'):
                    add(path)
    for library in libraries:
        apps = os.path.join(library, 'steamapps')
        manifest = _read_text(os.path.join(apps, 'appmanifest_%s.acf' % STEAM_APP_ID))
        for folder in _vdf_values(manifest, 'installdir') + [DEFAULT_INSTALL_DIR]:
            candidate = os.path.join(apps, 'common', folder)
            if os.path.isfile(os.path.join(candidate, 'umvc3.exe')):
                return candidate
    return None


# --------------------------------------------------------------------------
# ARC
# --------------------------------------------------------------------------
def arc_entries(path):
    data = open(path, 'rb').read()
    magic, _version, count = struct.unpack_from('<4sHH', data, 0)
    if magic != b'ARC\0':
        raise ValueError('%s is not an ARC archive' % path)
    entries = []
    for index in range(count):
        offset = 8 + index * 80
        name = data[offset:offset + 64].split(b'\0')[0].decode('latin1')
        type_hash, packed, size, start = struct.unpack_from('<IIII', data, offset + 64)
        entries.append((name, type_hash, packed, size & 0x1FFFFFFF, start))
    return data, entries


def arc_extract(path, wanted_types):
    data, entries = arc_entries(path)
    for name, type_hash, packed, _size, start in entries:
        if type_hash in wanted_types:
            yield name, type_hash, zlib.decompress(data[start:start + packed])


# --------------------------------------------------------------------------
# TEX
# --------------------------------------------------------------------------
def _dxt_color_blocks(blocks, dxt1):
    import numpy as np
    c0 = blocks[:, 0].astype(np.uint32) | (blocks[:, 1].astype(np.uint32) << 8)
    c1 = blocks[:, 2].astype(np.uint32) | (blocks[:, 3].astype(np.uint32) << 8)

    def rgb(c):
        return np.stack([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63,
                         (c & 31) * 255 // 31], -1).astype(np.int32)

    r0, r1 = rgb(c0), rgb(c1)
    palette = np.zeros((len(blocks), 4, 4), np.int32)
    palette[:, :, 3] = 255
    palette[:, 0, :3] = r0
    palette[:, 1, :3] = r1
    if dxt1:
        greater = (c0 > c1)[:, None]
        palette[:, 2, :3] = np.where(greater, (2 * r0 + r1) // 3, (r0 + r1) // 2)
        palette[:, 3, :3] = np.where(greater, (r0 + 2 * r1) // 3, 0)
        palette[:, 3, 3] = np.where(c0 > c1, 255, 0)
    else:
        palette[:, 2, :3] = (2 * r0 + r1) // 3
        palette[:, 3, :3] = (r0 + 2 * r1) // 3
    indices = np.zeros((len(blocks), 16), np.int32)
    for row in range(4):
        bits = blocks[:, 4 + row].astype(np.int32)
        for col in range(4):
            indices[:, row * 4 + col] = (bits >> (2 * col)) & 3
    return np.take_along_axis(palette, indices[:, :, None].repeat(4, 2), 1)


def _dxt_alpha_blocks(blocks):
    import numpy as np
    a0 = blocks[:, 0].astype(np.int32)
    a1 = blocks[:, 1].astype(np.int32)
    palette = np.zeros((len(blocks), 8), np.int32)
    palette[:, 0] = a0
    palette[:, 1] = a1
    greater = a0 > a1
    for i in range(1, 7):
        six = ((5 - i) * a0 + i * a1) // 5 if i < 5 else (0 if i == 5 else 255)
        palette[:, i + 1] = np.where(greater, ((7 - i) * a0 + i * a1) // 7, six)
    bits = np.zeros(len(blocks), np.uint64)
    for i in range(6):
        bits |= blocks[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    indices = np.zeros((len(blocks), 16), np.int32)
    for i in range(16):
        indices[:, i] = ((bits >> np.uint64(3 * i)) & np.uint64(7)).astype(np.int32)
    return np.take_along_axis(palette, indices, 1)


# The formats tex_decode reads.
TEX_FORMATS = frozenset((0x13, 0x14, 0x19, 0x17, 0x1F, 0x2A, 0x20, 0x21, 0x23, 0x25))


def tex_header(data):
    """(width, height, format) from the first 16 bytes of a TEX resource, or None."""
    if len(data) < 16 or data[:4] != b'TEX\0':
        return None
    _word, dims, info = struct.unpack_from('<III', data, 4)
    return (dims >> 6) & 0x1FFF, (dims >> 19) & 0x1FFF, (info >> 8) & 0xFF


def tex_decode(data, step=1):
    """Returns a PIL RGBA image for mip 0 of a TEX resource. With `step`
    above 1 only every step-th 4x4 block each way is read: a rough small
    copy, made that many times squared faster, for thumbnails."""
    import numpy as np
    from PIL import Image
    if data[:4] != b'TEX\0':
        raise ValueError('not a TEX resource')
    _word, dims, info = struct.unpack_from('<III', data, 4)
    width, height = (dims >> 6) & 0x1FFF, (dims >> 19) & 0x1FFF
    fmt = (info >> 8) & 0xFF
    start = struct.unpack_from('<I', data, 16)[0]
    pixels = np.frombuffer(data, np.uint8, offset=start)
    bw, bh = (width + 3) // 4, (height + 3) // 4
    if fmt in (0x13, 0x14, 0x19):
        blocks = pixels[:bw * bh * 8].reshape(bh, bw, 8)[::step, ::step]
        bh, bw = blocks.shape[:2]
        texels = _dxt_color_blocks(blocks.reshape(-1, 8), True)
    elif fmt in (0x17, 0x1F, 0x2A, 0x20, 0x21, 0x23, 0x25):
        blocks = pixels[:bw * bh * 16].reshape(bh, bw, 16)[::step, ::step]
        bh, bw = blocks.shape[:2]
        blocks = blocks.reshape(-1, 16)
        texels = _dxt_color_blocks(blocks[:, 8:], False)
        texels[:, :, 3] = _dxt_alpha_blocks(blocks[:, :8])
    else:
        raise ValueError('unsupported TEX format 0x%02X' % fmt)
    if fmt == 0x2A:
        luma = texels[:, :, 3].astype(np.float32)
        cr = texels[:, :, 0].astype(np.float32) - 128
        cb = texels[:, :, 2].astype(np.float32) - 128
        alpha = texels[:, :, 1].copy()
        texels = np.stack([luma + 1.402 * cr, luma - 0.344 * cb - 0.714 * cr,
                           luma + 1.772 * cb, alpha], -1).clip(0, 255).astype(np.int32)
    image = texels.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    if step > 1:
        width, height = bw * 4, bh * 4
    return Image.fromarray(image[:height, :width].astype(np.uint8), 'RGBA')


def extract(game_dir, cache):
    """Unpacks the lobby's archives: textures as PNG, layouts and metrics raw."""
    ui_dir = os.path.join(game_dir, 'nativePCx64', 'ui')
    os.makedirs(cache, exist_ok=True)
    counts = {'png': 0, 'sdl': 0, 'fsd': 0}
    for archive in LOBBY_ARCHIVES:
        path = os.path.join(ui_dir, archive + '.arc')
        if not os.path.exists(path):
            print('missing', path)
            continue
        wanted = ARCHIVE_FILTER.get(archive)
        for name, type_hash, payload in arc_extract(path, (TYPE_TEX, TYPE_SDL, TYPE_FSD)):
            if wanted and wanted not in name:
                continue
            base = name.replace('\\', '/').split('/')[-1]
            if type_hash == TYPE_TEX:
                try:
                    tex_decode(payload).save(os.path.join(cache, base + '.png'))
                    counts['png'] += 1
                except ValueError as error:
                    print('skip %s: %s' % (name, error))
            else:
                extension = '.sdl' if type_hash == TYPE_SDL else '.fsd'
                open(os.path.join(cache, base + extension), 'wb').write(payload)
                counts['sdl' if type_hash == TYPE_SDL else 'fsd'] += 1
    # Character select faces are loose TEX files, one per character and color.
    faces = os.path.join(ui_dir, 'chs', 'chs_face_a', 'chs_cs_f')
    if os.path.isdir(faces):
        for name in sorted(os.listdir(faces)):
            # (and the roster's RANDOM cells, "typeC" art; the plain name is English)
            if not name.endswith(('_BM_HQ_NOMIP.tex', '_BM_HQ_NOMIP_typeC.tex')):
                continue
            try:
                image = tex_decode(open(os.path.join(faces, name), 'rb').read())
                image.save(os.path.join(cache, name[:-4] + '.png'))
                counts['png'] += 1
            except ValueError as error:
                print('skip %s: %s' % (name, error))
    # The "next battle" cards the pre-match prompt shows: one archive per
    # character and color, holding the Japanese art and "_other" for every
    # other language. The cache keeps the latter under the plain name.
    cards = os.path.join(ui_dir, 'res', 'arc', 'chr')
    if os.path.isdir(cards):
        for archive in sorted(glob.glob(os.path.join(cards, 'l[0-9]*.arc'))):
            for name, _type_hash, payload in arc_extract(archive, (TYPE_TEX,)):
                base = name.replace('\\', '/').split('/')[-1]
                if not base.endswith('_typeB_other'):
                    continue
                try:
                    tex_decode(payload).save(os.path.join(cache, base[:-len('_typeB_other')] + '.png'))
                    counts['png'] += 1
                except ValueError as error:
                    print('skip %s: %s' % (name, error))
    print('extracted %(png)d textures, %(sdl)d layouts, %(fsd)d font metric tables' % counts,
          'to', os.path.abspath(cache))


# --------------------------------------------------------------------------
# SDL
# --------------------------------------------------------------------------
def _cstring(data, offset):
    return data[offset:data.index(b'\0', offset)].decode('latin1')


def sdl_parse(path, max_keys=64):
    """Returns [{name, dti, index, props: {name: (frames, values)}}]."""
    data = open(path, 'rb').read()
    if data[:4] != b'SDL\0':
        raise ValueError('%s is not an SDL scheduler' % path)
    count = struct.unpack_from('<H', data, 6)[0] - 1
    strings = struct.unpack_from('<Q', data, 0x18)[0]
    units = []
    for index in range(count):
        offset = 0x50 + index * 0x30
        kind, ptype, keys, _parent, name_offset, _pad, dti = struct.unpack_from('<BBHIIII', data, offset)
        if kind == 0:
            break
        name = _cstring(data, strings + name_offset)
        if kind in (1, 2):
            units.append({'name': name, 'dti': dti, 'index': index, 'props': {}})
            continue
        key_offset, value_offset = struct.unpack_from('<QQ', data, offset + 0x20)
        keys = min(keys, max_keys)
        frames = [word & 0xFFFFFF for word in struct.unpack_from('<%dI' % keys, data, key_offset)]
        values = []
        for key in range(keys):
            if ptype in (0x13, 0x14, 0x15, 0x0F, 0x22):
                values.append(struct.unpack_from('<4f', data, value_offset + 16 * key))
            elif ptype in (0x0C, 0x0D):
                values.append(struct.unpack_from('<f', data, value_offset + 4 * key)[0])
            elif ptype == 0x80:
                pointer = struct.unpack_from('<Q', data, value_offset + 8 * key)[0]
                try:
                    values.append(_cstring(data, strings + pointer))
                except ValueError:
                    values.append('')
            elif ptype == 0x03:
                values.append(data[value_offset + key])
            else:
                values.append(struct.unpack_from('<i', data, value_offset + 4 * key)[0])
        units[-1]['props'][name] = (frames, values)
    return units


def _value_at(prop, frame):
    frames, values = prop
    value = values[0]
    for key_frame, key_value in zip(frames, values):
        if key_frame <= frame:
            value = key_value
    return value


def sdl_commands(path, frame=0):
    """Flattens a scheduler into render commands at the given frame."""
    units = sdl_parse(path)
    by_index = {unit['index']: unit for unit in units}

    def prop(unit, name, default=None):
        return _value_at(unit['props'][name], frame) if name in unit['props'] else default

    def world(unit):
        pos = prop(unit, 'mPos', (0, 0, 0, 0))
        scale = prop(unit, 'mScale', (1, 1, 1, 0))
        parent = prop(unit, 'mpParent', 0)
        if parent and parent - 1 in by_index:
            px, py, sx, sy = world(by_index[parent - 1])
            return px + pos[0] * sx, py + pos[1] * sy, sx * scale[0], sy * scale[1]
        return pos[0], pos[1], scale[0], scale[1]

    commands = []
    for unit in units:
        kind = UNIT_KINDS.get(unit['dti'])
        if kind is None:
            continue
        x, y, sx, sy = world(unit)
        command = {'type': kind, 'pos': [x, y], 'scale': [sx, sy],
                   'anchor': prop(unit, 'mCenterAttr', 4), 'depth': prop(unit, 'mDepth', 0),
                   'color': list(prop(unit, 'mColor', (1, 1, 1, 1))), 'name': unit['name']}
        if kind == 'text':
            command['str'] = unit['name']
        else:
            texture = prop(unit, 'LocalizeBaseTexture')
            if not texture:
                continue
            size = prop(unit, 'mSize', (0, 0, 0, 0))
            uv0, uv1 = prop(unit, 'mUV0', (0, 0, 0, 0)), prop(unit, 'mUV1', (1, 1, 0, 0))
            command.update(tex=texture, size=[size[0], size[1]], uv=[uv0[0], uv0[1], uv1[0], uv1[1]])
            command['scale'] = [sx, sy]
        commands.append(command)
    return commands


def sdl_summary(path, frame=0):
    for unit in sdl_parse(path):
        kind = UNIT_KINDS.get(unit['dti'], '%08X' % unit['dti'])
        props = unit['props']

        def first(name):
            return _value_at(props[name], frame) if name in props else None

        pos, size = first('mPos'), first('mSize')
        animated = [name for name, (frames, _values) in props.items() if len(frames) > 1]
        print('%4d %-7s %-24s pos=%s size=%s anchor=%s depth=%s tex=%s%s' % (
            unit['index'], kind, unit['name'], pos and tuple(round(v, 1) for v in pos[:2]),
            size and tuple(round(v, 1) for v in size[:2]), first('mCenterAttr'), first('mDepth'),
            str(first('LocalizeBaseTexture') or '').split('\\')[-1],
            ' anim=' + ','.join(animated) if animated else ''))


# --------------------------------------------------------------------------
# Renderer (1280x720 UI space, same primitives as lobby::Canvas)
# --------------------------------------------------------------------------
# uUiAnmSpriteFont font types: (texture, glyph cell, atlas columns). 1..3 hold
# U+0020..U+005F only (no lower case).
FONTS = {0: ('nrodinpro_BM_HQ_NOMIP', 32, 32), 1: ('acme_BM_NOMIP', 32, 16),
         2: ('bada_BM_NOMIP', 64, 16), 3: ('news_BM_NOMIP', 64, 16),
         4: ('bada_b_BM_HQ_NOMIP', 64, 16)}


# The flag texture that hosts the plugin's star image in game (TEX_SPACE_STARS),
# and the image as the plugin builds it: the embedded tile, 2 x 2.
SPACE_STARS_HOST = 'lic_flag_germany_BM_HQ_NOMIP'
SPACE_STARS_TILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..',
                                'UMVC3Hook', 'assets', 'space_stars_512.dxt1')


def space_stars_texture():
    import numpy as np
    from PIL import Image
    blocks = np.frombuffer(open(SPACE_STARS_TILE, 'rb').read(), np.uint8).reshape(-1, 8)
    texels = _dxt_color_blocks(blocks, True)
    tile = texels.reshape(128, 128, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(512, 512, 4)
    tile = tile.astype(np.uint8).copy()
    tile[:, :, 3] = 255
    return Image.fromarray(np.tile(tile, (2, 2, 1)), 'RGBA')


class Renderer:
    def __init__(self, cache):
        self.cache = cache
        self.textures = {}
        self.fonts = {}

    def texture(self, name):
        base = name.replace('\\', '/').split('/')[-1]
        if base == SPACE_STARS_HOST and base not in self.textures:
            self.textures[base] = space_stars_texture()
        if base not in self.textures:
            from PIL import Image
            path = os.path.join(self.cache, base + '.png')
            if not os.path.exists(path):
                raise KeyError('texture not in cache: ' + base)
            self.textures[base] = Image.open(path).convert('RGBA')
        return self.textures[base]

    def font(self, kind):
        if kind not in self.fonts:
            name, cell, columns = FONTS[kind]
            data = open(os.path.join(self.cache, name + '.fsd'), 'rb').read()
            widths = struct.unpack_from('<%dI' % ((len(data) - 0x1C) // 4), data, 0x1C)
            self.fonts[kind] = (self.texture(name), widths, cell, columns)
        return self.fonts[kind]

    @staticmethod
    def anchor_rect(width, height, anchor):
        # uBaseSprite::move: row = anchor / 3, column = anchor % 3.
        right = width * (2 - anchor % 3) / 2
        bottom = height * (2 - anchor // 3) / 2
        return right - width, bottom - height

    @staticmethod
    def crop(texture, uv):
        width, height = texture.size
        box = [int(round(uv[0] * width)), int(round(uv[1] * height)),
               int(round(uv[2] * width)), int(round(uv[3] * height))]
        flip_x, flip_y = box[0] > box[2], box[1] > box[3]
        box = (min(box[0], box[2]), min(box[1], box[3]), max(box[0], box[2]), max(box[1], box[3]))
        if box[2] <= box[0] or box[3] <= box[1]:
            return None
        from PIL import Image
        image = texture.crop(box)
        if flip_x:
            image = image.transpose(Image.FLIP_LEFT_RIGHT)
        if flip_y:
            image = image.transpose(Image.FLIP_TOP_BOTTOM)
        return image

    @staticmethod
    def blit(canvas, image, x, y, width, height, color):
        import numpy as np
        from PIL import Image
        if image is None or width <= 0.5 or height <= 0.5:
            return
        image = image.resize((max(1, int(round(width))), max(1, int(round(height)))), Image.BILINEAR)
        if color is not None:
            pixels = np.asarray(image).astype(np.float32) * np.array(color, np.float32)[None, None, :]
            image = Image.fromarray(pixels.clip(0, 255).astype(np.uint8), 'RGBA')
        px, py = int(round(x)), int(round(y))
        left, top = max(0, -px), max(0, -py)
        right, bottom = min(image.width, canvas.width - px), min(image.height, canvas.height - py)
        if right > left and bottom > top:
            canvas.alpha_composite(image.crop((left, top, right, bottom)), (px + left, py + top))

    def sprite(self, canvas, command):
        texture = self.texture(command['tex'])
        sx, sy = command.get('scale', (1, 1))
        width, height = command['size'][0] * sx, command['size'][1] * sy
        image = self.crop(texture, command.get('uv', (0, 0, 1, 1)))
        turn = command.get('turn')
        if turn and image is not None and width > 0.5 and height > 0.5:
            # Canvas::SpriteTurned: centred, a positive turn is clockwise on
            # screen (PIL's is the other way).
            from PIL import Image
            image = image.resize((max(1, int(round(width))), max(1, int(round(height)))), Image.BILINEAR)
            image = image.rotate(-math.degrees(turn), Image.BILINEAR, expand=True)
            width, height = image.size
        left, top = self.anchor_rect(width, height, command.get('anchor', 4))
        self.blit(canvas, image,
                  command['pos'][0] + left, command['pos'][1] + top, width, height, command.get('color'))

    def frame(self, canvas, command):
        # uUiAnmSpriteFrame: the UV window's outer thirds frame a centre of
        # mSize; 'tileframe' repeats the centre cell (mFrameAttr bit 0).
        texture = self.texture(command['tex'])
        uv = command.get('uv', (0, 0, 1, 1))
        us = [uv[0] + (uv[2] - uv[0]) * t for t in (0, 1 / 3, 2 / 3, 1)]
        vs = [uv[1] + (uv[3] - uv[1]) * t for t in (0, 1 / 3, 2 / 3, 1)]
        sx, sy = command.get('scale', (1, 1))
        border_x = texture.width * (us[1] - us[0])
        border_y = texture.height * (vs[1] - vs[0])
        inner_w, inner_h = command['size']
        total_w, total_h = (2 * border_x + inner_w) * sx, (2 * border_y + inner_h) * sy
        left, top = self.anchor_rect(total_w, total_h, command.get('anchor', 4))
        x, y = command['pos'][0] + left, command['pos'][1] + top
        xs = [x, x + border_x * sx, x + (border_x + inner_w) * sx, x + total_w]
        ys = [y, y + border_y * sy, y + (border_y + inner_h) * sy, y + total_h]
        tile = command['type'] == 'tileframe'
        for row in range(3):
            for col in range(3):
                piece = self.crop(texture, (us[col], vs[row], us[col + 1], vs[row + 1]))
                if piece is None:
                    continue
                if not tile or (row != 1 and col != 1):
                    self.blit(canvas, piece, xs[col], ys[row], xs[col + 1] - xs[col],
                              ys[row + 1] - ys[row], command.get('color'))
                    continue
                cell_w = piece.width * sx if col == 1 else xs[col + 1] - xs[col]
                cell_h = piece.height * sy if row == 1 else ys[row + 1] - ys[row]
                cy = ys[row]
                while cy < ys[row + 1] - 0.01:
                    part_h = min(cell_h, ys[row + 1] - cy)
                    cx = xs[col]
                    while cx < xs[col + 1] - 0.01:
                        part_w = min(cell_w, xs[col + 1] - cx)
                        part = piece.crop((0, 0, max(1, int(round(piece.width * part_w / cell_w))),
                                           max(1, int(round(piece.height * part_h / cell_h)))))
                        self.blit(canvas, part, cx, cy, part_w, part_h, command.get('color'))
                        cx += cell_w
                    cy += cell_h

    def text(self, canvas, command):
        texture, widths, cell, columns = self.font(command.get('font', 0))
        sx, sy = command.get('scale', (1, 1))
        # Only Basic Latin is laid out here; the game handles the rest itself.
        glyphs = [ord(ch) - 0x20 if 0x20 <= ord(ch) < 0x7F else 0x1F for ch in command['str']]
        total = sum(widths[glyph] for glyph in glyphs) * sx
        left, top = self.anchor_rect(total, cell * sy, command.get('anchor', 3))
        x, y = command['pos'][0] + left, command['pos'][1] + top
        for glyph in glyphs:
            gx, gy = (glyph % columns) * cell, (glyph // columns) * cell
            advance = widths[glyph] * sx
            self.blit(canvas, texture.crop((gx, gy, gx + cell, gy + cell)),
                      x - (cell * sx - advance) / 2, y, cell * sx, cell * sy, command.get('color'))
            x += advance

    def bounds(self, command):
        """The rectangle a command covers, as (left, top, right, bottom), or
        None for one that has none worth pointing at (a line). A turned
        sprite gives the rectangle it would cover unturned."""
        try:
            kind = command['type']
            sx, sy = command.get('scale', (1, 1))
            if kind == 'sprite':
                width, height = command['size'][0] * sx, command['size'][1] * sy
            elif kind in ('frame', 'tileframe'):
                texture = self.texture(command['tex'])
                uv = command.get('uv', (0, 0, 1, 1))
                width = (2 * texture.width * (uv[2] - uv[0]) / 3 + command['size'][0]) * sx
                height = (2 * texture.height * (uv[3] - uv[1]) / 3 + command['size'][1]) * sy
            elif kind == 'text':
                _texture, widths, cell, _columns = self.font(command.get('font', 0))
                width = sum(widths[ord(ch) - 0x20 if 0x20 <= ord(ch) < 0x7F else 0x1F]
                            for ch in command['str']) * sx
                height = cell * sy
            else:
                return None
        except (KeyError, OSError):
            return None
        left, top = self.anchor_rect(width, height,
                                     command.get('anchor', 3 if kind == 'text' else 4))
        x, y = command['pos'][0] + left, command['pos'][1] + top
        return (x, y, x + width, y + height)

    def backdrop(self, size=(1280, 720)):
        """What the commands are drawn over."""
        from PIL import Image
        canvas = Image.new('RGBA', size, (8, 8, 16, 255))
        # Stand-in for the live title art the lobby is drawn over in game.
        try:
            canvas.alpha_composite(self.texture(UNDERLAY).convert('RGBA').resize(size))
        except (KeyError, OSError):
            pass
        return canvas

    def render(self, commands, out_path, size=(1280, 720)):
        canvas = self.backdrop(size)
        missing = self.draw(canvas, commands)
        canvas.convert('RGB').save(out_path)
        for item in sorted(missing):
            print('warning:', item)

    def draw(self, canvas, commands):
        """Draws the commands onto `canvas`; returns the textures it lacked."""
        ordered = sorted(enumerate(commands), key=lambda item: (-item[1].get('depth', 0), item[0]))
        missing = set()
        for _index, command in ordered:
            color = command.get('color')
            if color is not None and color[3] <= 0.003:
                continue
            try:
                if command['type'] == 'sprite':
                    self.sprite(canvas, command)
                elif command['type'] in ('frame', 'tileframe'):
                    self.frame(canvas, command)
                elif command['type'] == 'text':
                    self.text(canvas, command)
                elif command['type'] == 'line':
                    # Drawn four times the size and scaled down, for edges as
                    # soft as the game's own filtering gives a thin quad.
                    from PIL import Image, ImageDraw
                    box = [min(command['from'][0], command['to'][0]) - 4,
                           min(command['from'][1], command['to'][1]) - 4,
                           max(command['from'][0], command['to'][0]) + 4,
                           max(command['from'][1], command['to'][1]) + 4]
                    box = [int(box[0]), int(box[1]), int(box[2]) + 1, int(box[3]) + 1]
                    w, h = box[2] - box[0], box[3] - box[1]
                    layer = Image.new('RGBA', (w * 4, h * 4), (0, 0, 0, 0))
                    rgba = tuple(int(max(0.0, min(1.0, c)) * 255) for c in command['color'])
                    ImageDraw.Draw(layer).line(
                        [((command['from'][0] - box[0]) * 4, (command['from'][1] - box[1]) * 4),
                         ((command['to'][0] - box[0]) * 4, (command['to'][1] - box[1]) * 4)],
                        fill=rgba, width=max(1, int(round(command['width'] * 4))))
                    # Only the part of the box that is on the canvas.
                    left, top = max(0, box[0]), max(0, box[1])
                    right, bottom = min(canvas.width, box[2]), min(canvas.height, box[3])
                    if right > left and bottom > top:
                        canvas.alpha_composite(
                            layer.resize((w, h), Image.LANCZOS), (left, top),
                            (left - box[0], top - box[1], right - box[0], bottom - box[1]))
            except KeyError as error:
                missing.add(str(error))
        return missing


def main(argv):
    if len(argv) < 2 or argv[1] in ('-h', '--help'):
        print(__doc__)
        return 0
    command = argv[1]
    if command == 'find-game':
        game = find_game()
        if not game:
            print('Ultimate Marvel vs. Capcom 3 was not found in the Steam libraries',
                  file=sys.stderr)
            return 1
        print(game)
    elif command == 'extract':
        game = argv[2] if len(argv) > 2 else find_game()
        if not game:
            print('give the game folder: it was not found in the Steam libraries', file=sys.stderr)
            return 1
        extract(game, argv[3] if len(argv) > 3 else DEFAULT_CACHE)
    elif command == 'list' and len(argv) >= 3:
        for name, type_hash, packed, size, _start in arc_entries(argv[2])[1]:
            print('%-64s %08X packed=%d size=%d' % (name, type_hash, packed, size))
    elif command == 'sdl' and len(argv) >= 3:
        sdl_summary(argv[2], int(argv[3]) if len(argv) > 3 else 0)
    elif command == 'render-sdl' and len(argv) >= 4:
        frame = int(argv[4]) if len(argv) > 4 else 0
        cache = argv[5] if len(argv) > 5 else DEFAULT_CACHE
        Renderer(cache).render(sdl_commands(argv[2], frame), argv[3])
    elif command == 'render' and len(argv) >= 4:
        cache = argv[4] if len(argv) > 4 else DEFAULT_CACHE
        Renderer(cache).render(json.load(open(argv[2])), argv[3])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
