// tentacle-flash's installer page (internal/flashui). Asks the local program for its state twice a second and
// shows one of three steps: plug in, install, done (with the board's setup Wi-Fi). The program does the work.
'use strict';

const T = {
  en: {
    title: 'Tentacle Installer', other: 'Deutsch',
    step1: 'Plug in', step2: 'Install', step3: 'Done',
    plugTitle: 'Plug in the board',
    plugText: 'Connect the Seeed XIAO ESP32-S3 to this computer with a USB-C data cable. A charge-only cable does not work.',
    noBoard: 'No board found yet. Waiting…', found: 'Board found on {p}', foundFw: 'Board found on {p} (Tentacle is running)',
    foundRom: 'Board found on {p} (new or in download mode)', several: 'Several boards: choose one under Advanced',
    chosen: 'Port {p} chosen (Advanced)', uart: '{p} is a USB serial adapter, not an ESP32-S3',
    fw: 'Firmware {v}', fwSigned: 'signed', fwUnsigned: 'not signed', fwBuiltin: 'built in', noImage: 'No firmware chosen',
    install: 'Install', keep: 'The stored Wi-Fi and the pairing stay on the board.',
    keepErase: 'The whole flash is erased first: the stored Wi-Fi and the pairing are gone afterwards.',
    runTitle: 'Installing…', dontUnplug: 'Do not unplug the board until this is done.',
    st_connect: 'Connecting to the board…', st_check: 'Checking the chip…', st_erase: 'Erasing the flash…',
    st_write: 'Writing the firmware… {n} %', st_verify: 'Checking what was written…', st_restart: 'Restarting the board…',
    st_setup: 'Asking the board for its setup Wi-Fi…', st_done: 'Done',
    doneTitle: 'Done!', again: 'Install again', close: 'Close',
    closed: 'You can close this tab now.',
    setupIntro: 'The board now opens its own Wi-Fi to be set up:',
    ssid: 'Wi-Fi', pass: 'Password',
    how1: 'Scan the code with the phone\'s camera, or join the Wi-Fi above by hand.',
    how2: 'The setup page opens by itself (otherwise open {u}).',
    how3: 'Choose your home Wi-Fi (2.4 GHz) and enter its password.',
    how4: 'Press the BOOT button (B) on the board within 30 seconds to confirm.',
    stored: 'The board already has a Wi-Fi stored: {s} (address {ip}). To set it up again, hold BOOT for 10 seconds.',
    noSetup: 'The board restarted. It now opens a Wi-Fi called Tentacle-XXXX; the name and password are on its OLED. Without a display: run "tentacle-flash --info" in a terminal, or see the manual, chapter 3.',
    manual: 'Next steps: the manual, chapter 3 (Wi-Fi) and 5 (pairing).',
    chipLine: '{n} {r} · {f} flash · PSRAM {ps} · MAC {m}', none: 'none',
    errTitle: 'That did not work', retry: 'Try again',
    bootHelp: 'Download mode by hand: hold the BOOT button (B), press and release RESET (R), let go of BOOT. The two tiny buttons sit next to the USB-C socket.',
    advanced: 'Advanced', port: 'Port', auto: 'Automatic', ownImage: 'Own firmware (.bin, with its .sig if there is one)',
    ownImageHint: 'Use the .merged.bin of a build. Choose the .bin and its .sig together.',
    useBuiltin: 'Use the built-in firmware again', erase: 'Erase the whole flash first',
    eraseHint: 'Removes the stored Wi-Fi, the pairing and everything else. Only needed when something is badly broken.',
    chipInfo: 'Read chip info', log: 'Log',
    c_chip: 'Chip', c_mac: 'MAC', c_flash: 'Flash', c_psram: 'PSRAM', c_id: 'Flash id', c_usb: 'USB-Serial/JTAG',
    yes: 'yes', no: 'no', reading: 'Reading…', lost: 'The installer program is not running any more. Start tentacle-flash again.',
  },
  de: {
    title: 'Tentacle-Installer', other: 'English',
    step1: 'Anstecken', step2: 'Installieren', step3: 'Fertig',
    plugTitle: 'Board anstecken',
    plugText: 'Verbinde den Seeed XIAO ESP32-S3 mit einem USB-C-Datenkabel mit diesem Computer. Ein reines Ladekabel geht nicht.',
    noBoard: 'Noch kein Board gefunden. Warte…', found: 'Board gefunden an {p}', foundFw: 'Board gefunden an {p} (Tentacle läuft)',
    foundRom: 'Board gefunden an {p} (neu oder im Download-Modus)', several: 'Mehrere Boards: unter Erweitert eins wählen',
    chosen: 'Anschluss {p} gewählt (Erweitert)', uart: '{p} ist ein USB-Seriell-Wandler, kein ESP32-S3',
    fw: 'Firmware {v}', fwSigned: 'signiert', fwUnsigned: 'nicht signiert', fwBuiltin: 'eingebaut', noImage: 'Keine Firmware gewählt',
    install: 'Installieren', keep: 'Das gespeicherte WLAN und die Kopplung bleiben auf dem Board.',
    keepErase: 'Zuerst wird der ganze Flash gelöscht: Gespeichertes WLAN und Kopplung sind danach weg.',
    runTitle: 'Installiere…', dontUnplug: 'Das Board nicht abstecken, bis das hier fertig ist.',
    st_connect: 'Verbinde mit dem Board…', st_check: 'Prüfe den Chip…', st_erase: 'Lösche den Flash…',
    st_write: 'Schreibe die Firmware… {n} %', st_verify: 'Prüfe das Geschriebene…', st_restart: 'Starte das Board neu…',
    st_setup: 'Frage das Board nach seinem Einrichtungs-WLAN…', st_done: 'Fertig',
    doneTitle: 'Fertig!', again: 'Noch einmal installieren', close: 'Schließen',
    closed: 'Du kannst diesen Tab jetzt schließen.',
    setupIntro: 'Das Board öffnet jetzt ein eigenes WLAN zum Einrichten:',
    ssid: 'WLAN', pass: 'Passwort',
    how1: 'Den Code mit der Kamera des Handys scannen oder dem WLAN oben von Hand beitreten.',
    how2: 'Die Einrichtungsseite öffnet sich von selbst (sonst {u} öffnen).',
    how3: 'Das eigene WLAN (2,4 GHz) wählen und sein Passwort eingeben.',
    how4: 'Innerhalb von 30 Sekunden die BOOT-Taste (B) am Board drücken, um zu bestätigen.',
    stored: 'Auf dem Board ist schon ein WLAN gespeichert: {s} (Adresse {ip}). Zum Neu-Einrichten BOOT 10 Sekunden halten.',
    noSetup: 'Das Board ist neu gestartet. Es öffnet jetzt ein WLAN namens Tentacle-XXXX; Name und Passwort stehen auf seinem OLED. Ohne Display: im Terminal „tentacle-flash --info“ aufrufen oder im Handbuch, Kapitel 3, nachlesen.',
    manual: 'Weiter geht es im Handbuch, Kapitel 3 (WLAN) und 5 (Koppeln).',
    chipLine: '{n} {r} · {f} Flash · PSRAM {ps} · MAC {m}', none: 'keins',
    errTitle: 'Das hat nicht geklappt', retry: 'Noch einmal',
    bootHelp: 'Download-Modus von Hand: BOOT-Taste (B) halten, RESET (R) kurz drücken, BOOT loslassen. Die beiden winzigen Tasten sitzen neben der USB-C-Buchse.',
    advanced: 'Erweitert', port: 'Anschluss', auto: 'Automatisch', ownImage: 'Eigene Firmware (.bin, mit .sig, falls vorhanden)',
    ownImageHint: 'Die .merged.bin eines Builds nehmen. .bin und .sig zusammen auswählen.',
    useBuiltin: 'Wieder die eingebaute Firmware nehmen', erase: 'Vorher den ganzen Flash löschen',
    eraseHint: 'Entfernt das gespeicherte WLAN, die Kopplung und alles andere. Nur nötig, wenn etwas gründlich kaputt ist.',
    chipInfo: 'Chip-Info lesen', log: 'Protokoll',
    c_chip: 'Chip', c_mac: 'MAC', c_flash: 'Flash', c_psram: 'PSRAM', c_id: 'Flash-ID', c_usb: 'USB-Serial/JTAG',
    yes: 'ja', no: 'nein', reading: 'Lese…', lost: 'Das Installer-Programm läuft nicht mehr. tentacle-flash neu starten.',
  },
};

const token = new URLSearchParams(location.search).get('t') || '';
const $ = (id) => document.getElementById(id);
let lang = null;
let st = null;
let hideJob = false; // the finished install was dismissed ("Install again", "Try again")
let dead = false;

function t(key, vars) {
  let s = (T[lang] || T.en)[key] || T.en[key] || key;
  for (const k in vars || {}) s = s.split('{' + k + '}').join(vars[k]);
  return s;
}
const msgText = (m) => (m ? (lang === 'de' ? m.de : m.en) : '');

function el(tag, attrs, ...kids) {
  const e = document.createElement(tag);
  for (const k in attrs || {}) {
    if (k === 'class') e.className = attrs[k]; else e.setAttribute(k, attrs[k]);
  }
  for (const k of kids) if (k != null) e.append(k);
  return e;
}

async function api(path, body, raw) {
  const r = await fetch(path, {
    method: body === undefined ? 'GET' : 'POST',
    headers: raw ? { 'X-Token': token } : { 'X-Token': token, 'Content-Type': 'application/json' },
    body: body === undefined ? undefined : raw ? body : JSON.stringify(body),
  });
  const j = await r.json().catch(() => ({}));
  if (!r.ok && j.error) throw j.error;
  if (!r.ok) throw { en: 'HTTP ' + r.status, de: 'HTTP ' + r.status };
  return j;
}

function setLang(l) {
  lang = l;
  try { localStorage.setItem('tf-lang', l); } catch (e) { /* private window */ }
  document.documentElement.lang = l;
  for (const e of document.querySelectorAll('[data-i]')) e.textContent = t(e.dataset.i);
  document.title = t('title');
  $('b-lang').textContent = t('other');
  if (st) render();
}

// qrSVG draws the firmware's QR rows (hex, first module in the top bit) dark on white with a quiet zone.
function qrSVG(rows) {
  const n = rows.length;
  if (n < 21) return null;
  const ns = 'http://www.w3.org/2000/svg';
  const svg = document.createElementNS(ns, 'svg');
  svg.setAttribute('viewBox', `0 0 ${n + 8} ${n + 8}`);
  svg.setAttribute('class', 'qr');
  svg.setAttribute('shape-rendering', 'crispEdges');
  svg.setAttribute('role', 'img');
  const bg = document.createElementNS(ns, 'rect');
  bg.setAttribute('width', n + 8); bg.setAttribute('height', n + 8); bg.setAttribute('fill', '#fff');
  svg.append(bg);
  let d = '';
  rows.forEach((r, y) => {
    for (let x = 0; x < n; x++) {
      const h = parseInt(r[x >> 2], 16);
      if ((h >> (3 - (x & 3))) & 1) d += `M${x + 4} ${y + 4}h1v1h-1z`;
    }
  });
  const p = document.createElementNS(ns, 'path');
  p.setAttribute('d', d); p.setAttribute('fill', '#000');
  svg.append(p);
  return svg;
}

function notes(list) {
  return (list || []).map((m) => el('p', { class: 'note' }, msgText(m)));
}

function boardStatus() {
  const pill = $('board');
  const ports = st.ports || [];
  const cands = ports.filter((p) => p.kind !== 'uart' && p.kind !== 'other');
  const uarts = ports.filter((p) => p.kind === 'uart');
  const chosen = $('port').value;
  let cls = 'warn', txt = t('noBoard');
  if (chosen) { cls = 'ok'; txt = t('chosen', { p: chosen }); }
  else if (cands.length === 1) {
    const p = cands[0];
    cls = 'ok';
    txt = t(p.kind === 'firmware' ? 'foundFw' : p.kind === 'bootloader' ? 'foundRom' : 'found', { p: p.name });
  } else if (cands.length > 1) txt = t('several');
  else if (uarts.length) { cls = 'bad'; txt = t('uart', { p: uarts[0].name }); }
  pill.className = 'pill ' + cls;
  pill.textContent = txt;
}

function imageStatus() {
  const pill = $('fw');
  const box = $('img-notes');
  box.replaceChildren();
  const im = st.image;
  if (!im) {
    pill.className = 'pill bad';
    pill.textContent = t('noImage');
    if (st.imageError) box.append(el('p', { class: 'note' }, msgText(st.imageError)));
    return;
  }
  const bits = [im.version ? t('fw', { v: im.version }) : im.name];
  bits.push(im.signed ? t('fwSigned') : t('fwUnsigned'));
  if (im.source === 'embedded') bits.push(t('fwBuiltin'));
  pill.className = 'pill ' + (im.signed ? 'ok' : 'warn');
  pill.textContent = bits.join(' · ');
  pill.title = im.name + ' · sha256 ' + im.sha256;
  box.append(...notes(im.notes));
}

function stageText(j) {
  if (j.stage === 'write' && j.total) return t('st_write', { n: Math.floor((j.done * 100) / j.total) });
  return t('st_' + j.stage);
}

function overall(j) {
  const span = { connect: [0, 4], check: [4, 8], erase: [8, 20], write: [20, 90], verify: [90, 95], restart: [95, 97], setup: [97, 99], done: [100, 100] }[j.stage] || [0, 0];
  const f = j.total ? j.done / j.total : 0;
  return span[0] + (span[1] - span[0]) * f;
}

function chipLine(c) {
  return t('chipLine', { n: c.name, r: c.revision || '', f: c.flash, ps: c.psram || t('none'), m: c.mac || '?' });
}

function doneView(j) {
  const box = $('done-setup');
  box.replaceChildren();
  const s = j.result && j.result.setup;
  if (j.chip) box.append(el('p', { class: 'muted small' }, chipLine(j.chip)));
  if (s && s.setup) {
    box.append(el('p', null, t('setupIntro')));
    const q = s.qr && qrSVG(s.qr);
    if (q) box.append(q);
    box.append(el('dl', { class: 'wifi' }, el('dt', null, t('ssid')), el('dd', null, s.ssid),
      el('dt', null, t('pass')), el('dd', null, s.pass || '')));
    box.append(el('ol', { class: 'how' }, el('li', null, t('how1')), el('li', null, t('how2', { u: s.url || 'http://192.168.4.1/' })),
      el('li', null, t('how3')), el('li', null, t('how4'))));
  } else if (s) {
    box.append(el('p', null, t('stored', { s: s.ssid, ip: s.ip || '?' })));
  } else {
    box.append(el('p', null, t('noSetup')));
    box.append(el('ol', { class: 'how' }, el('li', null, t('how1')), el('li', null, t('how2', { u: 'http://192.168.4.1/' })),
      el('li', null, t('how3')), el('li', null, t('how4'))));
  }
  box.append(el('p', { class: 'muted small' }, t('manual')));
  $('done-notes').replaceChildren(...notes(j.notes));
}

function show(view) {
  for (const v of ['plug', 'run', 'done', 'err']) $('v-' + v).hidden = v !== view;
  const step = { plug: 1, run: 2, done: 3, err: 2 }[view];
  for (let i = 1; i <= 3; i++) {
    $('s' + i).className = i === step ? 'on' : i < step ? 'ok' : '';
  }
}

function renderPorts() {
  const sel = $('port');
  const cur = sel.value || (sel.dataset.init ? '' : st.port || '');
  sel.dataset.init = '1';
  const opts = [el('option', { value: '' }, t('auto'))];
  const names = new Set();
  for (const p of st.ports || []) {
    names.add(p.name);
    const extra = [p.kind, p.product, p.vid && p.vid + ':' + p.pid].filter(Boolean).join(', ');
    opts.push(el('option', { value: p.name }, p.name + (extra ? ' (' + extra + ')' : '')));
  }
  if (cur && !names.has(cur)) opts.push(el('option', { value: cur }, cur));
  const sig = opts.map((o) => o.value + o.textContent).join('|');
  if (sel.dataset.sig !== sig) {
    sel.replaceChildren(...opts);
    sel.dataset.sig = sig;
  }
  sel.value = cur;
}

function render() {
  $('ver').textContent = st.version ? 'v' + st.version : '';
  renderPorts();
  boardStatus();
  imageStatus();
  $('keep').textContent = $('erase').checked ? t('keepErase') : t('keep');
  const j = st.job;
  const busy = !!(j && j.running);
  $('b-install').disabled = busy || !st.image;
  $('b-chip').disabled = busy;
  $('file').disabled = busy;
  $('b-builtin').hidden = !st.image || st.image.source !== 'file';
  $('log').textContent = j ? j.log.join('\n') : '';
  // chip info (advanced)
  const dl = $('chip');
  if (j && j.chip) {
    const c = j.chip;
    const rows = [['c_chip', c.name + ' ' + (c.revision || '')], ['c_mac', c.mac], ['c_flash', c.flash + (c.embeddedFlash ? ' (embedded ' + c.embeddedFlash + ')' : '')],
      ['c_psram', c.psram || t('none')], ['c_id', c.flashId], ['c_usb', c.usbJtag ? t('yes') : t('no')]];
    dl.replaceChildren(...rows.flatMap(([k, v]) => [el('dt', null, t(k)), el('dd', null, v || '')]));
    dl.hidden = false;
  } else if (j && j.kind === 'chip' && j.running) {
    dl.replaceChildren(el('dd', null, t('reading')));
    dl.hidden = false;
  }
  if (j && j.kind === 'chip' && !j.running && j.error) {
    dl.replaceChildren(el('dd', { class: 'err' }, msgText(j.error)));
    dl.hidden = false;
  }
  if (!j || j.kind !== 'install' || (hideJob && !j.running)) return show('plug');
  if (j.running) {
    hideJob = false;
    $('stage').textContent = stageText(j);
    $('bar').value = overall(j);
    return show('run');
  }
  if (j.error) {
    $('err-text').textContent = msgText(j.error);
    const help = $('err-help');
    help.replaceChildren();
    if (j.error.code === 'no_download') help.append(el('p', { class: 'note' }, t('bootHelp')));
    return show('err');
  }
  doneView(j);
  return show('done');
}

async function poll() {
  if (dead) return;
  try {
    st = await api('/api/state');
    if (!lang) {
      let l = null;
      try { l = localStorage.getItem('tf-lang'); } catch (e) { /* private window */ }
      setLang(l || st.lang || 'en');
    }
    if (!$('erase').dataset.init) {
      $('erase').checked = !!st.erase;
      $('erase').dataset.init = '1';
    }
    render();
  } catch (e) {
    if (st) {
      dead = true;
      document.querySelector('main').replaceChildren(el('section', { class: 'card' }, el('p', null, t('lost'))));
      return;
    }
  }
  setTimeout(poll, st && st.job && st.job.running ? 400 : 900);
}

function fail(e) {
  alert(msgText(e) || String(e));
}

$('b-install').onclick = async () => {
  hideJob = false;
  try { await api('/api/install', { port: $('port').value, erase: $('erase').checked }); } catch (e) { fail(e); }
};
$('b-chip').onclick = async () => {
  try { await api('/api/chip', { port: $('port').value }); } catch (e) { fail(e); }
};
$('b-again').onclick = () => { hideJob = true; render(); };
$('b-retry').onclick = () => { hideJob = true; render(); };
$('b-close').onclick = async () => {
  try { await api('/api/quit', {}); } catch (e) { /* gone already */ }
  dead = true;
  document.querySelector('main').replaceChildren(el('section', { class: 'card' }, el('p', null, t('closed'))));
};
$('b-lang').onclick = () => setLang(lang === 'de' ? 'en' : 'de');
$('erase').onchange = () => render();
$('port').onchange = () => render();
$('b-builtin').onclick = async () => {
  try { await api('/api/image/reset', {}); } catch (e) { fail(e); }
};
$('file').onchange = async () => {
  const files = [...$('file').files].sort((a, b) => (a.name.endsWith('.sig') ? 1 : 0) - (b.name.endsWith('.sig') ? 1 : 0));
  try {
    for (const f of files) await api('/api/image?name=' + encodeURIComponent(f.name), f, true);
  } catch (e) { fail(e); }
  $('file').value = '';
};

poll();
