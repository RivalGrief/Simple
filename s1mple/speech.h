// =====================================================================
//  S1mple (Саша) — офлайн голосовой помощник для Windows
//  speech.h — native-обёртка над S1mpleBridge.dll через CLR Hosting.
//
//  Контракт с мостом (см. bridge/Bridge.cs):
//    Entry(requestJson)  — выполняет запрос, пишет JSON-ответ в
//                          %TEMP%\S1mpleBridge\resp.json; возвращает 0.
//    FetchLast("")       — возвращает длину ответа в символах.
//  Native после Entry читает resp.json (UTF-8 без BOM). Вызовы
//  сериализуются критической секцией.
// =====================================================================
#pragma once

#include <windows.h>
#include <metahost.h>
#include <string>
#include <vector>

class SpeechEngine {
public:
    struct RecognizerInfo { std::wstring id, name, culture; };
    struct VoiceInfo      { std::wstring name, culture; };

    ~SpeechEngine();

    HRESULT Init(const std::wstring& bridgeAssemblyPath, std::wstring& errorOut);
    void    Shutdown();
    bool    Available() const { return available_; }

    // ---- распознавание ----
    bool GetRecognizers(std::vector<RecognizerInfo>& out, std::wstring& err);
    bool CreateRecognizer(const std::wstring& recId, std::wstring& handleOut, std::wstring& err);
    void DestroyRecognizer(const std::wstring& handle);
    // Блокирующий! Вызывать только из рабочего потока прослушивания.
    bool WaitForPhrase(const std::wstring& handle, int timeoutMs, std::wstring& textOut);
    void CancelListening(const std::wstring& handle);

    // ---- синтез ----
    bool Speak(const std::wstring& text);           // асинхронно
    bool IsSpeaking();
    void StopSpeak();
    bool GetVoices(std::vector<VoiceInfo>& out, std::wstring& err);
    bool SetVoice(const std::wstring& name);        // "" = голос по умолчанию
    bool SetRate(int rate);                         // -10 .. +10

private:
    SpeechEngine() = default;
    SpeechEngine(const SpeechEngine&) = delete;
    SpeechEngine& operator=(const SpeechEngine&) = delete;

    // Универсальный вызов метода моста. Возвращает JSON-ответ (L"" при сбое связи).
    std::wstring Call(const wchar_t* method, VARIANT* args, UINT argc);
    static std::wstring MakeReq(const wchar_t* method, VARIANT* args, UINT argc);
    std::wstring ReadSharedResponse();
    HRESULT ExecuteBridge(LPCWSTR method, const std::wstring& arg, DWORD& ret);

    ICLRMetaHost*     metaHost_    = nullptr;
    ICLRRuntimeInfo*  rtInfo_      = nullptr;
    ICLRRuntimeHost*  runtimeHost_ = nullptr;
    std::wstring      assemblyPath_;
    CRITICAL_SECTION  callLock_;
    bool              lockInited_  = false;
    bool              available_   = false;
};

// ---------------- мини-JSON парсер (реализация в speech.cpp) ----------------
bool JsonGetFieldStr(const std::wstring& obj, const wchar_t* key, std::wstring& out);
bool JsonGetFieldRaw(const std::wstring& obj, const wchar_t* key, std::wstring& out);
bool JsonExtractArray(const std::wstring& json, std::vector<std::wstring>& items);
