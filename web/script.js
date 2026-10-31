const $ = (s) => document.querySelector(s);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const fmt = (n) => n < 1024 ? n + ' B' : n < 1048576 ? (n / 1024).toFixed(1) + ' KB' : (n / 1048576).toFixed(2) + ' MB';
const esc = (t) => String(t).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const base = (p) => p.split('/').pop();
const MAX_BYTES = 190 * 1024 * 1024;   // the local server accepts about 200 MB per upload
const MAX_FILES = 1000;

let picked = [];   // Home: [{ file, path }]
let files = [];    // Recovery: file list reported by the C++ server
let cur = 0;       // selected file in Recovery
let selBlock = null;

// ---------- helpers ----------
async function api(path, opts) {
  const res = await fetch(path, opts);
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(data.error || 'Request failed');
  return data;
}

async function download(path, name) {
  const res = await fetch(path);
  if (!res.ok) throw new Error('Download failed');
  const url = URL.createObjectURL(await res.blob());
  const a = document.createElement('a');
  a.href = url; a.download = name; a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

// ---------- navigation ----------
function show(screen) {
  document.querySelectorAll('.screen').forEach((el) => el.hidden = el.id !== screen);
  document.querySelectorAll('nav button').forEach((b) => b.classList.toggle('active', b.dataset.go === screen));
  if (screen === 'recovery') loadRecovery();
}
document.querySelectorAll('[data-go]').forEach((b) => b.onclick = () => show(b.dataset.go));

// ---------- home: choose files ----------
function resetResult() { $('#result').hidden = true; $('#homeError').hidden = true; }

function addFiles(items) {
  for (const it of items) {
    const key = (p) => p.path + '|' + p.file.size + '|' + p.file.lastModified;
    if (!picked.some((p) => key(p) === key(it))) picked.push(it);   // ignore the exact same file twice
  }
  resetResult();
  renderPicked();
}

function renderPicked() {
  const total = picked.reduce((n, p) => n + p.file.size, 0);
  $('#selection').hidden = picked.length === 0;
  $('#selCount').textContent = picked.length + (picked.length === 1 ? ' file' : ' files');
  $('#selTotal').textContent = 'Total ' + fmt(total);
  $('#fileList').innerHTML = picked.map((p, i) =>
    `<li><span class="fname" title="${esc(p.path)}">${esc(p.path)}</span><span class="fsize">${fmt(p.file.size)}</span>` +
    `<button class="x" data-i="${i}" aria-label="Remove ${esc(p.path)}">&times;</button></li>`).join('');
  $('#compress').disabled = picked.length === 0;
}

$('#fileList').onclick = (e) => {
  const b = e.target.closest('.x');
  if (!b) return;
  picked.splice(Number(b.dataset.i), 1);
  resetResult();
  renderPicked();
};

// Folder drops: walk the directory tree so relative paths (folder structure) are kept.
const readAll = (reader) => new Promise((res, rej) => {
  const out = [];
  const next = () => reader.readEntries((batch) => batch.length ? (out.push(...batch), next()) : res(out), rej);
  next();
});
async function walk(entry) {
  if (entry.isFile) {
    const file = await new Promise((res, rej) => entry.file(res, rej));
    return [{ file, path: entry.fullPath.replace(/^\//, '') }];
  }
  const kids = await readAll(entry.createReader());
  return (await Promise.all(kids.map(walk))).flat();
}

const drop = $('#drop');
$('#browse').onclick = (e) => { e.stopPropagation(); $('#picker').click(); };
drop.onclick = () => $('#picker').click();
drop.onkeydown = (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); $('#picker').click(); } };
$('#picker').onchange = (e) => {
  addFiles([...e.target.files].map((f) => ({ file: f, path: f.name })));
  e.target.value = '';
};
['dragenter', 'dragover'].forEach((t) => drop.addEventListener(t, (e) => { e.preventDefault(); drop.classList.add('over'); }));
['dragleave', 'drop'].forEach((t) => drop.addEventListener(t, (e) => { e.preventDefault(); drop.classList.remove('over'); }));
drop.addEventListener('drop', async (e) => {
  // dataTransfer is only readable before the first await
  const entries = [...(e.dataTransfer.items || [])].map((i) => i.webkitGetAsEntry ? i.webkitGetAsEntry() : null);
  const plain = [...e.dataTransfer.files].map((f) => ({ file: f, path: f.name }));
  if (entries.length && entries.every(Boolean)) {
    try { addFiles((await Promise.all(entries.map(walk))).flat()); } catch { addFiles(plain); }
  } else addFiles(plain);
});

// ---------- home: compress ----------
// Upload format read by the C++ server: count(4) | { nameLen(4) name size(8) data } * count   (little-endian)
function buildBundle(items) {
  const enc = new TextEncoder();
  const head = new DataView(new ArrayBuffer(4));
  head.setUint32(0, items.length, true);
  const parts = [head];
  for (const { file, path } of items) {
    const name = enc.encode(path);
    const meta = new DataView(new ArrayBuffer(4 + name.length + 8));
    meta.setUint32(0, name.length, true);
    new Uint8Array(meta.buffer).set(name, 4);
    meta.setUint32(4 + name.length, file.size % 4294967296, true);
    meta.setUint32(8 + name.length, Math.floor(file.size / 4294967296), true);
    parts.push(meta, file);
  }
  return new Blob(parts);
}

$('#compress').onclick = async () => {
  const btn = $('#compress');
  const total = picked.reduce((n, p) => n + p.file.size, 0);
  $('#homeError').hidden = true;
  const problem = picked.length > MAX_FILES ? `Select at most ${MAX_FILES} files.` :
    total > MAX_BYTES ? `The selection is ${fmt(total)}. The limit is ${fmt(MAX_BYTES)} per backup.` : '';
  if (problem) { $('#homeError').textContent = problem; $('#homeError').hidden = false; return; }
  btn.disabled = true; btn.textContent = 'Compressing…';
  try {
    const r = await api('/api/compress', { method: 'POST', body: buildBundle(picked) });
    const saved = r.originalSize ? (1 - r.compressedSize / r.originalSize) * 100 : 0;
    $('#rHead').textContent = `${r.fileCount} ${r.fileCount === 1 ? 'file' : 'files'} processed and verified` +
      (r.renamed ? ` · ${r.renamed} renamed to avoid duplicate names` : '');
    $('#rOrig').textContent = fmt(r.originalSize);
    $('#rComp').textContent = fmt(r.compressedSize);
    $('#rBackup').textContent = fmt(r.archiveSize);
    $('#rSaved').textContent = saved.toFixed(1) + '%';
    const net = r.originalSize ? (r.archiveSize / r.originalSize - 1) * 100 : 0;
    $('#rHint').textContent = r.originalSize
      ? `With parity blocks and metadata included, the backup is ${Math.abs(net).toFixed(1)}% ${net >= 0 ? 'larger' : 'smaller'} than the original files.`
      : 'The backup holds only metadata and parity for these empty files.';
    $('#result').hidden = false;
    cur = 0;
  } catch (err) {
    $('#homeError').textContent = err.message;
    $('#homeError').hidden = false;
  }
  btn.textContent = 'Compress & Create Backup';
  btn.disabled = picked.length === 0;
};

$('#dlBackup').onclick = () => download('/api/archive', 'DataMend-backup.dmb').catch((e) => { $('#homeError').textContent = e.message; $('#homeError').hidden = false; });

// ---------- recovery ----------
const statusText = (f) => ({
  intact: 'Intact', lost: 'Block ' + (f.missing + 1) + ' unavailable', restored: 'Restored', failed: 'Verification failed'
}[f.status]);

function describe(f) {
  if (f.status === 'lost') return ['Block ' + (f.missing + 1) + ' is unavailable. Recover it from the parity block.', 'bad'];
  if (f.status === 'restored') return [f.missing >= 0 ? 'Block ' + (f.missing + 1) + ' was rebuilt from parity and the file verified.' : 'File restored and verified.', 'good'];
  if (f.status === 'failed') return ['Verification failed. The data stored for this file in the archive is damaged.', 'bad'];
  return ['Select a data block to simulate losing it.', ''];
}

function say(text, tone) {
  $('#note').textContent = text;
  $('#note').className = 'note ' + (tone || '');
}

async function sync() {
  const r = await api('/api/files');
  files = r.files;
  return r.ready;
}

async function loadRecovery() {
  let ready = false;
  try { ready = await sync(); } catch { files = []; }
  if (ready) pickFile(Math.min(cur, files.length - 1)); else renderRecovery();
}

function pickFile(i) {
  cur = i;
  selBlock = files[i].missing >= 0 ? files[i].missing : null;
  say(...describe(files[i]));
  renderRecovery();
}

function renderRecovery() {
  const ready = files.length > 0;
  $('#empty').hidden = ready;
  $('#recBody').hidden = !ready;
  $('#archInfo').textContent = ready
    ? `${files.length} ${files.length === 1 ? 'file' : 'files'} in archive · ${fmt(files.reduce((n, f) => n + f.originalSize, 0))} original`
    : 'No archive loaded';
  if (!ready) return;
  const f = files[cur];
  $('#rfiles').innerHTML = files.map((x, i) =>
    `<li><button class="frow${i === cur ? ' sel' : ''}" data-i="${i}"><span class="fname" title="${esc(x.name)}">${esc(x.name)}</span>` +
    `<span class="fsize">${fmt(x.originalSize)}</span><span class="chip"><i class="dot ${x.status}"></i>${statusText(x)}</span></button></li>`).join('');
  $('#detailName').textContent = f.name;
  $('#pSize').textContent = fmt(f.blockSize);
  const labels = { ok: 'Available', lost: 'Unavailable', recovered: 'Recovered' };
  $('#blocks').innerHTML = [0, 1, 2, 3].map((i) => {
    const st = i === f.missing ? (f.status === 'restored' ? 'recovered' : 'lost') : 'ok';
    return `<button class="card ${st}${i === selBlock ? ' selected' : ''}" data-i="${i}">
      <span class="row"><b>Block ${i + 1}</b><i class="dot"></i></span>
      <span class="size">${fmt(f.blockSize)}</span>
      <span class="state">${labels[st]}</span></button>`;
  }).join('');
  $('#simulate').disabled = f.status === 'lost' || selBlock === null;
  $('#recover').disabled = f.status !== 'lost';
  $('#download').disabled = f.status === 'lost';
}

$('#rfiles').onclick = (e) => {
  const row = e.target.closest('.frow');
  if (row) pickFile(Number(row.dataset.i));
};

$('#blocks').onclick = (e) => {
  const card = e.target.closest('.card');
  if (!card || files[cur].status === 'lost') return;
  selBlock = Number(card.dataset.i);
  say('Block ' + (selBlock + 1) + ' selected. Simulate losing it.');
  renderRecovery();
};

$('#simulate').onclick = async () => {
  try {
    files = (await api(`/api/simulate?file=${cur}&block=${selBlock}`, { method: 'POST' })).files;
    $('#allNote').textContent = '';
    say(...describe(files[cur]));
    renderRecovery();
  } catch (err) { say(err.message, 'bad'); }
};

$('#recover').onclick = async () => {
  const lostCard = document.querySelector(`#blocks [data-i="${files[cur].missing}"]`);
  const parityCard = $('#parity');
  [lostCard, parityCard].forEach((c) => c && c.classList.add('rebuilding'));
  $('#recover').disabled = true;
  say('Rebuilding from parity…');
  try {
    const [r] = await Promise.all([api('/api/recover?file=' + cur, { method: 'POST' }), sleep(1100)]);
    await sync();
    if (r.verified) say('Block ' + (r.block + 1) + ' recovered. Fingerprint verified (FNV-1a ' + r.fingerprint + ').', 'good');
    else say('Block rebuilt, but the fingerprint does not match the original. The archive data is damaged.', 'bad');
  } catch (err) { say(err.message, 'bad'); }
  parityCard.classList.remove('rebuilding');
  renderRecovery();
};

$('#download').onclick = async () => {
  try {
    if (files[cur].status !== 'restored') {
      const r = await api('/api/recover?file=' + cur, { method: 'POST' });
      await sync();
      say(...(r.verified ? ['File restored and verified.', 'good'] : describe(files[cur])));
      renderRecovery();
      if (!r.verified) return;
    }
    await download('/api/restored?file=' + cur, base(files[cur].name));
  } catch (err) { say(err.message, 'bad'); }
};

$('#downloadAll').onclick = async () => {
  const btn = $('#downloadAll');
  const note = $('#allNote');
  btn.disabled = true; btn.textContent = 'Restoring…';
  try {
    const r = await api('/api/restoreall', { method: 'POST' });
    await sync();
    const failed = r.failed.length ? ` Not restored: ${r.failed.join(', ')}.` : '';
    if (!r.restored) throw new Error('No file could be restored. The archive data is damaged.');
    await download('/api/zip', 'DataMend-restored.zip');
    note.textContent = `${r.restored} of ${r.total} files restored and verified.` + failed;
    note.className = 'note left ' + (r.failed.length ? 'bad' : 'good');
  } catch (err) {
    note.textContent = err.message;
    note.className = 'note left bad';
  }
  btn.textContent = 'Download All Files'; btn.disabled = false;
  pickFile(cur);
};

// ---------- recovery: open a .dmb archive ----------
async function openArchive(file) {
  $('#archError').hidden = true;
  try {
    files = (await api('/api/open', { method: 'POST', body: file })).files;
    $('#allNote').textContent = '';
    cur = 0;
    pickFile(0);
  } catch (err) {
    $('#archError').textContent = err.message;
    $('#archError').hidden = false;
  }
}
$('#openArchive').onclick = () => $('#archPicker').click();
$('#archPicker').onchange = (e) => { if (e.target.files[0]) openArchive(e.target.files[0]); e.target.value = ''; };
const bar = $('#openBar');
['dragenter', 'dragover'].forEach((t) => bar.addEventListener(t, (e) => { e.preventDefault(); bar.classList.add('over'); }));
['dragleave', 'drop'].forEach((t) => bar.addEventListener(t, (e) => { e.preventDefault(); bar.classList.remove('over'); }));
bar.addEventListener('drop', (e) => { if (e.dataTransfer.files[0]) openArchive(e.dataTransfer.files[0]); });
