// Quick Look do Shell, como no Mac: espaço num arquivo da mesa (ou da pilha de Downloads, ou ⌘Y/Ctrl+Y no
// Spotlight) abre um painel só com a prévia do arquivo; espaço ou Esc fecha. A prévia é a miniatura grande do
// Windows (fotos, PDFs, vídeos e documentos com visualizador instalado) ou o texto, nos arquivos de texto.
// Barra de cima: fechar à esquerda, nome no meio, "Abrir com <app>" à direita. Arrasta pela barra.
#include "shell.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#include <algorithm>
#include <cmath>
#include <thread>
#include "../comum/idioma.h"
#include "../comum/barra_video.h"
#include "../comum/dica.h"
#include <d2d1_3.h>
#include <wincodec.h>
#include <shcore.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "windowscodecs.lib")
#include <mfplay.h>
#include <mfapi.h>
#pragma comment(lib, "mfplay.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

namespace {

const UINT WM_QL_READY = WM_APP + 91;
const UINT WM_QL_FRAME = WM_APP + 141;  // quadro da animação de abrir e fechar (relógio da tela)
const UINT WM_QL_MEDIA = WM_APP + 92;   // evento do MFPlay (vídeo/música pronto, terminou, erro)
const float BAR = 38, R = 12, PAD = 0;
HWND g_wnd;
ui::Surface g_s;
std::wstring g_path, g_name, g_app, g_text;
UINT g_iw, g_ih; std::vector<BYTE> g_px;
ID2D1Bitmap* g_bmp;
ID2D1RenderTarget* g_bmpRt;
int g_gen;
bool g_loading, g_hoverClose, g_hoverOpen;
IDWriteTextFormat *g_tfTitle, *g_tfBtn, *g_tfText;
D2D1_RECT_F g_closeR, g_openR, g_shareR;
// Golden Gate: vidro fosco de verdade (o mesmo dos menus de contexto) numa janela atrás (g_back), com o desenho
// dentro do vidro; a g_wnd (camadas, quase invisível) só recebe o mouse e o teclado. Arrasta por qualquer ponto.
HWND g_back;
bool g_glass, g_hoverShare, g_hoverFull, g_hoverRot, g_hoverMark;
bool g_full;                         // botão de tela cheia: a prévia ocupa o monitor
D2D1_RECT_F g_fullR, g_rotR, g_markR;
// Zoom das fotos (issue #8): a roda do mouse amplia em volta do ponteiro (1 = a foto cabe no painel, até 8×) e,
// ampliada, arrastar move a foto em vez do painel
float g_zoom = 1, g_panX, g_panY;
bool g_panning; POINT g_panFrom; float g_panX0, g_panY0;
D2D1_RECT_F g_bodyR;                 // onde a foto foi desenhada por último (DIP da janela)
bool g_bodyFill;                     // a foto preenche a moldura (Golden Gate fora da tela cheia) ou cabe nela
// Texto com rolagem (issue #8, carlosjrrr, 22/11/2026: antes só a primeira tela do arquivo aparecia e a roda não
// fazia nada): a roda, as setas e Page Up/Down rolam, com a barrinha fina do Mac à direita enquanto há mais texto.
float g_textY;                       // quanto o texto subiu (DIP)
IDWriteTextLayout* g_textLay; float g_textLayW, g_textH;
void TextoSolta() { if (g_textLay) { g_textLay->Release(); g_textLay = nullptr; } g_textY = 0; g_textH = 0; }
void DrawTexto(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, D2D1_RECT_F body, float px, float py, bool dark) {
    float w = std::max(10.f, body.right - body.left - 2 * px), vis = body.bottom - body.top;
    if (!g_textLay || std::fabs(g_textLayW - w) > 0.5f) {
        if (g_textLay) { g_textLay->Release(); g_textLay = nullptr; }
        ui::dw->CreateTextLayout(g_text.c_str(), (UINT32)std::min<size_t>(g_text.size(), 300000), g_tfText, w, 1e7f, &g_textLay);
        g_textLayW = w; g_textH = 0;
        if (g_textLay) {  // o formato pode vir centrado: aqui é do topo, à esquerda, quebrando nas palavras
            g_textLay->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            g_textLay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            g_textLay->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        }
        DWRITE_TEXT_METRICS tm{};
        if (g_textLay && SUCCEEDED(g_textLay->GetMetrics(&tm))) g_textH = tm.height;
    }
    if (!g_textLay) return;
    float maxY = std::max(0.f, g_textH + 2 * py - vis);
    g_textY = std::clamp(g_textY, 0.f, maxY);
    rt->DrawTextLayout({ body.left + px, body.top + py - g_textY }, g_textLay, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    if (maxY > 0) {  // a barrinha de rolagem
        float tot = g_textH + 2 * py, th = std::max(28.f, vis * vis / tot), ty = body.top + 3 + (vis - 6 - th) * (g_textY / maxY);
        D2D1_COLOR_F old = br->GetColor();
        br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.35f) : D2D1::ColorF(0, 0, 0, 0.30f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(body.right - 8, ty, body.right - 3.5f, ty + th), 2.25f, 2.25f), br);
        br->SetColor(old);
    }
}
bool TextoRola(float dy) {  // true se andou
    if (g_text.empty()) return false;
    float y0 = g_textY;
    g_textY = std::max(0.f, g_textY + dy);  // o teto é preso no desenho (depende do tamanho do painel)
    return g_textY != y0 || dy > 0;
}

// o retângulo da foto no corpo, com o zoom e o deslocamento (que fica preso: a foto não sai da moldura)
D2D1_RECT_F PhotoRect(D2D1_RECT_F body, UINT iw, UINT ih, bool fill) {
    float bw = body.right - body.left, bh = body.bottom - body.top;
    if (!iw || !ih || bw <= 0 || bh <= 0) return body;
    float k = (fill ? std::max(bw / iw, bh / ih) : std::min(bw / iw, bh / ih)) * g_zoom, w = iw * k, h = ih * k;
    float mx = std::max(0.f, (w - bw) / 2), my = std::max(0.f, (h - bh) / 2);
    g_panX = std::clamp(g_panX, -mx, mx); g_panY = std::clamp(g_panY, -my, my);
    float x = body.left + (bw - w) / 2 + g_panX, y = body.top + (bh - h) / 2 + g_panY;
    return D2D1::RectF(x, y, x + w, y + h);
}
void ZoomReset() { g_zoom = 1; g_panX = g_panY = 0; g_panning = false; }
bool g_dragging; POINT g_dragFrom, g_dragOrg;  // arrastar o painel
// Vídeo e música (Golden Gate, como no Mac): o MFPlay toca numa janela própria por cima do vidro (g_vid, na moldura
// de 5, cantos 15) e a barra do QuickTime (barra_video.h) fica numa janela em camadas (g_ctl) com o vidro fosco
// (acrílico, g_ctlBack) atrás. Vídeo: a barra aparece com o mouse e some 2,5 s depois. Música: o ícone (ou a capa) e
// a barra sempre visível embaixo.
IMFPMediaPlayer* g_mp;
HWND g_vid, g_ctl, g_ctlBack;
bool g_media, g_hasVideo, g_scrub, g_ended, g_mpVideo;
float g_vol = 1;
bool g_volDrag;  // arrastando o volume da barra do vídeo
DWORD g_ctlMoveAt;
std::vector<barravideo::Hit> g_vhits;
const float CTL_DOWN = 20;  // a barra fica a 20 do fundo da moldura

bool IsMediaPath(const std::wstring& p, bool* video = nullptr) {
    std::wstring x = PathFindExtensionW(p.c_str());
    for (auto& c : x) c = towlower(c);
    for (auto e : { L".mp4", L".mov", L".m4v", L".mkv", L".avi", L".wmv", L".webm" }) if (x == e) { if (video) *video = true; return true; }
    for (auto e : { L".mp3", L".m4a", L".flac", L".wav", L".aac", L".ogg", L".wma", L".aif", L".aiff", L".opus" }) if (x == e) { if (video) *video = false; return true; }
    return false;
}

class MediaCb : public IMFPMediaPlayerCallback {
    LONG ref = 1;
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** p) override {
        if (riid == IID_IUnknown || riid == __uuidof(IMFPMediaPlayerCallback)) { *p = this; AddRef(); return S_OK; }
        *p = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* e) override {
        if (!e) return;
        HWND w = FindWindowW(L"ShellQuickLook", nullptr);
        if (e->eEventType == MFP_EVENT_TYPE_MEDIAITEM_SET) PostMessageW(w, WM_QL_MEDIA, 1, FAILED(e->hrEvent));
        else if (e->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED) PostMessageW(w, WM_QL_MEDIA, 2, 0);
        else if (e->eEventType == MFP_EVENT_TYPE_ERROR) PostMessageW(w, WM_QL_MEDIA, 3, e->hrEvent);
    }
};
MediaCb* g_mcb;

double MPos() { if (!g_mp) return 0; PROPVARIANT v; PropVariantInit(&v); double s = 0; if (SUCCEEDED(g_mp->GetPosition(MFP_POSITIONTYPE_100NS, &v)) && v.vt == VT_I8) s = v.hVal.QuadPart / 1e7; PropVariantClear(&v); return s; }
double MLen() { if (!g_mp) return 0; PROPVARIANT v; PropVariantInit(&v); double s = 0; if (SUCCEEDED(g_mp->GetDuration(MFP_POSITIONTYPE_100NS, &v)) && v.vt == VT_UI8) s = v.uhVal.QuadPart / 1e7; PropVariantClear(&v); return s; }
bool MPlaying() { MFP_MEDIAPLAYER_STATE st = MFP_MEDIAPLAYER_STATE_EMPTY; if (g_mp) g_mp->GetState(&st); return st == MFP_MEDIAPLAYER_STATE_PLAYING; }
void MSeek(double s) {
    if (!g_mp) return;
    double len = MLen(); s = std::clamp(s, 0.0, len > 0 ? len - 0.05 : s);
    PROPVARIANT v; PropVariantInit(&v); v.vt = VT_I8; v.hVal.QuadPart = (LONGLONG)(s * 1e7);
    g_mp->SetPosition(MFP_POSITIONTYPE_100NS, &v);
    g_ended = false;
}
void MToggle() { if (!g_mp) return; if (MPlaying()) g_mp->Pause(); else { if (g_ended) MSeek(0); g_mp->Play(); } }
void MediaStop() {
    if (g_mp) { g_mp->Shutdown(); g_mp->Release(); g_mp = nullptr; }
    g_media = g_hasVideo = g_scrub = g_ended = false;
    if (g_vid) ShowWindow(g_vid, SW_HIDE);
    if (g_ctl) ShowWindow(g_ctl, SW_HIDE);
    if (g_ctlBack) ShowWindow(g_ctlBack, SW_HIDE);
    if (g_wnd) { KillTimer(g_wnd, 7); KillTimer(g_wnd, 11); }
}
const float GG_R = 20;               // cantos do painel (a foto: 15, concêntrica com a moldura de 5)
bool g_isIcon;                       // sem prévia (pasta, arquivo sem miniatura): ícone grande e informações
std::wstring g_kind, g_info, g_date; // "Pasta · 5 itens", "Modificado: 13 de nov. de 2026 às 19:50"

float Sc() { return ui::Scale(g_wnd); }

// Abrir e fechar animados, como o painel do Mac: abre crescendo de 94% com a opacidade subindo (180 ms, mola sem
// repique) e fecha sumindo e encolhendo um pouco (130 ms). É o vidro inteiro (o desenho está dentro dele) que anima.
float g_qlOp = 1, g_qlSc = 1;
double g_qlAnimT0 = -1;
bool g_qlAnimOut;
void CloseNow();
void ApplyFade() {
    if (!g_back) return;
    RECT wr; GetWindowRect(g_wnd, &wr);
    backdrop::Fade(g_back, g_qlOp, g_qlSc, (wr.right - wr.left) / 2.f, (wr.bottom - wr.top) / 2.f);
}
void AnimStep() {
    if (g_qlAnimT0 < 0) return;
    double dur = g_qlAnimOut ? 130.0 : 180.0;
    float t = (float)std::clamp((anim::Now() - g_qlAnimT0) / dur, 0.0, 1.0);
    float e = 1 - (1 - t) * (1 - t) * (1 - t);  // sai rápido e assenta
    if (g_qlAnimOut) { g_qlOp = 1 - e; g_qlSc = 1 - 0.04f * e; }
    else { g_qlOp = e; g_qlSc = 0.94f + 0.06f * e; }
    ApplyFade();
    if (t >= 1) {
        ui::FrameClock(g_wnd, WM_QL_FRAME, false);
        g_qlAnimT0 = -1;
        if (g_qlAnimOut) { g_qlAnimOut = false; g_qlOp = g_qlSc = 1; CloseNow(); }
        else { g_qlOp = g_qlSc = 1; ApplyFade(); }
    }
}
void AnimStart(bool out) {
    g_qlAnimOut = out;
    g_qlAnimT0 = anim::Now();
    ui::FrameClock(g_wnd, WM_QL_FRAME, true);
}

// Por cima do Finder (como o painel do Mac): fica "sempre visível" enquanto a janela da frente é do Finder ou do
// próprio Shell; trocando para outro app, volta a ser uma janela comum e fica atrás dele.
bool WantTop() {
    HWND f = GetForegroundWindow();
    if (!f) return false;
    DWORD pid = 0; GetWindowThreadProcessId(f, &pid);
    if (pid == GetCurrentProcessId()) return true;
    // pela classe da janela do Finder (ArquivosJanela...), não pelo nome do .exe: ao instalar uma versão nova, o
    // .exe em uso é renomeado (inst-...-WacOSFinder.exe) e o nome deixava de bater
    wchar_t cls[64] = {}; GetClassNameW(GetAncestor(f, GA_ROOT), cls, 64);
    return !wcsncmp(cls, L"Arquivos", 8);
}
void SyncBack();
void KeepTop() {
    if (!g_wnd || !IsWindowVisible(g_wnd)) return;
    bool want = WantTop(), is = (GetWindowLongW(g_wnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    // como no Mac: saiu do Finder (outro app na frente), o Quick Look fecha. Duas leituras seguidas (0,6 s), para
    // não fechar no instante em que o Windows troca a janela da frente
    static int away;
    if (!want && GetForegroundWindow()) { if (++away >= 2) { away = 0; quicklook::Close(); return; } } else away = 0;
    if (want != is) { SetWindowPos(g_wnd, want ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); SyncBack(); }
}

bool IsTextFile(const std::wstring& p) {
    std::wstring x = PathFindExtensionW(p.c_str());
    for (auto& c : x) c = towlower(c);
    for (auto e : { L".txt", L".md", L".log", L".csv", L".json", L".xml", L".ini", L".cfg", L".yml", L".yaml", L".cpp", L".h", L".c",
                    L".hpp", L".py", L".js", L".ts", L".ps1", L".cmd", L".bat", L".html", L".htm", L".css", L".reg", L".inc" })
        if (x == e) return true;
    return false;
}

std::wstring ReadText(const std::wstring& p) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    std::vector<char> b(1024 * 1024);  // com rolagem dá para ver o arquivo inteiro (até 1 MB)
    DWORD n = 0; ReadFile(f, b.data(), (DWORD)b.size(), &n, nullptr);
    CloseHandle(f);
    if (n == b.size()) { int k = 0; while (n > 0 && k < 4 && ((BYTE)b[n - 1] & 0x80)) { n--; k++; } }  // cortado no meio de uma letra UTF-8
    b.resize(n);
    if (n >= 2 && (BYTE)b[0] == 0xFF && (BYTE)b[1] == 0xFE) return std::wstring((wchar_t*)(b.data() + 2), (n - 2) / 2);
    int off = n >= 3 && (BYTE)b[0] == 0xEF && (BYTE)b[1] == 0xBB && (BYTE)b[2] == 0xBF ? 3 : 0;
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, b.data() + off, (int)n - off, nullptr, 0);
    UINT cp = len > 0 ? CP_UTF8 : CP_ACP;
    len = MultiByteToWideChar(cp, 0, b.data() + off, (int)n - off, nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(cp, 0, b.data() + off, (int)n - off, w.data(), len);
    return w;
}

// ---- PDF e SVG (issue #8): o Mac mostra a página/o desenho no Quick Look; aqui eles viram a "foto" da prévia
std::wstring LowerExt(const std::wstring& p) {
    std::wstring x = PathFindExtensionW(p.c_str());
    for (auto& c : x) c = towlower(c);
    return x;
}
bool IsPdf(const std::wstring& p) { return LowerExt(p) == L".pdf"; }
bool IsSvg(const std::wstring& p) { std::wstring x = LowerExt(p); return x == L".svg" || x == L".svgz"; }

// PNG/BMP de um IStream -> BGRA pré-multiplicado
bool DecodeStream(IStream* st, UINT& w, UINT& h, std::vector<BYTE>& px) {
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) || !wic) return false;
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
    bool ok = SUCCEEDED(wic->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
              SUCCEEDED(wic->CreateFormatConverter(&cv)) &&
              SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(cv->GetSize(&w, &h)) && w && h;
    if (ok) { px.resize((size_t)w * h * 4); ok = SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data())); }
    if (cv) cv->Release(); if (fr) fr->Release(); if (dec) dec->Release(); wic->Release();
    return ok;
}

// 1ª página do PDF pelo Windows.Data.Pdf (o mesmo da Pré-Visualização), com o lado maior em maxSide px, fundo branco.
// O WinRT espera com .get(): roda numa thread MTA própria (a do Quick Look é STA)
bool LoadPdf(const std::wstring& path, UINT maxSide, UINT& w, UINT& h, std::vector<BYTE>& px, UINT& pages) {
    bool ok = false;
    std::thread([&] {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            namespace wdp = winrt::Windows::Data::Pdf;
            namespace wss = winrt::Windows::Storage::Streams;
            auto file = winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(path).get();
            auto doc = wdp::PdfDocument::LoadFromFileAsync(file).get();
            pages = doc.PageCount();
            if (!pages) return;
            auto page = doc.GetPage(0);
            auto s = page.Size();
            wss::InMemoryRandomAccessStream ms;
            wdp::PdfPageRenderOptions opt;
            if (s.Width >= s.Height) opt.DestinationWidth(maxSide); else opt.DestinationHeight(maxSide);
            opt.BackgroundColor(winrt::Windows::UI::Color{ 255, 255, 255, 255 });
            page.RenderToStreamAsync(ms, opt).get();
            ms.Seek(0);
            IStream* st = nullptr;
            if (SUCCEEDED(CreateStreamOverRandomAccessStream(winrt::get_unknown(ms), IID_PPV_ARGS(&st))) && st) {
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                ok = DecodeStream(st, w, h, px);
                st->Release();
            }
        } catch (...) { ok = false; }
    }).join();
    return ok;
}

// SVG pelo SVG do Direct2D (ID2D1DeviceContext5), desenhado num bitmap WIC com o lado maior em maxSide px
bool LoadSvg(const std::wstring& path, UINT maxSide, UINT& w, UINT& h, std::vector<BYTE>& px) {
    IStream* st = nullptr;
    if (FAILED(SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &st)) || !st) return false;
    IWICImagingFactory* wic = nullptr;
    ID2D1Factory* fac = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &fac);
    bool ok = false;
    // o tamanho: viewBox, ou width/height; sem nada, 512 × 512
    auto measure = [&](float& vw, float& vh) {
        vw = vh = 512;
        IWICBitmap* b1 = nullptr; ID2D1RenderTarget* rt1 = nullptr; ID2D1DeviceContext5* dc = nullptr; ID2D1SvgDocument* doc = nullptr;
        if (wic && fac && SUCCEEDED(wic->CreateBitmap(8, 8, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &b1)) &&
            SUCCEEDED(fac->CreateWicBitmapRenderTarget(b1, D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &rt1)) &&
            SUCCEEDED(rt1->QueryInterface(IID_PPV_ARGS(&dc))) && SUCCEEDED(dc->CreateSvgDocument(st, D2D1::SizeF(512, 512), &doc))) {
            ID2D1SvgElement* root = nullptr; doc->GetRoot(&root);
            if (root) {
                D2D1_SVG_VIEWBOX vb{};
                if (SUCCEEDED(root->GetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &vb, sizeof(vb))) && vb.width > 0 && vb.height > 0) { vw = vb.width; vh = vb.height; }
                else {
                    D2D1_SVG_LENGTH lw{}, lh{};
                    if (SUCCEEDED(root->GetAttributeValue(L"width", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &lw, sizeof(lw))) && lw.units == D2D1_SVG_LENGTH_UNITS_NUMBER && lw.value > 0 &&
                        SUCCEEDED(root->GetAttributeValue(L"height", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &lh, sizeof(lh))) && lh.units == D2D1_SVG_LENGTH_UNITS_NUMBER && lh.value > 0) { vw = lw.value; vh = lh.value; }
                }
                root->Release();
            }
            ok = true;
        }
        if (doc) doc->Release(); if (dc) dc->Release(); if (rt1) rt1->Release(); if (b1) b1->Release();
    };
    float vw = 512, vh = 512;
    measure(vw, vh);
    if (ok) {
        ok = false;
        LARGE_INTEGER z{}; st->Seek(z, STREAM_SEEK_SET, nullptr);
        // ícones (16, 24 pt...) num tamanho bom de ver, como no Mac; desenhos grandes até o maxSide (issue #8: o SVG
        // abria minúsculo no canto)
        float side = std::min((float)maxSide, std::max(512.f, std::max(vw, vh) * 2));
        float k = side / std::max(vw, vh);
        w = std::max(1u, (UINT)std::lround(vw * k)); h = std::max(1u, (UINT)std::lround(vh * k));
        IWICBitmap* b = nullptr; ID2D1RenderTarget* rt = nullptr; ID2D1DeviceContext5* dc = nullptr; ID2D1SvgDocument* doc = nullptr;
        if (SUCCEEDED(wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &b)) &&
            SUCCEEDED(fac->CreateWicBitmapRenderTarget(b, D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &rt)) &&
            SUCCEEDED(rt->QueryInterface(IID_PPV_ARGS(&dc))) && SUCCEEDED(dc->CreateSvgDocument(st, D2D1::SizeF((float)w, (float)h), &doc))) {
            // o width/height do próprio arquivo mandava no tamanho do desenho (o viewport pedido era ignorado): com
            // viewBox (o tamanho original, se o arquivo não tem) e width/height = o bitmap, o desenho ocupa tudo
            if (ID2D1SvgElement* root = nullptr; (doc->GetRoot(&root), root)) {
                D2D1_SVG_VIEWBOX vb{};
                if (FAILED(root->GetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &vb, sizeof(vb))) || vb.width <= 0 || vb.height <= 0) {
                    vb = { 0, 0, vw, vh };
                    root->SetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &vb, sizeof(vb));
                }
                D2D1_SVG_LENGTH lw{ (float)w, D2D1_SVG_LENGTH_UNITS_NUMBER }, lh{ (float)h, D2D1_SVG_LENGTH_UNITS_NUMBER };
                root->SetAttributeValue(L"width", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &lw, sizeof(lw));
                root->SetAttributeValue(L"height", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &lh, sizeof(lh));
                root->Release();
            }
            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(0, 0, 0, 0));
            dc->DrawSvgDocument(doc);
            ok = SUCCEEDED(dc->EndDraw());
        }
        if (ok) { px.resize((size_t)w * h * 4); ok = SUCCEEDED(b->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data())); }
        if (doc) doc->Release(); if (dc) dc->Release(); if (rt) rt->Release(); if (b) b->Release();
    }
    if (fac) fac->Release(); if (wic) wic->Release(); st->Release();
    return ok;
}

// ---- Word, PowerPoint, Excel (e fontes): o "preview handler" do Windows, se o PC tiver um para o tipo (o Office
// instala os dele). Ele desenha numa janela comum por cima do vidro (g_ph), como o vídeo; sem handler, fica o ícone.
IPreviewHandler* g_ph;
HWND g_phWnd;
bool g_handler;  // a prévia atual é de um preview handler
bool g_skipHandler;  // o handler falhou no DoPreview: carrega de novo sem ele (ícone)
bool WantsHandler(const std::wstring& p, CLSID* out) {
    std::wstring x = LowerExt(p);
    bool want = false;
    for (auto e : { L".doc", L".docx", L".docm", L".dot", L".dotx", L".odt", L".rtf", L".ppt", L".pptx", L".pptm", L".pps", L".ppsx", L".odp",
                    L".xls", L".xlsx", L".xlsm", L".ods", L".vsd", L".vsdx", L".pub", L".ttf", L".otf", L".ttc" })
        if (x == e) want = true;
    if (!want) return false;
    wchar_t s[64] = {}; DWORD n = 64;
    if (FAILED(AssocQueryStringW(ASSOCF_INIT_DEFAULTTOSTAR, ASSOCSTR_SHELLEXTENSION, x.c_str(), L"{8895b1c6-b41f-4c1c-a562-0d564250836f}", s, &n)) || !s[0]) return false;
    return SUCCEEDED(CLSIDFromString(s, out));
}
void HandlerStop() {
    if (g_ph) { g_ph->Unload(); g_ph->Release(); g_ph = nullptr; }
    if (g_phWnd) ShowWindow(g_phWnd, SW_HIDE);
    g_handler = false;
}
LRESULT CALLBACK PhProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) { RECT r; GetClientRect(h, &r); FillRect((HDC)w, &r, (HBRUSH)GetStockObject(WHITE_BRUSH)); return 1; }
    return DefWindowProcW(h, m, w, l);
}

// os apps do Mac do projeto para o tipo (como no Mac: Pré-Visualização abre imagens e PDF, TextEdit o texto)
std::wstring g_openExe;
std::wstring OwnApp(const std::wstring& p, std::wstring& name) {
    std::wstring x = PathFindExtensionW(p.c_str());
    for (auto& c : x) c = towlower(c);
    const wchar_t* exe = nullptr;
    for (auto e : { L".pdf", L".png", L".jpg", L".jpeg", L".gif", L".bmp", L".tif", L".tiff", L".webp", L".heic", L".ico" }) if (x == e) { exe = EXE_PREVIEW; name = Tr(L"Pré-Visualização"); }
    for (auto e : { L".txt", L".rtf", L".md", L".log" }) if (x == e) { exe = EXE_TEXTEDIT; name = L"TextEdit"; }
    for (auto e : { L".mp3", L".m4a", L".flac", L".wav", L".aac", L".ogg", L".wma", L".aif", L".aiff", L".opus" }) if (x == e) { exe = EXE_MUSIC; name = Tr(L"Música"); }
    for (auto e : { L".mp4", L".mov", L".m4v", L".mkv", L".avi", L".wmv", L".webm" }) if (x == e) { exe = EXE_QUICKTIME; name = L"QuickTime Player"; }
    if (!exe) return L"";
    // "Apps do Windows": o app padrão do Windows (o nome dele vem de DefaultAppName)
    if (shell::UsarWindows((!wcscmp(exe, EXE_MUSIC) || !wcscmp(exe, EXE_QUICKTIME)) ? L"midia" : L"visualizar")) return L"";
    std::wstring full = shell::dir + L"\\" + exe;
    return GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES ? full : L"";
}
void OpenPath(const std::wstring& p) {
    if (!g_openExe.empty()) shell::Abrir(L"open", g_openExe.c_str(), (L"\"" + p + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    else if (!shell::OpenCalculator(p)) shell::Abrir(L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::wstring DefaultAppName(const std::wstring& p) {
    std::wstring own;
    g_openExe = OwnApp(p, own);
    if (!g_openExe.empty()) return own;
    std::wstring x = PathFindExtensionW(p.c_str());
    wchar_t name[256] = {}; DWORD n = 256;
    if (!x.empty() && SUCCEEDED(AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_FRIENDLYAPPNAME, x.c_str(), nullptr, name, &n)) && name[0]) return name;
    return L"";
}

// tamanho do painel (DIP) para a prévia: proporção da imagem, até 62% da área de trabalho
SIZE PanelSize(float sc) {
    MONITORINFO mi{ sizeof(mi) };
    POINT c; GetCursorPos(&c);
    GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY), &mi);
    float W = (mi.rcWork.right - mi.rcWork.left) / sc * 0.62f, H = (mi.rcWork.bottom - mi.rcWork.top) / sc * 0.72f - BAR;
    float w = 560, h = 420;
    if (!g_text.empty()) { w = std::min(W, 720.f); h = std::min(H, 760.f); }
    else if (g_handler) { w = std::min(W, 760.f); h = std::min(H, 580.f); }  // Word/PowerPoint: folha de tamanho fixo
    else if (g_iw && g_ih) {
        float k = std::min(W / g_iw, H / g_ih);
        k = std::min(k, 1.6f / sc);  // imagem pequena não estoura
        // vídeo: pelo menos 420 de largura, para a barra do QuickTime caber (vídeo em pé deixava o painel estreito)
        w = std::max(g_media ? 420.f : 360.f, g_iw * k); h = std::max(240.f, g_ih * k);
    }
    if (liquid::Golden() && g_isIcon && g_media) return { 540, 520 };  // música: o ícone e a barra embaixo
    if (liquid::Golden() && g_isIcon) return { 540, 440 };  // medido na captura do usuário (19/11/2026)
    if (liquid::Golden()) return { (LONG)std::lround(w + 10), (LONG)std::lround(h + BAR + 5) };  // moldura de 5
    return { (LONG)std::lround(w), (LONG)std::lround(h + BAR) };
}

std::wstring DateText(const FILETIME& ft) {
    FILETIME lf; SYSTEMTIME st;
    FileTimeToLocalFileTime(&ft, &lf); FileTimeToSystemTime(&lf, &st);
    return idioma::DateTime(st, idioma::D_SHORT);  // "20 de nov. de 2026 às 14:30", "Nov 20, 2026 at 14:30"
}

std::wstring SizeText(ULONGLONG n) {  // como o Mac: unidades de 1000
    wchar_t b[48];
    if (n < 1000) swprintf_s(b, L"%llu bytes", n);
    else if (n < 1000ull * 1000) swprintf_s(b, L"%.0f KB", n / 1000.0);
    else if (n < 1000ull * 1000 * 1000) swprintf_s(b, L"%.1f MB", n / 1e6);
    else swprintf_s(b, L"%.2f GB", n / 1e9);
    return b;
}

// as linhas de baixo do ícone: tipo · itens (ou tamanho) e a data de modificação
void LoadInfo(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    g_kind = g_info = g_date = L"";
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return;
    bool dir = (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (dir) {
        g_kind = Tr(L"Pasta");
        size_t n = 0;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((p + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.cFileName[0] == L'.' || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
                n++;
            } while (FindNextFileW(h, &fd) && n < 100000);
            FindClose(h);
        }
        g_info = g_kind + L" · " + std::to_wstring(n) + (n == 1 ? std::wstring(Tr(L" item")) : std::wstring(Tr(L" itens")));
    } else {
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW(p.c_str(), 0, &sfi, sizeof(sfi), SHGFI_TYPENAME) && sfi.szTypeName[0]) g_kind = sfi.szTypeName;
        else g_kind = Tr(L"Documento");
        g_info = g_kind + L" · " + SizeText(((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
    }
    g_date = std::wstring(Tr(L"Modificado: ")) + DateText(fa.ftLastWriteTime);
}

// macOS 27 (capturas Finder-Quick-Look e a do usuário de 19/11/2026, 1x): painel de vidro fosco (o dos menus de
// contexto), cantos 16, sem barra separada; fechar = círculo cinza de 16 com o × escuro, a 16 da borda e 19 do topo;
// nome em negrito de 15 a 33 da borda; compartilhar e "Abrir" numa cápsula de 26 (cantos 13) a 8 da borda.
// Sem prévia: ícone de 200 com o centro a 210 do topo, nome em negrito a 311, "Pasta · 5 itens" a 354 e a data a 372.
// Com prévia: a foto (cantos 12) numa moldura de 5 do próprio vidro.
void DrawGolden(ID2D1RenderTarget* rt, float W, float H, bool dark, bool glass) {
    ID2D1SolidColorBrush* br = nullptr;
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
    if (!br) return;
    ID2D1Factory* fac = nullptr; rt->GetFactory(&fac);  // a geometria tem que ser da fábrica do alvo (o vidro tem a dele)
    if (!fac) { br->Release(); return; }
    const float RR = GG_R, M = 5;
    D2D1_RECT_F all = D2D1::RectF(0, 0, W, H);
    if (!glass) {  // sem vidro (falhou): o fundo opaco na mesma cor
        br->SetColor(dark ? D2D1::ColorF(0.17f, 0.165f, 0.165f, 1) : D2D1::ColorF(0.95f, 0.945f, 0.94f, 1));
        rt->FillRoundedRectangle(D2D1::RoundedRect(all, RR, RR), br);
    }
    // borda: só o fio claro por dentro (o vidro é o próprio painel, sem gradiente) e um contorno bem leve
    br->SetColor(D2D1::ColorF(1, 1, 1, dark ? 0.16f : 0.6f));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(1, 1, W - 1, H - 1), RR - 1, RR - 1), br, 1);
    br->SetColor(dark ? D2D1::ColorF(0, 0, 0, 0.5f) : D2D1::ColorF(0, 0, 0, 0.12f));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.25f, 0.25f, W - 0.25f, H - 0.25f), RR - 0.25f, RR - 0.25f), br, 0.5f);
    // cores da captura: texto (67,62,56), ícones (61,51,48), botões redondos (89,70,50) sobre o vidro
    D2D1_COLOR_F text = dark ? D2D1::ColorF(1, 1, 1, 0.9f) : D2D1::ColorF(0, 0, 0, 0.76f);
    D2D1_COLOR_F text2 = dark ? D2D1::ColorF(1, 1, 1, 0.55f) : D2D1::ColorF(0, 0, 0, 0.5f);
    D2D1_COLOR_F icon = dark ? D2D1::ColorF(1, 1, 1, 0.82f) : D2D1::ColorF(0, 0, 0, 0.74f);
    const float cy = 19.5f;
    bool canFull = !g_isIcon;  // pasta e ícone: sem tela cheia (o nome vem logo depois do fechar)
    bool image = !g_isIcon && g_text.empty() && !g_px.empty() && !g_media;
    // fechar e tela cheia: círculos cheios de 13 com o símbolo vazado, a 18 e 38 da borda
    auto roundBtn = [&](D2D1_POINT_2F c, bool hot) {
        br->SetColor(dark ? D2D1::ColorF(1, 1, 1, hot ? 0.75f : 0.6f) : D2D1::ColorF(0.30f, 0.23f, 0.16f, hot ? 0.95f : 0.82f));
        rt->FillEllipse(D2D1::Ellipse(c, 6.5f, 6.5f), br);
        br->SetColor(dark ? D2D1::ColorF(0.16f, 0.16f, 0.16f) : D2D1::ColorF(0.95f, 0.93f, 0.90f));
    };
    D2D1_POINT_2F cc{ 18, cy };
    g_closeR = D2D1::RectF(cc.x - 9, cc.y - 10, cc.x + 9, cc.y + 10);
    roundBtn(cc, g_hoverClose);
    rt->DrawLine({ cc.x - 2.4f, cc.y - 2.4f }, { cc.x + 2.4f, cc.y + 2.4f }, br, 1.6f);
    rt->DrawLine({ cc.x - 2.4f, cc.y + 2.4f }, { cc.x + 2.4f, cc.y - 2.4f }, br, 1.6f);
    g_fullR = D2D1::RectF(0, 0, 0, 0);
    if (canFull) {
        D2D1_POINT_2F fc{ 38, cy };
        g_fullR = D2D1::RectF(fc.x - 10, fc.y - 10, fc.x + 10, fc.y + 10);
        roundBtn(fc, g_hoverFull);
        // arrow.up.left.and.arrow.down.right (o SF Symbol do Mac): duas setinhas em diagonal, as pontas em "L" nos
        // cantos; em tela cheia, viradas para dentro (arrow.down.right.and.arrow.up.left). Antes era uma barra só
        ID2D1StrokeStyle* ss = nullptr;
        fac->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &ss);
        for (int s = -1; s <= 1; s += 2) {  // -1: a de cima à esquerda; 1: a de baixo à direita
            float fora = 3.3f * s, perto = 0.9f * s;  // ponta e base da seta, a partir do centro
            float tip = g_full ? perto : fora, base = g_full ? fora : perto, d = 2.3f * (g_full ? -s : s);
            rt->DrawLine({ fc.x + base, fc.y + base }, { fc.x + tip, fc.y + tip }, br, 1.25f, ss);
            rt->DrawLine({ fc.x + tip, fc.y + tip }, { fc.x + tip - d, fc.y + tip }, br, 1.25f, ss);
            rt->DrawLine({ fc.x + tip, fc.y + tip }, { fc.x + tip, fc.y + tip - d }, br, 1.25f, ss);
        }
        if (ss) ss->Release();
    }
    // "Abrir com <app>": cápsula de 24 a 7,5 da borda, texto normal de 13 com 12,5 de cada lado
    static IDWriteTextFormat* tfBtn = ui::Format(13, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (tfBtn) tfBtn->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    bool folder = g_kind == Tr(L"Pasta");
    std::wstring btn = g_app.empty() || folder ? std::wstring(Tr(L"Abrir")) : std::wstring(Tr(L"Abrir com ")) + g_app;
    float bw = std::max(56.f, ui::TextWidth(tfBtn, btn) + 25);
    g_openR = D2D1::RectF(W - 7.5f - bw, cy - 12, W - 7.5f, cy + 12);
    br->SetColor(dark ? D2D1::ColorF(1, 1, 1, g_hoverOpen ? 0.17f : 0.10f) : D2D1::ColorF(0, 0, 0, g_hoverOpen ? 0.10f : 0.05f));
    rt->FillRoundedRectangle(D2D1::RoundedRect(g_openR, 12, 12), br);
    br->SetColor(text);
    rt->DrawTextW(btn.c_str(), (UINT32)btn.size(), tfBtn, g_openR, br);
    // ícones à esquerda da cápsula, de 40 em 40: compartilhar (24 antes dela), girar e marcação (só imagens)
    auto path = [&](auto build, float w) {
        ID2D1PathGeometry* g = nullptr;
        if (FAILED(fac->CreatePathGeometry(&g))) return;
        ID2D1GeometrySink* s = nullptr;
        if (SUCCEEDED(g->Open(&s))) { build(s); s->Close(); s->Release(); }
        rt->DrawGeometry(g, br, w);
        g->Release();
    };
    auto hoverBg = [&](D2D1_RECT_F r, bool hot) {
        if (!hot) return;
        br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.1f) : D2D1::ColorF(0, 0, 0, 0.06f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, 12, 12), br);
    };
    float sx = g_openR.left - 24;
    g_shareR = D2D1::RectF(sx - 16, cy - 12, sx + 16, cy + 12);
    hoverBg(g_shareR, g_hoverShare);
    br->SetColor(icon);
    path([&](ID2D1GeometrySink* s) {  // caixa aberta em cima (11 × 9) e a seta
        s->BeginFigure({ sx - 2.5f, cy - 2.5f }, D2D1_FIGURE_BEGIN_HOLLOW);
        s->AddLine({ sx - 4.5f, cy - 2.5f });
        s->AddArc(D2D1::ArcSegment({ sx - 5.5f, cy - 1.5f }, { 1, 1 }, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        s->AddLine({ sx - 5.5f, cy + 6.f });
        s->AddArc(D2D1::ArcSegment({ sx - 4.5f, cy + 7.f }, { 1, 1 }, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        s->AddLine({ sx + 4.5f, cy + 7.f });
        s->AddArc(D2D1::ArcSegment({ sx + 5.5f, cy + 6.f }, { 1, 1 }, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        s->AddLine({ sx + 5.5f, cy - 1.5f });
        s->AddArc(D2D1::ArcSegment({ sx + 4.5f, cy - 2.5f }, { 1, 1 }, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        s->AddLine({ sx + 2.5f, cy - 2.5f });
        s->EndFigure(D2D1_FIGURE_END_OPEN);
    }, 1.2f);
    rt->DrawLine({ sx, cy + 2.5f }, { sx, cy - 7.5f }, br, 1.2f);
    path([&](ID2D1GeometrySink* s) {
        s->BeginFigure({ sx - 2.8f, cy - 4.8f }, D2D1_FIGURE_BEGIN_HOLLOW);
        s->AddLine({ sx, cy - 7.6f }); s->AddLine({ sx + 2.8f, cy - 4.8f });
        s->EndFigure(D2D1_FIGURE_END_OPEN);
    }, 1.2f);
    g_rotR = g_markR = D2D1::RectF(0, 0, 0, 0);
    if (image) {
        float rx = sx - 39;
        g_rotR = D2D1::RectF(rx - 16, cy - 12, rx + 16, cy + 12);
        hoverBg(g_rotR, g_hoverRot);
        br->SetColor(icon);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rx - 5.5f, cy - 3.f, rx + 3.5f, cy + 6.f), 1.3f, 1.3f), br, 1.2f);
        path([&](ID2D1GeometrySink* s) {  // a seta curva em cima, girando para a esquerda
            s->BeginFigure({ rx + 6.f, cy - 1.f }, D2D1_FIGURE_BEGIN_HOLLOW);
            s->AddArc(D2D1::ArcSegment({ rx + 1.5f, cy - 6.f }, { 4.5f, 5.f }, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            s->EndFigure(D2D1_FIGURE_END_OPEN);
            s->BeginFigure({ rx + 3.6f, cy - 8.3f }, D2D1_FIGURE_BEGIN_HOLLOW);
            s->AddLine({ rx + 1.3f, cy - 6.f }); s->AddLine({ rx + 3.6f, cy - 3.8f });
            s->EndFigure(D2D1_FIGURE_END_OPEN);
        }, 1.2f);
        float mx = rx - 41;
        g_markR = D2D1::RectF(mx - 16, cy - 12, mx + 16, cy + 12);
        hoverBg(g_markR, g_hoverMark);
        br->SetColor(icon);
        rt->DrawEllipse(D2D1::Ellipse({ mx, cy }, 6.3f, 6.3f), br, 1.2f);
        path([&](ID2D1GeometrySink* s) {  // a ponta da caneta
            s->BeginFigure({ mx - 2.9f, cy + 5.4f }, D2D1_FIGURE_BEGIN_HOLLOW);
            s->AddLine({ mx - 2.9f, cy + 1.2f }); s->AddLine({ mx, cy - 4.2f }); s->AddLine({ mx + 2.9f, cy + 1.2f }); s->AddLine({ mx + 2.9f, cy + 5.4f });
            s->EndFigure(D2D1_FIGURE_END_OPEN);
        }, 1.2f);
        rt->FillEllipse(D2D1::Ellipse({ mx, cy - 0.6f }, 0.9f, 0.9f), br);
    }
    float iconsLeft = image ? g_markR.left : g_shareR.left;
    // nome: negrito de 13, a 53,5 da borda (33 sem o botão de tela cheia)
    static IDWriteTextFormat* tfT = ui::Format(13, DWRITE_FONT_WEIGHT_BOLD);
    if (tfT) {
        tfT->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        tfT->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 }; tfT->SetTrimming(&tr, nullptr);
        br->SetColor(text);
        rt->DrawTextW(g_name.c_str(), (UINT32)g_name.size(), tfT, D2D1::RectF(canFull ? 53.5f : 33.f, cy - 11, iconsLeft - 8, cy + 11), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    if (g_bmpRt != rt && g_bmp) { g_bmp->Release(); g_bmp = nullptr; }
    if (!g_bmp && !g_px.empty()) { g_bmp = ui::Bitmap(rt, g_iw, g_ih, g_px); g_bmpRt = rt; }
    if (g_hasVideo || g_handler) {  // vídeo: a moldura preta (o MFPlay desenha em g_vid por cima); Office: a folha branca
        br->SetColor(g_handler ? D2D1::ColorF(1, 1, 1, 1) : D2D1::ColorF(0, 0, 0, 1));
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(M, BAR, W - M, H - M), RR - M, RR - M), br);
        br->Release(); fac->Release();
        return;
    }
    if (g_isIcon || (g_text.empty() && !g_bmp)) {  // sem prévia: ícone grande e informações, direto no vidro
        float k = g_media ? 1.f : H / 440;  // música: posições fixas (a barra fica embaixo)
        float nameY = 311 * k;  // o nome fica abaixo do ícone desenhado (os de documento são altos e encostavam nele)
        if (g_bmp) {
            float s = std::min(200.f / g_iw, 200.f / g_ih), w = g_iw * s, h = g_ih * s, x = W / 2, y = 210 * k;
            nameY = std::max(nameY, y + h / 2 + 18);
            rt->DrawBitmap(g_bmp, D2D1::RectF(x - w / 2, y - h / 2, x + w / 2, y + h / 2), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        static IDWriteTextFormat* tfN = ui::Format(14, DWRITE_FONT_WEIGHT_BOLD, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        static IDWriteTextFormat* tfI = ui::Format(11.5f, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        for (auto* f : { tfN, tfI }) if (f) { f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 }; f->SetTrimming(&tr, nullptr); }
        br->SetColor(text);
        if (tfN) rt->DrawTextW(g_name.c_str(), (UINT32)g_name.size(), tfN, D2D1::RectF(20, nameY - 11, W - 20, nameY + 11), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        br->SetColor(text2);
        if (tfI) {
            rt->DrawTextW(g_info.c_str(), (UINT32)g_info.size(), tfI, D2D1::RectF(20, nameY + 43 - 9, W - 20, nameY + 43 + 9), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            rt->DrawTextW(g_date.c_str(), (UINT32)g_date.size(), tfI, D2D1::RectF(20, nameY + 61 - 9, W - 20, nameY + 61 + 9), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        br->Release(); fac->Release();
        return;
    }
    D2D1_RECT_F body = D2D1::RectF(M, BAR, W - M, H - M);
    ID2D1RoundedRectangleGeometry* clip = nullptr;
    fac->CreateRoundedRectangleGeometry(D2D1::RoundedRect(body, RR - M, RR - M), &clip);
    if (clip) rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip), nullptr);
    if (!g_text.empty()) {  // texto: folha dentro da moldura
        br->SetColor(dark ? D2D1::ColorF(0.1f, 0.1f, 0.1f) : D2D1::ColorF(1, 1, 1));
        rt->FillRectangle(body, br);
        br->SetColor(text);
        DrawTexto(rt, br, body, 18, 14, dark);
    } else if (g_bmp) {  // a foto preenche a moldura
        // tela cheia: a foto inteira (o vidro aparece nas sobras); normal: o painel já tem a proporção da foto.
        // Com zoom, amplia em volta do ponteiro e o arrasto move (PhotoRect)
        g_bodyR = body; g_bodyFill = !g_full;
        rt->DrawBitmap(g_bmp, PhotoRect(body, g_iw, g_ih, !g_full), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    if (clip) { rt->PopLayer(); clip->Release(); }
    br->Release();
    fac->Release();
}

void PaintGolden(ID2D1RenderTarget* rt, RECT wr, int wpx, int hpx, float sc, bool dark) {
    float W = wpx / sc, H = hpx / sc;
    if (g_glass) {
        // o vidro dos menus de contexto: fosco, não segue o controle de transparência
        // vidro bem fosco e com tinta firme (a dos menus deixava o painel transparente demais): o fundo só aparece
        // como cor, sem dar para ler o que está atrás, como no Quick Look do macOS 27
        D2D1_COLOR_F tint = dark ? D2D1::ColorF(0.16f, 0.155f, 0.155f, 0.86f) : D2D1::ColorF(0.95f, 0.945f, 0.94f, 0.86f);
        backdrop::SetPieces(g_back, (float)wpx, (float)hpx, { { D2D1::RectF(0, 0, (float)wpx, (float)hpx), GG_R * sc } }, tint, 32.f);
        if (ID2D1DeviceContext* dc = backdrop::ContentBegin(g_back, wpx, hpx, sc)) {
            DrawGolden(dc, W, H, dark, true);
            backdrop::ContentEnd(g_back);
            ApplyFade();
            // a janela de cima só pega o mouse: um véu de 1/255 (pixel transparente deixaria o clique passar)
            ID2D1SolidColorBrush* b = nullptr;
            rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1.f / 255), &b);
            if (b) { rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, W, H), GG_R, GG_R), b); b->Release(); }
            ui::End(g_s, true, POINT{ wr.left, wr.top });
            return;
        }
    }
    DrawGolden(rt, W, H, dark, false);
    ui::End(g_s, true, POINT{ wr.left, wr.top });
}

void PaintCtl();
bool g_ctlRestack = true;  // a barra precisa voltar para cima do vídeo (o painel mexeu): só aí mexe na ordem das janelas
// o vídeo e a barra ficam por cima do painel no mesmo "sempre visível" dele (por cima do Finder, o painel é
// sempre visível; um HWND_TOP comum deixava o vídeo atrás do painel, só com a moldura preta à vista)
UINT PlaceOver(HWND x) {
    bool top = (GetWindowLongW(g_wnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    if (!top && (GetWindowLongW(x, GWL_EXSTYLE) & WS_EX_TOPMOST))
        SetWindowPos(x, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return 0;
}
HWND OverAfter() { return (GetWindowLongW(g_wnd, GWL_EXSTYLE) & WS_EX_TOPMOST) ? HWND_TOPMOST : HWND_TOP; }
void PlaceHandler() {  // a folha do Word/PowerPoint por cima do corpo do painel (como o vídeo)
    if (!g_handler || !g_phWnd || !g_wnd || !IsWindowVisible(g_wnd)) return;
    float sc = Sc();
    RECT wr; GetWindowRect(g_wnd, &wr);
    bool gg = liquid::Golden();
    int m = gg ? (int)std::lround(5 * sc) : 0, top = (int)std::lround(BAR * sc);
    int w = wr.right - wr.left - 2 * m, h = wr.bottom - wr.top - top - m;
    if (w <= 0 || h <= 0) return;
    int rr = (int)std::lround(2 * ((gg ? GG_R : R) - (gg ? 5 : 0)) * sc);
    SetWindowRgn(g_phWnd, CreateRoundRectRgn(0, 0, w + 1, h + 1, gg ? rr : 0, gg ? rr : 0), FALSE);
    PlaceOver(g_phWnd);
    SetWindowPos(g_phWnd, OverAfter(), wr.left + m, wr.top + top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (g_ph) { RECT rc{ 0, 0, w, h }; g_ph->SetRect(&rc); }
}
void PlaceMedia() {
    PlaceHandler();
    if (!g_media || !g_wnd || !IsWindowVisible(g_wnd)) return;
    RECT wr; GetWindowRect(g_wnd, &wr);
    // Arrastando (só mudou o lugar): o vídeo, a barra e o vidro dela andam juntos, numa só troca de posição, sem refazer
    // o recorte nem redesenhar a barra (issue #8: com o vídeo aberto, a barra tremia e o arrasto pesava)
    static RECT last{};
    bool sameSize = last.right - last.left == wr.right - wr.left && last.bottom - last.top == wr.bottom - wr.top;
    if (sameSize && (last.left != wr.left || last.top != wr.top) && last.right > last.left) {
        int dx = wr.left - last.left, dy = wr.top - last.top;
        last = wr;
        HDWP dw = BeginDeferWindowPos(3);
        for (HWND x : { g_vid, g_ctlBack, g_ctl }) {
            if (!dw || !x || !IsWindowVisible(x)) continue;
            RECT r; GetWindowRect(x, &r);
            dw = DeferWindowPos(dw, x, nullptr, r.left + dx, r.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (dw) EndDeferWindowPos(dw);
        return;
    }
    last = wr;
    g_ctlRestack = true;
    float sc = Sc();
    int m = (int)std::lround(5 * sc), top = (int)std::lround(BAR * sc);
    if (g_hasVideo && g_vid) {
        int w = wr.right - wr.left - 2 * m, h = wr.bottom - wr.top - top - m;
        if (w > 0 && h > 0) {
            int rr = (int)std::lround(2 * (GG_R - 5) * sc);
            SetWindowRgn(g_vid, CreateRoundRectRgn(0, 0, w + 1, h + 1, rr, rr), FALSE);
            PlaceOver(g_vid);
            SetWindowPos(g_vid, OverAfter(), wr.left + m, wr.top + top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            if (g_mp) g_mp->UpdateVideo();
        }
    }
    PaintCtl();
}
void SyncBack() {  // o vidro acompanha a janela (mesmo lugar, logo abaixo dela)
    PlaceMedia();
    if (!g_back) return;
    if (!liquid::Golden() || !g_wnd || !IsWindowVisible(g_wnd)) { ShowWindow(g_back, SW_HIDE); return; }
    RECT r; GetWindowRect(g_wnd, &r);
    // o vidro acompanha o "sempre visível" do painel (não é janela dona dele: o Windows não faz isso sozinho)
    bool top = (GetWindowLongW(g_wnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    if (top != ((GetWindowLongW(g_back, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0))
        SetWindowPos(g_back, top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(g_back, g_wnd, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

bool CtlVisible() { return g_media && (!g_hasVideo || g_scrub || !MPlaying() || GetTickCount() - g_ctlMoveAt < 2500); }
void PaintCtl() {
    if (!g_ctl) return;
    if (!CtlVisible() || !g_wnd || !IsWindowVisible(g_wnd)) { ShowWindow(g_ctl, SW_HIDE); if (g_ctlBack) ShowWindow(g_ctlBack, SW_HIDE); return; }
    float sc = Sc();
    RECT wr; GetWindowRect(g_wnd, &wr);
    float PW = (wr.right - wr.left) / sc;
    float W = std::min(barravideo::LARGURA, PW - 10 - 40), H = barravideo::ALTURA;
    if (W < 300) { ShowWindow(g_ctl, SW_HIDE); if (g_ctlBack) ShowWindow(g_ctlBack, SW_HIDE); return; }
    int w = (int)std::lround(W * sc), h = (int)std::lround(H * sc);
    int x = wr.left + ((wr.right - wr.left) - w) / 2, y = wr.bottom - (int)std::lround((5 + CTL_DOWN) * sc) - h;
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
    void* bits = nullptr; HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    ID2D1DCRenderTarget* rt2 = nullptr;
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * sc, 96 * sc);
    g_vhits.clear();
    if (SUCCEEDED(ui::d2d->CreateDCRenderTarget(&props, &rt2))) {
        RECT rb{ 0, 0, w, h }; rt2->BindDC(mem, &rb);
        rt2->BeginDraw(); rt2->Clear(D2D1::ColorF(0, 0, 0, 0));
        ID2D1SolidColorBrush* b = nullptr; rt2->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0), &b);
        static IDWriteTextFormat* tfL = nullptr; static IDWriteTextFormat* tfR = nullptr;
        if (!tfL) {
            tfL = ui::Format(13, DWRITE_FONT_WEIGHT_NORMAL); tfR = ui::Format(13, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_TEXT_ALIGNMENT_TRAILING);
            for (auto* f : { tfL, tfR }) if (f) { f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
        }
        if (b && g_hasVideo) barravideo::FundoBorrado(rt2, g_vid, RECT{ x, y, x + w, y + h }, sc, D2D1::RectF(0, 0, W, H), 22);  // o vídeo desfocado atrás
        if (b) { barravideo::Desenha(rt2, b, W, H, g_vol, MPlaying(), g_ended ? MLen() : MPos(), MLen(), tfL, tfR, g_vhits); b->Release(); }
        rt2->EndDraw(); rt2->Release();
    }
    POINT dst{ x, y }, src{ 0, 0 }; SIZE sz{ w, h };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_ctl, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr, screen);
    // A ordem das janelas só muda quando a barra aparece ou o painel mexeu. Antes ia para cima a cada quadro (15×/s),
    // e cada troca de ordem fazia o Shell remontar o vidro real e as sombras: CPU alta com o vídeo aberto (issue #8)
    if (!IsWindowVisible(g_ctl) || g_ctlRestack) {
        g_ctlRestack = false;
        if (g_ctlBack) {  // o vidro fosco de trás, 4,5 pt para dentro (o acrílico não aceita recorte)
            int in = (int)std::lround(4.5f * sc);
            SetWindowPos(g_ctlBack, HWND_TOP, x + in, y + in, w - 2 * in, h - 2 * in, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        PlaceOver(g_ctl);
        SetWindowPos(g_ctl, OverAfter(), 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

void Paint() {
    if (!g_wnd || !IsWindowVisible(g_wnd)) return;
    float sc = Sc();
    RECT wr; GetWindowRect(g_wnd, &wr);
    int wpx = wr.right - wr.left, hpx = wr.bottom - wr.top;
    auto* rt = ui::Begin(g_s, wpx, hpx, sc);
    if (!rt) return;
    float W = wpx / sc, H = hpx / sc;
    bool dark = ui::Dark();
    ID2D1SolidColorBrush* br = nullptr;
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
    if (liquid::Golden()) { br->Release(); PaintGolden(rt, wr, wpx, hpx, sc, dark); return; }
    if (liquid::Leopard()) {
        // Leopard: o Quick Look do 10.5 — HUD preto translúcido de cantos redondos com o fio claro, o título branco no
        // meio da faixa de cima (24), o X branco num círculo cinza à esquerda e a prévia no corpo. Sem "Abrir com".
        const float LB = 24, LR = 10;
        br->SetColor(D2D1::ColorF(0.07f, 0.07f, 0.08f, 0.90f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), LR, LR), br);
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.30f));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), LR, LR), br, 1);
        g_openR = D2D1::RectF(0, 0, 0, 0);
        g_closeR = D2D1::RectF(5, 4, 21, 20);
        D2D1_POINT_2F cc{ 13, 12 };
        br->SetColor(g_hoverClose ? D2D1::ColorF(0.55f, 0.55f, 0.57f, 1) : D2D1::ColorF(0.42f, 0.42f, 0.44f, 1));
        rt->FillEllipse(D2D1::Ellipse(cc, 6.5f, 6.5f), br);
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.55f));
        rt->DrawEllipse(D2D1::Ellipse(cc, 6.5f, 6.5f), br, 0.8f);
        br->SetColor(D2D1::ColorF(0.07f, 0.07f, 0.08f, 1));
        rt->DrawLine({ cc.x - 2.4f, cc.y - 2.4f }, { cc.x + 2.4f, cc.y + 2.4f }, br, 1.5f);
        rt->DrawLine({ cc.x - 2.4f, cc.y + 2.4f }, { cc.x + 2.4f, cc.y - 2.4f }, br, 1.5f);
        static IDWriteTextFormat* tfT = [] { auto* f = ui::FormatFace(L"Lucida Sans Unicode", 12, DWRITE_FONT_WEIGHT_NORMAL); if (f) f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); return f; }();
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.92f));
        rt->DrawTextW(g_name.c_str(), (UINT32)g_name.size(), tfT, D2D1::RectF(30, 0, W - 30, LB), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        D2D1_RECT_F body = D2D1::RectF(8, LB, W - 8, H - 8);
        rt->PushAxisAlignedClip(body, D2D1_ANTIALIAS_MODE_ALIASED);
        if (!g_text.empty()) {  // texto: a folha branca
            br->SetColor(D2D1::ColorF(1, 1, 1, 1));
            rt->FillRectangle(body, br);
            br->SetColor(D2D1::ColorF(0, 0, 0, 0.9f));
            DrawTexto(rt, br, body, 14, 10, false);
        } else {
            if (g_bmpRt != rt && g_bmp) { g_bmp->Release(); g_bmp = nullptr; }
            if (!g_bmp && !g_px.empty()) { g_bmp = ui::Bitmap(rt, g_iw, g_ih, g_px); g_bmpRt = rt; }
            if (g_bmp) {
                g_bodyR = body; g_bodyFill = false;
                rt->DrawBitmap(g_bmp, PhotoRect(body, g_iw, g_ih, false), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else {
                br->SetColor(D2D1::ColorF(1, 1, 1, 0.55f));
                const wchar_t* t = g_loading ? Tr(L"Carregando…") : Tr(L"Sem prévia");
                rt->DrawTextW(t, (UINT32)wcslen(t), tfT, D2D1::RectF(body.left, (body.top + body.bottom) / 2 - 10, body.right, (body.top + body.bottom) / 2 + 10), br);
            }
        }
        rt->PopAxisAlignedClip();
        br->Release();
        ui::End(g_s, true, POINT{ wr.left, wr.top });
        return;
    }
    // corpo e barra
    br->SetColor(dark ? D2D1::ColorF(0.12f, 0.12f, 0.13f, 0.97f) : D2D1::ColorF(0.97f, 0.97f, 0.97f, 0.98f));
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), R, R), br);
    br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.16f) : D2D1::ColorF(0, 0, 0, 0.16f));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), R, R), br, 1);
    br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.08f) : D2D1::ColorF(0, 0, 0, 0.08f));
    rt->DrawLine({ 0, BAR }, { W, BAR }, br, 1);
    // fechar (círculo com x, como o do Quick Look)
    g_closeR = D2D1::RectF(12, BAR / 2 - 11, 34, BAR / 2 + 11);
    D2D1_POINT_2F cc{ 23, BAR / 2 };
    br->SetColor(g_hoverClose ? (dark ? D2D1::ColorF(1, 1, 1, 0.18f) : D2D1::ColorF(0, 0, 0, 0.12f)) : D2D1::ColorF(0, 0, 0, 0));
    rt->FillEllipse(D2D1::Ellipse(cc, 10, 10), br);
    br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.75f) : D2D1::ColorF(0, 0, 0, 0.65f));
    rt->DrawLine({ cc.x - 4, cc.y - 4 }, { cc.x + 4, cc.y + 4 }, br, 1.4f);
    rt->DrawLine({ cc.x - 4, cc.y + 4 }, { cc.x + 4, cc.y - 4 }, br, 1.4f);
    // "Abrir com ..." à direita
    std::wstring btn = g_app.empty() ? std::wstring(Tr(L"Abrir")) : std::wstring(Tr(L"Abrir com ")) + g_app;
    float bw = ui::TextWidth(g_tfBtn, btn) + 22;
    g_openR = D2D1::RectF(W - 12 - bw, BAR / 2 - 12, W - 12, BAR / 2 + 12);
    br->SetColor(g_hoverOpen ? (dark ? D2D1::ColorF(1, 1, 1, 0.22f) : D2D1::ColorF(0, 0, 0, 0.12f)) : (dark ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(0, 0, 0, 0.06f)));
    rt->FillRoundedRectangle(D2D1::RoundedRect(g_openR, 6, 6), br);
    br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.9f) : D2D1::ColorF(0, 0, 0, 0.85f));
    rt->DrawTextW(btn.c_str(), (UINT32)btn.size(), g_tfBtn, g_openR, br);
    // nome no meio (entre os botões)
    float side = std::max(44.f, bw + 24);
    rt->DrawTextW(g_name.c_str(), (UINT32)g_name.size(), g_tfTitle, D2D1::RectF(side, 0, W - side, BAR), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    // prévia
    D2D1_RECT_F body = D2D1::RectF(PAD, BAR, W - PAD, H - PAD);
    rt->PushAxisAlignedClip(body, D2D1_ANTIALIAS_MODE_ALIASED);
    if (!g_text.empty()) {
        br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.88f) : D2D1::ColorF(0, 0, 0, 0.88f));
        DrawTexto(rt, br, body, 18, 14, dark);
    } else {
        if (g_bmpRt != rt && g_bmp) { g_bmp->Release(); g_bmp = nullptr; }
        if (!g_bmp && !g_px.empty()) { g_bmp = ui::Bitmap(rt, g_iw, g_ih, g_px); g_bmpRt = rt; }
        if (g_handler) {  // Office: a folha branca (o preview handler desenha por cima, em g_phWnd)
            br->SetColor(D2D1::ColorF(1, 1, 1, 1));
            rt->FillRectangle(body, br);
        } else if (g_bmp) {
            g_bodyR = body; g_bodyFill = false;
            rt->DrawBitmap(g_bmp, PhotoRect(body, g_iw, g_ih, false), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.45f) : D2D1::ColorF(0, 0, 0, 0.45f));
            const wchar_t* t = g_loading ? Tr(L"Carregando…") : Tr(L"Sem prévia");
            rt->DrawTextW(t, (UINT32)wcslen(t), g_tfTitle, D2D1::RectF(body.left, (body.top + body.bottom) / 2 - 10, body.right, (body.top + body.bottom) / 2 + 10), br);
        }
    }
    rt->PopAxisAlignedClip();
    br->Release();
    ui::End(g_s, true, POINT{ wr.left, wr.top });
}

void Place() {
    float sc = Sc();
    if (g_full && liquid::Golden()) {  // tela cheia: o monitor inteiro
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowPos(g_wnd, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOACTIVATE);
        return;
    }
    SetWindowPos(g_wnd, WantTop() ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SIZE s = PanelSize(sc);
    int w = (int)std::lround(s.cx * sc), h = (int)std::lround(s.cy * sc);
    RECT wr; GetWindowRect(g_wnd, &wr);
    int cx = (wr.left + wr.right) / 2, cy = (wr.top + wr.bottom) / 2;
    if (!IsWindowVisible(g_wnd)) {  // primeira vez: no meio do monitor do mouse
        MONITORINFO mi{ sizeof(mi) }; POINT c; GetCursorPos(&c);
        GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY), &mi);
        cx = (mi.rcWork.left + mi.rcWork.right) / 2; cy = (mi.rcWork.top + mi.rcWork.bottom) / 2;
    }
    SetWindowPos(g_wnd, nullptr, cx - w / 2, cy - h / 2, w, h, SWP_NOACTIVATE | SWP_NOZORDER);
}

void MediaStart(const std::wstring& path);
void Load(const std::wstring& path) {
    MediaStop();
    HandlerStop();
    ZoomReset();
    dica::Esconde();
    g_path = path;
    g_name = path.substr(path.find_last_of(L"\\/") + 1);
    g_app = DefaultAppName(path);
    g_text.clear(); g_px.clear(); g_iw = g_ih = 0;
    TextoSolta();
    if (g_bmp) { g_bmp->Release(); g_bmp = nullptr; }
    int gen = ++g_gen;
    LoadInfo(path);
    DWORD at = GetFileAttributesW(path.c_str());
    bool dir = at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY);
    // Golden Gate: o que não tem miniatura (pasta, arquivo comum) mostra o ícone grande; sabe antes de abrir
    g_isIcon = false;
    // Vídeo e música não passam por aqui: pedir a miniatura de um vídeo decodifica um quadro e, nesta thread (a da
    // interface do Shell), travava a Dock e a barra por segundos com a CPU alta, e o vídeo demorava a abrir ou nem
    // abria (issue #8). PDF e SVG têm a prévia própria (abaixo)
    bool mediaFile = IsMediaPath(path), ownPreview = !dir && (IsPdf(path) || IsSvg(path));
    CLSID phClsid{};
    if (!g_skipHandler && !dir && !mediaFile && !ownPreview && WantsHandler(path, &phClsid)) {  // Word/PowerPoint/Excel com o handler do Windows
        if (!g_phWnd) {
            HINSTANCE hi = GetModuleHandleW(nullptr);
            WNDCLASSW pc{}; pc.lpfnWndProc = PhProc; pc.hInstance = hi; pc.lpszClassName = L"ShellQuickLookPrevia"; pc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            RegisterClassW(&pc);
            g_phWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, pc.lpszClassName, L"", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 10, 10, g_wnd, nullptr, hi, nullptr);
            if (g_phWnd) apps::ours.push_back(g_phWnd);
        }
        IPreviewHandler* ph = nullptr;
        HRESULT hr = g_phWnd ? CoCreateInstance(phClsid, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&ph)) : E_FAIL;
        if (FAILED(hr) && g_phWnd) hr = CoCreateInstance(phClsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ph));
        if (SUCCEEDED(hr) && ph) {
            IInitializeWithFile* iwf = nullptr; IInitializeWithStream* iws = nullptr;
            if (SUCCEEDED(ph->QueryInterface(IID_PPV_ARGS(&iwf))) && iwf) { hr = iwf->Initialize(path.c_str(), STGM_READ); iwf->Release(); }
            else if (SUCCEEDED(ph->QueryInterface(IID_PPV_ARGS(&iws))) && iws) {
                IStream* st = nullptr;
                hr = SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &st);
                if (SUCCEEDED(hr)) { hr = iws->Initialize(st, STGM_READ); st->Release(); }
                iws->Release();
            } else hr = E_NOINTERFACE;
            if (SUCCEEDED(hr)) { g_ph = ph; g_handler = true; } else ph->Release();
        }
        Log(L"quick look: preview handler %ls (0x%08X)", g_handler ? L"ok" : L"falhou", (unsigned)hr);
        if (g_handler) { g_loading = false; return; }  // Toggle mostra a janela e chama StartHandler
    }
    if (liquid::Golden() && !IsTextFile(path) && !mediaFile && !ownPreview) {
        g_isIcon = true;
        IShellItemImageFactory* f = nullptr;
        HBITMAP hb = nullptr;
        if (!dir && SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f)))) {
            if (FAILED(f->GetImage(SIZE{ 64, 64 }, SIIGBF_RESIZETOFIT | SIIGBF_THUMBNAILONLY, &hb))) hb = nullptr;
            f->Release();
        }
        if (hb) { DeleteObject(hb); g_isIcon = false; }
    }
    if (IsTextFile(path)) { g_text = ReadText(path); g_loading = false; return; }
    bool isVideo = false;
    if (liquid::Golden() && IsMediaPath(path, &isVideo)) {
        g_media = true;
        if (!isVideo) g_isIcon = true;  // música: o ícone (ou a capa) e a barra embaixo
        MediaStart(path);
    }
    g_loading = true;
    HWND w = g_wnd;
    int px = (int)std::lround((g_isIcon ? 400 : 1200) * Sc());
    bool icon = g_isIcon;
    // fotos: o arquivo inteiro até o tamanho da tela (a tela cheia do Quick Look usa tudo), não a miniatura
    std::wstring ext = path.substr(path.find_last_of(L'.') == std::wstring::npos ? path.size() : path.find_last_of(L'.'));
    for (auto& c : ext) c = towlower(c);
    bool photo = !icon && !dir;
    if (photo) {
        photo = false;
        for (auto e : { L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".bmp", L".gif", L".webp", L".heic", L".heif",
                        L".avif", L".tif", L".tiff", L".jxr", L".wdp", L".ico", L".dds" })
            if (ext == e) photo = true;
    }
    UINT screen = (UINT)std::max({ GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), 1600 });
    bool pdf = ownPreview && IsPdf(path), svg = ownPreview && IsSvg(path);
    std::thread([path, gen, w, px, icon, dir, photo, screen, pdf, svg] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto* r = new std::tuple<int, UINT, UINT, std::vector<BYTE>>(gen, 0, 0, std::vector<BYTE>{});
        auto& [g, iw, ih, px2] = *r;
        bool got = photo && ui::LoadPhoto(path, screen, iw, ih, px2);  // sem codec (HEIC sem a extensão): a miniatura
        UINT pages = 0;
        if (!got && pdf) { got = LoadPdf(path, std::min(screen, 2400u), iw, ih, px2, pages); if (!got) Log(L"quick look: PDF não abriu (%ls)", path.c_str()); }
        if (!got && svg) { got = LoadSvg(path, std::min(screen, 2400u), iw, ih, px2); if (!got) Log(L"quick look: SVG não abriu (%ls)", path.c_str()); }
        if (icon && dir && GetFileAttributesW((path + L"\\desktop.ini").c_str()) == INVALID_FILE_ATTRIBUTES) {  // a pasta do Mac (pacote de ícones)
            std::wstring png = ui::PackIcon(L"folder default");
            got = !png.empty() && ui::LoadPng(png, iw, ih, px2);
        }
        bool vid2 = false;
        if (!got && icon && IsMediaPath(path, &vid2) && !vid2) got = ui::ShellImage(path, px, iw, ih, px2, false);  // a capa do álbum
        if (!got && icon) got = ui::FileImage(path, px, iw, ih, px2);
        if (!got && !ui::ShellImage(path, px, iw, ih, px2, false)) ui::ShellImage(path, 256, iw, ih, px2, true);
        if (SUCCEEDED(co)) CoUninitialize();
        if (!PostMessageW(w, WM_QL_READY, 0, (LPARAM)r)) delete r;
    }).detach();
}

// foto na prévia (não ícone, texto, vídeo nem Office): a roda do mouse amplia
bool Zoomable() { return g_bmp && g_text.empty() && !g_media && !g_handler && !g_isIcon && g_iw && g_ih && g_bodyR.right > g_bodyR.left; }

LRESULT CALLBACK VidProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); if (g_mp) g_mp->UpdateVideo(); else FillRect(dc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH)); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: { static LPARAM last; if (l != last) { last = l; bool was = CtlVisible(); g_ctlMoveAt = GetTickCount(); if (!was) PaintCtl(); } return 0; }
    case WM_LBUTTONDOWN: MToggle(); PaintCtl(); return 0;  // clique no vídeo pausa/continua
    case WM_LBUTTONDBLCLK: g_full = !g_full; Place(); Paint(); SyncBack(); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
LRESULT CALLBACK CtlProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    float sc = Sc();
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        g_ctlMoveAt = GetTickCount();
        if (g_scrub) { float x = GET_X_LPARAM(l) / sc; for (auto& ht : g_vhits) if (ht.id == barravideo::SCRUB) MSeek(barravideo::PosicaoEm(ht.r, x) * MLen()); PaintCtl(); return 0; }
        // volume arrastado (issue #8: só dava para clicar)
        if (g_volDrag) { float x = GET_X_LPARAM(l) / sc; for (auto& ht : g_vhits) if (ht.id == barravideo::VOL) { g_vol = barravideo::VolumeEm(ht.r, x); if (g_mp) g_mp->SetVolume(g_vol); } PaintCtl(); return 0; }
        float x = GET_X_LPARAM(l) / sc, y = GET_Y_LPARAM(l) / sc;
        std::wstring tip;
        for (auto& ht : g_vhits) {
            if (x < ht.r.left || x >= ht.r.right || y < ht.r.top || y >= ht.r.bottom) continue;
            switch (ht.id) {
            case barravideo::VOL: tip = Tr(L"Volume"); break;
            case barravideo::BACK: tip = Tr(L"Voltar 15 Segundos"); break;
            case barravideo::PLAY: tip = MPlaying() ? Tr(L"Pausar") : Tr(L"Reproduzir"); break;
            case barravideo::FWD: tip = Tr(L"Avançar 15 Segundos"); break;
            case barravideo::FULL: tip = g_full ? Tr(L"Sair da Tela Cheia") : Tr(L"Tela Cheia"); break;
            }
            break;
        }
        dica::Quer(h, tip, ui::Dark());
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE: dica::Esconde(); return 0;
    case WM_LBUTTONDOWN: {
        dica::Esconde();
        float x = GET_X_LPARAM(l) / sc, y = GET_Y_LPARAM(l) / sc;
        for (auto& ht : g_vhits) {
            if (x < ht.r.left || x >= ht.r.right || y < ht.r.top || y >= ht.r.bottom) continue;
            switch (ht.id) {
            case barravideo::VOL: g_vol = barravideo::VolumeEm(ht.r, x); if (g_mp) g_mp->SetVolume(g_vol); g_volDrag = true; SetCapture(h); break;
            case barravideo::BACK: MSeek(MPos() - 15); break;
            case barravideo::FWD: MSeek(MPos() + 15); break;
            case barravideo::PLAY: MToggle(); break;
            case barravideo::FULL: if (g_hasVideo) { g_full = !g_full; Place(); Paint(); SyncBack(); } break;
            case barravideo::SCRUB: g_scrub = true; SetCapture(h); MSeek(barravideo::PosicaoEm(ht.r, x) * MLen()); break;
            }
            break;
        }
        PaintCtl();
        return 0;
    }
    case WM_LBUTTONUP: if (g_scrub || g_volDrag) { g_scrub = g_volDrag = false; ReleaseCapture(); PaintCtl(); } return 0;
    case WM_CAPTURECHANGED: g_scrub = g_volDrag = false; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void MediaStart(const std::wstring& path) {
    static bool mf = SUCCEEDED(MFStartup(MF_VERSION));
    if (!mf) return;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!g_vid) {
        WNDCLASSW vc{}; vc.style = CS_DBLCLKS; vc.lpfnWndProc = VidProc; vc.hInstance = hi; vc.lpszClassName = L"ShellQuickLookVideo"; vc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&vc);
        g_vid = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, vc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, g_wnd, nullptr, hi, nullptr);
        WNDCLASSW cc{}; cc.lpfnWndProc = CtlProc; cc.hInstance = hi; cc.lpszClassName = L"ShellQuickLookControles"; cc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&cc);
        g_ctl = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, g_wnd, nullptr, hi, nullptr);
        // o vidro fosco da barra é desenhado nela mesma (barravideo::FundoBorrado), recortado nos cantos de 22
        for (HWND x : { g_vid, g_ctl, g_ctlBack }) if (x) apps::ours.push_back(x);
    }
    if (!g_mcb) g_mcb = new MediaCb;
    if (FAILED(MFPCreateMediaPlayer(path.c_str(), FALSE, 0, g_mcb, g_vid, &g_mp))) { g_mp = nullptr; return; }
    g_mp->SetVolume(g_vol);
    g_mp->SetBorderColor(RGB(0, 0, 0));
    g_ctlMoveAt = GetTickCount();
    g_ctlRestack = true;
    SetTimer(g_wnd, 7, 66, nullptr);  // a barra acompanha o vídeo (o desfoque de trás anda junto)
    if (IsMediaPath(path, &g_mpVideo) && g_mpVideo) SetTimer(g_wnd, 11, 8000, nullptr);  // vídeo que não abre: desiste (WM_TIMER 11)
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_QL_MEDIA:
        if (!g_mp) return 0;
        if (w == 1 && !l) {  // pronto: vídeo ganha o tamanho dele; toca sozinho, como no Mac
            SIZE vs{}, ar{};
            if (SUCCEEDED(g_mp->GetNativeVideoSize(&vs, &ar)) && vs.cx && vs.cy) {
                if (ar.cx > 0 && ar.cy > 0) vs.cy = std::max<LONG>(1, (LONG)std::lround((double)vs.cx * ar.cy / ar.cx));
                g_hasVideo = true; g_isIcon = false;
                g_px.clear(); if (g_bmp) { g_bmp->Release(); g_bmp = nullptr; }
                g_iw = vs.cx; g_ih = vs.cy; g_loading = false;
            }
            KillTimer(h, 11);
            g_mp->Play();
            Place(); Paint(); SyncBack();
        } else if (w == 2) { g_ended = true; PaintCtl(); }
        else if (w == 1 || w == 3) {  // não deu para tocar (sem o codec, arquivo estragado): para o player e fica a prévia
            Log(L"quick look: o vídeo não abriu (0x%08X) %ls", (unsigned)l, g_path.c_str());
            bool icon = !g_hasVideo && g_px.empty();
            MediaStop();
            if (icon && liquid::Golden()) g_isIcon = true;
            Place(); Paint(); SyncBack();
        }
        return 0;
    case WM_TIMER:
        if (w == 7) { if (g_media) PaintCtl(); return 0; }
        if (w == 11) {  // o Media Foundation não respondeu em 8 s: desiste (antes ficava girando, com a CPU alta)
            KillTimer(h, 11);
            if (g_mp && !g_hasVideo) { Log(L"quick look: o vídeo não abriu em 8 s %ls", g_path.c_str()); MediaStop(); Place(); Paint(); SyncBack(); }
            return 0;
        }
        if (w == 8) { KeepTop(); return 0; }
        break;
    case WM_QL_FRAME:
        if (g_qlAnimT0 >= 0) AnimStep(); else ui::FrameClock(h, WM_QL_FRAME, false);
        return 0;
    case WM_QL_READY: {
        auto* r = (std::tuple<int, UINT, UINT, std::vector<BYTE>>*)l;
        if (std::get<0>(*r) == g_gen && !g_hasVideo) {
            g_iw = std::get<1>(*r); g_ih = std::get<2>(*r); g_px = std::move(std::get<3>(*r)); g_loading = false;
            if (g_bmp) { g_bmp->Release(); g_bmp = nullptr; }
            Place(); Paint();
        }
        delete r;
        return 0;
    }
    case WM_NCHITTEST: {
        POINT p{ (short)LOWORD(l), (short)HIWORD(l) }; ScreenToClient(h, &p);
        float sc = Sc(), x = p.x / sc, y = p.y / sc;
        auto in = [&](D2D1_RECT_F r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
        if (liquid::Golden()) return HTCLIENT;  // arrasta à mão (WM_LBUTTONDOWN): o HTCAPTION não movia este painel  // como no Mac: arrasta pelo painel todo
        if (y < BAR && !(x >= g_closeR.left && x < g_closeR.right) && !(x >= g_openR.left && x < g_openR.right)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_MOUSEWHEEL: {  // zoom da foto em volta do ponteiro (issue #8)
        if (!g_text.empty()) { TextoRola(-GET_WHEEL_DELTA_WPARAM(w) * 0.6f); dica::Esconde(); Paint(); return 0; }  // texto: rola
        if (!Zoomable()) return 0;
        POINT p{ (short)LOWORD(l), (short)HIWORD(l) }; ScreenToClient(h, &p);
        float sc = Sc(), x = p.x / sc, y = p.y / sc;
        float z0 = g_zoom, z1 = std::clamp(z0 * std::pow(1.0018f, (float)GET_WHEEL_DELTA_WPARAM(w)), 1.f, 8.f);
        if (z1 == z0) return 0;
        D2D1_RECT_F a = PhotoRect(g_bodyR, g_iw, g_ih, g_bodyFill);
        float fx = (x - a.left) / std::max(1.f, a.right - a.left), fy = (y - a.top) / std::max(1.f, a.bottom - a.top);
        g_zoom = z1;
        D2D1_RECT_F b = PhotoRect(g_bodyR, g_iw, g_ih, g_bodyFill);  // tamanho novo; o ponto sob o mouse fica parado
        g_panX += (x - fx * (b.right - b.left)) - b.left;
        g_panY += (y - fy * (b.bottom - b.top)) - b.top;
        if (g_zoom <= 1.001f) ZoomReset();
        dica::Esconde();
        Paint();
        return 0;
    }
    case WM_LBUTTONDOWN:
        dica::Esconde();
        {   // foto ampliada: arrastar move a foto
            float sc = Sc(), x = (short)LOWORD(l) / sc, y = (short)HIWORD(l) / sc;
            if (Zoomable() && g_zoom > 1.001f && x >= g_bodyR.left && x < g_bodyR.right && y >= g_bodyR.top && y < g_bodyR.bottom) {
                GetCursorPos(&g_panFrom); g_panX0 = g_panX; g_panY0 = g_panY;
                g_panning = true; SetCapture(h);
                return 0;
            }
        }
        // como no Mac: arrasta por qualquer ponto do painel que não seja botão
        if (liquid::Golden() && !g_full && !g_hoverClose && !g_hoverOpen && !g_hoverShare && !g_hoverFull && !g_hoverRot && !g_hoverMark) {
            GetCursorPos(&g_dragFrom);
            RECT r; GetWindowRect(h, &r); g_dragOrg = { r.left, r.top };
            g_dragging = true; SetCapture(h);
        }
        return 0;
    case WM_CAPTURECHANGED: g_dragging = g_panning = false; return 0;
    case WM_MOUSEMOVE: {
        if (g_panning) {
            POINT p; GetCursorPos(&p);
            float sc = Sc();
            g_panX = g_panX0 + (p.x - g_panFrom.x) / sc; g_panY = g_panY0 + (p.y - g_panFrom.y) / sc;
            Paint();
            return 0;
        }
        if (g_dragging) {
            POINT p; GetCursorPos(&p);
            SetWindowPos(h, nullptr, g_dragOrg.x + p.x - g_dragFrom.x, g_dragOrg.y + p.y - g_dragFrom.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        float sc = Sc(), x = (short)LOWORD(l) / sc, y = (short)HIWORD(l) / sc;
        bool hc = x >= g_closeR.left && x < g_closeR.right && y >= g_closeR.top && y < g_closeR.bottom;
        bool ho = x >= g_openR.left && x < g_openR.right && y >= g_openR.top && y < g_openR.bottom;
        auto in = [&](D2D1_RECT_F r) { return liquid::Golden() && x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
        bool hs = in(g_shareR), hf = in(g_fullR), hr = in(g_rotR), hm = in(g_markR);
        if (hc != g_hoverClose || ho != g_hoverOpen || hs != g_hoverShare || hf != g_hoverFull || hr != g_hoverRot || hm != g_hoverMark) {
            g_hoverClose = hc; g_hoverOpen = ho; g_hoverShare = hs; g_hoverFull = hf; g_hoverRot = hr; g_hoverMark = hm; Paint();
        }
        // dica do botão parado sob o mouse, como no Mac
        std::wstring tip = hc ? Tr(L"Fechar") : hf ? (g_full ? Tr(L"Sair da Tela Cheia") : Tr(L"Tela Cheia")) : hs ? Tr(L"Compartilhar")
                         : hr ? Tr(L"Girar para a Esquerda") : hm ? Tr(L"Marcação") : ho ? (g_app.empty() ? std::wstring(Tr(L"Abrir")) : std::wstring(Tr(L"Abrir com ")) + g_app) : L"";
        dica::Quer(h, tip, ui::Dark());
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE:
        dica::Esconde();
        if (g_hoverClose || g_hoverOpen || g_hoverShare || g_hoverFull || g_hoverRot || g_hoverMark) {
            g_hoverClose = g_hoverOpen = g_hoverShare = g_hoverFull = g_hoverRot = g_hoverMark = false; Paint();
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_panning) { g_panning = false; ReleaseCapture(); return 0; }
        if (g_dragging) { g_dragging = false; ReleaseCapture(); return 0; }
        if (g_hoverClose) { quicklook::Close(); return 0; }
        if (g_hoverFull) { g_full = !g_full; ZoomReset(); Place(); Paint(); SyncBack(); return 0; }
        if (g_hoverRot && g_iw && g_ih && g_px.size() >= (size_t)g_iw * g_ih * 4) {  // gira 90° para a esquerda, como o Mac
            std::vector<BYTE> o(g_px.size());
            for (UINT y = 0; y < g_ih; y++)
                for (UINT x = 0; x < g_iw; x++)
                    memcpy(&o[((size_t)(g_iw - 1 - x) * g_ih + y) * 4], &g_px[((size_t)y * g_iw + x) * 4], 4);
            g_px.swap(o); std::swap(g_iw, g_ih);
            if (g_bmp) { g_bmp->Release(); g_bmp = nullptr; }
            ZoomReset();
            Place(); Paint(); SyncBack();
            return 0;
        }
        if (g_hoverMark) { std::wstring p = g_path; quicklook::Close(); OpenPath(p); return 0; }  // marcação: abre no app
        if (g_hoverShare) {  // a janela de Compartilhar do Windows
            SHELLEXECUTEINFOW se{ sizeof(se) };
            se.fMask = SEE_MASK_INVOKEIDLIST; se.lpVerb = L"windows.modernshare"; se.lpFile = g_path.c_str(); se.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&se);
            return 0;
        }
        if (g_hoverOpen) {
            std::wstring p = g_path; quicklook::Close();
            OpenPath(p);
            return 0;
        }
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE && g_full) { g_full = false; ZoomReset(); Place(); Paint(); SyncBack(); return 0; }
        if (w == VK_ESCAPE || w == VK_SPACE) { quicklook::Close(); return 0; }
        if (w == VK_RETURN) { std::wstring p = g_path; quicklook::Close(); OpenPath(p); return 0; }
        if (!g_text.empty()) {  // texto: setas, Page Up/Down, Home/End rolam
            RECT cr; GetClientRect(h, &cr);
            float pg = std::max(60.f, cr.bottom / Sc() - BAR - 40);
            float dy = w == VK_DOWN ? 40.f : w == VK_UP ? -40.f : w == VK_NEXT ? pg : w == VK_PRIOR ? -pg : w == VK_END ? 1e7f : w == VK_HOME ? -1e7f : 0.f;
            if (dy != 0) { TextoRola(dy); Paint(); }
        }
        return 0;
    case WM_MOVE: Paint(); return 0;
    case WM_WINDOWPOSCHANGED: SyncBack(); break;
    case WM_DPICHANGED: Place(); Paint(); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK BackProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

bool Ensure() {
    if (g_wnd) return true;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"ShellQuickLook"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, wc.lpszClassName, L"Quick Look", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    if (!g_wnd) return false;
    g_s.hwnd = g_wnd;
    apps::ours.push_back(g_wnd);
    {
        WNDCLASSW bc{}; bc.lpfnWndProc = BackProc; bc.hInstance = hi; bc.lpszClassName = L"ShellQuickLookFundo";
        RegisterClassW(&bc);
        g_back = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP, bc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
        g_glass = g_back && backdrop::Attach(g_back);
        if (g_back) {
            DWORD c1 = 1; COLORREF none = 0xFFFFFFFE;
            DwmSetWindowAttribute(g_back, 33, &c1, sizeof(c1)); DwmSetWindowAttribute(g_back, 34, &none, sizeof(none));
            apps::ours.push_back(g_back);
        }
    }
    g_tfTitle = ui::Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfBtn = ui::Format(12.5f, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfText = ui::Format(13, DWRITE_FONT_WEIGHT_NORMAL);
    for (auto* f : { g_tfTitle, g_tfBtn }) if (f) f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (g_tfTitle) { DWRITE_TRIMMING t{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 }; g_tfTitle->SetTrimming(&t, nullptr); g_tfTitle->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
    return true;
}

}  // namespace


bool quicklook::IsOpen() { return g_wnd && IsWindowVisible(g_wnd); }
std::wstring quicklook::Path() { return IsOpen() ? g_path : L""; }

namespace {
void CloseNow() {
    if (!g_wnd) return;
    ui::FrameClock(g_wnd, WM_QL_FRAME, false); g_qlAnimT0 = -1; g_qlAnimOut = false; g_qlOp = g_qlSc = 1;
    MediaStop();
    HandlerStop();
    ZoomReset();
    dica::Esconde();
    KillTimer(g_wnd, 8);
    SetWindowPos(g_wnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(g_wnd, SW_HIDE);
    if (g_back) ShowWindow(g_back, SW_HIDE);
    g_full = false;
    g_gen++;
    if (g_bmp) { g_bmp->Release(); g_bmp = nullptr; }
    g_px.clear(); g_text.clear();
}
}  // namespace

void quicklook::Close() {
    if (!g_wnd) return;
    if (g_qlAnimOut) return;  // já fechando
    if (IsWindowVisible(g_wnd) && g_glass && liquid::Golden()) {  // some animado; o vídeo para já
        MediaStop();
        HandlerStop();
        dica::Esconde();
        AnimStart(true);
        return;
    }
    CloseNow();
}

void quicklook::Toggle(const std::wstring& path, bool activate) {
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (g_qlAnimOut) CloseNow();  // fechando ainda: termina já e abre o novo
    if (IsOpen() && !_wcsicmp(path.c_str(), g_path.c_str())) { Close(); return; }
    if (!Ensure()) return;
    bool opening = !IsOpen();
    if (opening && g_glass && liquid::Golden()) { g_qlOp = 0; g_qlSc = 0.94f; }  // a primeira pintura já sai do começo da animação
    Load(path);
    Place();  // fechado: no meio do monitor do mouse; aberto: fica onde está, no tamanho da prévia nova
    // do Finder: não tira o foco dele (as setas trocam o arquivo e o espaço fecha, como no Mac)
    ShowWindow(g_wnd, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
    // por cima do Finder, que está na frente (o Windows não deixa outro programa passar à frente dele sem isso)
    SetWindowPos(g_wnd, WantTop() ? HWND_TOPMOST : HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetTimer(g_wnd, 8, 300, nullptr);
    if (activate) SetForegroundWindow(g_wnd);
    Paint();
    SyncBack();
    if (g_handler && g_ph) {  // Word/PowerPoint: o handler desenha na folha por cima do corpo (PlaceHandler)
        RECT rc{}; GetClientRect(g_phWnd, &rc);
        HRESULT hr = g_ph->SetWindow(g_phWnd, &rc);
        if (SUCCEEDED(hr)) hr = g_ph->DoPreview();
        if (FAILED(hr)) {  // não desenhou: fica o ícone grande, como antes
            Log(L"quick look: DoPreview 0x%08X", (unsigned)hr);
            std::wstring p = g_path;
            g_skipHandler = true; Load(p); g_skipHandler = false;
            Place(); Paint(); SyncBack();
        }
    }
    if (opening && g_glass && liquid::Golden()) AnimStart(false);
    Log(L"quick look: %ls", path.c_str());
}
