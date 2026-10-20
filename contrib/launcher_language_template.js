// Makes Launcher/languages/en-template.json, the launcher's English to translate: every tr("..."),
// trf("...") and translatable("...") in Launcher/, and the labels of game_settings.cpp's tables.
// node contrib/launcher_language_template.js (contrib/launcher-language.md)
const fs = require('fs');
const path = require('path');
const root = path.join(__dirname, '..', 'Launcher');
const lit = String.raw`"((?:[^"\\\r\n]|\\.)*)"`;
const run = new RegExp(String.raw`(?:${lit}\s*)+`, 'y');
const one = new RegExp(lit, 'g');

function decode(body) {
  const bytes = [];
  for (let i = 0; i < body.length; i++) {
    const c = body[i];
    if (c !== '\\') { for (const b of Buffer.from(c, 'utf8')) bytes.push(b); continue; }
    const n = body[++i];
    if (n === 'x') { let h = ''; while (/[0-9a-fA-F]/.test(body[i + 1] || '')) h += body[++i]; bytes.push(parseInt(h, 16)); }
    else if (n === 'n') bytes.push(10);
    else if (n === 't') bytes.push(9);
    else bytes.push(n.charCodeAt(0));   // \" \\ \'
  }
  return Buffer.from(bytes).toString('utf8');
}
function joined(text) {
  let out = '';
  for (const m of text.matchAll(one)) out += decode(m[1]);
  return out;
}

const keys = [];
const seen = new Set();
const add = (k) => { if (k && /[A-Za-z]/.test(k) && !seen.has(k)) { seen.add(k); keys.push(k); } };

const order = ['gui_home.cpp', 'gui_launcher.cpp', 'problem.h', 'gui_settings.cpp', 'game_settings.cpp', 'gui_sign_in.cpp',
  'gui_mods.cpp', 'gui_mods_browse.cpp', 'gui_mods_profiles.cpp', 'gui_changelog.cpp', 'gui_legal.cpp', 'gui.cpp', 'main.cpp'];
const files = fs.readdirSync(root).filter(f => /\.(cpp|h)$/.test(f) && f !== 'language.h' && f !== 'language.cpp');
files.sort((a, b) => (order.indexOf(a) + 1 || 99) - (order.indexOf(b) + 1 || 99));
for (const file of files) {
  const s = fs.readFileSync(path.join(root, file), 'utf8');
  if (file === 'game_settings.cpp') {
    for (const m of s.matchAll(/\{"[^"]*",\s*"([^"]*)"\}/g)) add(m[1]);                       // Choice
    for (const m of s.matchAll(/\{\d+,\s*"([^"]*)"\}/g)) add(m[1]);                             // Named
    for (const m of s.matchAll(/\{"([A-Z][A-Z ]+)",\s*"[^"]*",\s*"([^"]*)",\s*[a-z_]+,/g)) { add(m[1]); add(m[2]); }  // Setting
    for (const m of s.matchAll(/(?:choice|toggle|whole|real)\("[A-Z]+",\s*"([^"]*)",\s*"[^"]*",\s*"([^"]*)"/g)) { add(m[1]); add(m[2]); }
    for (const m of s.matchAll(/\{"GRAPHICS",\s*"([^"]*)",\s*"Resolution",\s*"([^"]*)"/g)) { add(m[1]); add(m[2]); }
    continue;
  }
  for (const m of s.matchAll(/\b(?:tr|trf|translatable)\(\s*/g)) {
    run.lastIndex = m.index + m[0].length;
    const r = run.exec(s);
    if (r) add(joined(r[0]));
  }
}
const template = {'@language': ''};
for (const k of keys) template[k] = '';
const out = process.argv[2] || path.join(root, 'languages', 'en-template.json');
fs.mkdirSync(path.dirname(out), {recursive: true});
fs.writeFileSync(out, JSON.stringify(template, null, 2) + '\n');
console.log(keys.length, 'strings ->', out);
