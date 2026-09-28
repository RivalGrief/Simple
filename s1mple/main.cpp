// =====================================================================
//  S1mple (Саша) — офлайн голосовой помощник для Windows
//  main.cpp — Win32 UI + отдельный STA-поток движка речи.
//
//  Потоки:
//    • UI-поток (STA)   — окно, сообщения;
//    • SpeechThread     — CLR Hosting и все вызовы моста (распознавание/синтез).
//  Взаимодействие — очередь задач PostTask() + события WM_APP_* в UI.
// =====================================================================
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <functional>
#include <atomic>

#include "resource.h"
#include "speech.h"
#include "brain.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// ================= глобальное состояние =================

static HINSTANCE g_hInst = nullptr;
static HWND      g_hMain = nullptr;
static HWND      g_hLog = nullptr, g_hInput = nullptr, g_hSend = nullptr;
static HWND      g_hListen = nullptr, g_hStopTts = nullptr, g_hStatus = nullptr;
static HWND      g_hMicCombo = nullptr, g_hVoiceCombo = nullptr;
static HWND      g_hRateSlider = nullptr, g_hRateLabel = nullptr;
static HWND      g_hWakeEdit = nullptr;

static SpeechEngine   g_engine;
static AssistantBrain g_brain;

static std::vector<SpeechEngine::RecognizerInfo> g_recognizers;
static std::vector<SpeechEngine::VoiceInfo>      g_voices;

static std::wstring      g_recHandle;          // активный распознаватель (доступ из SpeechThread)
static std::atomic<bool> g_listening{ false };
static std::atomic<bool> g_shutdown{ false };

static const UINT_PTR TIMER_TTS_POLL = 0xB1;
static const int      LISTEN_TIMEOUT_MS = 15000;

// ================= очередь задач для SpeechThread =================

using Task = std::function<void()>;
static std::deque<Task> g_tasks;
static std::mutex       g_taskMtx;
static HANDLE           g_taskEvent = nullptr;

static void PostTask(Task t) {
    {
        std::lock_guard<std::mutex> lk(g_taskMtx);
        g_tasks.push_back(std::move(t));
    }
    if (g_taskEvent) SetEvent(g_taskEvent);
}

static bool PopTask(Task& out) {
    std::lock_guard<std::mutex> lk(g_taskMtx);
    if (g_tasks.empty()) return false;
    out = std::move(g_tasks.front());
    g_tasks.pop_front();
    return true;
}

// ================= UI helpers =================

static void AppendLog(const std::wstring& line) {
    if (!g_hLog) return;
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t stamp[16];
    swprintf(stamp, 16, L"[%02d:%02d] ", st.wHour, st.wMinute);
    std::wstring text = stamp + line + L"\r\n";
    SendMessageW(g_hLog, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
    SendMessageW(g_hLog, EM_SCROLLCARET, 0, 0);
}

static void SetStatusText(const std::wstring& s) {
    if (g_hStatus) SetWindowTextW(g_hStatus, s.c_str());
}

// Сообщения из SpeechThread в UI
#define WM_APP_READY        (WM_APP + 1)  // wParam=HRESULT, lParam=std::wstring* ошибки
#define WM_APP_PHRASE       (WM_APP + 2)  // lParam=std::wstring* (владеет UI)
#define WM_APP_LISTEN_STATE (WM_APP + 3)  // wParam=listening?1:0
#define WM_APP_EXIT         (WM_APP + 4)
#define WM_APP_ENGINE_LOG   (WM_APP + 5)  // lParam=std::wstring*

static void UiPhrase(const std::wstring& text) {
    PostMessageW(g_hMain, WM_APP_PHRASE, 0, (LPARAM)new std::wstring(text));
}
static void UiListenState(bool on) {
    PostMessageW(g_hMain, WM_APP_LISTEN_STATE, on ? 1 : 0, 0);
}
static void UiEngineLog(const std::wstring& line) {
    PostMessageW(g_hMain, WM_APP_ENGINE_LOG, 0, (LPARAM)new std::wstring(line));
}

// ================= действия =================

static void StartListeningAsync() {
    if (g_shutdown || g_listening) return;
    PostTask([]() {
        if (g_recHandle.empty()) {
            UiEngineLog(L"⚠️ Распознаватель не создан — микрофон недоступен.");
            return;
        }
        g_listening = true;
        UiListenState(true);
        std::wstring phrase;
        bool got = g_engine.WaitForPhrase(g_recHandle, LISTEN_TIMEOUT_MS, phrase);
        g_listening = false;
        UiListenState(false);
        if (got && !phrase.empty()) UiPhrase(phrase);
    });
}

static void SpeakNow(const std::wstring& text) {
    PostTask([text]() {
        if (text.empty()) g_engine.StopSpeak();
        else              g_engine.Speak(text);
    });
}

// Обработка команды: логика выполняется в SpeechThread (CreateProcess/volume),
// вывод — через сообщения в UI.
static void HandleCommandText(const std::wstring& text, bool fromVoice) {
    if (text.empty()) return;
    AppendLog((fromVoice ? L"🎤 Распознано: " : L"⌨️ Ввод: ") + text);

    std::wstring copy = text;
    PostTask([copy, fromVoice]() {
        CommandResult res = g_brain.Process(copy);
        if (res.stopSpeaking) g_engine.StopSpeak();
        if (!res.logLine.empty()) UiEngineLog(res.logLine);
        if (!res.speech.empty())  g_engine.Speak(res.speech);
        if (res.exitApp) { PostMessageW(g_hMain, WM_APP_EXIT, 0, 0); return; }
        if (fromVoice || res.startListen) {
            Sleep(res.speech.empty() ? 300 : 1800);
            if (!g_shutdown) StartListeningAsync();
        }
    });
}

// ================= SpeechThread =================

static DWORD WINAPI SpeechThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    wchar_t exeDir[MAX_PATH];
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    std::wstring dir(exeDir);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir = dir.substr(0, slash + 1);
    std::wstring bridgePath = dir + L"S1mpleBridge.dll";

    std::wstring err;
    HRESULT hr = g_engine.Init(bridgePath, err);

    if (SUCCEEDED(hr)) {
        g_engine.GetRecognizers(g_recognizers, err);
        if (!err.empty()) UiEngineLog(L"⚠️ Распознаватели: " + err);
        g_engine.GetVoices(g_voices, err);
        if (!err.empty()) UiEngineLog(L"⚠️ Голоса: " + err);

        for (auto& r : g_recognizers) {
            std::wstring h, e2;
            if (g_engine.CreateRecognizer(r.id, h, e2)) { g_recHandle = h; break; }
        }
    }
    PostMessageW(g_hMain, WM_APP_READY, (WPARAM)hr, (LPARAM)new std::wstring(err));

    // Цикл задач
    while (!g_shutdown) {
        WaitForSingleObject(g_taskEvent, INFINITE);
        Task t;
        while (!g_shutdown && PopTask(t)) t();
    }

    if (!g_recHandle.empty()) g_engine.DestroyRecognizer(g_recHandle);
    g_engine.Shutdown();
    CoUninitialize();
    return 0;
}

// ================= создание контролов =================

static void CreateControls(HWND hwnd) {
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    g_hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"RICHEDIT50W", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        12, 12, 740, 300, hwnd, (HMENU)IDC_LOG, g_hInst, nullptr);
    SendMessageW(g_hLog, WM_SETFONT, (WPARAM)font, TRUE);

    g_hInput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        12, 324, 560, 28, hwnd, (HMENU)IDC_INPUT, g_hInst, nullptr);
    SendMessageW(g_hInput, WM_SETFONT, (WPARAM)font, TRUE);

    g_hSend = CreateWindowW(L"BUTTON", L"Отправить",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        584, 324, 90, 28, hwnd, (HMENU)IDC_SEND, g_hInst, nullptr);
    SendMessageW(g_hSend, WM_SETFONT, (WPARAM)font, TRUE);

    g_hListen = CreateWindowW(L"BUTTON", L"Слушать",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        686, 324, 66, 28, hwnd, (HMENU)IDC_LISTEN, g_hInst, nullptr);
    SendMessageW(g_hListen, WM_SETFONT, (WPARAM)font, TRUE);

    g_hStopTts = CreateWindowW(L"BUTTON", L"Молчать",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        686, 358, 66, 28, hwnd, (HMENU)IDC_STOP_TTS, g_hInst, nullptr);
    SendMessageW(g_hStopTts, WM_SETFONT, (WPARAM)font, TRUE);

    int y = 358;
    g_hMicCombo = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        12, y, 300, 240, hwnd, (HMENU)IDC_MIC_COMBO, g_hInst, nullptr);
    SendMessageW(g_hMicCombo, WM_SETFONT, (WPARAM)font, TRUE);

    g_hVoiceCombo = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        320, y, 220, 240, hwnd, (HMENU)IDC_VOICE_COMBO, g_hInst, nullptr);
    SendMessageW(g_hVoiceCombo, WM_SETFONT, (WPARAM)font, TRUE);

    g_hRateSlider = CreateWindowExW(0, TRACKBAR_CLASS_W, L"",
        WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
        550, y - 2, 120, 32, hwnd, (HMENU)IDC_RATE_SLIDER, g_hInst, nullptr);
    SendMessageW(g_hRateSlider, TBM_SETRANGE, TRUE, MAKELONG(-10, 10));
    SendMessageW(g_hRateSlider, TBM_SETPOS, TRUE, 0);

    g_hRateLabel = CreateWindowW(L"STATIC", L"скорость 0",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        550, y + 28, 120, 18, hwnd, (HMENU)IDC_RATE_LABEL, g_hInst, nullptr);
    SendMessageW(g_hRateLabel, WM_SETFONT, (WPARAM)font, TRUE);

    g_hWakeEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"саша",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        12, y + 40, 120, 26, hwnd, (HMENU)IDC_WAKE_EDIT, g_hInst, nullptr);
    SendMessageW(g_hWakeEdit, WM_SETFONT, (WPARAM)font, TRUE);

    HWND hWakeLbl = CreateWindowW(L"STATIC", L"— ключевое слово",
        WS_CHILD | WS_VISIBLE,
        140, y + 44, 160, 18, hwnd, (HMENU)IDC_WAKE_LABEL, g_hInst, nullptr);
    SendMessageW(hWakeLbl, WM_SETFONT, (WPARAM)font, TRUE);

    g_hStatus = CreateWindowW(L"STATIC", L"Инициализация…",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        12, y + 74, 740, 20, hwnd, (HMENU)IDC_STATUS, g_hInst, nullptr);
    SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)font, TRUE);
}

static void PopulateCombos() {
    SendMessageW(g_hMicCombo, CB_RESETCONTENT, 0, 0);
    for (auto& r : g_recognizers) {
        std::wstring label = r.name + L" [" + r.culture + L"]";
        SendMessageW(g_hMicCombo, CB_ADDSTRING, 0, (LPARAM)label.c_str());
    }
    if (!g_recognizers.empty()) SendMessageW(g_hMicCombo, CB_SETCURSEL, 0, 0);

    SendMessageW(g_hVoiceCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_hVoiceCombo, CB_ADDSTRING, 0, (LPARAM)L"(по умолчанию)");
    for (auto& v : g_voices)
        SendMessageW(g_hVoiceCombo, CB_ADDSTRING, 0, (LPARAM)v.name.c_str());
    SendMessageW(g_hVoiceCombo, CB_SETCURSEL, 0, 0);
}

// ================= меню / диалоги =================

static HMENU BuildMenu() {
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_CLEAR, L"&Очистить журнал");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"&Выход");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&Файл");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&О программе");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Справка");
    return bar;
}

static void ShowAbout() {
    MessageBoxW(g_hMain,
        L"S1mple (Сашa) — офлайн голосовой помощник.\r\n\r\n"
        L"Распознавание и синтез речи: Microsoft System.Speech (.NET).\r\n"
        L"Интерфейс: чистый Win32 C++.\r\n\r\n"
        L"Команды: «который час», «открой блокнот», «увеличь громкость»,\r\n"
        L"«запиши заметку …», «погугли …», «расскажи анекдот», «стоп», «повтори», «пока».",
        L"О программе", MB_OK | MB_ICONINFORMATION);
}

// ================= оконная процедура =================

static void OnSendText() {
    int len = GetWindowTextLengthW(g_hInput);
    std::wstring text(len + 1, L'\0');
    GetWindowTextW(g_hInput, &text[0], len + 1);
    text.resize(len);
    SetWindowTextW(g_hInput, L"");
    if (!text.empty()) HandleCommandText(text, false);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hMain = hwnd;
        CreateControls(hwnd);
        SetMenu(hwnd, BuildMenu());
        g_taskEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        CreateThread(nullptr, 0, SpeechThread, nullptr, 0, nullptr);
        SetTimer(hwnd, TIMER_TTS_POLL, 500, nullptr);
        return 0;
    }

    case WM_APP_READY: {
        HRESULT hr = (HRESULT)(INT_PTR)wParam;
        std::wstring* err = (std::wstring*)lParam;
        if (SUCCEEDED(hr)) {
            SetStatusText(L"Готов. Нажмите «Слушать» и скажите команду.");
            PopulateCombos();
            AppendLog(L"✅ Движок речи запущен. Распознавателей: " +
                      std::to_wstring(g_recognizers.size()) + L", голосов: " +
                      std::to_wstring(g_voices.size()));
            SpeakNow(L"Привет! Я Саша, голосовой помощник S1mple. Скажи «помощь», чтобы узнать мои команды.");
        } else {
            SetStatusText(L"Ошибка инициализации речи — доступен текстовый режим.");
            AppendLog(L"❌ " + (err ? *err : std::wstring(L"неизвестная ошибка")));
        }
        delete err;
        return 0;
    }

    case WM_APP_PHRASE: {
        std::wstring* text = (std::wstring*)lParam;
        if (text) { HandleCommandText(*text, true); delete text; }
        return 0;
    }

    case WM_APP_LISTEN_STATE: {
        bool on = wParam != 0;
        EnableWindow(g_hListen, !on);
        SetWindowTextW(g_hListen, on ? L"Слушаю…" : L"Слушать");
        SetStatusText(on ? L"Слушаю…" : L"Готов");
        return 0;
    }

    case WM_APP_ENGINE_LOG: {
        std::wstring* line = (std::wstring*)lParam;
        if (line) { AppendLog(*line); delete line; }
        return 0;
    }

    case WM_APP_EXIT:
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_TTS_POLL) {
            static bool wasSpeaking = false;
            bool now = g_engine.Available() && g_engine.IsSpeaking();
            if (now) SetStatusText(L"Говорю…");
            else if (wasSpeaking && !g_listening) SetStatusText(L"Готов");
            wasSpeaking = now;
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_SEND:
            if (HIWORD(wParam) == BN_CLICKED) OnSendText();
            return 0;
        case IDC_LISTEN:
            StartListeningAsync();
            return 0;
        case IDC_STOP_TTS:
            SpeakNow(L"");
            return 0;
        case IDC_VOICE_COMBO:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                int sel = (int)SendMessageW(g_hVoiceCombo, CB_GETCURSEL, 0, 0);
                std::wstring name;
                if (sel > 0 && sel - 1 < (int)g_voices.size()) name = g_voices[sel - 1].name;
                PostTask([name]() { g_engine.SetVoice(name); });
            }
            return 0;
        case IDC_WAKE_EDIT:
            if (HIWORD(wParam) == EN_KILLFOCUS) {
                int len = GetWindowTextLengthW(g_hWakeEdit);
                std::wstring w(len + 1, L'\0');
                GetWindowTextW(g_hWakeEdit, &w[0], len + 1);
                w.resize(len);
                if (!w.empty()) {
                    g_brain.SetWakeWord(w);
                    AppendLog(L"⚙️ Ключевое слово: «" + w + L"»");
                }
            }
            return 0;
        case IDC_MIC_COMBO:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                int sel = (int)SendMessageW(g_hMicCombo, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < (int)g_recognizers.size()) {
                    std::wstring id = g_recognizers[sel].id;
                    PostTask([id]() {
                        std::wstring h, e;
                        if (g_engine.CreateRecognizer(id, h, e)) {
                            if (!g_recHandle.empty()) g_engine.DestroyRecognizer(g_recHandle);
                            g_recHandle = h;
                            UiEngineLog(L"⚙️ Распознаватель переключён.");
                        } else UiEngineLog(L"⚠️ Не удалось переключить распознаватель: " + e);
                    });
                }
            }
            return 0;
        case IDM_CLEAR:
            SetWindowTextW(g_hLog, L"");
            return 0;
        case IDM_ABOUT:
            ShowAbout();
            return 0;
        case IDM_EXIT:
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        break;

    case WM_HSCROLL:
        if ((HWND)lParam == g_hRateSlider) {
            int pos = (int)SendMessageW(g_hRateSlider, TBM_GETPOS, 0, 0);
            wchar_t buf[48];
            swprintf(buf, 48, L"скорость %+d", pos);
            SetWindowTextW(g_hRateLabel, buf);
            PostTask([pos]() { g_engine.SetRate(pos); });
        }
        return 0;

    case WM_CLOSE:
        g_shutdown = true;
        if (g_taskEvent) SetEvent(g_taskEvent);
        KillTimer(hwnd, TIMER_TTS_POLL);
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ================= WinMain =================

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow) {
    g_hInst = hInst;
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);
    LoadLibraryW(L"Msftedit.dll"); // RICHEDIT50W

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"S1mpleMainWindow";
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_MAIN));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"S1mpleMainWindow",
        L"S1mple — голосовой помощник Саша",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 790, 520,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    AppendLog(L"Добро пожаловать! Я Саша. Нажмите «Слушать» и скажите команду, либо напишите её ниже.");

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    g_shutdown = true;
    if (g_taskEvent) { SetEvent(g_taskEvent); CloseHandle(g_taskEvent); g_taskEvent = nullptr; }
    CoUninitialize();
    return (int)m.wParam;
}
