const { app, BrowserWindow, ipcMain, dialog, desktopCapturer, shell, MessageChannelMain } = require('electron');
const path = require('path');
const fs = require('fs');
const crypto = require('crypto');
const { execFile, execFileSync, exec, spawn } = require('child_process');
const { io: socketIo } = require('socket.io-client');

function getYtDlpPath() {
  try {
    const pkg = require.resolve('youtube-dl-exec/package.json');
    let bin = path.join(path.dirname(pkg), 'bin', process.platform === 'win32' ? 'yt-dlp.exe' : 'yt-dlp');
    if (bin.includes('app.asar') && !bin.includes('app.asar.unpacked')) {
      bin = bin.replace('app.asar', 'app.asar.unpacked');
    }
    return bin;
  } catch { return null; }
}

function getYtDlp() {
  const bin = getYtDlpPath();
  if (!bin || !fs.existsSync(bin)) throw new Error('Link support unavailable: yt-dlp is missing.');
  return runYtDlp;
}

function flagName(name) {
  return '--' + String(name).replace(/[A-Z]/g, m => '-' + m.toLowerCase());
}

function buildYtDlpArgs(target, flags = {}) {
  const args = [target];
  for (const [key, value] of Object.entries(flags || {})) {
    if (value === false || value === undefined || value === null) continue;
    args.push(flagName(key));
    if (value !== true) args.push(String(value));
  }
  return args;
}

function runYtDlp(target, flags = {}) {
  const bin = getYtDlpPath();
  return new Promise((resolve, reject) => {
    execFile(bin, buildYtDlpArgs(target, flags), {
      windowsHide: true,
      maxBuffer: 24 * 1024 * 1024,
    }, (error, stdout, stderr) => {
      const output = String(stdout || '').trim();
      if (error) {
        error.stderr = stderr;
        error.stdout = stdout;
        reject(error);
        return;
      }
      if (flags.dumpSingleJson) {
        try { resolve(JSON.parse(output)); }
        catch (e) {
          e.stderr = stderr;
          e.stdout = stdout;
          reject(e);
        }
        return;
      }
      resolve(output);
    });
  });
}

function isHttpUrl(v) { return /^https?:\/\//i.test(String(v || '').trim()); }

function decodeHtmlText(text = '') {
  return String(text).replace(/&amp;/g, '&').replace(/&#x27;|&#39;/g, "'").replace(/&quot;/g, '"').replace(/&lt;/g, '<').replace(/&gt;/g, '>');
}

async function spotifyToSearchQuery(url) {
  try {
    const res = await fetch(url, { headers: { 'User-Agent': 'Mozilla/5.0 XurcoEQ' } });
    const html = await res.text();
    const title = html.match(/<meta property="og:title" content="([^"]+)"/i)?.[1];
    const desc = html.match(/<meta property="og:description" content="([^"]+)"/i)?.[1];
    if (!title) return null;
    const artist = desc ? decodeHtmlText(desc.split('·')[0]?.trim() || '') : '';
    const cleanTitle = decodeHtmlText(title);
    return cleanTitle + (artist && !cleanTitle.toLowerCase().includes(artist.toLowerCase()) ? ` ${artist}` : '');
  } catch { return null; }
}

const onlineAudioCache = new Map();

function normalizeYtDlpError(e) {
  return String(e?.stderr || e?.message || e || '').replace(/\s+/g, ' ').trim();
}

function uniqueCandidates(entries, fallback) {
  const seen = new Set();
  const candidates = [];
  for (const entry of entries || []) {
    if (!entry) continue;
    const url = entry.webpage_url || entry.original_url || entry.url;
    if (!url || seen.has(url)) continue;
    seen.add(url);
    candidates.push({ ...entry, webpage_url: url });
  }
  if (!candidates.length && fallback) candidates.push({ webpage_url: fallback, title: fallback });
  return candidates;
}

async function getAudioStreamForCandidate(ytdlp, candidate) {
  const target = candidate.webpage_url || candidate.original_url || candidate.url;
  if (!target) throw new Error('Missing stream URL.');
  const streamRaw = await ytdlp(target, {
    getUrl: true,
    noPlaylist: true,
    noWarnings: true,
    format: 'bestaudio[acodec!=none]/bestaudio/best',
  });
  const streamUrl = String(streamRaw || '').split(/\r?\n/).find(line => /^https?:\/\//i.test(line));
  if (!streamUrl) throw new Error('No playable audio stream found.');
  return streamUrl;
}

async function resolveOnlineAudioSource(source) {
  const original = String(source || '').trim();
  if (!isHttpUrl(original)) return null;
  const cached = onlineAudioCache.get(original);
  if (cached && Date.now() - cached.cachedAt < 8 * 60 * 1000) return cached;

  const ytdlp = getYtDlp();
  let target = original;
  let fallbackTitle = original;
  if (/open\.spotify\.com|spotify\.link/i.test(original)) {
    const query = await spotifyToSearchQuery(original);
    if (!query) throw new Error('Could not read that Spotify link.');
    fallbackTitle = query;
    target = `ytsearch8:${query} official audio`;
  }

  let raw;
  try {
    raw = await ytdlp(target, {
      dumpSingleJson: true,
      noPlaylist: true,
      noWarnings: true,
      ignoreErrors: true,
      format: 'bestaudio[acodec!=none]/bestaudio/best',
    });
  } catch (e) {
    throw new Error(normalizeYtDlpError(e) || 'Could not resolve that link.');
  }

  const entries = Array.isArray(raw?.entries) ? raw.entries : [raw];
  const candidates = uniqueCandidates(entries, /^https?:/i.test(target) ? target : null);
  let info = null;
  let streamUrl = '';
  let lastError = '';
  for (const candidate of candidates) {
    try {
      streamUrl = await getAudioStreamForCandidate(ytdlp, candidate);
      info = candidate;
      break;
    } catch (e) {
      lastError = normalizeYtDlpError(e);
    }
  }
  if (!streamUrl || !info) throw new Error(lastError || 'Could not get a playable audio stream for that link.');

  const webpageUrl = info.webpage_url || info.original_url || original;
  const resolved = {
    source: original,
    streamUrl,
    title: info.title || fallbackTitle || webpageUrl || original,
    durationMs: Number.isFinite(Number(info.duration)) ? Math.round(Number(info.duration) * 1000) : 0,
    webpageUrl,
    thumbnail: pickThumbnail(info),
    cachedAt: Date.now(),
  };
  onlineAudioCache.set(original, resolved);
  return resolved;
}

// ── Local audio cache ──
// A link is streamed at most once: the audio is downloaded into userData and
// every later play reads that file, so playback never re-runs link resolution.
const AUDIO_CACHE_DIR = path.join(app.getPath('userData'), 'audio-cache');
const MIN_VALID_AUDIO_BYTES = 16 * 1024;
const inFlightDownloads = new Map();
let ffmpegBinCache;

function audioCacheKey(url) {
  return crypto.createHash('sha1').update(String(url || '').trim()).digest('hex');
}

function audioCacheMetaPath(key) {
  return path.join(AUDIO_CACHE_DIR, `${key}.json`);
}

// Returns the cached file only when it is actually usable. A missing, empty or
// truncated download reports as a miss so the caller re-downloads it.
function readAudioCacheEntry(url) {
  const key = audioCacheKey(url);
  try {
    const meta = JSON.parse(fs.readFileSync(audioCacheMetaPath(key), 'utf8'));
    if (!meta?.file) return null;
    const abs = path.join(AUDIO_CACHE_DIR, meta.file);
    const stat = fs.statSync(abs);
    if (!stat.isFile() || stat.size < MIN_VALID_AUDIO_BYTES) return null;
    if (Number.isFinite(meta.size) && meta.size > 0 && stat.size !== meta.size) return null;
    return { source: url, path: abs, name: meta.name || '', durationMs: meta.durationMs || 0, size: stat.size, cover: existingCover(key) };
  } catch {
    return null;
  }
}

function discardAudioCacheEntry(url) {
  const key = audioCacheKey(url);
  try {
    const meta = JSON.parse(fs.readFileSync(audioCacheMetaPath(key), 'utf8'));
    if (meta?.file) fs.rmSync(path.join(AUDIO_CACHE_DIR, meta.file), { force: true });
  } catch {}
  try { fs.rmSync(audioCacheMetaPath(key), { force: true }); } catch {}
}

function findFfmpeg() {
  if (ffmpegBinCache !== undefined) return ffmpegBinCache;
  ffmpegBinCache = null;
  for (const bin of ['ffmpeg', 'C:\\ffmpeg\\bin\\ffmpeg.exe']) {
    try {
      execFileSync(bin, ['-version'], { windowsHide: true, stdio: 'ignore' });
      ffmpegBinCache = bin;
      break;
    } catch {}
  }
  return ffmpegBinCache;
}

function runYtDlpDownload(target, flags, onProgress) {
  const bin = getYtDlpPath();
  return new Promise((resolve, reject) => {
    const child = spawn(bin, buildYtDlpArgs(target, flags), { windowsHide: true });
    let stderr = '';
    child.stdout.on('data', chunk => {
      const percent = String(chunk).match(/\[download\]\s+([\d.]+)%/);
      if (percent && onProgress) onProgress(Number(percent[1]) || 0);
    });
    child.stderr.on('data', chunk => { stderr += String(chunk); });
    child.on('error', reject);
    child.on('close', code => {
      if (code === 0) return resolve();
      const clean = stderr.replace(/\s+/g, ' ').trim();
      const reason = (clean.match(/ERROR:\s*([^]*?)(?:\s+File "|$)/) || [])[1] || clean;
      const err = new Error(reason || `Download failed (yt-dlp exit ${code}).`);
      err.stderr = clean;
      reject(err);
    });
  });
}

let ytDlpUpdatedThisRun = false;

function looksLikeStaleExtractor(message) {
    return /403|forbidden|unable to (download|extract)|nsig|signature|player response|precondition check failed|sign in to confirm/i
        .test(String(message || ""));
}

/** Run yt-dlp's own updater. Resolves to true when it reports success. */
function updateYtDlp() {
    const bin = getYtDlpPath();
    if (!bin || !fs.existsSync(bin)) return Promise.resolve(false);
    return new Promise(resolve => {
        execFile(bin, ["-U"], { windowsHide: true, maxBuffer: 4 * 1024 * 1024 }, (error, stdout) => {
            const out = String(stdout || "");
            const ok = !error && /Updated yt-dlp|is up to date/i.test(out);
            console.log("[yt-dlp] update:", out.replace(/\s+/g, " ").trim().slice(0, 200));
            resolve(ok);
        });
    });
}

// Downloads into a staging folder, then moves the finished file into place so a
// crash mid-download can never leave a half-written file looking like a hit.
async function downloadAudioToCache(source, onProgress) {
  const info = await resolveOnlineAudioSource(source);
  if (!info) throw new Error('Not a valid link.');

  fs.mkdirSync(AUDIO_CACHE_DIR, { recursive: true });
  const key = audioCacheKey(source);
  const stageDir = path.join(AUDIO_CACHE_DIR, `.tmp-${key}`);
  fs.rmSync(stageDir, { recursive: true, force: true });
  fs.mkdirSync(stageDir, { recursive: true });

  const flags = {
    output: path.join(stageDir, 'audio.%(ext)s'),
    format: 'bestaudio[acodec!=none]/bestaudio/best',
    noPlaylist: true,
    noWarnings: true,
    noPart: true,
    newline: true,
  };
  // Transcoding to mp3 needs ffmpeg; without it we keep the original audio
  // container (m4a/webm/opus), which the soundpad already supports.
  const ffmpeg = findFfmpeg();
  if (ffmpeg) {
    flags.extractAudio = true;
    flags.audioFormat = 'mp3';
    flags.audioQuality = 0;
    if (ffmpeg !== 'ffmpeg') flags.ffmpegLocation = ffmpeg;
  }

  try {
    try {
      await runYtDlpDownload(info.webpageUrl || source, flags, onProgress);
    } catch (e) {
      // One retry, and only for the failure shape that an out-of-date
      // extractor produces - a genuinely private or removed video should fail
      // straight away rather than sit through an update first.
      if (ytDlpUpdatedThisRun || !looksLikeStaleExtractor(e.message)) throw e;
      ytDlpUpdatedThisRun = true;
      if (mainWindow && !mainWindow.isDestroyed()) {
        mainWindow.webContents.send('audio-download-progress', { source, percent: 0, note: 'Updating link support...' });
      }
      const updated = await updateYtDlp();
      if (!updated) throw e;
      fs.rmSync(stageDir, { recursive: true, force: true });
      fs.mkdirSync(stageDir, { recursive: true });
      await runYtDlpDownload(info.webpageUrl || source, flags, onProgress);
    }

    const produced = fs.readdirSync(stageDir).filter(f => !f.endsWith('.part'));
    if (!produced.length) throw new Error('Download produced no audio file.');
    const staged = path.join(stageDir, produced[0]);
    if (fs.statSync(staged).size < MIN_VALID_AUDIO_BYTES) throw new Error('Downloaded file was incomplete.');

    discardAudioCacheEntry(source);
    const finalName = `${key}${path.extname(produced[0]) || '.mp3'}`;
    const finalPath = path.join(AUDIO_CACHE_DIR, finalName);
    fs.renameSync(staged, finalPath);

    const size = fs.statSync(finalPath).size;
    fs.writeFileSync(audioCacheMetaPath(key), JSON.stringify({
      source,
      file: finalName,
      name: info.title,
      durationMs: info.durationMs,
      size,
      savedAt: Date.now(),
    }, null, 2));

    // Artwork is a nicety: a failed image download must never fail the song.
    const cover = await downloadCover(info.thumbnail, key).catch(() => '');
    return { source, path: finalPath, name: info.title, durationMs: info.durationMs, size, cover };
  } finally {
    fs.rmSync(stageDir, { recursive: true, force: true });
  }
}

// Cache-first: returns the local file if one is already usable, otherwise
// downloads it. Concurrent requests for the same link share one download.
async function ensureLocalAudio(source, onProgress) {
  const url = String(source || '').trim();
  if (!isHttpUrl(url)) throw new Error('Not a valid link.');

  const existing = readAudioCacheEntry(url);
  if (existing) return existing;

  if (inFlightDownloads.has(url)) return inFlightDownloads.get(url);
  const job = downloadAudioToCache(url, onProgress).finally(() => inFlightDownloads.delete(url));
  inFlightDownloads.set(url, job);
  return job;
}

// VST2 Host native addon
let vstHost = null;
try {
  let vstPath = path.join(__dirname, 'native', 'build', 'Release', 'vst2_host.node');
  if (vstPath.includes('app.asar') && !vstPath.includes('app.asar.unpacked')) {
    vstPath = vstPath.replace('app.asar', 'app.asar.unpacked');
  }
  vstHost = require(vstPath);
} catch (e) {
  console.warn('VST2 host addon not available:', e.message);
}

let mainWindow;

// ── AI Voice Changer backend ──
// Custom Mic can use a separately installed CUDA voice-conversion runtime.
// The UI/audio transport are native here; only MMVCServerSIO is started.
let aiVoiceProcess = null;
let aiVoiceSocket = null;
const AI_VOICE_DEV_FOLDER = 'C:\\Users\\xurco\\Desktop\\Voice changer';
const AI_VOICE_PROGRAMDATA_FOLDER = path.join(process.env.PROGRAMDATA || 'C:\\ProgramData', 'Custom Mic', 'ai-voice-runtime');
const AI_VOICE_LOCALDATA_FOLDER = path.join(app.getPath('userData'), 'ai-voice-runtime');
const AI_VOICE_BUNDLED_FOLDER = path.join(process.resourcesPath, 'ai-voice-runtime');
let aiVoiceFolder = resolveAiVoiceFolder();
let aiVoiceLastOutput = '';
const AI_VOICE_PORT = 18888;

function resolveAiVoiceFolder(preferred) {
  const candidates = [
    preferred,
    process.env.CUSTOM_MIC_AI_RUNTIME,
    AI_VOICE_PROGRAMDATA_FOLDER,
    AI_VOICE_LOCALDATA_FOLDER,
    AI_VOICE_BUNDLED_FOLDER,
    AI_VOICE_DEV_FOLDER,
  ].filter(Boolean);
  return candidates.find(folder => fs.existsSync(path.join(folder, 'MMVCServerSIO.exe'))) || (preferred || AI_VOICE_BUNDLED_FOLDER);
}

function aiVoiceStatus() {
  aiVoiceFolder = resolveAiVoiceFolder(aiVoiceFolder);
  return {
    running: !!aiVoiceProcess && !aiVoiceProcess.killed,
    pid: aiVoiceProcess?.pid || null,
    folder: aiVoiceFolder,
    url: `http://127.0.0.1:${AI_VOICE_PORT}/`,
    lastOutput: aiVoiceLastOutput.slice(-1200),
  };
}

function aiVoiceArgs() {
  return [
    '-p', String(AI_VOICE_PORT),
    '--https', 'false',
    '--content_vec_500', 'pretrain/checkpoint_best_legacy_500.pt',
    '--content_vec_500_onnx', 'pretrain/content_vec_500.onnx',
    '--content_vec_500_onnx_on', 'true',
    '--hubert_base', 'pretrain/hubert_base.pt',
    '--hubert_base_jp', 'pretrain/rinna_hubert_base_jp.pt',
    '--hubert_soft', 'pretrain/hubert/hubert-soft-0d54a1f4.pt',
    '--nsf_hifigan', 'pretrain/nsf_hifigan/model',
    '--crepe_onnx_full', 'pretrain/crepe_onnx_full.onnx',
    '--crepe_onnx_tiny', 'pretrain/crepe_onnx_tiny.onnx',
    '--rmvpe', 'pretrain/rmvpe.pt',
    '--model_dir', 'model_dir',
    '--samples', 'samples.json',
  ];
}

ipcMain.handle('aivoice:select-folder', async () => {
  const result = await dialog.showOpenDialog(mainWindow, {
    title: 'Select CUDA Voice Changer Folder',
    properties: ['openDirectory'],
  });
  if (result.canceled || !result.filePaths.length) return null;
  const folder = result.filePaths[0];
  if (!fs.existsSync(path.join(folder, 'MMVCServerSIO.exe'))) {
    return { error: 'That folder does not contain MMVCServerSIO.exe.' };
  }
  aiVoiceFolder = folder;
  return { folder };
});

ipcMain.handle('aivoice:status', () => aiVoiceStatus());

ipcMain.handle('aivoice:start', async (_event, folder) => {
  try {
    if (aiVoiceProcess && !aiVoiceProcess.killed) return { ok: true, ...aiVoiceStatus() };
    aiVoiceFolder = resolveAiVoiceFolder(folder ? String(folder) : aiVoiceFolder);
    const exePath = path.join(aiVoiceFolder, 'MMVCServerSIO.exe');
    if (!fs.existsSync(exePath)) {
      return { error: `MMVCServerSIO.exe was not found in ${aiVoiceFolder}` };
    }

    aiVoiceLastOutput = '';
    aiVoiceProcess = spawn(exePath, aiVoiceArgs(), {
      cwd: aiVoiceFolder,
      windowsHide: true,
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    const capture = chunk => {
      aiVoiceLastOutput = `${aiVoiceLastOutput}${String(chunk || '')}`.slice(-4000);
    };
    aiVoiceProcess.stdout?.on('data', capture);
    aiVoiceProcess.stderr?.on('data', capture);
    aiVoiceProcess.once('error', error => {
      capture(error.message);
      aiVoiceProcess = null;
    });
    aiVoiceProcess.once('exit', () => { aiVoiceProcess = null; });
    return { ok: true, ...aiVoiceStatus() };
  } catch (e) {
    aiVoiceProcess = null;
    return { error: e.message || 'Could not start the AI voice backend.' };
  }
});

ipcMain.handle('aivoice:stop', async () => {
  if (!aiVoiceProcess) return { ok: true };
  try { aiVoiceProcess.kill(); } catch {}
  aiVoiceProcess = null;
  return { ok: true };
});

ipcMain.handle('aivoice:open-folder', async (_event, folder) => {
  const target = String(folder || aiVoiceFolder);
  if (!fs.existsSync(target)) return { error: 'AI voice folder was not found.' };
  await shell.openPath(target);
  return { ok: true };
});

function aiVoiceUrl(endpoint = '') {
  return `http://127.0.0.1:${AI_VOICE_PORT}${endpoint}`;
}

async function aiVoiceFetchJson(endpoint, options = {}) {
  const response = await fetch(aiVoiceUrl(endpoint), options);
  const body = await response.text();
  let data;
  try { data = body ? JSON.parse(body) : {}; } catch { data = { raw: body }; }
  if (!response.ok) throw new Error(data?.message || data?.error || `AI voice server returned ${response.status}`);
  return data;
}

ipcMain.handle('aivoice:info', async () => {
  try { return { ok: true, data: await aiVoiceFetchJson('/info') }; }
  catch (e) { return { ok: false, error: e.message || String(e) }; }
});

ipcMain.handle('aivoice:performance', async () => {
  try { return { ok: true, data: await aiVoiceFetchJson('/performance') }; }
  catch (e) { return { ok: false, error: e.message || String(e) }; }
});

ipcMain.handle('aivoice:update-setting', async (_event, key, value) => {
  try {
    const form = new FormData();
    form.append('key', String(key));
    form.append('val', typeof value === 'string' ? value : JSON.stringify(value));
    return { ok: true, data: await aiVoiceFetchJson('/update_settings', { method: 'POST', body: form }) };
  } catch (e) { return { ok: false, error: e.message || String(e) }; }
});

ipcMain.handle('aivoice:connect', async () => {
  try {
    if (aiVoiceSocket?.connected) return { ok: true };
    if (aiVoiceSocket) { try { aiVoiceSocket.disconnect(); } catch {} }
    aiVoiceSocket = socketIo(`${aiVoiceUrl('')}/test`, {
      transports: ['websocket'],
      reconnection: false,
      timeout: 5000,
    });
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('Timed out connecting to the AI voice engine.')), 6000);
      aiVoiceSocket.once('connect', () => { clearTimeout(timer); resolve(); });
      aiVoiceSocket.once('connect_error', error => { clearTimeout(timer); reject(error); });
    });
    aiVoiceSocket.on('response', packet => {
      if (!Array.isArray(packet)) return;
      const [timestamp, audio, metrics] = packet;
      const bytes = audio instanceof ArrayBuffer
        ? new Uint8Array(audio)
        : (ArrayBuffer.isView(audio) ? new Uint8Array(audio.buffer, audio.byteOffset, audio.byteLength) : Uint8Array.from(audio || []));
      mainWindow?.webContents.send('aivoice:audio-response', {
        timestamp,
        audio: bytes.buffer,
        metrics: metrics || {},
      });
    });
    return { ok: true };
  } catch (e) {
    try { aiVoiceSocket?.disconnect(); } catch {}
    aiVoiceSocket = null;
    return { ok: false, error: e.message || String(e) };
  }
});

ipcMain.handle('aivoice:send-audio', async (_event, timestamp, audio) => {
  if (!aiVoiceSocket?.connected) return { ok: false, error: 'AI voice stream is not connected.' };
  try {
    const bytes = audio instanceof ArrayBuffer
      ? new Uint8Array(audio)
      : (ArrayBuffer.isView(audio) ? new Uint8Array(audio.buffer, audio.byteOffset, audio.byteLength) : new Uint8Array(audio));
    aiVoiceSocket.emit('request_message', [timestamp, bytes.buffer]);
    return { ok: true };
  } catch (e) { return { ok: false, error: e.message || String(e) }; }
});

ipcMain.handle('aivoice:disconnect', async () => {
  try { aiVoiceSocket?.disconnect(); } catch {}
  aiVoiceSocket = null;
  return { ok: true };
});

function startedAtLogin() {
  return process.argv.includes("--startup");
}

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1200,
    height: 850,
    minWidth: 900,
    minHeight: 600,
    frame: false,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
    },
    backgroundColor: '#1a1b1e',
    // Launched by Windows at sign-in, the window starts minimised: opening on
    // login should not take over the screen. The tray/taskbar entry is enough.
    show: !startedAtLogin(),
  });
  if (startedAtLogin()) {
    // show:false alone would leave no taskbar entry at all, so it is shown
    // without focus and then minimised - present, but out of the way.
    mainWindow.once('ready-to-show', () => {
      mainWindow.showInactive();
      mainWindow.minimize();
    });
  }
  mainWindow.loadFile('index.html');
  mainWindow.on('closed', () => { mainWindow = null; });
}

app.whenReady().then(createWindow);
app.on('window-all-closed', () => app.quit());
app.on('before-quit', () => {
  stopSpotifyBridge();
  stopSpotifyAudio();
  try { aiVoiceSocket?.disconnect(); } catch {}
  aiVoiceSocket = null;
  try { aiVoiceProcess?.kill(); } catch {}
  aiVoiceProcess = null;
});

ipcMain.on('window-minimize', () => mainWindow?.minimize());
ipcMain.on('window-maximize', () => {
  if (mainWindow?.isMaximized()) mainWindow.unmaximize();
  else mainWindow?.maximize();
});
ipcMain.on('window-close', () => mainWindow?.close());

ipcMain.handle('set-content-protection', async (_e, enabled) => {
  const on = !!enabled;
  try {
    mainWindow?.setContentProtection(on);
    return { success: true, enabled: on };
  } catch (e) {
    return { error: e.message || String(e), enabled: false };
  }
});

ipcMain.handle('select-sound-files', async () => {
  const result = await dialog.showOpenDialog(mainWindow, {
    title: 'Add Songs',
    filters: [{ name: 'Audio & Video', extensions: ['mp3','wav','ogg','flac','m4a','aac','opus','webm','mp4','mkv','avi','mov','m4v','wmv','wma','3gp'] }],
    properties: ['openFile', 'multiSelections'],
  });
  if (result.canceled) return [];
  return result.filePaths.map(p => ({
    name: path.basename(p).replace(/\.[^.]+$/, ''),
    path: p,
    type: 'file',
  }));
});

ipcMain.handle('import-plugin-files', async () => {
  const result = await dialog.showOpenDialog(mainWindow, {
    title: 'Import Equalizer APO Sets or Plugin Files',
    filters: [
      { name: 'EQ / Plugin Files', extensions: ['txt', 'cfg', 'config', 'apo', 'json', 'dll', 'vst3'] },
      { name: 'All Files', extensions: ['*'] },
    ],
    properties: ['openFile', 'multiSelections'],
  });
  if (result.canceled) return [];
  const libraryDir = path.join(app.getPath('userData'), 'plugin-library');
  fs.mkdirSync(libraryDir, { recursive: true });
  return result.filePaths.map(filePath => {
    const ext = path.extname(filePath).toLowerCase();
    const stat = fs.statSync(filePath);
    const safeName = path.basename(filePath).replace(/[<>:"/\\|?*\x00-\x1F]/g, '_');
    const storedName = `${Date.now()}-${Math.random().toString(16).slice(2)}-${safeName}`;
    const storedPath = path.join(libraryDir, storedName);
    fs.copyFileSync(filePath, storedPath);
    const isText = ['.txt', '.cfg', '.config', '.apo', '.json'].includes(ext);
    let content = '';
    if (isText && stat.size <= 2 * 1024 * 1024) {
      content = fs.readFileSync(storedPath, 'utf8');
    }
    return {
      name: path.basename(filePath),
      path: filePath,
      storedPath,
      ext,
      size: stat.size,
      type: isText ? 'apo-set' : 'plugin-file',
      content,
    };
  });
});

ipcMain.handle('get-audio-devices', async () => {
  return [];
});

const AI_SYSTEM_PROMPT = `You are an audio EQ preset generator. The user describes a sound they want. Return ONLY valid JSON (no markdown, no explanation) with these fields:
{
  "bands": [{"freq":number,"gain":number,"type":"peaking|lowshelf|highshelf|lowpass|highpass|bandpass|notch","Q":number},...],
  "gain": number (0.5-1.5),
  "volume": number (50-150),
  "speed": number (0.5-2.0, default 1.0),
  "pitch": {"on":boolean,"val":number (-12 to 12)},
  "reverb": {"on":boolean,"dry":number,"wet":number,"damp":number,"room":number},
  "comp": {"on":boolean,"threshold":number(-60 to 0),"ratio":number(1-20),"knee":number(0-40),"attack":number,"release":number},
  "dist": {"on":boolean,"amount":number(0-100),"mix":number(0-1)},
  "delay": {"on":boolean,"time":number(0.01-1),"feedback":number(0-0.9),"mix":number(0-1)},
  "tremolo": {"on":boolean,"speed":number(0.5-20),"depth":number(0-1)},
  "stereo": {"on":boolean,"width":number(1-2)},
  "spatial": {"on":boolean,"speed":number(0.05-2),"depth":number(0.1-5),"orbit":boolean},
  "gate": {"on":boolean,"threshold":number(-70 to -20),"floor":number(0-1)},
  "deEss": {"on":boolean,"freq":number(3500-10000),"amount":number(0-18)},
  "description": "short 1-line description of what was applied"
}
Use 5 EQ bands. Frequency range 20-20000 Hz, gain range -30 to 30 dB. Only enable effects that match the request. Be creative but musically sensible.`;

ipcMain.handle('ai-generate-preset', async (_e, provider, apiKey, prompt) => {
  try {
    if (!apiKey || !prompt) return { error: 'Missing API key or prompt' };

    let body, url, headers, extractResult;

    if (provider === 'openai') {
      url = 'https://api.openai.com/v1/chat/completions';
      headers = { 'Content-Type': 'application/json', 'Authorization': `Bearer ${apiKey}` };
      body = JSON.stringify({
        model: 'gpt-4o-mini',
        messages: [{ role: 'system', content: AI_SYSTEM_PROMPT }, { role: 'user', content: prompt }],
        temperature: 0.7,
        max_tokens: 1000,
      });
      extractResult = data => data.choices?.[0]?.message?.content;
    } else if (provider === 'claude') {
      url = 'https://api.anthropic.com/v1/messages';
      headers = { 'Content-Type': 'application/json', 'x-api-key': apiKey, 'anthropic-version': '2023-06-01' };
      body = JSON.stringify({
        model: 'claude-haiku-4-5-20251001',
        max_tokens: 1000,
        system: AI_SYSTEM_PROMPT,
        messages: [{ role: 'user', content: prompt }],
      });
      extractResult = data => data.content?.[0]?.text;
    } else if (provider === 'gemini') {
      url = `https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent?key=${apiKey}`;
      headers = { 'Content-Type': 'application/json' };
      body = JSON.stringify({
        system_instruction: { parts: [{ text: AI_SYSTEM_PROMPT }] },
        contents: [{ parts: [{ text: prompt }] }],
        generationConfig: { temperature: 0.7, maxOutputTokens: 1000 },
      });
      extractResult = data => data.candidates?.[0]?.content?.parts?.[0]?.text;
    } else if (provider === 'openrouter') {
      url = 'https://openrouter.ai/api/v1/chat/completions';
      headers = { 'Content-Type': 'application/json', 'Authorization': `Bearer ${apiKey}` };
      body = JSON.stringify({
        model: 'google/gemini-2.5-flash-preview-05-20',
        messages: [{ role: 'system', content: AI_SYSTEM_PROMPT }, { role: 'user', content: prompt }],
        temperature: 0.7,
        max_tokens: 1000,
      });
      extractResult = data => data.choices?.[0]?.message?.content;
    } else {
      return { error: 'Unknown provider' };
    }

    const res = await fetch(url, { method: 'POST', headers, body });
    if (!res.ok) {
      const err = await res.text().catch(() => '');
      return { error: `API error ${res.status}: ${err.slice(0, 200)}` };
    }

    const data = await res.json();
    const raw = extractResult(data) || '';
    const jsonStr = raw.replace(/```json\s*/g, '').replace(/```\s*/g, '').trim();
    const parsed = JSON.parse(jsonStr);
    return { result: parsed };
  } catch (e) {
    return { error: e.message };
  }
});

function sendDownloadProgress(source, percent) {
  try { mainWindow?.webContents?.send('audio-download-progress', { source, percent }); } catch {}
}

// Adding a link downloads it up front, so the library entry points at a local file.
// Windows owns the run-at-login state, so it is read back rather than
// mirrored in our own settings where the two could disagree.
// ── Spotify bridge ──
// Follows the Spotify desktop app through Windows' media session API. A small
// PowerShell process does the WinRT work - no native build, no Spotify login -
// and speaks JSON lines: state on stdout, commands on stdin.
let spotifyProc = null;
let spotifyWanted = false;
let spotifyRestartTimer = null;
let spotifyRestarts = 0;
let spotifyLastState = { available: false };
let spotifyLastCover = '';

function spotifyBridgePath() {
  let p = path.join(__dirname, 'spotify-bridge.ps1');
  // PowerShell cannot read a script from inside the asar archive.
  if (p.includes('app.asar') && !p.includes('app.asar.unpacked')) p = p.replace('app.asar', 'app.asar.unpacked');
  return p;
}

function sendSpotifyState(state) {
  if (mainWindow && !mainWindow.isDestroyed()) mainWindow.webContents.send('spotify:state', state);
}

function startSpotifyBridge() {
  spotifyWanted = true;
  if (spotifyProc) return { ok: true };
  const script = spotifyBridgePath();
  if (!fs.existsSync(script)) return { error: 'Spotify bridge script is missing.' };

  const child = spawn('powershell.exe', ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', script], {
    windowsHide: true,
    stdio: ['pipe', 'pipe', 'pipe'],
  });
  spotifyProc = child;

  let buffer = '';
  child.stdout.setEncoding('utf8');
  child.stdout.on('data', chunk => {
    buffer += chunk;
    let nl;
    while ((nl = buffer.indexOf('\n')) >= 0) {
      const line = buffer.slice(0, nl).replace(/^﻿/, '').trim();
      buffer = buffer.slice(nl + 1);
      if (!line) continue;
      let msg;
      try { msg = JSON.parse(line); } catch { continue; }
      if (msg.type === 'state') {
        spotifyRestarts = 0;
        const state = { ...msg, receivedAt: Date.now() };
        // Cover art is only sent when the track changes; keep it for late
        // listeners such as a reloaded window.
        if (Object.prototype.hasOwnProperty.call(msg, 'cover')) spotifyLastCover = msg.cover || '';
        if (!msg.available) spotifyLastCover = '';
        spotifyLastState = { ...state };
        delete spotifyLastState.cover;
        sendSpotifyState(state);
      } else if (msg.type === 'error') {
        console.warn('[spotify]', msg.message);
      }
    }
  });
  child.stderr.on('data', d => console.warn('[spotify] stderr:', String(d).trim().slice(0, 300)));
  child.on('error', e => console.warn('[spotify] could not start:', e.message));
  child.on('exit', () => {
    if (spotifyProc === child) spotifyProc = null;
    if (!spotifyWanted) return;
    // Unexpected exit: retry with backoff rather than respawning in a tight loop.
    spotifyRestarts++;
    const delay = Math.min(30000, 1000 * 2 ** Math.min(spotifyRestarts, 5));
    clearTimeout(spotifyRestartTimer);
    spotifyRestartTimer = setTimeout(() => { if (spotifyWanted) startSpotifyBridge(); }, delay);
  });
  return { ok: true };
}

function stopSpotifyBridge() {
  spotifyWanted = false;
  clearTimeout(spotifyRestartTimer);
  if (spotifyProc) {
    try { spotifyProc.stdin.end(); } catch {}
    try { spotifyProc.kill(); } catch {}
    spotifyProc = null;
  }
  spotifyLastState = { available: false };
  spotifyLastCover = '';
}

const SPOTIFY_COMMANDS = new Set(['toggle', 'play', 'pause', 'next', 'prev', 'seek', 'shuffle']);

ipcMain.handle('spotify:start', () => startSpotifyBridge());
ipcMain.handle('spotify:stop', () => { stopSpotifyBridge(); return { ok: true }; });
ipcMain.handle('spotify:get', () => ({ ...spotifyLastState, cover: spotifyLastCover }));
ipcMain.handle('spotify:command', (_e, cmd, arg) => {
  if (!SPOTIFY_COMMANDS.has(cmd)) return { error: 'Unknown command.' };
  if (!spotifyProc) return { error: 'Not connected.' };
  let line = cmd;
  if (cmd === 'seek') {
    const ms = Number(arg);
    if (!Number.isFinite(ms) || ms < 0) return { error: 'Bad position.' };
    line += ' ' + Math.round(ms);
  }
  if (cmd === 'shuffle') line += ' ' + (arg ? 'true' : 'false');
  try {
    spotifyProc.stdin.write(line + '\n');
    return { ok: true };
  } catch (e) {
    return { error: e.message };
  }
});

// ── Song artwork ──
// Links carry a thumbnail in their metadata; local files often have cover art
// embedded, which ffmpeg can extract. Either way the image is saved under
// userData, so the library never depends on a remote URL staying valid.
const COVER_DIR = path.join(app.getPath('userData'), 'covers');
const inFlightCovers = new Map();

function coverExtension(contentType, url) {
  const t = String(contentType || '').toLowerCase();
  if (t.includes('png')) return '.png';
  if (t.includes('webp')) return '.webp';
  if (t.includes('jpeg') || t.includes('jpg')) return '.jpg';
  const m = String(url || '').match(/\.(jpe?g|png|webp)(?:[?#]|$)/i);
  return m ? '.' + m[1].toLowerCase().replace('jpeg', 'jpg') : '.jpg';
}

function existingCover(key) {
  for (const ext of ['.jpg', '.png', '.webp']) {
    const p = path.join(COVER_DIR, key + ext);
    try { if (fs.statSync(p).size > 0) return p; } catch {}
  }
  return '';
}

// Largest thumbnail wins; yt-dlp's single `thumbnail` field is usually it.
function pickThumbnail(info) {
  if (!info) return '';
  if (typeof info.thumbnail === 'string' && isHttpUrl(info.thumbnail)) return info.thumbnail;
  const list = Array.isArray(info.thumbnails) ? info.thumbnails.filter(t => t && isHttpUrl(t.url)) : [];
  if (!list.length) return '';
  list.sort((a, b) => ((b.width || 0) * (b.height || 0)) - ((a.width || 0) * (a.height || 0)));
  return list[0].url;
}

async function downloadCover(url, key) {
  if (!isHttpUrl(url)) return '';
  const hit = existingCover(key);
  if (hit) return hit;
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), 15000);
  try {
    const res = await fetch(url, { signal: controller.signal });
    if (!res.ok) return '';
    const type = res.headers.get('content-type') || '';
    if (type && !/^image\//i.test(type)) return '';
    const buf = Buffer.from(await res.arrayBuffer());
    if (buf.length < 64 || buf.length > 8 * 1024 * 1024) return '';
    fs.mkdirSync(COVER_DIR, { recursive: true });
    const file = path.join(COVER_DIR, key + coverExtension(type, url));
    fs.writeFileSync(file, buf);
    return file;
  } catch {
    return '';
  } finally {
    clearTimeout(timer);
  }
}

function extractEmbeddedCover(filePath, key) {
  const hit = existingCover(key);
  if (hit) return Promise.resolve(hit);
  const ffmpeg = findFfmpeg();
  if (!ffmpeg) return Promise.resolve('');
  fs.mkdirSync(COVER_DIR, { recursive: true });
  const out = path.join(COVER_DIR, key + '.jpg');
  // Audio files store art as an attached-picture video stream. Real video
  // gets a frame a few seconds in, since the opening is often black.
  const isVideo = /\.(mp4|mkv|avi|mov|m4v|wmv|3gp)$/i.test(filePath);
  const args = ['-y', '-v', 'error', ...(isVideo ? ['-ss', '3'] : []), '-i', filePath,
    '-an', '-map', '0:v:0', '-frames:v', '1',
    '-vf', 'scale=320:320:force_original_aspect_ratio=increase,crop=320:320', out];
  return new Promise(resolve => {
    execFile(ffmpeg, args, { windowsHide: true, timeout: 20000 }, err => {
      try { if (!err && fs.statSync(out).size > 0) return resolve(out); } catch {}
      try { fs.unlinkSync(out); } catch {}
      resolve('');
    });
  });
}

async function resolveSongCover({ path: filePath, sourceUrl } = {}) {
  if (sourceUrl && isHttpUrl(sourceUrl)) {
    const key = audioCacheKey(sourceUrl);
    const hit = existingCover(key);
    if (hit) return hit;
    // Prefer metadata already resolved this session; otherwise ask yt-dlp for
    // metadata only - no stream lookup, no download.
    let thumb = pickThumbnail(onlineAudioCache.get(String(sourceUrl).trim()));
    if (!thumb) {
      try {
        let target = sourceUrl;
        if (/open\.spotify\.com|spotify\.link/i.test(sourceUrl)) {
          const query = await spotifyToSearchQuery(sourceUrl);
          if (!query) return '';
          target = `ytsearch1:${query} official audio`;
        }
        const raw = await runYtDlp(target, { dumpSingleJson: true, skipDownload: true, noPlaylist: true, noWarnings: true, ignoreErrors: true });
        thumb = pickThumbnail(Array.isArray(raw?.entries) ? raw.entries[0] : raw);
      } catch {
        return '';
      }
    }
    return thumb ? downloadCover(thumb, key) : '';
  }
  if (typeof filePath === 'string' && filePath && !isHttpUrl(filePath)) {
    let stat;
    try { stat = fs.statSync(filePath); } catch { return ''; }
    if (!stat.isFile()) return '';
    // Keyed on size and mtime too, so a replaced file gets fresh artwork.
    const key = crypto.createHash('sha1').update(filePath + '|' + stat.size + '|' + stat.mtimeMs).digest('hex');
    return extractEmbeddedCover(filePath, key);
  }
  return '';
}

ipcMain.handle('song-cover', (_e, song) => {
  const id = JSON.stringify([song?.path || '', song?.sourceUrl || '']);
  if (inFlightCovers.has(id)) return inFlightCovers.get(id);
  const job = resolveSongCover(song || {}).catch(() => '').finally(() => inFlightCovers.delete(id));
  inFlightCovers.set(id, job);
  return job;
});

// ── Spotify into the mic ──
// A native addon captures only Spotify's audio (Windows per-app loopback), so
// a Discord call on the same speakers is never sent back to the people in it.
// Samples go to the window over a MessagePort, straight into an AudioWorklet.
let spotifyCapture = null;
try {
  let addonPath = path.join(__dirname, 'native', 'build', 'Release', 'spotify_capture.node');
  if (addonPath.includes('app.asar') && !addonPath.includes('app.asar.unpacked')) {
    addonPath = addonPath.replace('app.asar', 'app.asar.unpacked');
  }
  spotifyCapture = require(addonPath);
} catch (e) {
  console.warn('Spotify capture addon not available:', e.message);
}

let spotifyAudioWanted = false;
let spotifyAudioRate = 48000;
let spotifyAudioPid = 0;
let spotifyAudioChannel = null;
let spotifyAudioWatch = null;

function sendSpotifyAudioStatus(status) {
  if (mainWindow && !mainWindow.isDestroyed()) mainWindow.webContents.send('spotify-audio:status', status);
}

function beginSpotifyCapture() {
  const pid = spotifyCapture.findProcess('Spotify.exe');
  if (!pid) {
    try { spotifyCapture.stop(); } catch {}
    spotifyAudioPid = 0;
    sendSpotifyAudioStatus({ state: 'waiting', error: '' });
    return false;
  }
  try { spotifyCapture.stop(); } catch {}
  if (spotifyAudioChannel) { try { spotifyAudioChannel.port1.close(); } catch {} }

  // A fresh port per capture: the page hands it to its worklet, and an old
  // port from a reloaded page or an earlier capture is simply abandoned.
  spotifyAudioChannel = new MessageChannelMain();
  const port = spotifyAudioChannel.port1;
  port.start();
  mainWindow.webContents.postMessage('spotify-audio-port', null, [spotifyAudioChannel.port2]);

  spotifyAudioPid = pid;
  sendSpotifyAudioStatus({ state: 'starting', error: '', pid });
  try {
    spotifyCapture.start(pid, spotifyAudioRate, (type, payload) => {
      if (type === 'audio') {
        try { port.postMessage(payload); } catch {}
      } else if (type === 'status') {
        sendSpotifyAudioStatus({ ...payload, pid });
      }
    });
  } catch (e) {
    sendSpotifyAudioStatus({ state: 'error', error: e.message, pid });
    return false;
  }
  return true;
}

function startSpotifyAudio(sampleRate) {
  if (!spotifyCapture) return { error: 'Spotify capture is not available in this build.' };
  if (!mainWindow || mainWindow.isDestroyed()) return { error: 'No window to send audio to.' };
  spotifyAudioWanted = true;
  spotifyAudioRate = Number(sampleRate) > 0 ? Math.round(Number(sampleRate)) : 48000;
  beginSpotifyCapture();

  // Spotify restarting gets a new process id, and Spotify may be opened after
  // this is switched on: follow it rather than capturing a dead process.
  clearInterval(spotifyAudioWatch);
  spotifyAudioWatch = setInterval(() => {
    if (!spotifyAudioWanted) return;
    const pid = spotifyCapture.findProcess('Spotify.exe');
    if (pid && (pid !== spotifyAudioPid || !spotifyCapture.isRunning())) {
      beginSpotifyCapture();
    } else if (!pid && spotifyAudioPid) {
      try { spotifyCapture.stop(); } catch {}
      spotifyAudioPid = 0;
      sendSpotifyAudioStatus({ state: 'waiting', error: '' });
    }
  }, 3000);
  return { ok: true };
}

function stopSpotifyAudio() {
  spotifyAudioWanted = false;
  clearInterval(spotifyAudioWatch);
  spotifyAudioWatch = null;
  try { spotifyCapture?.stop(); } catch {}
  if (spotifyAudioChannel) {
    try { spotifyAudioChannel.port1.close(); } catch {}
    spotifyAudioChannel = null;
  }
  spotifyAudioPid = 0;
}

ipcMain.handle('spotify-audio:start', (_e, sampleRate) => startSpotifyAudio(sampleRate));
ipcMain.handle('spotify-audio:stop', () => { stopSpotifyAudio(); return { ok: true }; });

ipcMain.handle('get-open-at-login', () => {
  try { return app.getLoginItemSettings().openAtLogin; } catch { return false; }
});

ipcMain.handle('set-open-at-login', (_e, enabled) => {
  try {
    app.setLoginItemSettings({
      openAtLogin: !!enabled,
      path: process.execPath,
      // Started minimised: opening on sign-in should not take over the screen.
      args: ['--startup'],
    });
    return { openAtLogin: app.getLoginItemSettings().openAtLogin };
  } catch (e) {
    return { error: e.message };
  }
});

ipcMain.handle('resolve-link', async (_e, url) => {
  try {
    const entry = await ensureLocalAudio(url, p => sendDownloadProgress(url, p));
    return { name: entry.name, path: entry.path, sourceUrl: url, durationMs: entry.durationMs, type: 'link', cover: entry.cover || '' };
  } catch (e) {
    return { error: e.message };
  }
});

// Cache-first lookup used on playback; re-downloads if the file went missing.
ipcMain.handle('ensure-local-audio', async (_e, url) => {
  try {
    const entry = await ensureLocalAudio(url, p => sendDownloadProgress(url, p));
    return { path: entry.path, name: entry.name, durationMs: entry.durationMs };
  } catch (e) {
    return { error: e.message };
  }
});

ipcMain.handle('get-cached-audio', (_e, url) => {
  const entry = readAudioCacheEntry(url);
  return entry ? { path: entry.path, name: entry.name, durationMs: entry.durationMs } : null;
});

ipcMain.handle('resolve-stream', async (_e, url) => {
  try {
    const info = await resolveOnlineAudioSource(url);
    return { streamUrl: info.streamUrl, durationMs: info.durationMs };
  } catch (e) {
    return { error: e.message };
  }
});

// VST2 Plugin IPC
ipcMain.handle('vst-available', () => !!vstHost);

ipcMain.handle('vst-load', async (_e, dllPath) => {
  if (!vstHost) return { error: 'VST host not available' };
  try {
    const info = vstHost.loadPlugin(dllPath);
    return info;
  } catch (e) {
    return { error: e.message };
  }
});

ipcMain.handle('vst-unload', async (_e, id) => {
  if (!vstHost) return;
  try { vstHost.unloadPlugin(id); } catch {}
});

ipcMain.handle('vst-list', () => {
  if (!vstHost) return [];
  try { return vstHost.listPlugins(); } catch { return []; }
});

ipcMain.handle('vst-info', async (_e, id) => {
  if (!vstHost) return null;
  try { return vstHost.getPluginInfo(id); } catch { return null; }
});

ipcMain.handle('vst-set-param', async (_e, id, paramIdx, value) => {
  if (!vstHost) return;
  try { vstHost.setPluginParam(id, paramIdx, value); } catch {}
});

ipcMain.handle('vst-set-enabled', async (_e, id, enabled) => {
  if (!vstHost) return;
  try { vstHost.setPluginEnabled(id, enabled); } catch {}
});

ipcMain.handle('vst-process', async (_e, id, leftArr, rightArr) => {
  if (!vstHost) return { left: leftArr, right: rightArr };
  try {
    const left = new Float32Array(leftArr);
    const right = new Float32Array(rightArr);
    const result = vstHost.processBlock(id, left, right);
    return { left: Array.from(result.left), right: Array.from(result.right) };
  } catch {
    return { left: leftArr, right: rightArr };
  }
});

ipcMain.handle('vst-process-all', async (_e, leftArr, rightArr) => {
  if (!vstHost) return { left: leftArr, right: rightArr };
  try {
    const left = new Float32Array(leftArr);
    const right = new Float32Array(rightArr);
    const result = vstHost.processAll(left, right);
    return { left: Array.from(result.left), right: Array.from(result.right) };
  } catch {
    return { left: leftArr, right: rightArr };
  }
});

ipcMain.handle('vst-open-editor', async (_e, id) => {
  if (!vstHost) return { error: 'VST host not available' };
  try {
    vstHost.openEditor(id);
    return { ok: true };
  } catch (e) {
    return { error: e.message };
  }
});

ipcMain.handle('vst-close-editor', async (_e, id) => {
  if (!vstHost) return;
  try { vstHost.closeEditor(id); } catch {}
});

ipcMain.handle('convert-to-audio', async (_e, filePath) => {
  try {
    const outputDir = path.join(app.getPath('userData'), 'converted-audio');
    fs.mkdirSync(outputDir, { recursive: true });
    const baseName = path.basename(filePath, path.extname(filePath));
    const outPath = path.join(outputDir, `${baseName}-${Date.now()}.mp3`);

    // Try ffmpeg first
    const ffmpegPaths = ['ffmpeg', 'C:\\ffmpeg\\bin\\ffmpeg.exe'];
    let ffmpegBin = null;
    for (const p of ffmpegPaths) {
      try {
        require('child_process').execFileSync(p, ['-version'], { windowsHide: true, stdio: 'ignore' });
        ffmpegBin = p;
        break;
      } catch {}
    }

    if (ffmpegBin) {
      await new Promise((resolve, reject) => {
        execFile(ffmpegBin, ['-i', filePath, '-vn', '-acodec', 'libmp3lame', '-q:a', '2', '-y', outPath], {
          windowsHide: true, timeout: 120000,
        }, (err) => err ? reject(err) : resolve());
      });
      return { path: outPath };
    }

    // No ffmpeg — return original path, Chromium can often play video audio tracks
    return { path: filePath, noConvert: true };
  } catch (e) {
    return { error: e.message, path: filePath };
  }
});

ipcMain.handle('vst-select-dll', async () => {
  const result = await dialog.showOpenDialog(mainWindow, {
    title: 'Select VST2 Plugin',
    filters: [{ name: 'VST Plugins', extensions: ['dll'] }],
    properties: ['openFile'],
  });
  if (result.canceled || !result.filePaths.length) return null;
  return result.filePaths[0];
});

ipcMain.handle('select-video-files', async () => {
  const result = await dialog.showOpenDialog(mainWindow, {
    title: 'Add Video Files',
    filters: [{ name: 'Video Files', extensions: ['mp4','mkv','avi','mov','webm','m4v','wmv','flv','3gp'] }],
    properties: ['openFile', 'multiSelections'],
  });
  if (result.canceled) return [];
  return result.filePaths.map(p => ({
    name: path.basename(p).replace(/\.[^.]+$/, ''),
    path: p,
    type: 'file',
  }));
});

// ── Virtual Audio Cable IPC ──

ipcMain.handle('vaudio:status', async () => {
  return new Promise((resolve) => {
    exec('powershell -Command "Get-AudioDevice -List 2>$null; if($?){exit 0}else{ Get-CimInstance Win32_SoundDevice | Select-Object Name }"', { windowsHide: true }, (err, stdout) => {
      const out = (stdout || '').toLowerCase();
      const installed = out.includes('cable input') || out.includes('cable output') || out.includes('vb-audio');
      resolve({ installed });
    });
  });
});

ipcMain.handle('vaudio:install', async () => {
  const { net } = require('electron');
  const os = require('os');
  const tmpDir = path.join(os.tmpdir(), 'vbcable_install');
  const zipPath = path.join(tmpDir, 'VBCABLE_Driver_Pack.zip');
  const setupExe = path.join(tmpDir, 'VBCABLE_Setup_x64.exe');

  try {
    if (!fs.existsSync(tmpDir)) fs.mkdirSync(tmpDir, { recursive: true });

    if (!fs.existsSync(setupExe)) {
      const url = 'https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack43.zip';
      const response = await net.fetch(url);
      if (!response.ok) return { error: `Download failed: ${response.status}` };
      const arrayBuf = await response.arrayBuffer();
      fs.writeFileSync(zipPath, Buffer.from(arrayBuf));

      await new Promise((resolve, reject) => {
        exec(`powershell -Command "Expand-Archive -Path '${zipPath}' -DestinationPath '${tmpDir}' -Force"`, { windowsHide: true }, (err) => {
          if (err) reject(new Error('Failed to extract VB-Cable archive.')); else resolve();
        });
      });
    }

    if (!fs.existsSync(setupExe)) return { error: 'VBCABLE_Setup_x64.exe not found after extraction.' };

    return new Promise((resolve) => {
      exec(`"${setupExe}" -i -h`, { windowsHide: true, timeout: 30000 }, (err) => {
        if (err) {
          exec(`"${setupExe}"`, { windowsHide: true }, () => {
            resolve({ ok: true, note: 'VB-Cable installer opened. Follow the prompts to install.' });
          });
        } else {
          resolve({ ok: true });
        }
      });
    });
  } catch (e) {
    return { error: e.message || 'Installation failed.' };
  }
});

ipcMain.handle('vaudio:uninstall', async () => {
  return { error: 'To uninstall VB-Cable, use Windows Settings > Apps > VB-Audio Virtual Cable.' };
});

// ── Virtual Camera IPC (Custom Mic Virtual Camera) ──
let vcamSender = null;
let vcamOpen = false;

function getUnpackedNativePath(fileName) {
  if (app.isPackaged) {
    return path.join(process.resourcesPath, 'app.asar.unpacked', 'native', 'build', 'Release', fileName);
  }
  return path.join(__dirname, 'native', 'build', 'Release', fileName);
}

function loadVcamSender() {
  if (vcamSender) return vcamSender;
  const senderPath = getUnpackedNativePath('vcam_sender.node');
  vcamSender = require(senderPath);
  return vcamSender;
}

ipcMain.handle('vcam:install', async () => {
  const dllPath = getUnpackedNativePath('virtual_cam.dll');
  if (!fs.existsSync(dllPath)) return { error: 'Custom Mic virtual camera driver not found in app files.' };
  return new Promise((resolve) => {
    execFile('regsvr32.exe', ['/s', dllPath], { windowsHide: true }, (err) => {
      if (err) resolve({ error: 'Failed to register Custom Mic Virtual Camera. Run the app as Administrator, then try again.' });
      else resolve({ ok: true, note: 'Custom Mic Virtual Camera registered. Select "Custom Mic Virtual Camera" in Discord/Zoom.' });
    });
  });
});

ipcMain.handle('vcam:start', async (_event, width = 1280, height = 720) => {
  try {
    const sender = loadVcamSender();
    sender.open(Number(width) || 1280, Number(height) || 720);
    vcamOpen = true;
    return { ok: true, note: 'Custom Mic Virtual Camera is active.' };
  } catch (e) {
    vcamOpen = false;
    return { error: e.message || 'Failed to start Custom Mic Virtual Camera.' };
  }
});

ipcMain.handle('vcam:send-frame', async (_event, frame) => {
  if (!vcamOpen) return { error: 'Virtual camera is not active.' };
  try {
    const sender = loadVcamSender();
    const buffer = Buffer.isBuffer(frame) ? frame : Buffer.from(frame);
    sender.sendFrame(buffer);
    return { ok: true };
  } catch (e) {
    return { error: e.message || 'Failed to send virtual camera frame.' };
  }
});

ipcMain.handle('vcam:stop', async () => {
  try {
    if (vcamSender && vcamOpen) vcamSender.close();
    vcamOpen = false;
  } catch {}
});

// ── Screen Recorder IPC ──
ipcMain.handle('recorder:get-sources', async (_e, type) => {
  const types = type === 'window' ? ['window'] : ['screen'];
  const sources = await desktopCapturer.getSources({ types, thumbnailSize: { width: 320, height: 180 } });
  return sources.map(s => ({ id: s.id, name: s.name, thumbnail: s.thumbnail.toDataURL() }));
});

ipcMain.handle('recorder:select-output-dir', async () => {
  const result = await dialog.showOpenDialog(mainWindow, {
    title: 'Select Output Folder',
    properties: ['openDirectory'],
  });
  if (result.canceled) return null;
  return result.filePaths[0];
});

ipcMain.handle('recorder:save-recording', async (_e, buffer, filename) => {
  try {
    const buf = Buffer.from(buffer);
    fs.writeFileSync(filename, buf);
    return { ok: true, path: filename, size: buf.length };
  } catch (e) {
    return { error: e.message };
  }
});

ipcMain.handle('recorder:open-file', async (_e, filePath) => {
  shell.openPath(filePath);
});

ipcMain.handle('recorder:open-folder', async (_e, filePath) => {
  shell.showItemInFolder(filePath);
});
