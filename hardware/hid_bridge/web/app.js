// The web interface's page (web_server.cpp). Shows the mirrored phone (H.264 over the WebSocket, decoded with
// WebCodecs) and sends mouse and keyboard as small messages (src/web/web_input.h); the board turns them into its
// own HID commands. Everything that decides where a point lands on the phone is on the board.
'use strict';

const T = {
  en: {
    loading: 'Connecting to the board…', settings: 'Settings', logout: 'Sign out', theme: 'Theme',
    nopwTitle: 'Set a password first',
    nopw1: 'The web interface controls your phone, so it needs a password. It is set on the board\'s Wi-Fi setup page, together with the Wi-Fi:',
    nopw2: 'Hold the BOOT button on the board for 10 seconds. It forgets its Wi-Fi and opens its setup hotspot (name and password on its display).',
    nopw3: 'Join the hotspot; the setup page opens. Choose the Wi-Fi again and fill in "Password for the web interface" twice.',
    nopw4: 'Press BOOT to confirm. Then open this page again.',
    nopw5: 'The password is stored on the board only as a salted hash.',
    signIn: 'Sign in', password: 'Password', signInGo: 'Sign in',
    loginHint: 'One browser at a time: signing in here signs out any other.',
    wrong: 'Wrong password.', locked: 'Too many wrong passwords. Try again in {s} s.', failed: 'The board did not answer.',
    control: 'Control', take: 'Take control', release: 'Release control',
    ctlYou: 'You control the phone. The computer\'s commands wait until you release control (or 5 minutes after your last input).',
    ctlCli: 'In use by a computer (its command line). Take control to drive the phone from here.',
    ctlView: 'View only. Take control to drive the phone.',
    ctlStop: 'Stopped with the BOOT button: no input reaches the phone until it is pressed again.',
    ctlWait: 'Waiting for control…', viewOnly: 'View only', stopped: 'Stopped',
    system: 'Phone', home: 'Home', switcher: 'App switcher', cc: 'Control Center', nc: 'Notifications',
    kbdToggle: 'On-screen keyboard', capture: 'Capture keys', captureOff: 'Stop capturing',
    captureHint: 'Keys you press go to the phone (Esc too). Click "Stop capturing" to end.',
    typing: 'Keyboard', send: 'Type',
    mouseHint: 'Click: tap. Drag: swipe. Right click: long press. Wheel: scroll.',
    stream: 'Stream', airplay: 'AirPlay', frameAge: 'Frame age', rate: 'Rate', phoneUsb: 'Phone (USB)',
    apIdle: 'Start Screen Mirroring on the iPhone: Control Center > Screen Mirroring > {name}.',
    apConnected: 'AirPlay connected, no picture yet. Start Screen Mirroring: Control Center > Screen Mirroring > {name}.',
    keyframe: 'Waiting for a keyframe…',
    keyframeLong: 'Still waiting for a keyframe ({s} s). Touching the phone often brings one; restarting Screen Mirroring always does.',
    frozen: 'The board\'s picture buffer is full. Restart Screen Mirroring on the iPhone.',
    noCodec: 'This browser cannot decode the video (it needs WebCodecs: a current Chrome, Edge, Safari or Firefox).',
    still: 'no new frames for {s} s (a still screen sends none)',
    disconnected: 'Connection to the board lost.', reconnect: 'Reconnect', replaced: 'Another tab or browser took over.',
    usbOk: 'connected', usbAsleep: 'asleep (locked?)', usbOff: 'not connected',
    idle: 'idle', connected: 'connected', streaming: 'mirroring',
    errControl: 'You do not have control.', errStopped: 'Stopped with the BOOT button.', errBusy: 'Too much input at once.',
    noAbs: 'This firmware has no absolute pointer: clicks on the picture do not work (build with ABS_POINTER).',
    board: 'Board', boardName: 'Name (Screen Mirroring and the command line show it with its id)',
    kbdLayout: 'Keyboard layout of the phone', language: 'Language', langAuto: 'Like the browser', save: 'Save',
    saved: 'Saved.', savedRestart: 'Saved. The new name is used after a restart.', badName: '1 to 20 letters, digits, spaces, . _ -',
    network: 'Network', ssid: 'Wi-Fi', ip: 'Address', rssi: 'Signal', host: 'Name', fp: 'Certificate SHA-256',
    memory: 'Free memory', uptime: 'Running for',
    certHint: 'The board made its own certificate, so the browser warned the first time. To be sure it is this board, compare the fingerprint above with the one the browser shows for the page.',
    changePw: 'Change password', pwOld: 'Current password', pwNew: 'New password (8 to 64 characters)', pwNew2: 'Repeat the new password',
    pwGo: 'Change', pwPress: 'Now press the BOOT button on the board to confirm (30 s).', pwSaved: 'Password changed.',
    pwExpired: 'Not confirmed in time; the old password stays.', pwMismatch: 'The new passwords differ.',
    pwWeak: 'The new password needs 8 to 64 characters.', pwBusy: 'The board is waiting for another confirmation. Try again shortly.',
    firmware: 'Firmware', fwNow: 'Running {fw}.', otaBin: 'Firmware image (.bin)', otaSig: 'Its signature (.sig)',
    otaGo: 'Update', check: 'Check for updates', otaNoVersion: 'This file is not a Tentacle firmware.',
    otaSending: 'Sending {p}%…', otaChecking: 'Checking…', otaDone: 'Done: restarting into {v}. This page reloads in 20 s.',
    otaNeedSig: 'Choose the .sig file too (a release build takes signed images only).',
    otaHint: 'After an update the board restarts: start Screen Mirroring on the iPhone again.',
    checkStub: 'Checking for updates is not possible yet: the project\'s repository is private. Get new releases from the maintainer and upload them here.',
    restart: 'Restart the board', restartAsk: 'Restart the board now?', restarting: 'Restarting…', close: 'Close',
    themeAuto: 'Theme: system', themeDark: 'Theme: dark', themeLight: 'Theme: light',
  },
  de: {
    loading: 'Verbinde mit dem Board…', settings: 'Einstellungen', logout: 'Abmelden', theme: 'Darstellung',
    nopwTitle: 'Zuerst ein Passwort setzen',
    nopw1: 'Die Weboberfläche steuert dein iPhone und braucht deshalb ein Passwort. Es wird auf der WLAN-Einrichtungsseite des Boards gesetzt, zusammen mit dem WLAN:',
    nopw2: 'Den BOOT-Taster am Board 10 Sekunden halten. Es vergisst sein WLAN und öffnet sein Einrichtungs-WLAN (Name und Passwort stehen im Display).',
    nopw3: 'Mit dem Einrichtungs-WLAN verbinden; die Einrichtungsseite öffnet sich. Das WLAN erneut wählen und „Passwort für die Weboberfläche“ zweimal ausfüllen.',
    nopw4: 'Zum Bestätigen BOOT drücken. Danach diese Seite erneut öffnen.',
    nopw5: 'Das Board speichert das Passwort nur als gesalzenen Hash.',
    signIn: 'Anmelden', password: 'Passwort', signInGo: 'Anmelden',
    loginHint: 'Immer nur ein Browser: Eine Anmeldung hier meldet jeden anderen ab.',
    wrong: 'Falsches Passwort.', locked: 'Zu viele falsche Passwörter. Neuer Versuch in {s} s.', failed: 'Das Board hat nicht geantwortet.',
    control: 'Steuerung', take: 'Steuerung übernehmen', release: 'Steuerung abgeben',
    ctlYou: 'Du steuerst das iPhone. Befehle des Computers warten, bis du die Steuerung abgibst (oder 5 Minuten nach deiner letzten Eingabe).',
    ctlCli: 'Ein Computer (seine Kommandozeile) steuert gerade. Übernimm die Steuerung, um von hier zu steuern.',
    ctlView: 'Nur ansehen. Übernimm die Steuerung, um das iPhone zu bedienen.',
    ctlStop: 'Mit dem BOOT-Taster gestoppt: Keine Eingabe erreicht das iPhone, bis er erneut gedrückt wird.',
    ctlWait: 'Warte auf die Steuerung…', viewOnly: 'Nur ansehen', stopped: 'Gestoppt',
    system: 'iPhone', home: 'Home', switcher: 'App-Umschalter', cc: 'Kontrollzentrum', nc: 'Mitteilungen',
    kbdToggle: 'Bildschirmtastatur', capture: 'Tasten erfassen', captureOff: 'Erfassen beenden',
    captureHint: 'Gedrückte Tasten gehen ans iPhone (auch Esc). „Erfassen beenden“ klicken zum Aufhören.',
    typing: 'Tastatur', send: 'Tippen',
    mouseHint: 'Klick: tippen. Ziehen: wischen. Rechtsklick: lange drücken. Mausrad: scrollen.',
    stream: 'Übertragung', airplay: 'AirPlay', frameAge: 'Bildalter', rate: 'Rate', phoneUsb: 'iPhone (USB)',
    apIdle: 'Bildschirmsynchronisierung am iPhone starten: Kontrollzentrum > Bildschirmsynchronisierung > {name}.',
    apConnected: 'AirPlay verbunden, noch kein Bild. Bildschirmsynchronisierung starten: Kontrollzentrum > Bildschirmsynchronisierung > {name}.',
    keyframe: 'Warte auf ein Schlüsselbild…',
    keyframeLong: 'Noch kein Schlüsselbild ({s} s). Eine Berührung am iPhone bringt oft eins; ein Neustart der Bildschirmsynchronisierung immer.',
    frozen: 'Der Bildpuffer des Boards ist voll. Bildschirmsynchronisierung am iPhone neu starten.',
    noCodec: 'Dieser Browser kann das Video nicht dekodieren (er braucht WebCodecs: ein aktueller Chrome, Edge, Safari oder Firefox).',
    still: 'seit {s} s keine neuen Bilder (ein ruhiger Bildschirm sendet keine)',
    disconnected: 'Verbindung zum Board verloren.', reconnect: 'Neu verbinden', replaced: 'Ein anderer Tab oder Browser hat übernommen.',
    usbOk: 'verbunden', usbAsleep: 'schläft (gesperrt?)', usbOff: 'nicht verbunden',
    idle: 'aus', connected: 'verbunden', streaming: 'spiegelt',
    errControl: 'Du hast nicht die Steuerung.', errStopped: 'Mit dem BOOT-Taster gestoppt.', errBusy: 'Zu viele Eingaben auf einmal.',
    noAbs: 'Diese Firmware hat keinen absoluten Zeiger: Klicks ins Bild wirken nicht (mit ABS_POINTER bauen).',
    board: 'Board', boardName: 'Name (Bildschirmsynchronisierung und Kommandozeile zeigen ihn mit seiner Kennung)',
    kbdLayout: 'Tastaturbelegung des iPhones', language: 'Sprache', langAuto: 'Wie der Browser', save: 'Speichern',
    saved: 'Gespeichert.', savedRestart: 'Gespeichert. Der neue Name gilt nach einem Neustart.', badName: '1 bis 20 Buchstaben, Ziffern, Leerzeichen, . _ -',
    network: 'Netzwerk', ssid: 'WLAN', ip: 'Adresse', rssi: 'Signal', host: 'Name', fp: 'Zertifikat SHA-256',
    memory: 'Freier Speicher', uptime: 'Läuft seit',
    certHint: 'Das Board hat sein eigenes Zertifikat erstellt, deshalb hat der Browser beim ersten Mal gewarnt. Um sicherzugehen, dass es dieses Board ist: den Fingerabdruck oben mit dem vergleichen, den der Browser für die Seite anzeigt.',
    changePw: 'Passwort ändern', pwOld: 'Aktuelles Passwort', pwNew: 'Neues Passwort (8 bis 64 Zeichen)', pwNew2: 'Neues Passwort wiederholen',
    pwGo: 'Ändern', pwPress: 'Jetzt zum Bestätigen den BOOT-Taster am Board drücken (30 s).', pwSaved: 'Passwort geändert.',
    pwExpired: 'Nicht rechtzeitig bestätigt; das alte Passwort bleibt.', pwMismatch: 'Die neuen Passwörter sind verschieden.',
    pwWeak: 'Das neue Passwort braucht 8 bis 64 Zeichen.', pwBusy: 'Das Board wartet auf eine andere Bestätigung. Gleich noch einmal versuchen.',
    firmware: 'Firmware', fwNow: 'Läuft mit {fw}.', otaBin: 'Firmware-Datei (.bin)', otaSig: 'Ihre Signatur (.sig)',
    otaGo: 'Aktualisieren', check: 'Nach Updates suchen', otaNoVersion: 'Diese Datei ist keine Tentacle-Firmware.',
    otaSending: 'Sende {p} %…', otaChecking: 'Prüfe…', otaDone: 'Fertig: Neustart mit {v}. Diese Seite lädt in 20 s neu.',
    otaNeedSig: 'Bitte auch die .sig-Datei wählen (ein Release-Build nimmt nur signierte Images).',
    otaHint: 'Nach einem Update startet das Board neu: die Bildschirmsynchronisierung am iPhone danach neu starten.',
    checkStub: 'Die Suche nach Updates geht noch nicht: Das Repository des Projekts ist privat. Neue Versionen vom Maintainer holen und hier hochladen.',
    restart: 'Board neu starten', restartAsk: 'Das Board jetzt neu starten?', restarting: 'Starte neu…', close: 'Schließen',
    themeAuto: 'Darstellung: System', themeDark: 'Darstellung: dunkel', themeLight: 'Darstellung: hell',
  },
};

const $ = (id) => document.getElementById(id);
let L = T.en, state = {}, st = {}, ws = null, wsOpenedAt = 0, retries = 0;

function t(key, vars) {
  let s = L[key] || T.en[key] || key;
  for (const k in vars || {}) s = s.replace('{' + k + '}', vars[k]);
  return s;
}

function store(k, v) { try { if (v === undefined) return localStorage.getItem(k); localStorage.setItem(k, v); } catch (e) { return null; } }

function translate() {
  document.documentElement.lang = L === T.de ? 'de' : 'en';
  document.querySelectorAll('[data-i]').forEach((e) => { e.textContent = t(e.dataset.i); });
  showTheme();
}

// ---- Theme ---------------------------------------------------------------------------------------------------------
const THEMES = ['', 'dark', 'light'];
function showTheme() {
  const th = store('theme') || '';
  if (th) document.documentElement.dataset.theme = th; else delete document.documentElement.dataset.theme;
  $('b-theme').textContent = th === 'dark' ? '☾' : th === 'light' ? '☀' : '◐';
  $('b-theme').title = t(th === 'dark' ? 'themeDark' : th === 'light' ? 'themeLight' : 'themeAuto');
}
$('b-theme').onclick = () => { store('theme', THEMES[(THEMES.indexOf(store('theme') || '') + 1) % 3]); showTheme(); };

// ---- Requests ----------------------------------------------------------------------------------------------------
async function api(path, body) {
  const opt = body === undefined ? {} : { method: 'POST', body, headers: { 'Content-Type': 'text/plain' } };
  const r = await fetch(path, Object.assign({ credentials: 'same-origin', cache: 'no-store' }, opt));
  let j = {};
  try { j = await r.json(); } catch (e) { /* empty */ }
  j.status = r.status;
  return j;
}

function view(id) {
  for (const v of ['v-load', 'v-nopw', 'v-login', 'v-app']) $(v).hidden = v !== id;
  $('b-settings').hidden = $('b-logout').hidden = id !== 'v-app';
}

async function start() {
  try { state = await api('/api/state'); } catch (e) { $('v-load').querySelector('p').textContent = t('failed'); return; }
  L = T[state.lang] || T.en;
  $('name').textContent = state.name;
  document.title = state.name;
  translate();
  if (!state.pw) return view('v-nopw');
  if (!state.auth) { view('v-login'); $('l-pw').focus(); return; }
  view('v-app');
  connect();
}

$('f-login').onsubmit = async (e) => {
  e.preventDefault();
  const err = $('l-err');
  err.hidden = true;
  let j;
  try { j = await api('/api/login', $('l-pw').value); } catch (x) { j = { status: 0 }; }
  if (j.ok) { $('l-pw').value = ''; return start(); }
  err.textContent = j.err === 'wrong' && !j.lock ? t('wrong') : j.lock ? t('locked', { s: j.lock }) : t('failed');
  err.hidden = false;
};

$('b-logout').onclick = async () => { await api('/api/logout', ''); if (ws) ws.close(); location.reload(); };

// ---- WebSocket ---------------------------------------------------------------------------------------------------
function send(m) { if (ws && ws.readyState === 1) ws.send(m); }

function connect() {
  ws = new WebSocket('wss://' + location.host + '/ws');
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => { wsOpenedAt = Date.now(); retries = 0; video.reset(); send('stream on'); if (video.w) send('size ' + video.w + ' ' + video.h); };
  ws.onmessage = (e) => { if (typeof e.data === 'string') onText(e.data); else video.chunk(new Uint8Array(e.data)); };
  ws.onclose = async () => {
    ws = null;
    const lasted = Date.now() - wsOpenedAt;
    let s = {};
    try { s = await api('/api/state'); } catch (x) { /* the board is away */ }
    if (s.status === 200 && !s.auth) return start();  // signed out (here or by another browser's sign-in)
    if (lasted > 10000 && retries++ < 5) return setTimeout(connect, 1500);
    overlay(t(lasted < 10000 && s.status === 200 ? 'replaced' : 'disconnected'), t('reconnect'), () => { retries = 0; connect(); });
  };
}
setInterval(() => send('ping'), 3000);  // keeps the connection the server's most recently used one

function onText(s) {
  let m;
  try { m = JSON.parse(s); } catch (e) { return; }
  if (m.t === 'st') { st = m; video.sync(m.now); showStatus(); }
  else if (m.t === 'err') flash(m.msg === 'control' ? t('errControl') : m.msg === 'stopped' ? t('errStopped') : m.msg === 'busy' ? t('errBusy') : m.msg);
}

let flashTimer;
function flash(msg) {
  const c = $('ctl-text');
  c.textContent = msg;
  c.classList.add('err');
  clearTimeout(flashTimer);
  flashTimer = setTimeout(() => { c.classList.remove('err'); showStatus(); }, 2500);
}

function pill(el, cls, text) { el.hidden = false; el.className = 'pill ' + cls; el.textContent = text; }

function showStatus() {
  const ap = st.ap || 'idle';
  pill($('p-ap'), ap === 'streaming' ? 'ok' : ap === 'connected' ? 'warn' : '', 'AirPlay: ' + t(ap));
  pill($('p-ctl'), st.stop ? 'bad' : st.ctl ? 'ok' : 'warn', t(st.stop ? 'stopped' : st.ctl ? 'control' : 'viewOnly'));
  if (!flashTimer || !$('ctl-text').classList.contains('err'))
    $('ctl-text').textContent = st.stop ? t('ctlStop') : st.ctl ? t('ctlYou') : st.cli ? t('ctlCli') : st.watch ? t('ctlView') : t('ctlWait');
  $('b-take').hidden = !!st.ctl;
  $('b-release').hidden = !st.ctl;
  $('cv').classList.toggle('view', !st.ctl);
  $('s-ap').textContent = t(ap);
  $('s-rate').textContent = st.fps ? st.fps.toFixed(1) + ' fps, ' + st.kbps + ' kbit/s' : '–';
  $('s-usb').textContent = t(st.usb === 'ok' ? 'usbOk' : st.usb === 'asleep' ? 'usbAsleep' : 'usbOff');
  video.showState();
}

$('b-take').onclick = () => send('take');
$('b-release').onclick = () => send('release');

// ---- Video -------------------------------------------------------------------------------------------------------
// Chunks as the board sends them to the CLI (src/video/gop.h): [seq u16][flags u8], the first of an access unit
// with its board time (u32 ms) before the bytes. A CONFIG unit (SPS/PPS) starts a stream: the GOP from its IDR
// follows, so a page that joins late starts at once.
const VF_START = 1, VF_END = 2, VF_IDR = 4, VF_CONFIG = 8;
const video = {
  w: 0, h: 0, parts: [], flags: 0, ts: 0, cfg: null, dec: null, needKey: true, lastTs: 0, offset: null,
  decoded: false, barsAt: 0, bars: '', ctx: null,
  reset() { this.parts = []; this.needKey = true; this.cfg = null; },
  sync(boardNow) { this.offset = boardNow - performance.now(); },
  chunk(b) {
    if (b.length < 3) return;
    const flags = b[2];
    let off = 3;
    if (flags & VF_START) {
      this.parts = [];
      this.flags = flags;
      this.ts = b[3] | b[4] << 8 | b[5] << 16 | (b[6] << 24 >>> 0);
      off = 7;
    } else if (!this.parts.length) return;  // the middle of a unit we did not see start
    this.parts.push(b.subarray(off));
    if (flags & VF_END) {
      const n = this.parts.reduce((a, p) => a + p.length, 0), au = new Uint8Array(n);
      let o = 0;
      for (const p of this.parts) { au.set(p, o); o += p.length; }
      this.parts = [];
      this.unit(au, this.flags, this.ts);
    }
  },
  unit(au, flags, ts) {
    if (flags & VF_CONFIG) { this.cfg = au; this.needKey = true; this.configure(au); return; }
    if (!this.dec || this.dec.state !== 'configured') return;
    const key = !!(flags & VF_IDR);
    if (this.needKey && !key) return;  // a P-frame needs the frames before it
    let data = au;
    if (key && this.cfg) { data = new Uint8Array(this.cfg.length + au.length); data.set(this.cfg); data.set(au, this.cfg.length); }
    try {
      this.dec.decode(new EncodedVideoChunk({ type: key ? 'key' : 'delta', timestamp: ts * 1000, data }));
      this.needKey = false;
      this.lastTs = ts;
    } catch (e) { this.needKey = true; }
  },
  configure(cfg) {
    if (typeof VideoDecoder === 'undefined') { overlay(t('noCodec')); return; }
    let i = 0;  // the SPS after its start code: profile, constraints, level make the codec string
    while (i + 4 < cfg.length && !(cfg[i] === 0 && cfg[i + 1] === 0 && cfg[i + 2] === 1 && (cfg[i + 3] & 31) === 7)) i++;
    const hex = (x) => x.toString(16).padStart(2, '0');
    const codec = i + 6 < cfg.length ? 'avc1.' + hex(cfg[i + 4]) + hex(cfg[i + 5]) + hex(cfg[i + 6]) : 'avc1.42e01f';
    if (this.dec && this.dec.state !== 'closed') try { this.dec.close(); } catch (e) { /* closed */ }
    this.dec = new VideoDecoder({ output: (f) => this.draw(f), error: () => { this.needKey = true; } });
    try { this.dec.configure({ codec, optimizeForLatency: true }); } catch (e) { overlay(t('noCodec')); }
  },
  draw(f) {
    const cv = $('cv');
    if (cv.width !== f.displayWidth || cv.height !== f.displayHeight || !this.w) {
      cv.width = this.w = f.displayWidth;
      cv.height = this.h = f.displayHeight;
      this.ctx = null;
      send('size ' + this.w + ' ' + this.h);
      this.barsAt = 0;
    }
    this.ctx = this.ctx || cv.getContext('2d', { alpha: false });
    this.ctx.drawImage(f, 0, 0);
    f.close();
    this.decoded = true;
    this.shown = Number(f.timestamp) / 1000;
    const now = performance.now();
    if (now - this.barsAt > 2000) { this.barsAt = now; this.measureBars(); }
    this.showState();
  },
  // Black rows / columns at the edges (letterboxing): the board decides whether they are bars (web_input.h).
  measureBars() {
    const c = this.ctx, w = this.w, h = this.h, dark = (d, i) => d[i] + d[i + 1] + d[i + 2] < 48;
    const run = (d, step) => { let n = 0; for (let i = 0; i < d.length && dark(d, i); i += step) n++; return n; };
    let l = w, r = w, tp = h, b = h;
    for (const f of [0.3, 0.5, 0.7]) {
      const row = c.getImageData(0, Math.floor(h * f), w, 1).data, col = c.getImageData(Math.floor(w * f), 0, 1, h).data;
      l = Math.min(l, run(row, 4));
      r = Math.min(r, run(reverseRgba(row), 4));
      tp = Math.min(tp, run(col, 4));
      b = Math.min(b, run(reverseRgba(col), 4));
    }
    const s = [l, tp, r, b].map((x) => (x >= w || x >= h ? 0 : x)).join(' ');
    if (s !== this.bars) { this.bars = s; send('bars ' + s); }
  },
  showState() {
    const name = state.name || 'Tentacle', age = $('age');
    let msg = '';
    if (st.ap !== 'streaming') msg = t(st.ap === 'connected' ? 'apConnected' : 'apIdle', { name });
    else if (st.vw === 2) msg = t('frozen');
    else if (!this.decoded || st.vw === 1) msg = st.vwait > 10000 ? t('keyframeLong', { s: Math.round(st.vwait / 1000) }) : t('keyframe');
    if (!msg || !$('overlay').querySelector('button')) overlay(msg);
    if (this.decoded && this.offset !== null && st.ap === 'streaming') {
      const boardNow = performance.now() + this.offset, a = Math.max(0, (boardNow - this.lastTs) / 1000);
      age.hidden = false;
      const stale = st.vage > 3000;
      age.classList.toggle('stale', stale);
      age.textContent = stale ? t('still', { s: Math.round(st.vage / 1000) }) : a.toFixed(1) + ' s';
      $('s-age').textContent = age.textContent;
    } else { age.hidden = true; $('s-age').textContent = '–'; }
  },
};

function reverseRgba(d) {
  const o = new Uint8ClampedArray(d.length);
  for (let i = 0, j = d.length - 4; j >= 0; i += 4, j -= 4) { o[i] = d[j]; o[i + 1] = d[j + 1]; o[i + 2] = d[j + 2]; o[i + 3] = d[j + 3]; }
  return o;
}

function overlay(msg, label, fn) {
  const o = $('overlay');
  o.hidden = !msg;
  o.textContent = msg || '';
  if (label) { const b = document.createElement('button'); b.textContent = label; b.onclick = fn; o.append(document.createElement('br'), b); }
}

// ---- Mouse -------------------------------------------------------------------------------------------------------
const cv = $('cv');
function at(e) {
  const r = cv.getBoundingClientRect();
  return Math.round((e.clientX - r.left) * cv.width / r.width) + ' ' + Math.round((e.clientY - r.top) * cv.height / r.height);
}
let down = false, pendingMove = null, moveTimer = null;
cv.addEventListener('pointerdown', (e) => {
  if (e.button !== 0 || !st.ctl) return;
  e.preventDefault();
  cv.setPointerCapture(e.pointerId);
  cv.focus();
  down = true;
  send('down ' + at(e));
});
cv.addEventListener('pointermove', (e) => {
  if (!down) return;
  pendingMove = at(e);
  if (!moveTimer) moveTimer = setTimeout(() => { moveTimer = null; if (pendingMove && down) send('move ' + pendingMove); pendingMove = null; }, 40);
});
const up = (e) => { if (!down) return; down = false; pendingMove = null; send('up ' + at(e)); };
cv.addEventListener('pointerup', up);
cv.addEventListener('pointercancel', up);
cv.addEventListener('contextmenu', (e) => { e.preventDefault(); if (st.ctl) send('long ' + at(e)); });
let wheelSum = 0, wheelAt = '', wheelTimer = null;
cv.addEventListener('wheel', (e) => {
  e.preventDefault();
  if (!st.ctl) return;
  const r = cv.getBoundingClientRect();
  wheelSum += (e.deltaMode === 1 ? e.deltaY * 16 : e.deltaY) * cv.height / r.height;
  wheelAt = at(e);
  clearTimeout(wheelTimer);
  wheelTimer = setTimeout(() => {
    const d = Math.max(-0.6 * cv.height, Math.min(0.6 * cv.height, wheelSum));
    wheelSum = 0;
    if (Math.abs(d) >= 4) send('wheel ' + wheelAt + ' ' + Math.round(d));
  }, 120);
}, { passive: false });

// ---- Keyboard ----------------------------------------------------------------------------------------------------
document.querySelectorAll('[data-btn]').forEach((b) => { b.onclick = () => send('btn ' + b.dataset.btn); });
document.querySelectorAll('[data-key]').forEach((b) => { b.onclick = () => send('key ' + b.dataset.key); });
$('f-text').onsubmit = (e) => {
  e.preventDefault();
  const v = $('t-text').value;
  if (v) send('text ' + v);
  $('t-text').value = '';
};

const NAMES = { Enter: 'enter', Escape: 'esc', Tab: 'tab', Backspace: 'backspace', ' ': 'space', ArrowUp: 'up',
  ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right' };
let capturing = false;
function setCapture(on) {
  capturing = on;
  $('capture').classList.toggle('on', on);
  $('capture').textContent = t(on ? 'captureOff' : 'capture');
  $('capture-hint').hidden = !on;
}
$('capture').onclick = () => setCapture(!capturing);
document.addEventListener('keydown', (e) => {
  if (!capturing || ['Shift', 'Control', 'Alt', 'Meta', 'CapsLock'].includes(e.key)) return;
  const mods = (e.metaKey ? 'cmd+' : '') + (e.ctrlKey ? 'ctrl+' : '') + (e.altKey ? 'alt+' : '');
  let k = NAMES[e.key];
  if (k) {
    if (e.shiftKey) k = 'shift+' + k;
  } else if (e.key.length === 1 && e.key > ' ' && e.key <= '~') {
    if (!mods) { e.preventDefault(); send('text ' + e.key); return; }  // the board types it in the phone's layout
    if (e.key === '+') return;  // can not be part of a combo
    k = (e.shiftKey ? 'shift+' : '') + e.key.toLowerCase();
  } else return;
  e.preventDefault();
  send('key ' + mods + k);
}, true);

// ---- Settings ----------------------------------------------------------------------------------------------------
const dlg = $('d-settings');
$('b-settings').onclick = async () => { await loadSettings(); dlg.showModal(); };
$('b-close').onclick = () => dlg.close();

function fmtUp(s) { const h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60; return h + ' h ' + m + ' min'; }

async function loadSettings() {
  const s = await api('/api/settings');
  if (s.status === 401) return location.reload();
  $('s-name').value = s.name;
  $('s-suffix').textContent = s.suffix;
  $('s-kbd').value = s.kbd;
  $('s-lang').value = s.lang;
  const rows = [['ssid', s.ssid], ['ip', s.ip], ['host', s.host], ['rssi', s.rssi + ' dBm'],
    ['memory', Math.round(s.heap / 1024) + ' KB + ' + Math.round(s.psram / 1024) + ' KB PSRAM'], ['uptime', fmtUp(s.up)]];
  const net = $('s-net');
  net.textContent = '';
  for (const [k, v] of rows) { const dt = document.createElement('dt'), dd = document.createElement('dd'); dt.textContent = t(k); dd.textContent = v; net.append(dt, dd); }
  const dt = document.createElement('dt'), dd = document.createElement('dd'), code = document.createElement('code');
  dt.textContent = t('fp');
  code.textContent = s.fp;
  dd.append(code);
  net.append(dt, dd);
  $('fw-now').textContent = t('fwNow', { fw: s.fw });
}

$('b-save').onclick = async () => {
  const name = $('s-name').value.trim(), msg = $('save-msg');
  if (!/^[A-Za-z0-9._-]([A-Za-z0-9 ._-]{0,18}[A-Za-z0-9._-])?$/.test(name)) { msg.textContent = t('badName'); return; }
  const j = await api('/api/settings', 'name=' + name + '\nkbd=' + $('s-kbd').value + '\nlang=' + $('s-lang').value);
  msg.textContent = j.ok ? t(j.restart ? 'savedRestart' : 'saved') : t('failed');
  if (j.ok) { state = await api('/api/state'); L = T[state.lang] || T.en; translate(); setCapture(capturing); }
};

$('b-pw').onclick = async () => {
  const msg = $('pw-msg'), a = $('pw-new').value;
  if (a !== $('pw-new2').value) { msg.textContent = t('pwMismatch'); return; }
  if (a.length < 8 || a.length > 64) { msg.textContent = t('pwWeak'); return; }
  const j = await api('/api/password', $('pw-old').value + '\n' + a);
  if (j.state !== 'wait') { msg.textContent = t(j.err === 'wrong' ? 'wrong' : j.err === 'weak' ? 'pwWeak' : j.err === 'busy' ? 'pwBusy' : 'failed'); return; }
  $('pw-old').value = $('pw-new').value = $('pw-new2').value = '';
  msg.textContent = t('pwPress');
  for (let i = 0; i < 40; i++) {
    await new Promise((r) => setTimeout(r, 1000));
    const s = await api('/api/password');
    if (s.state === 'saved' || s.state === 'expired') { msg.textContent = t(s.state === 'saved' ? 'pwSaved' : 'pwExpired'); return; }
  }
};

function hex(buf) { return Array.from(new Uint8Array(buf), (b) => b.toString(16).padStart(2, '0')).join(''); }

// The version as `tentacle update` finds it: "tentacle-fw:X;" somewhere in the image (version.h, FW_MARKER).
function fwVersion(bytes) {
  const m = [...'tentacle-fw:'].map((c) => c.charCodeAt(0));
  outer: for (let i = 0; i + m.length < bytes.length; i++) {
    for (let k = 0; k < m.length; k++) if (bytes[i + k] !== m[k]) continue outer;
    let v = '';
    for (let j = i + m.length; j < bytes.length && j < i + m.length + 32 && bytes[j] !== 59; j++) v += String.fromCharCode(bytes[j]);
    return /^[0-9A-Za-z.+_-]+$/.test(v) ? v : null;
  }
  return null;
}

$('b-ota').onclick = async () => {
  const msg = $('ota-msg'), bin = $('ota-bin').files[0], sigFile = $('ota-sig').files[0], prog = $('ota-prog');
  if (!bin) return;
  msg.className = 'small';
  const bytes = new Uint8Array(await bin.arrayBuffer()), v = fwVersion(bytes);
  if (!v) { msg.textContent = t('otaNoVersion'); return; }
  const sha = hex(await crypto.subtle.digest('SHA-256', bytes));
  const sig = sigFile ? hex(await sigFile.arrayBuffer()) : '-';
  const b = await api('/api/ota/begin', bytes.length + ' ' + sha + ' ' + sig + ' ' + v);
  if (!b.reply || !b.reply.startsWith('ok')) { msg.textContent = (b.reply || t('failed')) + (sig === '-' ? ' ' + t('otaNeedSig') : ''); return; }
  prog.hidden = false;
  $('b-ota').disabled = true;
  const ok = await new Promise((done) => {
    const x = new XMLHttpRequest();
    x.open('POST', '/api/ota/data');
    x.upload.onprogress = (e) => { prog.value = e.loaded / e.total; msg.textContent = t('otaSending', { p: Math.round(100 * e.loaded / e.total) }); };
    x.onload = () => done(x.status === 200);
    x.onerror = () => done(false);
    x.send(bytes);
  });
  msg.textContent = t('otaChecking');
  const e = ok ? await api('/api/ota/end', '') : await api('/api/ota/abort', '');
  $('b-ota').disabled = false;
  if (e.reply && e.reply.startsWith('ok restarting')) { msg.textContent = t('otaDone', { v }); setTimeout(() => location.reload(), 20000); }
  else { msg.textContent = e.reply || t('failed'); msg.className = 'small err'; prog.hidden = true; }
};

$('b-check').onclick = async () => { await api('/api/update/check'); $('ota-msg').textContent = t('checkStub'); };
$('b-restart').onclick = async () => {
  if (!confirm(t('restartAsk'))) return;
  await api('/api/restart', '');
  $('ota-msg').textContent = t('restarting');
  setTimeout(() => location.reload(), 15000);
};

start();
