// Runs the actual embedded page script in a small DOM/HTTP harness, no npm dependencies.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const html = fs.readFileSync(path.join(__dirname, '../components/charger/web/index.html'), 'utf8');
const script = html.split('<script>')[1].split('</script>')[0];
const elements = new Map(), calls = [];
const context = new Proxy({}, {get: (obj, key) => obj[key] || (() => {}), set: (obj, key, val) => (obj[key] = val, true)});
function element(id) {
    return {id, style: {}, value: '', textContent: '', innerText: '', width: 960, height: 720,
        clientWidth: 640, clientHeight: 200, children: [], handlers: {},
        addEventListener(event, fn) {this.handlers[event] = fn;},
        appendChild(child) {this.children.push(child);}, getContext() {return context;},
        getBoundingClientRect() {return {left: 0, top: 0, width: 640, height: 480};}, click() {}};
}
for (const match of html.matchAll(/\bid="([^"]+)"/g)) elements.set(match[1], element(match[1]));
function state(ch) {return {ch, v: ch===1?54.6:48, a: 2, w: 100, sv: 54.6, sa: 15, tr: .5, pm: 819,
    mode: 0, preset: 1, run: false, runActive: false, stopped: true, rs: true, ready: true,
    ah: 0, wh: 0, cnt: 0, trip: '', tripN: 0, nextWriteMs: 0, buzzer: false, st: 'IDLE', addr: 0,
    ap: true, wf: false, ip: '192.168.4.1', lock: false};}
let failure = false, hold = null;
const sandbox = {
    document: {getElementById(id) {assert(elements.has(id), `Missing DOM element ${id}`); return elements.get(id);},
        createElement(tag) {return element(tag);}, querySelectorAll() {return [];}, documentElement: {style: {setProperty() {}}}},
    window: {innerWidth: 1000, addEventListener() {}}, location: {origin: 'http://192.168.4.1:8899'},
    sessionStorage: {getItem() {return null;}, setItem() {}}, URL, URLSearchParams, DOMException,
    setInterval() {}, setTimeout() {}, clearTimeout() {}, requestAnimationFrame() {}, confirm: () => true,
    fetch: async (url, opt={}) => {
        calls.push({url, opt});
        const u = new URL(url, 'http://test');
        if (hold) await hold;
        let data = 'OK';
        if (u.pathname === '/state') data = state(Number(u.searchParams.get('ch')));
        if (u.pathname === '/theme') data = Array(25).fill('#000000');
        if (u.pathname === '/settings') data = {port: 8899, maxV: 100, maxA: 50, maxP: 3000, rampMs: 1000, rampSt: .05, addr: 0};
        if (u.pathname === '/presets') data = [[48,10],[54.6,12.5],[60,15],[67.2,8],[71.4,5]];
        if (u.pathname === '/wifiget') data = {ssid: 'test'};
        return {ok: !failure, status: failure ? 409 : 200, text: async () => failure?'Stop first':String(data), json: async () => data};
    }
};
vm.createContext(sandbox);
vm.runInContext(script, sandbox, {filename: 'embedded-index.html'});
const flush = () => new Promise(resolve => setImmediate(resolve));
(async () => {
    await flush();
    // All inline handlers must resolve (catches partial UI migrations).
    for (const m of html.matchAll(/on(?:click|change)="([a-zA-Z_]\w*)\(/g)) assert.equal(typeof sandbox[m[1]], 'function', m[1]);
    assert(sandbox.st && sandbox.st.ch === 1);
    sandbox.drawScreen(context); sandbox.drawWave(); sandbox.openColor(); sandbox.openPick(); sandbox.pickSync();
    sandbox.openCfg(); sandbox.openPresets(); await flush();
    sandbox.selectChannel(2); await flush(); assert.equal(sandbox.st.ch, 2);
    sandbox.pendAction = {id: 'run'}; sandbox.cfYes(); await flush();
    let sent = calls.findLast(c => c.url.startsWith('/btn'));
    assert(sent.url.includes('id=start') && sent.url.includes('ch=2')); assert.equal(sent.opt.method, 'POST');
    sandbox.stopNow(); await flush(); sent = calls.findLast(c => c.url.startsWith('/btn'));
    assert(sent.url.includes('id=stop') && sent.url.includes('ch=2'));
    elements.get('pin').value = '1234'; await sandbox.api('/buzzer?on=1&pin=1234');
    assert(!calls.at(-1).url.includes('pin=')); assert.equal(calls.at(-1).opt.headers['X-Charger-PIN'], '1234');
    failure = true; await assert.rejects(sandbox.api('/btn?id=start'), /Stop first/); failure = false;
    let release; hold = new Promise(r => release=r);
    const pending = sandbox.api('/state'); sandbox.selectedChannel = 1; release(); hold = null;
    await assert.rejects(pending, /Channel changed/);
    sandbox.selectedChannel = 2;
    sandbox.openNum({id:'setV',name:'Voltage',range:[1,100]});elements.get('npVal').value = '50';
    sandbox.npConfirm(); await flush(); assert(calls.some(c => c.url.includes('/setparam?field=setV&val=50') && c.opt.method==='POST'));
    sandbox.cfgSave(); await flush(); assert(calls.some(c => c.url.startsWith('/settings') && c.opt.method==='POST'));
    console.log('PASS: embedded JS, DOM handlers, two-channel routing, explicit start/stop, PIN header, HTTP errors, stale response rejection, settings and canvas');
})().catch(err => {console.error(err); process.exitCode = 1;});
