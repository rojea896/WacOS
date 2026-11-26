// Apps do Dock: os fixos (dock.txt; na primeira vez, os fixados na barra de tarefas do Windows),
// os abertos (janelas que apareceriam na barra de tarefas) e os recentes. Abrir, ativar, ocultar,
// encerrar, fixar, abrir ao iniciar sessão.
#include "shell.h"
#include "../comum/ajustes_arquivo.h"
#include "../comum/jogos.h"
#include <mutex>
#include <unordered_map>
#include <memory>
#include <thread>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <propkey.h>
#include <propvarutil.h>
#include <dwmapi.h>
#include <appmodel.h>
#include <algorithm>
#include <map>

std::vector<App> apps::list;
std::vector<std::wstring> apps::recent;
std::vector<HWND> apps::ours;
static std::wstring g_finderExe;

static std::wstring Lower(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }
static std::wstring VerKey(const std::wstring& name);   // apps que se atualizam numa pasta nova (mais abaixo, perto de Launch)
static std::vector<unsigned> VerNums(const std::wstring& name);
static bool Heal(App& a);

static std::wstring DataDir() {
    PWSTR p = nullptr;
    std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) { d = std::wstring(p) + L"\\Shell"; CoTaskMemFree(p); }
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

// Um app nosso aberto antes de uma compilação continua rodando do .exe antigo, que foi renomeado/movido
// (WacOSFinder-000312.exe na pasta de antigos, WacOSFinder.run-..., .old, .bak): para o Dock ele é o
// mesmo app do bin (antes aparecia um segundo Finder no fim do Dock).
static std::wstring OurCanonical(const std::wstring& path) {
    size_t sl = path.find_last_of(L'\\');
    std::wstring file = path.substr(sl == std::wstring::npos ? 0 : sl + 1);
    std::wstring low = file; for (auto& c : low) c = towlower(c);
    if (low.rfind(L"sequoia", 0) != 0 || low.size() < 12) return path;
    size_t cut = low.find_first_of(L"-.");
    if (cut == std::wstring::npos || low.compare(cut, std::wstring::npos, L".exe") == 0) return path;  // já é o nome certo
    std::wstring canon = shell::dir + L"\\" + file.substr(0, cut) + L".exe";
    return GetFileAttributesW(canon.c_str()) != INVALID_FILE_ATTRIBUTES ? canon : path;
}

static std::wstring ExeOfProcess(DWORD pid) {
    std::wstring out;
    if (HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2]; DWORD n = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(hp, 0, buf, &n)) out = buf;
        CloseHandle(hp);
    }
    return OurCanonical(out);
}

// O que já se perguntou de cada janela. O programa de uma janela não muda; perguntar de novo a cada volta do timer
// (OpenProcess + QueryFullProcessImageName, que espera o disco) parou a interface do Shell por 4,9 s em 22/11/2026
// (pilha: menubar::Update -> OfWindow -> ExeOfWindow). A chave é janela + processo + thread: um HWND reaproveitado não casa.
struct DaJanela {
    DWORD pid = 0, tid = 0;
    ULONGLONG desde = 0;
    std::wstring exe, aumid, aumidProc;
    bool temAumidProc = false;
};
static std::mutex g_janMx;
static std::unordered_map<HWND, DaJanela> g_jan;
static DaJanela Guardado(HWND h) {
    DWORD pid = 0, tid = GetWindowThreadProcessId(h, &pid);
    std::lock_guard<std::mutex> l(g_janMx);
    if (g_jan.size() > 1024)
        for (auto it = g_jan.begin(); it != g_jan.end();) it = IsWindow(it->first) ? std::next(it) : g_jan.erase(it);
    auto& d = g_jan[h];
    if (d.pid != pid || d.tid != tid) { d = {}; d.pid = pid; d.tid = tid; d.desde = GetTickCount64(); }
    return d;
}
template <class F> static void Guarda(HWND h, const DaJanela& de, F&& muda) {
    std::lock_guard<std::mutex> l(g_janMx);
    auto it = g_jan.find(h);
    if (it != g_jan.end() && it->second.pid == de.pid && it->second.tid == de.tid) muda(it->second);
}

static std::wstring ExeOfWindow(HWND h) {
    DaJanela d = Guardado(h);
    if (!d.exe.empty() || !d.pid) return d.exe;
    std::wstring exe = ExeOfProcess(d.pid);
    if (!exe.empty()) Guarda(h, d, [&](DaJanela& g) { g.exe = exe; });
    return exe;
}

static std::wstring AumidOfWindow(HWND h) {
    DaJanela d = Guardado(h);
    // o da janela pode chegar ou mudar logo depois de ela abrir: só fica guardado depois de 10 s
    if (!d.aumid.empty()) return d.aumid;
    IPropertyStore* ps = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(&ps)))) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_AppUserModel_ID, &v)) && v.vt == VT_LPWSTR && v.pwszVal) out = v.pwszVal;
        PropVariantClear(&v);
        ps->Release();
    }
    if (!out.empty()) {
        if (GetTickCount64() - d.desde > 10000) Guarda(h, d, [&](DaJanela& g) { g.aumid = out; });
        return out;
    }
    // app da Loja cuja janela não traz o AUMID (WhatsApp, WinUI/WebView2): o do processo, que não muda
    if (d.temAumidProc) return d.aumidProc;
    if (HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, d.pid)) {
        wchar_t buf[APPLICATION_USER_MODEL_ID_MAX_LENGTH] = {}; UINT32 n = APPLICATION_USER_MODEL_ID_MAX_LENGTH;
        if (GetApplicationUserModelId(hp, &n, buf) == ERROR_SUCCESS) out = buf;  // sem pacote: APPMODEL_ERROR_NO_APPLICATION
        CloseHandle(hp);
        Guarda(h, d, [&](DaJanela& g) { g.aumidProc = out; g.temAumidProc = true; });
    }
    return out;
}

// Os nossos apps aparecem só com o nome do app (como no Mac), na língua escolhida, nunca "Sequoia ...".
// Vazio: não é um dos nossos.
static std::wstring OurName(const std::wstring& exe) {
    std::wstring f = Lower(exe.substr(exe.find_last_of(L'\\') + 1));
    struct N { const wchar_t* exe; const wchar_t* pt; const wchar_t* en; };
    static const N names[] = {
        { L"wacosfinder.exe", L"Finder", L"Finder" },
        { L"wacossettings.exe", L"Ajustes do Sistema", L"System Settings" },
        { L"wacosactivitymonitor.exe", L"Monitor de Atividade", L"Activity Monitor" },
        { L"wacosterminal.exe", L"Terminal", L"Terminal" },
        { L"wacoscalculator.exe", L"Calculadora", L"Calculator" },
        { L"wacosnotes.exe", L"Notas", L"Notes" },
        { L"wacosscreenshot.exe", L"Captura de Tela", L"Screenshot" },
    };
    for (auto& n : names) if (f == n.exe) return TrPick(n.pt, n.en);
    return L"";
}

void apps::Relabel() {
    for (auto& a : list) {
        if (a.finder) continue;
        if (std::wstring o = OurName(a.exe.empty() ? a.launch : a.exe); !o.empty()) a.name = o;
    }
}

// Nome amigável do programa (descrição do arquivo), ou o nome do .exe.
static std::wstring ExeName(const std::wstring& exe) {
    if (std::wstring o = OurName(exe); !o.empty()) return o;
    DWORD dummy = 0, n = GetFileVersionInfoSizeW(exe.c_str(), &dummy);
    if (n) {
        std::vector<BYTE> buf(n);
        if (GetFileVersionInfoW(exe.c_str(), 0, n, buf.data())) {
            struct LangCp { WORD lang, cp; } *tr = nullptr; UINT len = 0;
            if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &len) && len >= sizeof(LangCp)) {
                wchar_t key[80];
                swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr[0].lang, tr[0].cp);
                wchar_t* desc = nullptr; UINT dl = 0;
                if (VerQueryValueW(buf.data(), key, (void**)&desc, &dl) && dl > 1 && desc[0]) return desc;
            }
        }
    }
    std::wstring f = exe.substr(exe.find_last_of(L'\\') + 1);
    if (f.size() > 4) f.resize(f.size() - 4);
    return f;
}

static std::wstring DisplayNameOf(const std::wstring& parsing) {
    IShellItem* si = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHCreateItemFromParsingName(parsing.c_str(), nullptr, IID_PPV_ARGS(&si)))) {
        PWSTR n = nullptr;
        if (SUCCEEDED(si->GetDisplayName(SIGDN_NORMALDISPLAY, &n))) { out = n; CoTaskMemFree(n); }
        si->Release();
    }
    return out;
}

// Destino (.exe) e AUMID de um atalho.
static void ReadLink(const std::wstring& lnk, std::wstring& target, std::wstring& aumid) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return;
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
        if (SUCCEEDED(pf->Load(lnk.c_str(), STGM_READ))) {
            wchar_t p[MAX_PATH] = {};
            if (SUCCEEDED(sl->GetPath(p, MAX_PATH, nullptr, 0))) target = p;
            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&ps)))) {
                PROPVARIANT v; PropVariantInit(&v);
                if (SUCCEEDED(ps->GetValue(PKEY_AppUserModel_ID, &v)) && v.vt == VT_LPWSTR && v.pwszVal) aumid = v.pwszVal;
                PropVariantClear(&v);
                ps->Release();
            }
        }
        pf->Release();
    }
    sl->Release();
}

bool apps::IsShellWindow(HWND h) { return std::find(ours.begin(), ours.end(), h) != ours.end(); }

std::wstring apps::IdOfWindow(HWND h) {
    std::wstring exe = Lower(ExeOfWindow(h));
    if (!g_finderExe.empty() && exe == Lower(g_finderExe)) return Lower(g_finderExe);
    wchar_t cls[32] = {}; GetClassNameW(h, cls, 32);
    if ((!wcscmp(cls, L"CabinetWClass") || !wcscmp(cls, L"ArquivosJanela")) && !g_finderExe.empty()) return Lower(g_finderExe);  // janela do Explorador (e o Finder nativo, dentro do explorer.exe) conta como do Finder
    std::wstring a = AumidOfWindow(h);
    if (!a.empty()) return L"aumid:" + Lower(a);
    return exe;
}

App* apps::FindById(const std::wstring& id) {
    for (auto& a : list) if (a.id == id) return &a;
    return nullptr;
}

// Janela com dona que fica sozinha: a dona está escondida (ou é só uma tela de abertura de ferramenta),
// como o aviso "recuperar o trabalho?" do Ableton ao abrir depois de fechar sem querer. O Windows mostra
// essas no Alt+Tab; antes ela ficava de fora do Dock, do seletor e do Mission Control e se perdia atrás de tudo.
bool apps::LoneOwned(HWND h) {
    HWND owner = GetWindow(h, GW_OWNER);
    if (!owner || !IsWindowVisible(h)) return false;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE), st = GetWindowLongPtrW(h, GWL_STYLE);
    if ((ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) || !(st & WS_CAPTION) && !GetPropW(h, L"MacJanela")) return false;
    // a janela de dona mais de cima: a dona dela não pode aparecer como app
    HWND root = GetAncestor(h, GA_ROOTOWNER);
    if (!root || root == h) return false;
    for (HWND o = owner; o; o = GetWindow(o, GW_OWNER)) {
        if (!IsWindowVisible(o)) continue;
        DWORD cl = 0; DwmGetWindowAttribute(o, DWMWA_CLOAKED, &cl, sizeof(cl));
        if (cl) continue;
        LONG_PTR oex = GetWindowLongPtrW(o, GWL_EXSTYLE);
        if ((oex & WS_EX_APPWINDOW) || (!(oex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) && !GetWindow(o, GW_OWNER) && GetWindowTextLengthW(o) > 0))
            return false;  // a dona já aparece como app (ela representa este aviso)
    }
    return true;
}

// Janela que o Windows mostraria na barra de tarefas (as mesmas regras do Alt+Tab).
static bool IsTaskbarWindow(HWND h) {
    if (!IsWindowVisible(h) || apps::IsShellWindow(h)) return false;
    // nenhuma janela do próprio Shell é "app" (no Alt+Tab rápido o "app anterior" chegava a ser o Shell,
    // e trocar para ele não fazia nada)
    if (shell::Nossa(h)) return false;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    HWND owner = GetWindow(h, GW_OWNER);
    bool lone = false;
    if (ex & WS_EX_APPWINDOW) {}
    else if (owner && apps::LoneOwned(h)) lone = true;
    else if (owner || (ex & WS_EX_TOOLWINDOW) || (ex & WS_EX_NOACTIVATE)) return false;
    DWORD cloaked = 0;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return false;
    wchar_t cls[64]; GetClassNameW(h, cls, 64);
    if (mesa::IsDeskClass(cls)) return false;
    for (auto c : { L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd" }) if (!wcscmp(cls, c)) return false;
    if (!wcscmp(cls, L"OperationStatusWindow") && progresso::Owns(h)) return false;  // trocada pela janela de progresso do Mac
    return GetWindowTextLengthW(h) > 0 || lone;  // aviso sem título (Ableton): vale também
}

bool apps::IsAppWindow(HWND h) { return IsTaskbarWindow(h); }

App* apps::OfWindow(HWND h) {
    std::wstring id = IdOfWindow(h);
    if (App* a = FindById(id)) return a;
    std::wstring exe = Lower(ExeOfWindow(h));
    for (auto& a : list) if (!a.exe.empty() && a.exe == exe) return &a;
    return nullptr;
}

void apps::Save() {
    // atômico (.tmp + troca): o Shell encerrado no meio da gravação deixava o dock.txt vazio e o Dock sem apps fixos
    std::wstring s;
    for (auto& a : list) if (a.pinned && !a.finder) s += a.id + L"\t" + a.launch + L"\t" + a.name + L"\t" + a.exe + L"\n";
    ajarq::GravarTexto(DataDir() + L"\\dock.txt", s);
}

void apps::Load(const std::wstring& finderExe) {
    g_finderExe = finderExe;
    list.clear();
    App fin;
    fin.id = Lower(finderExe); fin.exe = fin.id; fin.launch = finderExe; fin.name = L"Finder";
    fin.pinned = fin.finder = true;
    list.push_back(fin);

    FILE* f = nullptr;
    if (!_wfopen_s(&f, (DataDir() + L"\\dock.txt").c_str(), L"r, ccs=UTF-8") && f) {
        wchar_t line[2048];
        while (fgetws(line, 2048, f)) {
            std::wstring s = line;
            while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
            std::vector<std::wstring> p;
            size_t a = 0, b;
            while ((b = s.find(L'\t', a)) != std::wstring::npos) { p.push_back(s.substr(a, b - a)); a = b + 1; }
            p.push_back(s.substr(a));
            if (p.size() < 3) continue;
            if (p[0] == L"aumid:microsoft.windows.explorer") continue;  // o Finder já está no Dock
            App x; x.id = p[0]; x.launch = p[1]; x.name = p[2]; x.exe = p.size() > 3 ? p[3] : L""; x.pinned = true;
            if (std::wstring o = OurName(x.exe.empty() ? x.launch : x.exe); !o.empty()) x.name = o;
            list.push_back(x);
        }
        fclose(f);
        Log(L"dock: %zu apps fixos", list.size() - 1);
        bool healed = false;
        for (auto& a : list) healed |= Heal(a);  // apps que se atualizaram desde a última vez
        if (healed) Save();
        return;
    }
    // primeira vez: o que está fixado na barra de tarefas do Windows
    std::wstring pins = DataDir();
    PWSTR rp = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &rp))) {
        pins = std::wstring(rp) + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar";
        CoTaskMemFree(rp);
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((pins + L"\\*.lnk").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring lnk = pins + L"\\" + fd.cFileName, target, aumid;
            ReadLink(lnk, target, aumid);
            std::wstring t = Lower(target);
            if ((t.size() >= 12 && t.substr(t.size() - 12) == L"explorer.exe") || Lower(aumid) == L"microsoft.windows.explorer") continue;  // o Finder já está no Dock
            App x;
            x.exe = t;
            x.id = !aumid.empty() ? L"aumid:" + Lower(aumid) : t;
            x.launch = lnk;
            x.name = fd.cFileName; x.name.resize(x.name.size() - 4);
            x.pinned = true;
            list.push_back(x);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    Log(L"dock: importados %zu apps fixos da barra de tarefas", list.size() - 1);
    Save();
}

// issue #49: o Zen (e o Firefox e outros do Gecko) dão à janela um AUMID próprio (um código da pasta de instalação),
// diferente do AUMID do atalho fixado no Dock: os dois ids eram "aumid:" e o .exe igual não juntava, e o Zen aparecia
// duas vezes. Junta pelo .exe também nesse caso, menos onde um .exe tem vários apps de verdade: navegadores do Chromium
// (cada app da web/PWA tem o AUMID dele), apps da Loja e o hospedeiro dos apps UWP.
static bool ExeDeUmApp(const std::wstring& exeLower) {
    static std::unordered_map<std::wstring, bool> cache;
    auto it = cache.find(exeLower);
    if (it != cache.end()) return it->second;
    bool um = !exeLower.empty() && exeLower.find(L"\\windowsapps\\") == std::wstring::npos &&
              exeLower.find(L"applicationframehost.exe") == std::wstring::npos && !jogos::ProgramaChromium(exeLower);
    cache[exeLower] = um;
    return um;
}

bool apps::Refresh() {
    struct Ctx {
        std::map<std::wstring, std::vector<AppWin>> found;  // id -> janelas (ordem Z)
        std::map<std::wstring, std::wstring> exeOf;
        std::vector<std::wstring> order;
    } ctx;
    EnumWindows([](HWND h, LPARAM p) -> BOOL {
        if (!IsTaskbarWindow(h)) return TRUE;
        auto& c = *(Ctx*)p;
        std::wstring id = IdOfWindow(h);
        // janela de um app fixo pelo .exe (inclusive app da Loja fixado pelo caminho dentro de WindowsApps, que
        // agora chega com o AUMID do processo): usa o id do fixo, senão aparecia duas vezes no Dock
        std::wstring exe = Lower(ExeOfWindow(h));
        bool idFixo = false;  // um ícone do Dock já tem exatamente este id (ex.: o atalho da janela anônima do Firefox)
        for (auto& a : list) if (a.id == id && a.pinned) { idFixo = true; break; }
        if (!idFixo) for (auto& a : list)
            if (a.id != id && !a.exe.empty() && Lower(a.exe) == exe &&
                (id.rfind(L"aumid:", 0) != 0 || a.id.rfind(L"aumid:", 0) != 0 || (a.pinned && ExeDeUmApp(exe)))) { id = a.id; break; }
        // app fixo que se atualizou enquanto o Shell rodava (o .exe novo está numa pasta de versão nova): é o mesmo
        // ícone do Dock, e o caminho guardado passa a ser o novo
        if (id.rfind(L"aumid:", 0) != 0 && exe.find_first_of(L"0123456789") != std::wstring::npos)
            for (auto& a : list)
                if (a.pinned && !a.finder && a.id != id && !a.exe.empty() && Lower(a.exe) != exe && VerKey(a.exe) == VerKey(exe) &&
                    VerNums(exe) > VerNums(Lower(a.exe))) {
                    Log(L"dock: %ls atualizou: %ls -> %ls", a.name.c_str(), a.exe.c_str(), exe.c_str());
                    bool idWasExe = a.id == Lower(a.exe);
                    if (Lower(a.launch) == Lower(a.exe)) a.launch = ExeOfWindow(h);
                    a.exe = exe;
                    if (idWasExe) a.id = exe;
                    id = a.id;
                    Save();
                    break;
                }
        // o nosso Ajustes do Sistema fica no ícone do Ajustes do Windows fixado (não aparece um segundo)
        if (exe.size() >= 12 && exe.compare(exe.size() - 12, 12, L"\\ajustes.exe") == 0)
            for (auto& a : list) {
                std::wstring l = Lower(a.launch);
                if (a.pinned && l.find(L"immersivecontrolpanel") != std::wstring::npos) { id = a.id; break; }
            }
        // a nossa Calculadora fica no ícone da Calculadora do Windows fixada
        if (exe.size() >= 16 && exe.compare(exe.size() - 16, 16, L"\\calculadora.exe") == 0)
            for (auto& a : list)
                if (a.pinned && Lower(a.id + a.launch).find(L"windowscalculator") != std::wstring::npos) { id = a.id; break; }
        wchar_t t[256]; GetWindowTextW(h, t, 256);
        if (!c.found.count(id)) c.order.push_back(id);
        c.found[id].push_back({ h, t, IsIconic(h) != FALSE });
        c.exeOf[id] = exe;
        return TRUE;
    }, (LPARAM)&ctx);
    auto& found = ctx.found;
    auto& exeOf = ctx.exeOf;
    auto& order = ctx.order;

    std::wstring before;
    for (auto& a : list) before += a.id + L"|" + std::to_wstring(a.wins.size()) + (a.recent ? L"r" : L"") + L";";
    for (auto& a : list) for (auto& w : a.wins) if (w.minimized) before += L"m" + std::to_wstring((ULONG_PTR)w.hwnd);

    for (auto& a : list) {
        auto it = found.find(a.id);
        bool was = !a.wins.empty();
        a.wins = it != found.end() ? it->second : std::vector<AppWin>{};
        if (!a.wins.empty()) { a.launchedAt = 0; a.recent = false; }
        (void)was;
    }
    for (auto& id : order) {  // abertos que não estão no Dock
        if (FindById(id)) continue;
        App x;
        x.id = id;
        x.exe = exeOf[id];
        x.wins = found[id];
        // o nome bonito (descrição do .exe, nome do pacote) vem de uma thread: o GetFileVersionInfo carrega o .exe do
        // disco e segurou a interface 8 s neste tique de 1 s (pilha de 22/11/2026 19:59, issue #32). Até chegar, o do arquivo.
        bool pacote = id.rfind(L"aumid:", 0) == 0;
        if (pacote) x.launch = L"shell:AppsFolder\\" + AumidOfWindow(x.wins[0].hwnd);
        else x.launch = ExeOfWindow(x.wins[0].hwnd);
        {
            std::wstring arq = OurName(pacote ? x.exe : x.launch);
            if (arq.empty()) { arq = (pacote ? x.exe : x.launch); arq = arq.substr(arq.find_last_of(L'\\') + 1); if (arq.size() > 4) arq.resize(arq.size() - 4); }
            x.name = arq;
        }
        std::thread([id, pacote, launch = x.launch, exe = x.exe, provisorio = x.name] {
            HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            std::wstring n = pacote ? DisplayNameOf(launch) : ExeName(launch);
            if (n.empty()) n = ExeName(exe);
            if (SUCCEEDED(ci)) CoUninitialize();
            if (n.empty() || n == provisorio) return;
            shell::NaInterface([id, n, provisorio] {
                for (auto& a : apps::list) if (a.id == id && a.name == provisorio) { a.name = n; dock::Update(); break; }
            });
        }).detach();
        list.push_back(x);
        Log(L"dock: aberto %ls (%ls)", x.name.c_str(), x.id.c_str());
    }
    // sem seção de recentes: o app que não está fixo sai do Dock assim que fecha (só fica enquanto abre)
    recent.clear();
    list.erase(std::remove_if(list.begin(), list.end(), [](const App& a) {
        return !a.pinned && a.wins.empty() && (a.launchedAt == 0 || GetTickCount() - a.launchedAt > 20000);
    }), list.end());
    // ordem: fixos (na ordem do usuário), depois abertos/recentes
    std::stable_partition(list.begin(), list.end(), [](const App& a) { return a.pinned; });

    std::wstring after;
    for (auto& a : list) after += a.id + L"|" + std::to_wstring(a.wins.size()) + (a.recent ? L"r" : L"") + L";";
    for (auto& a : list) for (auto& w : a.wins) if (w.minimized) after += L"m" + std::to_wstring((ULONG_PTR)w.hwnd);  // minimizar/restaurar também conta
    return before != after;
}

// Ícone do pacote do Sequoia para o app: pelo nome, pelo .exe ou por um apelido conhecido.
static std::wstring Norm(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) if (iswalnum(c)) o += towlower(c);
    return o;
}

// ---- Ícone próprio do app (pedido do Reddit, 20/11/2026: "adiciona uma opção de editar os ícones dos apps").
// Botão direito no app no Dock ou no Launchpad > Alterar Ícone… (PNG, ICO, JPG, WebP, ou o ícone de outro .exe) e
// Restaurar Ícone Original. Fica em %APPDATA%\Shell\icones-dock (a mesma pasta de Ajustes > Personalizar o Dock >
// Ícones próprios) como <nome do .exe>.png e <nome do app>.png: o Dock conhece o .exe, o Launchpad e o Spotlight
// muitas vezes só o nome. Vale em todo lugar que pega o ícone pelo pacote (PackIconFor): Dock, Launchpad, Spotlight,
// Alt+Tab e Mission Control.
static std::wstring IconKey(std::wstring s) {
    for (auto& c : s) if (c < 32 || wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    while (!s.empty() && (s.back() == L' ' || s.back() == L'.')) s.pop_back();
    return s;
}
std::wstring apps::CustomIconDir() {
    static std::wstring d;
    if (d.empty()) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) { d = std::wstring(p) + L"\\Shell\\icones-dock\\"; CoTaskMemFree(p); }
    }
    return d;
}
static std::vector<std::wstring> CustomKeys(const std::wstring& name, const std::wstring& exe) {
    std::vector<std::wstring> k;
    std::wstring e = exe;
    size_t sl = e.find_last_of(L"\\/");
    if (sl != std::wstring::npos) e = e.substr(sl + 1);
    if (e.size() > 4 && !_wcsicmp(e.c_str() + e.size() - 4, L".exe") && e.find(L':') == std::wstring::npos) {
        e.resize(e.size() - 4);
        if (!(e = IconKey(e)).empty()) k.push_back(e);
    }
    if (std::wstring n = IconKey(name); !n.empty() && (k.empty() || _wcsicmp(n.c_str(), k[0].c_str()))) k.push_back(n);
    return k;
}
static std::mutex g_nomesMx;
static std::map<std::wstring, std::wstring> g_nomes;  // chave (minúsculas) -> nome escolhido
static std::wstring ArquivoNomes() {
    std::wstring d = apps::CustomIconDir();  // ...\Shell\icones-dock\ -> ...\Shell\nomes-apps.txt
    if (d.size() < 13) return L"";
    return d.substr(0, d.size() - 12) + L"nomes-apps.txt";
}
// relê o arquivo se ele mudou (conferido no máximo uma vez por segundo: o Dock pede o nome a cada quadro)
static void LeNomes() {
    static ULONGLONG quando = 0, gravado = 0;
    ULONGLONG agora = GetTickCount64();
    if (quando && agora - quando < 1000) return;
    quando = agora;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    std::wstring f = ArquivoNomes();
    ULONGLONG t = (!f.empty() && GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &fa))
                      ? ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32 | fa.ftLastWriteTime.dwLowDateTime) : 0;
    if (t == gravado) return;
    gravado = t;
    std::map<std::wstring, std::wstring> m;
    FILE* fp = nullptr;
    if (t && !_wfopen_s(&fp, f.c_str(), L"r, ccs=UTF-8") && fp) {
        wchar_t line[1024];
        while (fgetws(line, 1024, fp)) {
            std::wstring s = line;
            while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
            if (!s.empty() && s[0] == 0xFEFF) s.erase(0, 1);
            size_t tab = s.find(L'\t');
            if (tab == std::wstring::npos || tab == 0 || tab + 1 >= s.size()) continue;
            m[Lower(s.substr(0, tab))] = s.substr(tab + 1);
        }
        fclose(fp);
    }
    std::lock_guard<std::mutex> lk(g_nomesMx);
    g_nomes.swap(m);
}
std::wstring apps::NomeVisivel(const std::wstring& name, const std::wstring& exe) {
    LeNomes();
    std::lock_guard<std::mutex> lk(g_nomesMx);
    if (g_nomes.empty()) return name;
    for (auto& k : CustomKeys(name, exe)) {
        auto it = g_nomes.find(Lower(k));
        if (it != g_nomes.end()) return it->second;
    }
    return name;
}
void apps::Renomear(const std::wstring& name, const std::wstring& exe) {
    auto limpa = [](std::wstring s) { for (auto& c : s) if (c == L'|' || c == L'"') c = L' '; return s; };
    std::wstring pedido = L"renomear:" + limpa(NomeVisivel(name, exe));
    for (auto& k : CustomKeys(name, exe)) pedido += L"|" + limpa(k);
    shell::Abrir(L"open", (shell::dir + L"\\" EXE_SETTINGS).c_str(), (L"\"--pagina=" + pedido + L"\"").c_str());
}

std::wstring apps::CustomIcon(const std::wstring& name, const std::wstring& exe) {
    std::wstring dir = CustomIconDir();
    if (dir.empty()) return L"";
    for (auto& k : CustomKeys(name, exe)) {
        std::wstring f = dir + k + L".png";
        if (GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES) return f;
    }
    return L"";
}

// PNG (BGRA pré-multiplicado na memória; o arquivo vai com alfa normal)
static bool SavePng(const std::wstring& path, UINT w, UINT h, const std::vector<BYTE>& pbgra) {
    std::vector<BYTE> px(pbgra);
    for (size_t i = 0; i + 3 < px.size(); i += 4)
        if (BYTE a = px[i + 3]; a && a < 255) for (int c = 0; c < 3; c++) px[i + c] = (BYTE)std::min(255, px[i + c] * 255 / a);
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    bool ok = false;
    IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr;
    std::wstring tmp = path + L".tmp";
    if (SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(w, h))) {
        WICPixelFormatGUID f = GUID_WICPixelFormat32bppBGRA;
        ok = SUCCEEDED(fr->SetPixelFormat(&f)) && IsEqualGUID(f, GUID_WICPixelFormat32bppBGRA) &&
             SUCCEEDED(fr->WritePixels(h, w * 4, (UINT)px.size(), px.data())) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    }
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    wic->Release();
    ok = ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

// Lê a imagem escolhida (foto pelo WIC; .ico/.exe/.dll pelo ícone do Windows, no maior tamanho), deixa quadrada
// (centrada, fundo transparente), no máximo 512 px, e grava nos nomes do app. Roda fora da thread da interface.
static bool SaveCustomIcon(const std::wstring& name, const std::wstring& exe, const std::wstring& src) {
    UINT w = 0, h = 0;
    std::vector<BYTE> px;
    std::wstring ext = src.size() > 4 ? src.substr(src.size() - 4) : L"";
    for (auto& c : ext) c = towlower(c);
    bool icon = ext == L".ico" || ext == L".exe" || ext == L".dll" || ext == L".lnk";
    bool ok = icon ? ui::ShellImage(src, 256, w, h, px, true) : ui::LoadPhoto(src, 512, w, h, px);
    if (!ok && !icon) ok = ui::ShellImage(src, 256, w, h, px, true);
    if (!ok || !w || !h || px.size() < (size_t)w * h * 4) { Log(L"ícone próprio: não deu para ler %ls", src.c_str()); return false; }
    if (icon) ui::TrimToSquare(w, h, px);
    if (w != h) {  // quadrada, centrada
        UINT s = std::max(w, h);
        std::vector<BYTE> sq((size_t)s * s * 4, 0);
        UINT ox = (s - w) / 2, oy = (s - h) / 2;
        for (UINT y = 0; y < h; y++) memcpy(&sq[(((size_t)(y + oy)) * s + ox) * 4], &px[(size_t)y * w * 4], (size_t)w * 4);
        px.swap(sq); w = h = s;
    }
    ui::Downscale(w, h, px, 512);
    std::wstring dir = apps::CustomIconDir();
    if (dir.empty()) return false;
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    auto keys = CustomKeys(name, exe);
    bool any = false;
    for (auto& k : keys) any |= SavePng(dir + k + L".png", w, h, px);
    Log(L"ícone próprio: %ls <- %ls (%ux%u, %ls)", name.c_str(), src.c_str(), w, h, any ? L"gravado" : L"ERRO ao gravar");
    return any;
}

static void AvisarIconeMudou() {
    static UINT msg = RegisterWindowMessageW(L"ShellIconeMudou");
    if (HWND s = FindWindowW(L"ShellMensagens", nullptr)) PostMessageW(s, msg, 0, 0);
}

void apps::ChooseIcon(const std::wstring& name, const std::wstring& exe) {
    // numa thread própria (STA): a janela de Abrir é modal e a interface do Shell não pode parar (o vigia o reiniciaria)
    std::thread([name, exe] {
        // Primeiro o Finder em modo escolher (a janela de Abrir do Mac, issue #8): o mesmo exe que atende o
        // Abrir/Salvar dos outros programas; ele passa o pedido para a reserva e devolve 0 = escolheu.
        // Sem o Finder (ou se ele não abrir), cai na janela de Abrir do Windows, como antes.
        std::wstring finder = shell::dir + L"\\" EXE_FINDER;
        if (GetFileAttributesW(finder.c_str()) != INVALID_FILE_ATTRIBUTES) {
            wchar_t tmp[MAX_PATH], res[MAX_PATH];
            GetTempPathW(MAX_PATH, tmp);
            GetTempFileNameW(tmp, L"ico", 0, res);
            PWSTR pics = nullptr; std::wstring start;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pics))) { start = pics; CoTaskMemFree(pics); }
            auto q = [](const std::wstring& s) { return L"\"" + s + L"\""; };
            std::wstring tipos = std::wstring(Tr(L"Imagens")) + L"|*.png;*.ico;*.jpg;*.jpeg;*.webp;*.bmp;*.gif;*.tif;*.tiff;*.heic||" +
                                 Tr(L"Ícones e programas") + L"|*.ico;*.exe;*.dll";
            std::wstring cmd = q(finder) + L" " + q(start) + L" --escolher abrir --tipos " + q(tipos) + L" --tipo 0 --ok " + q(Tr(L"Escolher")) +
                               L" --resultado " + q(res);
            STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                AllowSetForegroundWindow(pi.dwProcessId);
                CloseHandle(pi.hThread);
                WaitForSingleObject(pi.hProcess, INFINITE);
                DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
                CloseHandle(pi.hProcess);
                std::wstring path;
                if (code == 0) {
                    FILE* f = nullptr;
                    if (!_wfopen_s(&f, res, L"r, ccs=UTF-8") && f) {
                        wchar_t line[2048];
                        while (path.empty() && fgetws(line, 2048, f)) {
                            std::wstring l = line;
                            while (!l.empty() && (l.back() == L'\n' || l.back() == L'\r')) l.pop_back();
                            if (!l.empty() && l.rfind(L"tipo=", 0) != 0) path = l;
                        }
                        fclose(f);
                    }
                }
                DeleteFileW(res);
                Log(L"ícone próprio: Finder escolher terminou (%lu)%ls", code, path.empty() ? L"" : L", arquivo escolhido");
                if (!path.empty() && SaveCustomIcon(name, exe, path)) AvisarIconeMudou();
                return;
            }
            DeleteFileW(res);
            Log(L"ícone próprio: Finder não abriu (%lu); janela do Windows", GetLastError());
        }
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        IFileOpenDialog* dlg = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
            std::wstring imgs = Tr(L"Imagens"), icos = Tr(L"Ícones e programas");
            COMDLG_FILTERSPEC f[] = { { imgs.c_str(), L"*.png;*.ico;*.jpg;*.jpeg;*.webp;*.bmp;*.gif;*.tif;*.tiff;*.heic" },
                                      { icos.c_str(), L"*.ico;*.exe;*.dll" } };
            dlg->SetFileTypes(2, f);
            wchar_t t[300]; swprintf_s(t, Tr(L"Escolha o novo ícone de “%ls”"), name.c_str());
            dlg->SetTitle(t);
            FILEOPENDIALOGOPTIONS o = 0;
            dlg->GetOptions(&o);
            dlg->SetOptions(o | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
            PWSTR pics = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pics))) {
                IShellItem* fo = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(pics, nullptr, IID_PPV_ARGS(&fo)))) { dlg->SetDefaultFolder(fo); fo->Release(); }
                CoTaskMemFree(pics);
            }
            if (SUCCEEDED(dlg->Show(nullptr))) {
                IShellItem* it = nullptr;
                PWSTR p = nullptr;
                if (SUCCEEDED(dlg->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                    if (SaveCustomIcon(name, exe, p)) AvisarIconeMudou();
                    CoTaskMemFree(p);
                }
                if (it) it->Release();
            }
            dlg->Release();
        }
        CoUninitialize();
    }).detach();
}

bool apps::HasCustomIcon(const std::wstring& name, const std::wstring& exe) { return !CustomIcon(name, exe).empty(); }

void apps::RestoreIcon(const std::wstring& name, const std::wstring& exe) {
    std::wstring dir = CustomIconDir();
    if (dir.empty()) return;
    int n = 0;
    for (auto& k : CustomKeys(name, exe)) if (DeleteFileW((dir + k + L".png").c_str())) n++;
    Log(L"ícone próprio: %ls volta ao original (%d arquivo(s))", name.c_str(), n);
    if (n) AvisarIconeMudou();
}

// O ícone mudou (escolhido, restaurado ou mexido na pasta): todo mundo relê
void apps::ReloadIcons() {
    for (auto& a : list) { a.icon.clear(); a.iw = a.ih = 0; a.mac = false; }
    dock::IconsChanged();
    seletor::IconsChanged();
    launchpad::IconsChanged();
    notificacoes::IconsChanged();  // o ícone próprio vale também nas notificações (issue #8)
}

static std::wstring PackIconFor(const App& a) {
    // ícone escolhido pelo usuário (acima) vem antes de tudo
    if (std::wstring own = apps::CustomIcon(a.finder ? L"Finder" : a.name, a.finder ? L"" : (a.exe.empty() ? a.id : a.exe)); !own.empty()) return own;
    static std::map<std::wstring, std::wstring> pack;  // nome normalizado -> arquivo
    static int loaded = -1;  // tema com que a lista foi montada (Tahoe usa icones\tahoe antes do Sequoia)
    int tema = liquid::Leopard() ? 3 : liquid::Golden() ? 2 : liquid::On() ? 1 : 0;  // Golden Gate usa icones\goldengate antes do Tahoe; Leopard, icones\leopard antes do Sequoia
    if (loaded != tema) {
        loaded = tema;
        pack.clear();
        // icones\extra (ícones escolhidos pelo usuário, ex.: baixados do macOSicons.com) tem prioridade
        // sobre o pacote do Sequoia; o nome do arquivo é o nome do app ("Discord.png", "Steam.png").
        // Tema Tahoe: icones\tahoe (tools\baixar-icones.ps1) vem antes do Sequoia; o que faltar, segue o Sequoia
        std::wstring root = shell::dir.substr(0, shell::dir.find_last_of(L'\\')) + L"\\icones";
        // Leopard: icones\leopard vem ANTES do extra (o extra tem os ícones modernos; o Leopard tem a versão Aqua deles)
        for (const wchar_t* sub : { loaded == 3 ? L"\\leopard" : L"", L"\\extra", loaded == 2 ? L"\\goldengate" : L"", loaded == 1 || loaded == 2 ? L"\\tahoe" : L"", L"\\sequoia" }) {
            if (!*sub) continue;
            std::wstring dir = root + sub;
            if (std::wstring custom = shell::Setting(L"icones", L""); !custom.empty() && !wcscmp(sub, L"\\sequoia")) dir = custom;
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((dir + L"\\*.png").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                std::wstring n = fd.cFileName; n.resize(n.size() - 4);
                pack.emplace(Norm(n), dir + L"\\" + fd.cFileName);  // emplace: o primeiro (extra) vence
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    if (pack.empty()) return L"";
    if (a.finder) return pack.count(L"finder") ? pack[L"finder"] : L"";
    static const std::map<std::wstring, std::wstring> alias = {
        { L"msedge", L"edge" }, { L"code", L"vscode" }, { L"winword", L"word" }, { L"powerpnt", L"powerpoint" },
        { L"chrome", L"googlechrome" }, { L"notepad", L"textedit" }, { L"mspaint", L"paint" }, { L"calculatorapp", L"calculator" }, { L"calculadora", L"calculator" },
        { L"snippingtool", L"screenshot" }, { L"screensketch", L"screenshot" }, { L"ferramentadecaptura", L"screenshot" },  // a Ferramenta de Captura com o ícone da Captura de Tela
        { L"windowsterminal", L"terminal" }, { L"wt", L"terminal" }, { L"taskmgr", L"activitymonitor" }, { L"monitoratividade", L"activitymonitor" }, { L"monitordeatividade", L"activitymonitor" }, { L"notas", L"notes" }, { L"sequoiacalculator", L"calculator" }, { L"sequoiaterminal", L"terminal" }, { L"sequoiaactivitymonitor", L"activitymonitor" }, { L"sequoianotes", L"notes" }, { L"sequoiasettings", L"settings" }, { L"systemsettings", L"settings" }, { L"ajustes", L"settings" }, { L"ajustesdosistema", L"settings" },
        { L"mbam", L"malwarebytes" }, { L"steamwebhelper", L"steam" }, { L"ms-teams", L"teams" }, { L"olk", L"outlook" },
        { L"photoshop", L"photoshop" }, { L"illustrator", L"illustrator" }, { L"sublime_text", L"sublimetext" },
    };
    std::wstring exeBase = a.exe.substr(a.exe.find_last_of(L'\\') + 1);
    if (exeBase.size() > 4) exeBase.resize(exeBase.size() - 4);
    // issue #84: navegadores feitos sobre o Chromium (Helium, Chromium, Ungoogled...) também se chamam chrome.exe e
    // ganhavam o ícone do Google Chrome. O chrome.exe só vale como Chrome dentro de uma pasta do Google.
    if (Norm(exeBase) == L"chrome" && Lower(a.exe).find(L"\\google\\") == std::wstring::npos) exeBase.clear();
    for (const std::wstring& k : { Norm(a.name), Norm(exeBase) }) {
        if (k.empty()) continue;
        auto it = pack.find(k);
        if (it != pack.end()) return it->second;
        auto al = alias.find(k);
        if (al != alias.end() && pack.count(al->second)) return pack[al->second];
    }
    return L"";
}

std::wstring apps::PackIconByName(const std::wstring& name, const std::wstring& exe) {
    App a; a.name = name; a.exe = exe;
    return PackIconFor(a);
}

void apps::EnsureIcon(App& a, int px) {
    if (!a.icon.empty() && (a.mac || (int)std::max(a.iw, a.ih) >= px)) return;
    std::wstring png = PackIconFor(a);
    if (!png.empty() && ui::LoadPng(png, a.iw, a.ih, a.icon)) { a.mac = true; return; }
    if (a.finder) return;
    if (!ui::ShellImage(a.launch, px, a.iw, a.ih, a.icon, true))
        if (!ui::ShellImage(a.launch, px, a.iw, a.ih, a.icon, false) && !a.exe.empty())
            ui::ShellImage(a.exe, px, a.iw, a.ih, a.icon, true);
    if (!a.icon.empty()) ui::TrimToSquare(a.iw, a.ih, a.icon);
}

// Apps feitos com Chromium/Electron param de desenhar quando estão cobertos por outra janela, e a
// miniatura do Mission Control fica preta. Abertos por aqui, eles recebem a opção que desliga isso
// (não muda nenhum atalho; vale enquanto o app estiver aberto). Ajuste "chromium_sem_oclusao".
static const wchar_t* NO_OCCLUSION = L"--disable-features=CalculateNativeWinOcclusion";

static bool IsChromium(const std::wstring& exe) {
    std::wstring base = exe.substr(exe.find_last_of(L'\\') + 1);
    for (auto& ch : base) ch = towlower(ch);
    for (auto n : { L"chrome.exe", L"msedge.exe", L"brave.exe", L"vivaldi.exe", L"opera.exe", L"arc.exe", L"thorium.exe" })
        if (base == n) return true;
    std::wstring dir = exe.substr(0, exe.find_last_of(L'\\'));
    return GetFileAttributesW((dir + L"\\chrome_100_percent.pak").c_str()) != INVALID_FILE_ATTRIBUTES ||  // Electron
           GetFileAttributesW((dir + L"\\resources\\app.asar").c_str()) != INVALID_FILE_ATTRIBUTES;
}

// "{GUID-de-pasta-conhecida}\resto\app.exe" (nome do shell:AppsFolder de apps de desktop) → caminho real
static std::wstring ExpandKnownFolder(const std::wstring& p) {
    if (p.size() < 40 || p[0] != L'{') return p;
    size_t close = p.find(L'}');
    GUID g;
    if (close == std::wstring::npos || FAILED(CLSIDFromString(p.substr(0, close + 1).c_str(), &g))) return p;
    PWSTR base = nullptr;
    if (FAILED(SHGetKnownFolderPath(g, 0, nullptr, &base))) return p;
    std::wstring r = std::wstring(base) + p.substr(close + 1);
    CoTaskMemFree(base);
    return r;
}

// O ativador segura quem chama até o app terminar de abrir (a Segurança do Windows leva vários segundos):
// numa thread separada, senão a interface do Shell para e o vigia o reinicia achando que travou.
bool apps::ActivatePackaged(const std::wstring& target, const std::wstring& args) {
    if (target.rfind(L"shell:AppsFolder\\", 0) != 0) return false;
    std::wstring id = target.substr(17);
    if (id.find(L'!') == std::wstring::npos) return false;  // não é empacotado
    // App da Loja antigo (UWP) sem o Explorador: o ativador fica esperando para sempre (a janela dele vem de
    // uma peça do explorer.exe). Sem resposta em 2,5 s, o Shell abre o Explorador escondido para ele.
    auto done = std::make_shared<std::atomic<bool>>(false);
    if (mesa::Standalone())
        std::thread([id, done] {
            Sleep(2500);
            if (!*done && mesa::Standalone()) mesa::NeedExplorer(id);
        }).detach();
    std::thread([id, args, done] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DWORD t0 = GetTickCount();
        IApplicationActivationManager* am = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&am));
        DWORD pid = 0;
        if (SUCCEEDED(hr)) {
            hr = am->ActivateApplication(id.c_str(), args.empty() ? nullptr : args.c_str(), AO_NONE, &pid);
            am->Release();
        }
        *done = true;
        if (FAILED(hr)) Log(L"app empacotado não abriu pelo ativador (0x%08X, %lu ms): %ls", (unsigned)hr, GetTickCount() - t0, id.c_str());
        else Log(L"app empacotado aberto em %lu ms (pid %lu): %ls", GetTickCount() - t0, pid, id.c_str());
        CoUninitialize();
    }).detach();
    return true;
}

bool apps::LaunchNoOcclusion(const std::wstring& target, const std::wstring& args, const std::wstring& exeHint) {
    if (shell::Setting(L"chromium_sem_oclusao", L"1") != L"1") return false;
    std::wstring exe, cmdArgs, dir;
    std::wstring t = target;
    std::wstring hint = exeHint;
    if (hint.empty())  // Launchpad: o .exe das janelas, se o app já apareceu no Dock
        for (auto& a : list) if (a.launch == target && !a.exe.empty()) { hint = a.exe; break; }
    if (t.rfind(L"shell:AppsFolder\\", 0) == 0) {
        std::wstring id = t.substr(17);
        if (id.find(L'!') != std::wstring::npos) {  // app empacotado (AUMID): o .exe não abre direto, só pelo ativador
            if (hint.empty() || !IsChromium(hint)) return false;
            IApplicationActivationManager* am = nullptr;
            if (FAILED(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&am)))) return false;
            std::wstring all = std::wstring(NO_OCCLUSION) + (args.empty() ? L"" : L" " + args);
            DWORD pid = 0;
            HRESULT hr = am->ActivateApplication(id.c_str(), all.c_str(), AO_NONE, &pid);
            am->Release();
            if (SUCCEEDED(hr)) Log(L"abrindo sem oclusão (miniatura viva no Mission Control): %ls", id.c_str());
            return SUCCEEDED(hr);
        }
        t = ExpandKnownFolder(id);
        if (t.find(L'\\') == std::wstring::npos) {
            // ID de app de desktop ("Chrome", "com.squirrel.Discord.Discord"): o item do shell sabe o atalho por trás
            IShellItem2* si = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(target.c_str(), nullptr, IID_PPV_ARGS(&si)))) {
                PWSTR p = nullptr, a = nullptr;
                if (SUCCEEDED(si->GetString(PKEY_Link_TargetParsingPath, &p)) && p) { exe = p; CoTaskMemFree(p); }
                if (SUCCEEDED(si->GetString(PKEY_Link_Arguments, &a)) && a) { cmdArgs = a; CoTaskMemFree(a); }
                si->Release();
            }
        }
    }
    std::wstring low = t;
    for (auto& ch : low) ch = towlower(ch);
    if (low.size() > 4 && low.compare(low.size() - 4, 4, L".lnk") == 0) {
        IShellLinkW* sl = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
            IPersistFile* pf = nullptr;
            if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf))) && SUCCEEDED(pf->Load(t.c_str(), STGM_READ))) {
                wchar_t p[MAX_PATH] = {}, a[1024] = {}, w[MAX_PATH] = {};
                sl->GetPath(p, MAX_PATH, nullptr, 0);
                sl->GetArguments(a, 1024);
                sl->GetWorkingDirectory(w, MAX_PATH);
                exe = p; cmdArgs = a; dir = w;
            }
            if (pf) pf->Release();
            sl->Release();
        }
    } else if (low.size() > 4 && low.compare(low.size() - 4, 4, L".exe") == 0) exe = t;
    else if (exe.empty() && !hint.empty()) exe = hint;  // sem caminho: usa o .exe das janelas do app
    if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    std::wstring base = exe.substr(exe.find_last_of(L'\\') + 1);
    for (auto& ch : base) ch = towlower(ch);
    if (base != L"update.exe" && !IsChromium(exe)) {
        // lançador do Squirrel (Discord, Slack...): o app de verdade está em app-x.y.z\ ao lado do Update.exe
        std::wstring dir2 = exe.substr(0, exe.find_last_of(L'\\'));
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir2 + L"\\app-*").c_str(), &fd);
        bool electron = false;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (IsChromium(dir2 + L"\\" + fd.cFileName + L"\\" + exe.substr(exe.find_last_of(L'\\') + 1))) electron = true;
            } while (!electron && FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (electron && GetFileAttributesW((dir2 + L"\\Update.exe").c_str()) != INVALID_FILE_ATTRIBUTES) {
            cmdArgs = L"--processStart " + exe.substr(exe.find_last_of(L'\\') + 1);
            exe = dir2 + L"\\Update.exe";
            base = L"update.exe";
        }
    }
    std::wstring all;
    if (base == L"update.exe" && cmdArgs.find(L"--processStart") != std::wstring::npos) {
        // Discord e outros apps Squirrel: o Update.exe repassa os argumentos ao app
        all = cmdArgs + L" --process-start-args \"" + NO_OCCLUSION + L"\"";
    } else {
        if (!IsChromium(exe)) return false;
        all = cmdArgs + (cmdArgs.empty() ? L"" : L" ") + NO_OCCLUSION;
    }
    if (!args.empty()) all += L" " + args;
    HINSTANCE r = ShellExecuteW(nullptr, L"open", exe.c_str(), all.c_str(), dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) return false;
    Log(L"abrindo sem oclusão (miniatura viva no Mission Control): %ls %ls", exe.c_str(), all.c_str());
    return true;
}

// App da Loja guardado pelo caminho do .exe (fixado enquanto estava aberto): o Windows não deixa abrir o
// .exe de dentro de WindowsApps direto (o Spotify não abria). A identidade do pacote sai do nome da pasta
// (Nome_Versão_Arq__Editor -> Nome_Editor) e o Id do app, do AppxManifest.xml (o <Application> deste .exe).
static std::wstring PackagedAumid(const std::wstring& path) {
    std::wstring low = Lower(path);
    size_t w = low.find(L"\\windowsapps\\");
    if (w == std::wstring::npos) return L"";
    size_t f0 = w + 13, f1 = path.find(L'\\', f0);
    if (f1 == std::wstring::npos) return L"";
    std::wstring folder = path.substr(f0, f1 - f0);
    size_t u = folder.find(L'_'), pub = folder.rfind(L"__");
    if (u == std::wstring::npos || pub == std::wstring::npos || pub <= u) return L"";
    std::wstring pfn = folder.substr(0, u) + L"_" + folder.substr(pub + 2);
    std::wstring exe = Lower(path.substr(path.find_last_of(L'\\') + 1)), appId;
    HANDLE fh = CreateFileW((path.substr(0, f1) + L"\\AppxManifest.xml").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (fh != INVALID_HANDLE_VALUE) {
        DWORD size = GetFileSize(fh, nullptr), rd = 0;
        if (size == INVALID_FILE_SIZE || size > 16 * 1024 * 1024) size = 0;  // não lido: nada de string de 4 GB
        std::string x(size, 0);
        if (!ReadFile(fh, x.data(), size, &rd, nullptr)) rd = 0;
        x.resize(rd);
        CloseHandle(fh);
        std::string lx = x; for (auto& c : lx) c = (char)tolower((unsigned char)c);
        std::string want(exe.begin(), exe.end());
        for (size_t at = lx.find("<application "); at != std::string::npos; at = lx.find("<application ", at + 1)) {
            size_t end = lx.find('>', at);
            std::string tag = lx.substr(at, end - at);
            size_t i = tag.find(" id=\"");
            if (i == std::string::npos) continue;
            std::string id = x.substr(at + i + 5, x.find('"', at + i + 5) - (at + i + 5));
            if (appId.empty() || tag.find(want) != std::string::npos) appId = std::wstring(id.begin(), id.end());
            if (tag.find(want) != std::string::npos) break;
        }
    }
    if (appId.empty()) appId = L"App";
    return pfn + L"!" + appId;
}

// ---- apps que se atualizam numa pasta nova (Discord e outros do Squirrel: ...\Discord\app-1.0.9268\Discord.exe;
// alguns guardam a versão em ...\Application\131.0.6778.86\...). O Dock guardava o caminho da versão em que o app
// foi fixado e, depois de atualizar, abria a versão velha (que o atualizador às vezes ainda deixa na pasta) ou
// nada. Agora, ao abrir e ao carregar o Dock, cada pedaço do caminho com número de versão é trocado pela pasta
// irmã mais nova que tenha o mesmo resto do caminho; o dock.txt é regravado com o caminho novo.

// o nome com a versão trocada por '#': "app-1.0.9268" -> "app-#", "claude_5.11492.5.0_x64__fnn" -> "claude_#_x64__fnn"
static std::wstring VerKey(const std::wstring& name) {
    std::wstring r;
    size_t i = 0;
    while (i < name.size()) {
        if (iswdigit(name[i])) {
            size_t j = i; bool dot = false;
            // só conta o ponto seguido de número ("1.0.9"), não o da extensão ("cubase15.exe")
            while (j < name.size() && (iswdigit(name[j]) || (name[j] == L'.' && j + 1 < name.size() && iswdigit(name[j + 1])))) { if (name[j] == L'.') dot = true; j++; }
            if (dot) { r += L'#'; i = j; continue; }
            r.append(name, i, j - i); i = j; continue;
        }
        r += (wchar_t)towlower(name[i++]);
    }
    return r;
}

static std::vector<unsigned> VerNums(const std::wstring& name) {
    std::vector<unsigned> v; unsigned cur = 0; bool in = false;
    for (wchar_t c : name) {
        if (iswdigit(c)) { cur = cur * 10 + (c - L'0'); in = true; }
        else { if (in) v.push_back(cur); cur = 0; in = false; }
    }
    if (in) v.push_back(cur);
    return v;
}

static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// o mesmo caminho na versão mais nova instalada (ou o próprio caminho, se não houver outra)
static std::wstring NewestVersion(const std::wstring& path) {
    if (path.size() < 4 || path[1] != L':') return path;  // só caminhos de disco (não shell:, não aumid)
    std::vector<std::wstring> parts;
    size_t a = 0, b;
    while ((b = path.find(L'\\', a)) != std::wstring::npos) { parts.push_back(path.substr(a, b - a)); a = b + 1; }
    parts.push_back(path.substr(a));
    std::wstring prefix = parts[0];
    for (size_t i = 1; i + 1 < parts.size(); i++) {
        const std::wstring& comp = parts[i];
        std::wstring key = VerKey(comp);
        if (key.find(L'#') != std::wstring::npos) {
            std::wstring rest;
            for (size_t k = i + 1; k < parts.size(); k++) rest += L"\\" + parts[k];
            std::wstring best = comp; std::vector<unsigned> bestV = Exists(prefix + L"\\" + comp + rest) ? VerNums(comp) : std::vector<unsigned>{};
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((prefix + L"\\*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
                    if (VerKey(fd.cFileName) != key) continue;
                    std::vector<unsigned> v = VerNums(fd.cFileName);
                    if (v > bestV && Exists(prefix + L"\\" + fd.cFileName + rest)) { best = fd.cFileName; bestV = v; }
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            parts[i] = best;
        }
        prefix += L"\\" + parts[i];
    }
    std::wstring r = prefix + L"\\" + parts.back();
    return Exists(r) ? r : path;
}

// troca o caminho velho pelo da versão nova (no exe, no jeito de abrir e no id, se o id era o caminho)
// Os nossos apps fixados (Ajustes, Notas...) apontam para o programa da pasta de onde o Shell roda, com o nome de
// agora: o nome velho (SequoiaSettings.exe) não existe mais depois da troca para WacOS, e outra pasta (a de
// desenvolvimento) não recebe as atualizações do GitHub (17/11/2026: o Ajustes fixado não abria pelo Dock)
static std::wstring OurApp(const std::wstring& path) {
    std::wstring base = Lower(path.substr(path.find_last_of(L'\\') + 1));
    size_t n = base.rfind(L"sequoia", 0) == 0 ? 7 : base.rfind(L"wacos", 0) == 0 ? 5 : 0;
    if (!n || base.size() < 5 || base.compare(base.size() - 4, 4, L".exe")) return path;
    std::wstring mine = shell::dir + L"\\WacOS" + base.substr(n);
    if (GetFileAttributesW(mine.c_str()) == INVALID_FILE_ATTRIBUTES) return path;
    return Lower(mine) == Lower(path) ? path : mine;
}

static bool Heal(App& a) {
    if (a.finder) return false;
    std::wstring exe = a.exe.empty() ? L"" : OurApp(NewestVersion(a.exe));
    bool launchIsPath = a.launch.size() > 4 && a.launch[1] == L':' && Lower(a.launch.substr(a.launch.size() - 4)) == L".exe";
    std::wstring launch = launchIsPath ? OurApp(NewestVersion(a.launch)) : a.launch;
    if (Lower(exe) == Lower(a.exe) && launch == a.launch) return false;
    Log(L"dock: %ls atualizou: %ls -> %ls", a.name.c_str(), (launchIsPath ? a.launch : a.exe).c_str(), (launchIsPath ? launch : exe).c_str());
    if (!a.id.empty() && a.id == Lower(a.exe)) a.id = Lower(exe);
    a.exe = Lower(exe);
    a.launch = launch;
    return true;
}

// Abrir um app mexe com o disco (versão nova do .exe, manifesto do pacote, atalho .lnk, Electron ou não) e com o
// ativador: tudo numa thread com uma cópia do app (issue #32: na thread da interface cada passo podia esperar o disco).
static void LaunchNow(App a, const std::wstring& args);
void apps::Launch(App& a, const std::wstring& args) {
    a.launchedAt = a.abertoEm = GetTickCount();
    AllowSetForegroundWindow(ASFW_ANY);  // o app aberto pela thread vem para a frente
    std::thread([c = a, args] {
        HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        LaunchNow(c, args);
        if (SUCCEEDED(ci)) CoUninitialize();
    }).detach();
}
static void LaunchNow(App a, const std::wstring& args) {
    if (a.pinned) {
        std::wstring id0 = a.id;
        if (Heal(a)) shell::NaInterface([id0, id = a.id, exe = a.exe, launch = a.launch] {
            for (auto& x : apps::list) if (x.id == id0) { x.id = id; x.exe = exe; x.launch = launch; apps::Save(); break; }
        });
    }
    for (const std::wstring* c : { &a.launch, &a.exe }) {
        std::wstring aumid = PackagedAumid(*c);
        if (aumid.empty()) continue;
        std::wstring target = L"shell:AppsFolder\\" + aumid;
        // Spotify e outros feitos sobre o Chromium: abertos sem a "oclusão" (senão param de desenhar quando a
        // janela nasce escondida para a animação de abrir e ficam parados)
        if (apps::LaunchNoOcclusion(target, args, a.exe.empty() ? *c : a.exe) || apps::ActivatePackaged(target, args)) {
            Log(L"dock: abrindo %ls (pacote %ls)", a.name.c_str(), aumid.c_str());
            return;
        }
    }
    if (shell::OpenCalculator(a.id + L"|" + a.launch + L"|" + a.exe)) return;  // a Calculadora do Windows fixada: abre a nossa
    if (apps::LaunchNoOcclusion(a.launch, args, a.exe)) return;
    std::wstring lower = a.launch + L"|" + a.exe;
    for (auto& ch : lower) ch = towlower(ch);
    if (lower.find(L"immersivecontrolpanel") != std::wstring::npos || lower.find(L"systemsettings.exe") != std::wstring::npos) {
        shell::OpenSettings(L"");  // Ajustes do Windows fixado no Dock: abre o nosso
        return;
    }
    if (apps::ActivatePackaged(a.launch, args)) { Log(L"dock: abrindo %ls", a.name.c_str()); return; }  // empacotado: sem segurar a interface
    ShellExecuteW(nullptr, L"open", a.launch.c_str(), args.empty() ? nullptr : args.c_str(), nullptr, SW_SHOWNORMAL);  // já numa thread
    Log(L"dock: abrindo %ls", a.name.c_str());
}

static void ForceForeground(HWND h) {
    if (SetForegroundWindow(h)) return;
    // o clique foi para o nosso processo, então costuma funcionar; senão, o truque do Alt
    shell::LiberaFrente();
    SetForegroundWindow(h);
}

// Ocultar: por baixo é minimizar (o Windows não deixa esconder janela de outro programa com segurança),
// mas o Shell marca as janelas: sem o gênio, sem a miniatura no Dock, e o clique no ícone traz todas.
static std::map<HWND, DWORD> g_hiddenWins;  // janela -> quando foi oculta

bool apps::IsHidden(HWND h) {
    auto it = g_hiddenWins.find(h);
    if (it == g_hiddenWins.end()) return false;
    if (GetTickCount() - it->second < 1500) return true;  // ainda minimizando
    if (!IsWindow(h) || !IsIconic(h)) { g_hiddenWins.erase(it); return false; }  // voltou por outro caminho (Alt+Tab)
    return true;
}

void apps::Activate(App& a) {
    if (a.wins.empty()) { Launch(a); return; }
    {   // estava oculto: todas as janelas voltam, na ordem em que estavam
        std::vector<HWND> back;
        for (auto& w : a.wins) if (IsHidden(w.hwnd)) back.push_back(w.hwnd);
        if (!back.empty()) {
            for (int i = (int)back.size() - 1; i >= 1; i--) { g_hiddenWins.erase(back[i]); ShowWindowAsync(back[i], SW_SHOWNOACTIVATE); }
            g_hiddenWins.erase(back[0]);
            // a da frente sai do ícone do app pelo gênio, como entrou (issue #29)
            RECT de = dock::AppIconTarget(back[0]);
            if (de.right > de.left && genio::Restore(back[0], de)) { Log(L"dock: %ls volta pelo gênio (%zu janelas)", a.name.c_str(), back.size()); return; }
            ShowWindowAsync(back[0], SW_SHOWNOACTIVATE);
            ForceForeground(back[0]);
            Log(L"dock: %ls volta (%zu janelas)", a.name.c_str(), back.size());
            return;
        }
    }
    bool anyShown = false;
    for (auto& w : a.wins) if (!w.minimized) anyShown = true;
    if (!anyShown) {  // tudo minimizado: volta a mais recente
        shell::Restaurar(a.wins[0].hwnd);
        ForceForeground(a.wins[0].hwnd);
        return;
    }
    for (int i = (int)a.wins.size() - 1; i >= 0; i--)  // de trás para frente: a ordem entre elas se mantém
        if (!a.wins[i].minimized) SetWindowPos(a.wins[i].hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    for (auto& w : a.wins) if (!w.minimized) { ForceForeground(w.hwnd); break; }
}

static HWND g_ocultaGenio;  // a janela da frente do app que está sendo ocultado: só ela entra no ícone pelo gênio
bool apps::OcultaComGenio(HWND h) { return h && h == g_ocultaGenio; }
void apps::Hide(App& a) {
    DWORD now = GetTickCount();
    std::vector<HWND> hs;
    for (auto& w : a.wins) if (!w.minimized && !IsIconic(w.hwnd)) { g_hiddenWins[w.hwnd] = now; hs.push_back(w.hwnd); }
    g_ocultaGenio = nullptr;
    {   // a de cima na ordem da tela
        for (HWND z = GetTopWindow(nullptr); z && !g_ocultaGenio; z = GetWindow(z, GW_HWNDNEXT))
            if (std::find(hs.begin(), hs.end(), z) != hs.end()) g_ocultaGenio = z;
    }
    // assíncrono: o ShowWindow numa janela de outro programa espera a thread dela (app ocupado = Shell parado, issue #32)
    for (HWND h : hs) ShowWindowAsync(h, SW_MINIMIZE);  // somem na hora (a animação do Windows está desligada)
    // a frente passa para a janela de cima de outro app, como no Mac. Minimizada por fora, a janela continuava sendo a
    // "da frente" para o Windows (medido: minimizada e ainda em primeiro plano), com o teclado num app que sumiu.
    struct Ctx { const std::vector<HWND>* hs; HWND next; } cx{ &hs, nullptr };
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        auto* c = (Ctx*)l;
        if (std::find(c->hs->begin(), c->hs->end(), h) != c->hs->end() || IsIconic(h) || !IsTaskbarWindow(h)) return TRUE;
        c->next = h;
        return FALSE;
    }, (LPARAM)&cx);
    if (cx.next) SetForegroundWindow(cx.next);
    Log(L"dock: %ls oculto (%zu janelas)", a.name.c_str(), hs.size());
}
// Encerrar = como o "Finalizar tarefa": mata os processos das janelas do app na hora, sem perguntar.
// Apps da loja vivem dentro do ApplicationFrameHost (que hospeda todos eles): ali vale o processo da
// janela de dentro, nunca o hospedeiro. O Arquivos (o Finder) e o Explorer só recebem o pedido de fechar.
void apps::Quit(App& a) {
    std::vector<DWORD> pids;
    for (auto& w : a.wins) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w.hwnd, &pid);
        std::wstring exe = ExeOfWindow(w.hwnd);
        exe = exe.substr(exe.find_last_of(L'\\') + 1);  // só o nome do arquivo, minúsculo e sem ".exe"
        for (auto& c : exe) c = towlower(c);
        if (exe.size() > 4 && exe.compare(exe.size() - 4, 4, L".exe") == 0) exe.resize(exe.size() - 4);
        if (exe == L"applicationframehost") {
            struct Find { DWORD host, inner; } f{ pid, 0 };
            EnumChildWindows(w.hwnd, [](HWND c, LPARAM l) -> BOOL {
                auto* f = (Find*)l;
                DWORD p = 0; GetWindowThreadProcessId(c, &p);
                if (p && p != f->host) { f->inner = p; return FALSE; }
                return TRUE;
            }, (LPARAM)&f);
            pid = f.inner;
        }
        if (a.finder || !pid || pid == GetCurrentProcessId() || exe == L"explorer" || exe == L"shell" || exe == L"sequoiashell") { PostMessageW(w.hwnd, WM_CLOSE, 0, 0); continue; }
        if (std::find(pids.begin(), pids.end(), pid) == pids.end()) pids.push_back(pid);
    }
    for (DWORD pid : pids) {
        if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) {
            TerminateProcess(h, 1);
            CloseHandle(h);
        }
    }
    Log(L"dock: encerrado %ls (%zu processos)", a.name.c_str(), pids.size());
}

void apps::SetPinned(App& a, bool pin) {
    a.pinned = pin;
    if (!pin && a.wins.empty()) {
        std::wstring id = a.id;
        list.erase(std::remove_if(list.begin(), list.end(), [&](const App& x) { return x.id == id; }), list.end());
    }
    std::stable_partition(list.begin(), list.end(), [](const App& x) { return x.pinned; });
    Save();
}

// Arrastar atalhos e programas para o Dock (Reddit, dougnite, 21/11/2026: "tenho que abrir cada programa para fixar")
bool apps::IsProgramFile(const std::wstring& path) {
    std::wstring l = Lower(path);
    for (const wchar_t* e : { L".lnk", L".exe", L".url", L".appref-ms" })
        if (l.size() > wcslen(e) && l.compare(l.size() - wcslen(e), wcslen(e), e) == 0) return true;
    return false;
}

bool apps::PinPath(const std::wstring& path, int at) {
    if (!IsProgramFile(path)) return false;
    std::wstring low = Lower(path), target, aumid;
    bool lnk = low.size() > 4 && low.compare(low.size() - 4, 4, L".lnk") == 0;
    bool exe = low.size() > 4 && low.compare(low.size() - 4, 4, L".exe") == 0;
    if (lnk) ReadLink(path, target, aumid);
    else if (exe) target = path;
    std::wstring t = Lower(target);
    // um atalho para uma pasta ou um documento não é app (o Finder abre pastas; documentos vão por "abrir com")
    if (lnk && t.empty() && aumid.empty()) return false;
    if (lnk && !t.empty() && (GetFileAttributesW(target.c_str()) & FILE_ATTRIBUTE_DIRECTORY) && GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    if (t.size() >= 12 && t.substr(t.size() - 12) == L"explorer.exe") return false;  // o Finder já está no Dock
    std::wstring id = !aumid.empty() ? L"aumid:" + Lower(aumid) : (!t.empty() ? t : low);
    // já está no Dock (fixo ou aberto): só fixa
    for (auto& a : list) {
        if (a.finder) continue;
        if (a.id == id || (!t.empty() && Lower(a.exe) == t)) { if (!a.pinned) SetPinned(a, true); return true; }
    }
    App x;
    x.id = id;
    x.exe = t;
    x.launch = path;
    std::wstring file = path.substr(path.find_last_of(L'\\') + 1);
    size_t dot = file.find_last_of(L'.');
    x.name = exe ? ExeName(path) : (dot != std::wstring::npos ? file.substr(0, dot) : file);
    x.pinned = true;
    int pinnedEnd = 0;  // depois do Finder e dos fixos
    for (int i = 0; i < (int)list.size(); i++) if (list[i].finder || list[i].pinned) pinnedEnd = i + 1;
    int pos = at < 1 || at > pinnedEnd ? pinnedEnd : at;
    list.insert(list.begin() + pos, x);
    Save();
    return true;
}

void apps::ForgetRecent(const std::wstring& id) {
    recent.erase(std::remove(recent.begin(), recent.end(), id), recent.end());
    list.erase(std::remove_if(list.begin(), list.end(), [&](const App& x) { return x.id == id && !x.pinned && x.wins.empty(); }), list.end());
}

void apps::Move(int from, int to) {
    if (from == to || from <= 0 || to <= 0 || from >= (int)list.size() || to >= (int)list.size()) return;  // o Arquivos não sai do lugar
    App a = list[from];
    list.erase(list.begin() + from);
    list.insert(list.begin() + to, a);
    Save();
}

static std::wstring StartupLink(const App& a) {
    PWSTR p = nullptr;
    std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &p))) { d = p; CoTaskMemFree(p); }
    std::wstring n = a.name;
    for (auto& c : n) if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    return d + L"\\" + n + L".lnk";
}

bool apps::OpenAtLogin(const App& a) { return GetFileAttributesW(StartupLink(a).c_str()) != INVALID_FILE_ATTRIBUTES; }

void apps::SetOpenAtLogin(const App& a, bool on) {
    std::wstring lnk = StartupLink(a);
    if (!on) { DeleteFileW(lnk.c_str()); return; }
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(a.launch.c_str(), nullptr, &pidl, 0, nullptr))) return;
    IShellLinkW* sl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
        sl->SetIDList(pidl);
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) { pf->Save(lnk.c_str(), TRUE); pf->Release(); }
        sl->Release();
    }
    CoTaskMemFree(pidl);
}

void apps::ShowInFinder(const App& a) {
    std::wstring target = a.exe;
    if (target.empty() && a.launch.size() > 4 && Lower(a.launch.substr(a.launch.size() - 4)) == L".lnk") {
        std::wstring aumid; ReadLink(a.launch, target, aumid);
    }
    if (target.empty()) target = a.launch;
    shell::Abrir(L"open", g_finderExe.c_str(), (L"\"" + target + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}
