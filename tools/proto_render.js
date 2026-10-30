// tools/proto_render.js SCEN.json OUT.f32 : renders the prototype's own engine classes (BuchlaEngine / MoogEngine / ProphetEngine / DrumEngine, extracted
// from the "FM-1 Engine Test Bench" HTML, no DOM) with a parameter set, a modulation setup and note events; float32 mono 44.1 kHz.
// SCEN = {"html": path (optional), "eng": "Vactrol|Minimoog|Prophet|Drums", "p": {param: value}, "events": [[sec, "on"|"off"|"hit", n, vel?]], "dur": sec}
// The modulation uses the prototype's own keys: p.m0t = destination id ("freq" ...), p.m0a = amount (already squared by the slider), p.m0s = shape,
// p.m0r = rate Hz, p.m0y = sync; m3* / m11* = S&H (m3l = smoothing), m1, m2, m10 = LFO 2, 3, 4.
const fs = require('fs'), vm = require('vm');
const sc = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
const html = fs.readFileSync(sc.html || '/root/.claude/projects/-home-claude/9ce91c03-575c-5b77-9332-be0b542a3ae1/tool-results/artifact-6a194d38-1793292356-7d6c.html', 'utf8');
const a = html.indexOf('const TAU=Math.PI*2'), b = html.indexOf('const tb=new Float32Array(8192)');
const ctx = vm.createContext({ performance, Math, console, Float32Array, Array, Object, Number, JSON, window: {}, document: { getElementById: () => ({}) } });
const c = html.indexOf('var FXD=[["Overdrive"'), d = html.indexOf('const PRO=["pla"');      // + the destination tables (MP ranges of every engine's destinations)
vm.runInContext(html.slice(a, b) + '\n' + html.slice(c, d) + '\nthis.E={BuchlaEngine,MoogEngine,ProphetEngine,DrumEngine,MP};', ctx);
const C = { Vactrol: 'BuchlaEngine', Minimoog: 'MoogEngine', Prophet: 'ProphetEngine', Drums: 'DrumEngine' }[sc.eng];
const e = new ctx.E[C](); e.init(44100);
for (const k in sc.p) e.setParam(k, sc.p[k]);
const N = Math.round(sc.dur * 44100), out = new Float32Array(N), ev = (sc.events || []).slice().sort((x, y) => x[0] - y[0]);
let k = 0; const probe = [];
for (let s = 0; s < N; s += 128) {
  const n = Math.min(128, N - s);
  while (k < ev.length && ev[k][0] * 44100 <= s) { const x = ev[k++]; if (x[1] === 'on') e.noteOn(x[2], x[3] === undefined ? 1 : x[3]); else if (x[1] === 'off') e.noteOff(); else if (x[1] === 'hit') e.hit(x[2], 1); else if (x[1] === 'set') e.setParam(x[2], x[3]); }
  e.render(out.subarray(s, s + n), n);
  if (sc.probe) probe.push([s / 44100].concat(sc.probe.map(q => e.q ? e.q[q] : NaN)));
}
fs.writeFileSync(process.argv[3], Buffer.from(out.buffer));
if (sc.probe) fs.writeFileSync(process.argv[3] + '.probe.json', JSON.stringify(probe));
