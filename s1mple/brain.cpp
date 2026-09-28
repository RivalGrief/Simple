// =====================================================================
//  S1mple (Саша) — офлайн голосовой помощник для Windows
//  brain.cpp — разбор команд и генерация ответов ассистента.
// =====================================================================
#include "brain.h"

#include <windows.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <ctime>
#include <random>
#include <algorithm>
#include <vector>

#pragma comment(lib, "ole32.lib")

// ---------------- нормализация ----------------

std::wstring AssistantBrain::Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    // заменяем ё->е для надёжного поиска
    std::replace(s.begin(), s.end(), L'ё', L'е');
    return s;
}

bool AssistantBrain::Contains(const std::wstring& hay, const std::wstring& needle) {
    return Lower(hay).find(Lower(needle)) != std::wstring::npos;
}

bool AssistantBrain::StartsWithAny(const std::wstring& s, const std::vector<std::wstring>& prefixes) {
    std::wstring l = Lower(s);
    for (auto& p : prefixes) {
        std::wstring lp = Lower(p);
        if (l.size() >= lp.size() && l.compare(0, lp.size(), lp) == 0) return true;
    }
    return false;
}

std::wstring AssistantBrain::AfterPrefix(const std::wstring& s, const std::vector<std::wstring>& prefixes) {
    std::wstring l = Lower(s);
    for (auto& p : prefixes) {
        std::wstring lp = Lower(p);
        if (l.size() >= lp.size() && l.compare(0, lp.size(), lp) == 0) {
            std::wstring rest = s.substr(lp.size());
            size_t a = rest.find_first_not_of(L" \t,.?!");
            return a == std::wstring::npos ? L"" : rest.substr(a);
        }
    }
    return s;
}

void AssistantBrain::SetCallbacks(SpeakFn speak, ListenFn listen) {
    speak_ = speak; listen_ = listen;
}

// ---------------- системные действия ----------------

void AssistantBrain::LaunchApp(const std::wstring& exe, const std::wstring& args) {
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = exe + (args.empty() ? L"" : L" " + args);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                   0, nullptr, nullptr, &si, &pi);
    if (pi.hProcess) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
}

static IAudioEndpointVolume* g_endpointVol = nullptr;

static bool EnsureEndpointVolume() {
    if (g_endpointVol) return true;
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en))) return false;
    IMMDevice* dev = nullptr;
    HRESULT hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    en->Release();
    if (FAILED(hr)) return false;
    hr = dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&g_endpointVol);
    dev->Release();
    return SUCCEEDED(hr) && g_endpointVol;
}

int AssistantBrain::GetMasterVolume() {
    if (!EnsureEndpointVolume()) return -1;
    float f = 0;
    if (FAILED(g_endpointVol->GetMasterVolumeLevelScalar(&f))) return -1;
    return (int)(f * 100.0f + 0.5f);
}

void AssistantBrain::SetMasterVolume(int deltaOrAbs, bool absolute) {
    if (!EnsureEndpointVolume()) return;
    float cur = 0;
    g_endpointVol->GetMasterVolumeLevelScalar(&cur);
    float target = absolute ? (float)deltaOrAbs / 100.0f : cur + (float)deltaOrAbs / 100.0f;
    target = std::max(0.0f, std::min(1.0f, target));
    g_endpointVol->SetMasterVolumeLevelScalar(target, nullptr);
}

void AssistantBrain::LockWorkstationNow() {
    LockWorkStation();
}

// ---------------- обработка команд ----------------

CommandResult AssistantBrain::Process(const std::wstring& rawText) {
    CommandResult r;
    std::wstring text = rawText;
    std::wstring low = Lower(text);

    // 1. Отрезаем wake word: «саша, ...» / «аша ...» и т.п.
    std::vector<std::wstring> wakes = { wake_, L"шаша", L"саня", L"саш", L"ача", L"аша" };
    bool hadWake = false;
    for (auto& w : wakes) {
        if (low.find(w) != std::wstring::npos) { hadWake = true; break; }
    }
    if (hadWake) {
        // убираем первое вхождение wake-слова
        for (auto& w : wakes) {
            size_t p = low.find(w);
            if (p != std::wstring::npos) {
                text = text.substr(0, p) + text.substr(p + w.size());
                break;
            }
        }
        low = Lower(text);
        // trim punctuation/spaces
        size_t a = low.find_first_not_of(L" \t,.?!");
        if (a != std::wstring::npos) { text = text.substr(a); low = Lower(text); }
        else { text = L""; low = L""; }
    }

    if (text.empty()) {
        r.speech = L"Я слушаю.";
        r.startListen = true;
        lastAnswer_ = r.speech;
        r.logLine = L"💬 " + r.speech;
        return r;
    }

    // 2. Команды управления самим ассистентом
    if (low == L"стоп" || low == L"хватит" || low == L"молчи") {
        r.stopSpeaking = true;
        r.logLine = L"⏹ Остановил речь.";
        return r;
    }
    if (low == L"повтори") {
        r.speech = lastAnswer_.empty() ? L"Ранее я ничего не говорил." : lastAnswer_;
        r.logLine = L"🔁 " + r.speech;
        return r;
    }
    if (low == L"пока" || low == L"до свидания" || low == L"выход" || low == L"закрылись") {
        r.speech = L"Пока! Обращайся, " + userName_ + L".";
        r.exitApp = true;
        r.logLine = L"👋 " + r.speech;
        return r;
    }

    // 3. Профильные обработчики
    static const struct { CommandResult (AssistantBrain::*fn)(const std::wstring&); } handlers[] = {
        &AssistantBrain::HandleSmallTalk,
        &AssistantBrain::HandleDateTime,
        &AssistantBrain::HandleApps,
        &AssistantBrain::HandleSystem,
        &AssistantBrain::HandleVolume,
        &AssistantBrain::HandleNotes,
        &AssistantBrain::HandleSearch,
        &AssistantBrain::HandleJokes,
    };
    for (auto& h : handlers) {
        CommandResult sub = (this->*h.fn)(low);
        if (!sub.speech.empty() || sub.stopSpeaking) {
            r = sub;
            goto finalize;
        }
    }

    // 4. Неизвестная команда
    r.speech = L"Команду «" + text + L"» я пока не знаю. Скажи «помощь», чтобы увидеть список.";
    r.logLine = L"❓ " + text;

finalize:
    if (!r.speech.empty()) lastAnswer_ = r.speech;
    if (r.logLine.empty() && !r.speech.empty()) r.logLine = L"💬 " + r.speech;
    return r;
}

// ---------------- small talk ----------------

CommandResult AssistantBrain::HandleSmallTalk(const std::wstring& t) {
    CommandResult r;
    auto pick = [](const std::vector<std::wstring>& v) {
        static std::mt19937 rng((unsigned)time(nullptr));
        return v[rng() % v.size()];
    };

    if (t == L"привет" || t == L"здравствуй" || t == L"ты тут" || StartsWithAny(t, { L"привет" })) {
        r.speech = pick({ L"Привет, " + userName_ + L"! Чем помочь?",
                          L"Здравствуй! Я на связи.",
                          L"Привет-привет. Слушаю тебя." });
        return r;
    }
    if (Contains(t, L"доброе утро")) { r.speech = L"Доброе утро! Как спалось?"; return r; }
    if (Contains(t, L"добрый день")) { r.speech = L"Добрый день! Готов к работе."; return r; }
    if (Contains(t, L"добрый вечер")) { r.speech = L"Добрый вечер! Отдыхаешь или работаешь?"; return r; }

    if (Contains(t, L"как дела") || Contains(t, L"как ты")) {
        r.speech = pick({ L"Отлично! Все нейроны в строю.",
                          L"Работаю потихоньку, спасибо что спросил.",
                          L"Как по маслу. А у тебя?" });
        return r;
    }
    if (Contains(t, L"спасибо")) {
        r.speech = pick({ L"Пожалуйста!", L"Обращайся.", L"Всегда рад помочь." });
        return r;
    }
    if (Contains(t, L"кто ты") || Contains(t, L"как тебя зовут") || Contains(t, L"что ты такое")) {
        r.speech = L"Я Саша — локальный голосовой помощник S1mple. Работаю без интернета, команды обрабатываю прямо на твоём компьютере.";
        return r;
    }
    if (Contains(t, L"что ты умеешь") || t == L"помощь") {
        r.speech = L"Я умею: говорить время и дату, открывать блокнот, калькулятор, Paint и проводник, "
                   L"менять громкость, искать в интернете, вести заметки, блокировать и выключать компьютер, "
                   L"рассказывать анекдоты. Просто назови команду вслух или напиши её.";
        r.logLine = L"ℹ️ Список команд продублирован в окне программы.";
        return r;
    }
    if (t == L"да" || t == L"нет") {
        r.speech = (t == L"да") ? L"Принято." : L"Хорошо, отменил.";
        return r;
    }
    return r;
}

// ---------------- время/дата ----------------

CommandResult AssistantBrain::HandleDateTime(const std::wstring& t) {
    CommandResult r;
    bool wantTime = Contains(t, L"который час") || Contains(t, L"сколько времени");
    bool wantDate = Contains(t, L"какое сегодня число") || Contains(t, L"какая дата") || Contains(t, L"число сегодня");
    if (!wantTime && !wantDate) return r;

    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t buf[128];

    static const wchar_t* months[] = {
        L"января", L"февраля", L"марта", L"апреля", L"мая", L"июня",
        L"июля", L"августа", L"сентября", L"октября", L"ноября", L"декабря" };
    static const wchar_t* days[] = {
        L"воскресенье", L"понедельник", L"вторник", L"среда", L"четверг", L"пятница", L"суббота" };

    if (wantTime) {
        swprintf(buf, 128, L"Сейчас %02d:%02d.", st.wHour, st.wMinute);
        r.speech = buf;
    }
    if (wantDate) {
        swprintf(buf, 128, L"Сегодня %u %s, %s.", st.wDay, months[st.wMonth - 1], days[st.wDayOfWeek]);
        if (!r.speech.empty()) r.speech += L" ";
        r.speech += buf;
    }
    return r;
}

// ---------------- приложения ----------------

CommandResult AssistantBrain::HandleApps(const std::wstring& t) {
    CommandResult r;
    bool open = StartsWithAny(t, { L"запусти", L"открой" });
    if (!open) return r;

    if (Contains(t, L"блокнот")) { LaunchApp(L"notepad.exe"); r.speech = L"Открываю Блокнот."; }
    else if (Contains(t, L"калькулятор")) { LaunchApp(L"calc.exe"); r.speech = L"Запускаю Калькулятор."; }
    else if (Contains(t, L"пейнт") || Contains(t, L"paint")) { LaunchApp(L"mspaint.exe"); r.speech = L"Открываю Paint."; }
    else if (Contains(t, L"проводник") || Contains(t, L"папку")) { LaunchApp(L"explorer.exe"); r.speech = L"Открываю Проводник."; }
    else if (Contains(t, L"браузер")) { LaunchApp(L"cmd.exe", L"/c start ""https://www.google.com"""); r.speech = L"Открываю браузер."; }
    else if (Contains(t, L"терминал") || Contains(t, L"командную строку")) { LaunchApp(L"cmd.exe"); r.speech = L"Открываю командную строку."; }
    else if (Contains(t, L"диспетчер")) { LaunchApp(L"taskmgr.exe"); r.speech = L"Открываю Диспетчер задач."; }
    else {
        std::wstring what = AfterPrefix(t, { L"запусти", L"открой" });
        r.speech = L"Не знаю приложение «" + what + L"». Добавлю его в следующий раз.";
    }
    return r;
}

// ---------------- система ----------------

CommandResult AssistantBrain::HandleSystem(const std::wstring& t) {
    CommandResult r;
    if (Contains(t, L"заблокируй экран") || Contains(t, L"заблокировать экран")) {
        LockWorkstationNow();
        r.speech = L"Экран заблокирован.";
        return r;
    }
    if (Contains(t, L"выключи компьютер") || Contains(t, L"выключить компьютер") || Contains(t, L"shutdown")) {
        r.speech = L"Выключаю компьютер через 10 секунд. Чтобы отменить, введи в командной строке: shutdown /a";
        LaunchApp(L"shutdown.exe", L"/s /t 10");
        return r;
    }
    if (Contains(t, L"перезагрузи компьютер") || Contains(t, L"ребут")) {
        r.speech = L"Перезагружаю компьютер через 10 секунд.";
        LaunchApp(L"shutdown.exe", L"/r /t 10");
        return r;
    }
    return r;
}

// ---------------- громкость ----------------

CommandResult AssistantBrain::HandleVolume(const std::wstring& t) {
    CommandResult r;
    if (Contains(t, L"увеличь громкость") || Contains(t, L"громче")) {
        SetMasterVolume(+10, false);
        int v = GetMasterVolume();
        wchar_t buf[64]; swprintf(buf, 64, L"Громкость %d процентов.", v);
        r.speech = buf;
        return r;
    }
    if (Contains(t, L"уменьши громкость") || Contains(t, L"тише")) {
        SetMasterVolume(-10, false);
        int v = GetMasterVolume();
        wchar_t buf[64]; swprintf(buf, 64, L"Громкость %d процентов.", v);
        r.speech = buf;
        return r;
    }
    if (Contains(t, L"выключи звук") || Contains(t, L"без звука")) {
        SetMasterVolume(0, true);
        r.speech = L"Звук выключен.";
        return r;
    }
    if (Contains(t, L"включи звук")) {
        SetMasterVolume(50, true);
        r.speech = L"Звук включён на половину.";
        return r;
    }
    if (t == L"громкость") {
        int v = GetMasterVolume();
        wchar_t buf[64]; swprintf(buf, 64, L"Текущая громкость %d процентов.", v);
        r.speech = buf;
        return r;
    }
    return r;
}

// ---------------- заметки ----------------

CommandResult AssistantBrain::HandleNotes(const std::wstring& t) {
    CommandResult r;
    if (StartsWithAny(t, { L"запиши заметку", L"создай заметку", L"добавь заметку" })) {
        std::wstring body = AfterPrefix(t, { L"запиши заметку", L"создай заметку", L"добавь заметку" });
        if (body.empty()) {
            r.speech = L"Что записать? Скажи текст после слова «заметка».";
            r.startListen = true;
        } else {
            notes_.push_back(body);
            r.speech = L"Записал. Заметок: " + std::to_wstring(notes_.size());
        }
        return r;
    }
    if (Contains(t, L"прочитай заметки") || Contains(t, L"покажи заметки") || Contains(t, L"мои заметки")) {
        if (notes_.empty()) { r.speech = L"Заметок пока нет."; return r; }
        std::wstring all;
        for (size_t i = 0; i < notes_.size(); ++i) {
            all += std::to_wstring(i + 1) + L". " + notes_[i];
            if (i + 1 < notes_.size()) all += L". ";
        }
        r.speech = all;
        return r;
    }
    if (Contains(t, L"удали все заметки")) {
        notes_.clear();
        r.speech = L"Все заметки удалены.";
        return r;
    }
    return r;
}

// ---------------- поиск ----------------

CommandResult AssistantBrain::HandleSearch(const std::wstring& t) {
    CommandResult r;
    std::wstring q;
    if (StartsWithAny(t, { L"найди в интернете", L"погугли" }))
        q = AfterPrefix(t, { L"найди в интернете", L"погугли" });
    else if (Contains(t, L"найди в интернете"))
        q = AfterPrefix(t, { L"найди в интернете" });
    if (q.empty()) return r;

    std::string u8q;
    int need = WideCharToMultiByte(CP_UTF8, 0, q.c_str(), (int)q.size(), nullptr, 0, nullptr, nullptr);
    u8q.resize(need);
    WideCharToMultiByte(CP_UTF8, 0, q.c_str(), (int)q.size(), &u8q[0], need, nullptr, nullptr);
    // url encode пробелы
    for (size_t p = u8q.find(' '); p != std::string::npos; p = u8q.find(' ', p)) u8q.replace(p, 1, "%20");

    std::wstring url = L"https://www.google.com/search?q=";
    for (char c : u8q) url += (wchar_t)(unsigned char)c;
    LaunchApp(L"rundll32.exe", L"url.dll,OpenURL \"" + url + L"\"");
    r.speech = L"Ищу в интернете: " + q;
    return r;
}

// ---------------- анекдоты ----------------

CommandResult AssistantBrain::HandleJokes(const std::wstring& t) {
    CommandResult r;
    if (!(Contains(t, L"анекдот") || Contains(t, L"рассмеши") || Contains(t, L"шутк"))) return r;
    static const std::vector<std::wstring> jokes = {
        L"Программист заходит в лифт, а там кнопки 0 и 1. Заходит второй — ищет децимальную.",
        L"— Саша, ты робот? — Ну, если не считать того, что меня написали на C++.",
        L"Заходит как-то баг в продакшен... и остаётся там жить.",
        L"Жена программиста говорит: сходи в магазин, купи батон хлеба. Если будут яйца — возьми десяток. Программист вернулся с десятью батонами: яйца были.",
        L"Оптимист: стакан наполовину полон. Пессимист: наполовину пуст. Инженер: стакан вдвое больше, чем нужно."
    };
    static std::mt19937 rng((unsigned)time(nullptr) ^ 0x5A5A);
    r.speech = jokes[rng() % jokes.size()];
    return r;
}
