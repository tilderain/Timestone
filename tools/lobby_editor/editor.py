#!/usr/bin/env python3
"""Layout editor for the native lobby screens, in the browser.

    tools/lobby_editor/editor.py [game-dir] [--port 8765] [--edits file.json]

Then open http://127.0.0.1:8765/. Pick a screen, click an element, drag it,
resize it, or replace its picture with another of the game's textures. The
screen shown is the real screen code (UMVC3Hook/code/lobby_ui.cpp), built for
this machine with LOBBY_LAYOUT_EDIT and drawn by tools/umvc3_ui/umvc3_ui.py
from the textures in build/ui_cache, the way tools/lobby_preview does it.
[game-dir] is the folder holding umvc3.exe; left out, it is looked for in the
Steam libraries as the installer does. With it the editor offers every
texture under the game's ui folder (they are unpacked into the cache as they
are looked at); without it, only what the cache already holds.

An element is one Painter call in the screen code, known by its file and
line: what is changed there applies to everything that line draws (every row
of a list, both halves of a shadowed label). Elements can also be deleted, and
new ones made: a picture, a 9-slice, a box or a line of text, drawn
over the screen's own, under its modals and footer. The changes are kept in
tools/lobby_editor/edits.json. They are not in the plugin: this is the
editor on its own, to see whether working this way is worth building on.

Everything the page does goes through the HTTP interface below, and the page
opens in a given state from its address, so both can be driven by a script:

    GET  /api/screens                    names
    GET  /api/screen?name=N              the screen's commands and rectangles
    GET  /screen?name=N                  the picture (BMP: quick to make)
    GET  /screen.png?name=N              the same as PNG
    GET  /api/edits                      every edit
    POST /api/edit   {"site": S, "set": {"dx": 4, "tex": "name", ...}}
    POST /api/edit   {"site": S, "set": {"hidden": true, "label": "what it was"}}   delete
    POST /api/edit   {"site": S, "set": {"swap": true}}   picture <-> 9-slice
    POST /api/edit   {"site": S, "reset": true}
    POST /api/add    {"screen": N, "kind": "sprite"|"frame"|"fill"|"text", "tex": T, "uv": [..], "text": "..."}
                     a new element, on every screen of the kind N is; answers with its site
    POST /api/undo   POST /api/reset
    GET  /api/assets                     every texture that can be used
    GET  /asset?name=N                   one of them, as PNG
    GET  /thumb?name=N                   a small copy of it
    GET  /api/source?site=S              the source lines around a site

    /?screen=N&select=S                  open with S selected
    /?screen=N&select=S&assets=1         ... and the texture browser open
    /?...&assets=1&folder=F              ... on the folder F
    /?screen=N&select=S&crop=NAME        ... and NAME open for cropping
    /?screen=N&play=drag,x0,y0,x1,y1     press the mouse at one point, let go at another
    /?screen=N&select=S&play=size,dx,dy  drag the selected element's corner
    /?screen=N&select=S&play=delete      press Delete on the selected element
    /?screen=N&play=newbox               choose New element > Box
    /?...&crop=NAME&play=crop,x0,y0,x1,y1  drag out a part of NAME and use it
    &snap=G  &cropsnap=G                 with the screen's or the picture's grid at G
"""
import argparse
import collections
import glob
import hashlib
import io
import json
import math
import os
import struct
import subprocess
import sys
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'umvc3_ui'))
import umvc3_ui  # noqa: E402

CACHE = os.path.join(ROOT, 'build', 'ui_cache')
WORK = os.path.join(ROOT, 'build', 'lobby_editor')
HOST = os.path.join(WORK, 'host_edit')
HOST_SOURCES = [
    'tools/lobby_preview/preview.cpp',
    'UMVC3Hook/code/lobby_ui.cpp',
    'UMVC3Hook/code/lobby_ui_settings.cpp',
    'UMVC3Hook/code/lobby_canvas.cpp',
    'UMVC3Hook/code/space_bg.cpp',
]
# Archives that are in memory whenever the lobby is up; a texture from any
# other has to have its archive asked for before the game can draw it.
RESIDENT = ('cmn', 'netroom', 'mnmain')
EDIT_FIELDS = {'dx': 0.0, 'dy': 0.0, 'dw': 0.0, 'dh': 0.0, 'scale': 1.0, 'tex': None, 'uv': None,
               # Deleted: the call draws nothing. `label` is what it drew, for the list of them.
               'hidden': False, 'label': '',
               # A picture drawn as a 9-slice, or a frame as a plain picture.
               'swap': False,
               # An element made in the editor has a site of its own, "added:N", and is
               # defined by `new`: {screen, kind, x, y, w, h, scale, tex, uv, text, color}.
               # The other fields apply to it as to any element; `text` and `color`
               # are for these alone.
               'new': None, 'text': None, 'color': None}
UI_SCREENS = ('Top menu', 'Lobby search', 'Create lobby', 'Room', 'Team setup', 'Settings')


def png_size(path):
    with open(path, 'rb') as file:
        head = file.read(24)
    return struct.unpack('>II', head[16:24]) if head[:8] == b'\x89PNG\r\n\x1a\n' else (0, 0)


def build_host():
    """Compiles the screen code for this machine, if it is older than its sources."""
    os.makedirs(WORK, exist_ok=True)
    code = os.path.join(ROOT, 'UMVC3Hook', 'code')
    newest = max(os.path.getmtime(os.path.join(code, name)) for name in os.listdir(code)
                 if name.startswith(('lobby_', 'space_bg', 'assist_names')))
    newest = max(newest, os.path.getmtime(os.path.join(ROOT, HOST_SOURCES[0])))
    if os.path.exists(HOST) and os.path.getmtime(HOST) >= newest:
        return
    print('building the screen code...')
    subprocess.run([os.environ.get('HOST_CXX', 'c++'), '-std=c++17', '-O1', '-w',
                    '-DLOBBY_LAYOUT_EDIT', '-I' + code]
                   + [os.path.join(ROOT, source) for source in HOST_SOURCES] + ['-o', HOST],
                   check=True)


class Textures:
    """Every texture the editor can offer: what build/ui_cache holds, and
    with a game folder every texture under its ui folder, unpacked into the
    cache the first time it is looked at."""

    LANGUAGES = ('_fr', '_ge', '_it', '_sp')  # the same art again, in another language

    def __init__(self, game_dir):
        self.index = {}
        self.bad = set()
        if game_dir:
            self.index = self.load_index(game_dir)
        self.listing = self.build_listing()

    def load_index(self, game_dir):
        ui_dir = self.ui_dir = os.path.join(game_dir, 'nativePCx64', 'ui')
        saved = os.path.join(WORK, 'texture_index.json')
        try:
            data = json.load(open(saved))
            if data.get('ui_dir') == ui_dir and data.get('version') == 1:
                return data['textures']
        except (OSError, ValueError):
            pass
        print('listing the game\'s textures (once)...', flush=True)
        index = {}
        archives = sorted(glob.glob(os.path.join(ui_dir, '**', '*.arc'), recursive=True))

        def rank(path):
            # A texture in several archives is listed under the one the lobby
            # is likeliest to have: its resident ones, then the others it uses.
            name = os.path.relpath(path, ui_dir)[:-4].replace(os.sep, '/')
            base = name[:-3] if name.endswith('_en') else name
            order = (0 if base in RESIDENT else 1 if base in umvc3_ui.LOBBY_ARCHIVES else 2)
            return order, name

        for path in sorted(archives, key=rank):
            name = rank(path)[1]
            if name.endswith(self.LANGUAGES):
                continue
            try:
                data, entries = umvc3_ui.arc_entries(path)
            except (OSError, ValueError, struct.error):
                continue
            for entry, type_hash, packed, _size, start in entries:
                base = entry.replace('\\', '/').split('/')[-1]
                if type_hash != umvc3_ui.TYPE_TEX or base in index:
                    continue
                try:
                    header = umvc3_ui.tex_header(
                        zlib.decompressobj().decompress(data[start:start + packed], 16))
                except zlib.error:
                    continue
                if header and header[2] in umvc3_ui.TEX_FORMATS and header[0] and header[1]:
                    index[base] = {'w': header[0], 'h': header[1], 'archive': name,
                                   'file': os.path.relpath(path, ui_dir), 'at': [start, packed]}
        for path in sorted(glob.glob(os.path.join(ui_dir, '**', '*.tex'), recursive=True)):
            base = os.path.basename(path)[:-4]
            if base in index:
                continue
            try:
                with open(path, 'rb') as file:
                    header = umvc3_ui.tex_header(file.read(16))
            except OSError:
                continue
            if header and header[2] in umvc3_ui.TEX_FORMATS and header[0] and header[1]:
                index[base] = {'w': header[0], 'h': header[1], 'archive': '',
                               'file': os.path.relpath(path, ui_dir), 'at': None}
        os.makedirs(WORK, exist_ok=True)
        with open(saved, 'w') as file:
            json.dump({'version': 1, 'ui_dir': ui_dir, 'textures': index}, file)
        return index

    def build_listing(self):
        listing = {}
        for name, entry in self.index.items():
            archive = entry['archive']
            base = archive[:-3] if archive.endswith('_en') else archive
            # Where the browser files it: under its archive, or for a file of
            # its own under the folder that is in.
            folder = archive or os.path.dirname(entry['file']).replace(os.sep, '/') or 'ui'
            listing[name] = {'name': name, 'w': entry['w'], 'h': entry['h'], 'folder': folder,
                             'archive': archive or 'loose file', 'resident': base in RESIDENT}
        if os.path.isdir(CACHE):
            for file in os.listdir(CACHE):
                # What the preview unpacked under a name of its own.
                if file.endswith('.png') and file[:-4] not in listing:
                    width, height = png_size(os.path.join(CACHE, file))
                    listing[file[:-4]] = {'name': file[:-4], 'w': width, 'h': height,
                                          'folder': 'preview', 'archive': '', 'resident': False}
        return [listing[name] for name in sorted(listing, key=str.lower)]

    def path(self, name):
        return os.path.join(CACHE, os.path.basename(str(name)) + '.png')

    def read(self, name):
        """The texture's TEX resource from the game folder, or None."""
        entry = self.index.get(name)
        if not entry:
            return None
        with open(os.path.join(self.ui_dir, entry['file']), 'rb') as file:
            if not entry['at']:
                return file.read()
            file.seek(entry['at'][0])
            return zlib.decompress(file.read(entry['at'][1]))

    @staticmethod
    def write(image, path, **options):
        # Under a name of the thread's own first: several can be at the same texture.
        scratch = '%s.%d.tmp' % (path, threading.get_ident())
        os.makedirs(os.path.dirname(path), exist_ok=True)
        image.save(scratch, 'PNG', **options)
        os.replace(scratch, path)

    def ensure(self, name):
        """The texture's PNG in the cache, unpacked now if need be; None if there is none."""
        name = os.path.basename(str(name))
        path = self.path(name)
        if os.path.exists(path):
            return path
        if name in self.bad:
            return None
        try:
            data = self.read(name)
            if data is None:
                return None
            self.write(umvc3_ui.tex_decode(data), path, compress_level=1)
            return path
        except (OSError, ValueError, zlib.error, IndexError, struct.error):
            self.bad.add(name)
            return None

    def thumb(self, name, most=160):
        """A small copy of the texture, as a PNG file; None if there is none."""
        name = os.path.basename(str(name))
        path = os.path.join(WORK, 'thumbs', name + '.png')
        if os.path.exists(path):
            return path
        if name in self.bad:
            return None
        try:
            entry = self.index.get(name)
            if entry:
                # Straight from the game's file, reading only as many of its
                # blocks as a picture this small needs. That is quicker than
                # opening the unpacked PNG even where there is one, and the
                # whole texture is only unpacked when it is opened or used.
                step = max(1, max(entry['w'], entry['h']) // (most * 2))
                image = umvc3_ui.tex_decode(self.read(name), step)
            else:
                from PIL import Image
                image = Image.open(self.path(name)).convert('RGBA')
            image.thumbnail((most, most))
            self.write(image, path, compress_level=1)
            return path
        except (OSError, ValueError, zlib.error, IndexError, struct.error):
            self.bad.add(name)
            return None

    def warm(self, pause=0.002):
        """Makes every thumbnail not yet made, in the background, so folders
        opened later show at once."""
        for item in list(self.listing):
            self.thumb(item['name'])
            time.sleep(pause)  # requests come first


class Editor:
    def __init__(self, edits_path, game_dir):
        self.lock = threading.RLock()
        self.edits_path = edits_path
        self.edits = {}
        self.history = []
        self.version = 0       # goes up with every change to the edits
        self.fresh = {}        # screen -> the version its commands were made at
        self.screens = {}
        self.pictures = {}
        self.backdrops = {}
        self.stamps = collections.OrderedDict()
        self.stamp_bytes = 0
        self.renderer = umvc3_ui.Renderer(CACHE)
        self.textures = Textures(game_dir)
        if os.path.exists(edits_path):
            self.edits = json.load(open(edits_path))
        self.run_host()

    # -- edits ------------------------------------------------------------
    def save(self):
        os.makedirs(os.path.dirname(self.edits_path), exist_ok=True)
        with open(self.edits_path, 'w') as file:
            json.dump(self.edits, file, indent=1, sort_keys=True)
            file.write('\n')

    def changed(self, before):
        self.history.append(before)
        del self.history[:-200]
        self.save()
        self.version += 1

    def edit(self, site, values=None, reset=False):
        with self.lock:
            before = json.dumps(self.edits)
            if reset:
                self.edits.pop(site, None)
            else:
                edit = dict(EDIT_FIELDS, **self.edits.get(site, {}))
                for key, value in (values or {}).items():
                    if key not in EDIT_FIELDS:
                        raise ValueError('unknown field: ' + key)
                    if key == 'tex':
                        if value is not None and not self.textures.ensure(value):
                            raise ValueError('no such texture: ' + str(value))
                        edit[key] = value
                    elif key == 'uv':
                        edit[key] = None if value is None else [float(v) for v in value][:4]
                    elif key == 'new':
                        raise ValueError('an element is made with /api/add')
                    elif key == 'text':
                        edit[key] = None if value is None else ' '.join(str(value).split())[:120]
                    elif key == 'color':
                        edit[key] = None if value is None else [
                            min(1.0, max(0.0, float(v))) for v in value][:4]
                    elif key in ('hidden', 'swap'):
                        edit[key] = bool(value)
                    elif key == 'label':
                        edit[key] = str(value)[:80]
                    else:
                        edit[key] = float(value)
                edit = {key: value for key, value in edit.items() if value != EDIT_FIELDS[key]}
                if edit.get('new') and edit.get('hidden'):
                    self.edits.pop(site, None)  # deleting what the editor made unmakes it
                elif edit:
                    self.edits[site] = edit
                else:
                    self.edits.pop(site, None)
            if json.dumps(self.edits) != before:
                self.changed(before)
            return self.version

    def add(self, scene, kind, tex=None, uv=None, text=None):
        """Makes a new element on the LobbyUi screen that `scene` shows; returns its site."""
        with self.lock:
            if kind not in ('sprite', 'frame', 'fill', 'text'):
                raise ValueError('unknown kind: ' + str(kind))
            meta = self.commands(scene)[0]
            if meta.get('type') != 'meta' or meta.get('screen', -1) < 0:
                raise ValueError('no screen to add to')
            new = {'screen': meta['screen'], 'kind': kind, 'scale': 1.0}
            width, height = 240.0, 96.0
            if kind in ('sprite', 'frame'):
                path = self.textures.ensure(tex)
                if not path:
                    raise ValueError('no such texture: ' + str(tex))
                new['tex'] = os.path.basename(str(tex))
                if kind == 'sprite':
                    size = png_size(path)
                    part = [float(v) for v in uv][:4] if uv else [0.0, 0.0, 1.0, 1.0]
                    if uv:
                        new['uv'] = part
                    width = max(1.0, round(abs(part[2] - part[0]) * size[0]))
                    height = max(1.0, round(abs(part[3] - part[1]) * size[1]))
            elif kind == 'text':
                new['text'] = ' '.join(str(text or 'New text').split())[:120]
                new['scale'] = 0.8
                width, height = 0.0, 0.0
            else:
                new['color'] = [0.0, 0.0, 0.0, 0.6]
            # In the middle of the screen, on whole units, to be dragged from there.
            new.update(x=float(round((1280 - width) / 2)), y=float(round((720 - height) / 2)),
                       w=width, h=height)
            before = json.dumps(self.edits)
            number = 1 + max([int(site[6:]) for site in self.edits if site.startswith('added:')]
                             or [0])
            site = 'added:%d' % number
            self.edits[site] = {'new': new}
            self.changed(before)
            return site

    def undo(self):
        with self.lock:
            if self.history:
                self.edits = json.loads(self.history.pop())
                self.save()
                self.version += 1
            return self.version

    def reset(self):
        with self.lock:
            before = json.dumps(self.edits)
            self.edits = {}
            if before != '{}':
                self.changed(before)
            return self.version

    # -- the screen code ----------------------------------------------------
    def run_host(self, only=None):
        """Runs the screen code with the edits applied: for one screen, or all of them."""
        with self.lock:
            textures = sorted({name for edit in self.edits.values()
                               for name in (edit.get('tex'), (edit.get('new') or {}).get('tex'))
                               if name})
            lines = []
            for name in textures:
                path = self.textures.ensure(name)
                lines.append('tex %s %d %d' % ((name,) + tuple(png_size(path) if path else (1, 1))))
            for site, edit in sorted(self.edits.items()):
                edit = dict(EDIT_FIELDS, **edit)
                new = edit['new']
                if new:
                    # What it was made as, with what was done to it since.
                    tex = edit['tex'] or new.get('tex')
                    uv = edit['uv'] or new.get('uv') or [0, 0, 1, 1]
                    color = edit['color'] or new.get('color') or [1, 1, 1, 1]
                    text = new.get('text', '') if edit['text'] is None else edit['text']
                    kind = new['kind']
                    if edit['swap'] and kind in ('sprite', 'frame'):
                        kind = 'frame' if kind == 'sprite' else 'sprite'
                    lines.append('add %s %d %s %g %g %g %g %g %d %g %g %g %g %g %g %g %g %s' % (
                        site[6:], new['screen'], kind, new['x'] + edit['dx'],
                        new['y'] + edit['dy'], max(1.0, new['w'] + edit['dw']),
                        max(1.0, new['h'] + edit['dh']), new['scale'] * edit['scale'],
                        textures.index(tex) if tex else -1, uv[0], uv[1], uv[2], uv[3],
                        color[0], color[1], color[2], color[3], text or ' '))
                    continue
                uv = edit['uv'] or [0, 0, 1, 1]
                lines.append('edit %s %g %g %g %g %g %d %d %g %g %g %g %d %d' % (
                    site, edit['dx'], edit['dy'], edit['dw'], edit['dh'], edit['scale'],
                    textures.index(edit['tex']) if edit['tex'] else -1, 1 if edit['uv'] else 0,
                    uv[0], uv[1], uv[2], uv[3], 1 if edit['hidden'] else 0,
                    1 if edit['swap'] else 0))
            out = os.path.join(WORK, 'out')
            os.makedirs(out, exist_ok=True)
            edits_txt = os.path.join(WORK, 'edits.txt')
            with open(edits_txt, 'w') as file:
                file.write('\n'.join(lines) + '\n')
            subprocess.run([HOST, out, edits_txt] + ([only] if only else []), check=True,
                           stdout=subprocess.DEVNULL)
            for name in ([only] if only else self.screen_names()):
                self.fresh[name] = self.version
                self.screens.pop(name, None)
                self.pictures.pop(name, None)

    def screen_names(self):
        out = os.path.join(WORK, 'out')
        return sorted(name[:-5] for name in os.listdir(out) if name.endswith('.json'))

    def commands(self, name):
        with self.lock:
            if name not in self.fresh:
                raise KeyError(name)
            # Only the screen being looked at is run again after an edit.
            if self.fresh[name] != self.version:
                self.run_host(name)
            if name not in self.screens:
                self.screens[name] = json.load(open(os.path.join(WORK, 'out', name + '.json')))
            return self.screens[name]

    def screen(self, name):
        """What the page needs of a screen: each command that can be pointed at."""
        with self.lock:
            items = []
            commands = self.commands(name)
            ui = commands[0].get('screen', -1) if commands and commands[0]['type'] == 'meta' else -1
            for index, command in enumerate(commands):
                bounds = self.renderer.bounds(command)
                color = command.get('color') or [1, 1, 1, 1]
                if bounds is None or 'site' not in command or color[3] <= 0.003:
                    continue
                item = {'i': index, 'type': command['type'], 'site': command['site'],
                        'bounds': [round(value, 2) for value in bounds],
                        'anchor': command.get('anchor', 4), 'color': color}
                if command['site'].startswith('added:'):
                    item['added'] = (self.edits.get(command['site'], {}).get('new') or {}).get('kind')
                if 'tex' in command:
                    item['tex'] = command['tex'].replace('\\', '/').split('/')[-1]
                    item['size'] = command['size']
                    item['uv'] = command.get('uv', [0, 0, 1, 1])
                    item['fill'] = item['tex'].startswith('white_')
                if 'str' in command:
                    item['str'] = command['str']
                    item['scale'] = command.get('scale', [1, 1])
                items.append(item)
            return {'name': name, 'version': self.version, 'commands': items, 'ui': ui,
                    'uiName': UI_SCREENS[ui] if 0 <= ui < len(UI_SCREENS) else ''}

    def stamp(self, command):
        """The command drawn by itself on a transparent picture just big
        enough for it, and where that goes: (picture, x, y). Kept, so after an
        edit only what changed is drawn again. None for a command that cannot
        be kept this way."""
        if command['type'] == 'line' or command.get('turn'):
            return None
        bounds = self.renderer.bounds(command)
        if bounds is None:
            return None
        key = json.dumps({k: v for k, v in command.items() if k not in ('depth', 'site')},
                         sort_keys=True)
        stamp = self.stamps.get(key)
        if stamp is None:
            from PIL import Image
            # A glyph's cell is wider than its advance, so text overhangs its ends.
            pad = 2
            if command['type'] == 'text':
                cell = self.renderer.font(command.get('font', 0))[2]
                pad += math.ceil(cell * command.get('scale', (1, 1))[0] / 2)
            # Drawn in place on a whole screen and cut out, so every pixel is
            # where drawing it straight onto the picture would put it.
            whole = Image.new('RGBA', (1280, 720), (0, 0, 0, 0))
            self.renderer.draw(whole, [command])
            box = (max(0, math.floor(bounds[0]) - pad), max(0, math.floor(bounds[1]) - 2),
                   min(1280, math.ceil(bounds[2]) + pad), min(720, math.ceil(bounds[3]) + 2))
            if box[2] <= box[0] or box[3] <= box[1]:
                stamp = (None, 0, 0)
            else:
                stamp = (whole.crop(box), box[0], box[1])
                self.stamp_bytes += stamp[0].width * stamp[0].height * 4
            self.stamps[key] = stamp
            while self.stamp_bytes > 512 << 20 and len(self.stamps) > 1:
                _key, old = self.stamps.popitem(last=False)
                if old[0] is not None:
                    self.stamp_bytes -= old[0].width * old[0].height * 4
        else:
            self.stamps.move_to_end(key)
        return stamp

    def compose(self, name):
        commands = self.commands(name)
        # What comes before the first editable command (the backdrop,
        # hundreds of lines) is the same from edit to edit: draw it once.
        first = next((i for i, command in enumerate(commands) if 'site' in command),
                     len(commands))
        key = hashlib.sha1(json.dumps(commands[:first]).encode()).hexdigest()
        if key not in self.backdrops:
            if len(self.backdrops) > 48:
                self.backdrops.clear()
            base = self.renderer.backdrop()
            self.renderer.draw(base, commands[:first])
            self.backdrops[key] = base
        canvas = self.backdrops[key].copy()
        for command in commands[first:]:
            color = command.get('color')
            if color is not None and color[3] <= 0.003:
                continue
            stamp = self.stamp(command)
            if stamp is None:
                self.renderer.draw(canvas, [command])
                continue
            tile, x, y = stamp
            if tile is not None:
                canvas.alpha_composite(tile, (x, y))
        return canvas

    def picture(self, name, kind='BMP'):
        with self.lock:
            self.commands(name)  # brings the screen up to date, dropping an old picture
            if (name, kind) not in self.pictures.get(name, {}):
                data = io.BytesIO()
                image = self.compose(name).convert('RGB')
                if kind == 'PNG':
                    image.save(data, 'PNG', compress_level=1)
                else:
                    image.save(data, 'BMP')
                self.pictures.setdefault(name, {})[(name, kind)] = data.getvalue()
            return self.pictures[name][(name, kind)]

    # -- textures -------------------------------------------------------------
    def assets(self):
        return self.textures.listing

    def source(self, site):
        file, _, line = site.partition(':')
        if file == 'added':
            raise ValueError('made in the editor: not in the source')
        path = os.path.join(ROOT, 'UMVC3Hook', 'code', os.path.basename(file))
        line = int(line)
        lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
        first = max(1, line - 2)
        return {'file': os.path.basename(file), 'line': line, 'first': first,
                'lines': lines[first - 1:line + 2]}

    def boot(self, query):
        """The state the page starts in, written into it so it needs no first round trip."""
        names = self.screen_names()
        name = query.get('screen', ['room_4slot' if 'room_4slot' in names else names[0]])[0]
        if name not in names:
            name = names[0]
        return {'screens': names, 'screen': self.screen(name), 'edits': self.edits,
                'assets': self.assets(), 'select': query.get('select', [''])[0],
                'openAssets': query.get('assets', [''])[0] == '1',
                'crop': query.get('crop', [''])[0], 'play': query.get('play', [''])[0],
                'snap': query.get('snap', [''])[0], 'cropSnap': query.get('cropsnap', [''])[0],
                'folder': query.get('folder', [''])[0]}


class Handler(BaseHTTPRequestHandler):
    editor = None
    protocol_version = 'HTTP/1.1'  # connections are kept: a folder is hundreds of thumbnails

    def log_message(self, *_args):
        pass

    def send(self, body, kind='application/json', status=200, cache=False):
        if not isinstance(body, bytes):
            body = json.dumps(body).encode()
        self.send_response(status)
        self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'max-age=86400' if cache else 'no-store')
        self.end_headers()
        self.wfile.write(body)

    def fail(self, status, message):
        try:
            self.send({'error': message}, status=status)
        except ConnectionError:
            pass

    def do_GET(self):
        url = urlparse(self.path)
        query = parse_qs(url.query)
        editor = self.editor
        try:
            if url.path == '/':
                page = open(os.path.join(HERE, 'editor.html'), encoding='utf-8').read()
                boot = json.dumps(editor.boot(query)).replace('</', '<\\/')
                self.send(page.replace('/*BOOT*/null', boot).encode(), 'text/html; charset=utf-8')
            elif url.path == '/api/screens':
                self.send(editor.screen_names())
            elif url.path == '/api/screen':
                self.send(editor.screen(query['name'][0]))
            elif url.path == '/screen':
                self.send(editor.picture(query['name'][0]), 'image/bmp')
            elif url.path == '/screen.png':
                self.send(editor.picture(query['name'][0], 'PNG'), 'image/png')
            elif url.path == '/api/edits':
                self.send(editor.edits)
            elif url.path == '/api/assets':
                self.send(editor.assets())
            elif url.path in ('/asset', '/thumb'):
                name = query['name'][0]
                path = (editor.textures.thumb(name) if url.path == '/thumb'
                        else editor.textures.ensure(name))
                if not path:
                    return self.fail(404, 'no such texture: ' + name)
                self.send(open(path, 'rb').read(), 'image/png', cache=True)
            elif url.path == '/api/source':
                self.send(editor.source(query['site'][0]))
            elif url.path == '/api/wait':
                # Answers once an edit has been taken (check.py's page waits on it).
                after, until = int(query['after'][0]), time.time() + 8
                while editor.version <= after and time.time() < until:
                    time.sleep(0.05)
                self.send({'version': editor.version})
            else:
                self.fail(404, 'not found')
        except ConnectionError:
            pass  # the browser stopped waiting: a newer picture replaced this one
        except (KeyError, OSError, ValueError) as error:
            self.fail(404, str(error))

    def do_POST(self):
        editor = self.editor
        try:
            length = int(self.headers.get('Content-Length') or 0)
            body = json.loads(self.rfile.read(length) or b'{}')
            made = None
            if self.path == '/api/edit':
                version = editor.edit(body['site'], body.get('set'), bool(body.get('reset')))
            elif self.path == '/api/add':
                made = editor.add(body['screen'], body.get('kind'), body.get('tex'),
                                  body.get('uv'), body.get('text'))
                version = editor.version
            elif self.path == '/api/undo':
                version = editor.undo()
            elif self.path == '/api/reset':
                version = editor.reset()
            else:
                return self.fail(404, 'not found')
            result = {'version': version, 'edits': editor.edits}
            if made:
                result['site'] = made
            if body.get('screen'):
                # The screen the page is showing, so it need not ask for it.
                result['screen'] = editor.screen(body['screen'])
            self.send(result)
        except ConnectionError:
            pass
        except (KeyError, ValueError) as error:
            self.fail(400, str(error))


def serve(port, edits_path, game_dir=None):
    """The editor's server, ready to serve_forever; port 0 takes any free one."""
    build_host()
    Handler.editor = Editor(edits_path, game_dir)
    threading.Thread(target=Handler.editor.textures.warm, daemon=True).start()
    server = ThreadingHTTPServer(('127.0.0.1', port), Handler)
    server.daemon_threads = True
    return server


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('game_dir', nargs='?',
                        help='the folder holding umvc3.exe (found in the Steam libraries if left out)')
    parser.add_argument('--port', type=int, default=8765)
    parser.add_argument('--edits', default=os.path.join(HERE, 'edits.json'))
    args = parser.parse_args()

    if not args.game_dir:
        args.game_dir = umvc3_ui.find_game()
        print('game folder:', args.game_dir or 'not found in the Steam libraries')
    if not os.path.exists(os.path.join(CACHE, 'nrodinpro_BM_HQ_NOMIP.fsd')):
        if not args.game_dir:
            parser.error('build/ui_cache is empty: give the game folder so it can be filled')
        umvc3_ui.extract(args.game_dir, CACHE)
    server = serve(args.port, args.edits, args.game_dir)
    print('layout editor: http://127.0.0.1:%d/   (edits in %s)' % (server.server_port, args.edits),
          flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
