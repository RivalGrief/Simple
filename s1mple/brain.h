// =====================================================================
//  S1mple (Саша) — офлайн голосовой помощник для Windows
//  brain.h — «мозг» ассистента: разбор команд и генерация ответов.
// =====================================================================
#pragma once

#include <string>
#include <vector>
#include <functional>

struct CommandResult {
    std::wstring speech;      // что произнести вслух
    std::wstring logLine;     // что показать в журнале (пусто = speech)
    bool         exitApp       = false;
    bool         startListen   = false;   // продолжить слушать дальше
    bool         stopSpeaking  = false;   // команда «стоп/молчи» — остановить синтез
};

class AssistantBrain {
public:
    using SpeakFn  = std::function<bool(const std::wstring&)>;
    using ListenFn = std::function<void()>;

    void SetCallbacks(SpeakFn speak, ListenFn listen);

    // Обработка распознанной/введённой текстом команды.
    CommandResult Process(const std::wstring& text);

    // Настройки
    void SetWakeWord(const std::wstring& wake) { wake_ = wake; }
    const std::wstring& WakeWord() const { return wake_; }
    void SetUserName(const std::wstring& name) { userName_ = name; }

private:
    std::wstring wake_    = L"саша";
    std::wstring userName_ = L"друг";

    SpeakFn  speak_;
    ListenFn listen_;

    std::wstring lastAnswer_;                 // для команды «повтори»
    std::vector<std::wstring> notes_;         // заметки в памяти
    int noteSessionIndex_ = -1;               // активная сессия записи заметки

    // нормализация
    static std::wstring Lower(std::wstring s);
    static bool Contains(const std::wstring& hay, const std::wstring& needle);
    static bool StartsWithAny(const std::wstring& s, const std::vector<std::wstring>& prefixes);
    static std::wstring AfterPrefix(const std::wstring& s, const std::vector<std::wstring>& prefixes);

    // отдельные обработчики
    CommandResult HandleSmallTalk(const std::wstring& t);
    CommandResult HandleDateTime(const std::wstring& t);
    CommandResult HandleApps(const std::wstring& t);
    CommandResult HandleSystem(const std::wstring& t);
    CommandResult HandleVolume(const std::wstring& t);
    CommandResult HandleNotes(const std::wstring& t);
    CommandResult HandleSearch(const std::wstring& t);
    CommandResult HandleJokes(const std::wstring& t);

    static void LaunchApp(const std::wstring& exe, const std::wstring& args = L"");
    static void SetMasterVolume(int deltaOrAbs, bool absolute);
    static int  GetMasterVolume();
    static void LockWorkstationNow();
};
