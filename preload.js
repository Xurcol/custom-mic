const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('api', {
  selectSoundFiles: () => ipcRenderer.invoke('select-sound-files'),
  importPluginFiles: () => ipcRenderer.invoke('import-plugin-files'),
  getAudioDevices: () => ipcRenderer.invoke('get-audio-devices'),
  aiGeneratePreset: (provider, apiKey, prompt) => ipcRenderer.invoke('ai-generate-preset', provider, apiKey, prompt),
  convertToAudio: (filePath) => ipcRenderer.invoke('convert-to-audio', filePath),
  resolveLink: (url) => ipcRenderer.invoke('resolve-link', url),
  resolveStream: (url) => ipcRenderer.invoke('resolve-stream', url),
  ensureLocalAudio: (url) => ipcRenderer.invoke('ensure-local-audio', url),
  getCachedAudio: (url) => ipcRenderer.invoke('get-cached-audio', url),
  onAudioDownloadProgress: (cb) => {
    const handler = (_e, data) => cb(data);
    ipcRenderer.on('audio-download-progress', handler);
    return () => ipcRenderer.removeListener('audio-download-progress', handler);
  },
  vstAvailable: () => ipcRenderer.invoke('vst-available'),
  vstLoad: (dllPath) => ipcRenderer.invoke('vst-load', dllPath),
  vstUnload: (id) => ipcRenderer.invoke('vst-unload', id),
  vstList: () => ipcRenderer.invoke('vst-list'),
  vstInfo: (id) => ipcRenderer.invoke('vst-info', id),
  vstSetParam: (id, paramIdx, value) => ipcRenderer.invoke('vst-set-param', id, paramIdx, value),
  vstSetEnabled: (id, enabled) => ipcRenderer.invoke('vst-set-enabled', id, enabled),
  vstProcess: (id, left, right) => ipcRenderer.invoke('vst-process', id, Array.from(left), Array.from(right)),
  vstProcessAll: (left, right) => ipcRenderer.invoke('vst-process-all', Array.from(left), Array.from(right)),
  vstOpenEditor: (id) => ipcRenderer.invoke('vst-open-editor', id),
  vstCloseEditor: (id) => ipcRenderer.invoke('vst-close-editor', id),
  vstSelectDll: () => ipcRenderer.invoke('vst-select-dll'),
  selectVideoFiles: () => ipcRenderer.invoke('select-video-files'),
  vaudioStatus: () => ipcRenderer.invoke('vaudio:status'),
  vaudioInstall: () => ipcRenderer.invoke('vaudio:install'),
  vaudioUninstall: () => ipcRenderer.invoke('vaudio:uninstall'),
  vcamInstall: () => ipcRenderer.invoke('vcam:install'),
  vcamStart: (width, height) => ipcRenderer.invoke('vcam:start', width, height),
  vcamSendFrame: (buffer) => ipcRenderer.invoke('vcam:send-frame', buffer),
  vcamStop: () => ipcRenderer.invoke('vcam:stop'),
  recorderGetSources: (type) => ipcRenderer.invoke('recorder:get-sources', type),
  recorderSelectOutputDir: () => ipcRenderer.invoke('recorder:select-output-dir'),
  recorderSaveRecording: (buffer, filename) => ipcRenderer.invoke('recorder:save-recording', buffer, filename),
  recorderOpenFile: (path) => ipcRenderer.invoke('recorder:open-file', path),
  recorderOpenFolder: (path) => ipcRenderer.invoke('recorder:open-folder', path),
  setContentProtection: (enabled) => ipcRenderer.invoke('set-content-protection', !!enabled),
  aiVoice: {
    selectFolder: () => ipcRenderer.invoke('aivoice:select-folder'),
    status: () => ipcRenderer.invoke('aivoice:status'),
    start: (folder) => ipcRenderer.invoke('aivoice:start', folder),
    stop: () => ipcRenderer.invoke('aivoice:stop'),
    openFolder: (folder) => ipcRenderer.invoke('aivoice:open-folder', folder),
    info: () => ipcRenderer.invoke('aivoice:info'),
    performance: () => ipcRenderer.invoke('aivoice:performance'),
    updateSetting: (key, value) => ipcRenderer.invoke('aivoice:update-setting', key, value),
    connect: () => ipcRenderer.invoke('aivoice:connect'),
    sendAudio: (timestamp, buffer) => ipcRenderer.invoke('aivoice:send-audio', timestamp, buffer),
    disconnect: () => ipcRenderer.invoke('aivoice:disconnect'),
    onAudioResponse: (callback) => {
      const handler = (_event, data) => callback(data);
      ipcRenderer.on('aivoice:audio-response', handler);
      return () => ipcRenderer.removeListener('aivoice:audio-response', handler);
    },
  },
});

contextBridge.exposeInMainWorld('windowControls', {
  minimize: () => ipcRenderer.send('window-minimize'),
  maximize: () => ipcRenderer.send('window-maximize'),
  close: () => ipcRenderer.send('window-close'),
});
