/* fh2pad: Windows.Gaming.Input.Gamepad activation factory with a keyboard.
 *
 * The Xbox One Forza Horizon 2 wrapper reads input only from Windows.Gaming.Input
 * gamepads (IGamepadStatics: get_Gamepads at start, GamepadAdded/Removed later,
 * GetCurrentReading per frame). This DLL is registered in the bottle as the
 * provider of that class (ActivatableClassId\Windows.Gaming.Input.Gamepad\DllPath),
 * so the wrapper gets it from the normal WinRT activation and is never modified.
 * Its statics list always holds one virtual pad, whose reading is every real pad
 * (from Wine's windows.gaming.input.dll) merged with the keyboard: keyboard and
 * DualSense work together and the game also runs with no controller.
 * FH2_KEYBOARD=0 hands out Wine's own statics unchanged. */
#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static FILE *logf;
static CRITICAL_SECTION log_cs;

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    if (!logf) return;
    EnterCriticalSection(&log_cs);
    fprintf(logf, "%lu tid=%lu ", GetTickCount(), GetCurrentThreadId());
    va_start(ap, fmt);
    vfprintf(logf, fmt, ap);
    va_end(ap);
    fputc('\n', logf);
    fflush(logf);
    LeaveCriticalSection(&log_cs);
}

enum {
    BTN_MENU = 0x1, BTN_VIEW = 0x2, BTN_A = 0x4, BTN_B = 0x8, BTN_X = 0x10, BTN_Y = 0x20,
    BTN_UP = 0x40, BTN_DOWN = 0x80, BTN_LEFT = 0x100, BTN_RIGHT = 0x200,
    BTN_LB = 0x400, BTN_RB = 0x800, BTN_LS = 0x1000, BTN_RS = 0x2000,
};

typedef struct { UINT64 timestamp; UINT32 buttons; double lt, rt, lx, ly, rx, ry; } pad_reading;
typedef struct { double left, right, lt, rt; } pad_vibration;

typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(void *);
    ULONG (STDMETHODCALLTYPE *Release)(void *);
    HRESULT (STDMETHODCALLTYPE *GetIids)(void *, ULONG *, IID **);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(void *, void **);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(void *, int *);
} inspectable_vtbl;

typedef struct {
    inspectable_vtbl base;
    HRESULT (STDMETHODCALLTYPE *get_Vibration)(void *, pad_vibration *);
    HRESULT (STDMETHODCALLTYPE *put_Vibration)(void *, pad_vibration);
    HRESULT (STDMETHODCALLTYPE *GetCurrentReading)(void *, pad_reading *);
} gamepad_vtbl;
typedef struct { const gamepad_vtbl *vt; } gamepad;

typedef struct {
    inspectable_vtbl base;
    HRESULT (STDMETHODCALLTYPE *GetAt)(void *, UINT32, gamepad **);
    HRESULT (STDMETHODCALLTYPE *get_Size)(void *, UINT32 *);
    HRESULT (STDMETHODCALLTYPE *IndexOf)(void *, gamepad *, UINT32 *, BOOLEAN *);
    HRESULT (STDMETHODCALLTYPE *GetMany)(void *, UINT32, UINT32, gamepad **, UINT32 *);
} pad_list_vtbl;
typedef struct { const pad_list_vtbl *vt; } pad_list;

typedef struct {
    inspectable_vtbl base;
    HRESULT (STDMETHODCALLTYPE *add_GamepadAdded)(void *, IUnknown *, INT64 *);
    HRESULT (STDMETHODCALLTYPE *remove_GamepadAdded)(void *, INT64);
    HRESULT (STDMETHODCALLTYPE *add_GamepadRemoved)(void *, IUnknown *, INT64 *);
    HRESULT (STDMETHODCALLTYPE *remove_GamepadRemoved)(void *, INT64);
    HRESULT (STDMETHODCALLTYPE *get_Gamepads)(void *, pad_list **);
} pad_statics_vtbl;
typedef struct { const pad_statics_vtbl *vt; } pad_statics;

static const GUID IID_inspectable = {0xaf86e2e0, 0xb12d, 0x4c6a, {0x9c,0x5a,0xd7,0xaa,0x65,0x10,0x1e,0x90}};
static const GUID IID_agile = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}};
static const GUID IID_gamepad = {0xbc7bb43c, 0x0a69, 0x3903, {0x9e,0x9d,0xa5,0x0f,0x86,0xa4,0x5d,0xe5}};
static const GUID IID_pad_list = {0xeb97bb69, 0x09c9, 0x5a99, {0x86,0xb2,0x3e,0x36,0x08,0x52,0x84,0xd4}};
static const GUID IID_pad_statics = {0x8bbce529, 0xd49c, 0x39e9, {0x95,0x60,0xe4,0x7d,0xde,0x96,0xb7,0xc8}};

#define MAX_REAL_PADS 4

static CRITICAL_SECTION pad_cs;
static pad_statics *real_statics;
static gamepad *real_pads[MAX_REAL_PADS];
static UINT32 nreal_pads, last_real_count = ~0u;
static LARGE_INTEGER qpc_freq, last_read, last_refresh;
static double kb_steer;
static BOOL kb_used;
static pad_vibration vibration;
static LONG64 next_token;

static const struct { int vk; UINT32 button; } key_buttons[] = {
    {VK_RETURN, BTN_A}, {VK_SPACE, BTN_A}, {VK_ESCAPE, BTN_B}, {VK_BACK, BTN_B},
    {'F', BTN_X}, {'R', BTN_Y}, {VK_TAB, BTN_MENU}, {'C', BTN_VIEW},
    {'Q', BTN_LB}, {'E', BTN_RB}, {'H', BTN_LS}, {'V', BTN_RS},
    {'W', BTN_UP}, {'S', BTN_DOWN}, {'A', BTN_LEFT}, {'D', BTN_RIGHT},
};

static BOOL key_down(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

/* keys only count while one of our windows has focus */
static BOOL game_focused(void)
{
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    if (!w) return FALSE;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

/* steering from digital keys: full lock in ~150 ms, back to centre in ~80 ms */
static double approach(double v, double target, double dt)
{
    double rate = (target * v >= 0 && (target > 0 ? target > v : target < v)) ? 6.7 : 12.5;
    double step = rate * dt;
    if (v < target) return v + step < target ? v + step : target;
    return v - step > target ? v - step : target;
}

static double stronger(double a, double b)
{
    return (a < 0 ? -a : a) >= (b < 0 ? -b : b) ? a : b;
}

/* re-read the real pad list at most once a second, so hot-plugged pads show up */
static void refresh_real_pads(LARGE_INTEGER now)
{
    pad_list *list = NULL;
    UINT32 n = 0;
    if (last_refresh.QuadPart && now.QuadPart - last_refresh.QuadPart < qpc_freq.QuadPart) return;
    last_refresh = now;
    for (UINT32 i = 0; i < nreal_pads; i++) real_pads[i]->vt->base.Release(real_pads[i]);
    nreal_pads = 0;
    if (!real_statics || FAILED(real_statics->vt->get_Gamepads(real_statics, &list)) || !list) return;
    list->vt->get_Size(list, &n);
    for (UINT32 i = 0; i < n && nreal_pads < MAX_REAL_PADS; i++)
        if (SUCCEEDED(list->vt->GetAt(list, i, &real_pads[nreal_pads])) && real_pads[nreal_pads]) nreal_pads++;
    list->vt->base.Release(list);
    if (n != last_real_count) logmsg("keyboard pad: %u real gamepad(s) merged", n);
    last_real_count = n;
}

static ULONG STDMETHODCALLTYPE static_addref(void *This) { (void)This; return 1; }
static ULONG STDMETHODCALLTYPE static_release(void *This) { (void)This; return 1; }

static HRESULT STDMETHODCALLTYPE insp_iids(void *This, ULONG *count, IID **iids)
{
    (void)This;
    *count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE insp_class(void *This, void **name)
{
    (void)This;
    *name = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE insp_trust(void *This, int *level)
{
    (void)This;
    *level = 0; /* BaseTrust */
    return S_OK;
}

static HRESULT qi_self(void *This, REFIID riid, const GUID *own, void **out)
{
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_inspectable) || IsEqualGUID(riid, &IID_agile) ||
        IsEqualGUID(riid, own)) {
        *out = This;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

/* virtual pad */

static HRESULT STDMETHODCALLTYPE vpad_qi(void *This, REFIID riid, void **out)
{
    return qi_self(This, riid, &IID_gamepad, out);
}

static HRESULT STDMETHODCALLTYPE vpad_get_vibration(void *This, pad_vibration *v)
{
    (void)This;
    *v = vibration;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE vpad_put_vibration(void *This, pad_vibration v)
{
    (void)This;
    EnterCriticalSection(&pad_cs);
    vibration = v;
    for (UINT32 i = 0; i < nreal_pads; i++) real_pads[i]->vt->put_Vibration(real_pads[i], v);
    LeaveCriticalSection(&pad_cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE vpad_reading(void *This, pad_reading *out)
{
    LARGE_INTEGER now;
    pad_reading r;
    double dt, steer_target = 0, krx = 0, kry = 0;
    UINT32 kbuttons = 0;
    BOOL throttle = FALSE, brake = FALSE;
    (void)This;
    EnterCriticalSection(&pad_cs);
    QueryPerformanceCounter(&now);
    dt = last_read.QuadPart ? (double)(now.QuadPart - last_read.QuadPart) / qpc_freq.QuadPart : 0;
    if (dt > 0.1) dt = 0.1;
    last_read = now;
    refresh_real_pads(now);

    memset(out, 0, sizeof(*out));
    for (UINT32 i = 0; i < nreal_pads; i++) {
        if (FAILED(real_pads[i]->vt->GetCurrentReading(real_pads[i], &r))) continue;
        out->buttons |= r.buttons;
        if (r.lt > out->lt) out->lt = r.lt;
        if (r.rt > out->rt) out->rt = r.rt;
        out->lx = stronger(out->lx, r.lx);
        out->ly = stronger(out->ly, r.ly);
        out->rx = stronger(out->rx, r.rx);
        out->ry = stronger(out->ry, r.ry);
    }

    if (game_focused()) {
        for (size_t i = 0; i < sizeof(key_buttons) / sizeof(key_buttons[0]); i++)
            if (key_down(key_buttons[i].vk)) kbuttons |= key_buttons[i].button;
        throttle = key_down(VK_UP);
        brake = key_down(VK_DOWN);
        steer_target = (key_down(VK_RIGHT) ? 1 : 0) - (key_down(VK_LEFT) ? 1 : 0);
        krx = (key_down('L') ? 1 : 0) - (key_down('J') ? 1 : 0);
        kry = (key_down('I') ? 1 : 0) - (key_down('K') ? 1 : 0);
    }
    kb_steer = approach(kb_steer, steer_target, dt);
    if (!kb_used && (kbuttons || throttle || brake || steer_target || krx || kry)) {
        kb_used = TRUE;
        logmsg("keyboard pad: first key press seen");
    }
    out->buttons |= kbuttons;
    if (throttle) out->rt = 1;
    if (brake) out->lt = 1;
    if (kb_steer != 0) out->lx = kb_steer;
    if (krx != 0) out->rx = krx;
    if (kry != 0) out->ry = kry;
    out->timestamp = now.QuadPart / qpc_freq.QuadPart * 1000000 +
                     now.QuadPart % qpc_freq.QuadPart * 1000000 / qpc_freq.QuadPart;
    LeaveCriticalSection(&pad_cs);
    return S_OK;
}

static const gamepad_vtbl vpad_vt = {
    {vpad_qi, static_addref, static_release, insp_iids, insp_class, insp_trust},
    vpad_get_vibration, vpad_put_vibration, vpad_reading,
};
static gamepad vpad = {&vpad_vt};

/* the one-pad list returned by get_Gamepads */

static HRESULT STDMETHODCALLTYPE vlist_qi(void *This, REFIID riid, void **out)
{
    return qi_self(This, riid, &IID_pad_list, out);
}

static HRESULT STDMETHODCALLTYPE vlist_get_at(void *This, UINT32 index, gamepad **out)
{
    (void)This;
    *out = index == 0 ? &vpad : NULL;
    return index == 0 ? S_OK : E_BOUNDS;
}

static HRESULT STDMETHODCALLTYPE vlist_size(void *This, UINT32 *n)
{
    (void)This;
    *n = 1;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE vlist_index_of(void *This, gamepad *g, UINT32 *index, BOOLEAN *found)
{
    (void)This;
    *index = 0;
    *found = g == &vpad;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE vlist_get_many(void *This, UINT32 start, UINT32 size, gamepad **items, UINT32 *n)
{
    (void)This;
    *n = start == 0 && size > 0 ? 1 : 0;
    if (*n) items[0] = &vpad;
    return S_OK;
}

static const pad_list_vtbl vlist_vt = {
    {vlist_qi, static_addref, static_release, insp_iids, insp_class, insp_trust},
    vlist_get_at, vlist_size, vlist_index_of, vlist_get_many,
};
static pad_list vlist = {&vlist_vt};

/* statics: the virtual pad exists from the start and never leaves, so the
 * added/removed events never fire */

static HRESULT STDMETHODCALLTYPE vstatics_qi(void *This, REFIID riid, void **out)
{
    HRESULT hr = qi_self(This, riid, &IID_pad_statics, out);
    if (hr == E_NOINTERFACE && real_statics) hr = real_statics->vt->base.QueryInterface(real_statics, riid, out);
    return hr;
}

static HRESULT STDMETHODCALLTYPE vstatics_add(void *This, IUnknown *handler, INT64 *token)
{
    (void)This; (void)handler;
    *token = InterlockedIncrement64(&next_token);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE vstatics_remove(void *This, INT64 token)
{
    (void)This; (void)token;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE vstatics_gamepads(void *This, pad_list **out)
{
    (void)This;
    *out = &vlist;
    return S_OK;
}

static const pad_statics_vtbl vstatics_vt = {
    {vstatics_qi, static_addref, static_release, insp_iids, insp_class, insp_trust},
    vstatics_add, vstatics_remove, vstatics_add, vstatics_remove, vstatics_gamepads,
};
static pad_statics vstatics = {&vstatics_vt};

/* the activation factory handed to combase: it only has to answer QueryInterface
 * for IGamepadStatics, which is what RoGetActivationFactory asks it for */
static const GUID IID_activation_factory = {0x00000035, 0x0000, 0x0000, {0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};

typedef struct {
    inspectable_vtbl base;
    HRESULT (STDMETHODCALLTYPE *ActivateInstance)(void *, void **);
} factory_vtbl;
typedef struct { const factory_vtbl *vt; } factory;

static factory *real_factory;

static HRESULT STDMETHODCALLTYPE vfactory_qi(void *This, REFIID riid, void **out)
{
    HRESULT hr = qi_self(This, riid, &IID_activation_factory, out);
    if (hr != E_NOINTERFACE) return hr;
    if (IsEqualGUID(riid, &IID_pad_statics)) {
        *out = &vstatics;
        return S_OK;
    }
    return real_factory ? real_factory->vt->base.QueryInterface(real_factory, riid, out) : E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE vfactory_activate(void *This, void **out)
{
    (void)This;
    *out = NULL;
    return E_NOTIMPL;   /* Gamepad has no default constructor */
}

static const factory_vtbl vfactory_vt = {
    {vfactory_qi, static_addref, static_release, insp_iids, insp_class, insp_trust},
    vfactory_activate,
};
static factory vfactory = {&vfactory_vt};

typedef HRESULT (WINAPI *get_activation_factory_fn)(void *, void **);

/* Wine's windows.gaming.input.dll, loaded by full path: it provides the real pads */
static HRESULT real_activation_factory(void *cls, void **out)
{
    static get_activation_factory_fn get;
    if (!get) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);
        HMODULE m;
        if (!n || n > MAX_PATH - 32) return E_FAIL;
        strcat(path, "\\windows.gaming.input.dll");
        if (!(m = LoadLibraryA(path)) || !(get = (get_activation_factory_fn)GetProcAddress(m, "DllGetActivationFactory"))) {
            logmsg("cannot load %s", path);
            return E_FAIL;
        }
    }
    return get(cls, out);
}

HRESULT WINAPI DllGetActivationFactory(void *cls, void **out)
{
    char v[8] = "";
    HRESULT hr;
    void *real = NULL;
    if (!out) return E_POINTER;
    hr = real_activation_factory(cls, &real);
    if (GetEnvironmentVariableA("FH2_KEYBOARD", v, sizeof(v)) && v[0] == '0') {
        *out = real;
        return hr;
    }
    EnterCriticalSection(&pad_cs);
    if (SUCCEEDED(hr) && real) {
        if (!real_factory) {
            real_factory = real;
            if (FAILED(real_factory->vt->base.QueryInterface(real_factory, &IID_pad_statics, (void **)&real_statics)))
                real_statics = NULL;
        } else ((factory *)real)->vt->base.Release(real);
    }
    LeaveCriticalSection(&pad_cs);
    logmsg("Gamepad factory requested (Wine's hr %#lx, statics %p), virtual pad handed out", hr, (void *)real_statics);
    *out = &vfactory;
    return S_OK;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);
    InitializeCriticalSection(&log_cs);
    InitializeCriticalSection(&pad_cs);
    QueryPerformanceFrequency(&qpc_freq);
    logf = fopen("data\\logs\\fh2pad.log", "a");
    logmsg("fh2pad loaded");
    return TRUE;
}
