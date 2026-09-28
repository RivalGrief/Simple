// =====================================================================
//  S1mple (Саша) — офлайн голосовой помощник для Windows
//  speech.cpp — SpeechEngine: CLR Hosting + ExecuteInDefaultAppDomain.
// =====================================================================
#include "speech.h"
#include <comutil.h>
#include <shlobj.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "mscoree.lib")
#pragma comment(lib, "comsuppw.lib")

// ================= мини-JSON парсер =================

static std::wstring JsonUnescape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (c == L'\\' && i + 1 < s.size()) {
            wchar_t n = s[++i];
            switch (n) {
                case L'"':  out += L'"';  break;
                case L'\\': out += L'\\'; break;
                case L'/':  out += L'/';  break;
                case L'n':  out += L'\n'; break;
                case L'r':  out += L'\r'; break;
                case L't':  out += L'\t'; break;
                case L'u':
                    if (i + 4 < s.size()) {
                        std::wstring hex = s.substr(i + 1, 4);
                        out += (wchar_t)wcstoul(hex.c_str(), nullptr, 16);
                        i += 4;
                    }
                    break;
                default: out += n;
            }
        } else out += c;
    }
    return out;
}

bool JsonGetFieldStr(const std::wstring& obj, const wchar_t* key, std::wstring& out) {
    std::wstring pat = L"\""; pat += key; pat += L"\":\"";
    size_t p = obj.find(pat);
    if (p == std::wstring::npos) return false;
    p += pat.size();
    std::wstring val;
    while (p < obj.size()) {
        wchar_t c = obj[p];
        if (c == L'\\' && p + 1 < obj.size()) { val += c; val += obj[p + 1]; p += 2; continue; }
        if (c == L'"') break;
        val += c; ++p;
    }
    out = JsonUnescape(val);
    return true;
}

bool JsonGetFieldRaw(const std::wstring& obj, const wchar_t* key, std::wstring& out) {
    std::wstring pat = L"\""; pat += key; pat += L"\":";
    size_t p = obj.find(pat);
    if (p == std::wstring::npos) return false;
    p += pat.size();
    while (p < obj.size() && iswspace(obj[p])) ++p;
    if (p >= obj.size()) return false;
    if (obj[p] == L'"') return JsonGetFieldStr(obj, key, out);

    int depth = 0; bool inStr = false;
    size_t start = p;
    for (; p < obj.size(); ++p) {
        wchar_t c = obj[p];
        if (inStr) { if (c == L'\\') ++p; else if (c == L'"') inStr = false; continue; }
        if (c == L'"') { inStr = true; continue; }
        if (c == L'{' || c == L'[') ++depth;
        else if (c == L'}' || c == L']') { if (depth == 0) break; --depth; }
        else if (c == L',' && depth == 0) break;
    }
    out = obj.substr(start, p - start);
    return !out.empty();
}

bool JsonExtractArray(const std::wstring& json, std::vector<std::wstring>& items) {
    items.clear();
    size_t start = json.find(L'[');
    if (start == std::wstring::npos) return false;
    int depth = 0; bool inStr = false; size_t elemStart = 0;
    for (size_t i = start; i < json.size(); ++i) {
        wchar_t c = json[i];
        if (inStr) { if (c == L'\\') ++i; else if (c == L'"') inStr = false; continue; }
        switch (c) {
            case L'"':
                inStr = true;
                if (depth == 1) elemStart = i;
                break;
            case L'{': case L'[':
                if (depth == 1) elemStart = i;
                ++depth;
                break;
            case L'}': case L']':
                --depth;
                if (depth == 1) items.push_back(json.substr(elemStart, i - elemStart + 1));
                if (depth == 0) return true;
                break;
            case L',':
                if (depth == 1) {
                    std::wstring piece = json.substr(elemStart, i - elemStart);
                    size_t a = piece.find_first_not_of(L" \t\r\n");
                    size_t b = piece.find_last_not_of(L" \t\r\n");
                    if (a != std::wstring::npos) items.push_back(piece.substr(a, b - a + 1));
                    elemStart = i + 1;
                }
                break;
        }
    }
    return depth == 0;
}

static std::wstring JsonEscapeReq(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        switch (c) {
            case L'"':  out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n";  break;
            case L'\r': out += L"\\r";  break;
            case L'\t': out += L"\\t";  break;
            default:
                if (c < 0x20) { wchar_t buf[8]; swprintf(buf, 8, L"\\u%04x", (unsigned)c); out += buf; }
                else out += c;
        }
    }
    return out;
}

// ================= SpeechEngine =================

SpeechEngine::~SpeechEngine() { Shutdown(); }

HRESULT SpeechEngine::Init(const std::wstring& bridgeAssemblyPath, std::wstring& errorOut) {
    Shutdown();
    assemblyPath_ = bridgeAssemblyPath;
    if (!lockInited_) { InitializeCriticalSection(&callLock_); lockInited_ = true; }

    HRESULT hr = CLRCreateInstance(CLSID_CLRMetaHost, IID_PPV_ARGS(&metaHost_));
    if (FAILED(hr)) { errorOut = L"Не удалось создать CLR Meta Host. Нужен .NET Framework 4.x."; return hr; }

    hr = metaHost_->GetRuntime(L"v4.0.30319", IID_PPV_ARGS(&rtInfo_));
    if (FAILED(hr)) { errorOut = L".NET Framework 4.x не найден в системе."; return hr; }

    hr = rtInfo_->GetInterface(CLSID_CLRRuntimeHost, IID_PPV_ARGS(&runtimeHost_));
    if (FAILED(hr)) { errorOut = L"ICLRRuntimeHost недоступна."; return hr; }

    hr = runtimeHost_->Start();
    if (FAILED(hr)) { errorOut = L"Не удалось запустить CLR."; return hr; }

    std::wstring resp = Call(L"Ping", nullptr, 0);
    if (resp.empty() || resp.find(L"\"error\"") != std::wstring::npos) {
        errorOut = L"Не удалось загрузить мост S1mpleBridge.dll.\n"
                   L"Соберите его (build.ps1) и положите рядом с S1mple.exe:\n" + assemblyPath_;
        std::wstring e;
        if (!resp.empty() && JsonGetFieldStr(resp, L"error", e)) errorOut += L"\nПричина: " + e;
        return E_FAIL;
    }
    available_ = true;
    return S_OK;
}

void SpeechEngine::Shutdown() {
    available_ = false;
    if (runtimeHost_) { runtimeHost_->Release(); runtimeHost_ = nullptr; }
    if (rtInfo_)      { rtInfo_->Release();      rtInfo_ = nullptr; }
    if (metaHost_)    { metaHost_->Release();    metaHost_ = nullptr; }
    if (lockInited_)  { DeleteCriticalSection(&callLock_); lockInited_ = false; }
}

std::wstring SpeechEngine::MakeReq(const wchar_t* method, VARIANT* args, UINT argc) {
    std::wstring req = L"{\"m\":\"";
    req += method;
    req += L"\",\"a\":[";
    for (UINT i = 0; i < argc; ++i) {
        if (i) req += L",";
        switch (args[i].vt) {
            case VT_BSTR:
                req += L"\""; req += JsonEscapeReq(args[i].bstrVal); req += L"\"";
                break;
            case VT_I4:   req += std::to_wstring(args[i].lVal); break;
            case VT_BOOL: req += (args[i].boolVal == VARIANT_TRUE) ? L"true" : L"false"; break;
            default:      req += L"null";
        }
    }
    req += L"]}";
    return req;
}

HRESULT SpeechEngine::ExecuteBridge(LPCWSTR method, const std::wstring& arg, DWORD& ret) {
    ret = 0;
    BSTR bstr = SysAllocString(arg.c_str());
    HRESULT hr = runtimeHost_->ExecuteInDefaultAppDomain(
        assemblyPath_.c_str(), L"S1mpleBridge.Bridge", method,
        reinterpret_cast<LPVOID>(bstr), &ret);
    SysFreeString(bstr);
    return hr;
}

std::wstring SpeechEngine::ReadSharedResponse() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = tmp;
    path += L"S1mpleBridge\\resp.json";

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    DWORD size = GetFileSize(h, nullptr);
    std::string u8(size ? size : 0, '\0');
    DWORD read = 0;
    BOOL ok = ReadFile(h, &u8[0], size, &read, nullptr);
    CloseHandle(h);
    if (!ok || read == 0) return L"";
    int wlen = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)read, nullptr, 0);
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)read, &w[0], wlen);
    return w;
}

std::wstring SpeechEngine::Call(const wchar_t* method, VARIANT* args, UINT argc) {
    if (!runtimeHost_) return L"";
    std::wstring req = MakeReq(method, args, argc);

    EnterCriticalSection(&callLock_);
    DWORD ret = 0;
    HRESULT hr = ExecuteBridge(L"Entry", req, ret);
    std::wstring resp;
    if (SUCCEEDED(hr)) {
        DWORD len = 0;
        hr = ExecuteBridge(L"FetchLast", L"", len);
        if (SUCCEEDED(hr) && (int)len >= 0) resp = ReadSharedResponse();
    }
    LeaveCriticalSection(&callLock_);
    return resp;
}

// ---------------- распознавание ----------------

bool SpeechEngine::GetRecognizers(std::vector<RecognizerInfo>& out, std::wstring& err) {
    out.clear();
    std::wstring resp = Call(L"GetRecognizers", nullptr, 0);
    std::wstring e;
    if (resp.empty()) { err = L"Мост речи не отвечает"; return false; }
    if (JsonGetFieldStr(resp, L"error", e)) { err = e; return false; }
    std::wstring ok;
    if (!JsonGetFieldRaw(resp, L"ok", ok)) { err = L"Некорректный ответ моста"; return false; }
    std::vector<std::wstring> items;
    if (!JsonExtractArray(ok, items)) return true; // пустой список — не ошибка
    for (auto& it : items) {
        RecognizerInfo ri;
        JsonGetFieldStr(it, L"id", ri.id);
        JsonGetFieldStr(it, L"name", ri.name);
        JsonGetFieldStr(it, L"culture", ri.culture);
        if (!ri.id.empty()) out.push_back(ri);
    }
    return true;
}

bool SpeechEngine::CreateRecognizer(const std::wstring& recId, std::wstring& handleOut, std::wstring& err) {
    VARIANT a{}; a.vt = VT_BSTR; a.bstrVal = SysAllocString(recId.c_str());
    std::wstring resp = Call(L"CreateRecognizer", &a, 1);
    SysFreeString(a.bstrVal);
    if (resp.empty()) { err = L"Мост речи не отвечает"; return false; }
    std::wstring e;
    if (JsonGetFieldStr(resp, L"error", e)) { err = e; return false; }
    if (!JsonGetFieldStr(resp, L"ok", handleOut) || handleOut.empty()) {
        err = L"Не удалось создать распознаватель"; return false;
    }
    return true;
}

void SpeechEngine::DestroyRecognizer(const std::wstring& handle) {
    VARIANT a{}; a.vt = VT_BSTR; a.bstrVal = SysAllocString(handle.c_str());
    Call(L"DestroyRecognizer", &a, 1);
    SysFreeString(a.bstrVal);
}

bool SpeechEngine::WaitForPhrase(const std::wstring& handle, int timeoutMs, std::wstring& textOut) {
    VARIANT args[2] = {};
    args[0].vt = VT_BSTR; args[0].bstrVal = SysAllocString(handle.c_str());
    args[1].vt = VT_I4;   args[1].lVal = timeoutMs;
    std::wstring resp = Call(L"WaitForPhrase", args, 2);
    SysFreeString(args[0].bstrVal);
    if (resp.empty()) return false;
    std::wstring e;
    if (JsonGetFieldStr(resp, L"error", e)) return false;
    return JsonGetFieldStr(resp, L"ok", textOut);
}

void SpeechEngine::CancelListening(const std::wstring& handle) {
    VARIANT a{}; a.vt = VT_BSTR; a.bstrVal = SysAllocString(handle.c_str());
    Call(L"CancelListening", &a, 1);
    SysFreeString(a.bstrVal);
}

// ---------------- синтез ----------------

static bool RespOkBool(const std::wstring& resp) {
    if (resp.empty()) return false;
    std::wstring raw;
    if (!JsonGetFieldRaw(resp, L"ok", raw)) return false;
    return raw == L"true";
}

bool SpeechEngine::Speak(const std::wstring& text) {
    VARIANT a{}; a.vt = VT_BSTR; a.bstrVal = SysAllocString(text.c_str());
    std::wstring resp = Call(L"Speak", &a, 1);
    SysFreeString(a.bstrVal);
    return RespOkBool(resp);
}

bool SpeechEngine::IsSpeaking() {
    std::wstring resp = Call(L"IsSpeaking", nullptr, 0);
    return RespOkBool(resp);
}

void SpeechEngine::StopSpeak() {
    Call(L"StopSpeak", nullptr, 0);
}

bool SpeechEngine::GetVoices(std::vector<VoiceInfo>& out, std::wstring& err) {
    out.clear();
    std::wstring resp = Call(L"GetVoices", nullptr, 0);
    if (resp.empty()) { err = L"Мост речи не отвечает"; return false; }
    std::wstring e;
    if (JsonGetFieldStr(resp, L"error", e)) { err = e; return false; }
    std::wstring ok;
    if (!JsonGetFieldRaw(resp, L"ok", ok)) return true;
    std::vector<std::wstring> items;
    if (!JsonExtractArray(ok, items)) return true;
    for (auto& it : items) {
        VoiceInfo vi;
        JsonGetFieldStr(it, L"name", vi.name);
        JsonGetFieldStr(it, L"culture", vi.culture);
        if (!vi.name.empty()) out.push_back(vi);
    }
    return true;
}

bool SpeechEngine::SetVoice(const std::wstring& name) {
    VARIANT a{}; a.vt = VT_BSTR; a.bstrVal = SysAllocString(name.c_str());
    std::wstring resp = Call(L"SetVoice", &a, 1);
    SysFreeString(a.bstrVal);
    return RespOkBool(resp);
}

bool SpeechEngine::SetRate(int rate) {
    VARIANT a{}; a.vt = VT_I4; a.lVal = rate;
    std::wstring resp = Call(L"SetRate", &a, 1);
    return RespOkBool(resp);
}
