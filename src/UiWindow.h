// UiWindow.h — the native desktop window.
//
// This replaces the browser-based control panel as the primary interface. The
// process is a GUI-subsystem binary: double-clicking it shows this window, not
// a console. It draws itself with GDI+ into an offscreen bitmap, so there is no
// toolkit dependency and the shipped artefact stays a single .exe.
//
// The window owns nothing but presentation and edits to AppConfig. Every system
// side effect still goes through the hooks main.cpp installs.

#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "Config.h"
#include "Touchpad.h"

// Posted to a running instance by a second launch, so double-clicking the exe
// while it is minimised brings the window back instead of starting a second
// engine that would fight the first one for the touchpad.
constexpr UINT kUiRestoreMessage = WM_APP + 12;

// Registered window class, so the second launch can find the first one.
constexpr wchar_t kUiWindowClass[] = L"TpGesturesMainWindow";

// One line in the event log.
struct UiLogLine {
    uint64_t    atMs = 0;
    std::string kind;
    std::string message;
};

// Everything the window displays about the running engine.
struct UiLive {
    uint64_t frames         = 0;
    uint64_t decodeFail     = 0;
    uint64_t gestures       = 0;
    float    fps            = 0.f;
    bool     dragging       = false;
    std::string dragTarget;
    bool     pinching       = false;
    bool     suppressionActive = false;
    uint64_t startedAtMs    = 0;
    uint32_t rangeX         = 0;
    uint32_t rangeY         = 0;
    std::vector<std::pair<float, float>> pointers;
};

// Injected by main.cpp so the window never links against the engine directly.
struct UiHooks {
    AppConfig*   cfg      = nullptr;
    TouchpadCaps caps;
    bool         haveCaps = false;

    // A setting finished changing. The window has already mutated *cfg; this is
    // the signal to re-arm the engine and persist to disk. Called on mouse-up
    // rather than on every pixel of a slider drag.
    std::function<void()> onApply;

    // "swap-left" / "swap-right" / "test-drag" / "test-pinch".
    std::function<bool(const std::string&, std::string*)> onCommand;

    // Appends to the shared event log, so edits made in the window show up in
    // the history alongside gestures.
    std::function<void(const char* kind, const std::string& message)> onLog;

    // Pulled lazily, only while a page that shows it is visible.
    std::function<std::vector<UiLogLine>()> pullLogs;
};

class UiWindow {
public:
    bool create(HINSTANCE inst, UiHooks* hooks);
    void destroy();

    HWND hwnd() const { return hwnd_; }

    void show();
    void hideToTray();

    // Cheap to call every loop pass; repaints are rate limited internally.
    void updateLive(const UiLive& live);

    // A new event was logged and the log page may need a repaint.
    void notifyNewLog();

    // Set when the user picked Exit from the tray menu or the about page.
    bool exitRequested() const { return exitRequested_; }

    // True when the window is hidden and only the tray icon remains.
    bool hiddenToTray() const { return trayOnly_; }

private:
    // ------------------------------------------------------------- internals
    enum Tab { kGestures = 0, kWindow, kAdvanced, kAbout, kTabCount };

    // Which config field a row or slider edits. Keeping this as an enum instead
    // of a captured lambda means the row table can be rebuilt on every paint
    // and still be read back by the click handler.
    enum class Field : uint8_t {
        None = 0,
        Enabled,        // master switch
        Drag3,          // three-finger window drag
        DragFingers,    // 3 or 4 fingers start the drag
        PinchEnabled,
        PinchAnchored,
        PinchRubber,
        PinchAxis,      // 0 uniform / 1 auto / 2 width / 3 height
        PinchDeadband,  // slider, share of the gap skipped before zooming
        TapEnabled,
        EdgeEnabled,
        Autostart,
        // One mapping row. `arg` is a slot::index(fingers, kind), so every
        // gesture kind is the same field with a different argument -- adding a
        // kind never adds a case to the click handler.
        Bind,
        Sens,           // sliders
        PinchSens,
        Threshold,
        Dominance,
        TapMs,
        TapSlop,        // slider, travel still counted as a tap (device units)
        DoubleTapMs,
        EdgeZone,
        AnimMs,
        Theme,          // appearance popup (follow / light / dark)
    };

    struct RowDef {
        Field field = Field::None;
        int   arg   = 0;
    };

    struct SliderDef {
        Field field = Field::None;
        float lo    = 0.f;
        float hi    = 1.f;
    };

    // Which custom-chord slot a binding row edits, or -1 for rows that are not
    // bindings. Lets the chord live with the binding instead of with the action.
    static int slotOf(Field f, int arg);

    // What a binding row shows on the right: the action name, or the payload
    // when the action is one that carries one.
    std::wstring slotValue(int slotIndex) const;
    // Human name of a slot ("三指 · 轻点"), used by status lines and the log.
    static std::wstring slotLabelW(int s);
    // One log entry per mapping change, routed through the shared hook.
    void logMapping(const std::wstring& line);

    // Keyboard capture for the "custom" action. recordSlot_ is -1 when idle.
    void startRecording(int slot);
    void finishRecording(uint32_t vk);
    int  recordSlot_ = -1;
    ActionId recordPrev_ = ActionId::None;   // restored when recording is cancelled
    // The window that had focus before the recorder stole it. Every chord
    // gesture afterwards lands on the foreground window, so the focus has to
    // go back or "Ctrl+C" fires into the settings window.
    HWND recordPrevFocus_ = nullptr;

    // File picker for the "launch" action. Writes both the path and the binding,
    // and puts the previous action back when the dialog is cancelled.
    void chooseProgram(int slot, ActionId prev);

    // Interactive element ids. Rows and sliders get ids derived from their
    // index in rowDefs_ / sliderDefs_, which is what lets the pages be laid out
    // freely without a hand-maintained id table.
    enum Id {
        kIdNone = 0,

        kIdNav0 = 1, kIdNav1, kIdNav2, kIdNav3,

        // Finger-count segments on the gestures page.
        kIdFingerSeg = 20, kIdFingerSegB, kIdFingerSegC,

        kIdRowBase    = 100,
        kIdSliderBase = 2000,

        kIdQuit = 4000, kIdClearLog,
        kIdTestLeft, kIdTestRight, kIdTestDrag, kIdTestPinch,
        kIdResetAll,
        kIdClearBinding,   // "clear this binding" in the action menu
    };

    struct Hotspot {
        int  id = 0;
        RECT rc{};
    };

    static LRESULT CALLBACK wndProcThunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK dropProcThunk(HWND, UINT, WPARAM, LPARAM);

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handleDrop(HWND h, UINT msg, WPARAM wp, LPARAM lp);

    // ----------------------------------------------------------- painting
    void paint(HDC hdc);
    void paintSidebar(void* g, int w, int h);
    void paintGestures(void* g, int cx, int cw, int y);
    void paintWindowPage(void* g, int cx, int cw, int y);
    void paintAdvanced(void* g, int cx, int cw, int y);
    void paintAbout(void* g, int cx, int cw, int y);
    void paintDropdown(void* g, int w, int h);

    // Row helpers. Each returns the y of the row after the one it drew.
    int  groupHeader(void* g, int cx, int cw, int y, const wchar_t* title);
    int  rowSwitch(void* g, int cx, int cw, int y, const wchar_t* label,
                   const wchar_t* desc, bool on, Field f, bool dim = false);
    // A mapping row: `slotIndex` addresses the cell in the binding table.
    int  rowChoice(void* g, int cx, int cw, int y, const wchar_t* label,
                   int slotIndex, bool dim = false);
    // Same row, but with free-form text on the right (appearance, modes).
    int  rowChoiceText(void* g, int cx, int cw, int y, const wchar_t* label,
                       const std::wstring& value, Field f, int arg,
                       bool dim = false);
    // Segmented control. Each segment gets its own hit id starting at idBase.
    int  rowSegment(void* g, int cx, int cw, int y, const wchar_t* label,
                    const wchar_t* const* names, int count, int cur, int idBase,
                    bool dim = false);
    int  rowSlider(void* g, int cx, int cw, int y, const wchar_t* label,
                   float value, const wchar_t* text, float t, Field f,
                   float lo, float hi, bool dim = false);
    int  rowButton(void* g, int cx, int cw, int y, const wchar_t* label,
                   int id, bool destructive = false);
    int  rowInfo(void* g, int cx, int cw, int y, const wchar_t* label,
                 const std::wstring& value);

    void addHit(int id, RECT r);

    // ------------------------------------------------------------ interaction
    int  hitTest(POINT pt) const;
    void onLButtonDown(POINT pt);
    void onLButtonUp(POINT pt);
    void onMouseMove(POINT pt);
    void onMouseWheel(int delta);
    void activate(int id);
    void setSliderFromPoint(int id, POINT pt);

    void openDropdown(int rowId);
    void closeDropdown();
    void commitDropdown(int optionIndex);

    void applyAndSave();       // calls hooks_->onApply
    void statusMessage(const std::wstring& text, bool isError = false);
    void statusMessage(const std::string& utf8, bool isError = false);

    // --------------------------------------------------------------- tray
    void addTrayIcon();
    void removeTrayIcon();
    void updateTrayTip();
    void showTrayMenu(POINT pt);

    // --------------------------------------------------------------- state
    HWND    hwnd_  = nullptr;
    HWND    drop_  = nullptr;
    HINSTANCE inst_ = nullptr;
    UiHooks* hooks_ = nullptr;

    float   scale_ = 1.f;
    bool    light_ = true;      // follows the system app theme
    int     tab_   = kGestures;
    // Which finger count the gesture page is showing: 0 = three, 1 = four,
    // 2 = five. Presentation only; every count is always stored.
    int     fingerTab_ = 0;
    int     hover_ = kIdNone;
    int     pressed_ = kIdNone;
    int     activeSlider_ = kIdNone;

    std::vector<Hotspot>  hits_;
    std::vector<RowDef>   rowDefs_;
    std::vector<SliderDef> sliderDefs_;
    RECT    sliderTrack_[8]{};   // indexed by id - kIdSliderBase

    // Content scrolling (the gesture page is taller than the window).
    int     scrollPx_  = 0;
    int     scrollMax_ = 0;
    int     contentH_  = 0;
    int     viewportH_ = 0;
    int     hitShift_  = 0;   // -scrollPx_ while page content is being drawn
    int     rowsInCard_ = 0;  // row counter, so separators skip the first row

    // Dropdown state. The menu is a flat list of entries that already know how
    // to draw themselves: an action row, a group caption, or one of the short
    // index menus. That is what lets 30-odd actions stay navigable.
    struct DropItem {
        std::wstring label;
        ActionId     action   = ActionId::None;
        int          index    = -1;      // index menus only
        bool         header   = false;
        bool         selected = false;
        bool         danger   = false;   // "clear this binding"
        int          top      = 0;       // client y, filled during layout
        int          h        = 0;
    };

    int  dropRow_ = kIdNone;
    int  dropHover_ = -1;
    Field dropField_ = Field::None;   // which row opened it
    int  dropArg_ = -1;               // slot index for binding menus
    std::vector<DropItem> dropItems_;
    int  dropScroll_ = 0;
    int  dropMaxScroll_ = 0;
    int  dropContentH_ = 0;
    int  dropViewH_ = 0;
    bool dropIndexMenu_ = false;

    // Live data + repaint pacing
    UiLive  live_;
    uint64_t lastLivePaint_ = 0;
    bool     liveDirty_ = false;
    uint64_t lastFrameSeen_ = 0;
    uint64_t lastDataAt_ = 0;        // for the "no data" hint

    // Log page
    std::vector<UiLogLine> logs_;
    bool    logsDirty_ = true;

    // Status line under the content area
    std::wstring status_;
    bool        statusError_ = false;
    uint64_t    statusAt_ = 0;

    // Tray
    bool    trayAdded_ = false;
    bool    trayOnly_ = false;
    bool    exitRequested_ = false;
    HICON   appIcon_ = nullptr;

    UINT    taskbarCreatedMsg_ = 0;
};
