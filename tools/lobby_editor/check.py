#!/usr/bin/env python3
"""Checks the layout editor end to end, without touching anyone's edits.

    tools/lobby_editor/check.py [game-dir] [--shots DIR] [--no-shots]

Starts the editor on a free port with a scratch edits file, drives its HTTP
interface (move, resize, scale, replace a picture, undo, reset) and checks
what each does to the screen. Then, if Firefox is installed, opens the page
headless in several states, saves pictures of them to DIR (default
build/lobby_editor/check) to be looked at, and has the page replay a mouse
drag and a corner drag to check the edits they make.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import editor  # noqa: E402

failures = []


def check(name, ok, detail=''):
    print('%-4s %s%s' % ('ok' if ok else 'FAIL', name, '  ' + detail if detail and not ok else ''))
    if not ok:
        failures.append(name)


class Client:
    def __init__(self, base):
        self.base = base

    def get(self, path, raw=False):
        with urllib.request.urlopen(self.base + path) as response:
            data = response.read()
        return data if raw else json.loads(data)

    def post(self, path, body=None):
        request = urllib.request.Request(self.base + path, json.dumps(body or {}).encode())
        try:
            with urllib.request.urlopen(request) as response:
                return response.status, json.loads(response.read())
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read())

    def first(self, screen, site):
        commands = self.get('/api/screen?name=' + screen)['commands']
        return next(c for c in commands if c['site'] == site)


def api_checks(client):
    screens = client.get('/api/screens')
    check('screens are listed', 'top' in screens and len(screens) > 20, str(len(screens)))
    top = client.get('/api/screen?name=top')['commands']
    picture = next(c for c in top if c['type'] == 'sprite' and not c['fill'])
    text = next(c for c in top if c['type'] == 'text')
    frame = next(c for c in top if c['type'] == 'frame')
    plain = client.get('/screen.png?name=top', raw=True)
    check('a picture comes back as PNG', plain[:8] == b'\x89PNG\r\n\x1a\n')
    check('and as the BMP the page shows', client.get('/screen?name=top', raw=True)[:2] == b'BM')

    status, _ = client.post('/api/edit', {'site': picture['site'], 'set': {'dx': 30, 'dy': -12}})
    moved = client.first('top', picture['site'])
    check('moving shifts the element', status == 200 and
          [round(a - b, 1) for a, b in zip(moved['bounds'], picture['bounds'])] == [30, -12, 30, -12],
          str(moved['bounds']))
    check('the picture changes with it', client.get('/screen.png?name=top', raw=True) != plain)

    client.post('/api/edit', {'site': picture['site'], 'set': {'dw': 20, 'dh': 10}})
    sized = client.first('top', picture['site'])
    check('resizing changes the size', round(sized['size'][0] - picture['size'][0], 1) == 20 and
          round(sized['size'][1] - picture['size'][1], 1) == 10, str(sized['size']))

    client.post('/api/edit', {'site': text['site'], 'set': {'scale': 2}})
    scaled = client.first('top', text['site'])
    check('scaling text scales it', abs(scaled['scale'][0] / text['scale'][0] - 2) < 0.01,
          str(scaled['scale']))

    other = next(a['name'] for a in client.get('/api/assets')
                 if a['name'] != picture['tex'] and a['w'] >= 32)
    client.post('/api/edit', {'site': picture['site'],
                              'set': {'tex': other, 'uv': [0, 0, 0.5, 0.5]}})
    swapped = client.first('top', picture['site'])
    check('replacing a picture shows the new one', swapped['tex'] == other and
          [round(v, 3) for v in swapped['uv']] == [0, 0, 0.5, 0.5], swapped['tex'])
    client.post('/api/edit', {'site': frame['site'], 'set': {'tex': other, 'dw': 40}})
    check('a frame takes a new picture too', client.first('top', frame['site'])['tex'] == other)

    # Any of the game's textures can be used, not only those already unpacked.
    assets = client.get('/api/assets')
    fresh = next((a for a in assets if a['archive'] not in ('', 'loose file') and
                  not os.path.exists(os.path.join(editor.CACHE, a['name'] + '.png'))), None)
    if fresh:
        check('the whole game\'s textures are listed', len(assets) > 3000, str(len(assets)))
        folders = {a['folder'] for a in assets}
        check('each is filed under a folder', all(a['folder'] for a in assets) and
              {'cmn', 'mnmain', 'netroom'} <= folders and len(folders) > 50, str(len(folders)))
        started = time.time()
        thumb = client.get('/thumb?name=' + fresh['name'], raw=True)
        check('a thumbnail is made without unpacking the texture', thumb[:4] == b'\x89PNG' and
              not os.path.exists(os.path.join(editor.CACHE, fresh['name'] + '.png')) and
              time.time() - started < 0.5, '%s, %.2f s' % (fresh['name'], time.time() - started))
        whole = client.get('/asset?name=' + fresh['name'], raw=True)
        check('the texture is unpacked when it is opened', whole[:4] == b'\x89PNG' and
              os.path.exists(os.path.join(editor.CACHE, fresh['name'] + '.png')), fresh['name'])
    else:
        print('skip the whole-game texture checks: no game folder, or all of it unpacked')

    # After the first picture of a screen, an edit and its picture are quick.
    client.get('/screen?name=room_4slot', raw=True)
    row = next(c for c in client.get('/api/screen?name=room_4slot')['commands'] if c['type'] != 'text')
    started = time.time()
    _status, result = client.post('/api/edit', {'site': row['site'], 'set': {'dx': 6},
                                                'screen': 'room_4slot'})
    client.get('/screen?name=room_4slot', raw=True)
    took = time.time() - started
    check('an edit is drawn in under half a second', took < 0.5 and
          result.get('screen', {}).get('name') == 'room_4slot', '%.2f s' % took)
    client.post('/api/edit', {'site': row['site'], 'reset': True})
    client.post('/api/undo')
    client.post('/api/undo')

    status, _ = client.post('/api/edit', {'site': picture['site'], 'set': {'bogus': 1}})
    check('an unknown field is refused', status == 400)
    status, _ = client.post('/api/edit', {'site': picture['site'], 'set': {'tex': 'no_such'}})
    check('an unknown texture is refused', status == 400)

    edits = client.get('/api/edits')
    check('edits are kept by site', set(edits) == {picture['site'], text['site'], frame['site']},
          str(sorted(edits)))
    client.post('/api/undo')
    check('undo takes back the last change', frame['site'] not in client.get('/api/edits'))
    client.post('/api/edit', {'site': text['site'], 'reset': True})
    check('one element can be reset', text['site'] not in client.get('/api/edits'))
    # A deleted element is drawn no more, is remembered, and can be put back.
    site = picture['site']
    there = sum(c['site'] == site for c in client.get('/api/screen?name=top')['commands'])
    client.post('/api/edit', {'site': site, 'set': {'hidden': True, 'label': 'a label'}})
    gone = sum(c['site'] == site for c in client.get('/api/screen?name=top')['commands'])
    check('deleting an element removes what it drew', there > 0 and gone == 0,
          '%d before, %d after' % (there, gone))
    check('and it is remembered as deleted', client.get('/api/edits')[site].get('hidden') is True)
    client.post('/api/edit', {'site': site, 'set': {'hidden': False, 'label': ''}})
    check('restoring it puts it back as it was edited', client.first('top', site)['tex'] == other)
    # New elements: made on the kind of screen being looked at, then edited like any other.
    before = len(client.get('/api/screen?name=top')['commands'])
    _status, made = client.post('/api/add', {'screen': 'top', 'kind': 'fill'})
    box = client.first('top', made.get('site', ''))
    check('a new box appears in the middle of the screen', made.get('site', '').startswith('added:')
          and box['added'] == 'fill' and abs((box['bounds'][0] + box['bounds'][2]) / 2 - 640) <= 1,
          str(box))
    _status, words = client.post('/api/add', {'screen': 'top', 'kind': 'text', 'text': 'Hello'})
    _status, art = client.post('/api/add', {'screen': 'top', 'kind': 'sprite', 'tex': other,
                                            'uv': [0, 0, 0.5, 0.5]})
    _status, plate = client.post('/api/add', {'screen': 'top', 'kind': 'frame', 'tex': other})
    check('text, a picture and a frame can be made too',
          client.first('top', words['site'])['str'] == 'Hello' and
          client.first('top', art['site'])['tex'] == other and
          client.first('top', plate['site'])['type'] in ('frame', 'tileframe') and
          len(client.get('/api/screen?name=top')['commands']) == before + 4)
    check('they belong to that kind of screen only',
          not any(c['site'].startswith('added:')
                  for c in client.get('/api/screen?name=room_4slot')['commands']) and
          any(c['site'] == made['site']
              for c in client.get('/api/screen?name=confirm_exit')['commands']))
    client.post('/api/edit', {'site': made['site'], 'set': {'dx': 100, 'dw': 60,
                                                              'color': [1, 0, 0, 0.5]}})
    client.post('/api/edit', {'site': words['site'], 'set': {'text': 'Changed', 'scale': 2}})
    moved = client.first('top', made['site'])
    check('a new element moves, resizes and takes a colour',
          round(moved['bounds'][0] - box['bounds'][0]) == 100 and
          round(moved['bounds'][2] - box['bounds'][2]) == 160 and moved['color'][:2] == [1, 0],
          str(moved))
    check('its text can be changed', client.first('top', words['site'])['str'] == 'Changed')
    client.post('/api/edit', {'site': art['site'], 'set': {'hidden': True}})
    check('deleting a new element unmakes it', art['site'] not in client.get('/api/edits'))
    for site in (made['site'], words['site'], plate['site']):
        client.post('/api/edit', {'site': site, 'set': {'hidden': True}})

    # A picture can be drawn as a 9-slice and back, where it stands.
    site = picture['site']
    as_picture = client.first('top', site)
    client.post('/api/edit', {'site': site, 'set': {'swap': True}})
    as_frame = client.first('top', site)
    check('a picture becomes a 9-slice', as_picture['type'] == 'sprite' and
          as_frame['type'] in ('frame', 'tileframe') and as_frame['tex'] == as_picture['tex'] and
          abs(as_frame['bounds'][0] - as_picture['bounds'][0]) < 0.6 and
          abs(as_frame['bounds'][1] - as_picture['bounds'][1]) < 0.6, str(as_frame))
    client.post('/api/edit', {'site': site, 'set': {'swap': False}})
    check('and a picture again', client.first('top', site)['type'] == 'sprite')
    client.post('/api/edit', {'site': frame['site'], 'set': {'swap': True}})
    check('a frame becomes a plain picture', client.first('top', frame['site'])['type'] == 'sprite')
    client.post('/api/edit', {'site': frame['site'], 'reset': True})
    _status, fresh_art = client.post('/api/add', {'screen': 'top', 'kind': 'sprite', 'tex': other})
    client.post('/api/edit', {'site': fresh_art['site'], 'set': {'swap': True, 'dw': 200}})
    check('so does one made in the editor',
          client.first('top', fresh_art['site'])['type'] in ('frame', 'tileframe'))
    client.post('/api/edit', {'site': fresh_art['site'], 'set': {'hidden': True}})

    source = client.get('/api/source?site=' + picture['site'])
    check('the source line is found', any('p.' in line or 'Sprite' in line or 'Draw' in line
                                          for line in source['lines']), str(source['lines']))
    return picture, other


def near(edit, wanted):
    """The same fields, each within two units: the browser's mouse is on whole
    pixels of a picture shown smaller than the 1280 units it stands for."""
    return set(edit) == set(wanted) and all(abs(edit[key] - wanted[key]) <= 2 for key in wanted)


def browse(firefox, home, url, out):
    if os.path.exists(out):
        os.remove(out)
    subprocess.run([firefox, '--headless', '--no-remote', '--profile', home,
                    '--window-size', '1500,860', '--screenshot', out, url],
                   env=dict(os.environ, HOME=home), stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, timeout=120)


def mouse_checks(client, base, firefox, home, directory):
    """The page's own mouse handling: a press, a move and a release in a real browser."""
    client.post('/api/reset')
    commands = client.get('/api/screen?name=top')['commands']
    # The element a press lands on is the last one drawn under the pointer.
    target = next(c for c in commands if c['type'] == 'sprite' and not c['fill'])
    x = (target['bounds'][0] + target['bounds'][2]) / 2
    y = (target['bounds'][1] + target['bounds'][3]) / 2
    top = [c for c in commands if c['bounds'][0] <= x < c['bounds'][2]
           and c['bounds'][1] <= y < c['bounds'][3]][-1]
    browse(firefox, home, '%s/?screen=top&snap=0&play=drag,%g,%g,%g,%g' % (base, x, y, x + 40, y + 25),
           os.path.join(directory, 'dragged.png'))
    edit = client.get('/api/edits').get(top['site'], {})
    check('dragging with the mouse moves the element', near(edit, {'dx': 40.0, 'dy': 25.0}),
          '%s: %s' % (top['site'], edit))

    client.post('/api/reset')
    browse(firefox, home, '%s/?screen=top&snap=0&select=%s&play=size,30,20' % (base, target['site']),
           os.path.join(directory, 'sized.png'))
    edit = client.get('/api/edits').get(target['site'], {})
    column, row = target['anchor'] % 3, target['anchor'] // 3
    wanted = {key: value for key, value in
              {'dw': 30.0, 'dh': 20.0, 'dx': 15.0 * column, 'dy': 10.0 * row}.items() if value}
    check('dragging the corner resizes it', near(edit, wanted), '%s, wanted %s' % (edit, wanted))
    client.post('/api/reset')

    # The New element control, for the kind that needs nothing chosen.
    browse(firefox, home, base + '/?screen=top&play=newbox', os.path.join(directory, 'new_box.png'))
    made = [site for site, edit in client.get('/api/edits').items() if edit.get('new')]
    check('New element > Box makes a box', len(made) == 1 and
          client.get('/api/edits')[made[0]]['new']['kind'] == 'fill', str(made))
    if made:
        client.post('/api/add', {'screen': 'top', 'kind': 'text', 'text': 'Made in the editor'})
        browse(firefox, home, '%s/?screen=top&select=%s' % (base, made[0]),
               os.path.join(directory, 'new_selected.png'))
    client.post('/api/reset')

    # A picture stretched wide as itself, and beside it the same as a frame.
    _status, wide = client.post('/api/add', {'screen': 'top', 'kind': 'sprite',
                                             'tex': 'ga_net_waku_f_BM_HQ_NOMIP'})
    _status, framed = client.post('/api/add', {'screen': 'top', 'kind': 'sprite',
                                               'tex': 'ga_net_waku_f_BM_HQ_NOMIP'})
    if 'site' in wide and 'site' in framed:
        client.post('/api/edit', {'site': wide['site'], 'set': {'dx': -300, 'dw': 300, 'dh': 100}})
        client.post('/api/edit', {'site': framed['site'], 'set': {'dx': 60, 'dw': 300, 'dh': 100,
                                                                    'swap': True}})
        browse(firefox, home, '%s/?screen=top&select=%s' % (base, framed['site']),
               os.path.join(directory, 'swapped.png'))
    client.post('/api/reset')

    # The Delete key, on the element the page opens with selected.
    browse(firefox, home, '%s/?screen=top&select=%s&play=delete' % (base, target['site']),
           os.path.join(directory, 'deleted.png'))
    check('the Delete key deletes the selected element',
          client.get('/api/edits').get(target['site'], {}).get('hidden') is True,
          str(client.get('/api/edits')))
    browse(firefox, home, base + '/?screen=top', os.path.join(directory, 'deleted_list.png'))
    client.post('/api/reset')

    # Unless it was changed, the screen's grid is on, at 8.
    browse(firefox, home, '%s/?screen=top&play=drag,%g,%g,%g,%g' % (base, x, y, x + 43, y + 27),
           os.path.join(directory, 'snapped_usual.png'))
    usual = client.first('top', top['site'])['bounds']
    check('the grid is on at 8 to begin with', top['site'] in client.get('/api/edits') and
          abs(usual[0] / 8 - round(usual[0] / 8)) < 1e-6 and
          abs(usual[1] / 8 - round(usual[1] / 8)) < 1e-6, str(usual))
    client.post('/api/reset')

    # With the grid on, wherever the mouse lets go the corner is on the grid.
    on_grid = lambda value, grid: abs(value / grid - round(value / grid)) < 1e-6
    browse(firefox, home, '%s/?screen=top&snap=16&play=drag,%g,%g,%g,%g' % (base, x, y, x + 43, y + 27),
           os.path.join(directory, 'snapped.png'))
    snapped = client.first('top', top['site'])['bounds']
    check('a drag with the grid on lands on it', top['site'] in client.get('/api/edits') and
          on_grid(snapped[0], 16) and on_grid(snapped[1], 16), str(snapped))
    client.post('/api/reset')
    browse(firefox, home, '%s/?screen=top&snap=16&select=%s&play=size,37,21' % (base, target['site']),
           os.path.join(directory, 'snapped_size.png'))
    snapped = client.first('top', target['site'])['bounds']
    check('a resize with the grid on lands on it', target['site'] in client.get('/api/edits') and
          on_grid(snapped[2], 16) and on_grid(snapped[3], 16), str(snapped))
    client.post('/api/reset')

    sheet = next(a for a in client.get('/api/assets') if a['w'] >= 256 and a['h'] >= 128)
    browse(firefox, home, '%s/?screen=top&select=%s&crop=%s&cropsnap=32&play=crop,13,9,150,101'
           % (base, target['site'], sheet['name']), os.path.join(directory, 'crop_snapped.png'))
    uv = client.get('/api/edits').get(target['site'], {}).get('uv') or [0.1] * 4
    part = [uv[0] * sheet['w'], uv[1] * sheet['h'], uv[2] * sheet['w'], uv[3] * sheet['h']]
    check('choosing a part with the grid on lands on it',
          all(on_grid(value, 32) for value in part) and part[2] > part[0] and part[3] > part[1],
          str(part))
    client.post('/api/reset')


def shots(client, base, directory, picture, other):
    firefox = shutil.which('firefox')
    if not firefox:
        print('skip the checks in a browser: no firefox')
        return
    os.makedirs(directory, exist_ok=True)
    home = tempfile.mkdtemp(prefix='lobby_editor_ff')
    pages = {
        'page': '/?screen=room_4slot',
        'selected': '/?screen=top&select=' + picture['site'],
        'assets': '/?screen=top&assets=1&select=' + picture['site'],
        'assets_folder': '/?screen=top&assets=1&folder=res/arc&select=' + picture['site'],
        'crop': '/?screen=top&crop=' + other + '&select=' + picture['site'],
    }
    for name, path in pages.items():
        out = os.path.join(directory, name + '.png')
        browse(firefox, home, base + path, out)
        check('page picture: ' + name, os.path.exists(out) and os.path.getsize(out) > 20000, out)
    mouse_checks(client, base, firefox, home, directory)
    shutil.rmtree(home, ignore_errors=True)
    print('pictures of the page in', directory)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('game_dir', nargs='?')
    parser.add_argument('--shots', default=os.path.join(editor.WORK, 'check'))
    parser.add_argument('--no-shots', action='store_true')
    args = parser.parse_args()

    scratch = tempfile.mkdtemp(prefix='lobby_editor_check')
    server = editor.serve(0, os.path.join(scratch, 'edits.json'),
                          args.game_dir or editor.umvc3_ui.find_game())
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = 'http://127.0.0.1:%d' % server.server_port
    client = Client(base)
    try:
        picture, other = api_checks(client)
        if not args.no_shots:
            # Left as the checks left it: an element moved, resized and given another picture.
            shots(client, base, args.shots, picture, other)
        client.post('/api/reset')
        check('reset clears everything', client.get('/api/edits') == {})
    finally:
        server.shutdown()
        shutil.rmtree(scratch, ignore_errors=True)
    print('%d failed' % len(failures) if failures else 'all passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
