// Shell no estilo do macOS Sequoia: barra de menus (topo), Dock (embaixo) e bandeja.
// Ver docs/dock-e-barra-de-menus.md e docs/estudo-bandeja.md.
#pragma once
#include "../comum/nomes.h"
#include "../comum/abrircom.h"  // áudio abre no Música (Finder, mesa, Spotlight, pilha)
#define NOMINMAX
#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include "../comum/idioma.h"
#include <cstdio>

struct IDXGISwapChain1;
struct IDCompositionTarget;
struct IDCompositionVisual;
struct IDCompositionDevice;
struct IDCompositionSurface;
struct IDCompositionAnimation;
struct ID3D11Device;

void Log(const wchar_t* fmt, ...);

// ---- ui.cpp: Direct2D em DIB de 32 bits (alfa preservado), acrílico, fontes, desenhos comuns.
// ---- anim.cpp: tempo de alta precisão e as curvas do macOS (para todas as animações do Shell)
namespace anim {
double Now();                                   // ms, QueryPerformanceCounter
double Tick();                                  // ms na base do GetTickCount, com fração (sem degraus)
float Bezier(float t, float x1, float y1, float x2, float y2);
float Ease(float t);                            // curva padrão do Core Animation (0.25, 0.1, 0.25, 1)
float EaseInOut(float t);                       // (0.42, 0, 0.58, 1)
float EaseOut(float t);                         // (0, 0, 0.58, 1)
float Spring(float tSec, float response = 0.55f, float damping = 0.825f);  // mola do SwiftUI, de 0 a 1
float SpringDuration(float response, float damping);
// A mola dos painéis do Shell (Spotlight, Central de Controle, menus de status, alertas...), a mesma em
// todos: abrindo, aparece na curva do Mac em 180 ms e cresce de 96% a 100% numa mola ágil (passa um
// tiquinho e assenta, ~0,5 s); fechando, o caminho de volta em 150 ms, acelerando.
struct Panel { float fade = 1, scale = 1; bool done = true; };
Panel PanelIn(double msSinceOpen);
Panel PanelOut(double msSinceClose);
Panel CCIn(double msSinceOpen);     // Central de Controle do Tahoe (medida no macOS 26)
// Ajustes > Acessibilidade > Reduzir movimento (reduzir_movimento=1), lido em LoadSettings: o que move ou escala
// vira corte seco ou fade curto (guia macOS26-Theme-Reference, seção 62); fades de saída continuam
extern bool reduceMotion;
Panel CCOut(double msSinceClose);
const double PANEL_OUT_MS = 150;
}

namespace ui {
extern ID2D1Factory* d2d;
extern IDWriteFactory* dw;
extern std::wstring family;       // SF Pro Text / Inter / Segoe UI
extern std::wstring iconFamily;   // Segoe Fluent Icons / MDL2
bool Init();
bool Dark();                      // tema escuro (ajuste "tema" ou o do Windows)
float Scale(HWND h);              // DPI / 96

// Superfície: desenhe em DIPs entre Begin e End.
struct Surface {
    HWND hwnd = nullptr;
    HDC mem = nullptr;
    HBITMAP dib = nullptr;
    void* bits = nullptr;
    int w = 0, h = 0;
    ID2D1DCRenderTarget* rt = nullptr;
    float scale = 1;
};
ID2D1RenderTarget* Begin(Surface& s, int w, int h, float scale);
// layered = UpdateLayeredWindow (alfa por pixel, partes vazias deixam o clique passar);
// senão BitBlt sobre o acrílico da janela.
void End(Surface& s, bool layered, POINT screenPos = { 0, 0 });
void Free(Surface& s);

// Superfície na GPU (Direct2D + DirectComposition, alfa pré-multiplicado): para o que anima.
// A janela precisa de WS_EX_NOREDIRECTIONBITMAP. Bitmaps criados com o contexto de Gpu() valem
// para todas as superfícies (o dispositivo é um só).
struct GpuSurface {
    HWND hwnd = nullptr;
    IDXGISwapChain1* swap = nullptr;
    IDCompositionTarget* target = nullptr;
    IDCompositionVisual* visual = nullptr;
    ID2D1Bitmap1* back = nullptr;
    int w = 0, h = 0;
};
ID2D1DeviceContext* Gpu();                 // contexto compartilhado (criado na primeira vez)
ID2D1DeviceContext* GpuBegin(GpuSurface& s, int w, int h, float scale);  // já limpo e com BeginDraw
void GpuEnd(GpuSurface& s);                       // EndDraw + Present (sem esperar a tela)
void GpuFree(GpuSurface& s);
void GpuSoltarEscondida(GpuSurface& s);  // painel de tela cheia: solta os buffers depois de 2 min escondido (pinte antes de mostrar)
// Desloca o conteúdo dentro da janela (px): a janela pode encolher/crescer sem recriar a imagem.
void GpuOffset(GpuSurface& s, float x, float y);
void GpuScale(GpuSurface& s, float scale, float cx, float cy);  // escala o desenho em volta de (cx, cy) px (animação dos painéis)
void GpuMotion(GpuSurface& s, float scale, float cx, float cy, float opacity);  // escala e opacidade (a mola dos painéis)
// a mesma coisa animada pelo compositor: a curva amostrada a cada passoMs (escala e opacidade), tocada pelo DWM no vsync
void GpuMotionAnim(GpuSurface& s, float cx, float cy, const std::vector<float>& escala, const std::vector<float>& opac, float passoMs);
// Desliza o conteúdo (px) de (fromX, fromY) até (toX, toY) em `ms`, com ease-out, pelo próprio compositor.
void GpuSlide(GpuSurface& s, float fromX, float fromY, float toX, float toY, float ms);
// Quadros no ritmo do monitor: enquanto `on`, manda `msg` para `hwnd` a cada atualização da tela.
void FrameClock(HWND hwnd, UINT msg, bool on);
void FrameAck(HWND hwnd, UINT msg);  // no começo do tratamento do quadro: o próximo só vem depois deste (sem fila)
// Compositor (DirectComposition) do dispositivo de Gpu(): para animações que o DWM faz sozinho.
IDCompositionDevice* Comp();
ID3D11Device* D3D();                       // o dispositivo D3D11 de Gpu()
// Superfície do compositor desenhada uma vez com Direct2D (pixels, 96 dpi). nullptr se falhar.
IDCompositionSurface* CompSurface(int w, int h, const std::function<void(ID2D1DeviceContext*)>& draw);
// Curva suave pelos pontos (igualmente espaçados em `durSec`): Hermite com inclinações de Catmull-Rom.
IDCompositionAnimation* CompCurve(const std::vector<float>& v, float durSec);

void Acrylic(HWND h, DWORD tintABGR);   // desfoque atrás da janela (SetWindowCompositionAttribute)
IDWriteTextFormat* Format(float size, DWRITE_FONT_WEIGHT w, bool icons = false,
                          DWRITE_TEXT_ALIGNMENT a = DWRITE_TEXT_ALIGNMENT_LEADING);
IDWriteTextFormat* FormatFace(const wchar_t* face, float size, DWRITE_FONT_WEIGHT w);  // família escolhida (Leopard: Lucida)
float TextWidth(IDWriteTextFormat* tf, const std::wstring& s);
ID2D1Bitmap* Bitmap(ID2D1RenderTarget* rt, UINT w, UINT h, const std::vector<BYTE>& bgra);
// Tahoe: Aparência > "Estilo de ícones e widgets" (estilo_icones): padrao | escuro | transparente | tingido.
// Devolve o estilo atual (só com o Liquid Glass; senão "padrao") e aplica nos pixels BGRA pré-multiplicados.
std::wstring IconStyle();
std::wstring IconStyleKey();  // estilo + cor de destaque (para os caches: trocar a cor refaz os ícones Tingidos)
D2D1_COLOR_F Accent();  // Ajustes > Aparência > Cor de destaque (destaque=), "multicor" = azul
void StyleIcon(UINT w, UINT h, std::vector<BYTE>& bgra, const std::wstring& style);

// Pixels BGRA pré-multiplicados de um item do shell (exe, atalho, pasta, shell:AppsFolder\...).
bool ShellImage(const std::wstring& parsingName, int px, UINT& w, UINT& h, std::vector<BYTE>& bgra, bool iconOnly);
// o mesmo pelos bytes do PIDL: itens cujo nome não volta a virar item (tarefas do Painel de Controle)
bool ShellImageId(const std::vector<BYTE>& idList, int px, UINT& w, UINT& h, std::vector<BYTE>& bgra);
// arquivo como no Finder do Mac: miniatura de verdade (foto, vídeo, PDF); sem ela, a folha do Mac com o tipo
// (comum\docicone.h). Pastas e programas: o ícone do Windows (o ícone do próprio app)
bool FileImage(const std::wstring& path, int px, UINT& w, UINT& h, std::vector<BYTE>& bgra);
void TrimToSquare(UINT& w, UINT& h, std::vector<BYTE>& bgra);
void Downscale(UINT& w, UINT& h, std::vector<BYTE>& bgra, UINT maxSide);  // reduz (média por área) se passar de maxSide
bool LoadPng(const std::wstring& path, UINT& w, UINT& h, std::vector<BYTE>& bgra);  // BGRA pré-multiplicado
// foto inteira (JPG, PNG, WebP, HEIC...) pelo WIC, girada pelo EXIF e reduzida até maxSide (o Quick Look)
bool LoadPhoto(const std::wstring& path, UINT maxSide, UINT& w, UINT& h, std::vector<BYTE>& bgra);
std::wstring PackIcon(const std::wstring& name);  // "icones\sequoia\<nome>.png" se existir, senão ""

void DrawFolder(ID2D1RenderTarget* rt, D2D1_RECT_F box, wchar_t glyph);   // pasta azul do Arquivos
void DrawTrash(ID2D1RenderTarget* rt, D2D1_RECT_F box, bool full);          // lixeira de tela
}

// ---- apps.cpp: apps do Dock (fixos + abertos), janelas, ícones, abrir/ativar/encerrar.
struct AppWin { HWND hwnd; std::wstring title; bool minimized; };
struct App {
    std::wstring id;        // "aumid:..." ou caminho do .exe em minúsculas
    std::wstring launch;    // .lnk, .exe ou shell:AppsFolder\AUMID
    std::wstring name;
    std::wstring exe;       // .exe de destino em minúsculas (casa janelas sem AUMID com o atalho fixo)
    bool pinned = false;
    bool finder = false;    // o Arquivos (sempre o primeiro)
    bool recent = false;    // fechado há pouco (seção de recentes)
    std::vector<AppWin> wins;
    UINT iw = 0, ih = 0;
    std::vector<BYTE> icon; // BGRA pré-multiplicado
    bool mac = false;       // ícone do pacote do Sequoia (já tem a margem do Mac: desenha no tamanho cheio)
    DWORD launchedAt = 0;   // pulando enquanto abre
    DWORD abertoEm = 0;     // quando foi aberto pelo Shell (não zera ao aparecer a janela; Dock, issue #64)
    DWORD attentionAt = 0;  // pediu atenção (janela piscando)
    DWORD lastActive = 0;
};
namespace apps {
extern std::vector<App> list;   // ordem do Dock: Arquivos, fixos, abertos/recentes
extern std::vector<std::wstring> recent;  // ids dos últimos apps fechados (até 3)
void Load(const std::wstring& finderExe);
void Save();
void Relabel();  // nomes dos nossos apps no idioma escolhido (Ajustes > Geral > Idioma)
bool Refresh();                 // relê as janelas; true se algo mudou
std::wstring IdOfWindow(HWND h);
App* FindById(const std::wstring& id);
App* OfWindow(HWND h);
void EnsureIcon(App& a, int px);
void Launch(App& a, const std::wstring& args = L"");
// Apps Chromium/Electron: abre com a opção que mantém a miniatura viva no Mission Control.
// false se o alvo não for um deles (quem chamou abre do jeito normal).
bool LaunchNoOcclusion(const std::wstring& target, const std::wstring& args = L"", const std::wstring& exeHint = L"");
// App empacotado ("shell:AppsFolder\<AUMID com !>": Segurança do Windows, ChatGPT, apps da Loja) pelo ativador
// do Windows. Sem o Explorador, abrir pelo item do shell dá "Classe não registrada". false = não é empacotado
// ou não abriu (quem chamou tenta o jeito comum).
bool ActivatePackaged(const std::wstring& target, const std::wstring& args = L"");
void Activate(App& a);          // traz todas as janelas para frente
void Hide(App& a);            // Ocultar do Mac: as janelas somem (sem gênio, sem ir para o Dock); clicar no ícone traz de volta
bool IsHidden(HWND h);
bool OcultaComGenio(HWND h);  // a janela da frente que o clique no ícone do Dock está ocultando (vai pelo gênio)        // janela oculta pelo Ocultar (minimizada por baixo, mas não aparece no Dock)
void Quit(App& a);
void SetPinned(App& a, bool pin);
// programa ou atalho (.lnk, .exe, .url, .appref-ms) solto no Dock: fica fixo antes do app de índice `at` (-1 = no fim
// dos fixos). Já no Dock: só fixa. false = não é programa
bool PinPath(const std::wstring& path, int at);
bool IsProgramFile(const std::wstring& path);
void Move(int from, int to);
void ForgetRecent(const std::wstring& id);  // tira um app da seção de recentes
bool OpenAtLogin(const App& a);
void SetOpenAtLogin(const App& a, bool on);
void ShowInFinder(const App& a);
bool IsAppWindow(HWND h);       // janela que apareceria na barra de tarefas (regras do Alt+Tab)
bool IsShellWindow(HWND h);     // nossas janelas (não entram no Dock nem na barra)
bool LoneOwned(HWND h);         // janela com dona escondida (aviso sozinho de um app): conta como janela do app
std::wstring PackIconByName(const std::wstring& name, const std::wstring& exe = L"");  // ícone do pacote do Sequoia
// Ícone próprio do app (Dock/Launchpad > Alterar Ícone…), em %APPDATA%\Shell\icones-dock: <exe>.png e <nome>.png
std::wstring CustomIconDir();                                              // com a barra no fim
std::wstring CustomIcon(const std::wstring& name, const std::wstring& exe);  // o PNG escolhido, ou ""
bool HasCustomIcon(const std::wstring& name, const std::wstring& exe);
// Nome próprio do app (pedido no Discord, 24/11/2026: "renomear apps"): %APPDATA%\Shell\nomes-apps.txt, uma linha
// "chave<TAB>nome" por chave (as mesmas do ícone próprio: o .exe e o nome original). Vale no Dock, no Launchpad e na
// barra de menus. Renomear abre os Ajustes com o campo de texto (o Shell não tem campo de digitar nos alertas).
std::wstring NomeVisivel(const std::wstring& name, const std::wstring& exe);
void Renomear(const std::wstring& name, const std::wstring& exe);
void ChooseIcon(const std::wstring& name, const std::wstring& exe);   // janela de Abrir (outra thread); avisa "ShellIconeMudou"
void RestoreIcon(const std::wstring& name, const std::wstring& exe);  // apaga o ícone próprio; avisa "ShellIconeMudou"
void ReloadIcons();  // Dock, Alt+Tab e Launchpad relêem os ícones (thread da interface)
extern std::vector<HWND> ours;
}

// ---- tray.cpp: Shell_TrayWnd próprio (ícones de status dos apps).
struct TrayIcon {
    HWND hwnd = nullptr;
    UINT uid = 0;
    GUID guid{};
    UINT callback = 0;
    UINT version = 0;
    HICON icon = nullptr;
    std::wstring tip;
    bool hidden = false;
};
namespace tray {
extern std::vector<TrayIcon> icons;
// modo nativo: os ícones chegam pela barra do Windows (MacBarra.dll, ITrayUI::HandleCopyData, na thread da
// barra); true = era da bandeja (res = resposta ao app)
bool DoExplorer(COPYDATASTRUCT* cd, LRESULT* res);
void Start(std::function<void()> changed);
void Stop();
void SemAvisoAoSair();  // reinício: o Shell novo avisa ao abrir; avisar também ao sair confundia o Lively
void Click(int index, UINT button, POINT screenPt);  // WM_LBUTTONUP / WM_RBUTTONUP
void Keepalive();                                    // mantém o nosso Shell_TrayWnd na frente
// O ícone "<app> está usando o microfone/a câmera" do próprio Windows (guid 7820AE82-...): não vai para a barra de
// menus (lá fica a bolinha laranja/verde, como no Mac) e é o sinal de uso; 1 microfone, 2 câmera (pelo texto)
bool IsPrivacy(const TrayIcon& t);
int PrivacyBits();
}

// ---- menubar.cpp / dock.cpp
namespace menubar {
bool Create(HINSTANCE hi);
void Update();          // app ativo mudou, relógio, ícones
HWND Hwnd();
int Height();           // px
void SetFullscreen(bool on, HMONITOR mon = nullptr);  // app em tela cheia na frente: some (só no monitor dele; nullptr = todos)
int HeightOn(HMONITOR mon);  // px da barra naquele monitor (cada monitor tem a sua barra)
int ReservedOn(HMONITOR mon);  // px que a barra ocupa no topo daquele monitor (0: secundário sem barra, barra_todos_monitores=0, ou barra embaixo)
bool Embaixo();         // barra_embaixo: a barra está na linha do Dock (Dock embaixo no monitor principal, fora do Leopard)
int ReservedBottom(HMONITOR mon);  // px que a linha do Dock ocupa embaixo com a barra nela (0 = barra em cima)
int TopInset();         // px da barra no topo do monitor principal (0 com ela embaixo): de onde descem os painéis
void Displays();        // monitores mudaram: uma barra em cada
void ApplyTheme();      // claro/escuro mudou
void SoundChanged(bool device);  // volume/mudo (ou, com device, a saída padrão) mudou, ou o menu Som abriu/fechou: redesenha
bool SoundIcon(RECT* screen);    // onde está o ícone de som (tela); false se não está na barra
}
namespace som {  // menu "Som" do ícone da barra e o aviso de volume das teclas (som.cpp)
bool Create(HINSTANCE hi);
void Toggle(RECT iconScreen);
bool IsOpen();
bool HudLit();                  // aviso do Tahoe na tela: o ícone da barra fica destacado
void Key(UINT vk, bool fine);   // VK_VOLUME_UP/DOWN/MUTE (fine: Shift+Alt, quartos de passo)
}
namespace menus {  // menus do Windows (#32768) de todos os programas no visual do Mac (menus.cpp)
void Start();
void Stop();
// "WacOSMenuVidro": um menu de contexto nosso pede o Liquid Glass atrás dele (true = a mensagem era essa; *res = resposta)
bool VidroMsg(UINT m, WPARAM w, LPARAM l, LRESULT* res);
}
namespace dialogs {  // janela de Abrir/Salvar trocada pelo Finder por fora (dialogs.cpp)
void Start();
void Stop();
std::wstring LastSaved();  // caminho escolhido no último Salvar (até 20 s atrás), para a confirmação de substituir
}
namespace janelas {  // barra de título do Mac nas janelas dos outros programas (janelas.cpp + MacJanela.dll)
void Start();
void Stop();          // devolve o visual do Windows e tira os ganchos (reiniciando: só tira os ganchos)
void Foreground(HWND h);
void ManterAoReiniciar();  // o próximo Stop é um reinício: as janelas ficam com a barra (o Shell novo assume)
}
namespace barras {  // barra de título do Mac por fora, sem nada dentro dos outros programas (modo nativo; barras.cpp)
void Start();
void Stop();
}
namespace sombras {  // sombra do Mac nas janelas que a MacJanela.dll não trata (sombras.cpp)
void Start();
void Stop();
}
namespace central {  // Central de Controle: painel embaixo do ícone da barra de menus (central.cpp)
bool Create(HINSTANCE hi);
void BrilhoPasso(int dir);  // teclas de brilho (F14/F15): -1/+1 de 16 passos, DDC/CI ou WMI, aviso na Ilha
bool SemBrilho();           // já se sabe que nenhuma tela aceita brilho (sem DDC/CI nem WMI): F14/F15 ficam para os outros programas
void Toggle();
bool IsOpen();
}
namespace missioncontrol {  // Win+Tab: todas as janelas e as Mesas (missioncontrol.cpp)
bool Create(HINSTANCE hi);
void Warm();  // prepara o fundo (papel de parede) antes da primeira abertura
void Toggle();
// Exposé do app (Ctrl+↓ no Mac; aqui Ctrl+Win+↓, "Mostrar Todas as Janelas" no Dock, Cantos Ativos): só as
// janelas do app `appId` (apps::App::id), espalhadas, sem a faixa de Mesas. De novo com o mesmo app: fecha.
void ToggleApp(const std::wstring& appId, const std::wstring& appName);
bool IsOpen();
}
namespace encaixe {  // encaixe.cpp: arrastar a janela até a borda = metade/quarto/preencher, com a prévia do Mac
void Start();
void Stop();         // o encaixe do Windows volta como estava
void Half(HWND h, bool right);  // Win+← / Win+→
}
namespace memoria {  // memoria.cpp: pressão de memória, soneca de apps (App Nap) e memória descartável, como no Mac
void Start();
void Stop();          // acorda todos os apps em soneca (devolve o estado de antes)
int Level();          // 0 normal, 1 alta, 2 crítica
bool Napping(DWORD pid);
}
namespace limites {  // limites.cpp: janelas voltam para dentro da área (não cruzam a barra de menus nem o Dock)
void Start();
void Stop();
void Maximizada(HWND h);  // a janela da frente mudou de lugar: maximizada por cima da barra/do Dock volta para a área
}
namespace pastas {  // pastas.cpp: janela de pasta do Explorador (Chrome "Mostrar na pasta", explorer /select) vira o Finder
void Start();
void Stop();
}
namespace indice {  // índice de arquivos próprio (indice.cpp): pasta do usuário e outros discos, salvo em indice.db
struct Hit { std::wstring path, name; bool dir; int score; };
void Start();
bool Ready();
size_t Count();
// foldedQuery: minúsculas sem acento; para logo se cancelled() der true
void Search(const std::wstring& foldedQuery, size_t max, const std::function<bool()>& cancelled, std::vector<Hit>& out);
}
namespace atualizacao {  // aviso de versão nova no GitHub (atualizacao.cpp)
void Start(HWND msgWnd);  // thread: confere 90 s depois e a cada 6 h; avisa msgWnd
bool Handle(UINT m);      // na MsgProc: mostra a notificação (uma vez por versão); true = era a mensagem
void Avisar(const std::wstring& title, const std::wstring& body);  // notificação do WacOS (vira o banner do Mac)
}
namespace migrar {  // troca de nome Sequoia → WacOS (migrar.cpp)
bool TrocarPeloNovo();  // rodando como SequoiaShell.exe com o WacOSShell.exe ao lado: abre o novo (true = sair)
void Registro();        // caminhos do nome velho no registro e a tarefa do login
}
namespace alertas {  // confirmações de excluir do Explorer no alerta do Mac (alertas.cpp)
bool Create(HINSTANCE hi);
void Stop();
// alerta do Mac para uma pergunta do próprio Shell; onYes roda se a resposta for o botão azul
// icon: "" = Lixeira, "pack:<nome>" = icones\sequoia\<nome>.png, senão o caminho de um programa
void Confirm(const std::wstring& title, const std::wstring& info, const std::wstring& yes, std::function<void()> onYes, const std::wstring& icon = L"");
}
namespace progresso {  // janela de progresso do Windows (copiar, mover, apagar...) trocada pela do Finder (progresso.cpp)
bool Create(HINSTANCE hi);
void Stop();
bool Owns(HWND h);  // janela do Windows que tiramos da tela (não entra no Dock)
}
namespace spotlight {  // Win+Espaço / lupa da barra de menus: busca de apps, ajustes e arquivos (spotlight.cpp)
bool Create(HINSTANCE hi);
void Toggle();
bool IsOpen();
void OpenMode(int mode);  // Tahoe: abre direto num modo (1 Aplicativos = o "Apps" do Dock, 2 Arquivos, 3 Ações); de novo fecha
void Rescan();            // relê a lista de apps (um programa foi instalado ou removido)
void ClickedDesktop();    // clique na mesa (gancho do revelar.cpp, outra thread): aberto, fecha
RECT Lugar();             // onde o painel abre agora (px): a Siri abre alinhada a ele
float LarguraCampo();     // a largura do campo de busca (DIP) no tema atual
}
namespace dashboard {  // Dashboard do Leopard (dashboard.cpp): Win+F12 / ShellAbrirPainel 16
bool Create(HINSTANCE hi);
void Toggle();
bool IsOpen();
}
namespace espacos {  // Spaces do Leopard (espacos.cpp): Win+F8 / ShellAbrirPainel 17
bool Create(HINSTANCE hi);
void Toggle();
bool IsOpen();
}
namespace tempo {  // a "viagem no tempo" do Time Machine do Leopard (tempo.cpp): app Time Machine / ShellAbrirPainel 18
bool Create(HINSTANCE hi);
void Enter();
bool IsOpen();
}
namespace sobre {  // "Sobre Este Mac" do menu da Apple (sobre.cpp)
bool Create(HINSTANCE hi);
void Show();
}
namespace genio {  // efeito gênio ao minimizar e ao restaurar pelo Dock (genio.cpp)
void Create(HINSTANCE hi);
void Stop();
bool Busy(HWND h);                 // a janela está no meio do gênio (o Dock ainda não mostra)
int Phase(HWND h);                 // 0 parada, 1 minimizando (indo para o Dock), 2 restaurando (saindo dele)
bool Restore(HWND h, RECT fromSlot);
bool OnMinRect(HWND h, RECT* out);  // HSHELL_GETMINRECT: começa o gênio antes da janela sumir
void Pause(DWORD ms);               // Win+D: minimiza tudo de uma vez, sem o gênio (pesado em várias janelas)
}
namespace seletor {  // Alt+Tab: o seletor de apps do Mac (seletor.cpp)
bool Create(HINSTANCE hi);
bool IsActive();
void Step(int dir);      // Tab (+1) / Shift+Tab (-1) com o Alt seguro
void Key(UINT vk);       // Q, H, Esc, setas enquanto aberto
void Release();          // soltou o Alt: troca para o escolhido
void IconsChanged();     // o pacote de ícones mudou (tema): solta os ícones guardados
}
namespace revelar {  // clique no vazio da área de trabalho mostra a mesa, como no Mac (revelar.cpp)
void Start();
void Stop();
void Pausar(bool p);  // modo jogo: sem o gancho global do mouse
void Foreground(HWND h);
bool Owns(HWND h);  // janela minimizada por "mostrar a mesa" (não vai para o Dock)
}
namespace mesas {  // áreas de trabalho virtuais pela interface interna do Explorer (mesas.cpp); false = use os atalhos
bool Switch(int index);
bool Create();
bool Remove(int index);
bool MoveWindow(HWND h, int index);  // leva a janela (de qualquer programa) para outra mesa
}
namespace widgets {  // widgets da mesa: Relógio Mundial, Tempo, Relógio, Calendário, Nascer do Sol (widgets.cpp)
// o tempo de agora em uma frase (inglês, para a Siri: issue #38), ou "" se ainda não chegou
std::wstring ResumoTempo();
void Create(HINSTANCE hi);
std::vector<RECT> Areas();  // onde estão (tela): os ícones da mesa não ficam embaixo deles
void EditMenu(POINT pt);    // "Editar Widgets…": ligar/desligar cada widget (também sem nenhum na tela)
void SettingsChanged();     // widget.<id>.on mudou no ajustes.txt (Ajustes > Personalizar a Mesa)
}
namespace mesa {  // mesa própria (papel de parede + ícones) e o modo sem explorer.exe (mesa.cpp, docs/sem-explorer.md)
void Start(HINSTANCE hi, HWND msgWnd);  // ajuste explorer=0: fecha o Explorador e assume a mesa
void Stop(bool restartExplorer);        // false: caiu/fim da sessão (o vigia abre o Explorador)
void Pulse();                           // laço principal: a interface andou (o vigia lê; ver mesa.cpp)
void NoExplorerOnExit();                // reiniciando o Shell: ao sair, o Explorador não é aberto (o Shell novo vem já)
void SettingsChanged();                 // ajustes.txt mudou (explorer=0/1)
// cor média do papel de parede que a mesa está mostrando, na faixa atrás da barra de menus (0..1); false sem mesa
bool TopColor(int barPx, float& r, float& g, float& b, bool baixo = false);
// a imagem do papel de parede do monitor principal como a mesa desenhou (BGRA, opaca) e onde fica na tela. key muda
// quando o papel de parede é refeito: com key igual ao que se passou, não copia nada e devolve true
bool Papel(void*& key, int& w, int& h, RECT& tela, std::vector<BYTE>& px);
bool Standalone();                      // sem Explorador agora (qualquer thread)
void WallpaperNow(const std::wstring& path);  // o Ajustes escolheu este papel de parede: mostra já
void DepoisDoJogo();                    // modo jogo acabou: refaz a mesa se a tela mudou durante o jogo
// App antigo da Loja (UWP: Segurança do Windows...) não abre sem o Explorador (a janela dele vem de uma peça
// que vive no explorer.exe). Abre o Explorador escondido, abre o app de novo e, sem nenhum desses apps
// aberto por 15 s, volta ao modo sem Explorador. Qualquer thread.
void NeedExplorer(const std::wstring& aumid);
bool IsDeskClass(const wchar_t* cls);   // Progman, WorkerW ou a nossa SequoiaMesa
bool IsDeskWindow(HWND h);
bool IconAt(POINT screen);              // há um ícone da nossa mesa embaixo do ponto
void WinKey(UINT vk);                   // Win+E / Win+R / Win+D sem o Explorador
bool Watchdog(const wchar_t* cmdLine);  // WacOSShell.exe --mesa-vigia <pid>
bool IsWindowsShell();                  // HKCU Winlogon Shell aponta para o WacOSShell
void RunStartupApps();                  // shell do Windows: Run e pastas Inicializar (uma vez por sessão)
}
namespace notificacoes {  // Central de Notificações: clique na data/hora da barra de menus (notificacoes.cpp)
bool Create(HINSTANCE hi);
void Toggle();
bool IsOpen();
bool BannerVisible();  // um banner está na tela (Tahoe: o relógio da barra ganha a pílula de destaque)
void IconsChanged();   // ícone próprio do Dock mudou (apps::ReloadIcons): relê os ícones das notificações
void Stop();  // ao sair: o banner do Windows volta a aparecer (o Shell troca pelo banner do Mac; ajuste "banners_mac")
// Widgets da coluna, embaixo das notificações. Sem chamar, vêm Calendário e Relógio próprios.
// w × h em pt (pequeno 164 × 164, médio 344 × 164); paint desenha em DIPs com a origem no canto do widget
// (o vidro e a borda já vêm desenhados); click roda depois que o painel fecha.
struct Widget {
    float w, h;
    std::function<void(ID2D1DeviceContext* dc, float w, float h, bool dark)> paint;
    std::function<void()> click;
};
void SetWidgets(std::vector<Widget> list, std::function<void()> onEdit);  // onEdit: "Editar Widgets…"
struct Brief { std::wstring aumid, app; long long time; };  // notificação do Windows (só o necessário para o badge)
std::vector<Brief> Recentes();  // as notificações da Central agora (qualquer thread)
}
namespace ilha {  // Ilha Dinâmica (ilha.cpp): notch/pílula preta no topo (música, timer, avisos, Estante...); ajuste ilha=1, ilha_estilo
bool Create(HINSTANCE hi);
void Reload();                  // ajustes mudaram (ilha, ilha_estilo, ilha_notificacoes, ilha_volume, ilha_passar)
void SetFullscreen(bool locked); // a janela da frente mudou: confere tela cheia no monitor principal (jogo, vídeo, app) e some na hora
bool On();
// notificação nova: true = a ilha mostrou (o banner do canto não aparece). icon desenha o ícone do app em r (DIP)
bool Notificacao(const std::wstring& app, const std::wstring& title, const std::wstring& body,
                 std::function<void(ID2D1DeviceContext*, ID2D1SolidColorBrush*, D2D1_RECT_F)> icon, std::function<void()> click);
bool Volume(float level, bool mute);  // teclas de volume: true = a ilha mostrou (sem o aviso de volume)
void BotaoMouse(bool down, POINT pt);  // gancho do mouse: botão esquerdo apertado/solto (arrastar arquivo até a Ilha desligada)
void Brilho(int pct);           // o brilho da tela mudou (qualquer thread): aviso de brilho, como o de volume
void CapsLock();                // a tecla Caps Lock foi apertada (gancho de teclado do main.cpp)
void PrivacidadeMudou();        // a bandeja mudou: relê o ícone de privacidade do Windows (tray::PrivacyBits)
void Captura(bool copiada);     // a Captura de Tela terminou: aviso rápido "Captura feita" / "Captura copiada"
int Privacidade();              // 1 microfone, 2 câmera em uso (bits); a 1ª chamada liga a vigia; mudou = menubar::Update()
}
namespace voz {  // voz.cpp: ditado pelo reconhecimento de fala do Windows (o microfone da Siri e do Spotlight)
enum Tipo { V_PARCIAL, V_FINAL, V_ERRO, V_PRIVACIDADE };
void Ouvir(HWND to, UINT msg);  // uma frase: msg com wParam = Tipo e lParam = new std::wstring (quem recebe apaga)
void Parar();
bool Ouvindo();
}
namespace siri {  // siri.cpp: a Siri com a chave do usuário (issue #39); provedores e chave em src/comum/siri.h
bool Pronta();                          // um serviço escolhido nos Ajustes (e a chave, se ele pede)
void Perguntar(const std::wstring& q, bool teste = false);
void Abrir(bool teste = false);   // o ícone da barra de menus: o painel com o campo para digitar (de novo: fecha)
bool Aberta();  // abre o painel da Siri e faz a pergunta (numa thread)
}
namespace brilho {  // brilho.cpp: vigia o brilho da tela do notebook (WMI) e mostra na Ilha
void Start();
}
namespace palco {  // Organizador Visual / Stage Manager (palco.cpp): ajuste organizador=1
bool On();
void Sync();    // liga/desliga conforme o ajuste
void Toggle();  // Central de Controle
void Stop();    // Shell saindo: devolve as janelas
bool Estacionada(HWND h);  // a janela está fora da tela esperando na faixa (não mexer nela)
void Recupera();  // Shell abrindo: as que uma execução anterior deixou estacionadas voltam (pelo diário)
}
namespace quicklook {  // Quick Look (quicklook.cpp): painel só com a prévia de um arquivo (espaço abre e fecha)
void Toggle(const std::wstring& path, bool activate = true);
void Close();
bool IsOpen();
std::wstring Path();
}
namespace pilhas {  // Pilhas do Dock (pilhas.cpp): Downloads em Leque, Grade ou Lista, como no Mac
void Show(POINT anchorScreen);  // anchor: centro de cima do ícone
void ShowApps(POINT anchorScreen);  // Leopard: a pasta Aplicativos da Dock como Pilha em Grade
void Menu(POINT anchorScreen);  // botão direito: ordenar, ver como, exibir como (pasta/pilha), abrir a pasta
bool IsOpen();
std::vector<std::wstring> Recentes(int max);  // os primeiros arquivos de Downloads na ordem escolhida (a pilha no Dock)
}
namespace verde {  // menu do botão verde (verde.cpp): encaixar a janela (metades, quartos, preencher)
void Show(HWND target, POINT screenPt);
}
namespace atalhos {  // itens do app no menu do Dock: tarefas ("Nova janela") e recentes (atalhos.cpp)
struct Item { std::wstring title, path, args, dir; bool recent = false; };
std::vector<Item> Tasks(const std::wstring& exePath);
std::vector<Item> Recent(const std::wstring& aumid, const std::wstring& exePath, int max = 8);
std::wstring AumidOf(HWND h);
void Run(const Item& it);
// o que o app está tocando (controles de mídia do Windows), como no menu do Spotify no Dock do Mac
struct Media { bool has = false, playing = false, canNext = false, canPrev = false; std::wstring title, artist; };
Media MediaOf(const std::wstring& exePath, const std::wstring& aumid);
void MediaCommand(const std::wstring& exePath, const std::wstring& aumid, int cmd);  // 0 tocar/pausar, 1 próxima, 2 anterior, 3 parar; sem app: a sessão atual
}
namespace cantos {  // Cantos Ativos (cantos.cpp): canto_se/sd/ie/id = nada|mission|mesa|launchpad|notificacoes|central|spotlight|bloquear|tela|nota
void Start();
bool NotaRapida();  // o canto inferior direito é o da Nota Rápida
void Pausar(bool p);  // modo jogo: sem ler o ponteiro
}
namespace notarapida {  // canto inferior direito da tela abre a Nota Rápida (notarapida.cpp); `canto_nota_rapida=0` desliga
void Create(HINSTANCE hi);
}

namespace bloqueio {  // tela de bloqueio do Sequoia (bloqueio.cpp); `bloqueio=0` em ajustes.txt usa a do Windows
void Create(HINSTANCE hi);
void Lock();                              // Win+L, menu da Apple → Bloquear Tela
bool Active();                            // travada agora (o gancho do teclado barra Alt+Tab, Win...)
void Fallback();                          // o Shell vai sair/caiu: se travada, bloqueia o Windows de verdade
void Stop();                              // Shell saindo: apaga a política do Win+L, dispensa o vigia
bool Watchdog(const wchar_t* cmdLine);    // Shell.exe --vigia <pid>: true = era o vigia (sair)
bool RenderPng(const wchar_t* cmdLine);   // Shell.exe --bloqueio-png ...: imagem de teste (sair)
int Preview(HINSTANCE hi);                // Shell.exe --testar-bloqueio: a tela por ~3,5 s, sem senha
int SetPasswordUi(HINSTANCE hi);          // Shell.exe --senha-bloqueio: senha própria da tela (só o resumo é gravado)
}
namespace boot {  // tela de inicialização do Mac depois do login (boot.cpp); ajuste boot=0 desliga
void Show(HINSTANCE hi);   // começo do WinMain: só aparece se o login foi há pouco
void Progress(float p);    // 0..1: partes do Shell já criadas
void Done();               // Shell pronto: a tela espera o Explorador/apps de inicialização e esmaece
int Preview(HINSTANCE hi); // Shell.exe --testar-boot: só a tela, ~6 s
}
namespace launchpad {
bool Create(HINSTANCE hi);
void Warm();    // prepara o fundo desfocado antes da primeira abertura
void Toggle();
bool IsOpen();
int Category(const std::wstring& appName);  // categoria do app (1..CategoryCount()-1; 0 = todos)
int CategoryCount();
const wchar_t* CategoryName(int k);
void Rescan();  // relê a lista de apps (um programa foi instalado ou removido)
void IconsChanged();  // um ícone próprio mudou: relê a lista e os ícones (sem reaproveitar os guardados)
// Fora do Launchpad e do painel Apps: tirado à mão (launchpad_ocultos) ou ferramenta do Windows com
// launchpad_sem_sistema=1 (Painel de Controle, Mapa de Caracteres, Ferramentas do Windows...)
bool Escondido(const std::wstring& name, const std::wstring& parsing);
bool AppDoSistema(const std::wstring& name, const std::wstring& parsing);
void Ocultar(const std::wstring& name);  // acrescenta a launchpad_ocultos
}
namespace instalados {  // vigia o Menu Iniciar e os apps da Store e avisa o Spotlight e o Launchpad (instalados.cpp)
void Start();
}
namespace dock {
bool Create(HINSTANCE hi);
// issue #45: no Tahoe e no Golden Gate, o ícone fora do formato do Mac vai para dentro do quadrado arredondado cinza
// (dock_icones_cinza, como o macOS 26). O Dock, o Launchpad e o Spotlight usam a mesma.
void Cela(UINT w, UINT h, std::vector<BYTE>& px);
void Update();          // apps mudaram
HWND Hwnd();
HWND Fundo();  // o vidro, a janela mais de baixo do Dock
bool Vidro(RECT* screen);  // retângulo do vidro do Dock na tela (todos os temas, também o Leopard); false = fora da tela
int ReservedHeight();   // px reservados na borda do Dock (área de trabalho): embaixo ou do lado (Posicao)
int Posicao();          // 0 embaixo, 1 esquerda, 2 direita (dock_posicao; o Leopard fica sempre embaixo)
HMONITOR MonitorHandle();  // o monitor do Dock (ajuste dock_monitor; vazio = o principal)
void Raise();          // traz o Dock para cima (ex.: por cima do Launchpad)
void OpenAppsStack();  // Leopard: a pasta Aplicativos como Pilha em Grade
void OpenDownloadsStack();  // a pilha Downloads (testes: ShellAbrirPainel 15)
void ReloadSettings();  // o Ajustes do Sistema mudou tamanho, ampliação...
void ApplyTheme();      // claro/escuro mudou
void IconsChanged();    // o pacote de ícones mudou (tema Sequoia/Tahoe): solta os ícones guardados
void SetFullscreen(bool on);  // app em tela cheia na frente: some
RECT MinimizeTarget(HWND h = nullptr);
RECT AppIconTarget(HWND h);  // tela: o ícone do app dono da janela (vazio se não está no Dock)  // tela: onde a janela minimizada vai parar (h: no ícone do app com dock_minimizar_no_icone)
void BadgesChanged();   // chegou ou saiu notificação: redesenha os badges vermelhos
}

namespace backdrop {  // vidro desfocado com recorte arredondado suave (backdrop.cpp)
bool Attach(HWND h);  // janela criada com WS_EX_NOREDIRECTIONBITMAP
void Detach(HWND h);  // a janela vai ser destruída: solta o vidro dela
void Restart();       // refaz o vidro real do zero (Ajustes: "Reiniciar o Liquid Glass")
void Set(HWND h, float w, float hgt, float radius, D2D1_COLOR_F tint);  // px
void Fade(HWND h, float opacity, float scale, float cx, float cy);     // animação: opacidade e escala em volta de (cx, cy) px
void FadeAnim(HWND h, float cx, float cy, const std::vector<float>& escala, const std::vector<float>& opac, float passoMs);  // pela Composição
void FadeOutAnim(HWND h, int ms);    // some em ms pela Composição (não depende desta thread)
// Conteúdo desenhado DENTRO do vidro (mesma árvore da Composição): vidro e desenho chegam à tela no mesmo
// quadro. Numa janela à parte (swap chain), a borda do vidro ficava um quadro atrás do desenho quando o
// painel mudava de tamanho. Coordenadas em DIP (scale = DPI/96); o contexto é do dispositivo de ui::Gpu()
// (bitmaps de lá valem). Recortado, esmaecido e escalado junto com o vidro. nullptr: desenhe do jeito antigo.
ID2D1DeviceContext* ContentBegin(HWND h, int wpx, int hpx, float scale);
void ContentEnd(HWND h);
// avisa (PostMessage(to, msg)) quando o que foi desenhado no vidro já chegou à tela (commit do compositor);
// quem mostra a janela depois disso não aparece com o vidro da última vez
void AfterCommit(HWND to, UINT msg);
// Transparência e Efeitos (Ajustes do Sistema): Liquid Glass, reduzir transparência, tinta do vidro
struct Style {
    bool liquid = false;     // liquid_glass: vidro líquido do macOS Tahoe
    bool golden = false;     // golden_gate (com liquid_glass): tema macOS 27 Golden Gate
    bool leopard = false;    // leopard (sem liquid_glass): tema Mac OS X Leopard (o Sequoia com o Aqua de 2007)
    bool tinted = false;     // liquid_estilo: "nitido" ou "tingido"
    float refraction = 0.6f; // liquid_refracao 0..1: força da lente nas bordas
    bool reduce = false;     // reduzir_transparencia
    float tintK = 0.5f;      // vidro_tinta 0..1 (0,5 = a tinta original de cada janela); no vidro real, também o desfoque
    bool real = false;       // vidro_real: Liquid Glass com refração de verdade (captura da tela por baixo)
};
struct Piece {  // px, nas coordenadas da janela
    D2D1_RECT_F r; float radius;
    // gosma (comum/vidro.h): a peça é a cápsula `cap` unida aos círculos `circ` (o Spotlight soltando os círculos)
    bool goo = false; D2D1_RECT_F cap{}; float capR = 0; std::vector<D2D1_ELLIPSE> circ; unsigned broken = 0;
};
// várias peças de vidro soltas numa janela só (módulos da Central): cada uma com a lente dela e a tinta;
// o contorno fica por conta de quem desenha por cima
void SetPieces(HWND h, float w, float hgt, const std::vector<Piece>& pieces, D2D1_COLOR_F tint, float minFrost = 0);  // minFrost: desfoque mínimo (DIP)
bool Real();                           // vidro real ativo (senão, o fosco)
void CaptureVisible(bool show);        // Captura de Tela do projeto: mostrar as peças só durante a foto
const Style& Current();
bool Liquid();                         // Liquid Glass ligado (e transparência não reduzida)
void Reload();                         // lê os ajustes e reaplica em todos os vidros
float AdjustAlpha(float a);            // opacidade da tinta conforme os ajustes
D2D1_COLOR_F AdjustTint(D2D1_COLOR_F c);
}
namespace liquid {  // tema macOS Tahoe nos painéis do Shell (liquid.cpp): detalhes D2D por cima do vidro
bool On();  // Tema = macOS Tahoe (liquid_glass), mesmo com a transparência reduzida
bool Golden();  // Tema = macOS Golden Gate (liquid_glass + golden_gate): o Tahoe com o que mudou no macOS 27
bool Leopard(); // Tema = Mac OS X Leopard (leopard=1, sem liquid_glass): barra de menus clara com a maçã grafite, Dock em prateleira
// contorno especular; spec = força do brilho dos cantos (0 em repouso, 1 com o ponteiro em cima), lx/ly = de onde vem a
// luz (0..1 dentro da peça, a posição do ponteiro; < 0 = canto de cima à esquerda)
void Rim(ID2D1DeviceContext* dc, D2D1_RECT_F r, float rad, bool dark, float strength = 1, float spec = 1, float lx = -1, float ly = -1);
void Tile(ID2D1DeviceContext* dc, D2D1_RECT_F r, float rad, bool dark, float fill = 1);     // bloco/cápsula de vidro
}

// ---- main.cpp
namespace shell {
// Modo nativo (barra-nativa\MacBarra.dll): o Shell é a barra de tarefas do próprio Windows, rodando numa
// thread dentro do explorer.exe (trocado pelo TrayUI Component). Nada de processo próprio: sem esconder a
// barra do Windows (ela é nossa), sem Shell_TrayWnd falsa, sem mesa própria, sem ganchos com DLL nos outros
// programas, sem TerminateProcess/filtro de falhas (o processo é o explorer).
extern bool nativo;
// Modo "Só Dock e barra de menus" (ajuste modo=dock; Ajustes > Geral): só a barra de menus (com Central de Controle,
// Notificações e Spotlight), o Dock (com Launchpad, gênio e Mission Control). O resto fica com o Windows: a área de
// trabalho e os ícones dele, o Explorador nas pastas, o Alt+Tab, a tecla Windows, a captura de tela e as barras de
// título dos outros apps. Lido na partida do Shell (trocar o modo reinicia o Shell).
bool SoDock();
// Tira a janela de minimizada. Numa janela de administrador o ShowWindow de fora e recusado pelo Windows (o explorer
// nao e administrador); o pedido pelo menu de sistema dela (WM_SYSCOMMAND) passa.
inline void Restaurar(HWND h) { ShowWindow(h, SW_RESTORE); if (IsIconic(h)) PostMessageW(h, WM_SYSCOMMAND, SC_RESTORE, 0); }
// Libera o SetForegroundWindow: o processo que mandou a última entrada pode trocar a janela da frente. Era só Alt
// apertado e solto, mas Alt sozinho, ao soltar, liga a barra de menus do app da frente (o Firefox abria com a barra
// Arquivo/Editar/Exibir à mostra, igual ao F10; issue #8). Uma tecla sem função (vkE8) no meio desfaz isso. Marcadas
// como nossas: o gancho do teclado do Shell deixa passar.
// Janelas "sempre por cima" de outros programas que estão acima de todas as nossas visíveis (uma ilha dinâmica de outro
// app, um widget flutuante). Ao subir o Dock, a barra de menus ou a camada do revelar, elas iam para baixo da barra e
// sumiam (issue #33: a ilha do Vibrance sumia ao clicar na mesa). Pega antes de subir as nossas; DevolveAlheias as põe
// de volta por cima, na mesma ordem.
std::vector<HWND> AlheiasNoTopo();
void DevolveAlheias(const std::vector<HWND>& v);
inline void LiberaFrente() {
    const ULONG_PTR M = 0x53484C4C;
    keybd_event(VK_MENU, 0, 0, M); keybd_event(0xE8, 0, 0, M); keybd_event(0xE8, 0, KEYEVENTF_KEYUP, M); keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, M);
}
int RodarNativo(HINSTANCE dll, const std::wstring& dirBin, const std::wstring& logPath);
// abre o Finder com estes argumentos: no nativo, no Finder de dentro do explorer (MacFinder.dll); senão o WacOSFinder.exe
void AbrirFinder(const std::wstring& args);
// abre o Finder com estes argumentos: no nativo, no Finder de dentro do explorer (MacFinder.dll); senão o WacOSFinder.exe
void AbrirFinder(const std::wstring& args);
DWORD SemProprio();             // WINEVENT_SKIPOWNPROCESS fora do modo nativo (no nativo, o processo é o explorer)
void ThreadDeGancho();          // a thread atual segura um gancho global de mouse/teclado: prioridade alta e fora do EcoQoS
bool Nossa(HWND h);             // janela do próprio Shell (no nativo: só as criadas pelo Shell, não as do explorer)
extern HINSTANCE inst;
extern std::wstring dir;        // pasta do Shell.exe
void Quit(const wchar_t* why);  // sai do shell e devolve a barra do Windows
void ApplyWorkArea();
std::wstring Setting(const wchar_t* key, const wchar_t* def);
void SetSetting(const wchar_t* key, const std::wstring& value);
// A hora no formato escolhido (relogio_formato: auto = o do Windows, 24 ou 12; issue #43): "14:05" ou "2:05 PM".
// segundos: com os segundos; ampm: false tira o AM/PM (o relógio grande da tela de bloqueio, como no Mac)
std::wstring Hora(const SYSTEMTIME& st, bool segundos = false, bool ampm = true);
bool Hora12();
// A pasta do programa por trás de um item da pasta Aplicativos ("shell:AppsFolder\..."), de um atalho ou de um .exe:
// jogos e apps que leem os arquivos "ao lado" só abrem a partir da pasta deles (issue #38: DELTARUNE). Vazio se não é exe.
std::wstring PastaDoApp(const std::wstring& alvo);
RECT WorkAreaFor(HMONITOR mon);  // a área de trabalho que o Shell reserva (barra de menus em cima, Dock embaixo), px
bool NaoIncomodar(HMONITOR mon);  // app em tela cheia nesse monitor (nullptr = qualquer um) ou Windows em "não incomodar"
void Sound(const wchar_t* name);
bool OpenCalculator(const std::wstring& target);  // alvo é a Calculadora do Windows: abre a nossa (Calculadora.exe)
void OpenForceQuit(bool memory);  // Forçar Encerrar (Ctrl+Alt+Esc, menu da Apple); memory = "ficou sem memória"
void OpenMonitor();  // Monitor de Atividade (Ctrl+Shift+Esc, menu da Apple)  // toca sons\mac\<name>.wav (sons do Mac), sem esperar
// abre o Ajustes do Sistema (Ajustes.exe) na página. Com "Apps do Windows" > Ajustes ligado, as páginas que são do
// Windows (rede, Bluetooth, telas, som, energia, links ms-settings:) abrem o Ajustes do Windows; nosso = o nosso sempre
void OpenSettings(const std::wstring& page, bool nosso = false);
// "Apps do Windows" (Ajustes > Geral): true = o app do Windows no lugar do nosso. app: captura, ajustes, gerenciador,
// calculadora, terminal, midia, visualizar, teclas_midia (chave windows_<app> no ajustes.txt, desligada por padrão)
bool UsarWindows(const wchar_t* app);
// ShellExecute numa thread (com o disco ocupado ele segurou a interface 3,2 s; issue #32). Os textos são copiados na hora.
void Abrir(const wchar_t* verb, const wchar_t* file, const wchar_t* args = nullptr, const wchar_t* dir = nullptr, int show = SW_SHOWNORMAL);
// roda `f` na thread da interface (pela janela de mensagens): o resultado de um trabalho feito numa thread
void NaInterface(std::function<void()> f);
}

namespace sentinela {  // sentinela.cpp: registra no shell.log onde a thread da interface ficou parada (1 s ou mais)
void Start(HWND alvo);
extern std::atomic<UINT> ultimaMsg;     // o laço principal anota a mensagem que vai tratar
extern std::atomic<HWND> ultimaJanela;
}

namespace ampliar {  // animação do "ampliar" do Mac nas janelas de outros apps (ampliar.cpp; pedida pela MacJanela.dll)
// Foto da janela no retângulo `from` (tela) animando até `to`. true = a foto já está na tela (a DLL esconde a janela)
bool Start(HWND h, RECT from, RECT to, int flags, COLORREF bar);  // flags: 1 barra padrão (botões e título na foto), 2 janela de plug-in
void SecondPhoto(HWND h);  // a janela já mudou de tamanho (escondida): foto nova entra por cima, ainda em movimento
void Shown(HWND h);        // a janela de verdade reapareceu: as fotos somem
bool Fechar(HWND h, LPARAM bar = 0);  // a janela vai sumir: foto dela encolhendo e sumindo, como no Mac (bar: a nossa barra, da DLL)
}
namespace abrir {  // janela nova aparecendo como no Mac (abrir.cpp; pedida pela WacOSWindow.dll)
void Open(HWND h, HWND bar = nullptr);  // a janela (e a nossa barra, se tiver) escondida e já pintada: anima e manda mostrar
}
namespace foto {
// Foto (BGRA pré-multiplicado, opaca) da parte `src` da janela `h` (coordenadas da miniatura do DWM);
// `corner` > 0: raio (px) dos cantos arredondados. Funciona com a janela minimizada.
bool Window(HWND h, const RECT& src, float corner, std::vector<BYTE>& bgra, UINT& w, UINT& hh);
// Várias janelas de uma vez, na placa de vídeo (Windows.Graphics.Capture), direto em superfícies do
// compositor (dispositivo de ui::Comp()). surf == nullptr na que falhar. Espera até `timeoutMs`.
struct Gpu { IDCompositionSurface* surf = nullptr; UINT w = 0, h = 0; };
std::vector<Gpu> Windows(const std::vector<HWND>& hs, const std::vector<RECT>& srcs, int timeoutMs = 150);
// Capturas que ficam abertas (guardam o último quadro de cada janela): Grab tira as fotos na hora.
struct Live;
Live* Start(const std::vector<HWND>& hs, const std::vector<RECT>& srcs);
std::vector<Gpu> Grab(Live* live, int timeoutMs = 150);
void Stop(Live* live);
}

namespace ponteiro {  // ponteiro.cpp: sacudir o mouse faz o ponteiro crescer (como no Mac)
void Start();
void Stop();
// tema Mac OS X Leopard: liga o cursor do Mac OS X (guardando o esquema de antes) ou devolve o de antes
void TemaLeopard(bool on);
void Pausar(bool p);  // modo jogo: sem Raw Input do mouse enquanto o jogo roda
}
namespace jogo {  // Modo Jogo (jogo.cpp): jogo na frente → desliga o que mais pesa; ajuste modo_jogo, lista %APPDATA%\Shell\jogos.txt
void Start();
void Reload();            // ajustes mudaram
void Foreground(HWND h);  // a janela da frente mudou
bool Ativo();
// gancho de EVENT_OBJECT_LOCATIONCHANGE que sai durante o modo jogo (cada movimento do cursor chegava ao Shell)
void GanchoPosicao(WINEVENTPROC fn, DWORD flags);
void SoltarPosicao(WINEVENTPROC fn);
}

namespace leve {  // Modo leve (leve.cpp): PC fraco ou na bateria → sem vidro real, fosco mais barato, animações a ~60 quadros/s
void Start();             // vigia tomada/bateria
bool Atualizar();         // relê modo_leve, a bateria e o PC; true = mudou (quem chama recarrega o vidro)
bool Ativo();             // pode ser lido de qualquer thread
}
