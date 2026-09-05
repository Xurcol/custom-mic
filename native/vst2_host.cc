#include <napi.h>
#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstring>
#include <cstdint>
#include <thread>
#include <atomic>
#include <mutex>

#pragma pack(push, 8)

typedef int32_t VstInt32;
typedef intptr_t VstIntPtr;

enum VstOpcodes {
  effOpen = 0,
  effClose = 1,
  effSetProgram = 2,
  effGetProgram = 3,
  effGetParamLabel = 6,
  effGetParamDisplay = 7,
  effGetParamName = 8,
  effSetSampleRate = 10,
  effSetBlockSize = 11,
  effMainsChanged = 12,
  effEditGetRect = 13,
  effEditOpen = 14,
  effEditClose = 15,
  effEditIdle = 19,
  effGetEffectName = 45,
  effGetVendorString = 47,
  effGetProductString = 48,
  effCanDo = 51,
  effStartProcess = 71,
  effStopProcess = 72,
};

enum VstFlags {
  effFlagsHasEditor = 1 << 0,
  effFlagsCanReplacing = 1 << 4,
  effFlagsCanDoubleReplacing = 1 << 12,
};

struct ERect {
  int16_t top, left, bottom, right;
};

const VstInt32 kEffectMagic = 0x56737450;

struct AEffect;
typedef VstIntPtr (*audioMasterCallback)(AEffect*, VstInt32, VstInt32, VstIntPtr, void*, float);
typedef AEffect* (*VstPluginMain)(audioMasterCallback);

struct AEffect {
  VstInt32 magic;
  VstIntPtr (*dispatcher)(AEffect*, VstInt32, VstInt32, VstIntPtr, void*, float);
  void (*process)(AEffect*, float**, float**, VstInt32);
  void (*setParameter)(AEffect*, VstInt32, float);
  float (*getParameter)(AEffect*, VstInt32);
  VstInt32 numPrograms;
  VstInt32 numParams;
  VstInt32 numInputs;
  VstInt32 numOutputs;
  VstInt32 flags;
  VstIntPtr resvd1;
  VstIntPtr resvd2;
  VstInt32 initialDelay;
  VstInt32 _realQualities;
  VstInt32 _offQualities;
  float _ioRatio;
  void* object;
  void* user;
  VstInt32 uniqueID;
  VstInt32 version;
  void (*processReplacing)(AEffect*, float**, float**, VstInt32);
  void (*processDoubleReplacing)(AEffect*, double**, double**, VstInt32);
  char future[56];
};

#pragma pack(pop)

static VstIntPtr CALLBACK hostCallback(AEffect* effect, VstInt32 opcode, VstInt32 index,
                                        VstIntPtr value, void* ptr, float opt) {
  switch (opcode) {
    case 1:  return 2400;
    case 6:  return 1;
    case 7:  return 0;
    case 13: return 0; // audioMasterIdle
    case 14: return 0; // audioMasterSizeWindow
    case 32: if (ptr) std::strncpy((char*)ptr, "Custom Mic VST Host", 64); return 1;
    case 33: if (ptr) std::strncpy((char*)ptr, "CustomMic", 64); return 1;
    case 42: return 0; // audioMasterUpdateDisplay
    default: return 0;
  }
}

static const wchar_t* VST_WND_CLASS = L"CustomMicVSTEditor";
static bool wndClassRegistered = false;

struct PluginInstance {
  HMODULE hModule = nullptr;
  AEffect* effect = nullptr;
  std::string name;
  std::string vendor;
  std::string path;
  float sampleRate = 44100.0f;
  int blockSize = 512;
  bool active = false;
  bool enabled = true;
  bool hasEditor = false;
  HWND editorHwnd = nullptr;
  std::thread editorThread;
  std::atomic<bool> editorOpen{false};
  std::atomic<bool> editorShouldClose{false};
  std::vector<float> inputL, inputR, outputL, outputR;
};

static std::unordered_map<int, PluginInstance> plugins;
static int nextPluginId = 1;
static std::mutex pluginMutex;

static std::string getEffectString(AEffect* effect, VstInt32 opcode) {
  char buf[256] = {0};
  effect->dispatcher(effect, opcode, 0, 0, buf, 0);
  return std::string(buf);
}

static void registerWindowClass() {
  if (wndClassRegistered) return;
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(WNDCLASSEXW);
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.lpszClassName = VST_WND_CLASS;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  RegisterClassExW(&wc);
  wndClassRegistered = true;
}

static void editorThreadFunc(PluginInstance* p, int pluginId) {
  registerWindowClass();

  ERect* rect = nullptr;
  p->effect->dispatcher(p->effect, effEditGetRect, 0, 0, &rect, 0);

  int w = 800, h = 600;
  if (rect) {
    w = rect->right - rect->left;
    h = rect->bottom - rect->top;
    if (w < 100) w = 800;
    if (h < 100) h = 600;
  }

  std::wstring title = L"VST: ";
  for (char c : p->name) title += (wchar_t)c;

  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT wr = {0, 0, (LONG)w, (LONG)h};
  AdjustWindowRect(&wr, style, FALSE);

  HWND hwnd = CreateWindowExW(
    0, VST_WND_CLASS, title.c_str(), style,
    CW_USEDEFAULT, CW_USEDEFAULT,
    wr.right - wr.left, wr.bottom - wr.top,
    nullptr, nullptr, GetModuleHandle(nullptr), nullptr
  );

  if (!hwnd) {
    p->editorOpen = false;
    return;
  }

  p->editorHwnd = hwnd;
  p->effect->dispatcher(p->effect, effEditOpen, 0, 0, (void*)hwnd, 0);

  // Re-query rect after open (some plugins set it here)
  ERect* rect2 = nullptr;
  p->effect->dispatcher(p->effect, effEditGetRect, 0, 0, &rect2, 0);
  if (rect2) {
    int w2 = rect2->right - rect2->left;
    int h2 = rect2->bottom - rect2->top;
    if (w2 > 50 && h2 > 50) {
      RECT wr2 = {0, 0, (LONG)w2, (LONG)h2};
      AdjustWindowRect(&wr2, style, FALSE);
      SetWindowPos(hwnd, nullptr, 0, 0, wr2.right - wr2.left, wr2.bottom - wr2.top,
                   SWP_NOMOVE | SWP_NOZORDER);
    }
  }

  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);

  SetTimer(hwnd, 1, 30, nullptr);

  MSG msg;
  while (!p->editorShouldClose.load()) {
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        p->editorShouldClose = true;
        break;
      }
      if (msg.message == WM_TIMER && msg.wParam == 1) {
        p->effect->dispatcher(p->effect, effEditIdle, 0, 0, nullptr, 0);
      }
      if (msg.message == WM_CLOSE && msg.hwnd == hwnd) {
        p->editorShouldClose = true;
        break;
      }
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
    if (!p->editorShouldClose.load()) {
      MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
    }
  }

  KillTimer(hwnd, 1);
  p->effect->dispatcher(p->effect, effEditClose, 0, 0, nullptr, 0);
  DestroyWindow(hwnd);
  p->editorHwnd = nullptr;
  p->editorOpen = false;
  p->editorShouldClose = false;
}

Napi::Value LoadPlugin(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "Expected DLL path string").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::string dllPath = info[0].As<Napi::String>().Utf8Value();
  HMODULE hModule = LoadLibraryA(dllPath.c_str());
  if (!hModule) {
    Napi::Error::New(env, "Failed to load DLL: " + dllPath).ThrowAsJavaScriptException();
    return env.Null();
  }

  VstPluginMain mainFunc = (VstPluginMain)GetProcAddress(hModule, "VSTPluginMain");
  if (!mainFunc) mainFunc = (VstPluginMain)GetProcAddress(hModule, "main");
  if (!mainFunc) {
    FreeLibrary(hModule);
    Napi::Error::New(env, "Not a valid VST2 plugin (no entry point)").ThrowAsJavaScriptException();
    return env.Null();
  }

  AEffect* effect = mainFunc(hostCallback);
  if (!effect || effect->magic != kEffectMagic) {
    FreeLibrary(hModule);
    Napi::Error::New(env, "Plugin returned invalid AEffect").ThrowAsJavaScriptException();
    return env.Null();
  }

  effect->dispatcher(effect, effOpen, 0, 0, nullptr, 0);
  effect->dispatcher(effect, effSetSampleRate, 0, 0, nullptr, 44100.0f);
  effect->dispatcher(effect, effSetBlockSize, 0, 512, nullptr, 0);
  effect->dispatcher(effect, effMainsChanged, 0, 1, nullptr, 0);
  effect->dispatcher(effect, effStartProcess, 0, 0, nullptr, 0);

  int id = nextPluginId++;
  PluginInstance& p = plugins[id];
  p.hModule = hModule;
  p.effect = effect;
  p.path = dllPath;
  p.name = getEffectString(effect, effGetEffectName);
  if (p.name.empty()) p.name = getEffectString(effect, effGetProductString);
  if (p.name.empty()) {
    size_t pos = dllPath.find_last_of("\\/");
    p.name = (pos != std::string::npos) ? dllPath.substr(pos + 1) : dllPath;
  }
  p.vendor = getEffectString(effect, effGetVendorString);
  p.active = true;
  p.hasEditor = (effect->flags & effFlagsHasEditor) != 0;
  p.blockSize = 512;
  p.inputL.resize(512, 0.0f);
  p.inputR.resize(512, 0.0f);
  p.outputL.resize(512, 0.0f);
  p.outputR.resize(512, 0.0f);

  Napi::Object result = Napi::Object::New(env);
  result.Set("id", id);
  result.Set("name", Napi::String::New(env, p.name));
  result.Set("vendor", Napi::String::New(env, p.vendor));
  result.Set("numParams", effect->numParams);
  result.Set("numInputs", effect->numInputs);
  result.Set("numOutputs", effect->numOutputs);
  result.Set("hasEditor", p.hasEditor);
  return result;
}

Napi::Value UnloadPlugin(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int id = info[0].As<Napi::Number>().Int32Value();
  auto it = plugins.find(id);
  if (it == plugins.end()) return env.Undefined();

  PluginInstance& p = it->second;
  if (p.editorOpen.load()) {
    p.editorShouldClose = true;
    if (p.editorThread.joinable()) p.editorThread.join();
  }
  if (p.effect) {
    p.effect->dispatcher(p.effect, effStopProcess, 0, 0, nullptr, 0);
    p.effect->dispatcher(p.effect, effMainsChanged, 0, 0, nullptr, 0);
    p.effect->dispatcher(p.effect, effClose, 0, 0, nullptr, 0);
  }
  if (p.hModule) FreeLibrary(p.hModule);
  plugins.erase(it);
  return env.Undefined();
}

Napi::Value OpenEditor(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int id = info[0].As<Napi::Number>().Int32Value();
  auto it = plugins.find(id);
  if (it == plugins.end()) {
    Napi::Error::New(env, "Plugin not found").ThrowAsJavaScriptException();
    return env.Null();
  }

  PluginInstance& p = it->second;
  if (!p.hasEditor) {
    Napi::Error::New(env, "Plugin has no editor GUI").ThrowAsJavaScriptException();
    return env.Null();
  }
  if (p.editorOpen.load()) {
    // Already open, bring to front
    if (p.editorHwnd) {
      SetForegroundWindow(p.editorHwnd);
      ShowWindow(p.editorHwnd, SW_RESTORE);
    }
    return Napi::Boolean::New(env, true);
  }

  p.editorOpen = true;
  p.editorShouldClose = false;
  if (p.editorThread.joinable()) p.editorThread.join();
  p.editorThread = std::thread(editorThreadFunc, &p, id);
  p.editorThread.detach();

  return Napi::Boolean::New(env, true);
}

Napi::Value CloseEditor(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int id = info[0].As<Napi::Number>().Int32Value();
  auto it = plugins.find(id);
  if (it == plugins.end()) return env.Undefined();
  PluginInstance& p = it->second;
  if (p.editorOpen.load()) {
    p.editorShouldClose = true;
  }
  return env.Undefined();
}

Napi::Value ProcessBlock(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int id = info[0].As<Napi::Number>().Int32Value();
  auto it = plugins.find(id);
  if (it == plugins.end()) {
    Napi::Error::New(env, "Plugin not found").ThrowAsJavaScriptException();
    return env.Null();
  }

  PluginInstance& p = it->second;
  if (!p.effect || !p.active || !p.enabled) {
    Napi::Object out = Napi::Object::New(env);
    out.Set("left", info[1]);
    out.Set("right", info[2]);
    return out;
  }

  Napi::Float32Array leftIn = info[1].As<Napi::Float32Array>();
  Napi::Float32Array rightIn = info[2].As<Napi::Float32Array>();
  size_t frames = leftIn.ElementLength();

  if ((int)frames != p.blockSize) {
    p.blockSize = (int)frames;
    p.effect->dispatcher(p.effect, effMainsChanged, 0, 0, nullptr, 0);
    p.effect->dispatcher(p.effect, effStopProcess, 0, 0, nullptr, 0);
    p.effect->dispatcher(p.effect, effSetBlockSize, 0, (VstIntPtr)frames, nullptr, 0);
    p.effect->dispatcher(p.effect, effMainsChanged, 0, 1, nullptr, 0);
    p.effect->dispatcher(p.effect, effStartProcess, 0, 0, nullptr, 0);
    p.inputL.resize(frames);  p.inputR.resize(frames);
    p.outputL.resize(frames); p.outputR.resize(frames);
  }

  std::memcpy(p.inputL.data(), leftIn.Data(), frames * sizeof(float));
  std::memcpy(p.inputR.data(), rightIn.Data(), frames * sizeof(float));
  std::memset(p.outputL.data(), 0, frames * sizeof(float));
  std::memset(p.outputR.data(), 0, frames * sizeof(float));

  float* inputs[2] = { p.inputL.data(), p.inputR.data() };
  float* outputs[2] = { p.outputL.data(), p.outputR.data() };

  if (p.effect->numInputs >= 2 && p.effect->numOutputs >= 2) {
    if (p.effect->flags & effFlagsCanReplacing)
      p.effect->processReplacing(p.effect, inputs, outputs, (VstInt32)frames);
  } else if (p.effect->numInputs == 1 && p.effect->numOutputs == 1) {
    float* monoIn[1] = { p.inputL.data() };
    float* monoOut[1] = { p.outputL.data() };
    if (p.effect->flags & effFlagsCanReplacing)
      p.effect->processReplacing(p.effect, monoIn, monoOut, (VstInt32)frames);
    std::memcpy(p.outputR.data(), p.outputL.data(), frames * sizeof(float));
  } else {
    std::memcpy(p.outputL.data(), p.inputL.data(), frames * sizeof(float));
    std::memcpy(p.outputR.data(), p.inputR.data(), frames * sizeof(float));
  }

  Napi::Float32Array leftOut = Napi::Float32Array::New(env, frames);
  Napi::Float32Array rightOut = Napi::Float32Array::New(env, frames);
  std::memcpy(leftOut.Data(), p.outputL.data(), frames * sizeof(float));
  std::memcpy(rightOut.Data(), p.outputR.data(), frames * sizeof(float));

  Napi::Object out = Napi::Object::New(env);
  out.Set("left", leftOut);
  out.Set("right", rightOut);
  return out;
}

// processAll(leftArr, rightArr) - chains ALL enabled plugins in order
Napi::Value ProcessAll(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::Float32Array leftIn = info[0].As<Napi::Float32Array>();
  Napi::Float32Array rightIn = info[1].As<Napi::Float32Array>();
  size_t frames = leftIn.ElementLength();

  // Collect enabled plugins in insertion order
  std::vector<PluginInstance*> chain;
  for (auto& [id, p] : plugins) {
    if (p.effect && p.active && p.enabled) chain.push_back(&p);
  }

  if (chain.empty()) {
    Napi::Object out = Napi::Object::New(env);
    out.Set("left", info[0]);
    out.Set("right", info[1]);
    return out;
  }

  // Working buffers
  std::vector<float> bufL(frames), bufR(frames);
  std::memcpy(bufL.data(), leftIn.Data(), frames * sizeof(float));
  std::memcpy(bufR.data(), rightIn.Data(), frames * sizeof(float));

  for (PluginInstance* p : chain) {
    if ((int)frames != p->blockSize) {
      p->blockSize = (int)frames;
      p->effect->dispatcher(p->effect, effMainsChanged, 0, 0, nullptr, 0);
      p->effect->dispatcher(p->effect, effStopProcess, 0, 0, nullptr, 0);
      p->effect->dispatcher(p->effect, effSetBlockSize, 0, (VstIntPtr)frames, nullptr, 0);
      p->effect->dispatcher(p->effect, effMainsChanged, 0, 1, nullptr, 0);
      p->effect->dispatcher(p->effect, effStartProcess, 0, 0, nullptr, 0);
      p->inputL.resize(frames);  p->inputR.resize(frames);
      p->outputL.resize(frames); p->outputR.resize(frames);
    }

    std::memcpy(p->inputL.data(), bufL.data(), frames * sizeof(float));
    std::memcpy(p->inputR.data(), bufR.data(), frames * sizeof(float));
    std::memset(p->outputL.data(), 0, frames * sizeof(float));
    std::memset(p->outputR.data(), 0, frames * sizeof(float));

    float* ins[2] = { p->inputL.data(), p->inputR.data() };
    float* outs[2] = { p->outputL.data(), p->outputR.data() };

    if (p->effect->numInputs >= 2 && p->effect->numOutputs >= 2) {
      if (p->effect->flags & effFlagsCanReplacing)
        p->effect->processReplacing(p->effect, ins, outs, (VstInt32)frames);
    } else if (p->effect->numInputs == 1 && p->effect->numOutputs == 1) {
      float* mi[1] = { p->inputL.data() };
      float* mo[1] = { p->outputL.data() };
      if (p->effect->flags & effFlagsCanReplacing)
        p->effect->processReplacing(p->effect, mi, mo, (VstInt32)frames);
      std::memcpy(p->outputR.data(), p->outputL.data(), frames * sizeof(float));
    } else {
      std::memcpy(p->outputL.data(), p->inputL.data(), frames * sizeof(float));
      std::memcpy(p->outputR.data(), p->inputR.data(), frames * sizeof(float));
    }

    std::memcpy(bufL.data(), p->outputL.data(), frames * sizeof(float));
    std::memcpy(bufR.data(), p->outputR.data(), frames * sizeof(float));
  }

  Napi::Float32Array leftOut = Napi::Float32Array::New(env, frames);
  Napi::Float32Array rightOut = Napi::Float32Array::New(env, frames);
  std::memcpy(leftOut.Data(), bufL.data(), frames * sizeof(float));
  std::memcpy(rightOut.Data(), bufR.data(), frames * sizeof(float));

  Napi::Object out = Napi::Object::New(env);
  out.Set("left", leftOut);
  out.Set("right", rightOut);
  return out;
}

Napi::Value GetPluginInfo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int id = info[0].As<Napi::Number>().Int32Value();
  auto it = plugins.find(id);
  if (it == plugins.end()) return env.Null();

  PluginInstance& p = it->second;
  Napi::Object result = Napi::Object::New(env);
  result.Set("name", Napi::String::New(env, p.name));
  result.Set("vendor", Napi::String::New(env, p.vendor));
  result.Set("numParams", p.effect->numParams);
  result.Set("enabled", p.enabled);
  result.Set("hasEditor", p.hasEditor);
  result.Set("editorOpen", p.editorOpen.load());

  Napi::Array params = Napi::Array::New(env, p.effect->numParams);
  for (int i = 0; i < p.effect->numParams && i < 128; i++) {
    char nameBuf[256] = {0}, labelBuf[256] = {0}, dispBuf[256] = {0};
    p.effect->dispatcher(p.effect, effGetParamName, i, 0, nameBuf, 0);
    p.effect->dispatcher(p.effect, effGetParamLabel, i, 0, labelBuf, 0);
    p.effect->dispatcher(p.effect, effGetParamDisplay, i, 0, dispBuf, 0);
    float val = p.effect->getParameter(p.effect, i);

    Napi::Object param = Napi::Object::New(env);
    param.Set("index", i);
    param.Set("name", Napi::String::New(env, nameBuf));
    param.Set("label", Napi::String::New(env, labelBuf));
    param.Set("display", Napi::String::New(env, dispBuf));
    param.Set("value", val);
    params.Set(i, param);
  }
  result.Set("params", params);
  return result;
}

Napi::Value SetPluginParam(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int id = info[0].As<Napi::Number>().Int32Value();
  int paramIdx = info[1].As<Napi::Number>().Int32Value();
  float value = info[2].As<Napi::Number>().FloatValue();
  auto it = plugins.find(id);
  if (it != plugins.end() && it->second.effect)
    it->second.effect->setParameter(it->second.effect, paramIdx, value);
  return env.Undefined();
}

Napi::Value SetPluginEnabled(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  int id = info[0].As<Napi::Number>().Int32Value();
  bool enabled = info[1].As<Napi::Boolean>().Value();
  auto it = plugins.find(id);
  if (it != plugins.end()) it->second.enabled = enabled;
  return env.Undefined();
}

Napi::Value ListPlugins(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::Array arr = Napi::Array::New(env, plugins.size());
  int i = 0;
  for (auto& [id, p] : plugins) {
    Napi::Object obj = Napi::Object::New(env);
    obj.Set("id", id);
    obj.Set("name", Napi::String::New(env, p.name));
    obj.Set("vendor", Napi::String::New(env, p.vendor));
    obj.Set("enabled", p.enabled);
    obj.Set("path", Napi::String::New(env, p.path));
    obj.Set("hasEditor", p.hasEditor);
    obj.Set("editorOpen", p.editorOpen.load());
    arr.Set(i++, obj);
  }
  return arr;
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("loadPlugin", Napi::Function::New(env, LoadPlugin));
  exports.Set("unloadPlugin", Napi::Function::New(env, UnloadPlugin));
  exports.Set("openEditor", Napi::Function::New(env, OpenEditor));
  exports.Set("closeEditor", Napi::Function::New(env, CloseEditor));
  exports.Set("processBlock", Napi::Function::New(env, ProcessBlock));
  exports.Set("processAll", Napi::Function::New(env, ProcessAll));
  exports.Set("getPluginInfo", Napi::Function::New(env, GetPluginInfo));
  exports.Set("setPluginParam", Napi::Function::New(env, SetPluginParam));
  exports.Set("setPluginEnabled", Napi::Function::New(env, SetPluginEnabled));
  exports.Set("listPlugins", Napi::Function::New(env, ListPlugins));
  return exports;
}

NODE_API_MODULE(vst2_host, Init)
