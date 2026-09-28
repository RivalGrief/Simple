// =====================================================================
//  S1mple (Саша) — офлайн голосовой помощник для Windows
//  bridge/Bridge.cs — управляемая прослойка между native C++ и .NET.
//
//  Компилируется в S1mpleBridge.dll (см. build.ps1):
//      csc /target:library /out:S1mpleBridge.dll Bridge.cs /reference:System.Speech.dll
//
//  Native-сторона вызывает статический метод
//      int Bridge.Entry(string jsonRequest, out string jsonResponse)
//  через ICLRRuntimeHost::ExecuteInDefaultAppDomain. Так как этот API
//  умеет возвращать только int, результат кладётся в out-параметр.
//
//  Формат запроса:  {"m":"Method","a":["arg",123,true,...]}
//  Формат ответа:   {"ok":...} либо {"error":"..."}
// =====================================================================
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;
using System.Threading;
using Microsoft.Speech.Recognition;   // распознавание речи (System.Speech)
using Microsoft.Speech.Synthesis;     // синтез речи        (System.Speech)

namespace S1mpleBridge
{
    public static class Bridge
    {
        // ------------------------- JSON utils -------------------------
        static string J(string s)
        {
            if (s == null) return "";
            var sb = new StringBuilder(s.Length + 8);
            foreach (char c in s)
            {
                switch (c)
                {
                    case '"':  sb.Append("\\\""); break;
                    case '\\': sb.Append("\\\\"); break;
                    case '\n': sb.Append("\\n");  break;
                    case '\r': sb.Append("\\r");  break;
                    case '\t': sb.Append("\\t");  break;
                    default:
                        if (c < 0x20) sb.AppendFormat("\\u{0:x4}", (int)c);
                        else sb.Append(c);
                        break;
                }
            }
            return sb.ToString();
        }

        static string Ok(string rawJson) { return "{\"ok\":" + rawJson + "}"; }
        static string OkStr(string s)    { return "{\"ok\":\"" + J(s) + "\"}"; }
        static string OkBool(bool b)     { return "{\"ok\":" + (b ? "true" : "false") + "}"; }
        static string Err(string m)      { return "{\"error\":\"" + J(m) + "\"}"; }

        // Разбор минимального JSON-запроса {"m":"Name","a":[...]}
        static string GetMember(string json)
        {
            const string key = "\"m\":\"";
            int p = json.IndexOf(key, StringComparison.Ordinal);
            if (p < 0) return "";
            p += key.Length;
            var sb = new StringBuilder();
            for (int i = p; i < json.Length; ++i)
            {
                char c = json[i];
                if (c == '\\' && i + 1 < json.Length) { sb.Append(json[++i]); continue; }
                if (c == '"') break;
                sb.Append(c);
            }
            return sb.ToString();
        }

        static List<string> GetArgs(string json)
        {
            var list = new List<string>();
            int p = json.IndexOf("\"a\":[", StringComparison.Ordinal);
            if (p < 0) return list;
            p += 5;
            int depth = 0; bool inStr = false; int start = p;
            for (int i = p; i < json.Length; ++i)
            {
                char c = json[i];
                if (inStr)
                {
                    if (c == '\\') { ++i; continue; }
                    if (c == '"') inStr = false;
                    continue;
                }
                switch (c)
                {
                    case '"': if (depth == 0) start = i; inStr = true; break;
                    case '[': case '{': ++depth; break;
                    case ']': case '}':
                        if (depth == 0) { AddArg(list, json.Substring(start, i - start)); return list; }
                        --depth; break;
                    case ',':
                        if (depth == 0) { AddArg(list, json.Substring(start, i - start)); start = i + 1; }
                        break;
                    default:
                        if (depth == 0 && !inStr && start <= p && !char.IsWhiteSpace(c)) start = i;
                        break;
                }
            }
            return list;
        }

        static void AddArg(List<string> list, string raw)
        {
            raw = raw.Trim();
            if (raw.Length >= 2 && raw[0] == '"' && raw[raw.Length - 1] == '"')
            {
                raw = raw.Substring(1, raw.Length - 2);
                var sb = new StringBuilder(raw.Length);
                for (int i = 0; i < raw.Length; ++i)
                {
                    char c = raw[i];
                    if (c == '\\' && i + 1 < raw.Length)
                    {
                        char n = raw[++i];
                        switch (n)
                        {
                            case 'n': sb.Append('\n'); break;
                            case 'r': sb.Append('\r'); break;
                            case 't': sb.Append('\t'); break;
                            case 'u':
                                if (i + 4 < raw.Length)
                                {
                                    sb.Append((char)int.Parse(raw.Substring(i + 1, 4), NumberStyles.HexNumber));
                                    i += 4;
                                }
                                break;
                            default: sb.Append(n); break;
                        }
                    }
                    else sb.Append(c);
                }
                list.Add(sb.ToString());
            }
            else list.Add(raw); // числа/bool приходят как строки
        }

        static string Arg(List<string> a, int i) { return i < a.Count ? a[i] : ""; }
        static int ArgInt(List<string> a, int i, int def)
        {
            int v;
            return int.TryParse(Arg(a, i), NumberStyles.Integer, CultureInfo.InvariantCulture, out v) ? v : def;
        }

        // ------------------------- entry point -------------------------
        // Единая точка входа для CLR Hosting API: ExecuteInDefaultAppDomain
        // умеет вызывать только static метод вида  int M(string)  и возвращать int.
        // Поэтому ответ передаётся через статический буфер, а native читает его
        // вторым round-trip'ом методом FetchLast(). Вызовы из native сериализуются
        // lock'ом в SpeechEngine::Call, так что гонки за буфер нет.
        //
        // Контракт чтения ответа:
        //   Entry(request)      — выполняет запрос, кэширует JSON-ответ;
        //   FetchLast(chunkReq) — chunkReq имеет вид "<index>", возвращает длину
        //                         чанка; сам чанк native получить напрямую не может,
        //                         поэтому используется трюк с shared memory:
        //   Ответ пишется в файл %TEMP%\S1mpleBridge\resp.json (атомарно), а
        //   FetchLast возвращает его размер. Native читает файл. Это надёжно и
        //   не требует COM-регистрации моста.
        static string _lastResponse = "";

        public static int Entry(string request)
        {
            try { _lastResponse = Dispatch(request); }
            catch (Exception e) { _lastResponse = Err(e.Message); }
            try { WriteShared(_lastResponse); } catch { }
            return 0;
        }

        public static int FetchLast(string unused)
        {
            return _lastResponse == null ? 0 : _lastResponse.Length;
        }

        static readonly object _fileLock = new object();

        static string SharedPath()
        {
            string dir = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "S1mpleBridge");
            System.IO.Directory.CreateDirectory(dir);
            return System.IO.Path.Combine(dir, "resp.json");
        }

        static void WriteShared(string content)
        {
            lock (_fileLock)
            {
                string path = SharedPath();
                string tmp = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
                System.IO.File.WriteAllText(tmp, content ?? "", new System.Text.UTF8Encoding(false));
                if (System.IO.File.Exists(path)) System.IO.File.Delete(path);
                System.IO.File.Move(tmp, path);
            }
        }

        static string Dispatch(string request)
        {
            string m = GetMember(request);
            var a = GetArgs(request);
            switch (m)
            {
                case "Ping":              return Ok("true");
                case "GetRecognizers":    return Ok(GetRecognizersJson());
                case "CreateRecognizer":  return CreateRecognizerHandler(Arg(a, 0));
                case "DestroyRecognizer": DestroyRecognizer(Arg(a, 0)); return Ok("true");
                case "WaitForPhrase":     return WaitForPhraseHandler(Arg(a, 0), ArgInt(a, 1, 8000));
                case "CancelListening":   CancelListening(Arg(a, 0));   return Ok("true");
                case "Speak":             return OkBool(SpeakAsync(Arg(a, 0)));
                case "IsSpeaking":        return OkBool(IsSpeaking());
                case "StopSpeak":         StopSpeak();                  return Ok("true");
                case "GetVoices":         return Ok(GetVoicesJson());
                case "SetVoice":          return OkBool(SetVoice(Arg(a, 0)));
                case "SetRate":           return OkBool(SetRate(ArgInt(a, 0, 0)));
                default:                  return Err("Неизвестный метод: " + m);
            }
        }

        // ------------------------- распознавание -------------------------
        class RecItem
        {
            public SpeechRecognitionEngine Engine;
            public Grammar                 Grammar;
            public readonly AutoResetEvent   Done   = new AutoResetEvent(false);
            public readonly ManualResetEvent Cancel = new ManualResetEvent(false);
            public volatile string Result = "";
        }

        static readonly Dictionary<string, RecItem> _recs = new Dictionary<string, RecItem>();
        static int _recCounter;

        static string GetRecognizersJson()
        {
            List<RecognizerInfo> infos = null;
            try { infos = RecognizerInfo.GetInstalledRecognizers(CultureInfo.GetCultureInfo("ru-RU")); }
            catch { }
            if (infos == null || infos.Count == 0) infos = RecognizerInfo.GetInstalledRecognizers();

            var sb = new StringBuilder("[");
            bool first = true;
            if (infos != null)
                foreach (RecognizerInfo info in infos)
                {
                    if (!first) sb.Append(',');
                    first = false;
                    sb.Append("{\"id\":\"").Append(J(info.Id))
                      .Append("\",\"name\":\"").Append(J(info.Name))
                      .Append("\",\"culture\":\"").Append(J(info.Culture.Name)).Append("\"}");
                }
            sb.Append(']');
            return sb.ToString();
        }

        // Словарь фраз грамматики. System.Speech распознаёт только то, что
        // объявлено в грамматике, поэтому держим здесь все ключевые слова команд.
        static string[] DefaultPhrases()
        {
            return new[]
            {
                // wake word варианты (распознаватель не всегда чётко ловит «Саша»)
                "саша", "шаша", "саня", "саш", "ача", "аша",
                // приветствия / светские
                "привет", "здравствуй", "доброе утро", "добрый день", "добрый вечер",
                "как дела", "что ты умеешь", "помощь", "спасибо", "до свидания", "пока",
                "кто ты", "как тебя зовут", "расскажи анекдот", "ты тут",
                // время / дата
                "который час", "сколько времени", "какое сегодня число", "какая дата",
                // запуск программ
                "запусти блокнот", "открой блокнот", "запусти калькулятор", "открой калькулятор",
                "запусти пейнт", "открой пейнт", "запусти проводник", "открой проводник",
                "запусти браузер", "открой браузер",
                // система
                "выключи компьютер", "выключить компьютер", "заблокируй экран", "перезагрузи компьютер",
                "увеличь громкость", "уменьши громкость", "выключи звук", "включи звук",
                // поиск
                "найди в интернете", "погугли",
                // управление ассистентом
                "стоп", "хватит", "молчи", "повтори", "слушай",
                // заметки
                "запиши заметку", "создай заметку", "прочитай заметки",
                // цифры
                "один", "два", "три", "четыре", "пять", "шесть", "семь", "восемь", "девять", "десять",
                "да", "нет"
            };
        }

        static string CreateRecognizerHandler(string recognizerId)
        {
            try
            {
                RecognizerInfo info = null;
                var all = RecognizerInfo.GetInstalledRecognizers();
                if (all != null)
                    foreach (RecognizerInfo i in all)
                        if (i.Id == recognizerId) { info = i; break; }
                if (info == null)
                    return Err("Распознаватель речи не найден. Установите пакет распознавания речи Windows для русского языка.");

                var engine = new SpeechRecognitionEngine(info);
                engine.SetInputToDefaultAudioDevice();

                var gb = new GrammarBuilder { Culture = engine.RecognizerInfo.Culture };
                gb.Append(new Choices(DefaultPhrases()));
                var grammar = new Grammar(gb) { Name = "S1mpleGrammar" };
                engine.LoadGrammar(grammar);

                var item = new RecItem { Engine = engine, Grammar = grammar };
                engine.RecognizeCompleted += (s, e) =>
                {
                    if (e.Result != null && !string.IsNullOrEmpty(e.Result.Text))
                        item.Result = e.Result.Text;
                    item.Done.Set();
                };

                string handle;
                lock (_recs)
                {
                    handle = "rec-" + (++_recCounter).ToString(CultureInfo.InvariantCulture);
                    _recs[handle] = item;
                }
                return OkStr(handle);
            }
            catch (Exception e) { return Err(e.Message); }
        }

        static void DestroyRecognizer(string handle)
        {
            RecItem item;
            lock (_recs) { if (!_recs.TryGetValue(handle, out item)) return; _recs.Remove(handle); }
            try { item.Cancel.Set(); item.Engine.RecognizeAsyncCancel(); } catch { }
            try { item.Engine.Dispose(); } catch { }
        }

        static string WaitForPhraseHandler(string handle, int timeoutMs)
        {
            try
            {
                RecItem item;
                lock (_recs) { if (!_recs.TryGetValue(handle, out item)) return Err("Нет такого распознавателя"); }

                item.Result = "";
                item.Cancel.Reset();
                item.Done.Reset();
                item.Engine.RecognizeAsync(RecognizeMode.Single);

                int waited = 0;
                while (!item.Done.WaitOne(100))
                {
                    if (item.Cancel.WaitOne(0))
                    {
                        try { item.Engine.RecognizeAsyncCancel(); } catch { }
                        return OkStr("");
                    }
                    waited += 100;
                    if (timeoutMs > 0 && waited >= timeoutMs)
                    {
                        try { item.Engine.RecognizeAsyncCancel(); } catch { }
                        item.Done.WaitOne(400);
                        break;
                    }
                }
                return OkStr(item.Result ?? "");
            }
            catch (Exception e) { return Err(e.Message); }
        }

        static void CancelListening(string handle)
        {
            RecItem item;
            lock (_recs) { if (!_recs.TryGetValue(handle, out item)) return; }
            try { item.Cancel.Set(); item.Engine.RecognizeAsyncCancel(); } catch { }
            item.Done.Set();
        }

        // ------------------------- синтез -------------------------
        static SpeechSynthesizer _synth;
        static readonly object _synthLock = new object();

        static SpeechSynthesizer Synth()
        {
            if (_synth == null)
            {
                _synth = new SpeechSynthesizer();
                _synth.SetOutputToDefaultAudioDevice();
            }
            return _synth;
        }

        static bool SpeakAsync(string text)
        {
            try
            {
                if (string.IsNullOrEmpty(text)) return false;
                lock (_synthLock)
                {
                    var s = Synth();
                    s.SpeakAsyncCancelAll();
                    s.SpeakAsync(text);
                }
                return true;
            }
            catch { return false; }
        }

        static bool IsSpeaking()
        {
            try { lock (_synthLock) { return _synth != null && _synth.State != SynthesizerState.NotSpeaking; } }
            catch { return false; }
        }

        static void StopSpeak()
        {
            try { lock (_synthLock) { if (_synth != null) _synth.SpeakAsyncCancelAll(); } } catch { }
        }

        static string GetVoicesJson()
        {
            try
            {
                var voices = new List<VoiceInfo>();
                lock (_synthLock)
                    foreach (var v in Synth().GetInstalledVoices())
                        if (v.Enabled) voices.Add(v.VoiceInfo);

                Func<CultureInfo, bool> isRu = ci =>
                    ci.TwoLetterISOLanguageName.Equals("ru", StringComparison.OrdinalIgnoreCase);

                var sb = new StringBuilder("[");
                bool first = true;
                foreach (var v in voices)
                {
                    if (!isRu(v.Culture)) continue;
                    if (!first) sb.Append(',');
                    first = false;
                    sb.Append("{\"name\":\"").Append(J(v.Name))
                      .Append("\",\"culture\":\"").Append(J(v.Culture.Name)).Append("\"}");
                }
                if (first)
                    foreach (var v in voices)
                    {
                        if (!first) sb.Append(',');
                        first = false;
                        sb.Append("{\"name\":\"").Append(J(v.Name))
                          .Append("\",\"culture\":\"").Append(J(v.Culture.Name)).Append("\"}");
                    }
                sb.Append(']');
                return sb.ToString();
            }
            catch (Exception e) { return Err(e.Message); }
        }

        static bool SetVoice(string name)
        {
            try
            {
                lock (_synthLock)
                {
                    if (string.IsNullOrEmpty(name)) return true;
                    Synth().SelectVoice(name);
                }
                return true;
            }
            catch { return false; }
        }

        static bool SetRate(int rate)
        {
            try { lock (_synthLock) { Synth().Rate = Math.Max(-10, Math.Min(10, rate)); return true; } }
            catch { return false; }
        }
    }
}
