const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');

function viewer() {
  const elements = new Map();
  const document = {
    querySelector(id) {
      if (!elements.has(id)) elements.set(id, {
        value: '', textContent: '', innerHTML: '',
        classList: { add() {}, remove() {} },
        getBoundingClientRect() { return {}; },
      });
      return elements.get(id);
    },
    addEventListener() {},
  };
  const context = vm.createContext({ document, console: { log() {} },
    performance: { now: () => 0 }, addEventListener() {},
    clearTimeout() {}, cancelAnimationFrame() {},
  });
  const html = fs.readFileSync(path.join(__dirname, '../rollback-profile-viewer.html'), 'utf8');
  vm.runInContext(html.match(/<script>([\s\S]*?)<\/script>/)[1], context);
  return { context, elements, run: (code) => vm.runInContext(code, context) };
}

const capture = `LAGCAPTURE duration_ms=15000; elapsed wall time, not CPU time
LAGDIAG tid=424 net_frame=423 window_ms=5000 build=abc dirty=1 units=count/avg_ms/max_ms/over25 frame=300/16.660/24.920/0 pad_detect=300/0.099/6.402/0 report_qpc=9007199254740993 self_avg_ms{ frame=0.006 pad_detect=0.001 }
LAGDIAG tid=492 net_frame=-1 window_ms=5001 units=count/avg_ms/max_ms/over25 render_gpu_fence=300/1.633/3.524/0 report_qpc=9007199254740993 self_avg_ms{ }
LAGWORST tid=424 net_frame=222 duration_ms=24.920 report_qpc=9007199254740993 units=inclusive_ms/self_ms frame=24.920/0.004
LAGSLOW tid=424 metric=pad_detect duration_ms=6.402 start_qpc=9007199254740990 end_qpc=9007199254740992 qpc_hz=10000000 report_qpc=9007199254740993
LAGDIAG tid=424 net_frame=424 window_ms=17 diag_enqueue=1/0.002/0.002/0 report_qpc=9007199254740994 self_avg_ms{ }
LAGCAPTURE stopped; dropped_reports_total=0; inactive/frozen thread tails may be incomplete`;

test('parses capture metadata, metrics, self times and exact QPC identifiers', () => {
  const { context, run } = viewer();
  context.text = capture.replaceAll('\n', '\r\n');
  const parsed = run('parseLagCapture(text)');
  assert.equal(parsed.reports.length, 3);
  assert.equal(parsed.headers.length, 2);
  assert.equal(parsed.reports[0].fields.report_qpc, '9007199254740993');
  assert.equal(parsed.reports[0].metrics[0].values[0], 300);
  assert.equal(parsed.reports[0].metrics[0].self, 0.006);
  assert.equal(parsed.reports[1].metrics[0].self, undefined);
  assert.equal(parsed.worst[0].metrics[0].values[1], 0.004);
});

test('renders pasted reports and associates events by both thread and report', () => {
  const { context, elements, run } = viewer();
  context.text = capture;
  run('loadText(text)');
  assert.match(elements.get('#lagSummary').textContent, /3 reports \| 2 threads/);
  assert.match(elements.get('#lagMetrics').innerHTML, /16.660/);
  assert.match(elements.get('#lagWorst').innerHTML, /24.920/);
  assert.match(elements.get('#lagSlow').innerHTML, /pad_detect/);
  for (const value of ['1', '2']) {
    elements.get('#lagReport').value = value;
    run('renderLagReport()');
    assert.match(elements.get('#lagWorst').innerHTML, /No samples/);
    assert.match(elements.get('#lagSlow').innerHTML, /No samples/);
  }
});

test('rejects missing reports and malformed metrics', () => {
  const { context, run } = viewer();
  for (const text of ['LAGCAPTURE stopped', 'LAGDIAG frame=1/2/3/0',
    'LAGDIAG tid=1 frame=1/NaN/3/0', 'LAGDIAG tid=1 frame=1/2/3']) {
    context.text = text;
    assert.throws(() => run('parseLagCapture(text)'));
  }
});

test('frame percentage bars weight invocation counts and handle missing denominators', () => {
  const { context, elements, run } = viewer();
  context.text = `LAGDIAG tid=1 frame=10/20/25/0 unit_draw=60/1/2/0 report_qpc=1 self_avg_ms{ unit_draw=0.5 }
LAGWORST tid=1 duration_ms=25 report_qpc=1 unit_draw=5/2
LAGDIAG tid=2 scene_gap=10/20/25/0 render=10/5/8/0 self_avg_ms{ }
LAGDIAG tid=3 hooklog=1/2/2/0 self_avg_ms{ }
LAGDIAG tid=4 frame=0/0/0/0 self_avg_ms{ }`;
  run('loadText(text)');
  assert.match(elements.get('#lagMetrics').innerHTML, /30.00%/);
  assert.match(elements.get('#lagMetrics').innerHTML, /15.00%/);
  assert.match(elements.get('#lagWorst').innerHTML, /20.00%/);
  assert.match(elements.get('#lagWorst').innerHTML, /8.00%/);
  assert.match(elements.get('#lagBreakdown').innerHTML, /Drawing game objects/);
  assert.match(elements.get('#lagBreakdown').innerHTML, /3.00 ms \/ frame &middot; 15.0%/);
  assert.match(elements.get('#lagWorstBreakdown').innerHTML, /2.00 ms  &middot; 8.0%/);
  elements.get('#lagTimingMode').value = 'inclusive';
  run('renderLagReport()');
  assert.match(elements.get('#lagBreakdown').innerHTML, /6.00 ms \/ frame &middot; 30.0%/);
  assert.match(elements.get('#lagBreakdownMeta').textContent, /Do not add/);
  elements.get('#lagReport').value = '1';
  run('renderLagReport()');
  assert.match(elements.get('#lagPercentMeta').textContent, /scene_gap total/);
  assert.match(elements.get('#lagMetrics').innerHTML, /25.00%/);
  assert.match(elements.get('#lagMetrics').innerHTML, /class="lag-percent">-</);
  for (const value of ['2', '3']) {
    elements.get('#lagReport').value = value;
    run('renderLagReport()');
    assert.match(elements.get('#lagPercentMeta').textContent, /unavailable/);
    assert.doesNotMatch(elements.get('#lagMetrics').innerHTML, /NaN|Infinity|width:/);
  }
});

test('escapes untrusted metadata and preserves JSON import after a capture', () => {
  const { context, elements, run } = viewer();
  context.text = capture.replace('net_frame=423', 'net_frame=<img>');
  run('loadText(text)');
  assert.ok(!elements.get('#lagReport').innerHTML.includes('<img>'));
  assert.match(elements.get('#lagReport').innerHTML, /&lt;img&gt;/);
  run('render = () => {}; loadText(JSON.stringify([{type: "rollback", frame_count: 1, total_ms: 2}]), "test.json")');
  assert.equal(run('rollbacks.length'), 1);
  assert.equal(elements.get('#fileName').textContent, 'test.json');
});

test('documents every metric in the current diagnostic emitter', () => {
  const { run } = viewer();
  const source = fs.readFileSync(path.join(__dirname, '../UMVC3Hook/code/lag_diagnostics.cpp'), 'utf8');
  const names = [...source.match(/constexpr const char\* names\[\] = \{([\s\S]*?)\};/)[1].matchAll(/"(\w+)"/g)].map((m) => m[1]);
  const descriptions = run('lagDescriptions');
  for (const name of names) assert.ok(descriptions[name], `Missing description: ${name}`);
  for (const name of ['frame_gap', 'scene_gap', 'tid', 'net_frame', 'window_ms', 'build', 'dirty',
    'units', 'resets', 'samples', 'corrections', 'bench_qpc', 'native_outside', 'reentry',
    'overlay_order', 'scope_overflow', 'report_qpc', 'dropped', 'writer_prev_ms', 'render_batches',
    'render_jobs', 'render_worker_sum', 'scene_drawable_sum', 'duration_ms', 'start_qpc',
    'end_qpc', 'qpc_hz', 'metric', 'self_avg_ms', 'dropped_reports_total']) {
    assert.ok(descriptions[name], `Missing field description: ${name}`);
  }
});

test('renders new overlay metrics, descriptions, metadata and unknown fields safely', () => {
  const { context, elements, run } = viewer();
  context.text = `LAGDIAG tid=832 net_frame=-1 window_ms=5001 build=<img> dirty=1 scene_gap=300/16.667/25.370/1 overlay_win32_mouse=301/0.004/0.025/0 overlay_mouse_child=301/0.001/0.022/0 future_metric=1/1/1/0 render_worker_sum=905 self_avg_ms{ overlay_win32_mouse=0.001 overlay_mouse_child=0.001 }`;
  run('loadText(text)');
  assert.match(elements.get('#lagMetrics').innerHTML, /Checking the child-window relationship/);
  assert.match(elements.get('#lagBreakdown').innerHTML, /Updating the overlay mouse/);
  assert.match(elements.get('#lagMetadata').innerHTML, /not a unique thread count/);
  assert.match(elements.get('#lagMetadata').innerHTML, /&lt;img&gt;/);
  assert.match(elements.get('#lagFieldGuide').innerHTML, /overlay_win32_mouse/);
  assert.match(elements.get('#lagMetrics').innerHTML, /Unrecognized metric/);
});
