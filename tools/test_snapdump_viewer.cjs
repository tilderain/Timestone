const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

function viewer() {
  const elements = new Map();
  const drawCalls = [];
  const drawing = new Proxy({}, { get(target, key) {
    return key in target ? target[key] : (...args) => drawCalls.push([key, ...args]);
  } });
  const document = {
    getElementById(id) {
      if (!elements.has(id)) elements.set(id, { checked: false, style: {},
        classList: { add() {}, remove() {} }, querySelectorAll() { return []; },
        getContext() { return drawing; },
        getBoundingClientRect() { return { width: 640, height: 480 }; } });
      return elements.get(id);
    },
    addEventListener() {},
  };
  const context = vm.createContext({ document, window: { addEventListener() {} }, TextDecoder });
  const html = fs.readFileSync(path.join(__dirname, '../ndjson-viewer-2.html'), 'utf8');
  vm.runInContext(html.match(/<script>([\s\S]*?)<\/script>/)[1], context);
  return { context, elements, drawCalls, run: code => vm.runInContext(code, context) };
}

function fixture(version = 5, observedValue = 1) {
  const marker = Buffer.alloc(24);
  marker.writeBigInt64LE(3n); marker.writeBigInt64LE(1n, 8); marker.writeBigInt64LE(88n, 16);
  const root = Buffer.alloc(0x2C0); root.writeUInt32LE(2, 0x10);
  root.writeFloatLE(12, 0x50);
  root.writeFloatLE(600, 0xF0); root.writeFloatLE(150, 0xF4); root.writeFloatLE(-7, 0xF8);
  root.writeUInt32LE(42, 0x124);
  const body = Buffer.alloc(0x2140); body.writeFloatLE(observedValue, 0x50);
  const identity = Buffer.alloc(32); identity.writeBigUInt64LE(7n);
  const regions = version === 5 ? [
    ['diagnostic.lifetime', marker], ['lifetime.effect.6.root', root],
    ['lifetime.effect.6.graph.0', Buffer.from('graph')], ['lifetime.shot.7.identity', identity],
    ['observed.shot.7.body', body], ['observed.shot.7.model.joints', Buffer.alloc(0xC0)],
  ] : [];
  const header = Buffer.alloc(96);
  header.writeUInt32LE(0x50445355); header.writeUInt32LE(version, 4);
  header.writeUInt32LE(96, 8); header.writeUInt32LE(2, 12); header.writeInt32LE(3, 16);
  header.writeUInt32LE(1, 20);
  const chunks = [header];
  let previous;
  for (const number of [2, 3]) {
    const data = Buffer.concat(regions.map(([, bytes]) => bytes));
    const manifest = regions.map(([name, bytes]) => {
      const entry = Buffer.alloc(112);
      entry.writeBigUInt64LE(BigInt(bytes.length), 8); entry.write(name, 48, 63);
      return entry;
    });
    let encoded = data, encoding = 0;
    if (previous && data.length) {
      // Exercise XOR-RLE reconstruction with the new regions in the payload.
      const tag = []; let value = data.length * 2;
      do { const byte = value & 127; value >>>= 7; tag.push(byte | (value ? 128 : 0)); } while (value);
      encoded = Buffer.from(tag); encoding = 1;
    }
    const frame = Buffer.alloc(72);
    frame.writeInt32LE(number); frame.writeUInt32LE(regions.length, 12);
    frame.writeUInt32LE(version === 5 ? 1 : 0, 16); frame.writeUInt32LE(version === 5 ? 1 : 0, 20);
    frame.writeBigUInt64LE(BigInt(data.length), 48); frame.writeBigUInt64LE(BigInt(encoded.length), 56);
    frame[64] = encoding;
    chunks.push(frame, ...manifest, encoded); previous = data;
  }
  const result = Buffer.concat(chunks);
  return result.buffer.slice(result.byteOffset, result.byteOffset + result.byteLength);
}

test('version 5 decodes named lifetime regions without consuming legacy shot records', () => {
  const { context, run } = viewer(); context.file = fixture();
  const dump = run('parseUsdp(file)');
  assert.equal(dump.version, 5); assert.equal(dump.frames.length, 2);
  assert.equal(dump.frames[1].shotCount, 1); assert.equal(dump.frames[1].shots.length, 0);
  assert.equal(dump.frames[1].regions.length, 6);
  assert.equal(run('lifetimeRegions(parseUsdp(file).frames[1]).length'), 5);
});

test('inspector clearly separates retained snapshots from dump-time shot models', () => {
  const { context, run } = viewer(); context.file = fixture();
  const html = run('state.dumps=[parseUsdp(file)]; lifetimeInspector(3, [])');
  assert.match(html, /Saved effect #6/); assert.match(html, /Saved shot #7/);
  assert.match(html, /Dump-time shot #7/); assert.match(html, /frame 88/);
  assert.match(html, /not historical snapshots/); assert.match(html, /model.joints: 192 bytes/);
  assert.match(html, /1.0000, 0.0000, 0.0000/);
});

test('dump-time observation differences are excluded from snapshot differences', () => {
  const { context, run } = viewer(); context.a = fixture(5, 1); context.b = fixture(5, 200);
  assert.equal(run('compareFrames(parseUsdp(a).frames[1],parseUsdp(b).frames[1]).length'), 0);
  assert.equal(run('historicalRegions(parseUsdp(a).frames[1]).length'), 3);
});

test('legacy version 4 remains readable and truncation is rejected', () => {
  const { context, run } = viewer(); context.file = fixture(4);
  assert.equal(run('parseUsdp(file).frames.length'), 2);
  context.file = fixture().slice(0, -1);
  assert.throws(() => run('parseUsdp(file)'), /Truncated/);
});

test('effect origins use saved world transforms rather than attachment-local positions', () => {
  const { context, run } = viewer(); context.file = fixture();
  assert.equal(run('const dump=parseUsdp(file); decodeEffects(dump,dump.frames[1])[0].position.join(",")'), '600,150,-7');
  assert.equal(run('decodeEffects(dump,dump.frames[1])[0].id'), '6');
  assert.equal(run('decodeEffects(dump,dump.frames[1])[0].active'), true);
  const html = run('state.dumps=[dump]; lifetimeInspector(3,[])');
  assert.match(html, /World origin/); assert.match(html, /600.0000, 150.0000, -7.0000/);
  assert.match(html, /Local position/); assert.match(html, /<td>42<\/td>/);
});

test('effects render in an effects-only frame and the toggle hides them', () => {
  const { context, elements, drawCalls, run } = viewer(); context.file = fixture();
  elements.get('showEffects').checked = true;
  run(`state.dumps=[parseUsdp(file)];
    const frame=state.dumps[0].byFrame.get(3);
    const root=frame.regions.find(region=>region.name==='lifetime.effect.6.root');
    regionView(frame,root).setFloat32(0xF0,0,true);
    regionView(frame,root).setFloat32(0xF4,0,true);
    state.dumps.push({...state.dumps[0],role:"client"}); drawArena(3)`);
  assert.ok(drawCalls.some(call => call[0] === 'fillText' && call[1] === 'H effect #6'));
  assert.ok(drawCalls.some(call => call[0] === 'fillText' && call[1] === 'C effect #6'));
  assert.ok(drawCalls.some(call => call[0] === 'fill'));
  assert.ok(drawCalls.some(call => call[0] === 'closePath'));
  assert.equal(run('state.arenaView.bounds[0]'), 0);
  assert.match(elements.get('effectLegend').textContent, /2 saved/);
  elements.get('showEffects').checked = false; drawCalls.length = 0;
  run('drawArena(3)');
  assert.ok(!drawCalls.some(call => call[0] === 'fillText' && /effect #6/.test(call[1])));
  assert.equal(elements.get('effectLegend').textContent, 'Effect origins hidden');
});

test('distant effects do not change camera framing or draw offscreen markers', () => {
  const { context, elements, drawCalls, run } = viewer(); context.file = fixture();
  run(`state.dumps=[parseUsdp(file)];
    decodeShots=()=>[{position:[40,20,0],collisions:[],role:'host',spawnSequence:1}];
    const frame=state.dumps[0].byFrame.get(3);
    const root=frame.regions.find(region=>region.name==='lifetime.effect.6.root');
    regionView(frame,root).setFloat32(0xF0,1000000,true);
    regionView(frame,root).setFloat32(0xF4,-1000000,true);
    drawArena(3)`);
  const before=run('JSON.stringify([state.arenaView.camera,state.arenaView.bounds,state.arenaView.scale])');
  elements.get('showEffects').checked = true; drawCalls.length = 0;
  run('drawArena(3)');
  assert.equal(run('JSON.stringify([state.arenaView.camera,state.arenaView.bounds,state.arenaView.scale])'), before);
  assert.ok(!drawCalls.some(call => call[0] === 'fillText' && /effect #6/.test(call[1])));
});

test('effect decoder ignores dump-time observations, short roots and nonfinite origins', () => {
  const { context, run } = viewer(); context.file = fixture();
  assert.equal(run(`const dump=parseUsdp(file), frame=dump.frames[1];
    const root=frame.regions.find(region=>region.name==='lifetime.effect.6.root');
    frame.regions.push({...root,name:'observed.effect.9.root'});
    root.size=0xF0;
    decodeEffects(dump,frame).length`), 0);
  assert.equal(run(`const invalid=parseUsdp(file), bad=invalid.frames[1];
    const region=bad.regions.find(region=>region.name==='lifetime.effect.6.root');
    regionView(bad,region).setFloat32(0xF0,NaN,true);
    decodeEffects(invalid,bad).length`), 0);
  context.file = fixture(4);
  assert.equal(run('const legacy=parseUsdp(file); decodeEffects(legacy,legacy.frames[1]).length'), 0);
  assert.equal(run('decodeEffects({},undefined).length'), 0);
});
