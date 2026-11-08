#include "audio.hpp"
#include <windowsx.h>
#include <commctrl.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <filesystem>
#include <string>
#include <thread>

using namespace dictophone;
namespace fs = std::filesystem;
namespace {
constexpr COLORREF Bg = RGB(20, 23, 30), Panel = RGB(32, 37, 47), Text = RGB(238, 241, 246),
    Muted = RGB(149, 160, 179), Accent = RGB(105, 226, 198), Red = RGB(255, 104, 121);
constexpr int Pin = 101, Menu = 102, Minimize = 103, Close = 104, Main = 105, Pause = 106, Save = 107;
constexpr UINT ExportDone = WM_APP + 2, SpaceDown = WM_APP + 3, SpaceUp = WM_APP + 4;
constexpr wchar_t ClassName[] = L"Frish07.Dictophone";
const std::array<std::wstring, 5> Formats{L"wav", L"mp3", L"ogg", L"flac", L"m4a"};
HWND window = nullptr;
HHOOK keyboardHook = nullptr;
bool walkie = false, globalSpace = false, pinned = false, compact = false, session = false,
    exporting = false, held = false, modal = false, closeAfterSave = false, finalized = false;
int formatIndex = 0;
float scale = 1.f;
Recorder recorder;
std::thread exportThread;
fs::path dataDir, outputDir, draft, savedPath;
std::wstring notice = L"Готов к записи", exportError;
std::deque<float> levels(49, .02f);
HFONT normalFont = nullptr, smallFont = nullptr, timerFont = nullptr, titleFont = nullptr;
HFONT compactTimerFont = nullptr;
HWND tooltips = nullptr;
int timerSeconds = -1;

void layoutControls();
void resizeWindow(int x, int y);

int px(int value) { return int(value * scale + .5f); }
fs::path knownFolder(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &value))) return fs::current_path();
    fs::path result(value); CoTaskMemFree(value); return result;
}
std::wstring stamp() {
    SYSTEMTIME t{}; GetLocalTime(&t); wchar_t value[80]{};
    swprintf_s(value, L"%04u-%02u-%02u_%02u-%02u-%02u-%03u", t.wYear, t.wMonth, t.wDay,
        t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return value;
}
void errorBox(const std::wstring& error) {
    modal = true; MessageBoxW(window, error.c_str(), L"Диктофон", MB_OK | MB_ICONERROR); modal = false;
}
void saveSettings() {
    std::error_code ec; fs::create_directories(dataDir, ec);
    auto path = dataDir / L"settings.ini";
    WritePrivateProfileStringW(L"settings", L"folder", outputDir.c_str(), path.c_str());
    WritePrivateProfileStringW(L"settings", L"format", Formats[formatIndex].c_str(), path.c_str());
    WritePrivateProfileStringW(L"settings", L"walkie", walkie ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"settings", L"globalSpace", globalSpace ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"settings", L"pinned", pinned ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"settings", L"compact", compact ? L"1" : L"0", path.c_str());
}
void loadSettings() {
    const auto path = dataDir / L"settings.ini";
    wchar_t folder[32768]{}; auto fallback = knownFolder(FOLDERID_Documents) / L"Dictophone";
    GetPrivateProfileStringW(L"settings", L"folder", fallback.c_str(), folder, 32768, path.c_str());
    outputDir = fs::path(folder).is_absolute() ? fs::path(folder) : fallback;
    wchar_t format[32]{}; GetPrivateProfileStringW(L"settings", L"format", L"wav", format, 32, path.c_str());
    for (size_t i = 0; i < Formats.size(); ++i) if (Formats[i] == format) formatIndex = int(i);
    walkie = GetPrivateProfileIntW(L"settings", L"walkie", 0, path.c_str()) != 0;
    globalSpace = GetPrivateProfileIntW(L"settings", L"globalSpace", 0, path.c_str()) != 0;
    pinned = GetPrivateProfileIntW(L"settings", L"pinned", 0, path.c_str()) != 0;
    compact = GetPrivateProfileIntW(L"settings", L"compact", 0, path.c_str()) != 0;
}
void update() {
    if (!window) return;
    timerSeconds = -1;
    auto label = [](int id, const wchar_t* value) {
        wchar_t previous[80]{}; HWND button = GetDlgItem(window, id);
        GetWindowTextW(button, previous, 80);
        if (std::wstring(previous) != value) SetWindowTextW(button, value);
    };
    auto enabled = [](int id, bool value) {
        HWND button = GetDlgItem(window, id);
        if (bool(IsWindowEnabled(button)) != value) EnableWindow(button, value);
    };
    label(Main, walkie ? L"Удерживать" : session ? L"Стоп" : L"Запись");
    label(Pause, recorder.isRecording() ? L"Пауза" : L"Продолжить");
    enabled(Main, !exporting && !(walkie && finalized));
    enabled(Pause, !walkie && session && !exporting && !finalized);
    enabled(Save, session && !exporting);
    HWND focused = GetFocus();
    if (GetActiveWindow() == window && (!focused || !IsWindowEnabled(focused) || !IsWindowVisible(focused)))
        SetFocus(IsWindowEnabled(GetDlgItem(window, Main)) ? GetDlgItem(window, Main) : window);
    for (int id : {Main, Pause, Save, Pin}) InvalidateRect(GetDlgItem(window, id), nullptr, FALSE);
    InvalidateRect(window, nullptr, FALSE);
}
bool begin() {
    if (exporting) return false;
    if (session) {
        if (finalized) return false;
        recorder.setRecording(true); notice.clear(); update(); return true;
    }
    if (formatIndex && findFfmpeg().empty()) {
        errorBox(L"Для MP3, OGG, FLAC и M4A нужен FFmpeg. Запустите scripts/setup-ffmpeg.ps1 или выберите WAV в меню ⋯.");
        return false;
    }
    try {
        fs::create_directories(dataDir / L"Recovery");
        draft = dataDir / L"Recovery" / (L"draft-" + stamp() + L".wav");
        std::wstring error;
        if (!recorder.start(draft, window, error)) {
            std::error_code ec; fs::remove(draft, ec); errorBox(error); return false;
        }
        session = true; finalized = false; notice.clear(); levels.assign(49, .02f); update(); return true;
    } catch (const std::exception&) { errorBox(L"Не удалось создать папку временных записей."); return false; }
}
void releaseSpace() {
    if (!held) return;
    held = false;
    if (walkie && session && !exporting) { recorder.setRecording(false); notice.clear(); update(); }
}
void toggleCompact() {
    releaseSpace(); compact = !compact;
    // Move keyboard focus off header buttons before hiding them.
    if (compact) SetFocus(GetDlgItem(window, Main));
    layoutControls();
    RECT current{}; GetWindowRect(window, &current);
    resizeWindow(current.left, current.top);
    saveSettings(); update();
}
void pressSpace() {
    if (!walkie || held || exporting || modal) return;
    held = true;
    if (!begin()) held = false;
}
void saveRecording() {
    if (!session || exporting) return;
    releaseSpace(); recorder.finish(); finalized = true;
    if (recorder.seconds() < .01) {
        std::error_code ec; fs::remove(draft, ec); session = false; finalized = false;
        notice = L"Запись слишком короткая";
        if (closeAfterSave) { DestroyWindow(window); return; }
        update(); return;
    }
    exporting = true; notice = L"Сохраняю…"; update();
    savedPath = outputDir / (L"Запись-" + stamp() + L"." + Formats[formatIndex]);
    const auto input = draft, output = savedPath, encoder = findFfmpeg();
    const auto extension = Formats[formatIndex];
    if (exportThread.joinable()) exportThread.join();
    exportThread = std::thread([input, output, encoder, extension] {
        std::wstring failure;
        const bool ok = exportAudio(input, output, extension, encoder, failure);
        exportError = failure;
        PostMessageW(window, ExportDone, ok, 0);
    });
}
LRESULT CALLBACK keyboardProc(int code, WPARAM action, LPARAM value) {
    if (code == HC_ACTION && walkie && globalSpace && !modal && !exporting) {
        auto* key = reinterpret_cast<KBDLLHOOKSTRUCT*>(value);
        if (key->vkCode == VK_SPACE) {
            if (action == WM_KEYDOWN) PostMessageW(window, SpaceDown, 0, 0);
            if (action == WM_KEYUP) PostMessageW(window, SpaceUp, 0, 0);
            // The hotkey intentionally consumes Space while global radio mode is enabled.
            return 1;
        }
    }
    return CallNextHookEx(keyboardHook, code, action, value);
}
void configureHook() {
    if (keyboardHook) { UnhookWindowsHookEx(keyboardHook); keyboardHook = nullptr; }
    if (walkie && globalSpace) {
        keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardProc, GetModuleHandleW(nullptr), 0);
        if (!keyboardHook) { globalSpace = false; errorBox(L"Не удалось включить глобальный пробел. Пробел работает в окне приложения."); }
    }
}
void chooseFolder() {
    releaseSpace(); modal = true;
    IFileDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0; dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(L"Куда сохранять записи?");
        IShellItem* initial = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(outputDir.c_str(), nullptr, IID_PPV_ARGS(&initial)))) {
            dialog->SetFolder(initial); initial->Release();
        }
        if (SUCCEEDED(dialog->Show(window))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR name = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
                    outputDir = name; CoTaskMemFree(name); saveSettings(); notice = L"Папка сохранения изменена";
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    modal = false; update();
}
void openFolder(const fs::path& path) {
    std::error_code ec; fs::create_directories(path, ec);
    if (ec || reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        errorBox(L"Не удалось открыть папку:\n" + path.wstring());
}
void menu(const POINT* anchor = nullptr) {
    releaseSpace(); modal = true;
    HMENU popup = CreatePopupMenu(), formats = CreatePopupMenu();
    const UINT locked = (session || exporting) ? MF_GRAYED : 0;
    AppendMenuW(popup, MF_STRING | locked | (!walkie ? MF_CHECKED : 0), 201, L"Диктофон");
    AppendMenuW(popup, MF_STRING | locked | (walkie ? MF_CHECKED : 0), 202, L"Рация · удерживать пробел");
    AppendMenuW(popup, MF_STRING | (globalSpace ? MF_CHECKED : 0) | (walkie && !exporting ? 0 : MF_GRAYED),
        203, L"Пробел в других приложениях");
    AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(popup, MF_STRING | (compact ? MF_CHECKED : 0), 208, L"Уменьшенное окно\tCtrl+M");
    AppendMenuW(popup, MF_STRING | (pinned ? MF_CHECKED : 0), Pin, L"Поверх других окон");
    AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < Formats.size(); ++i) {
        auto label = Formats[i]; std::transform(label.begin(), label.end(), label.begin(), ::towupper);
        AppendMenuW(formats, MF_STRING | (int(i) == formatIndex ? MF_CHECKED : 0), 220 + i, label.c_str());
    }
    AppendMenuW(popup, MF_POPUP | ((exporting || (session && !finalized)) ? MF_GRAYED : 0), reinterpret_cast<UINT_PTR>(formats), L"Формат аудио");
    AppendMenuW(popup, MF_STRING | (exporting ? MF_GRAYED : 0), 204, L"Выбрать папку сохранения…");
    AppendMenuW(popup, MF_STRING, 205, L"Открыть папку записей");
    AppendMenuW(popup, MF_STRING, 206, L"Открыть временные WAV");
    AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(popup, MF_STRING, 207, L"О приложении");
    POINT position{};
    if (anchor) position = *anchor;
    else if (compact) GetCursorPos(&position);
    else { RECT button{}; GetWindowRect(GetDlgItem(window, Menu), &button); position = {button.right, button.bottom}; }
    const int selected = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_RIGHTALIGN, position.x, position.y, 0, window, nullptr);
    DestroyMenu(popup); modal = false;
    // A menu opened from the draggable timer may leave no keyboard focus.
    SetFocus(IsWindowEnabled(GetDlgItem(window, Main)) ? GetDlgItem(window, Main) : window);
    if (selected == 201 || selected == 202) { walkie = selected == 202; notice = L"Готов к записи"; configureHook(); }
    if (selected == 203) { globalSpace = !globalSpace; configureHook(); }
    if (selected >= 220 && selected < 225) formatIndex = selected - 220;
    if (selected == 204) chooseFolder();
    if (selected == 205) openFolder(outputDir);
    if (selected == 206) openFolder(dataDir / L"Recovery");
    if (selected == 208) toggleCompact();
    if (selected == Pin) SendMessageW(window, WM_COMMAND, Pin, 0);
    if (selected == 207) {
        modal = true;
        MessageBoxW(window, L"Dictophone 1.1.0\n\nДиктофон: запись → пауза → стоп.\nРация: держите пробел, чтобы записывать.\nКнопка «Сохранить» завершает запись.\n\nCtrl+S — сохранить. Ctrl+O — папка записей.\nCtrl+M — уменьшенное окно.\nВ уменьшенном окне: ПКМ — меню, таймер — перетащить.\nДвойной щелчок по таймеру — обычное окно.\nНастройки и черновики: %LOCALAPPDATA%\\Dictophone\n\nГлобальный пробел включается отдельно в меню.\nВ этом режиме он перехватывается в других приложениях.",
            L"О приложении", MB_OK | MB_ICONINFORMATION); modal = false;
    }
    saveSettings(); update();
}
void text(HDC dc, const std::wstring& value, RECT r, HFONT font, COLORREF color, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color); auto old = SelectObject(dc, font);
    DrawTextW(dc, value.c_str(), -1, &r, flags); SelectObject(dc, old);
}
RECT rect(int x, int y, int w, int h) { return {px(x), px(y), px(x+w), px(y+h)}; }
void rounded(HDC dc, RECT r, COLORREF color, int radius = 12) {
    auto brush = CreateSolidBrush(color); auto oldB = SelectObject(dc, brush); auto oldP = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, px(radius), px(radius));
    SelectObject(dc, oldB); SelectObject(dc, oldP); DeleteObject(brush);
}
void paint(HDC target) {
    RECT client{}; GetClientRect(window, &client);
    HDC dc = CreateCompatibleDC(target); HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    auto old = SelectObject(dc, bitmap); auto bg = CreateSolidBrush(Bg); FillRect(dc, &client, bg); DeleteObject(bg);
    int seconds = int(recorder.seconds()); wchar_t time[40]{};
    if (seconds >= 3600) swprintf_s(time, L"%02d:%02d:%02d", seconds/3600, seconds/60%60, seconds%60);
    else swprintf_s(time, L"%02d:%02d", seconds/60, seconds%60);
    if (compact) {
        text(dc, time, rect(184, 10, 112, 44), compactTimerFont, recorder.isRecording() ? Accent : Text,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
    text(dc, L"DICTOPHONE", rect(20, 12, 170, 28), titleFont, Text);
    text(dc, walkie ? L"РАЦИЯ" : L"ДИКТОФОН", rect(22, 49, 165, 20), smallFont, Accent);
    std::wstring format = Formats[formatIndex]; std::transform(format.begin(), format.end(), format.begin(), ::towupper);
    text(dc, format + L"  /  MONO", rect(218, 49, 120, 20), smallFont, Muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    text(dc, time, rect(20, 73, 320, 56), timerFont, Text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    int i = 0;
    for (float level : levels) {
        int height = std::max(3, int(std::sqrt(level) * 27));
        rounded(dc, rect(33 + i*6, 147-height/2, 3, height), recorder.isRecording() ? Accent : RGB(66, 77, 94), 3); ++i;
    }
    auto status = notice.empty() ? (recorder.isRecording() ? L"Идёт запись" : L"На паузе") : notice;
    text(dc, status, rect(20, 220, 320, 21), smallFont, recorder.isRecording() ? Red : Muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    text(dc, walkie ? (globalSpace ? L"Пробел работает во всех приложениях" : L"Удерживайте пробел в этом окне") : L"Нажмите ▶, чтобы начать запись",
        rect(16, 244, 328, 18), smallFont, Muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    text(dc, outputDir.wstring(), rect(20, 269, 320, 18), smallFont, RGB(105, 118, 139), DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    }
    BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc);
}
void drawCompactIcon(HDC dc, int id, RECT r, COLORREF color, COLORREF background) {
    const int cx = (r.left+r.right)/2, cy = (r.top+r.bottom)/2, unit = px(9);
    auto brush = CreateSolidBrush(color); auto oldBrush = SelectObject(dc, brush);
    auto oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    if (id == Main && walkie) Ellipse(dc, cx-unit, cy-unit, cx+unit, cy+unit);
    else if (id == Main && session) Rectangle(dc, cx-unit, cy-unit, cx+unit, cy+unit);
    else if (id == Pause && recorder.isRecording()) {
        Rectangle(dc, cx-unit, cy-unit, cx-px(3), cy+unit);
        Rectangle(dc, cx+px(3), cy-unit, cx+unit, cy+unit);
    } else if (id == Save) {
        RoundRect(dc, cx-unit, cy-unit, cx+unit, cy+unit, px(3), px(3));
        auto cutout = CreateSolidBrush(background);
        RECT slot{cx-px(5), cy-unit, cx+px(4), cy-px(2)}; FillRect(dc, &slot, cutout);
        RECT label{cx-px(5), cy+px(3), cx+px(5), cy+unit}; FillRect(dc, &label, cutout);
        DeleteObject(cutout);
    } else {
        POINT triangle[]{{cx-unit+px(2),cy-unit},{cx+unit,cy},{cx-unit+px(2),cy+unit}};
        Polygon(dc, triangle, 3);
    }
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(brush);
}
void drawButton(const DRAWITEMSTRUCT* item) {
    RECT r = item->rcItem;
    // Compose the complete button offscreen, including its rounded corners.
    HDC dc = CreateCompatibleDC(item->hDC);
    auto bitmap = CreateCompatibleBitmap(item->hDC, r.right-r.left, r.bottom-r.top);
    auto oldBitmap = SelectObject(dc, bitmap);
    auto background = CreateSolidBrush(Bg); FillRect(dc, &r, background); DeleteObject(background);
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const bool down = (item->itemState & ODS_SELECTED) != 0;
    int id = int(item->CtlID);
    COLORREF fill = id == Main ? (session && !walkie ? Red : Accent) : Panel;
    if (id == Pin && pinned) fill = RGB(44, 79, 77);
    if (id <= Close) fill = id == Pin && pinned ? RGB(44, 79, 77) : Bg;
    if (down) fill = RGB(GetRValue(fill)*3/4, GetGValue(fill)*3/4, GetBValue(fill)*3/4);
    rounded(dc, r, disabled ? RGB(27, 31, 40) : fill);
    COLORREF foreground = disabled ? RGB(79, 88, 103) : id == Main ? Bg : Text;
    std::wstring label;
    if (id == Pin) label = pinned ? L"◆" : L"◇";
    if (id == Menu) label = L"⋯";
    if (id == Minimize) label = L"−";
    if (id == Close) label = L"×";
    if (id == Main) label = walkie ? L"● Держать" : session ? L"■ Стоп" : L"▶ Запись";
    if (id == Pause) label = recorder.isRecording() ? L"Ⅱ" : L"▶";
    if (id == Save) label = L"Сохранить";
    if (compact && (id == Main || id == Pause || id == Save))
        drawCompactIcon(dc, id, r, foreground, disabled ? RGB(27, 31, 40) : fill);
    else text(dc, label, r, normalFont, foreground, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if ((item->itemState & ODS_FOCUS) && !disabled) { InflateRect(&r, -px(4), -px(4)); DrawFocusRect(dc, &r); }
    BitBlt(item->hDC, 0, 0, item->rcItem.right, item->rcItem.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBitmap); DeleteObject(bitmap); DeleteDC(dc);
}
void addButton(int id, const wchar_t* label, int x, int y, int w, int h, const wchar_t* tooltip, HWND tips) {
    HWND button = CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        px(x), px(y), px(w), px(h), window, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(normalFont), TRUE);
    TOOLINFOW info{}; info.cbSize = sizeof(info); info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    info.hwnd = window; info.uId = reinterpret_cast<UINT_PTR>(button); info.lpszText = const_cast<wchar_t*>(tooltip);
    SendMessageW(tips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
}
LRESULT CALLBACK buttonProc(HWND button, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (message == WM_ERASEBKGND) return 1;
    const bool main = GetDlgCtrlID(button) == Main;
    if (main && walkie && message == WM_LBUTTONDOWN && !exporting) {
        SetFocus(button); SetCapture(button); pressSpace(); return 0;
    }
    if (main && walkie && message == WM_LBUTTONUP) { releaseSpace(); ReleaseCapture(); return 0; }
    if (main && walkie && (message == WM_CAPTURECHANGED || message == WM_CANCELMODE)) releaseSpace();
    return DefSubclassProc(button, message, wp, lp);
}
void layoutControls() {
    for (int id : {Pin, Menu, Minimize, Close}) ShowWindow(GetDlgItem(window, id), compact ? SW_HIDE : SW_SHOWNA);
    auto move = [](int id, int x, int y, int w, int h) {
        SetWindowPos(GetDlgItem(window, id), nullptr, px(x), px(y), px(w), px(h), SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    };
    if (compact) {
        move(Main, 10, 10, 52, 44); move(Pause, 70, 10, 44, 44); move(Save, 122, 10, 52, 44);
    } else {
        move(Main, 22, 176, 120, 40); move(Pause, 150, 176, 52, 40); move(Save, 210, 176, 128, 40);
    }
    TOOLINFOW timerTip{}; timerTip.cbSize = sizeof(timerTip); timerTip.hwnd = window; timerTip.uId = 800;
    timerTip.rect = compact ? RECT{px(184),px(10),px(296),px(54)} : RECT{px(20),px(73),px(340),px(129)};
    SendMessageW(tooltips, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&timerTip));
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}
void resizeWindow(int x, int y) {
    const int width = px(compact ? 306 : 360), height = px(compact ? 64 : 300);
    MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) {
        x = std::clamp(x, int(monitor.rcWork.left), std::max(int(monitor.rcWork.left), int(monitor.rcWork.right)-width));
        y = std::clamp(y, int(monitor.rcWork.top), std::max(int(monitor.rcWork.top), int(monitor.rcWork.bottom)-height));
    }
    SetWindowPos(window, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}
void createControls() {
    auto font = [](int size, int weight, const wchar_t* face) { return CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face); };
    normalFont = font(13, FW_SEMIBOLD, L"Segoe UI"); smallFont = font(11, FW_NORMAL, L"Segoe UI");
    timerFont = font(45, FW_NORMAL, L"Segoe UI"); titleFont = font(13, FW_BOLD, L"Segoe UI");
    compactTimerFont = font(22, FW_SEMIBOLD, L"Consolas");
    HWND tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window, nullptr, GetModuleHandleW(nullptr), nullptr);
    tooltips = tips;
    SendMessageW(tips, TTM_SETMAXTIPWIDTH, 0, px(280));
    addButton(Pin, L"Закрепить поверх окон", 220, 10, 28, 30, L"Закрепить поверх остальных окон", tips);
    addButton(Menu, L"Меню", 250, 10, 28, 30, L"Режим, формат и папка сохранения", tips);
    addButton(Minimize, L"Свернуть", 280, 10, 28, 30, L"Свернуть", tips);
    addButton(Close, L"Закрыть", 310, 10, 28, 30, L"Закрыть", tips);
    addButton(Main, L"Запись", 22, 176, 120, 40, L"Запись / стоп. В режиме рации — удерживать", tips);
    addButton(Pause, L"Пауза", 150, 176, 52, 40, L"Пауза / продолжить", tips);
    addButton(Save, L"Сохранить", 210, 176, 128, 40, L"Сохранить запись (Ctrl+S)", tips);
    for (int id : {Pin, Menu, Minimize, Close, Main, Pause, Save}) SetWindowSubclass(GetDlgItem(window, id), buttonProc, id, 0);
    TOOLINFOW timerTip{}; timerTip.cbSize = sizeof(timerTip); timerTip.uFlags = TTF_SUBCLASS;
    timerTip.hwnd = window; timerTip.uId = 800;
    timerTip.lpszText = const_cast<wchar_t*>(L"ПКМ — меню. Ctrl+M — размер окна. В уменьшенном виде перетащите за таймер; двойной щелчок — обычное окно.");
    SendMessageW(tips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&timerTip));
    layoutControls();
}
LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE:
        window = hwnd; scale = GetDpiForWindow(hwnd) / 96.f;
        createControls(); SetTimer(hwnd, 1, 40, nullptr); update(); return 0;
    case WM_NCCALCSIZE: return 0;
    case WM_DPICHANGED: {
        scale = HIWORD(wp) / 96.f;
        if (tooltips) { DestroyWindow(tooltips); tooltips = nullptr; }
        while (HWND child = GetWindow(hwnd, GW_CHILD)) DestroyWindow(child);
        for (HFONT font : {normalFont, smallFont, timerFont, titleFont, compactTimerFont}) DeleteObject(font);
        createControls();
        auto* suggested = reinterpret_cast<RECT*>(lp);
        resizeWindow(suggested->left, suggested->top);
        update(); return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps); paint(dc); EndPaint(hwnd, &ps); return 0; }
    case WM_DRAWITEM: drawButton(reinterpret_cast<DRAWITEMSTRUCT*>(lp)); return TRUE;
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; ScreenToClient(hwnd, &point);
        if (compact ? point.x >= px(184) : (point.y < px(45) && point.x < px(215))) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_CONTEXTMENU: {
        POINT position{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (position.x == -1 && position.y == -1) { RECT current{}; GetWindowRect(hwnd, &current); position = {current.right, current.bottom}; }
        menu(&position); return 0;
    }
    case WM_NCRBUTTONUP:
        if (compact && wp == HTCAPTION) { POINT position{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)}; menu(&position); return 0; }
        break;
    case WM_NCLBUTTONDBLCLK:
        if (compact && wp == HTCAPTION) { toggleCompact(); return 0; }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && !globalSpace) releaseSpace();
        break; // DefWindowProc restores keyboard focus when the window activates.
    case WM_TIMER: {
        if (!compact) {
            levels.pop_front(); levels.push_back(recorder.peak());
            auto wave = rect(30, 131, 300, 32); InvalidateRect(hwnd, &wave, FALSE);
        }
        const int seconds = int(recorder.seconds());
        if (seconds != timerSeconds) {
            timerSeconds = seconds;
            auto timer = compact ? rect(184, 10, 112, 44) : rect(20, 73, 320, 56);
            InvalidateRect(hwnd, &timer, FALSE);
        }
        if (held && !(GetAsyncKeyState(VK_SPACE) & 0x8000) && GetCapture() != GetDlgItem(hwnd, Main)) releaseSpace();
        return 0;
    }
    case SpaceDown: pressSpace(); return 0;
    case SpaceUp: releaseSpace(); return 0;
    case Recorder::ErrorMessage:
        recorder.finish(); finalized = true; held = false; notice = L"Запись прервана · можно сохранить"; update(); errorBox(recorder.error()); return 0;
    case ExportDone:
        if (exportThread.joinable()) exportThread.join();
        exporting = false;
        if (wp) {
            std::error_code ec; fs::remove(draft, ec); session = false; finalized = false;
            notice = L"Сохранено · " + savedPath.filename().wstring();
            if (closeAfterSave) { DestroyWindow(hwnd); return 0; }
        } else { notice = L"Не сохранено · повторите или выберите WAV"; errorBox(exportError); }
        closeAfterSave = false; update(); return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case Pin:
            pinned = !pinned; SetWindowPos(hwnd, pinned ? HWND_TOPMOST : HWND_NOTOPMOST, 0,0,0,0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            saveSettings(); update(); break;
        case Menu: menu(); break;
        case Minimize: releaseSpace(); ShowWindow(hwnd, SW_MINIMIZE); break;
        case Close: SendMessageW(hwnd, WM_CLOSE, 0, 0); break;
        case Main: if (!walkie && !exporting) { if (session) saveRecording(); else begin(); } break;
        case Pause:
            if (session && !walkie && !exporting && !finalized) { recorder.setRecording(!recorder.isRecording()); notice.clear(); update(); } break;
        case Save: saveRecording(); break;
        }
        return 0;
    case WM_CLOSE:
        releaseSpace();
        if (exporting) { notice = L"Дождитесь окончания сохранения"; update(); return 0; }
        if (session) {
            recorder.setRecording(false); update(); modal = true;
            const int answer = MessageBoxW(hwnd, L"Сохранить текущую запись перед выходом?\n\nДа — сохранить и выйти.\nНет — выйти, оставив временный WAV для восстановления.",
                L"Есть несохранённая запись", MB_YESNOCANCEL | MB_ICONQUESTION);
            modal = false;
            if (answer == IDCANCEL) return 0;
            if (answer == IDYES) { closeAfterSave = true; saveRecording(); return 0; }
        }
        recorder.finish(); DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        recorder.finish(); if (keyboardHook) UnhookWindowsHookEx(keyboardHook);
        saveSettings(); KillTimer(hwnd, 1); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    dataDir = knownFolder(FOLDERID_LocalAppData) / L"Dictophone";
    int argc = 0; auto args = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i + 1 < argc; ++i) if (std::wstring(args[i]) == L"--data-dir") dataDir = fs::absolute(args[++i]);
    LocalFree(args); loadSettings();
    scale = GetDpiForSystem() / 96.f;
    WNDCLASSEXW cls{}; cls.cbSize = sizeof(cls); cls.style = CS_DBLCLKS; cls.lpfnWndProc = windowProc; cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.lpszClassName = ClassName;
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1)); cls.hIconSm = cls.hIcon;
    RegisterClassExW(&cls);
    RECT size{0, 0, px(compact ? 306 : 360), px(compact ? 64 : 300)};
    // Parent animation must never paint over native child buttons.
    constexpr DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    // WM_NCCALCSIZE extends the client area across the standard caption.
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, ClassName, L"Dictophone", style, CW_USEDEFAULT, CW_USEDEFAULT,
        size.right-size.left, size.bottom-size.top, nullptr, nullptr, instance, nullptr);
    if (!hwnd) { CoUninitialize(); return 1; }
    BOOL dark = TRUE; DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    const MARGINS margins{1,1,1,1}; DwmExtendFrameIntoClientArea(hwnd, &margins);
    const DWORD roundedCorners = 2; DwmSetWindowAttribute(hwnd, 33, &roundedCorners, sizeof(roundedCorners));
    if (pinned) SetWindowPos(hwnd, HWND_TOPMOST, 0,0,0,0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    configureHook(); ShowWindow(hwnd, show); UpdateWindow(hwnd);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!modal && GetAncestor(msg.hwnd, GA_ROOT) == hwnd) {
            if (walkie && !globalSpace && msg.wParam == VK_SPACE && (msg.message == WM_KEYDOWN || msg.message == WM_KEYUP)) {
                if (msg.message == WM_KEYDOWN) pressSpace(); else releaseSpace(); continue;
            }
            if (msg.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000)) {
                if (msg.wParam == 'S') { saveRecording(); continue; }
                if (msg.wParam == 'O') { openFolder(outputDir); continue; }
                if (msg.wParam == 'M') { toggleCompact(); continue; }
            }
            if (IsDialogMessageW(hwnd, &msg)) continue;
        }
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    if (exportThread.joinable()) exportThread.join();
    for (HFONT font : {normalFont, smallFont, timerFont, titleFont, compactTimerFont}) DeleteObject(font);
    CoUninitialize(); return 0;
}
