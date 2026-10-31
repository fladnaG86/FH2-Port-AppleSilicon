/* fh2port d3d12.dll: a D3D12 layer over D3DMetal for running the Xbox One
 * Forza Horizon 2 compatibility wrapper on Apple Silicon (CrossOver + GPTK).
 * It sits between the wrapper and D3DMetal and never modifies the wrapper.
 *
 * D3DMetal does not implement ID3D12Device3::OpenExistingHeapFromAddress, which
 * the wrapper needs to put GPU heaps over the game's (title) memory. We emulate it:
 *   - create a normal CUSTOM/WRITE_BACK heap, place one buffer over all of it and
 *     Map it: that pointer is the CPU view ("shadow") of the GPU memory;
 *   - before every ExecuteCommandLists copy what the CPU changed in title memory
 *     into the shadow;
 *   - defer every queue Signal: a worker waits for the GPU, copies what the GPU
 *     changed in the shadow back into title memory, then signals the fence from
 *     the CPU, so the wrapper never sees a fence pass before the data is visible.
 * Change tracking is three-way against a "last synced" copy, in 64-byte blocks.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define BLOCK 64
#define PAGE 4096
#define MAX_REGIONS 8192
#define DEV_SLOTS 82
#define QUEUE_SLOTS 19

typedef HRESULT (WINAPI *create_device_fn)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef HRESULT (WINAPI *debug_iface_fn)(REFIID, void **);
typedef HRESULT (WINAPI *serialize_rs_fn)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION,
                                          ID3DBlob **, ID3DBlob **);

static create_device_fn real_create_device;
static debug_iface_fn real_get_debug_interface;
static serialize_rs_fn real_serialize_rs;

static FILE *logf;
static CRITICAL_SECTION log_cs;
/* FH2_QUIET=1: no periodic draws/stats lines and no control file polling (stutter test) */
static BOOL quiet;
/* FH2_MIN=1: minimal mode. No D3D12 interception at all: the device is the real one,
 * untouched, so a complete D3D12 implementation (vkd3d-proton, DXVK) drives the game
 * as it is. The fiber fix and the ntdll hooks (FlushProcessWriteBuffers, address
 * placement) stay on: they fix Wine on macOS, not D3DMetal. */
static BOOL minimal;

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

/* stutter probe: a hooked call slower than 4 ms is logged with its duration */
static LARGE_INTEGER slow_freq;

static LONG64 slow_start(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static void slow_end(const char *what, LONG64 t0)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (slow_freq.QuadPart && (t.QuadPart - t0) * 1000 > slow_freq.QuadPart * 4)
        logmsg("slow: %s %lld ms", what, (t.QuadPart - t0) * 1000 / slow_freq.QuadPart);
}

/* ---------------- regions ---------------- */

typedef struct {
    uint8_t *title, *shadow, *last;
    SIZE_T size;
    ID3D12Heap *heap;
    ID3D12Resource *buffer;
    D3D12_GPU_VIRTUAL_ADDRESS gva;
} region;

static region regions[MAX_REGIONS];
static LONG nregions;
static region heaps[MAX_REGIONS];   /* title heaps: title, size, heap */
static LONG nheaps;
static uint64_t stat_textures;
static SRWLOCK sync_lock = SRWLOCK_INIT;
static uint64_t stat_up_bytes, stat_down_bytes, stat_conflicts, stat_syncs, stat_imm, stat_imm_applied;
static uint64_t stat_wbi_copied, stat_wbi_unknown, stat_tile_updates;

static void sync_up(void)
{
    AcquireSRWLockExclusive(&sync_lock);
    for (LONG r = 0; r < nregions; r++) {
        region *g = &regions[r];
        for (SIZE_T p = 0; p < g->size; p += PAGE) {
            SIZE_T pend = min(p + PAGE, g->size);
            if (!memcmp(g->title + p, g->last + p, pend - p)) continue;
            for (SIZE_T b = p; b < pend; b += BLOCK) {
                SIZE_T n = min(BLOCK, pend - b);
                if (memcmp(g->title + b, g->last + b, n)) {
                    memcpy(g->last + b, g->title + b, n);
                    memcpy(g->shadow + b, g->last + b, n);
                    stat_up_bytes += n;
                }
            }
        }
    }
    ReleaseSRWLockExclusive(&sync_lock);
}

static void sync_down(void)
{
    D3D12_RANGE none = {0, 0};
    AcquireSRWLockExclusive(&sync_lock);
    for (LONG r = 0; r < nregions; r++) {
        region *g = &regions[r];
        uint8_t *ptr = NULL;
        /* re-map so a staging-backed Map picks up what the GPU wrote */
        ID3D12Resource_Unmap(g->buffer, 0, &none);
        if (SUCCEEDED(ID3D12Resource_Map(g->buffer, 0, NULL, (void **)&ptr)) && ptr) {
            if (ptr != g->shadow) logmsg("region %ld moved %p -> %p", r, g->shadow, ptr);
            g->shadow = ptr;
        }
        for (SIZE_T p = 0; p < g->size; p += PAGE) {
            SIZE_T pend = min(p + PAGE, g->size);
            if (!memcmp(g->shadow + p, g->last + p, pend - p)) continue;
            for (SIZE_T b = p; b < pend; b += BLOCK) {
                SIZE_T n = min(BLOCK, pend - b);
                if (!memcmp(g->shadow + b, g->last + b, n)) continue;
                if (memcmp(g->title + b, g->last + b, n)) {
                    /* both sides changed: keep the CPU's, it goes up on the next submit */
                    stat_conflicts++;
                    continue;
                }
                memcpy(g->last + b, g->shadow + b, n);
                memcpy(g->title + b, g->last + b, n);
                stat_down_bytes += n;
            }
        }
    }
    if ((++stat_syncs & 1023) == 0 && !quiet)
        logmsg("stats syncs=%llu up=%llu down=%llu conflicts=%llu regions=%ld heaps=%ld textures=%llu imm=%llu applied=%llu wbi_copied=%llu wbi_unknown=%llu tile_updates=%llu",
               stat_syncs, stat_up_bytes, stat_down_bytes, stat_conflicts, nregions, nheaps, stat_textures,
               stat_imm, stat_imm_applied, stat_wbi_copied, stat_wbi_unknown, stat_tile_updates);
    ReleaseSRWLockExclusive(&sync_lock);
}

/* ---------------- deferred signals ---------------- */

/* WriteBufferImmediate into a title region (end-of-pipe value writes):
 * D3DMetal's write never shows up in the mapped buffer, so we replay it on the CPU
 * once the GPU has passed the submission, before the caller's fence is signalled */
typedef struct { volatile LONG *dst; LONG value; } imm_write;
/* vb/root_srv/watch: bound state, kept only for "watch" logging */
typedef struct {
    void *list; imm_write *w; UINT n, cap; void *pso;
    D3D12_VERTEX_BUFFER_VIEW vb[4]; D3D12_GPU_VIRTUAL_ADDRESS root_srv[16]; BOOL watch;
} list_writes;
/* every command list that records writes gets an entry; entries are freed when
 * the list is destroyed (hk_list_release), otherwise the table fills up after a
 * few thousand lists and later lists silently lose their writes */
#define LIST_TABLE 16384
#define LIST_GONE ((void *)1)   /* tombstone: keeps probe chains intact */
static list_writes list_table[LIST_TABLE];
static CRITICAL_SECTION list_cs;

/* label watch: the wrapper stops with "the GPU passed the work that writes the
 * value N at <address> and the value is not there". FH2_LABEL=<hex address> logs
 * every break in the sequence of values written to that address, once when the
 * write is recorded in a command list and once when it is applied after the GPU,
 * so a crash shows whether N was lost before or after us. */
static volatile LONG *label_addr;
static volatile LONG label_recorded = -1, label_applied = -1;
static volatile LONG64 label_n_recorded, label_n_applied;

static void label_note(const imm_write *w, BOOL applied)
{
    volatile LONG *last = applied ? &label_applied : &label_recorded;
    LONG64 n;
    if (!label_addr || w->dst != label_addr) return;
    n = InterlockedIncrement64(applied ? &label_n_applied : &label_n_recorded);
    if (*last != -1 && w->value != *last + 1)
        logmsg("label %p %s %ld after %ld (#%lld)", (void *)label_addr, applied ? "applied" : "recorded", w->value, *last, n);
    else if ((n & 4095) == 0)
        logmsg("label %p %s %ld (#%lld)", (void *)label_addr, applied ? "applied" : "recorded", w->value, n);
    *last = w->value;
}

/* the label values in a set of writes, for the trace: first and last, or -1 */
static void label_span(const imm_write *w, UINT n, LONG *first, LONG *last)
{
    *first = *last = -1;
    for (UINT i = 0; label_addr && i < n; i++)
        if (w[i].dst == label_addr) {
            if (*first == -1) *first = w[i].value;
            *last = w[i].value;
        }
}

static void push_write(imm_write **w, UINT *n, UINT *cap, imm_write v)
{
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *w = *w ? HeapReAlloc(GetProcessHeap(), 0, *w, *cap * sizeof(**w))
                : HeapAlloc(GetProcessHeap(), 0, *cap * sizeof(**w));
    }
    (*w)[(*n)++] = v;
}

/* caller holds list_cs */
static list_writes *list_entry(void *list, BOOL create)
{
    static BOOL full_logged;
    UINT h = (UINT)(((uintptr_t)list >> 4) % LIST_TABLE);
    list_writes *tomb = NULL;
    for (UINT i = 0; i < LIST_TABLE; i++, h = (h + 1) % LIST_TABLE) {
        if (list_table[h].list == list) return &list_table[h];
        if (list_table[h].list == LIST_GONE) {
            if (!tomb) tomb = &list_table[h];
            continue;
        }
        if (!list_table[h].list) {
            if (!create) return NULL;
            if (!tomb) tomb = &list_table[h];
            break;
        }
    }
    if (!create || !tomb) {
        if (create && !full_logged) { full_logged = TRUE; logmsg("command list table full: writes of new lists are lost"); }
        return NULL;
    }
    memset(tomb, 0, sizeof(*tomb));
    tomb->list = list;
    return tomb;
}

/* caller holds list_cs */
static void list_forget(void *list)
{
    list_writes *e = list_entry(list, FALSE);
    if (!e) return;
    if (e->w) HeapFree(GetProcessHeap(), 0, e->w);
    memset(e, 0, sizeof(*e));
    e->list = LIST_GONE;
}

typedef struct job {
    struct job *next;
    imm_write *w;
    UINT nw;
    ID3D12Fence *gpu_fence;   /* our fence, signalled by the queue */
    UINT64 gpu_value;
    ID3D12Fence *fence;       /* the caller's fence */
    UINT64 value;
} job;

/* Each hooked queue has its own fence, job list, staged writes and worker thread.
 * Azimuth 1.0 drives two queues at once: with one shared fence their values
 * interleaved, and one FIFO worker could wait on queue B's GPU work while B
 * waited on a fence only a later job of queue A would signal. */
#define MAX_QUEUES 8
typedef struct {
    ID3D12CommandQueue *q;
    ID3D12Fence *fence;          /* ours, signalled by this queue only */
    volatile LONG64 value;
    job *head, *tail;
    CRITICAL_SECTION cs;         /* job list */
    CRITICAL_SECTION signal_cs;  /* keeps job order == this queue's GPU order */
    HANDLE sem;
    imm_write *staged;
    UINT nstaged, capstaged;
} qstate;
static qstate qstates[MAX_QUEUES];
static volatile LONG nqueues;

static qstate *find_qstate(ID3D12CommandQueue *q)
{
    for (LONG i = 0; i < nqueues; i++)
        if (qstates[i].q == q) return &qstates[i];
    return NULL;
}

static void poll_control(void);

static DWORD WINAPI signal_worker(void *arg)
{
    qstate *qs = arg;
    BOOL first = qs == &qstates[0];   /* the first queue's worker also does the idle duties */
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    for (;;) {
        job *j;
        static DWORD last_poll;
        if (first && !quiet && GetTickCount() - last_poll > 500) { last_poll = GetTickCount(); poll_control(); }
        if (WaitForSingleObject(qs->sem, 2) == WAIT_TIMEOUT) {
            if (first && nregions) sync_down();   /* the game also polls GPU-written values directly */
            continue;
        }
        EnterCriticalSection(&qs->cs);
        j = qs->head;
        qs->head = j->next;
        if (!qs->head) qs->tail = NULL;
        LeaveCriticalSection(&qs->cs);

        if (ID3D12Fence_GetCompletedValue(j->gpu_fence) < j->gpu_value) {
            ID3D12Fence_SetEventOnCompletion(j->gpu_fence, j->gpu_value, ev);
            WaitForSingleObject(ev, INFINITE);
        }
        for (UINT i = 0; i < j->nw; i++) {
            InterlockedExchange(j->w[i].dst, j->w[i].value);
            label_note(&j->w[i], TRUE);
        }
        stat_imm_applied += j->nw;
        if (j->w) HeapFree(GetProcessHeap(), 0, j->w);
        sync_down();
        if (label_addr) {
            LONG first, last;
            label_span(j->w, j->nw, &first, &last);
            logmsg("trace done fence %p=%llu labels %ld..%ld (gpu %llu)", (void *)j->fence, (unsigned long long)j->value,
                   first, last, (unsigned long long)j->gpu_value);
        }
        ID3D12Fence_Signal(j->fence, j->value);
        ID3D12Fence_Release(j->fence);
        HeapFree(GetProcessHeap(), 0, j);
    }
    return 0;
}

/* ---------------- queue hooks ---------------- */

typedef void (STDMETHODCALLTYPE *execute_fn)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
typedef HRESULT (STDMETHODCALLTYPE *signal_fn)(ID3D12CommandQueue *, ID3D12Fence *, UINT64);

static void **queue_vt_orig;
static void *queue_vt[QUEUE_SLOTS];

typedef void (STDMETHODCALLTYPE *update_tiles_fn)(ID3D12CommandQueue *, ID3D12Resource *, UINT,
                                                  const D3D12_TILED_RESOURCE_COORDINATE *, const D3D12_TILE_REGION_SIZE *,
                                                  ID3D12Heap *, UINT, const D3D12_TILE_RANGE_FLAGS *, const UINT *,
                                                  const UINT *, D3D12_TILE_MAPPING_FLAGS);
static void STDMETHODCALLTYPE hk_update_tiles(ID3D12CommandQueue *q, ID3D12Resource *res, UINT nregions_, 
                                              const D3D12_TILED_RESOURCE_COORDINATE *coords, const D3D12_TILE_REGION_SIZE *sizes,
                                              ID3D12Heap *heap, UINT nranges, const D3D12_TILE_RANGE_FLAGS *rflags,
                                              const UINT *starts, const UINT *counts, D3D12_TILE_MAPPING_FLAGS flags)
{
    if (stat_tile_updates++ < 16)
        logmsg("UpdateTileMappings res %p regions %u heap %p ranges %u (first count %u)", res, nregions_, heap, nranges,
               counts ? counts[0] : 0);
    ((update_tiles_fn)queue_vt_orig[8])(q, res, nregions_, coords, sizes, heap, nranges, rflags, starts, counts, flags);
}

static void STDMETHODCALLTYPE hk_execute(ID3D12CommandQueue *q, UINT n, ID3D12CommandList *const *lists)
{
    LONG64 t0 = slow_start();
    qstate *qs = find_qstate(q);
    if (nregions && qs) {
        sync_up();
        slow_end("sync_up", t0);
        EnterCriticalSection(&qs->signal_cs);
        EnterCriticalSection(&list_cs);
        for (UINT i = 0; i < n; i++) {
            list_writes *e = list_entry(lists[i], FALSE);
            for (UINT k = 0; e && k < e->n; k++) push_write(&qs->staged, &qs->nstaged, &qs->capstaged, e->w[k]);
            if (label_addr && e && e->n) {
                LONG first, last;
                label_span(e->w, e->n, &first, &last);
                if (first != -1) logmsg("trace exec list %p labels %ld..%ld", (void *)lists[i], first, last);
            }
        }
        LeaveCriticalSection(&list_cs);
        LeaveCriticalSection(&qs->signal_cs);
    }
    t0 = slow_start();
    ((execute_fn)queue_vt_orig[10])(q, n, lists);
    slow_end("ExecuteCommandLists", t0);
}

static HRESULT STDMETHODCALLTYPE hk_signal(ID3D12CommandQueue *q, ID3D12Fence *fence, UINT64 value)
{
    job *j;
    HRESULT hr;
    qstate *qs = find_qstate(q);
    if (!nregions || !qs || !qs->fence)
        return ((signal_fn)queue_vt_orig[14])(q, fence, value);

    j = HeapAlloc(GetProcessHeap(), 0, sizeof(*j));
    EnterCriticalSection(&qs->signal_cs);
    j->gpu_fence = qs->fence;
    j->gpu_value = InterlockedIncrement64(&qs->value);
    hr = ((signal_fn)queue_vt_orig[14])(q, qs->fence, j->gpu_value);
    if (FAILED(hr)) {
        LeaveCriticalSection(&qs->signal_cs);
        HeapFree(GetProcessHeap(), 0, j);
        return ((signal_fn)queue_vt_orig[14])(q, fence, value);
    }
    ID3D12Fence_AddRef(fence);
    j->fence = fence;
    j->value = value;
    j->w = qs->staged;
    j->nw = qs->nstaged;
    if (label_addr) {
        LONG first, last;
        label_span(j->w, j->nw, &first, &last);
        logmsg("trace signal fence %p=%llu labels %ld..%ld (gpu %llu)", (void *)fence, (unsigned long long)value,
               first, last, (unsigned long long)j->gpu_value);
    }
    qs->staged = NULL;
    qs->nstaged = qs->capstaged = 0;
    j->next = NULL;
    EnterCriticalSection(&qs->cs);
    if (qs->tail) qs->tail->next = j; else qs->head = j;
    qs->tail = j;
    LeaveCriticalSection(&qs->cs);
    LeaveCriticalSection(&qs->signal_cs);
    ReleaseSemaphore(qs->sem, 1, NULL);
    return S_OK;
}

/* own fence, job list and worker for a new queue */
static void add_qstate(ID3D12CommandQueue *q, D3D12_COMMAND_LIST_TYPE type)
{
    static SRWLOCK add_lock = SRWLOCK_INIT;
    ID3D12Device *dev = NULL;
    qstate *qs;
    AcquireSRWLockExclusive(&add_lock);
    if (nqueues == MAX_QUEUES) {
        ReleaseSRWLockExclusive(&add_lock);
        logmsg("queue %p: more than %d queues, its signals are not deferred", q, MAX_QUEUES);
        return;
    }
    qs = &qstates[nqueues];
    memset(qs, 0, sizeof(*qs));
    qs->q = q;
    InitializeCriticalSection(&qs->cs);
    InitializeCriticalSection(&qs->signal_cs);
    qs->sem = CreateSemaphoreA(NULL, 0, 0x7fffffff, NULL);
    if (SUCCEEDED(ID3D12CommandQueue_GetDevice(q, &IID_ID3D12Device, (void **)&dev)) && dev) {
        ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&qs->fence);
        ID3D12Device_Release(dev);
    }
    CloseHandle(CreateThread(NULL, 0, signal_worker, qs, 0, NULL));
    InterlockedIncrement(&nqueues);   /* published last: find_qstate reads without the lock */
    ReleaseSRWLockExclusive(&add_lock);
    logmsg("queue %p type %d: own fence %p and signal worker", q, type, (void *)qs->fence);
}

static void hook_queue(ID3D12CommandQueue *q, D3D12_COMMAND_LIST_TYPE type)
{
    void ***obj = (void ***)q;
    if (*obj == queue_vt) return;
    add_qstate(q, type);
    if (!queue_vt_orig) {
        queue_vt_orig = *obj;
        memcpy(queue_vt, queue_vt_orig, sizeof(queue_vt));
        queue_vt[8] = (void *)hk_update_tiles;
        queue_vt[10] = (void *)hk_execute;
        queue_vt[14] = (void *)hk_signal;
    } else if (*obj != queue_vt_orig) {
        logmsg("queue %p has an unexpected vtable %p, not hooked", q, *obj);
        return;
    }
    *obj = queue_vt;
    logmsg("hooked queue %p", q);
}

/* ---------------- buffer table + immediate-write emulation ---------------- */

/* D3DMetal drops WriteBufferImmediate. Every write is turned into a CopyBufferRegion
 * from a persistently mapped upload ring, which needs GPU VA -> buffer resource. */
typedef struct { D3D12_GPU_VIRTUAL_ADDRESS gva; UINT64 size; ID3D12Resource *res; } buffer_entry;
static buffer_entry *buffers;
static UINT nbuffers, capbuffers;
static SRWLOCK buffers_lock = SRWLOCK_INIT;
static ID3D12Device *the_device;
static ID3D12Resource *ring;
static uint32_t *ring_ptr;
static volatile LONG64 ring_pos;
#define RING_SIZE (16u << 20)

static void record_buffer(void *obj, const D3D12_RESOURCE_DESC *desc)
{
    ID3D12Resource *res;
    if (!obj || !desc || desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return;
    if (FAILED(IUnknown_QueryInterface((IUnknown *)obj, &IID_ID3D12Resource, (void **)&res))) return;
    AcquireSRWLockExclusive(&buffers_lock);
    if (nbuffers == capbuffers) {
        capbuffers = capbuffers ? capbuffers * 2 : 1024;
        buffers = buffers ? HeapReAlloc(GetProcessHeap(), 0, buffers, capbuffers * sizeof(*buffers))
                          : HeapAlloc(GetProcessHeap(), 0, capbuffers * sizeof(*buffers));
    }
    buffers[nbuffers].gva = ID3D12Resource_GetGPUVirtualAddress(res);
    buffers[nbuffers].size = desc->Width;
    buffers[nbuffers].res = res;   /* not owned: entries may go stale, newest match wins */
    nbuffers++;
    ReleaseSRWLockExclusive(&buffers_lock);
    ID3D12Resource_Release(res);
}

static ID3D12Resource *find_buffer(D3D12_GPU_VIRTUAL_ADDRESS va, UINT64 *offset)
{
    ID3D12Resource *res = NULL;
    AcquireSRWLockShared(&buffers_lock);
    for (UINT i = nbuffers; i-- > 0;)
        if (va >= buffers[i].gva && va + 4 <= buffers[i].gva + buffers[i].size) {
            res = buffers[i].res;
            *offset = va - buffers[i].gva;
            break;
        }
    ReleaseSRWLockShared(&buffers_lock);
    return res;
}

static BOOL ensure_ring(void)
{
    D3D12_HEAP_PROPERTIES upload = {D3D12_HEAP_TYPE_UPLOAD, 0, 0, 0, 0};
    D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, RING_SIZE, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    ID3D12Resource *r = NULL;
    void *ptr = NULL;
    if (ring_ptr) return TRUE;
    if (!the_device) return FALSE;
    if (FAILED(ID3D12Device_CreateCommittedResource(the_device, &upload, D3D12_HEAP_FLAG_NONE, &rd,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&r)))
        return FALSE;
    if (FAILED(ID3D12Resource_Map(r, 0, NULL, &ptr))) { ID3D12Resource_Release(r); return FALSE; }
    if (InterlockedCompareExchangePointer((void **)&ring, r, NULL)) {
        ID3D12Resource_Release(r);   /* another thread won */
        while (!ring_ptr) Sleep(0);
        return TRUE;
    }
    ring_ptr = ptr;
    logmsg("immediate-write ring %p mapped at %p", r, ptr);
    return TRUE;
}

/* ---------------- command list hooks ---------------- */

#define LIST_SLOTS 84
typedef HRESULT (STDMETHODCALLTYPE *list_reset_fn)(ID3D12GraphicsCommandList *, ID3D12CommandAllocator *, ID3D12PipelineState *);
typedef void (STDMETHODCALLTYPE *wbi_fn)(ID3D12GraphicsCommandList2 *, UINT, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *,
                                         const D3D12_WRITEBUFFERIMMEDIATE_MODE *);
static void **list_vt_orig;
static void *list_vt[LIST_SLOTS];

typedef ULONG (STDMETHODCALLTYPE *release_fn)(void *);

static ULONG STDMETHODCALLTYPE hk_list_release(ID3D12GraphicsCommandList *l)
{
    ULONG r = ((release_fn)list_vt_orig[2])(l);
    if (!r) {
        EnterCriticalSection(&list_cs);
        list_forget(l);
        LeaveCriticalSection(&list_cs);
    }
    return r;
}

static HRESULT STDMETHODCALLTYPE hk_list_reset(ID3D12GraphicsCommandList *l, ID3D12CommandAllocator *a, ID3D12PipelineState *ps)
{
    list_writes *e;
    EnterCriticalSection(&list_cs);
    if ((e = list_entry(l, TRUE))) {
        e->n = 0; e->pso = ps; e->watch = FALSE;
        memset(e->vb, 0, sizeof(e->vb));
        memset(e->root_srv, 0, sizeof(e->root_srv));
    }
    LeaveCriticalSection(&list_cs);
    return ((list_reset_fn)list_vt_orig[10])(l, a, ps);
}

static void STDMETHODCALLTYPE hk_wbi(ID3D12GraphicsCommandList2 *l, UINT count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *params,
                                     const D3D12_WRITEBUFFERIMMEDIATE_MODE *modes)
{
    for (UINT i = 0; i < count; i++) {
        D3D12_GPU_VIRTUAL_ADDRESS va = params[i].Dest;
        for (LONG r = 0; r < nregions; r++) {
            region *g = &regions[r];
            if (va >= g->gva && va + 4 <= g->gva + g->size) {
                imm_write w = {(volatile LONG *)(g->title + (va - g->gva)), (LONG)params[i].Value};
                list_writes *e;
                EnterCriticalSection(&list_cs);
                if ((e = list_entry(l, TRUE))) push_write(&e->w, &e->n, &e->cap, w);
                LeaveCriticalSection(&list_cs);
                label_note(&w, FALSE);
                if (stat_imm++ < 8) logmsg("WriteBufferImmediate %#llx (title %p) = %u", va, w.dst, params[i].Value);
                goto next;
            }
        }
        {
            UINT64 off;
            ID3D12Resource *dst = find_buffer(va, &off);
            if (dst && ensure_ring()) {
                UINT64 slot = (UINT64)(InterlockedIncrement64(&ring_pos) - 1) % (RING_SIZE / 4);
                ring_ptr[slot] = params[i].Value;
                ID3D12GraphicsCommandList2_CopyBufferRegion(l, dst, off, ring, slot * 4, 4);
                if (stat_wbi_copied++ < 8) logmsg("WriteBufferImmediate %#llx -> copy into %p+%#llx = %u", va, dst, off, params[i].Value);
            } else if (stat_wbi_unknown++ < 8) {
                logmsg("WriteBufferImmediate %#llx: no buffer known, value %u dropped", va, params[i].Value);
            }
        }
    next:;
    }
}

/* PSOs whose vertex shader reads SV_VertexID: D3DMetal leaves BaseVertexLocation out of it */
#define VID_SET 8192
static void *vid_psos[VID_SET];
static volatile LONG n_vid_psos, n_psos;
static BOOL skip_vid, skip_ei;

static BOOL vs_reads_vertex_id(const void *code, SIZE_T len)
{
    const uint8_t *d = code;
    uint32_t nchunks, off, size;
    if (!code || len < 32 || memcmp(d, "DXBC", 4)) return FALSE;
    nchunks = *(const uint32_t *)(d + 28);
    for (uint32_t c = 0; c < nchunks; c++) {
        off = *(const uint32_t *)(d + 32 + 4 * c);
        if (memcmp(d + off, "SHEX", 4) && memcmp(d + off, "SHDR", 4)) continue;
        size = *(const uint32_t *)(d + off + 4) / 4;
        const uint32_t *t = (const uint32_t *)(d + off + 8);
        for (uint32_t k = 2; k < size;) {
            uint32_t op = t[k] & 0x7ff, ln = op == 0x35 ? t[k + 1] : (t[k] >> 24) & 0x7f;
            if (!ln) break;
            if (op == 0x60 && t[k + ln - 1] == 6) return TRUE;   /* dcl_input_sgv ..., vertex_id */
            k += ln;
        }
    }
    return FALSE;
}

/* every PSO gets a sequential id; a control file can hide draws by id for live bisection */
#define PSO_IDS 16384
/* the full input layout is kept next to the shaders: a shader signature that asks
 * for a semantic the layout does not supply gets zero, which silently changes how a
 * prop is oriented */
#define PSO_ELEMS 16
typedef struct { char sem[16]; UINT index; UINT slot; UINT offset; DXGI_FORMAT fmt; UINT cls; UINT step; } pso_elem;
static struct {
    void *pso; LONG id; const void *vs; SIZE_T vs_len; const void *ps; SIZE_T ps_len;
    UINT n_elem; pso_elem elem[PSO_ELEMS];
    void *tint;
    void *patched;   /* twin with FH2_PATCH applied to its VS */
    uint32_t hash;   /* of VS+PS bytecode: unlike the id, the same in every run */
} pso_ids[PSO_IDS];
static volatile LONG filter_mask, filter_value, filter_on;
/* tinting: a bisection that keeps every draw. PSO ids depend on the order the
 * library loads PSOs from several threads, so twins are chosen by shader hash. Hiding draws hangs the game (some of
 * them write values the CPU waits for), so instead each PSO with per-instance
 * input gets a twin whose render target 0 blends to the blend factor (magenta);
 * shaders, UAV writes and depth stay the same. FH2_TINT=1 builds the twins,
 * ctl "tint <mask> <value>" swaps them in for matching ids, "tint off" stops. */
static BOOL tint_build;
static volatile LONG tint_mask, tint_value, tint_on;
#define TINT_SET 64
static uint32_t tint_set[TINT_SET];
static volatile LONG n_tint_set;   /* > 0: tint exactly these hashes instead of mask/value */
static volatile LONG n_tint_built, n_tint_failed;
/* shader patch experiment: FH2_PATCH=<hash>:<dword>:<value> (hex) builds a twin
 * of the PSOs with that shader hash whose VS has one dword replaced; ctl
 * "patch on|off" swaps it in live, for an A/B on the same frame */
static uint32_t patch_hash, patch_dword, patch_value;
static BOOL patch_build, patch_auto;

/* FH2_PATCH=auto: the prop/tree shaders choose between a per-instance matrix in
 * t64 (dcl_resource_structured t64, 288) and the compact I_POS/I_XAXIS/I_YAXIS or
 * constant-buffer transform with one "ne rX, l(0), cb9[r + 16].w". The game never
 * creates a stride-288 view, so when that flag is set the shader reads garbage.
 * Returns the dword index of that ne (0 if the shader does not have it); the patch
 * turns it into "and rX, l(0), ...", i.e. always the compact path. */
static uint32_t find_path_switch(const void *code, SIZE_T len)
{
    const uint8_t *d = code;
    uint32_t nchunks, at = 0;
    BOOL t64 = FALSE;
    int nes = 0;
    if (!code || len < 32 || memcmp(d, "DXBC", 4)) return 0;
    nchunks = *(const uint32_t *)(d + 28);
    for (uint32_t c = 0; c < nchunks; c++) {
        uint32_t off = *(const uint32_t *)(d + 32 + 4 * c), size;
        const uint32_t *t;
        if (memcmp(d + off, "SHEX", 4) && memcmp(d + off, "SHDR", 4)) continue;
        size = *(const uint32_t *)(d + off + 4) / 4;
        t = (const uint32_t *)(d + off + 8);
        if ((t[0] >> 16) != 1) return 0;   /* vertex shaders only */
        for (uint32_t k = 2; k < size;) {
            uint32_t op = t[k] & 0x7ff, ln = op == 0x35 ? t[k + 1] : (t[k] >> 24) & 0x7f;
            if (!ln || k + ln > size) break;
            if (op == 0xa2 && ln >= 3 && t[k + 2] == 64) t64 = TRUE;   /* dcl_resource_structured t64 */
            if (op == 0x39 && ln == 13 && t[k + 8] == 0x0620803a && t[k + 9] == 9) {
                nes++;
                at = (off + 8) / 4 + k;
            }
            k += ln;
        }
    }
    return t64 && nes == 1 ? at : 0;
}
static volatile LONG patch_on, n_patched;
/* watch: see watch_draw */
static volatile LONG watch_on, watch_left;
static uint32_t watch_hash;
static volatile LONG srv288_logged;
/* per-PSO draw counts, so a bisection can start from the PSOs that are actually
 * drawn instead of from all 3000 the library contains. Off by default: the lookup
 * costs a hash probe on all ~700k draws a frame. FH2_COUNT=1 enables it. */
static volatile LONG64 pso_draws[PSO_IDS];
/* shape of the draws each PSO issues: a prop drawn in one huge instanced call and a
 * fullscreen quad drawn a million times look nothing alike in this table */
static volatile LONG64 pso_max_inst[PSO_IDS], pso_max_vert[PSO_IDS], pso_inst_total[PSO_IDS];
static BOOL count_on;
static volatile LONG64 n_unknown_pso;
static volatile LONG64 n_filtered;

static void note_pso_id(void *pso, LONG id, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d)
{
    UINT h = (UINT)(((uintptr_t)pso >> 4) % PSO_IDS);
    for (UINT i = 0; i < PSO_IDS; i++, h = (h + 1) % PSO_IDS)
        if (!pso_ids[h].pso || pso_ids[h].pso == pso) {
            pso_ids[h].id = id;
            /* keep shader copies so a bisected PSO can be dumped */
            pso_ids[h].vs_len = d->VS.BytecodeLength;
            pso_ids[h].ps_len = d->PS.BytecodeLength;
            pso_ids[h].vs = d->VS.BytecodeLength ? memcpy(HeapAlloc(GetProcessHeap(), 0, d->VS.BytecodeLength), d->VS.pShaderBytecode, d->VS.BytecodeLength) : NULL;
            pso_ids[h].ps = d->PS.BytecodeLength ? memcpy(HeapAlloc(GetProcessHeap(), 0, d->PS.BytecodeLength), d->PS.pShaderBytecode, d->PS.BytecodeLength) : NULL;
            {
                uint32_t x = 2166136261u;
                for (SIZE_T b = 0; b < pso_ids[h].vs_len; b++) x = (x ^ ((const uint8_t *)pso_ids[h].vs)[b]) * 16777619u;
                for (SIZE_T b = 0; b < pso_ids[h].ps_len; b++) x = (x ^ ((const uint8_t *)pso_ids[h].ps)[b]) * 16777619u;
                pso_ids[h].hash = x;
            }
            pso_ids[h].n_elem = 0;
            for (UINT e = 0; e < d->InputLayout.NumElements && e < PSO_ELEMS; e++) {
                const D3D12_INPUT_ELEMENT_DESC *el = &d->InputLayout.pInputElementDescs[e];
                pso_elem *o = &pso_ids[h].elem[e];
                strncpy(o->sem, el->SemanticName ? el->SemanticName : "?", sizeof(o->sem) - 1);
                o->sem[sizeof(o->sem) - 1] = 0;
                o->index = el->SemanticIndex;
                o->slot = el->InputSlot;
                o->offset = el->AlignedByteOffset;
                o->fmt = el->Format;
                o->cls = el->InputSlotClass;
                o->step = el->InstanceDataStepRate;
                pso_ids[h].n_elem = e + 1;
            }
            pso_ids[h].pso = pso;
            return;
        }
}

static LONG pso_id_of(void *pso)
{
    UINT h = (UINT)(((uintptr_t)pso >> 4) % PSO_IDS);
    if (!pso) return -1;
    for (UINT i = 0; i < PSO_IDS; i++, h = (h + 1) % PSO_IDS) {
        if (pso_ids[h].pso == pso) return pso_ids[h].id;
        if (!pso_ids[h].pso) return -1;
    }
    return -1;
}

static void *tint_of(void *pso)
{
    UINT h = (UINT)(((uintptr_t)pso >> 4) % PSO_IDS);
    if (!pso) return NULL;
    for (UINT i = 0; i < PSO_IDS; i++, h = (h + 1) % PSO_IDS) {
        if (pso_ids[h].pso == pso) {
            if (!pso_ids[h].tint) return NULL;
            if (n_tint_set > 0) {
                for (LONG k = 0; k < n_tint_set; k++)
                    if (tint_set[k] == pso_ids[h].hash) return pso_ids[h].tint;
                return NULL;
            }
            return ((LONG)pso_ids[h].hash & tint_mask) == tint_value ? pso_ids[h].tint : NULL;
        }
        if (!pso_ids[h].pso) return NULL;
    }
    return NULL;
}

static BOOL filtered(void *pso)
{
    LONG id;
    if (!filter_on) return FALSE;
    id = pso_id_of(pso);
    if (id < 0) { InterlockedIncrement64(&n_unknown_pso); return FALSE; }
    return (id & filter_mask) == filter_value;
}

/* counted for every draw, filter or not: the counters answer "which PSO draws the
 * thing in front of the camera", which is where a bisection has to start */
/* count a draw and record the shape: vertex count and instance count */
static void count_draw_shape(void *pso, LONG64 verts, LONG64 insts)
{
    LONG id;
    if (!count_on) return;
    id = pso_id_of(pso);
    if (id < 0 || id >= PSO_IDS) return;
    InterlockedIncrement64(&pso_draws[id]);
    InterlockedAdd64(&pso_inst_total[id], insts);
    if (insts > pso_max_inst[id]) pso_max_inst[id] = insts;
    if (verts > pso_max_vert[id]) pso_max_vert[id] = verts;
}

static void dump_pso(LONG want)
{
    for (UINT h = 0; h < PSO_IDS; h++)
        if (pso_ids[h].pso && pso_ids[h].id == want) {
            char name[64];
            FILE *f;
            sprintf(name, "data\\logs\\pso%ld_vs.dxbc", want);
            if (pso_ids[h].vs && (f = fopen(name, "wb"))) { fwrite(pso_ids[h].vs, 1, pso_ids[h].vs_len, f); fclose(f); }
            sprintf(name, "data\\logs\\pso%ld_ps.dxbc", want);
            if (pso_ids[h].ps && (f = fopen(name, "wb"))) { fwrite(pso_ids[h].ps, 1, pso_ids[h].ps_len, f); fclose(f); }
            logmsg("dumped PSO %ld (%p, hash %08x) vs %zu ps %zu bytes", want, pso_ids[h].pso, pso_ids[h].hash,
                   pso_ids[h].vs_len, pso_ids[h].ps_len);
            for (UINT e = 0; e < pso_ids[h].n_elem; e++) {
                pso_elem *o = &pso_ids[h].elem[e];
                logmsg("  pso %ld elem %u: %s%u slot %u offset %u fmt %u class %u step %u", want, e, o->sem,
                       o->index, o->slot, o->offset, o->fmt, o->cls, o->step);
            }
            return;
        }
    logmsg("dump: PSO %ld not found", want);
}


static void report_top(void)
{
    LONG64 best[24];
    LONG ids[24];
    int n = 0, i2, k2;
    for (i2 = 0; i2 < PSO_IDS; i2++) {
        LONG64 c = pso_draws[i2];
        if (!c) continue;
        for (k2 = 0; k2 < n; k2++)
            if (c > best[k2]) break;
        if (k2 >= 24) continue;
        if (n < 24) n++;
        for (int m = n - 1; m > k2; m--) { best[m] = best[m - 1]; ids[m] = ids[m - 1]; }
        best[k2] = c;
        ids[k2] = i2;
    }
    for (i2 = 0; i2 < n; i2++)
        logmsg("top pso %ld: %lld draws, max %lld verts, max %lld inst, %lld inst total",
               ids[i2], best[i2], pso_max_vert[ids[i2]], pso_max_inst[ids[i2]], pso_inst_total[ids[i2]]);
    if (!n) logmsg("top: no draws counted");
}

/* ctl "stacks": the Windows-side call stack of every other thread, as module+offset,
 * for hangs (a native sample under Rosetta shows nothing but ntdll.so). Each
 * thread is suspended only to read its registers. */
typedef HRESULT (WINAPI *get_thread_description_fn)(HANDLE, PWSTR *);

static void describe_address(DWORD64 pc, char *out, size_t n)
{
    HMODULE m = NULL;
    char path[MAX_PATH] = "?";
    const char *base;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)pc, &m) && m) {
        GetModuleFileNameA(m, path, sizeof(path));
        base = strrchr(path, '\\');
        snprintf(out, n, "%s+%#llx", base ? base + 1 : path, (unsigned long long)(pc - (DWORD64)(uintptr_t)m));
    } else snprintf(out, n, "%#llx", (unsigned long long)pc);
}

static void dump_stacks(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = {sizeof(te)};
    get_thread_description_fn describe =
        (get_thread_description_fn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    if (snap == INVALID_HANDLE_VALUE) { logmsg("stacks: no snapshot"); return; }
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        HANDLE t;
        CONTEXT c;
        char line[1024], frame[96], name[64] = "";
        size_t len;
        PWSTR wname = NULL;
        BOOL got;
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        if (!(t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID)))
            continue;
        if (describe && SUCCEEDED(describe(t, &wname)) && wname) {
            WideCharToMultiByte(CP_UTF8, 0, wname, -1, name, sizeof(name), NULL, NULL);
            LocalFree(wname);
        }
        memset(&c, 0, sizeof(c));
        c.ContextFlags = CONTEXT_FULL;
        if (SuspendThread(t) == (DWORD)-1) { CloseHandle(t); continue; }
        got = GetThreadContext(t, &c);
        /* resume before walking: the walk takes the loader lock, which the
         * suspended thread may hold; a hung thread's stack does not move anyway */
        ResumeThread(t);
        if (got) {
            len = (size_t)snprintf(line, sizeof(line), "stack tid %lu \"%s\":", te.th32ThreadID, name);
            for (int f = 0; f < 16 && c.Rip && len < sizeof(line) - 100; f++) {
                DWORD64 image = 0;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &image, NULL);
                describe_address(c.Rip, frame, sizeof(frame));
                len += (size_t)snprintf(line + len, sizeof(line) - len, " %s", frame);
                if (fn) {
                    void *handler_data;
                    DWORD64 establisher;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, fn, &c, &handler_data, &establisher, NULL);
                } else {   /* leaf: return address on top of the stack */
                    c.Rip = *(DWORD64 *)(uintptr_t)c.Rsp;
                    c.Rsp += 8;
                }
            }
        } else snprintf(line, sizeof(line), "stack tid %lu \"%s\": no context", te.th32ThreadID, name);
        CloseHandle(t);
        logmsg("%s", line);
    }
    CloseHandle(snap);
}

/* the instanced PSOs drawn since the last "reset", by shader hash: the candidate
 * list for a "tintset" bisection (needs FH2_COUNT=1 and FH2_TINT=1) */
static void report_tinted(void)
{
    int n = 0;
    for (UINT h = 0; h < PSO_IDS; h++) {
        LONG id = pso_ids[h].id;
        if (!pso_ids[h].pso || !pso_ids[h].tint || id < 0 || id >= PSO_IDS || !pso_draws[id]) continue;
        n++;
        logmsg("tinted hash %08x: pso %ld, %lld draws, max %lld verts, max %lld inst, vs %zu ps %zu bytes",
               pso_ids[h].hash, id, pso_draws[id], pso_max_vert[id], pso_max_inst[id], pso_ids[h].vs_len, pso_ids[h].ps_len);
    }
    logmsg("tintlist: %d instanced PSOs drawn", n);
}

/* control file: "mask value" (hex) enables the filter, "off" disables, "dump N" writes PSO N's shaders */
static void poll_control(void)
{
    static FILETIME last;
    WIN32_FILE_ATTRIBUTE_DATA a;
    char line[128] = "";
    FILE *f;
    if (!GetFileAttributesExA("data\\logs\\fh2port.ctl", GetFileExInfoStandard, &a)) return;
    if (!CompareFileTime(&a.ftLastWriteTime, &last)) return;
    last = a.ftLastWriteTime;
    if (!(f = fopen("data\\logs\\fh2port.ctl", "r"))) return;
    fgets(line, sizeof(line), f);
    fclose(f);
    if (!strncmp(line, "off", 3)) { filter_on = 0; logmsg("filter off"); return; }
    if (!strncmp(line, "dump", 4)) { dump_pso(atol(line + 4)); return; }
    if (!strncmp(line, "reset", 5)) {
        for (int i2 = 0; i2 < PSO_IDS; i2++) pso_draws[i2] = 0;
        logmsg("draw counters reset");
        return;
    }
    if (!strncmp(line, "top", 3)) { report_top(); return; }
    if (!strncmp(line, "tintlist", 8)) { report_tinted(); return; }
    if (!strncmp(line, "stacks", 6)) { dump_stacks(); return; }
    if (!strncmp(line, "patch", 5)) {
        patch_on = strncmp(line + 5, " off", 4) != 0;
        logmsg("patch %s (%ld patched twins)", patch_on ? "on" : "off", n_patched);
        return;
    }
    if (!strncmp(line, "watch", 5)) {
        if (!strncmp(line + 5, " off", 4)) { watch_on = 0; logmsg("watch off"); return; }
        watch_hash = (uint32_t)strtoul(line + 5, NULL, 16);
        watch_left = 24;
        srv288_logged = 0;
        watch_on = 1;
        logmsg("watch on: next 24 draws of shader hash %08x, and 16 buffer SRVs with stride 288", watch_hash);
        return;
    }
    if (!strncmp(line, "tintset", 7)) {
        char *p = line + 7, *end;
        LONG n = 0;
        tint_on = 0;
        for (unsigned long h; n < TINT_SET && (h = strtoul(p, &end, 16), end != p); p = end) tint_set[n++] = (uint32_t)h;
        n_tint_set = n;
        tint_on = n > 0;
        logmsg("tint on: %ld chosen shader hashes drawn magenta", n);
        return;
    }
    if (!strncmp(line, "tint", 4)) {
        unsigned long m, v;
        if (!strncmp(line + 4, " off", 4)) { tint_on = 0; logmsg("tint off"); return; }
        if (sscanf(line + 4, "%lx %lx", &m, &v) == 2) {
            n_tint_set = 0;
            tint_mask = (LONG)m; tint_value = (LONG)v; tint_on = 1;
            logmsg("tint on: shader hashes with (hash & %#lx) == %#lx drawn magenta (%ld twins built, %ld failed)",
                   m, v, n_tint_built, n_tint_failed);
        }
        return;
    }
    {
        unsigned long m, v;
        if (sscanf(line, "%lx %lx", &m, &v) == 2) {
            filter_mask = (LONG)m; filter_value = (LONG)v; n_filtered = 0; filter_on = 1;
            logmsg("filter on: skip PSO ids with (id & %#lx) == %#lx", m, v);
        }
    }
}

static void note_pso(void *pso, const D3D12_SHADER_BYTECODE *vs)
{
    LONG id = InterlockedIncrement(&n_psos) - 1;
    if (pso && vs) note_pso_id(pso, id, CONTAINING_RECORD(vs, D3D12_GRAPHICS_PIPELINE_STATE_DESC, VS));
    if (!pso || !vs || !vs_reads_vertex_id(vs->pShaderBytecode, vs->BytecodeLength)) return;
    UINT h = (UINT)(((uintptr_t)pso >> 4) % VID_SET);
    for (UINT i = 0; i < VID_SET; i++, h = (h + 1) % VID_SET)
        if (!vid_psos[h] || vid_psos[h] == pso) {
            if (!vid_psos[h]) InterlockedIncrement(&n_vid_psos);
            vid_psos[h] = pso;
            return;
        }
}

static void **dev_vt_orig;
typedef HRESULT (STDMETHODCALLTYPE *create_gpso_fn)(ID3D12Device *, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *, REFIID, void **);

static void build_patched(ID3D12Device *dev, void *pso, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC t;
    uint32_t *code;
    void *twin = NULL;
    HRESULT hr;
    UINT h = (UINT)(((uintptr_t)pso >> 4) % PSO_IDS);
    uint32_t at, value;
    if (!patch_build || !dev || !pso || !d) return;
    for (UINT i = 0; i < PSO_IDS && pso_ids[h].pso != pso; i++) h = (h + 1) % PSO_IDS;
    if (pso_ids[h].pso != pso) return;
    if (patch_auto) {
        if (!(at = find_path_switch(d->VS.pShaderBytecode, d->VS.BytecodeLength))) return;
        value = (((const uint32_t *)d->VS.pShaderBytecode)[at] & ~0x7ffu) | 0x01;   /* ne -> and */
    } else {
        if (pso_ids[h].hash != patch_hash || (SIZE_T)(patch_dword + 1) * 4 > d->VS.BytecodeLength) return;
        at = patch_dword;
        value = patch_value;
    }
    code = HeapAlloc(GetProcessHeap(), 0, d->VS.BytecodeLength);
    memcpy(code, d->VS.pShaderBytecode, d->VS.BytecodeLength);
    t = *d;
    t.CachedPSO.pCachedBlob = NULL;
    t.CachedPSO.CachedBlobSizeInBytes = 0;
    if (!patch_auto || n_patched < 8)
        logmsg("patch: PSO %ld hash %08x VS dword %u %08x -> %08x", pso_ids[h].id, pso_ids[h].hash, at, code[at], value);
    code[at] = value;
    t.VS.pShaderBytecode = code;
    hr = ((create_gpso_fn)dev_vt_orig[10])(dev, &t, &IID_ID3D12PipelineState, &twin);
    if (FAILED(hr) || !twin) { logmsg("patch: twin failed hr %#lx", hr); return; }
    pso_ids[h].patched = twin;
    InterlockedIncrement(&n_patched);
}

/* build the magenta twin of an instanced PSO (see tint_build) */
static void build_tint(ID3D12Device *dev, void *pso, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC t;
    D3D12_RENDER_TARGET_BLEND_DESC *rt0 = &t.BlendState.RenderTarget[0];
    BOOL instanced = FALSE;
    void *twin = NULL;
    HRESULT hr;
    if (!tint_build || !dev || !pso || !d || !d->NumRenderTargets) return;
    for (UINT e = 0; e < d->InputLayout.NumElements; e++)
        if (d->InputLayout.pInputElementDescs[e].InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA) instanced = TRUE;
    if (!instanced) return;
    t = *d;
    t.CachedPSO.pCachedBlob = NULL;
    t.CachedPSO.CachedBlobSizeInBytes = 0;
    if (!t.BlendState.IndependentBlendEnable)
        for (UINT r = 1; r < 8; r++) t.BlendState.RenderTarget[r] = t.BlendState.RenderTarget[0];
    t.BlendState.IndependentBlendEnable = TRUE;
    t.BlendState.AlphaToCoverageEnable = FALSE;
    rt0->BlendEnable = TRUE;
    rt0->LogicOpEnable = FALSE;
    rt0->SrcBlend = rt0->SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR;
    rt0->DestBlend = rt0->DestBlendAlpha = D3D12_BLEND_ZERO;
    rt0->BlendOp = rt0->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt0->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    hr = ((create_gpso_fn)dev_vt_orig[10])(dev, &t, &IID_ID3D12PipelineState, &twin);
    if (FAILED(hr) || !twin) {
        if (InterlockedIncrement(&n_tint_failed) <= 8) logmsg("tint: twin of PSO %ld failed hr %#lx (rt0 fmt %u)", pso_id_of(pso), hr, d->RTVFormats[0]);
        return;
    }
    {
        UINT h = (UINT)(((uintptr_t)pso >> 4) % PSO_IDS);
        for (UINT i = 0; i < PSO_IDS; i++, h = (h + 1) % PSO_IDS)
            if (pso_ids[h].pso == pso) { pso_ids[h].tint = twin; InterlockedIncrement(&n_tint_built); return; }
    }
    ID3D12PipelineState_Release((ID3D12PipelineState *)twin);
}

static BOOL is_vid_pso(void *pso)
{
    UINT h = (UINT)(((uintptr_t)pso >> 4) % VID_SET);
    if (!pso) return FALSE;
    for (UINT i = 0; i < VID_SET; i++, h = (h + 1) % VID_SET) {
        if (vid_psos[h] == pso) return TRUE;
        if (!vid_psos[h]) return FALSE;
    }
    return FALSE;
}

/* watch: log the bound vertex buffers, root SRVs and arguments of the next draws
 * of one shader hash (ctl "watch <hash>"), to see what a broken prop is fed */

static uint32_t hash_of(void *pso)
{
    UINT h = (UINT)(((uintptr_t)pso >> 4) % PSO_IDS);
    if (!pso) return 0;
    for (UINT i = 0; i < PSO_IDS; i++, h = (h + 1) % PSO_IDS) {
        if (pso_ids[h].pso == pso) return pso_ids[h].hash;
        if (!pso_ids[h].pso) return 0;
    }
    return 0;
}

static ID3D12Resource *find_buffer(D3D12_GPU_VIRTUAL_ADDRESS va, UINT64 *offset);

/* decode the compact instance records (24 bytes: I_POS R16x4 UNORM, I_UVSCALE
 * R8x4, I_XAXIS/I_YAXIS R10G10B10A2 UNORM as *2-1, I_COLOUR R8x4) of a watched
 * draw, when the buffer is CPU-visible */
static void watch_instances(const D3D12_VERTEX_BUFFER_VIEW *vb, UINT first, UINT count)
{
    UINT64 off = 0;
    ID3D12Resource *res = find_buffer(vb->BufferLocation, &off);
    D3D12_HEAP_PROPERTIES hp;
    D3D12_HEAP_FLAGS hf;
    D3D12_RANGE none = {0, 0};
    uint8_t *base = NULL;
    if (!res) { logmsg("watch: instance buffer %#llx unknown", (unsigned long long)vb->BufferLocation); return; }
    if (FAILED(ID3D12Resource_GetHeapProperties(res, &hp, &hf))) {
        logmsg("watch: instance buffer %#llx: no heap properties (placed/reserved?)", (unsigned long long)vb->BufferLocation);
        return;
    }
    if (hp.Type == D3D12_HEAP_TYPE_DEFAULT || (hp.Type == D3D12_HEAP_TYPE_CUSTOM && hp.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE)) {
        logmsg("watch: instance buffer %#llx in heap type %u, not CPU-visible", (unsigned long long)vb->BufferLocation, hp.Type);
        return;
    }
    if (FAILED(ID3D12Resource_Map(res, 0, &none, (void **)&base)) || !base) { logmsg("watch: map failed"); return; }
    for (UINT i = first; i < first + count && i < first + 6 && (UINT64)(i + 1) * 24 <= vb->SizeInBytes; i++) {
        const uint8_t *r = base + off + (UINT64)i * 24;
        const uint16_t *p16 = (const uint16_t *)r;
        uint32_t xa = *(const uint32_t *)(r + 12), ya = *(const uint32_t *)(r + 16);
        double x[3], y[3], dot = 0, lx = 0, ly = 0;
        for (int k = 0; k < 3; k++) {
            x[k] = ((xa >> (10 * k)) & 1023) / 1023.0 * 2 - 1;
            y[k] = ((ya >> (10 * k)) & 1023) / 1023.0 * 2 - 1;
            dot += x[k] * y[k]; lx += x[k] * x[k]; ly += y[k] * y[k];
        }
        logmsg("watch inst %u (heap %u): pos %.4f %.4f %.4f scale %.2f | uv %02x%02x%02x%02x | x %.3f %.3f %.3f |x|^2 %.3f"
               " | y %.3f %.3f %.3f |y|^2 %.3f | x.y %.3f | col %02x%02x%02x%02x",
               i, hp.Type, p16[0] / 65535.0, p16[1] / 65535.0, p16[2] / 65535.0, p16[3] / 65535.0 * 100, r[8], r[9], r[10], r[11],
               x[0], x[1], x[2], lx, y[0], y[1], y[2], ly, dot, r[20], r[21], r[22], r[23]);
    }
    ID3D12Resource_Unmap(res, 0, &none);
}

static void watch_draw(ID3D12GraphicsCommandList *l, const char *kind, UINT a, UINT b, UINT c, INT d, UINT e5)
{
    list_writes *e;
    char srv[256] = "";
    size_t n = 0;
    if (!watch_on || watch_left <= 0) return;
    EnterCriticalSection(&list_cs);
    e = list_entry(l, FALSE);
    if (e && e->watch && InterlockedDecrement(&watch_left) >= 0) {
        for (UINT r = 0; r < 16 && n < sizeof(srv) - 32; r++)
            if (e->root_srv[r]) n += (size_t)snprintf(srv + n, sizeof(srv) - n, " r%u=%#llx", r, (unsigned long long)e->root_srv[r]);
        logmsg("watch %08x %s %u %u %u %d %u | vb0 %#llx/%u/%u vb1 %#llx/%u/%u vb2 %#llx/%u/%u | root srv%s", watch_hash,
               kind, a, b, c, d, e5,
               (unsigned long long)e->vb[0].BufferLocation, e->vb[0].SizeInBytes, e->vb[0].StrideInBytes,
               (unsigned long long)e->vb[1].BufferLocation, e->vb[1].SizeInBytes, e->vb[1].StrideInBytes,
               (unsigned long long)e->vb[2].BufferLocation, e->vb[2].SizeInBytes, e->vb[2].StrideInBytes, srv[0] ? srv : " none");
        if (!strcmp(kind, "DrawIndexed") && e->vb[2].StrideInBytes == 24) {
            D3D12_VERTEX_BUFFER_VIEW vb = e->vb[2];
            LeaveCriticalSection(&list_cs);
            watch_instances(&vb, e5, b);
            return;
        }
    }
    LeaveCriticalSection(&list_cs);
}

typedef void (STDMETHODCALLTYPE *set_vb_fn)(ID3D12GraphicsCommandList *, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW *);
typedef void (STDMETHODCALLTYPE *set_root_srv_fn)(ID3D12GraphicsCommandList *, UINT, D3D12_GPU_VIRTUAL_ADDRESS);

static void STDMETHODCALLTYPE hk_set_vb(ID3D12GraphicsCommandList *l, UINT start, UINT n, const D3D12_VERTEX_BUFFER_VIEW *v)
{
    if (watch_on) {
        list_writes *e;
        EnterCriticalSection(&list_cs);
        if ((e = list_entry(l, TRUE))) {
            for (UINT i = 0; i < n && start + i < 4; i++) {
                if (v) e->vb[start + i] = v[i];
                else memset(&e->vb[start + i], 0, sizeof(e->vb[0]));
            }
        }
        LeaveCriticalSection(&list_cs);
    }
    ((set_vb_fn)list_vt_orig[44])(l, start, n, v);
}

static void STDMETHODCALLTYPE hk_set_root_srv(ID3D12GraphicsCommandList *l, UINT param, D3D12_GPU_VIRTUAL_ADDRESS va)
{
    if (watch_on && param < 16) {
        list_writes *e;
        EnterCriticalSection(&list_cs);
        if ((e = list_entry(l, TRUE))) e->root_srv[param] = va;
        LeaveCriticalSection(&list_cs);
    }
    ((set_root_srv_fn)list_vt_orig[40])(l, param, va);
}

/* draw census: how many draws carry a vertex offset that D3DMetal drops from SV_VertexID */
typedef void (STDMETHODCALLTYPE *draw_fn)(ID3D12GraphicsCommandList *, UINT, UINT, UINT, UINT);
typedef void (STDMETHODCALLTYPE *draw_indexed_fn)(ID3D12GraphicsCommandList *, UINT, UINT, UINT, INT, UINT);
typedef void (STDMETHODCALLTYPE *exec_indirect_fn)(ID3D12GraphicsCommandList *, ID3D12CommandSignature *, UINT, ID3D12Resource *,
                                                   UINT64, ID3D12Resource *, UINT64);
static volatile LONG64 n_draw, n_draw_off, n_drawidx, n_drawidx_off, n_indirect, n_vid_base, n_vid_skipped;
/* draws whose PSO has no id: the filter cannot hide those, so a bisection that
 * leaves the wall on screen may simply never have been able to reach it */
static volatile LONG64 n_unknown_pso;

static void draw_stats(void)
{
    LONG64 t = n_draw + n_drawidx + n_indirect;
    if (t && (t & 0xffff) == 0 && !quiet)
        logmsg("draws: Draw %lld (start!=0 %lld) DrawIndexed %lld (base!=0 %lld, of which vertex_id PSO %lld, skipped %lld) "
               "ExecuteIndirect %lld; PSOs %ld (vertex_id %ld); filtered %lld; unknown-pso %lld",
               n_draw, n_draw_off, n_drawidx, n_drawidx_off, n_vid_base, n_vid_skipped, n_indirect, n_psos, n_vid_psos,
               n_filtered, n_unknown_pso);
}

static void STDMETHODCALLTYPE hk_draw(ID3D12GraphicsCommandList *l, UINT vc, UINT ic, UINT sv, UINT si)
{
    InterlockedIncrement64(&n_draw);
    watch_draw(l, "Draw", vc, ic, sv, 0, si);
    if (sv) InterlockedIncrement64(&n_draw_off);
    if (count_on) {
        list_writes *e;
        void *pso;
        EnterCriticalSection(&list_cs);
        e = list_entry(l, FALSE);
        pso = e ? e->pso : NULL;
        LeaveCriticalSection(&list_cs);
        count_draw_shape(pso, vc, ic);
    }
    if (filter_on) {
        list_writes *e;
        void *pso;
        EnterCriticalSection(&list_cs);
        e = list_entry(l, FALSE);
        pso = e ? e->pso : NULL;
        LeaveCriticalSection(&list_cs);
        if (filtered(pso)) { InterlockedIncrement64(&n_filtered); return; }
    }
    draw_stats();
    ((draw_fn)list_vt_orig[12])(l, vc, ic, sv, si);
}

static void STDMETHODCALLTYPE hk_draw_indexed(ID3D12GraphicsCommandList *l, UINT ic, UINT inst, UINT si, INT bv, UINT sinst)
{
    InterlockedIncrement64(&n_drawidx);
    watch_draw(l, "DrawIndexed", ic, inst, si, bv, sinst);
    if (count_on || filter_on) {
        list_writes *e;
        void *pso;
        EnterCriticalSection(&list_cs);
        e = list_entry(l, FALSE);
        pso = e ? e->pso : NULL;
        LeaveCriticalSection(&list_cs);
        count_draw_shape(pso, ic, inst);
        if (filtered(pso)) { InterlockedIncrement64(&n_filtered); return; }
    }
    if (bv) {
        list_writes *e;
        void *pso;
        InterlockedIncrement64(&n_drawidx_off);
        EnterCriticalSection(&list_cs);
        e = list_entry(l, FALSE);
        pso = e ? e->pso : NULL;
        LeaveCriticalSection(&list_cs);
        if (is_vid_pso(pso)) {
            InterlockedIncrement64(&n_vid_base);
            if (skip_vid) { InterlockedIncrement64(&n_vid_skipped); draw_stats(); return; }
        }
    }
    draw_stats();
    ((draw_indexed_fn)list_vt_orig[13])(l, ic, inst, si, bv, sinst);
}

static void STDMETHODCALLTYPE hk_exec_indirect(ID3D12GraphicsCommandList *l, ID3D12CommandSignature *sig, UINT max, ID3D12Resource *args,
                                               UINT64 off, ID3D12Resource *cnt, UINT64 coff)
{
    InterlockedIncrement64(&n_indirect);
    watch_draw(l, "ExecuteIndirect", max, 0, (UINT)off, 0, 0);
    if (count_on || filter_on) {
        list_writes *e;
        void *pso;
        EnterCriticalSection(&list_cs);
        e = list_entry(l, FALSE);
        pso = e ? e->pso : NULL;
        LeaveCriticalSection(&list_cs);
        count_draw_shape(pso, 0, max);   /* per-draw shape unknown: only the batch size */
        if (filtered(pso)) { InterlockedIncrement64(&n_filtered); return; }
    }
    draw_stats();
    if (skip_ei) return;
    ((exec_indirect_fn)list_vt_orig[59])(l, sig, max, args, off, cnt, coff);
}

typedef void (STDMETHODCALLTYPE *set_pso_fn)(ID3D12GraphicsCommandList *, ID3D12PipelineState *);
static void STDMETHODCALLTYPE hk_set_pso(ID3D12GraphicsCommandList *l, ID3D12PipelineState *ps)
{
    list_writes *e;
    EnterCriticalSection(&list_cs);
    if ((e = list_entry(l, TRUE))) { e->pso = ps; e->watch = watch_on && hash_of(ps) == watch_hash; }
    LeaveCriticalSection(&list_cs);
    if (patch_on) {
        UINT h = (UINT)(((uintptr_t)ps >> 4) % PSO_IDS);
        for (UINT i = 0; i < PSO_IDS && pso_ids[h].pso; i++, h = (h + 1) % PSO_IDS)
            if (pso_ids[h].pso == ps) { if (pso_ids[h].patched) ps = pso_ids[h].patched; break; }
    }
    if (tint_on) {
        void *twin = tint_of(ps);
        if (twin) {
            static const FLOAT magenta[4] = {1, 0, 1, 1};
            ID3D12GraphicsCommandList_OMSetBlendFactor(l, magenta);
            ps = twin;
        }
    }
    ((set_pso_fn)list_vt_orig[25])(l, ps);
}

static void hook_list(void *list)
{
    void ***obj = list;
    if (*obj == list_vt) return;
    if (!list_vt_orig) {
        list_vt_orig = *obj;
        memcpy(list_vt, list_vt_orig, sizeof(list_vt));
        list_vt[2] = (void *)hk_list_release;
        list_vt[10] = (void *)hk_list_reset;
        list_vt[12] = (void *)hk_draw;
        list_vt[13] = (void *)hk_draw_indexed;
        list_vt[59] = (void *)hk_exec_indirect;
        list_vt[25] = (void *)hk_set_pso;
        list_vt[40] = (void *)hk_set_root_srv;
        list_vt[44] = (void *)hk_set_vb;
        list_vt[66] = (void *)hk_wbi;
    } else if (*obj != list_vt_orig) {
        logmsg("command list %p has an unexpected vtable %p, not hooked", list, *obj);
        return;
    }
    *obj = list_vt;
}

/* ---------------- device hooks ---------------- */

typedef HRESULT (STDMETHODCALLTYPE *qi_fn)(void *, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *create_queue_fn)(ID3D12Device *, const D3D12_COMMAND_QUEUE_DESC *, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *create_heap_fn)(ID3D12Device *, const D3D12_HEAP_DESC *, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *create_placed_fn)(ID3D12Device *, ID3D12Heap *, UINT64, const D3D12_RESOURCE_DESC *,
                                                      D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *create_fence_fn)(ID3D12Device *, UINT64, D3D12_FENCE_FLAGS, REFIID, void **);

static void *dev_vt[DEV_SLOTS];

typedef HRESULT (STDMETHODCALLTYPE *create_list_fn)(ID3D12Device *, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *,
                                                    ID3D12PipelineState *, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *create_list1_fn)(ID3D12Device *, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS,
                                                     REFIID, void **);

static HRESULT STDMETHODCALLTYPE hk_create_list(ID3D12Device *dev, UINT mask, D3D12_COMMAND_LIST_TYPE type,
                                                ID3D12CommandAllocator *a, ID3D12PipelineState *ps, REFIID riid, void **ppv)
{
    HRESULT hr = ((create_list_fn)dev_vt_orig[12])(dev, mask, type, a, ps, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) hook_list(*ppv);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_create_list1(ID3D12Device *dev, UINT mask, D3D12_COMMAND_LIST_TYPE type,
                                                 D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void **ppv)
{
    HRESULT hr = ((create_list1_fn)dev_vt_orig[51])(dev, mask, type, flags, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) hook_list(*ppv);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *create_committed_fn)(ID3D12Device *, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS,
                                                         const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES,
                                                         const D3D12_CLEAR_VALUE *, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *create_reserved_fn)(ID3D12Device *, const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES,
                                                        const D3D12_CLEAR_VALUE *, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *create_committed1_fn)(ID3D12Device *, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS,
                                                          const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES,
                                                          const D3D12_CLEAR_VALUE *, ID3D12ProtectedResourceSession *,
                                                          REFIID, void **);

/* Apple GPUs take at most 4 samples per pixel; the car showroom asks for 8 and
 * the wrapper stops when the host has no 8x. With FH2_MSAA8 on (the default)
 * 8x is reported as supported and every 8x resource and pipeline is made 4x
 * instead, so the showroom renders with less anti-aliasing. msaa_host is the
 * most samples the host takes (0: not asked yet, 16: no cap needed). */
static BOOL msaa_cap = TRUE;
static volatile LONG msaa_host;
static HRESULT (STDMETHODCALLTYPE *real_check_feature)(ID3D12Device *, D3D12_FEATURE, void *, UINT);

static LONG host_samples(ID3D12Device *dev)
{
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q = {DXGI_FORMAT_R16G16B16A16_FLOAT, 8, 0, 0};
    LONG host = 16;
    if (msaa_host || !dev || !real_check_feature) return msaa_host ? msaa_host : 16;
    if (SUCCEEDED(real_check_feature(dev, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q))) && !q.NumQualityLevels) {
        q.SampleCount = 4;
        if (SUCCEEDED(real_check_feature(dev, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q))) && q.NumQualityLevels)
            host = 4;
    }
    if (!InterlockedCompareExchange(&msaa_host, host, 0) && host == 4)
        logmsg("MSAA: the host takes no 8 samples, 8x targets and pipelines are made 4x (FH2_MSAA8=0 turns this off)");
    return msaa_host;
}

static const D3D12_RESOURCE_DESC *cap_resource(ID3D12Device *dev, const D3D12_RESOURCE_DESC *d, D3D12_RESOURCE_DESC *tmp)
{
    if (!msaa_cap || !d || d->SampleDesc.Count <= 4 || d->SampleDesc.Count <= (UINT)host_samples(dev)) return d;
    *tmp = *d;
    tmp->SampleDesc.Count = msaa_host;
    tmp->SampleDesc.Quality = 0;
    return tmp;
}

/* MSAA diagnostics: every multisampled resource and every failed creation, so the
 * log shows what the game asks for right before Azimuth gives up on it */
static void log_resource(const char *what, const D3D12_RESOURCE_DESC *desc, HRESULT hr)
{
    static volatile LONG nms, nfail;
    if (!desc) return;
    if (FAILED(hr) ? InterlockedIncrement(&nfail) > 64 : desc->SampleDesc.Count <= 1 || InterlockedIncrement(&nms) > 64) return;
    logmsg("%s dim %u %llux%u x%u mips %u fmt %u samples %u/%u flags %#x layout %u -> %#lx", what,
           desc->Dimension, desc->Width, desc->Height, desc->DepthOrArraySize, desc->MipLevels, desc->Format,
           desc->SampleDesc.Count, desc->SampleDesc.Quality, desc->Flags, desc->Layout, hr);
}

static HRESULT STDMETHODCALLTYPE hk_create_committed(ID3D12Device *dev, const D3D12_HEAP_PROPERTIES *hp, D3D12_HEAP_FLAGS f,
                                                     const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES st,
                                                     const D3D12_CLEAR_VALUE *cv, REFIID riid, void **ppv)
{
    D3D12_RESOURCE_DESC capped;
    LONG64 t0 = slow_start();
    HRESULT hr;
    desc = cap_resource(dev, desc, &capped);
    hr = ((create_committed_fn)dev_vt_orig[27])(dev, hp, f, desc, st, cv, riid, ppv);
    slow_end("CreateCommittedResource", t0);
    log_resource("CreateCommittedResource", desc, hr);
    if (SUCCEEDED(hr) && ppv) record_buffer(*ppv, desc);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_create_reserved(ID3D12Device *dev, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES st,
                                                    const D3D12_CLEAR_VALUE *cv, REFIID riid, void **ppv)
{
    HRESULT hr = ((create_reserved_fn)dev_vt_orig[30])(dev, desc, st, cv, riid, ppv);
    logmsg("CreateReservedResource dim %u %llux%u x%u mips %u fmt %u flags %#x layout %u -> %#lx",
           desc->Dimension, desc->Width, desc->Height, desc->DepthOrArraySize, desc->MipLevels, desc->Format,
           desc->Flags, desc->Layout, hr);
    if (SUCCEEDED(hr) && ppv) record_buffer(*ppv, desc);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_create_committed1(ID3D12Device *dev, const D3D12_HEAP_PROPERTIES *hp, D3D12_HEAP_FLAGS f,
                                                      const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES st,
                                                      const D3D12_CLEAR_VALUE *cv, ID3D12ProtectedResourceSession *ps,
                                                      REFIID riid, void **ppv)
{
    D3D12_RESOURCE_DESC capped;
    LONG64 t0 = slow_start();
    HRESULT hr;
    desc = cap_resource(dev, desc, &capped);
    hr = ((create_committed1_fn)dev_vt_orig[53])(dev, hp, f, desc, st, cv, ps, riid, ppv);
    slow_end("CreateCommittedResource1", t0);
    log_resource("CreateCommittedResource1", desc, hr);
    if (SUCCEEDED(hr) && ppv) record_buffer(*ppv, desc);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *create_signature_fn)(ID3D12Device *, const D3D12_COMMAND_SIGNATURE_DESC *,
                                                         ID3D12RootSignature *, REFIID, void **);
static LONG nsignatures;

static HRESULT STDMETHODCALLTYPE hk_create_signature(ID3D12Device *dev, const D3D12_COMMAND_SIGNATURE_DESC *d,
                                                     ID3D12RootSignature *rs, REFIID riid, void **ppv)
{
    HRESULT hr = ((create_signature_fn)dev_vt_orig[41])(dev, d, rs, riid, ppv);
    char types[128] = "";
    for (UINT i = 0; i < d->NumArgumentDescs && strlen(types) < sizeof(types) - 8; i++)
        sprintf(types + strlen(types), "%s%u", i ? "," : "", d->pArgumentDescs[i].Type);
    if (InterlockedIncrement(&nsignatures) <= 64)
        logmsg("CreateCommandSignature stride %u args [%s] rootsig %p -> %p hr %#lx", d->ByteStride, types, rs,
               ppv ? *ppv : NULL, hr);
    return hr;
}

/* format census: which DXGI formats feed vertex input and typed buffer views */
static volatile LONG seen_vtx[256], seen_buf[256], seen_stride[1024];
/* layout census: full input-layout signatures. the wrapper gets most PSOs from its
 * pipeline library, so the CreateGraphicsPSO hook alone sees almost nothing. */
#define LAYOUT_SIGS 512
static char layout_sigs[LAYOUT_SIGS][1024];
static volatile LONG n_layout_sigs;

/* bytes per element of the vertex formats D3D12 accepts; 0 = unknown */
static UINT vertex_format_size(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT: return 16;
    case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32_UINT: case DXGI_FORMAT_R32G32B32_SINT: return 12;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT: case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT: return 8;
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT: case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM: case DXGI_FORMAT_R8G8B8A8_SINT: case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT: case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM: return 4;
    case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT: return 2;
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SNORM: case DXGI_FORMAT_R8_SINT: return 1;
    default: return 0;
    }
}

/* D3DMetal resolves D3D12_APPEND_ALIGNED_ELEMENT wrongly in multi-slot layouts:
 * in the prop layout (POSITION on slot 0, five elements on slot 1, five per-instance
 * elements on slot 2) I_YAXIS is fetched 28 bytes and I_COLOUR 56 bytes past their
 * real offsets (layout-test), which skews and stretches every such prop. Writing
 * the offsets out per slot, as D3D defines them, sidesteps it. Returns FALSE (and
 * leaves out untouched) when the layout has nothing to resolve or a format whose
 * size is unknown. */
static BOOL resolve_append_offsets(const D3D12_INPUT_LAYOUT_DESC *in, D3D12_INPUT_ELEMENT_DESC *out, UINT max)
{
    UINT next[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {0};
    BOOL any = FALSE;
    if (!in || !in->pInputElementDescs || in->NumElements > max) return FALSE;
    for (UINT e = 0; e < in->NumElements; e++) {
        const D3D12_INPUT_ELEMENT_DESC *el = &in->pInputElementDescs[e];
        UINT size = vertex_format_size(el->Format), off;
        if (el->InputSlot >= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT || !size) return FALSE;
        out[e] = *el;
        if (el->AlignedByteOffset == D3D12_APPEND_ALIGNED_ELEMENT) {
            UINT align = size < 4 ? size : 4;
            off = (next[el->InputSlot] + align - 1) / align * align;
            out[e].AlignedByteOffset = off;
            any = TRUE;
        } else off = el->AlignedByteOffset;
        next[el->InputSlot] = off + size;
    }
    return any;
}

static BOOL fix_append = TRUE;   /* FH2_APPEND=0 keeps the game's layouts as they are */
static volatile LONG n_layout_fixed;

/* d with its input layout offsets written out (see resolve_append_offsets), in tmp
 * and els; d itself when there is nothing to fix */
static const D3D12_GRAPHICS_PIPELINE_STATE_DESC *fix_layout(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d,
                                                             D3D12_GRAPHICS_PIPELINE_STATE_DESC *tmp,
                                                             D3D12_INPUT_ELEMENT_DESC *els, UINT max)
{
    if (!fix_append || !d || !resolve_append_offsets(&d->InputLayout, els, max)) return d;
    *tmp = *d;
    tmp->InputLayout.pInputElementDescs = els;
    InterlockedIncrement(&n_layout_fixed);
    return tmp;
}

static void census_layout(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d)
{
    char line[1024];
    size_t n = 0;
    LONG i;
    if (!d) return;
    line[0] = 0;
    for (UINT e = 0; e < d->InputLayout.NumElements && n < sizeof(line) - 48; e++) {
        const D3D12_INPUT_ELEMENT_DESC *el = &d->InputLayout.pInputElementDescs[e];
        if (el->Format < 256 && !InterlockedExchange(&seen_vtx[el->Format], 1))
            logmsg("vertex format %u first seen: %s%u slot %u offset %u class %u step %u", el->Format,
                   el->SemanticName, el->SemanticIndex, el->InputSlot, el->AlignedByteOffset,
                   el->InputSlotClass, el->InstanceDataStepRate);
        n += (size_t)snprintf(line + n, sizeof(line) - n, "%s%s%u:s%u:o%u:f%u:c%u:r%u", e ? "|" : "",
                              el->SemanticName ? el->SemanticName : "?", el->SemanticIndex, el->InputSlot,
                              el->AlignedByteOffset, el->Format, el->InputSlotClass, el->InstanceDataStepRate);
    }
    for (i = 0; i < n_layout_sigs; i++)
        if (!strcmp(layout_sigs[i], line)) return;
    if (n_layout_sigs < LAYOUT_SIGS) {
        LONG idx = InterlockedIncrement(&n_layout_sigs) - 1;
        if (idx < LAYOUT_SIGS) {
            strcpy(layout_sigs[idx], line);
            logmsg("layout #%ld: n=%u %s", idx, d->InputLayout.NumElements, line[0] ? line : "(none)");
        }
    }
}
/* the pipeline side of the 8x -> 4x cap: its sample count must match the targets' */
static const D3D12_GRAPHICS_PIPELINE_STATE_DESC *cap_pipeline(ID3D12Device *dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d,
                                                             D3D12_GRAPHICS_PIPELINE_STATE_DESC *tmp)
{
    if (!msaa_cap || !d || d->SampleDesc.Count <= 4 || d->SampleDesc.Count <= (UINT)host_samples(dev)) return d;
    if (d != tmp) *tmp = *d;
    tmp->SampleDesc.Count = msaa_host;
    tmp->SampleDesc.Quality = 0;
    return tmp;
}

typedef void (STDMETHODCALLTYPE *create_srv_fn)(ID3D12Device *, ID3D12Resource *, const D3D12_SHADER_RESOURCE_VIEW_DESC *,
                                                D3D12_CPU_DESCRIPTOR_HANDLE);

static HRESULT STDMETHODCALLTYPE hk_create_gpso(ID3D12Device *dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d0, REFIID riid, void **ppv)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC tmp;
    D3D12_INPUT_ELEMENT_DESC els[D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT];
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d = fix_layout(d0, &tmp, els, D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT);
    d = cap_pipeline(dev, d, &tmp);
    census_layout(d0);
    {
        LONG64 t0 = slow_start();
        HRESULT hr = ((create_gpso_fn)dev_vt_orig[10])(dev, d, riid, ppv);
        slow_end("CreateGraphicsPipelineState", t0);
        if (SUCCEEDED(hr) && ppv && d) { note_pso(*ppv, &d->VS); build_tint(dev, *ppv, d); build_patched(dev, *ppv, d); }
        return hr;
    }
}

/* pipeline library: the wrapper loads its graphics PSOs from here */
typedef HRESULT (STDMETHODCALLTYPE *create_lib_fn)(ID3D12Device *, const void *, SIZE_T, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *load_gpso_fn)(ID3D12PipelineLibrary *, LPCWSTR, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *,
                                                  REFIID, void **);
static void **lib_vt_orig;
static void *lib_vt[14];
static int lib_blob_saved;

static HRESULT STDMETHODCALLTYPE hk_lib_load_gpso(ID3D12PipelineLibrary *lib, LPCWSTR name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d0,
                                                  REFIID riid, void **ppv)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC tmp;
    D3D12_INPUT_ELEMENT_DESC els[D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT];
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d = fix_layout(d0, &tmp, els, D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT);
    d = cap_pipeline(the_device, d, &tmp);
    HRESULT hr;
    LONG64 t0 = slow_start();
    /* the library holds the PSO compiled from the original layout and sample
     * count, so a fixed one is compiled afresh instead of loaded */
    if (d != d0 && the_device) {
        tmp.CachedPSO.pCachedBlob = NULL;
        tmp.CachedPSO.CachedBlobSizeInBytes = 0;
        hr = ((create_gpso_fn)dev_vt_orig[10])(the_device, d, riid, ppv);
    } else hr = ((load_gpso_fn)lib_vt_orig[9])(lib, name, d0, riid, ppv);
    slow_end(d != d0 ? "LoadGraphicsPipeline (recompiled)" : "LoadGraphicsPipeline", t0);
    if (SUCCEEDED(hr) && ppv && d) {
        census_layout(d0);
        note_pso(*ppv, &d->VS);
        build_tint(the_device, *ppv, d);
        build_patched(the_device, *ppv, d);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_create_lib(ID3D12Device *dev, const void *blob, SIZE_T len, REFIID riid, void **ppv)
{
    HRESULT hr = ((create_lib_fn)dev_vt_orig[44])(dev, blob, len, riid, ppv);
    FILE *f = NULL;
    if (SUCCEEDED(hr) && ppv && *ppv) {
        void ***obj = *ppv;
        if (!lib_vt_orig) {
            lib_vt_orig = *obj;
            memcpy(lib_vt, lib_vt_orig, sizeof(lib_vt));
            lib_vt[9] = (void *)hk_lib_load_gpso;
        }
        if (*obj == lib_vt_orig) *obj = lib_vt;
        /* keep the library blob: it holds every serialized PSO description, so the
         * input layouts can be parsed offline instead of only as the game loads them */
        if (!lib_blob_saved && blob && len && (f = fopen("data\\logs\\psolib.bin", "wb"))) {
            fwrite(blob, 1, len, f);
            fclose(f);
            lib_blob_saved = 1;
            logmsg("saved pipeline library blob (%zu bytes) to data\\logs\\psolib.bin", len);
        }
        logmsg("hooked pipeline library %p (%zu bytes)", *ppv, len);
    }
    return hr;
}

static void STDMETHODCALLTYPE hk_create_srv(ID3D12Device *dev, ID3D12Resource *res, const D3D12_SHADER_RESOURCE_VIEW_DESC *d,
                                            D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    if (d && d->ViewDimension == D3D12_SRV_DIMENSION_BUFFER && d->Format < 256
        && !InterlockedExchange(&seen_buf[d->Format], 1))
        logmsg("buffer SRV format %u first seen (stride %u, flags %#x)", d->Format, d->Buffer.StructureByteStride, d->Buffer.Flags);
    /* structured strides: a view whose stride differs from the shader's declared
     * one only addresses correctly if the stride comes from the shader */
    if (d && d->ViewDimension == D3D12_SRV_DIMENSION_BUFFER && d->Buffer.StructureByteStride &&
        d->Buffer.StructureByteStride < 1024 && !InterlockedExchange(&seen_stride[d->Buffer.StructureByteStride], 1))
        logmsg("structured SRV stride %u first seen (first %llu, num %u)", d->Buffer.StructureByteStride,
               (unsigned long long)d->Buffer.FirstElement, d->Buffer.NumElements);
    if (watch_on && d && d->ViewDimension == D3D12_SRV_DIMENSION_BUFFER && d->Buffer.StructureByteStride == 288 &&
        InterlockedIncrement(&srv288_logged) <= 16)
        logmsg("watch: SRV stride 288 first %llu num %u res %p va %#llx", (unsigned long long)d->Buffer.FirstElement,
               d->Buffer.NumElements, res, res ? (unsigned long long)ID3D12Resource_GetGPUVirtualAddress(res) : 0ull);
    ((create_srv_fn)dev_vt_orig[18])(dev, res, d, h);
}

typedef HRESULT (STDMETHODCALLTYPE *check_feature_fn)(ID3D12Device *, D3D12_FEATURE, void *, UINT);
static BOOL hide_tiled;

/* D3DMetal's tiled resources do not alias (see self-test), so optionally report
 * no tiled support and let the wrapper pick another path */
/* The wrapper refuses to start unless the device reports existing heaps and
 * resource binding tier 3. Existing heaps are emulated here (see the top of this
 * file), and binding tier 3 is what D3DMetal's argument buffers provide in
 * practice, so both are reported as supported. */
static HRESULT STDMETHODCALLTYPE hk_check_feature(ID3D12Device *dev, D3D12_FEATURE feature, void *data, UINT size)
{
    HRESULT hr = ((check_feature_fn)dev_vt_orig[13])(dev, feature, data, size);
    if (feature == D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS && data && size >= sizeof(D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS)) {
        static volatile LONG n;
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS *m = data;
        if (InterlockedIncrement(&n) <= 64)
            logmsg("CheckFeatureSupport(MULTISAMPLE_QUALITY_LEVELS): fmt %u samples %u flags %#x -> hr %#lx levels %u",
                   m->Format, m->SampleCount, m->Flags, hr, SUCCEEDED(hr) ? m->NumQualityLevels : 0);
        if (msaa_cap && SUCCEEDED(hr) && !m->NumQualityLevels && m->SampleCount > 4 && host_samples(dev) == 4) {
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS four = *m;
            four.SampleCount = 4;
            if (SUCCEEDED(real_check_feature(dev, feature, &four, sizeof(four))) && four.NumQualityLevels)
                m->NumQualityLevels = 1;
        }
    }
    if (feature == D3D12_FEATURE_EXISTING_HEAPS && data && size >= sizeof(D3D12_FEATURE_DATA_EXISTING_HEAPS)) {
        D3D12_FEATURE_DATA_EXISTING_HEAPS *e = data;
        if (FAILED(hr) || !e->Supported)
            logmsg("CheckFeatureSupport(EXISTING_HEAPS): hr %#lx supported %d -> emulated", hr, SUCCEEDED(hr) ? e->Supported : 0);
        e->Supported = TRUE;
        return S_OK;
    }
    if (SUCCEEDED(hr) && feature == D3D12_FEATURE_D3D12_OPTIONS && size >= sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS)) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS *o = data;
        if (o->ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3) {
            logmsg("CheckFeatureSupport(OPTIONS): ResourceBindingTier %u -> 3", o->ResourceBindingTier);
            o->ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_3;
        }
        if (hide_tiled) {
            logmsg("CheckFeatureSupport(OPTIONS): TiledResourcesTier %u -> 0", o->TiledResourcesTier);
            o->TiledResourcesTier = D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED;
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_create_queue(ID3D12Device *dev, const D3D12_COMMAND_QUEUE_DESC *desc,
                                                 REFIID riid, void **ppv)
{
    HRESULT hr = ((create_queue_fn)dev_vt_orig[8])(dev, desc, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) hook_queue(*ppv, desc ? desc->Type : -1);
    return hr;
}

static void self_test(ID3D12Device *dev);

static void op_test(ID3D12Device *dev);
static HRESULT STDMETHODCALLTYPE hk_open_existing_heap(ID3D12Device *dev, const void *address, REFIID riid, void **ppv)
{
    MEMORY_BASIC_INFORMATION mbi;
    const uint8_t *base = address, *p = address;
    SIZE_T size = 0;
    D3D12_HEAP_DESC hd;
    region *g;
    HRESULT hr;
    LONG idx;

    while (VirtualQuery(p, &mbi, sizeof(mbi)) && mbi.AllocationBase == base && mbi.State == MEM_COMMIT) {
        size += mbi.RegionSize;
        p += mbi.RegionSize;
    }
    if (!size || nheaps >= MAX_REGIONS) {
        logmsg("OpenExistingHeapFromAddress %p: unusable (size %#zx, heaps %ld)", address, size, nheaps);
        return E_INVALIDARG;
    }
    size = (size + 0xffff) & ~(SIZE_T)0xffff;

    memset(&hd, 0, sizeof(hd));
    hd.SizeInBytes = size;
    hd.Properties.Type = D3D12_HEAP_TYPE_CUSTOM;
    hd.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    hd.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    hd.Alignment = 0x10000;
    hd.Flags = D3D12_HEAP_FLAG_NONE;

    idx = nheaps;
    g = &heaps[idx];
    memset(g, 0, sizeof(*g));
    hr = ((create_heap_fn)dev_vt_orig[28])(dev, &hd, &IID_ID3D12Heap, (void **)&g->heap);
    if (FAILED(hr)) { logmsg("CreateHeap %#zx failed %#lx", size, hr); return hr; }

    hr = ID3D12Heap_QueryInterface(g->heap, riid, ppv);
    if (FAILED(hr)) { logmsg("heap QI failed %#lx", hr); ID3D12Heap_Release(g->heap); return hr; }

    /* D3DMetal gives each placed resource its own storage, so the heap itself is
     * only bookkeeping: buffers placed in it become regions in hk_create_placed */
    g->title = (uint8_t *)address;
    g->size = (SIZE_T)(p - base);
    nheaps = idx + 1;
    if (idx == 0 && list_vt_orig && queue_vt_orig) {
        self_test(dev);
        op_test(dev);
    }
    logmsg("heap %ld: title %p size %#zx (heap %#zx) %p", idx, address, g->size, size, g->heap);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE hk_create_placed(ID3D12Device *dev, ID3D12Heap *heap, UINT64 offset,
                                                  const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state,
                                                  const D3D12_CLEAR_VALUE *clear, REFIID riid, void **ppv)
{
    ID3D12Resource *res = NULL;
    region *h = NULL, *g;
    uint8_t *ptr = NULL;
    D3D12_RESOURCE_DESC capped;
    LONG64 t0 = slow_start();
    HRESULT hr;
    desc = cap_resource(dev, desc, &capped);
    hr = ((create_placed_fn)dev_vt_orig[29])(dev, heap, offset, desc, state, clear, riid, ppv);
    slow_end("CreatePlacedResource", t0);
    log_resource("CreatePlacedResource", desc, hr);

    if (FAILED(hr) || !ppv || !*ppv) return hr;
    record_buffer(*ppv, desc);
    for (LONG i = 0; i < nheaps; i++)
        if (heaps[i].heap == heap) { h = &heaps[i]; break; }
    if (!h) return hr;
    if (desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        stat_textures++;
        logmsg("texture placed in title heap %p at %#llx (%llux%u fmt %u): not synced",
               heap, offset, desc->Width, desc->Height, desc->Format);
        return hr;
    }
    if (FAILED(IUnknown_QueryInterface((IUnknown *)*ppv, &IID_ID3D12Resource, (void **)&res))) return hr;
    if (FAILED(ID3D12Resource_Map(res, 0, NULL, (void **)&ptr)) || !ptr || nregions >= MAX_REGIONS
        || offset + desc->Width > h->size) {
        logmsg("buffer at %#llx size %#llx in heap %p: cannot shadow", offset, desc->Width, heap);
        ID3D12Resource_Release(res);
        return hr;
    }
    /* res keeps its reference: the region must outlive the wrapper's release */
    g = &regions[nregions];
    g->title = h->title + offset;
    g->shadow = ptr;
    g->size = desc->Width;
    g->buffer = res;
    g->gva = ID3D12Resource_GetGPUVirtualAddress(res);
    g->last = VirtualAlloc(NULL, g->size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    AcquireSRWLockExclusive(&sync_lock);
    memcpy(g->last, g->title, g->size);
    memcpy(g->shadow, g->last, g->size);
    InterlockedIncrement(&nregions);
    ReleaseSRWLockExclusive(&sync_lock);
    logmsg("region %ld: title %p size %#llx shadow %p gva %#llx", nregions - 1, g->title, desc->Width, ptr, g->gva);
    return hr;
}

/* One-shot probe of how D3DMetal treats a mapped CUSTOM/WRITE_BACK buffer:
 * does it see GPU writes (WriteBufferImmediate, CopyBufferRegion) and do CPU
 * writes reach the GPU? Uses its own resources only. */
static void self_test(ID3D12Device *dev)
{
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    D3D12_HEAP_PROPERTIES custom = {D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0, 0, 0};
    D3D12_HEAP_PROPERTIES upload = {D3D12_HEAP_TYPE_UPLOAD, 0, 0, 0, 0}, readback = {D3D12_HEAP_TYPE_READBACK, 0, 0, 0, 0};
    D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 0x10000, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    ID3D12CommandQueue *q = NULL;
    ID3D12CommandAllocator *a = NULL;
    ID3D12GraphicsCommandList *l = NULL;
    ID3D12GraphicsCommandList2 *l2 = NULL;
    ID3D12Resource *tb = NULL, *ub = NULL, *rb = NULL, *ra = NULL, *rb2 = NULL;
    ID3D12Heap *th = NULL;
    D3D12_HEAP_DESC thd = {0x10000, {D3D12_HEAP_TYPE_DEFAULT, 0, 0, 0, 0}, 0x10000, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS};
    D3D12_TILED_RESOURCE_COORDINATE tc = {0, 0, 0, 0};
    D3D12_TILE_REGION_SIZE ts = {1, FALSE, 0, 0, 0};
    UINT tstart = 0, tcount = 1;
    BOOL alias_ok = FALSE, placed_ok = FALSE;
    ID3D12Heap *ph = NULL;
    ID3D12Resource *pa = NULL, *pb = NULL;
    ID3D12Fence *f = NULL;
    volatile uint32_t *t = NULL, *u = NULL, *r = NULL;
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER wp;
    HRESULT hr;

    hr = ((create_queue_fn)dev_vt_orig[8])(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&a);
    if (SUCCEEDED(hr)) hr = ((create_list_fn)dev_vt_orig[12])(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, NULL,
                                                             &IID_ID3D12GraphicsCommandList, (void **)&l);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommittedResource(dev, &custom, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
                                                                NULL, &IID_ID3D12Resource, (void **)&tb);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommittedResource(dev, &upload, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                                NULL, &IID_ID3D12Resource, (void **)&ub);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommittedResource(dev, &readback, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                                NULL, &IID_ID3D12Resource, (void **)&rb);
    if (SUCCEEDED(hr)) hr = ((create_fence_fn)dev_vt_orig[36])(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&f);
    /* two reserved buffers mapped onto the same heap tile must alias */
    if (SUCCEEDED(hr) && SUCCEEDED(((create_heap_fn)dev_vt_orig[28])(dev, &thd, &IID_ID3D12Heap, (void **)&th))
        && SUCCEEDED(((create_reserved_fn)dev_vt_orig[30])(dev, &rd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&ra))
        && SUCCEEDED(((create_reserved_fn)dev_vt_orig[30])(dev, &rd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&rb2))) {
        ((update_tiles_fn)queue_vt_orig[8])(q, ra, 1, &tc, &ts, th, 1, NULL, &tstart, &tcount, D3D12_TILE_MAPPING_FLAG_NONE);
        ((update_tiles_fn)queue_vt_orig[8])(q, rb2, 1, &tc, &ts, th, 1, NULL, &tstart, &tcount, D3D12_TILE_MAPPING_FLAG_NONE);
        alias_ok = TRUE;
    } else logmsg("self-test: tiled setup failed");
    /* two placed buffers at the same heap offset must alias too */
    if (SUCCEEDED(hr) && SUCCEEDED(((create_heap_fn)dev_vt_orig[28])(dev, &thd, &IID_ID3D12Heap, (void **)&ph))
        && SUCCEEDED(((create_placed_fn)dev_vt_orig[29])(dev, ph, 0, &rd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&pa))
        && SUCCEEDED(((create_placed_fn)dev_vt_orig[29])(dev, ph, 0, &rd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&pb)))
        placed_ok = TRUE;
    else logmsg("self-test: placed alias setup failed");
    if (SUCCEEDED(hr)) hr = ID3D12Resource_Map(tb, 0, NULL, (void **)&t);
    if (SUCCEEDED(hr)) hr = ID3D12Resource_Map(ub, 0, NULL, (void **)&u);
    if (SUCCEEDED(hr)) hr = ID3D12Resource_Map(rb, 0, NULL, (void **)&r);
    if (FAILED(hr)) { logmsg("self-test setup failed %#lx", hr); goto done; }

    t[0] = t[1] = t[2] = 0;
    t[3] = 0x9abc0003;            /* CPU -> GPU probe */
    u[1] = 0x56780002;            /* source for the GPU copy */
    r[3] = 0;
    wp.Dest = ID3D12Resource_GetGPUVirtualAddress(tb);
    wp.Value = 0x12340001;
    if (SUCCEEDED(ID3D12GraphicsCommandList_QueryInterface(l, &IID_ID3D12GraphicsCommandList2, (void **)&l2))) {
        ID3D12GraphicsCommandList2_WriteBufferImmediate(l2, 1, &wp, NULL);
        ID3D12GraphicsCommandList2_Release(l2);
    }
    ID3D12GraphicsCommandList_CopyBufferRegion(l, tb, 4, ub, 4, 4);
    ID3D12GraphicsCommandList_CopyBufferRegion(l, rb, 12, tb, 12, 4);
    u[2] = 0xdef00004;
    r[4] = 0;
    if (alias_ok) {
        ID3D12GraphicsCommandList_CopyBufferRegion(l, ra, 0, ub, 8, 4);
        ID3D12GraphicsCommandList_CopyBufferRegion(l, rb, 16, rb2, 0, 4);
    }
    u[3] = 0x13570005;
    r[5] = 0;
    if (placed_ok) {
        ID3D12GraphicsCommandList_CopyBufferRegion(l, pa, 0, ub, 12, 4);
        ID3D12GraphicsCommandList_CopyBufferRegion(l, rb, 20, pb, 0, 4);
    }
    ID3D12GraphicsCommandList_Close(l);
    ((execute_fn)queue_vt_orig[10])(q, 1, (ID3D12CommandList *const *)&l);
    ((signal_fn)queue_vt_orig[14])(q, f, 1);
    ID3D12Fence_SetEventOnCompletion(f, 1, ev);
    WaitForSingleObject(ev, 5000);
    logmsg("self-test fence=%llu: wbi->map %s (%#x), gpu copy->map %s (%#x), cpu map->gpu %s (%#x), tile alias %s (%#x), placed alias %s (%#x)",
           ID3D12Fence_GetCompletedValue(f),
           t[0] == 0x12340001 ? "OK" : "LOST", t[0], t[1] == 0x56780002 ? "OK" : "LOST", t[1],
           r[3] == 0x9abc0003 ? "OK" : "LOST", r[3], !alias_ok ? "n/a" : r[4] == 0xdef00004 ? "OK" : "LOST", r[4],
           !placed_ok ? "n/a" : r[5] == 0x13570005 ? "OK" : "LOST", r[5]);
done:
    if (f) ID3D12Fence_Release(f);
    if (ra) ID3D12Resource_Release(ra);
    if (rb2) ID3D12Resource_Release(rb2);
    if (th) ID3D12Heap_Release(th);
    if (pa) ID3D12Resource_Release(pa);
    if (pb) ID3D12Resource_Release(pb);
    if (ph) ID3D12Heap_Release(ph);
    if (rb) ID3D12Resource_Release(rb);
    if (ub) ID3D12Resource_Release(ub);
    if (tb) ID3D12Resource_Release(tb);
    if (l) ID3D12GraphicsCommandList_Release(l);
    if (a) ID3D12CommandAllocator_Release(a);
    if (q) ID3D12CommandQueue_Release(q);
    CloseHandle(ev);
}

/* Runs a compute shader of the bit/convert ops the wrapper's translated DXBC leans on
 * (f16tof32, ubfe, ushr, ftoi/ftou, bfi, ...) on D3DMetal and compares every result
 * with what D3D (DXBC semantics) defines. Needs the native d3dcompiler_47. */
static const char op_test_hlsl[] =
    "RWStructuredBuffer<uint> b : register(u0);\n"
    "[numthreads(1,1,1)] void main() {\n"
    "  uint i0=b[0],i1=b[1],i2=b[2],i3=b[3],i4=b[4],i5=b[5],i6=b[6],i7=b[7],i8=b[8],i9=b[9],i10=b[10],i11=b[11],i12=b[12],i13=b[13];\n"
    "  b[32]=asuint(f16tof32(i0)); b[33]=asuint(f16tof32(i1)); b[34]=asuint(f16tof32(i2)); b[35]=asuint(f16tof32(i3));\n"
    "  b[36]=(i4>>5)&0x7f; b[37]=(i4>>25)&0x7f; b[38]=i4>>i5; b[39]=i4<<i5; b[40]=asuint(((int)i4)>>i5);\n"
    "  b[41]=(i4&~(0x7fu<<5))|((i6<<5)&(0x7fu<<5)); b[42]=asuint((float)(int)i6); b[43]=asuint((int)asfloat(i7));\n"
    "  b[44]=(uint)asfloat(i8); b[45]=asuint((int)asfloat(i9)); b[46]=(uint)asfloat(i10); b[47]=asuint(((int)i11)>>31);\n"
    "  b[48]=asuint(((int)(i4<<20))>>25); b[49]=f32tof16(asfloat(i12)); b[50]=i4>>i13; b[51]=asuint((float)i6);\n"
    "}\n";

typedef HRESULT (WINAPI *d3dcompile_fn)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *,
                                        UINT, UINT, ID3DBlob **, ID3DBlob **);
typedef HRESULT (WINAPI *d3ddisasm_fn)(const void *, SIZE_T, UINT, const char *, ID3DBlob **);
typedef HRESULT (STDMETHODCALLTYPE *create_rs_fn)(ID3D12Device *, UINT, const void *, SIZE_T, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *create_cpso_fn)(ID3D12Device *, const D3D12_COMPUTE_PIPELINE_STATE_DESC *, REFIID, void **);

static void op_test(ID3D12Device *dev)
{
    static const uint32_t in[14] = {0x0000bc00, 0xabcdbc00, 0x00003c00, 0x00008001, 0xf0f0a5c3, 37, 0xffff8001,
                                    0xc0200000 /* -2.5f */, 0xbf800000 /* -1.0f */, 0x4f32d05e /* 3e9f */,
                                    0x7fc00000 /* NaN */, 0x80000000, 0xbf800000, 32};
    static const struct { const char *name; uint32_t want; } exp[] = {
        {"f16tof32(0xbc00)", 0xbf800000}, {"f16tof32(hi bits|0xbc00)", 0xbf800000}, {"f16tof32(0x3c00)", 0x3f800000},
        {"f16tof32(-denorm)", 0xb3800000}, {"ubfe 7@5", 0x2e}, {"ubfe 7@25", 0x78}, {"ushr by 37", 0x0787852e},
        {"ishl by 37", 0x1e14b860}, {"ishr by 37", 0xff87852e}, {"bfi 7@5", 0xf0f0a023},
        {"itof(-32767)", 0xc6fffe00}, {"ftoi(-2.5)", 0xfffffffe}, {"ftou(-1.0)", 0}, {"ftoi(3e9)", 0x7fffffff},
        {"ftou(NaN)", 0}, {"ishr sign", 0xffffffff}, {"ibfe 7@5", 0x2e}, {"f32tof16(-1.0)", 0xbc00},
        {"ushr by 32", 0xf0f0a5c3}, {"utof(0xffff8001)", 0x4f7fff80},
    };
    D3D12_HEAP_PROPERTIES custom = {D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0, 0, 0};
    D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 0x10000, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    D3D12_ROOT_PARAMETER rp;
    D3D12_ROOT_SIGNATURE_DESC rsd;
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd;
    HMODULE comp = LoadLibraryA("d3dcompiler_47.dll");
    d3dcompile_fn compile = comp ? (d3dcompile_fn)GetProcAddress(comp, "D3DCompile") : NULL;
    d3ddisasm_fn disasm = comp ? (d3ddisasm_fn)GetProcAddress(comp, "D3DDisassemble") : NULL;
    ID3DBlob *cs = NULL, *err = NULL, *rsb = NULL, *dis = NULL;
    ID3D12RootSignature *rs = NULL;
    ID3D12PipelineState *ps = NULL;
    ID3D12CommandQueue *q = NULL;
    ID3D12CommandAllocator *a = NULL;
    ID3D12GraphicsCommandList *l = NULL;
    ID3D12Resource *buf = NULL;
    ID3D12Fence *f = NULL;
    uint32_t *m = NULL;
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    HRESULT hr;
    int bad = 0;

    if (!compile) { logmsg("op-test: no D3DCompile"); return; }
    hr = compile(op_test_hlsl, sizeof(op_test_hlsl) - 1, "optest", NULL, NULL, "main", "cs_5_0", 0, 0, &cs, &err);
    if (FAILED(hr)) { logmsg("op-test: compile failed %#lx %s", hr, err ? (char *)ID3D10Blob_GetBufferPointer(err) : ""); goto done; }
    if (disasm && SUCCEEDED(disasm(ID3D10Blob_GetBufferPointer(cs), ID3D10Blob_GetBufferSize(cs), 0, NULL, &dis))) {
        const char *t = ID3D10Blob_GetBufferPointer(dis);
        logmsg("op-test: shader has ubfe %d ibfe %d bfi %d f16tof32 %d ftou %d", !!strstr(t, "ubfe"), !!strstr(t, "ibfe"),
               !!strstr(t, "bfi"), !!strstr(t, "f16tof32"), !!strstr(t, "ftou"));
    }
    memset(&rp, 0, sizeof(rp));
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    memset(&rsd, 0, sizeof(rsd));
    rsd.NumParameters = 1;
    rsd.pParameters = &rp;
    hr = real_serialize_rs(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, NULL);
    if (SUCCEEDED(hr)) hr = ((create_rs_fn)dev_vt_orig[16])(dev, 0, ID3D10Blob_GetBufferPointer(rsb), ID3D10Blob_GetBufferSize(rsb),
                                                           &IID_ID3D12RootSignature, (void **)&rs);
    memset(&cd, 0, sizeof(cd));
    cd.pRootSignature = rs;
    cd.CS.pShaderBytecode = ID3D10Blob_GetBufferPointer(cs);
    cd.CS.BytecodeLength = ID3D10Blob_GetBufferSize(cs);
    if (SUCCEEDED(hr)) hr = ((create_cpso_fn)dev_vt_orig[11])(dev, &cd, &IID_ID3D12PipelineState, (void **)&ps);
    if (SUCCEEDED(hr)) hr = ((create_queue_fn)dev_vt_orig[8])(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&a);
    if (SUCCEEDED(hr)) hr = ((create_list_fn)dev_vt_orig[12])(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, ps,
                                                             &IID_ID3D12GraphicsCommandList, (void **)&l);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommittedResource(dev, &custom, D3D12_HEAP_FLAG_NONE, &rd,
                                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
                                                                &IID_ID3D12Resource, (void **)&buf);
    if (SUCCEEDED(hr)) hr = ((create_fence_fn)dev_vt_orig[36])(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&f);
    if (SUCCEEDED(hr)) hr = ID3D12Resource_Map(buf, 0, NULL, (void **)&m);
    if (FAILED(hr)) { logmsg("op-test: setup failed %#lx", hr); goto done; }

    memset(m, 0xcd, 64 * 4);
    memcpy(m, in, sizeof(in));
    ID3D12GraphicsCommandList_SetComputeRootSignature(l, rs);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(l, 0, ID3D12Resource_GetGPUVirtualAddress(buf));
    ID3D12GraphicsCommandList_Dispatch(l, 1, 1, 1);
    ID3D12GraphicsCommandList_Close(l);
    ((execute_fn)queue_vt_orig[10])(q, 1, (ID3D12CommandList *const *)&l);
    ((signal_fn)queue_vt_orig[14])(q, f, 1);
    ID3D12Fence_SetEventOnCompletion(f, 1, ev);
    WaitForSingleObject(ev, 5000);
    for (UINT i = 0; i < sizeof(exp) / sizeof(exp[0]); i++) {
        BOOL ok = m[32 + i] == exp[i].want;
        bad += !ok;
        logmsg("op-test %-26s got %08x want %08x %s", exp[i].name, m[32 + i], exp[i].want, ok ? "ok" : "MISMATCH");
    }
    logmsg("op-test: %d mismatches (fence %llu)", bad, ID3D12Fence_GetCompletedValue(f));

    /* ubfe with width 0 in some components (1334 such ubfe in the wrapper's shaders): DXBC gives 0 */
    {
        static const char hlsl2[] =
            "RWStructuredBuffer<uint> b : register(u0);\n"
            "[numthreads(1,1,1)] void main() { uint4 v = b[4].xxxx; uint4 r = (v >> uint4(5,6,7,8)) & 0x7f;\n"
            "  b[60]=r.x; b[61]=r.y; b[62]=r.z; b[63]=r.w; }\n";
        ID3DBlob *cs2 = NULL;
        ID3D12PipelineState *ps2 = NULL;
        uint32_t *code;
        UINT ntok, patched = 0;
        hr = compile(hlsl2, sizeof(hlsl2) - 1, "optest2", NULL, NULL, "main", "cs_5_0", 0, 0, &cs2, NULL);
        if (FAILED(hr)) { logmsg("op-test2: compile failed %#lx", hr); goto done; }
        code = ID3D10Blob_GetBufferPointer(cs2);
        ntok = ID3D10Blob_GetBufferSize(cs2) / 4;
        /* find "ubfe dst, l(7,7,7,7), l(5,6,7,8), src": opcode 138, then immediate vec4 of 7s */
        for (UINT k = 0; k + 12 < ntok && !patched; k++) {
            if ((code[k] & 0x7ff) != 138) continue;
            for (UINT j = k + 1; j + 4 < k + ((code[k] >> 24) & 0x7f); j++)
                if (code[j] == 7 && code[j + 1] == 7 && code[j + 2] == 7 && code[j + 3] == 7) {
                    code[j + 1] = 0;   /* y width 0 */
                    code[j + 3] = 0;   /* w width 0 */
                    patched = 1;
                    break;
                }
        }
        logmsg("op-test2: ubfe width immediates %s", patched ? "patched (y,w = 0)" : "NOT FOUND");
        cd.CS.pShaderBytecode = code;
        cd.CS.BytecodeLength = ID3D10Blob_GetBufferSize(cs2);
        hr = ((create_cpso_fn)dev_vt_orig[11])(dev, &cd, &IID_ID3D12PipelineState, (void **)&ps2);
        if (FAILED(hr)) { logmsg("op-test2: pso failed %#lx (checksum?)", hr); ID3D10Blob_Release(cs2); goto done; }
        ID3D12CommandAllocator_Reset(a);
        ID3D12GraphicsCommandList_Reset(l, a, ps2);
        ID3D12GraphicsCommandList_SetComputeRootSignature(l, rs);
        ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(l, 0, ID3D12Resource_GetGPUVirtualAddress(buf));
        ID3D12GraphicsCommandList_Dispatch(l, 1, 1, 1);
        ID3D12GraphicsCommandList_Close(l);
        m[60] = m[61] = m[62] = m[63] = 0xcdcdcdcd;
        ((execute_fn)queue_vt_orig[10])(q, 1, (ID3D12CommandList *const *)&l);
        ((signal_fn)queue_vt_orig[14])(q, f, 2);
        ID3D12Fence_SetEventOnCompletion(f, 2, ev);
        WaitForSingleObject(ev, 5000);
        logmsg("op-test2 ubfe vec: x %08x (want 2e) y %08x (want 0) z %08x (want %02x) w %08x (want 0) %s",
               m[60], m[61], m[62], (in[4] >> 7) & 0x7f, m[63],
               m[60] == 0x2e && m[61] == 0 && m[62] == ((in[4] >> 7) & 0x7f) && m[63] == 0 ? "ok" : "MISMATCH");
        ID3D12PipelineState_Release(ps2);
        ID3D10Blob_Release(cs2);
    }

    /* typed buffer SRV loads in the formats the wrapper uses (10, 34, 57) */
    {
        static const char hlsl3[] =
            "RWStructuredBuffer<uint> b : register(u0);\n"
            "Buffer<float4> t0 : register(t0); Buffer<float2> t1 : register(t1); Buffer<uint> t2 : register(t2); Buffer<float2> t3 : register(t3);\n"
            "[numthreads(1,1,1)] void main() { uint i = b[13] - 31;\n"   /* i == 1, opaque to the compiler */
            "  float4 a = t0[i]; float2 c = t1[i + 1]; float2 e = t3[i - 1];\n"
            "  b[64]=asuint(a.x); b[65]=asuint(a.y); b[66]=asuint(a.z); b[67]=asuint(a.w);\n"
            "  b[68]=asuint(c.x); b[69]=asuint(c.y); b[70]=t2[i + 2]; b[71]=asuint(e.x); b[72]=asuint(e.y); }\n";
        static const struct { const char *name; uint32_t want; } exp3[] = {
            {"RGBA16F[1].x  1.0", 0x3f800000}, {"RGBA16F[1].y -2.0", 0xc0000000}, {"RGBA16F[1].z  0.5", 0x3f000000},
            {"RGBA16F[1].w -0.25", 0xbe800000}, {"RG16F[2].x -1.0", 0xbf800000}, {"RG16F[2].y  3.0", 0x40400000},
            {"R16_UINT[3] 65534", 65534}, {"RG16F first=1 [0].x", 0xbf800000}, {"RG16F first=1 [0].y", 0x40400000},
        };
        D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0};
        D3D12_ROOT_PARAMETER rp3[2];
        D3D12_ROOT_SIGNATURE_DESC rsd3;
        D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        D3D12_RESOURCE_DESC dd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 0x10000, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
        ID3DBlob *cs3 = NULL, *rsb3 = NULL;
        ID3D12RootSignature *rs3 = NULL;
        ID3D12PipelineState *ps3 = NULL;
        ID3D12DescriptorHeap *dh = NULL;
        ID3D12Resource *data = NULL;
        uint16_t *dm = NULL;
        UINT inc;
        int bad3 = 0;

        hr = compile(hlsl3, sizeof(hlsl3) - 1, "optest3", NULL, NULL, "main", "cs_5_0", 0, 0, &cs3, &err);
        if (FAILED(hr)) { logmsg("op-test3: compile failed %#lx", hr); goto done; }
        memset(rp3, 0, sizeof(rp3));
        rp3[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        rp3[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp3[1].DescriptorTable.NumDescriptorRanges = 1;
        rp3[1].DescriptorTable.pDescriptorRanges = &range;
        memset(&rsd3, 0, sizeof(rsd3));
        rsd3.NumParameters = 2;
        rsd3.pParameters = rp3;
        hr = real_serialize_rs(&rsd3, D3D_ROOT_SIGNATURE_VERSION_1, &rsb3, NULL);
        if (SUCCEEDED(hr)) hr = ((create_rs_fn)dev_vt_orig[16])(dev, 0, ID3D10Blob_GetBufferPointer(rsb3), ID3D10Blob_GetBufferSize(rsb3),
                                                               &IID_ID3D12RootSignature, (void **)&rs3);
        cd.pRootSignature = rs3;
        cd.CS.pShaderBytecode = ID3D10Blob_GetBufferPointer(cs3);
        cd.CS.BytecodeLength = ID3D10Blob_GetBufferSize(cs3);
        if (SUCCEEDED(hr)) hr = ((create_cpso_fn)dev_vt_orig[11])(dev, &cd, &IID_ID3D12PipelineState, (void **)&ps3);
        if (SUCCEEDED(hr)) hr = ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&dh);
        if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommittedResource(dev, &custom, D3D12_HEAP_FLAG_NONE, &dd,
                                                                    D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                                    &IID_ID3D12Resource, (void **)&data);
        if (SUCCEEDED(hr)) hr = ID3D12Resource_Map(data, 0, NULL, (void **)&dm);
        if (FAILED(hr)) { logmsg("op-test3: setup failed %#lx", hr); goto done3; }
        memset(dm, 0, 0x1000);
        /* t0 @0: RGBA16F element 1 = (1, -2, 0.5, -0.25) */
        dm[4] = 0x3c00; dm[5] = 0xc000; dm[6] = 0x3800; dm[7] = 0xb400;
        /* t1/t3 @0x100: RG16F element 2 = (-1, 3); t3 views it with FirstElement 2 */
        dm[0x80 + 4] = 0xbc00; dm[0x80 + 5] = 0x4200;
        /* t2 @0x200: R16_UINT element 3 = 65534 */
        dm[0x100 + 3] = 0xfffe;
        inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        {
            D3D12_CPU_DESCRIPTOR_HANDLE h;
            D3D12_SHADER_RESOURCE_VIEW_DESC sd;
            static const struct { DXGI_FORMAT fmt; UINT first; } views[4] = {
                {DXGI_FORMAT_R16G16B16A16_FLOAT, 0}, {DXGI_FORMAT_R16G16_FLOAT, 0x100 / 4},
                {DXGI_FORMAT_R16_UINT, 0x200 / 2}, {DXGI_FORMAT_R16G16_FLOAT, 0x100 / 4 + 2}};
            dh->lpVtbl->GetCPUDescriptorHandleForHeapStart(dh, &h);
            for (UINT v = 0; v < 4; v++) {
                memset(&sd, 0, sizeof(sd));
                sd.Format = views[v].fmt;
                sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Buffer.FirstElement = views[v].first;
                sd.Buffer.NumElements = 16;
                ((create_srv_fn)dev_vt_orig[18])(dev, data, &sd, h);
                h.ptr += inc;
            }
        }
        {
            D3D12_GPU_DESCRIPTOR_HANDLE g;
            dh->lpVtbl->GetGPUDescriptorHandleForHeapStart(dh, &g);
            ID3D12CommandAllocator_Reset(a);
            ID3D12GraphicsCommandList_Reset(l, a, ps3);
            ID3D12GraphicsCommandList_SetDescriptorHeaps(l, 1, &dh);
            ID3D12GraphicsCommandList_SetComputeRootSignature(l, rs3);
            ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(l, 0, ID3D12Resource_GetGPUVirtualAddress(buf));
            ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(l, 1, g);
            ID3D12GraphicsCommandList_Dispatch(l, 1, 1, 1);
            ID3D12GraphicsCommandList_Close(l);
        }
        ((execute_fn)queue_vt_orig[10])(q, 1, (ID3D12CommandList *const *)&l);
        ((signal_fn)queue_vt_orig[14])(q, f, 3);
        ID3D12Fence_SetEventOnCompletion(f, 3, ev);
        WaitForSingleObject(ev, 5000);
        for (UINT i = 0; i < sizeof(exp3) / sizeof(exp3[0]); i++) {
            BOOL ok = m[64 + i] == exp3[i].want;
            bad3 += !ok;
            logmsg("op-test3 %-22s got %08x want %08x %s", exp3[i].name, m[64 + i], exp3[i].want, ok ? "ok" : "MISMATCH");
        }
        logmsg("op-test3: %d mismatches", bad3);
    done3:
        if (data) ID3D12Resource_Release(data);
        if (dh) ID3D12DescriptorHeap_Release(dh);
        if (ps3) ID3D12PipelineState_Release(ps3);
        if (rs3) ID3D12RootSignature_Release(rs3);
        if (rsb3) ID3D10Blob_Release(rsb3);
        if (cs3) ID3D10Blob_Release(cs3);
    }
done:
    if (f) ID3D12Fence_Release(f);
    if (buf) ID3D12Resource_Release(buf);
    if (l) ID3D12GraphicsCommandList_Release(l);
    if (a) ID3D12CommandAllocator_Release(a);
    if (q) ID3D12CommandQueue_Release(q);
    if (ps) ID3D12PipelineState_Release(ps);
    if (rs) ID3D12RootSignature_Release(rs);
    if (rsb) ID3D10Blob_Release(rsb);
    if (dis) ID3D10Blob_Release(dis);
    if (cs) ID3D10Blob_Release(cs);
    if (err) ID3D10Blob_Release(err);
    CloseHandle(ev);
}

static void hook_device(void *device);

static HRESULT STDMETHODCALLTYPE hk_device_qi(void *dev, REFIID riid, void **ppv)
{
    HRESULT hr = ((qi_fn)dev_vt_orig[0])(dev, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv && *ppv != dev && **(void ***)ppv == dev_vt_orig)
        hook_device(*ppv);
    return hr;
}

static void hook_device(void *device)
{
    void ***obj = device;
    if (*obj == dev_vt) return;
    if (!dev_vt_orig) {
        dev_vt_orig = *obj;
        memcpy(dev_vt, dev_vt_orig, sizeof(dev_vt));
        dev_vt[0] = (void *)hk_device_qi;
        dev_vt[8] = (void *)hk_create_queue;
        dev_vt[10] = (void *)hk_create_gpso;
        dev_vt[12] = (void *)hk_create_list;
        dev_vt[18] = (void *)hk_create_srv;
        dev_vt[13] = (void *)hk_check_feature;
        real_check_feature = dev_vt_orig[13];
        dev_vt[41] = (void *)hk_create_signature;
        dev_vt[44] = (void *)hk_create_lib;
        dev_vt[27] = (void *)hk_create_committed;
        dev_vt[29] = (void *)hk_create_placed;
        dev_vt[30] = (void *)hk_create_reserved;
        dev_vt[53] = (void *)hk_create_committed1;
        dev_vt[51] = (void *)hk_create_list1;
        dev_vt[48] = (void *)hk_open_existing_heap;
        the_device = device;
    } else if (*obj != dev_vt_orig) {
        logmsg("device %p has an unexpected vtable %p, not hooked", device, *obj);
        return;
    }
    *obj = dev_vt;
    logmsg("hooked device %p", device);
}

/* ---------------- game fix-up in memory ---------------- */

/* The game reads the current fiber inline with "mov REG, gs:[0x20]". On macOS
 * Wine gs does not point at the TEB except for a few slots, and that read
 * returns a Mach port number (or 0), so the game crashes in its job system.
 * Each site is rerouted, in memory only, to a small cave in the .text padding
 * that reads the TEB through its self pointer:
 *     mov REG, gs:[0x30]; mov REG, [REG+0x20]; ret
 * The game file on disk stays untouched. A thread looks for the game image (by
 * its PE timestamp and size) once the wrapper has mapped it, and patches it only
 * if every site still holds the original instruction. FH2_FIBER=0 disables it.
 *
 * Every title that ships this engine and needs the reroute is listed here by the
 * PE timestamp and the size of its image; the sites are found by scanning that
 * image's executable sections at run time, so a new title costs two numbers.
 * Forza Horizon 2 1.0.0.64, its Fast & Furious standalone, Forza Horizon 3,
 * Forza Motorsport 5 and Forza Motorsport 6 are the ones seen so far. */
struct fiber_site { DWORD rva; uint8_t reg; };

struct game_image { DWORD timestamp, size; const char *name; };

static const struct game_image known_images[] = {
    {0x565501edu, 0x3a18000u, "Forza Horizon 2 1.0.0.64"},
    {0x55b62fdeu, 0x3a89000u, "Forza Horizon 2 Presents Fast & Furious 1.0.0.6"},
    {0x5b16eccau, 0x5495000u, "Forza Horizon 3 1.0.125.0"},
    {0x53d8d1f2u, 0x279d000u, "Forza Motorsport 5 1.0.0.20"},
    {0x58afb1cdu, 0x1fc8000u, "Forza Motorsport 6 1.0.0.44"},
};

/* reads memory the wrapper may be remapping right now: a region VirtualQuery
 * reported as readable can be gone a moment later, and a plain read of it would
 * crash the process */
static BOOL safe_read(const void *src, void *dst, SIZE_T n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), src, dst, n, &got) && got == n;
}

/* the game image the wrapper mapped, if it is one this layer knows */
static uint8_t *find_game_image(const struct game_image **which)
{
    MEMORY_BASIC_INFORMATION mi;
    for (uint8_t *p = (uint8_t *)0x10000; VirtualQuery(p, &mi, sizeof(mi)) == sizeof(mi); p = (uint8_t *)mi.BaseAddress + mi.RegionSize) {
        IMAGE_DOS_HEADER dos;
        IMAGE_NT_HEADERS64 nt;
        if (mi.State != MEM_COMMIT || mi.RegionSize < 0x1000 || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !mi.Protect)
            continue;
        if (!safe_read(mi.BaseAddress, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
            dos.e_lfanew <= 0 || dos.e_lfanew > 0x800)
            continue;
        if (!safe_read((const uint8_t *)mi.BaseAddress + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE)
            continue;
        for (size_t i = 0; i < sizeof(known_images) / sizeof(known_images[0]); i++) {
            if (nt.FileHeader.TimeDateStamp == known_images[i].timestamp && nt.OptionalHeader.SizeOfImage == known_images[i].size) {
                *which = &known_images[i];
                return (uint8_t *)mi.BaseAddress;
            }
        }
    }
    return NULL;
}

/* The sites differ per title and some titles have two hundred of them, so they
 * are found by scanning the image's executable sections instead of being listed
 * here. Only what still holds the original instruction is patched. */
#define FIBER_MAX_SITES 1024

/* the 9 bytes of "mov REG, gs:[0x20]": 65 REX.W 8b modrm(SIB) 25 disp32 0x20 */
static BOOL fiber_site_bytes(const uint8_t *p, size_t n, uint8_t *reg)
{
    int32_t disp;
    if (n < 9 || p[0] != 0x65 || p[2] != 0x8b || p[4] != 0x25) return FALSE;
    if ((p[1] & 0xfb) != 0x48) return FALSE;              /* REX.W, X and B clear, R free */
    if ((p[3] >> 6) != 0 || (p[3] & 7) != 4) return FALSE;  /* mod 0, rm = SIB */
    memcpy(&disp, p + 5, 4);
    if (disp != 0x20) return FALSE;
    *reg = (uint8_t)(((p[1] >> 2) & 1) << 3 | ((p[3] >> 3) & 7));
    return TRUE;
}

/* every site in the image's executable sections that still holds the instruction */
static size_t scan_fiber_sites(const uint8_t *game, struct fiber_site *out, size_t cap)
{
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    size_t n = 0;
    if (!safe_read(game, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) return 0;
    if (!safe_read(game + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE) return 0;
    const uint8_t *secbase = game + dos.e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
    for (unsigned s = 0; s < nt.FileHeader.NumberOfSections && n < cap; s++) {
        IMAGE_SECTION_HEADER sh;
        if (!safe_read(secbase + s * sizeof(sh), &sh, sizeof(sh))) break;
        if (!(sh.Characteristics & IMAGE_SCN_MEM_EXECUTE) || !sh.Misc.VirtualSize) continue;
        const uint8_t *base = game + sh.VirtualAddress;
        DWORD len = sh.Misc.VirtualSize;
        for (DWORD off = 0; off + 9 <= len && n < cap; off += 4096) {
            uint8_t buf[4096 + 8];
            DWORD chunk = len - off < 4096 ? len - off : 4096;
            DWORD got = chunk + (chunk < len - off ? 8 : 0);   /* +8: matches across the seam */
            if (!safe_read(base + off, buf, got)) continue;
            for (DWORD k = 0; k + 9 <= got && n < cap; k++) {
                uint8_t reg;
                if (!fiber_site_bytes(buf + k, got - k, &reg)) continue;
                out[n].rva = sh.VirtualAddress + off + k;
                out[n].reg = reg;
                n++;
            }
        }
    }
    return n;
}

static volatile LONG fiber_patching;
static uint8_t *volatile fiber_game;
static const struct game_image *volatile fiber_image;

/* the exception handler reads fiber_game first and then fiber_image: publish the
 * image first, so a handler that sees the game never sees it without its size */
static void publish_fiber_game(uint8_t *game, const struct game_image *which)
{
    fiber_image = which;
    MemoryBarrier();
    fiber_game = game;
}

static const uint8_t fiber_pad[16] = {0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
                                      0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc};

/* a 16-byte run of int3 padding in the image's executable sections, searched from
 * *cursor on: it must be inside the image, so a call rel32 from any site reaches
 * it, and it must be padding the compiler left after a function */
static uint8_t *fiber_cave(uint8_t *game, DWORD *cursor)
{
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    if (!safe_read(game, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) return NULL;
    if (!safe_read(game + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE) return NULL;
    const uint8_t *secbase = game + dos.e_lfanew + 4 + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
    for (unsigned s = 0; s < nt.FileHeader.NumberOfSections; s++) {
        IMAGE_SECTION_HEADER sh;
        if (!safe_read(secbase + s * sizeof(sh), &sh, sizeof(sh))) return NULL;
        if (!(sh.Characteristics & IMAGE_SCN_MEM_EXECUTE) || sh.Misc.VirtualSize < 16) continue;
        DWORD end = sh.VirtualAddress + sh.Misc.VirtualSize;
        DWORD off = sh.VirtualAddress > *cursor ? sh.VirtualAddress : *cursor;
        for (; off + 16 <= end; off += 4096) {
            uint8_t buf[4096 + 16];
            DWORD chunk = end - off < 4096 ? end - off : 4096;
            DWORD got = chunk + (chunk < end - off ? 16 : 0);
            if (!safe_read(game + off, buf, got)) continue;
            for (DWORD k = 0; k + 16 <= got; k++) {
                if (buf[k] != 0xcc || memcmp(buf + k, fiber_pad, 16)) continue;
                *cursor = off + k + 16;
                return game + off + k;
            }
        }
    }
    return NULL;
}

/* reroutes the sites found in the image to a per-register cave in the .text
 * padding; returns how many it patched in this pass (0: none left) */
static size_t patch_fiber_sites(uint8_t *game, const struct game_image *which)
{
    static struct fiber_site sites[FIBER_MAX_SITES];
    uint8_t *caves[16] = {0};
    DWORD old, cursor = 0;
    size_t n, done = 0;
    if (InterlockedCompareExchange(&fiber_patching, 1, 0)) return 0;   /* one patcher at a time */
    n = scan_fiber_sites(game, sites, FIBER_MAX_SITES);
    if (!n) {
        InterlockedExchange(&fiber_patching, 0);
        return 0;
    }
    /* one cave per register: mov REG, gs:[0x30]; mov REG, [REG+0x20]; ret */
    for (size_t i = 0; i < n; i++) {
        uint8_t reg = sites[i].reg, r = reg >= 8;
        uint8_t cave[14] = {0x65, (uint8_t)(0x48 | r << 2), 0x8b, (uint8_t)(0x04 | (reg & 7) << 3), 0x25, 0x30, 0, 0, 0,
                            (uint8_t)(0x48 | r << 2 | r), 0x8b, (uint8_t)(0x40 | (reg & 7) << 3 | (reg & 7)), 0x20, 0xc3};
        uint8_t *c;
        if (caves[reg]) continue;
        c = fiber_cave(game, &cursor);
        if (!c) {
            logmsg("fiber fix: no padding left for a cave");
            InterlockedExchange(&fiber_patching, 0);
            return 0;
        }
        if (!VirtualProtect(c, 16, PAGE_EXECUTE_READWRITE, &old)) {
            InterlockedExchange(&fiber_patching, 0);
            return 0;
        }
        memcpy(c, cave, sizeof(cave));
        VirtualProtect(c, 16, old, &old);
        caves[reg] = c;
    }
    for (size_t i = 0; i < n; i++) {
        uint8_t *site = game + sites[i].rva, code[9];
        int32_t rel = (int32_t)(caves[sites[i].reg] - (site + 5));
        if (!caves[sites[i].reg]) continue;
        code[0] = 0xe8;
        memcpy(code + 1, &rel, 4);
        memset(code + 5, 0x90, 4);
        if (!VirtualProtect(site, 9, PAGE_EXECUTE_READWRITE, &old)) continue;
        memcpy(site, code, 9);
        VirtualProtect(site, 9, old, &old);
        done++;
    }
    FlushInstructionCache(GetCurrentProcess(), game, which->size);
    logmsg("fiber fix: %zu inline fiber reads rerouted in the game image at %p (%s)", done, game, which->name);
    InterlockedExchange(&fiber_patching, 0);
    return done;
}

/* Safety net for the race above: if a game thread runs a fiber read before the
 * patch, the bad value (a Mach port number) faults on first use. The handler
 * recognises that fault (inside the game image, at an address just above the bad
 * value gs:[0x20] gives on this thread), puts the real FiberData into every
 * register that holds the bad value, patches all sites and resumes the thread. */
static LONG WINAPI fiber_fault_handler(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    DWORD64 *regs[] = {&c->Rax, &c->Rcx, &c->Rdx, &c->Rbx, &c->Rbp, &c->Rsi, &c->Rdi, &c->R8,
                       &c->R9, &c->R10, &c->R11, &c->R12, &c->R13, &c->R14, &c->R15};
    DWORD64 bad, good, addr;
    uint8_t *game = fiber_game;
    const struct game_image *which;
    int fixed = 0;
    MemoryBarrier();   /* pairs with publish_fiber_game: image first, then game */
    which = fiber_image;
    if (!which) game = NULL;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    /* cheap checks first: other access violations must not pay for the image search */
    bad = __readgsqword(0x20);
    good = (DWORD64)(uintptr_t)((NT_TIB *)NtCurrentTeb())->FiberData;
    addr = er->ExceptionInformation[1];
    if (bad == good || addr < bad || addr >= bad + 0x1000) return EXCEPTION_CONTINUE_SEARCH;
    if (!game && !(game = find_game_image(&which))) return EXCEPTION_CONTINUE_SEARCH;
    if (c->Rip < (DWORD64)(uintptr_t)game || c->Rip >= (DWORD64)(uintptr_t)game + which->size) return EXCEPTION_CONTINUE_SEARCH;
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
        if (*regs[i] == bad) { *regs[i] = good; fixed++; }
    if (!fixed) return EXCEPTION_CONTINUE_SEARCH;
    publish_fiber_game(game, which);
    patch_fiber_sites(game, which);   /* everything still unpatched */
    logmsg("fiber fix: caught a fiber read before the patch at game+%#llx, %d register(s) corrected",
           (unsigned long long)(c->Rip - (DWORD64)(uintptr_t)game), fixed);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI game_fixup_thread(void *arg)
{
    DWORD start = GetTickCount();
    (void)arg;
    uint8_t *game = NULL;
    const struct game_image *which = NULL;
    DWORD found = 0;
    /* the launcher can stay open for minutes before the game is mapped */
    while (GetTickCount() - start < 1800000) {
        if (!game && (game = find_game_image(&which))) {
            publish_fiber_game(game, which);
            found = GetTickCount();
            logmsg("game image found at %p after %lu ms (%s)", game, found - start, which->name);
        }
        if (game) {
            DWORD waited = GetTickCount();
            size_t total = 0;
            /* the wrapper writes the headers first and the sections after: patch
             * what is there, and keep going while new code still arrives */
            for (;;) {
                size_t pass = patch_fiber_sites(game, which);
                if (pass) {
                    if (!total) logmsg("game code in place %lu ms after its headers", GetTickCount() - found);
                    total += pass;
                    Sleep(100);
                    continue;
                }
                if (total) return 0;   /* nothing left to patch */
                if (GetTickCount() - waited > 60000) {
                    logmsg("fiber fix: no inline fiber reads found in %s, game left alone", which->name);
                    return 0;
                }
                Sleep(5);
            }
        }
        Sleep(5);
    }
    logmsg("fiber fix: game image not found in 30 minutes");
    return 0;
}

/* ---------------- thread priorities ---------------- */

/* Wine on macOS gives threads of normal priority Mach priority 26, below the 31
 * of native threads, and Azimuth's worker, queue and many game threads run there;
 * the scheduler then treats them as less urgent and frames come late. Every 2 s
 * every thread below Windows priority n is raised to n: by default 1 (above
 * normal, which Wine maps to 31). That halved the late frames in busy places;
 * 2 (highest, Mach 35) gave nothing more. FH2_PRIO=0 leaves priorities alone.
 * The first pass logs how many threads had each priority. */
static int prio_target;

static DWORD WINAPI prio_thread(void *arg)
{
    DWORD pid = GetCurrentProcessId();
    BOOL first = TRUE;
    (void)arg;
    for (;;) {
        int seen[5] = {0};   /* lowest, below normal, normal, above normal, highest+ */
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te = {sizeof(te)};
        int raised = 0;
        if (snap != INVALID_HANDLE_VALUE) {
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                HANDLE t;
                if (te.th32OwnerProcessID != pid) continue;
                if (!(t = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SET_INFORMATION, FALSE, te.th32ThreadID)))
                    continue;
                {
                    int p = GetThreadPriority(t);
                    seen[p <= -2 ? 0 : p >= 2 ? 4 : p + 2]++;
                    if (p < prio_target) {
                        if (SetThreadPriority(t, prio_target)) raised++;
                        else if (first) logmsg("priorities: thread %lu not raised, error %lu", te.th32ThreadID, GetLastError());
                    }
                }
                CloseHandle(t);
            }
            CloseHandle(snap);
        }
        if (first || raised)
            logmsg("priorities: lowest %d, below normal %d, normal %d, above normal %d, highest+ %d; %d raised to %d",
                   seen[0], seen[1], seen[2], seen[3], seen[4], raised, prio_target);
        first = FALSE;
        Sleep(2000);
    }
    return 0;
}

/* ---------------- FlushProcessWriteBuffers ---------------- */

/* Azimuth calls FlushProcessWriteBuffers from several threads, its worker very
 * often. CrossOver's Wine implements it on macOS by walking task_threads and
 * calling thread_get_register_pointer_values on every thread, with no lock.
 * Under Rosetta two threads doing that at once end up waiting in thread_get_state
 * on each other forever: in the hang the worker and 5 more threads all sit in
 * NtFlushProcessWriteBuffers -> thread_get_state, the GPU is idle and the game
 * stops presenting. kernel32 and kernelbase only forward it to ntdll, and the
 * wrapper hands it to the game through GetProcAddress, so the hook sits in ntdll:
 * the 32-byte syscall stub of NtFlushProcessWriteBuffers is copied out as the
 * real call and its entry jumps to a wrapper that lets one thread through at a
 * time. If the stub is not the expected one, only azimuth.exe's import is
 * redirected (which misses the game's calls). All in memory. FH2_FLUSH=0 disables it.
 * By default (since 0.1.7) the wrapper does not make the real call but the CoreCLR
 * trick (FH2_FLUSH=real keeps the real call, serialized): one page is made
 * writable, written and made inaccessible again, and the protection change makes
 * the kernel shoot down that page's TLB entry on every core running the process,
 * which drains their write buffers. It never looks at other threads, so it cannot
 * get stuck in thread_get_state (seen with a single caller under Azimuth 1.0.2)
 * and costs a couple of syscalls instead of reading every thread's registers.
 * Wine's VirtualProtect waits for its process-wide virtual memory lock, which other
 * threads sometimes hold for 80-250 ms (mapping views), so the page is changed
 * with macOS's mprotect called straight through the syscall instruction, which
 * Rosetta hands to the kernel without going through Wine; VirtualProtect is the
 * fallback if that call fails. The page ends inaccessible, as Wine believes it is;
 * FH2_FLUSH=nolock keeps the wrapper for its timings but takes no lock;
 * FH2_FLUSH=skip (diagnostic only, not safe) replaces the call with a local fence.
 * Every 10 s the log gets the call count, the time spent in the real call and
 * the time spent waiting for the lock (average and worst, in microseconds). */
static void (WINAPI *real_flush)(void);
static SRWLOCK flush_lock = SRWLOCK_INIT;
static BOOL flush_nolock, flush_skip;
static volatile LONG *flush_page;   /* the protect trick, unless FH2_FLUSH=real */
static LARGE_INTEGER flush_freq;
static LONG64 flush_calls, flush_busy, flush_busy_max, flush_wait, flush_wait_max, flush_report;

static void flush_max(LONG64 *max, LONG64 v)
{
    LONG64 m;
    while (v > (m = *max) && InterlockedCompareExchange64(max, v, m) != m) ;
}

#define FLUSH_PAGE_SIZE 0x4000   /* a whole host page on Apple Silicon */
static BOOL flush_raw = TRUE;

/* macOS mprotect (BSD syscall 74); returns 0 or the errno */
static long mac_mprotect(volatile void *addr, SIZE_T len, int prot)
{
    long ret;
    unsigned char failed;
    __asm__ volatile("syscall; setc %1"
                     : "=a"(ret), "=r"(failed)
                     : "a"(0x2000000L + 74), "D"(addr), "S"(len), "d"((long)prot)
                     : "rcx", "r11", "memory", "cc");
    return failed ? (ret ? ret : -1) : 0;
}

static void protect_flush(void)
{
    DWORD old;
    if (flush_raw) {
        long e = mac_mprotect(flush_page, FLUSH_PAGE_SIZE, 3 /* PROT_READ|PROT_WRITE */);
        if (!e) {
            InterlockedIncrement(flush_page);   /* the page is in this core's TLB and dirty */
            e = mac_mprotect(flush_page, FLUSH_PAGE_SIZE, 0 /* PROT_NONE */);
        }
        if (!e) return;
        flush_raw = FALSE;
        logmsg("flush: mprotect syscall failed (%ld), using VirtualProtect", e);
    }
    {
        LONG64 t0 = slow_start();
        VirtualProtect((void *)flush_page, FLUSH_PAGE_SIZE, PAGE_READWRITE, &old);
        InterlockedIncrement(flush_page);
        VirtualProtect((void *)flush_page, FLUSH_PAGE_SIZE, PAGE_NOACCESS, &old);
        slow_end("flush: VirtualProtect pair", t0);
    }
}

static void WINAPI hk_flush(void)
{
    LARGE_INTEGER t0, t1, t2;
    LONG64 report;
    QueryPerformanceCounter(&t0);
    if (!flush_nolock) AcquireSRWLockExclusive(&flush_lock);
    QueryPerformanceCounter(&t1);
    if (flush_skip) MemoryBarrier(); else if (flush_page) protect_flush(); else real_flush();
    QueryPerformanceCounter(&t2);
    if (!flush_nolock) ReleaseSRWLockExclusive(&flush_lock);
    InterlockedIncrement64(&flush_calls);
    InterlockedAdd64(&flush_wait, t1.QuadPart - t0.QuadPart);
    InterlockedAdd64(&flush_busy, t2.QuadPart - t1.QuadPart);
    flush_max(&flush_wait_max, t1.QuadPart - t0.QuadPart);
    flush_max(&flush_busy_max, t2.QuadPart - t1.QuadPart);
    report = flush_report;
    if (t2.QuadPart - report > flush_freq.QuadPart * 10 &&
        InterlockedCompareExchange64(&flush_report, t2.QuadPart, report) == report) {
        LONG64 n = InterlockedExchange64(&flush_calls, 0), us = flush_freq.QuadPart / 1000000;
        LONG64 busy = InterlockedExchange64(&flush_busy, 0), busy_max = InterlockedExchange64(&flush_busy_max, 0);
        LONG64 wait = InterlockedExchange64(&flush_wait, 0), wait_max = InterlockedExchange64(&flush_wait_max, 0);
        if (n && us)
            logmsg("flush: %lld calls in 10 s, call avg %lld us max %lld us, lock wait avg %lld us max %lld us",
                   n, busy / n / us, busy_max / us, wait / n / us, wait_max / us);
    }
}

/* Wine 11's x86_64 syscall stub; byte 4 (the syscall number) may differ:
 *   mov r10,rcx; mov eax,N; test byte [0x7ffe0308],1; jne 1f; syscall; ret
 *   1: jmp 2f; ret; 2: call [0x7ffe1000]; ret
 * It holds no rip-relative operand, so a copy runs anywhere. */
static const uint8_t nt_stub[32] = {
    0x4c, 0x8b, 0xd1, 0xb8, 0x00, 0x00, 0x00, 0x00, 0xf6, 0x04, 0x25, 0x08, 0x03, 0xfe, 0x7f, 0x01,
    0x75, 0x03, 0x0f, 0x05, 0xc3, 0xeb, 0x01, 0xc3, 0xff, 0x14, 0x25, 0x00, 0x10, 0xfe, 0x7f, 0xc3,
};

static BOOL hook_flush_ntdll(void)
{
    uint8_t *fn = (uint8_t *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtFlushProcessWriteBuffers");
    uint8_t *copy, jump[12] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};   /* mov rax, imm64; jmp rax */
    uintptr_t target = (uintptr_t)hk_flush;
    DWORD old;
    if (!fn) return FALSE;
    for (int i = 0; i < 32; i++)
        if (i != 4 && fn[i] != nt_stub[i]) {
            logmsg("FlushProcessWriteBuffers: ntdll stub differs at byte %d (%02x), not hooked there", i, fn[i]);
            return FALSE;
        }
    if (!(copy = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))) return FALSE;
    memcpy(copy, fn, 32);
    real_flush = (void (WINAPI *)(void))copy;
    QueryPerformanceFrequency(&flush_freq);
    memcpy(jump + 2, &target, 8);
    if (!VirtualProtect(fn, sizeof(jump), PAGE_EXECUTE_READWRITE, &old)) return FALSE;
    memcpy(fn, jump, sizeof(jump));
    VirtualProtect(fn, sizeof(jump), old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, sizeof(jump));
    logmsg("FlushProcessWriteBuffers: ntdll's NtFlushProcessWriteBuffers %s",
           flush_skip ? "SKIPPED (diagnostic)" : flush_nolock ? "timed, not serialized" : flush_page ? "replaced by a page protection change (protect), serialized" : "serialized for every caller");
    return TRUE;
}

static BOOL hook_flush(HMODULE exe)
{
    uint8_t *base = (uint8_t *)exe;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp;
    if (!dir->VirtualAddress) return FALSE;
    real_flush = (void (WINAPI *)(void))GetProcAddress(GetModuleHandleA("kernel32.dll"), "FlushProcessWriteBuffers");
    if (!real_flush) return FALSE;
    QueryPerformanceFrequency(&flush_freq);
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); imp->Name; imp++) {
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA *slots = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            DWORD old;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            if (strcmp((const char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name,
                       "FlushProcessWriteBuffers")) continue;
            if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &old)) return FALSE;
            slots->u1.Function = (ULONG_PTR)hk_flush;
            VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), old, &old);
            logmsg("FlushProcessWriteBuffers: %s's import %s", (const char *)(base + imp->Name),
                   flush_skip ? "SKIPPED (diagnostic)" : flush_nolock ? "timed, not serialized" : flush_page ? "protect, serialized" : "serialized");
            return TRUE;
        }
    }
    return FALSE;
}

/* ---------------- virtual memory: statistics and address placement ---------------- */

/* Near busy places two threads allocate and free virtual memory while Azimuth's
 * worker waits: one sits in NtAllocateVirtualMemoryEx -> mach_vm_map, the other
 * waits for Wine's virtual memory lock in NtFreeVirtualMemory. FH2_VMSTATS=12
 * hooks NtAllocateVirtualMemoryEx and NtFreeVirtualMemory like the flush hook and
 * logs every 10 s the calls, time spent, sizes, call shapes and the callers in
 * azimuth.exe or the game that cost the most (bit 2, NtAllocateVirtualMemory,
 * hangs the start: TlsGetValue/TlsSetValue allocate under it). */
typedef LONG (WINAPI *nt_alloc_fn)(HANDLE, void **, ULONG_PTR, SIZE_T *, ULONG, ULONG);
typedef LONG (WINAPI *nt_alloc_ex_fn)(HANDLE, void **, SIZE_T *, ULONG, ULONG, void *, ULONG);
typedef LONG (WINAPI *nt_free_fn)(HANDLE, void **, SIZE_T *, ULONG);
static nt_alloc_fn real_nt_alloc;
static nt_alloc_ex_fn real_nt_alloc_ex;
static nt_free_fn real_nt_free;
static DWORD vm_tls = TLS_OUT_OF_INDEXES;
static SRWLOCK vm_lock = SRWLOCK_INIT;
enum { VM_ALLOC, VM_FREE, VM_OPS };
static const char *const vm_op_name[VM_OPS] = {"alloc", "free"};
static struct { LONG64 n, ticks, max, bytes, big; } vm_op[VM_OPS];
#define VM_CALLERS 512
/* per caller and call shape: sig = type | protect << 32, shape = log2(size) | base given << 8 | ext params << 9 */
typedef struct { void *addr; int op; ULONG64 sig; UINT shape; LONG64 n, ticks, bytes; } vm_caller;
static vm_caller vm_callers[VM_CALLERS];
static BOOL place_on;   /* on unless FH2_PLACE=0, see place_alloc */
static volatile LONG64 place_hits, place_misses, place_fallbacks;

/* Modules the unwind walks through, with their .pdata read once: the loader's
 * own lookup takes the loader lock, which the callers may hold with the heap
 * lock. 0-2 system DLLs, 3 this layer, 4 azimuth.exe, 5 the game (set later). */
extern IMAGE_DOS_HEADER __ImageBase;
typedef struct { uintptr_t base, end; const RUNTIME_FUNCTION *rf; DWORD n; } vm_module;
static vm_module vm_mods[6];

static void vm_module_init(vm_module *m, uint8_t *b, SIZE_T size)
{
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY *d = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    m->rf = (const RUNTIME_FUNCTION *)(b + d->VirtualAddress);
    m->n = d->Size / sizeof(RUNTIME_FUNCTION);
    m->end = (uintptr_t)b + (size ? size : nt->OptionalHeader.SizeOfImage);
    m->base = (uintptr_t)b;
}

static const RUNTIME_FUNCTION *vm_lookup(const vm_module *m, uintptr_t pc)
{
    DWORD rva = (DWORD)(pc - m->base), lo = 0, hi = m->n;
    while (lo < hi) {
        DWORD mid = (lo + hi) / 2;
        if (rva < m->rf[mid].BeginAddress) hi = mid;
        else if (rva >= m->rf[mid].EndAddress) lo = mid + 1;
        else return &m->rf[mid];
    }
    return NULL;
}

/* the first return address in azimuth.exe or the game, by a real unwind */
static void *vm_find_caller(void)
{
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    for (int depth = 0; depth < 24; depth++) {
        const vm_module *m = NULL;
        const RUNTIME_FUNCTION *fe;
        for (int i = 0; i < 6; i++)
            if (vm_mods[i].base && ctx.Rip >= vm_mods[i].base && ctx.Rip < vm_mods[i].end) m = &vm_mods[i];
        if (!m) return NULL;
        if (depth && m >= &vm_mods[4]) return (void *)ctx.Rip;
        if ((fe = vm_lookup(m, ctx.Rip))) {
            void *hd;
            ULONG64 frame;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, m->base, ctx.Rip, (PRUNTIME_FUNCTION)fe, &ctx, &hd, &frame, NULL);
        } else {
            ctx.Rip = *(DWORD64 *)ctx.Rsp;   /* leaf function */
            ctx.Rsp += 8;
        }
        if (!ctx.Rip) return NULL;
    }
    return NULL;
}

static void vm_record(int op, LONG64 ticks, SIZE_T bytes, void *caller, ULONG64 sig, UINT shape)
{
    AcquireSRWLockExclusive(&vm_lock);
    vm_op[op].n++; vm_op[op].ticks += ticks; vm_op[op].bytes += bytes;
    if (ticks > vm_op[op].max) vm_op[op].max = ticks;
    if (ticks * 1000 > flush_freq.QuadPart) vm_op[op].big++;   /* over 1 ms */
    for (UINT i = 0, h = (UINT)((((uintptr_t)caller >> 2) ^ sig ^ shape) * 2654435761u + op) % VM_CALLERS; i < VM_CALLERS;
         i++, h = (h + 1) % VM_CALLERS) {
        if (vm_callers[h].n && (vm_callers[h].addr != caller || vm_callers[h].op != op || vm_callers[h].sig != sig ||
                                vm_callers[h].shape != shape)) continue;
        vm_callers[h].addr = caller; vm_callers[h].op = op; vm_callers[h].sig = sig; vm_callers[h].shape = shape;
        vm_callers[h].n++; vm_callers[h].ticks += ticks; vm_callers[h].bytes += bytes;
        break;
    }
    ReleaseSRWLockExclusive(&vm_lock);
}

static void vm_report_once(void)
{
    LONG64 us = flush_freq.QuadPart / 1000000;
    static vm_caller top[VM_CALLERS];
    UINT ntop = 0;
    struct { LONG64 n, ticks, max, bytes, big; } ops[VM_OPS];
    /* copy under the lock, log after: logging takes the heap lock, which a
     * thread waiting for vm_lock inside an allocation may hold */
    AcquireSRWLockExclusive(&vm_lock);
    memcpy(ops, vm_op, sizeof(ops));
    memset(vm_op, 0, sizeof(vm_op));
    for (UINT i = 0; i < VM_CALLERS; i++) if (vm_callers[i].n) memcpy(&top[ntop++], &vm_callers[i], sizeof(top[0]));
    memset(vm_callers, 0, sizeof(vm_callers));
    ReleaseSRWLockExclusive(&vm_lock);
    if (place_on) {
        LONG64 hits = InterlockedExchange64(&place_hits, 0), misses = InterlockedExchange64(&place_misses, 0);
        LONG64 fallbacks = InterlockedExchange64(&place_fallbacks, 0);
        if (hits || misses || fallbacks)
            logmsg("vm: placed %lld at the first try, %lld after a taken address, %lld left to Wine", hits, misses, fallbacks);
    }
    for (int o = 0; o < VM_OPS; o++)
        if (ops[o].n && us)
            logmsg("vm: %s %lld calls in 10 s, %lld ms in all, max %lld us, %lld over 1 ms, %lld MiB", vm_op_name[o],
                   ops[o].n, ops[o].ticks / us / 1000, ops[o].max / us, ops[o].big, ops[o].bytes >> 20);
    for (int k = 0; k < 8 && ntop; k++) {
        UINT best = 0;
        char name[MAX_PATH] = "?";
        HMODULE m = NULL;
        for (UINT i = 1; i < ntop; i++) if (top[i].ticks > top[best].ticks) best = i;
        for (int c = 4; c < 6; c++)
            if ((uintptr_t)top[best].addr >= vm_mods[c].base && (uintptr_t)top[best].addr < vm_mods[c].end) {
                m = (HMODULE)vm_mods[c].base;
                strcpy(name, c == 5 ? "game" : "azimuth.exe");
            }
        logmsg("vm:   %s from %s+%#llx type %#lx prot %#lx size ~2^%u%s%s: %lld calls, %lld ms, %lld KiB", vm_op_name[top[best].op], name,
               (unsigned long long)((uintptr_t)top[best].addr - (uintptr_t)m), (ULONG)top[best].sig, (ULONG)(top[best].sig >> 32),
               top[best].shape & 0xff, top[best].shape & 0x100 ? " base given" : "", top[best].shape & 0x200 ? " ext params" : "",
               top[best].n, us ? top[best].ticks / us / 1000 : 0, top[best].bytes >> 10);
        top[best] = top[--ntop];
    }
}

/* the thread's own logging and lookups allocate too: those calls are not counted */
static UINT vm_shape(SIZE_T size, void **base, BOOL ext)
{
    UINT l = 0;
    while (l < 63 && ((SIZE_T)1 << (l + 1)) <= size) l++;
    return l | (base && *base ? 0x100 : 0) | (ext ? 0x200 : 0);
}

#define VM_WRAP(op, bytes, sig, shape, call) \
    LARGE_INTEGER t0, t1; LONG r; UINT sh; \
    if (TlsGetValue(vm_tls)) return call; \
    TlsSetValue(vm_tls, (void *)1); \
    sh = (shape); \
    QueryPerformanceCounter(&t0); r = call; QueryPerformanceCounter(&t1); \
    vm_record(op, t1.QuadPart - t0.QuadPart, (bytes), vm_find_caller(), (sig), sh); \
    TlsSetValue(vm_tls, NULL); \
    return r;

static LONG WINAPI hk_nt_alloc(HANDLE p, void **base, ULONG_PTR zero, SIZE_T *size, ULONG type, ULONG prot)
{
    VM_WRAP(VM_ALLOC, size ? *size : 0, type | (ULONG64)prot << 32, vm_shape(size ? *size : 0, base, FALSE),
            real_nt_alloc(p, base, zero, size, type, prot))
}

/* ---- address placement (on by default, FH2_PLACE=0 turns it off) ----
 * Azimuth asks for its placeholders and write-watched blocks with an address
 * requirement (4 GB - 1 TB, or 2 - 4 TB, 64 KB aligned) and no address. Wine
 * looks for room from the bottom of the range, probing every 64 KB with
 * mach_vm_map, and right above 4 GB macOS has Wine, Rosetta, D3DMetal and Metal
 * mapped: thousands of failing probes, 40-170 ms per call under Rosetta, which
 * is the stutter near busy places. The layer picks the address instead, from a
 * cursor that starts in the middle of the range, and asks for exactly that
 * address (one probe). An address that is taken moves the cursor on; after a few
 * misses the call goes to Wine unchanged, so it is never worse than before. */
typedef struct { ULONG64 low, high, start; volatile LONG64 cursor; } place_range;
static place_range place_ranges[8];
static volatile LONG nplace;
static SRWLOCK place_lock = SRWLOCK_INIT;
static place_range *place_find(ULONG64 low, ULONG64 high)
{
    place_range *r = NULL;
    for (LONG i = 0; i < nplace; i++)
        if (place_ranges[i].low == low && place_ranges[i].high == high) return &place_ranges[i];
    AcquireSRWLockExclusive(&place_lock);
    for (LONG i = 0; i < nplace && !r; i++)
        if (place_ranges[i].low == low && place_ranges[i].high == high) r = &place_ranges[i];
    if (!r && nplace < 8) {
        r = &place_ranges[nplace];
        r->low = low; r->high = high;
        r->start = (low + (high - low) / 2 + 0x3fffffff) & ~(ULONG64)0x3fffffff;   /* mid-range, 1 GB aligned */
        r->cursor = (LONG64)r->start;
        MemoryBarrier();
        nplace++;
    }
    ReleaseSRWLockExclusive(&place_lock);
    return r;
}

/* STATUS_SUCCESS or the failure; *done FALSE when the call is not one to place */
static LONG place_alloc(HANDLE p, void **base, SIZE_T *size, ULONG type, ULONG prot,
                        const MEM_EXTENDED_PARAMETER *params, ULONG count, BOOL *done)
{
    MEM_EXTENDED_PARAMETER rest[8];
    const MEM_ADDRESS_REQUIREMENTS *req = NULL;
    ULONG nrest = 0;
    ULONG64 align, sz;
    place_range *r;
    LONG st = -1;
    *done = FALSE;
    if (!place_on || !base || *base || !size || !*size || !params || !count || count > 8 ||
        p != GetCurrentProcess() || !(type & MEM_RESERVE)) return 0;
    for (ULONG i = 0; i < count; i++) {
        if (params[i].Type == MemExtendedParameterAddressRequirements) req = params[i].Pointer;
        else rest[nrest++] = params[i];
    }
    if (!req || !req->HighestEndingAddress) return 0;
    align = req->Alignment > 0x10000 ? req->Alignment : 0x10000;
    sz = (*size + align - 1) & ~(align - 1);
    if (!(r = place_find((ULONG64)req->LowestStartingAddress, (ULONG64)req->HighestEndingAddress))) return 0;
    for (int tries = 0; tries < 6; tries++) {
        ULONG64 a = (ULONG64)InterlockedExchangeAdd64(&r->cursor, (LONG64)sz);
        void *b;
        SIZE_T s2 = *size;
        a = (a + align - 1) & ~(align - 1);
        if (a + sz - 1 > r->high) {   /* the end of the range: start over from the middle */
            InterlockedCompareExchange64(&r->cursor, (LONG64)r->start, (LONG64)(a + sz));
            continue;
        }
        b = (void *)a;
        st = real_nt_alloc_ex(p, &b, &s2, type, prot, nrest ? rest : NULL, nrest);
        if (st >= 0) {
            *base = b;
            *size = s2;
            *done = TRUE;
            InterlockedIncrement64(tries ? &place_misses : &place_hits);
            return st;
        }
        InterlockedExchangeAdd64(&r->cursor, 16 << 20);   /* taken: skip ahead */
    }
    InterlockedIncrement64(&place_fallbacks);
    return st;
}

static LONG alloc_ex_call(HANDLE p, void **base, SIZE_T *size, ULONG type, ULONG prot, void *params, ULONG count)
{
    BOOL done;
    LONG st = place_alloc(p, base, size, type, prot, params, count, &done);
    return done ? st : real_nt_alloc_ex(p, base, size, type, prot, params, count);
}

static BOOL vm_stats_on;

static LONG WINAPI hk_nt_alloc_ex(HANDLE p, void **base, SIZE_T *size, ULONG type, ULONG prot, void *params, ULONG count)
{
    if (!vm_stats_on) return alloc_ex_call(p, base, size, type, prot, params, count);
    VM_WRAP(VM_ALLOC, size ? *size : 0, type | (ULONG64)prot << 32, vm_shape(size ? *size : 0, base, count != 0),
            alloc_ex_call(p, base, size, type, prot, params, count))
}

static LONG WINAPI hk_nt_free(HANDLE p, void **base, SIZE_T *size, ULONG type)
{
    VM_WRAP(VM_FREE, size ? *size : 0, type, vm_shape(size ? *size : 0, NULL, FALSE), real_nt_free(p, base, size, type))
}

/* same inline hook as the flush one: copies the 32-byte syscall stub out as the real call */
static void *hook_nt_stub(const char *name, void *hook)
{
    uint8_t *fn = (uint8_t *)GetProcAddress(GetModuleHandleA("ntdll.dll"), name);
    uint8_t *copy, jump[12] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
    uintptr_t target = (uintptr_t)hook;
    DWORD old;
    if (!fn) return NULL;
    for (int i = 0; i < 32; i++)
        if (i != 4 && i != 5 && fn[i] != nt_stub[i]) {
            logmsg("vm: %s stub differs at byte %d, not hooked", name, i);
            return NULL;
        }
    if (!(copy = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))) return NULL;
    memcpy(copy, fn, 32);
    memcpy(jump + 2, &target, 8);
    if (!VirtualProtect(fn, sizeof(jump), PAGE_EXECUTE_READWRITE, &old)) return NULL;
    memcpy(fn, jump, sizeof(jump));
    VirtualProtect(fn, sizeof(jump), old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, sizeof(jump));
    return copy;
}

/* reports from its own thread: logging allocates and takes the CRT and log locks */
static DWORD WINAPI vm_report_thread(void *arg)
{
    (void)arg;
    TlsSetValue(vm_tls, (void *)1);   /* its own allocations are not counted */
    for (;;) {
        Sleep(10000);
        if (!vm_mods[5].base) {
            const struct game_image *which = NULL;
            uint8_t *g = find_game_image(&which);
            if (g) {
                vm_module tmp;
                vm_module_init(&tmp, g, which->size);
                vm_mods[5].rf = tmp.rf; vm_mods[5].n = tmp.n; vm_mods[5].end = tmp.end;
                MemoryBarrier();
                vm_mods[5].base = tmp.base;
            }
        }
        vm_report_once();
    }
    return 0;
}

/* mask: as FH2_VMSTATS (0: no statistics); place: address placement */
static void hook_vm(int mask, BOOL place)
{
    vm_tls = TlsAlloc();
    vm_stats_on = mask > 0;
    place_on = place;
    {
        static const char *const names[] = {"ntdll.dll", "kernelbase.dll", "kernel32.dll"};
        for (int i = 0; i < 3; i++) if (GetModuleHandleA(names[i])) vm_module_init(&vm_mods[i], (uint8_t *)GetModuleHandleA(names[i]), 0);
        volatile uintptr_t self = (uintptr_t)&__ImageBase;   /* volatile: the header is read past the symbol */
        vm_module_init(&vm_mods[3], (uint8_t *)self, 0);
        vm_module_init(&vm_mods[4], (uint8_t *)GetModuleHandleA("azimuth.exe"), 0);
    }
    QueryPerformanceFrequency(&flush_freq);
    /* mask: 1 NtAllocateVirtualMemory, 2 NtAllocateVirtualMemoryEx, 4 NtFreeVirtualMemory ("1" alone = all) */
    if (mask == 1) mask = 7; else mask >>= 1;
    if (place) mask |= 2;
    if (mask & 2) real_nt_alloc_ex = hook_nt_stub("NtAllocateVirtualMemoryEx", hk_nt_alloc_ex);
    if (mask & 4) real_nt_free = hook_nt_stub("NtFreeVirtualMemory", hk_nt_free);
    if (mask & 1) real_nt_alloc = hook_nt_stub("NtAllocateVirtualMemory", hk_nt_alloc);
    if (place && !real_nt_alloc_ex) place_on = FALSE;
    logmsg("vm: statistics %s, address placement %s (alloc %p, alloc_ex %p, free %p)", vm_stats_on ? "on" : "off",
           place_on ? "on" : "off", (void *)real_nt_alloc, (void *)real_nt_alloc_ex, (void *)real_nt_free);
    CloseHandle(CreateThread(NULL, 0, vm_report_thread, NULL, 0, NULL));
}

/* ---------------- exports ---------------- */

/* Exports this layer does not need to see are forwarded with a jump through a
 * pointer filled from the real d3d12.dll in DllMain, so their signatures do not
 * matter. Ordinals match the real DLL (100-110). */
#define FORWARD_EXPORT(name) \
    void *real_##name; \
    __asm__(".globl fwd_" #name "\n fwd_" #name ": jmp *real_" #name "(%rip)\n");
FORWARD_EXPORT(GetBehaviorValue)
FORWARD_EXPORT(D3D12CoreCreateLayeredDevice)
FORWARD_EXPORT(D3D12CoreGetLayeredDeviceSize)
FORWARD_EXPORT(D3D12CoreRegisterLayers)
FORWARD_EXPORT(D3D12CreateRootSignatureDeserializer)
FORWARD_EXPORT(D3D12CreateVersionedRootSignatureDeserializer)
FORWARD_EXPORT(D3D12EnableExperimentalFeatures)
FORWARD_EXPORT(D3D12SerializeVersionedRootSignature)

static BOOL resolve_passthrough(HMODULE real)
{
    static const struct { const char *name; void **slot; } fwd[] = {
        {"GetBehaviorValue", &real_GetBehaviorValue},
        {"D3D12CoreCreateLayeredDevice", &real_D3D12CoreCreateLayeredDevice},
        {"D3D12CoreGetLayeredDeviceSize", &real_D3D12CoreGetLayeredDeviceSize},
        {"D3D12CoreRegisterLayers", &real_D3D12CoreRegisterLayers},
        {"D3D12CreateRootSignatureDeserializer", &real_D3D12CreateRootSignatureDeserializer},
        {"D3D12CreateVersionedRootSignatureDeserializer", &real_D3D12CreateVersionedRootSignatureDeserializer},
        {"D3D12EnableExperimentalFeatures", &real_D3D12EnableExperimentalFeatures},
        {"D3D12SerializeVersionedRootSignature", &real_D3D12SerializeVersionedRootSignature},
    };
    BOOL ok = TRUE;
    for (size_t i = 0; i < sizeof(fwd) / sizeof(fwd[0]); i++)
        if (!(*fwd[i].slot = (void *)GetProcAddress(real, fwd[i].name))) {
            logmsg("real d3d12.dll lacks %s", fwd[i].name);
            ok = FALSE;
        }
    return ok;
}

HRESULT WINAPI proxy_create_device(IUnknown *adapter, D3D_FEATURE_LEVEL level, REFIID riid, void **ppv)
{
    HRESULT hr = real_create_device(adapter, level, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) {
        if (minimal) logmsg("minimal mode: the real device is used as it is");
        else hook_device(*ppv);
    }
    return hr;
}

HRESULT WINAPI proxy_get_debug_interface(REFIID riid, void **ppv)
{
    return real_get_debug_interface(riid, ppv);
}

HRESULT WINAPI proxy_serialize_root_signature(const D3D12_ROOT_SIGNATURE_DESC *desc, D3D_ROOT_SIGNATURE_VERSION v,
                                              ID3DBlob **blob, ID3DBlob **err)
{
    return real_serialize_rs(desc, v, blob, err);
}

/* SV_VertexID / SV_InstanceID as seen by a vertex shader, direct and indirect draws.
 * D3D: InstanceID excludes StartInstanceLocation, VertexID includes StartVertexLocation. */
typedef HRESULT (STDMETHODCALLTYPE *create_signature_fn2)(ID3D12Device *, const D3D12_COMMAND_SIGNATURE_DESC *,
                                                          ID3D12RootSignature *, REFIID, void **);
static void gfx_test(ID3D12Device *dev)
{
    static const char vs_src[] =
        "RWStructuredBuffer<uint> b : register(u0); cbuffer c : register(b0) { uint base; };\n"
        "float4 main(uint vid : SV_VertexID, uint iid : SV_InstanceID) : SV_Position {\n"
        "  b[base + (iid & 15)] = (vid << 16) | iid; return float4(2, 2, 2, 1); }\n";
    static const char ps_src[] = "float4 main() : SV_Target { return 0; }\n";
    HMODULE comp = LoadLibraryA("d3dcompiler_47.dll");
    d3dcompile_fn compile = comp ? (d3dcompile_fn)GetProcAddress(comp, "D3DCompile") : NULL;
    D3D12_HEAP_PROPERTIES custom = {D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0, 0, 0};
    D3D12_HEAP_PROPERTIES def = {D3D12_HEAP_TYPE_DEFAULT, 0, 0, 0, 0};
    D3D12_RESOURCE_DESC bd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 0x10000, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                              D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC rh = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    D3D12_ROOT_PARAMETER rp[2];
    D3D12_ROOT_SIGNATURE_DESC rsd;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gd;
    D3D12_INDIRECT_ARGUMENT_DESC iad = {D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
    D3D12_COMMAND_SIGNATURE_DESC sd = {16, 1, &iad, 0};
    D3D12_VIEWPORT vp = {0, 0, 4, 4, 0, 1};
    D3D12_RECT sc = {0, 0, 4, 4};
    ID3DBlob *vs = NULL, *ps = NULL, *err = NULL, *rsb = NULL;
    ID3D12RootSignature *rs = NULL;
    ID3D12PipelineState *pso = NULL;
    ID3D12CommandSignature *sig = NULL;
    ID3D12CommandQueue *q = NULL;
    ID3D12CommandAllocator *a = NULL;
    ID3D12GraphicsCommandList *l = NULL;
    ID3D12Resource *buf = NULL, *rt = NULL;
    ID3D12DescriptorHeap *rtvh = NULL;
    ID3D12Fence *f = NULL;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    uint32_t *m = NULL;
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    HRESULT hr;

    if (!compile) return;
    hr = compile(vs_src, sizeof(vs_src) - 1, "gfxvs", NULL, NULL, "main", "vs_5_0", 0, 0, &vs, &err);
    if (FAILED(hr)) { logmsg("gfx-test: vs compile failed %s", err ? (char *)ID3D10Blob_GetBufferPointer(err) : ""); goto done; }
    hr = compile(ps_src, sizeof(ps_src) - 1, "gfxps", NULL, NULL, "main", "ps_5_0", 0, 0, &ps, NULL);
    memset(rp, 0, sizeof(rp));
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.Num32BitValues = 1;
    memset(&rsd, 0, sizeof(rsd));
    rsd.NumParameters = 2;
    rsd.pParameters = rp;
    if (SUCCEEDED(hr)) hr = real_serialize_rs(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, NULL);
    if (SUCCEEDED(hr)) hr = ((create_rs_fn)dev_vt_orig[16])(dev, 0, ID3D10Blob_GetBufferPointer(rsb), ID3D10Blob_GetBufferSize(rsb),
                                                           &IID_ID3D12RootSignature, (void **)&rs);
    memset(&gd, 0, sizeof(gd));
    gd.pRootSignature = rs;
    gd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs);
    gd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
    gd.PS.pShaderBytecode = ps ? ID3D10Blob_GetBufferPointer(ps) : NULL;
    gd.PS.BytecodeLength = ps ? ID3D10Blob_GetBufferSize(ps) : 0;
    gd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gd.SampleMask = ~0u;
    gd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gd.RasterizerState.DepthClipEnable = TRUE;
    gd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    gd.NumRenderTargets = 1;
    gd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    gd.SampleDesc.Count = 1;
    if (SUCCEEDED(hr)) hr = ((create_gpso_fn)dev_vt_orig[10])(dev, &gd, &IID_ID3D12PipelineState, (void **)&pso);
    if (SUCCEEDED(hr)) hr = ((create_signature_fn2)dev_vt_orig[41])(dev, &sd, NULL, &IID_ID3D12CommandSignature, (void **)&sig);
    if (SUCCEEDED(hr)) hr = ((create_queue_fn)dev_vt_orig[8])(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&a);
    if (SUCCEEDED(hr)) hr = ((create_list_fn)dev_vt_orig[12])(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, pso,
                                                             &IID_ID3D12GraphicsCommandList, (void **)&l);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommittedResource(dev, &custom, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                NULL, &IID_ID3D12Resource, (void **)&buf);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommittedResource(dev, &def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                                NULL, &IID_ID3D12Resource, (void **)&rt);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateDescriptorHeap(dev, &rh, &IID_ID3D12DescriptorHeap, (void **)&rtvh);
    if (SUCCEEDED(hr)) hr = ((create_fence_fn)dev_vt_orig[36])(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&f);
    if (SUCCEEDED(hr)) hr = ID3D12Resource_Map(buf, 0, NULL, (void **)&m);
    if (FAILED(hr)) { logmsg("gfx-test: setup failed %#lx", hr); goto done; }

    rtvh->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtvh, &rtv);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);
    memset(m, 0xff, 0x400);
    /* indirect args at 0x800: Draw(1 vertex, 2 instances, start vertex 7, start instance 5) */
    m[0x200] = 1; m[0x201] = 2; m[0x202] = 7; m[0x203] = 5;
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(l, rs);
    ID3D12GraphicsCommandList_SetGraphicsRootUnorderedAccessView(l, 0, ID3D12Resource_GetGPUVirtualAddress(buf));
    ID3D12GraphicsCommandList_OMSetRenderTargets(l, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_RSSetViewports(l, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(l, D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstant(l, 1, 16, 0);     /* direct draw -> b[16..31] */
    ID3D12GraphicsCommandList_DrawInstanced(l, 1, 2, 7, 5);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstant(l, 1, 48, 0);     /* indirect draw -> b[48..63] */
    ID3D12GraphicsCommandList_ExecuteIndirect(l, sig, 1, buf, 0x800, NULL, 0);
    {
        /* indexed: indices {2} at 0xc00, BaseVertexLocation 3 -> D3D VertexID 5 */
        D3D12_INDEX_BUFFER_VIEW ib = {ID3D12Resource_GetGPUVirtualAddress(buf) + 0xc00, 16, DXGI_FORMAT_R32_UINT};
        m[0x300] = 2;
        ID3D12GraphicsCommandList_IASetIndexBuffer(l, &ib);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstant(l, 1, 80, 0);  /* indexed draw -> b[80..95] */
        ID3D12GraphicsCommandList_DrawIndexedInstanced(l, 1, 2, 0, 3, 5);
    }
    ID3D12GraphicsCommandList_Close(l);
    ((execute_fn)queue_vt_orig[10])(q, 1, (ID3D12CommandList *const *)&l);
    ((signal_fn)queue_vt_orig[14])(q, f, 1);
    ID3D12Fence_SetEventOnCompletion(f, 1, ev);
    WaitForSingleObject(ev, 5000);
    for (int base = 16; base <= 80; base += 32) {
        char line[256] = "";
        for (int i = 0; i < 8; i++)
            if (m[base + i] != 0xffffffff)
                sprintf(line + strlen(line), " [%d]=vid %u iid %u", i, m[base + i] >> 16, m[base + i] & 0xffff);
        logmsg("gfx-test %s draw: slots written:%s  (D3D wants [0]=vid %d iid 0 [1]=vid %d iid 1)",
               base == 16 ? "direct" : base == 48 ? "indirect" : "indexed base 3", line[0] ? line : " none",
               base == 80 ? 5 : 7, base == 80 ? 5 : 7);
    }
done:
    if (f) ID3D12Fence_Release(f);
    if (rtvh) ID3D12DescriptorHeap_Release(rtvh);
    if (rt) ID3D12Resource_Release(rt);
    if (buf) ID3D12Resource_Release(buf);
    if (l) ID3D12GraphicsCommandList_Release(l);
    if (a) ID3D12CommandAllocator_Release(a);
    if (q) ID3D12CommandQueue_Release(q);
    if (sig) ID3D12CommandSignature_Release(sig);
    if (pso) ID3D12PipelineState_Release(pso);
    if (rs) ID3D12RootSignature_Release(rs);
    if (rsb) ID3D10Blob_Release(rsb);
    if (vs) ID3D10Blob_Release(vs);
    if (ps) ID3D10Blob_Release(ps);
    if (err) ID3D10Blob_Release(err);
    CloseHandle(ev);
}

/* per-instance input layouts, the shape the world geometry actually uses:
 * FH2 feeds instance placement through I_POS (R16G16B16A16_UNORM) and
 * I_XAXIS (R10G10B10A2_UNORM) on input slot 2, class PER_INSTANCE, step rate 1.
 * Any error in stepping or in unpacking these packed formats moves, scales or
 * rotates whole props -- which is what the phantom wall and the tilted trees look
 * like. This probe feeds known bytes through each format and through step rate 2
 * and checks what the vertex shader received. */
typedef struct {
    const char *name;
    const char *decl;      /* shader parameter declaration */
    const char *param;     /* shader parameter name */
    DXGI_FORMAT fmt;
    UINT step;             /* InstanceDataStepRate */
    UINT n_inst;
    UINT stride;           /* bytes per instance element */
    const void *data;      /* instance data */
    float want[4][4];      /* expected value per instance */
} inst_case;

static void inst_test(ID3D12Device *dev)
{
    static const char vs_fmt[] =
        "RWStructuredBuffer<float4> outb : register(u0);\n"
        "cbuffer c : register(b0) { uint base; };\n"
        "float4 main(%s, uint iid : SV_InstanceID) : SV_Position {\n"
        "  outb[base + iid] = %s; return float4(0, 0, 0, 1); }\n";
    static const char ps_src[] = "float4 main() : SV_Target { return 0; }\n";
    /* R32G32B32_FLOAT: 3 floats per element, two instances */
    static const float f6_data[6] = {1, 2, 3, 4, 5, 6};
    /* R16G16B16A16_UNORM: 0.0 0.25 0.5 1.0 then 1.0 0.5 0.25 0.0 */
    static const uint16_t f11_data[8] = {0, 16384, 32768, 65535, 65535, 32768, 16384, 0};
    /* R10G10B10A2_UNORM, max 1023 per channel: 256/1023 . 512/1023 . 768/1023 . A 3/3
     * then A=2/3. Expected values are the exact quantized ones, not round decimals. */
    static const uint32_t f24_data[2] = {
        256u | (512u << 10) | (768u << 20) | (3u << 30),
        1023u | (512u << 10) | (0u << 20) | (2u << 30)
    };
    /* step rate 2 over four instances: two elements, each used by two instances */
    static const float f6_step2[8] = {10, 0, 0, 0, 20, 0, 0, 0};
    static const inst_case cases[] = {
        {"f6  R32G32B32_FLOAT step 1", "float3 v : IPOS", "float4(v, 1)", DXGI_FORMAT_R32G32B32_FLOAT, 1, 2, 12, f6_data,
         {{1, 2, 3, 1}, {4, 5, 6, 1}, {0}, {0}}},
        {"f11 R16G16B16A16_UNORM step 1", "float4 v : IPOS", "v", DXGI_FORMAT_R16G16B16A16_UNORM, 1, 2, 8, f11_data,
         {{0, 0.25f, 0.5f, 1}, {1, 0.5f, 0.25f, 0}, {0}, {0}}},
        {"f24 R10G10B10A2_UNORM step 1", "float4 v : IPOS", "v", DXGI_FORMAT_R10G10B10A2_UNORM, 1, 2, 4, f24_data,
         {{256.f / 1023.f, 512.f / 1023.f, 768.f / 1023.f, 1}, {1, 512.f / 1023.f, 0, 2.f / 3.f}, {0}, {0}}},
        {"f6  R32G32B32_FLOAT step 2", "float3 v : IPOS", "float4(v, 1)", DXGI_FORMAT_R32G32B32_FLOAT, 2, 4, 16, f6_step2,
         {{10, 0, 0, 1}, {10, 0, 0, 1}, {20, 0, 0, 1}, {20, 0, 0, 1}}},
    };
    HMODULE comp = LoadLibraryA("d3dcompiler_47.dll");
    d3dcompile_fn compile = comp ? (d3dcompile_fn)GetProcAddress(comp, "D3DCompile") : NULL;
    D3D12_HEAP_PROPERTIES custom = {D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0, 0, 0};
    D3D12_HEAP_PROPERTIES def = {D3D12_HEAP_TYPE_DEFAULT, 0, 0, 0, 0};
    D3D12_RESOURCE_DESC bd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 0x10000, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                              D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC rh = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    D3D12_ROOT_PARAMETER rp[2];
    D3D12_ROOT_SIGNATURE_DESC rsd;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gd;
    D3D12_VIEWPORT vp = {0, 0, 4, 4, 0, 1};
    D3D12_RECT sc = {0, 0, 4, 4};
    ID3DBlob *vs = NULL, *ps = NULL, *err = NULL, *rsb = NULL;
    ID3D12RootSignature *rs = NULL;
    ID3D12CommandQueue *q = NULL;
    ID3D12CommandAllocator *a = NULL;
    ID3D12GraphicsCommandList *l = NULL;
    ID3D12Resource *buf = NULL, *rt = NULL;
    ID3D12DescriptorHeap *rtvh = NULL;
    ID3D12Fence *f = NULL;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    uint32_t *m = NULL;
    char src[512];
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    HRESULT hr;
    UINT i;

    if (!compile) { logmsg("inst-test: no d3dcompiler_47"); return; }
    hr = compile(ps_src, sizeof(ps_src) - 1, "instps", NULL, NULL, "main", "ps_5_0", 0, 0, &ps, NULL);
    memset(rp, 0, sizeof(rp));
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.Num32BitValues = 1;
    memset(&rsd, 0, sizeof(rsd));
    rsd.NumParameters = 2;
    rsd.pParameters = rp;
    if (SUCCEEDED(hr)) hr = real_serialize_rs(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, NULL);
    if (SUCCEEDED(hr)) hr = ((create_rs_fn)dev_vt_orig[16])(dev, 0, ID3D10Blob_GetBufferPointer(rsb),
                                                           ID3D10Blob_GetBufferSize(rsb), &IID_ID3D12RootSignature, (void **)&rs);
    if (SUCCEEDED(hr)) hr = ((create_queue_fn)dev_vt_orig[8])(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&a);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommittedResource(dev, &custom, D3D12_HEAP_FLAG_NONE, &bd,
                                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
                                                                &IID_ID3D12Resource, (void **)&buf);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommittedResource(dev, &def, D3D12_HEAP_FLAG_NONE, &td,
                                                                D3D12_RESOURCE_STATE_RENDER_TARGET, NULL,
                                                                &IID_ID3D12Resource, (void **)&rt);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateDescriptorHeap(dev, &rh, &IID_ID3D12DescriptorHeap, (void **)&rtvh);
    if (SUCCEEDED(hr)) hr = ((create_fence_fn)dev_vt_orig[36])(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&f);
    if (SUCCEEDED(hr)) hr = ID3D12Resource_Map(buf, 0, NULL, (void **)&m);
    if (FAILED(hr)) { logmsg("inst-test: setup failed %#lx", hr); goto done; }

    rtvh->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtvh, &rtv);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const inst_case *c = &cases[i];
        UINT b;
        D3D12_INPUT_ELEMENT_DESC el = {"IPOS", 0, c->fmt, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, c->step};
        D3D12_VERTEX_BUFFER_VIEW vb[2];
        UINT64 step_bytes = (UINT64)c->stride * (c->step ? c->step : 1);
        vs = err = NULL;
        memset((uint8_t *)m + 0x3000, 0, 0x400);   /* clear the readback area first */
        snprintf(src, sizeof(src), vs_fmt, c->decl, c->param);
        if (FAILED(compile(src, strlen(src), "instvs", NULL, NULL, "main", "vs_5_0", 0, 0, &vs, &err))) {
            logmsg("inst-test %s: compile failed %s", c->name, err ? (char *)ID3D10Blob_GetBufferPointer(err) : "");
            continue;
        }
        memset(&gd, 0, sizeof(gd));
        gd.pRootSignature = rs;
        gd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs);
        gd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
        gd.PS.pShaderBytecode = ps ? ID3D10Blob_GetBufferPointer(ps) : NULL;
        gd.PS.BytecodeLength = ps ? ID3D10Blob_GetBufferSize(ps) : 0;
        gd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        gd.SampleMask = ~0u;
        gd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        gd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        gd.RasterizerState.DepthClipEnable = TRUE;
        gd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        gd.NumRenderTargets = 1;
        gd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        gd.SampleDesc.Count = 1;
        gd.InputLayout.NumElements = 1;
        gd.InputLayout.pInputElementDescs = &el;
        {
            ID3D12PipelineState *pso = NULL;
            if (FAILED(((create_gpso_fn)dev_vt_orig[10])(dev, &gd, &IID_ID3D12PipelineState, (void **)&pso))) {
                logmsg("inst-test %s: PSO failed", c->name);
                ID3D10Blob_Release(vs);
                continue;
            }
            /* instance data lives at offset 0x1000 of the same mapped buffer */
            memcpy((uint8_t *)m + 0x1000, c->data, (size_t)c->stride * c->n_inst);
            memset((uint8_t *)m + 0x2000, 0xff, 0x100);
            vb[0].BufferLocation = ID3D12Resource_GetGPUVirtualAddress(buf) + 0x1000;
            vb[0].SizeInBytes = c->n_inst * (UINT)step_bytes;
            vb[0].StrideInBytes = c->stride;
            ((create_list_fn)dev_vt_orig[12])(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, pso,
                                             &IID_ID3D12GraphicsCommandList, (void **)&l);
            ID3D12GraphicsCommandList_Reset(l, a, pso);
            ID3D12GraphicsCommandList_SetGraphicsRootSignature(l, rs);
            ID3D12GraphicsCommandList_SetGraphicsRootUnorderedAccessView(l, 0, ID3D12Resource_GetGPUVirtualAddress(buf) + 0x3000);
            ID3D12GraphicsCommandList_OMSetRenderTargets(l, 1, &rtv, FALSE, NULL);
            ID3D12GraphicsCommandList_RSSetViewports(l, 1, &vp);
            ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
            ID3D12GraphicsCommandList_IASetPrimitiveTopology(l, D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
            ID3D12GraphicsCommandList_IASetVertexBuffers(l, 1, 1, &vb[0]);
            ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstant(l, 1, 0, 0);
            ID3D12GraphicsCommandList_DrawInstanced(l, 1, c->n_inst, 0, 0);
            ID3D12GraphicsCommandList_Close(l);
            ((execute_fn)queue_vt_orig[10])(q, 1, (ID3D12CommandList *const *)&l);
            /* a fresh fence value each iteration: reusing 1 makes the wait return
             * before this iteration's GPU work has run and reads the previous one */
            ((signal_fn)queue_vt_orig[14])(q, f, i + 1);
            ID3D12Fence_SetEventOnCompletion(f, i + 1, ev);
            WaitForSingleObject(ev, 5000);
            {
                /* read back through the CPU mapping of our own CUSTOM heap */
                uint8_t *out = (uint8_t *)m + 0x3000;
                char line[256] = "";
                for (b = 0; b < c->n_inst; b++) {
                    float *got = (float *)(out + 16 * b);
                    BOOL ok = 1;
                    for (int k = 0; k < 4; k++) {
                        float d = got[k] - c->want[b][k];
                        if (d < 0) d = -d;
                        if (d > 0.002f) ok = 0;
                    }
                    sprintf(line + strlen(line), " [%u] %g %g %g %g %s", b, got[0], got[1], got[2], got[3], ok ? "ok" : "BAD");
                }
                logmsg("inst-test %s:%s", c->name, line);
            }
            ID3D12GraphicsCommandList_Release(l);
            l = NULL;
            ID3D12PipelineState_Release(pso);
        }
        ID3D10Blob_Release(vs);
        vs = NULL;
    }
done:
    if (buf) ID3D12Resource_Release(buf);
    if (f) ID3D12Fence_Release(f);
    if (rtvh) ID3D12DescriptorHeap_Release(rtvh);
    if (rt) ID3D12Resource_Release(rt);
    if (l) ID3D12GraphicsCommandList_Release(l);
    if (a) ID3D12CommandAllocator_Release(a);
    if (q) ID3D12CommandQueue_Release(q);
    if (rs) ID3D12RootSignature_Release(rs);
    if (rsb) ID3D10Blob_Release(rsb);
    if (ps) ID3D10Blob_Release(ps);
    if (err) ID3D10Blob_Release(err);
    CloseHandle(ev);
}

/* The world layouts place several per-instance elements on one slot with
 * AlignedByteOffset = APPEND_ALIGNED_ELEMENT (the 4294967295 in the census):
 *   I_POS    R16G16B16A16_UNORM  8 bytes
 *   I_UVSCALE R8G8B8A8_UNORM     4 bytes
 *   I_XAXIS  R10G10B10A2_UNORM   4 bytes
 * so the packed stride is 16 and the offsets must resolve to 0, 8 and 12. If the
 * translator resolves them differently the position scale and the axis vector come
 * from the wrong bytes, which is exactly how a prop ends up huge or rotated. */
typedef struct {
    const char *name;
    UINT stride;
    UINT n;
    const void *data;
    float want[4][3][4];   /* [instance][element][component] */
} append_case;

static void append_test(ID3D12Device *dev)
{
    static const char vs_src[] =
        "RWStructuredBuffer<float4> outb : register(u0);\n"
        "cbuffer c : register(b0) { uint base; };\n"
        "float4 main(float4 a : IPOS, float4 b : IUV, float4 c2 : IXA, uint iid : SV_InstanceID) : SV_Position {\n"
        "  outb[base + iid * 3 + 0] = a;\n"
        "  outb[base + iid * 3 + 1] = b;\n"
        "  outb[base + iid * 3 + 2] = c2; return float4(0, 0, 0, 1); }\n";
    static const char ps_src[] = "float4 main() : SV_Target { return 0; }\n";
    /* instance 0: POS = 0.1 0.2 0.3 0.4 | UVSCALE = 64,128,192,255 /255 | XAXIS = 0,1,2,3 /1023 then A 1/3
     * instance 1: POS = 0.5 0.6 0.7 0.8 | UVSCALE = 255,0,128,64 /255    | XAXIS = 1023,0,512,0 /1023 A 2/3 */
    static const uint16_t pos[16] = {
        6554, 13107, 19661, 26214,   /* 0.1 0.2 0.3 0.4 */
        32768, 39322, 45875, 52429    /* 0.5 0.6 0.7 0.8 */
    };
    static const uint8_t uv[8] = {64, 128, 192, 255, 255, 0, 128, 64};
    static const uint32_t xa[2] = {
        0u | (1u << 10) | (2u << 20) | (1u << 30),
        1023u | (0u << 10) | (512u << 20) | (2u << 30)
    };
    /* interleaved: POS(8) UV(4) XA(4) per instance = 16 bytes, four instances so
     * a shifted read shows up as an obviously wrong index rather than as zeros */
    uint8_t inter[64];
    /* POS values chosen so the source instance is identifiable at a glance:
     * instance k has x = 0.1 + 0.1k, y = 0.2 + 0.1k, z = 0.3 + 0.1k, w = 0.4 */
    static const uint16_t pos4[16] = {
        6554, 13107, 19661, 26214,    /* 0.1 0.2 0.3 0.4 */
        13107, 19661, 26214, 26214,   /* 0.2 0.3 0.4 0.4 */
        19661, 26214, 32768, 26214,   /* 0.3 0.4 0.5 0.4 */
        26214, 32768, 39322, 26214    /* 0.4 0.5 0.6 0.4 */
    };
    static const uint8_t uv4[16] = {64, 128, 192, 255, 128, 192, 255, 64, 192, 255, 64, 128, 255, 64, 128, 192};
    static const uint32_t xa4[4] = {
        0u | (1u << 10) | (2u << 20) | (1u << 30),
        1023u | (0u << 10) | (512u << 20) | (2u << 30),
        512u | (1023u << 10) | (0u << 20) | (0u << 30),
        256u | (256u << 10) | (256u << 20) | (3u << 30)
    };
    HMODULE comp = LoadLibraryA("d3dcompiler_47.dll");
    d3dcompile_fn compile = comp ? (d3dcompile_fn)GetProcAddress(comp, "D3DCompile") : NULL;
    D3D12_HEAP_PROPERTIES custom = {D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0, 0, 0};
    D3D12_HEAP_PROPERTIES def = {D3D12_HEAP_TYPE_DEFAULT, 0, 0, 0, 0};
    D3D12_RESOURCE_DESC bd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 0x10000, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                              D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC rh = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    D3D12_ROOT_PARAMETER rp[2];
    D3D12_ROOT_SIGNATURE_DESC rsd;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gd;
    D3D12_INPUT_ELEMENT_DESC el[3];
    D3D12_VIEWPORT vp = {0, 0, 4, 4, 0, 1};
    D3D12_RECT sc = {0, 0, 4, 4};
    ID3DBlob *vs = NULL, *ps = NULL, *rsb = NULL;
    ID3D12RootSignature *rs = NULL;
    ID3D12CommandQueue *q = NULL;
    ID3D12CommandAllocator *a = NULL;
    ID3D12GraphicsCommandList *l = NULL;
    ID3D12Resource *buf = NULL, *rt = NULL;
    ID3D12DescriptorHeap *rtvh = NULL;
    ID3D12Fence *f = NULL;
    ID3D12PipelineState *pso = NULL;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    D3D12_VERTEX_BUFFER_VIEW vb;
    uint32_t *m = NULL;
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    HRESULT hr;

    if (!compile) { logmsg("append-test: no d3dcompiler_47"); return; }
    for (int k = 0; k < 4; k++) {
        memcpy(inter + k * 16 + 0, pos4 + k * 4, 8);
        memcpy(inter + k * 16 + 8, uv4 + k * 4, 4);
        memcpy(inter + k * 16 + 12, xa4 + k, 4);
    }
    (void)pos; (void)uv; (void)xa;

    hr = compile(vs_src, sizeof(vs_src) - 1, "appvs", NULL, NULL, "main", "vs_5_0", 0, 0, &vs, NULL);
    if (SUCCEEDED(hr)) hr = compile(ps_src, sizeof(ps_src) - 1, "appps", NULL, NULL, "main", "ps_5_0", 0, 0, &ps, NULL);
    memset(rp, 0, sizeof(rp));
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.Num32BitValues = 1;
    memset(&rsd, 0, sizeof(rsd));
    rsd.NumParameters = 2;
    rsd.pParameters = rp;
    if (SUCCEEDED(hr)) hr = real_serialize_rs(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, NULL);
    if (SUCCEEDED(hr)) hr = ((create_rs_fn)dev_vt_orig[16])(dev, 0, ID3D10Blob_GetBufferPointer(rsb),
                                                           ID3D10Blob_GetBufferSize(rsb), &IID_ID3D12RootSignature, (void **)&rs);
    memset(el, 0, sizeof(el));
    el[0].SemanticName = "IPOS";  el[0].Format = DXGI_FORMAT_R16G16B16A16_UNORM;
    el[1].SemanticName = "IUV";   el[1].Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    el[2].SemanticName = "IXA";   el[2].Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    for (int k = 0; k < 3; k++) {
        el[k].InputSlot = 2;                       /* slot 2 is the instance slot, as in the census */
        /* FH2_EXPLICIT_OFFSETS=1 uses the equivalent 0/8/12 offsets so the two
         * layout spellings can be compared on the same data */
        {
            char v[8] = "";
            BOOL expl = GetEnvironmentVariableA("FH2_EXPLICIT_OFFSETS", v, sizeof(v)) && v[0] == '1';
            el[k].AlignedByteOffset = expl ? (UINT)(k == 0 ? 0 : k == 1 ? 8 : 12) : D3D12_APPEND_ALIGNED_ELEMENT;
        }
        el[k].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
        el[k].InstanceDataStepRate = 1;
    }
    memset(&gd, 0, sizeof(gd));
    gd.pRootSignature = rs;
    gd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs);
    gd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
    gd.PS.pShaderBytecode = ps ? ID3D10Blob_GetBufferPointer(ps) : NULL;
    gd.PS.BytecodeLength = ps ? ID3D10Blob_GetBufferSize(ps) : 0;
    gd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gd.SampleMask = ~0u;
    gd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gd.RasterizerState.DepthClipEnable = TRUE;
    gd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    gd.NumRenderTargets = 1;
    gd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    gd.SampleDesc.Count = 1;
    gd.InputLayout.NumElements = 3;
    gd.InputLayout.pInputElementDescs = el;
    if (SUCCEEDED(hr)) hr = ((create_gpso_fn)dev_vt_orig[10])(dev, &gd, &IID_ID3D12PipelineState, (void **)&pso);
    if (SUCCEEDED(hr)) hr = ((create_queue_fn)dev_vt_orig[8])(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&a);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommittedResource(dev, &custom, D3D12_HEAP_FLAG_NONE, &bd,
                                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
                                                                &IID_ID3D12Resource, (void **)&buf);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommittedResource(dev, &def, D3D12_HEAP_FLAG_NONE, &td,
                                                                D3D12_RESOURCE_STATE_RENDER_TARGET, NULL,
                                                                &IID_ID3D12Resource, (void **)&rt);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateDescriptorHeap(dev, &rh, &IID_ID3D12DescriptorHeap, (void **)&rtvh);
    if (SUCCEEDED(hr)) hr = ((create_fence_fn)dev_vt_orig[36])(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&f);
    if (SUCCEEDED(hr)) hr = ID3D12Resource_Map(buf, 0, NULL, (void **)&m);
    if (FAILED(hr)) { logmsg("append-test: setup failed %#lx", hr); goto done; }
    rtvh->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtvh, &rtv);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);
    memcpy((uint8_t *)m + 0x1000, inter, sizeof(inter));
    /* FH2_PAD=<n> adds trailing bytes to the vertex buffer view, to tell an
     * off-by-one on the last element from a real per-instance stride bug */
    {
        char v[16] = "";
        UINT pad = GetEnvironmentVariableA("FH2_PAD", v, sizeof(v)) ? (UINT)atoi(v) : 0;
        memset((uint8_t *)m + 0x1100, 0, 0x100);
        vb.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(buf) + 0x1000;
        vb.SizeInBytes = sizeof(inter) + pad;
        vb.StrideInBytes = 16;
    }
    memset((uint8_t *)m + 0x3000, 0, 0x400);
    ((create_list_fn)dev_vt_orig[12])(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, pso,
                                     &IID_ID3D12GraphicsCommandList, (void **)&l);
    ID3D12GraphicsCommandList_Reset(l, a, pso);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(l, rs);
    ID3D12GraphicsCommandList_SetGraphicsRootUnorderedAccessView(l, 0, ID3D12Resource_GetGPUVirtualAddress(buf) + 0x3000);
    ID3D12GraphicsCommandList_OMSetRenderTargets(l, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_RSSetViewports(l, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(l, D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D12GraphicsCommandList_IASetVertexBuffers(l, 2, 1, &vb);   /* slot 2 */
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstant(l, 1, 0, 0);
    ID3D12GraphicsCommandList_DrawInstanced(l, 1, 4, 0, 0);
    ID3D12GraphicsCommandList_Close(l);
    ((execute_fn)queue_vt_orig[10])(q, 1, (ID3D12CommandList *const *)&l);
    ((signal_fn)queue_vt_orig[14])(q, f, 7);
    ID3D12Fence_SetEventOnCompletion(f, 7, ev);
    WaitForSingleObject(ev, 5000);
    {
        uint8_t *out = (uint8_t *)m + 0x3000;
        char line[512] = "";
        for (UINT i2 = 0; i2 < 4; i2++) {
            for (int e = 0; e < 3; e++) {
                float *got = (float *)(out + 16 * (i2 * 3 + e));
                BOOL ok = 1;
                float wv[4];
                if (e == 0) {
                    wv[0] = pos4[i2 * 4 + 0] / 65535.f; wv[1] = pos4[i2 * 4 + 1] / 65535.f;
                    wv[2] = pos4[i2 * 4 + 2] / 65535.f; wv[3] = pos4[i2 * 4 + 3] / 65535.f;
                } else if (e == 1) {
                    wv[0] = uv4[i2 * 4 + 0] / 255.f; wv[1] = uv4[i2 * 4 + 1] / 255.f;
                    wv[2] = uv4[i2 * 4 + 2] / 255.f; wv[3] = uv4[i2 * 4 + 3] / 255.f;
                } else {
                    uint32_t v = xa4[i2];
                    wv[0] = (v & 1023) / 1023.f; wv[1] = ((v >> 10) & 1023) / 1023.f;
                    wv[2] = ((v >> 20) & 1023) / 1023.f; wv[3] = ((v >> 30) & 3) / 3.f;
                }
                for (int k = 0; k < 4; k++) {
                    float d = got[k] - wv[k];
                    if (d < 0) d = -d;
                    if (d > 0.002f) ok = 0;
                }
                sprintf(line + strlen(line), " i%u/e%d %g,%g,%g,%g%s", i2, e, got[0], got[1], got[2], got[3],
                        ok ? "" : " BAD");
            }
        }
        logmsg("append-test 4 instances:%s", line);
    }
done:
    if (pso) ID3D12PipelineState_Release(pso);
    if (l) ID3D12GraphicsCommandList_Release(l);
    if (f) ID3D12Fence_Release(f);
    if (rtvh) ID3D12DescriptorHeap_Release(rtvh);
    if (rt) ID3D12Resource_Release(rt);
    if (buf) ID3D12Resource_Release(buf);
    if (a) ID3D12CommandAllocator_Release(a);
    if (q) ID3D12CommandQueue_Release(q);
    if (rs) ID3D12RootSignature_Release(rs);
    if (rsb) ID3D10Blob_Release(rsb);
    if (vs) ID3D10Blob_Release(vs);
    if (ps) ID3D10Blob_Release(ps);
    CloseHandle(ev);
}

/* The prop vertex shaders read their placement from a structured buffer with
 * stride 288 (dcl_resource_structured t64, 288; ld_structured_indexable at 0, 4,
 * 20, 36, 52, ...). A wrong stride or a wrong element offset returns the next
 * object's transform, which is exactly how a prop ends up placed, scaled or
 * rotated like its neighbour while single-instance objects stay correct. */
/* the prop shader declares this buffer as "dcl_resource_structured t64, 288" and
 * reads byte offsets 0, 4, 20, 36, 52, 132, 148, 164 of element 0; it also uses
 * round_pi and div_sat, so both are probed here against known values */
static const char srv_probe_hlsl[] =
    "StructuredBuffer<uint> sb : register(t0);\n"
    "RWStructuredBuffer<float4> b : register(u0);\n"
    "[numthreads(1,1,1)] void main() {\n"
    "  b[0] = float4(asfloat(sb[1]), asfloat(sb[5]), asfloat(sb[9]), asfloat(sb[13]));\n"
    "  b[1] = float4(asfloat(sb[33]), asfloat(sb[37]), asfloat(sb[41]), asfloat(sb[0]));\n"
    "  b[2] = float4(round(0.5), round(1.5), round(2.5), round(-0.5));\n"
    "  b[3] = float4(saturate(1.0 / 0.0), saturate(-1.0 / 0.0), saturate(3.0 / 2.0), saturate(-3.0 / 2.0));\n"
    "}\n";

static void srv_test(ID3D12Device *dev)
{
    HMODULE comp = LoadLibraryA("d3dcompiler_47.dll");
    d3dcompile_fn compile = comp ? (d3dcompile_fn)GetProcAddress(comp, "D3DCompile") : NULL;
    D3D12_HEAP_PROPERTIES custom = {D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0, 0, 0};
    D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 0x10000, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC dh = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    D3D12_ROOT_PARAMETER rp[2];
    D3D12_DESCRIPTOR_RANGE range;
    D3D12_ROOT_SIGNATURE_DESC rsd;
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd;
    ID3DBlob *cs = NULL, *rsb = NULL;
    ID3D12RootSignature *rs = NULL;
    ID3D12PipelineState *ps = NULL;
    ID3D12CommandQueue *q = NULL;
    ID3D12CommandAllocator *a = NULL;
    ID3D12GraphicsCommandList *l = NULL;
    ID3D12Resource *buf = NULL;
    ID3D12DescriptorHeap *heap = NULL;
    ID3D12Fence *f = NULL;
    uint32_t *m = NULL;
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    HRESULT hr;
    int bad = 0;

    if (!compile) { logmsg("srv-test: no D3DCompile"); return; }
    hr = compile(srv_probe_hlsl, sizeof(srv_probe_hlsl) - 1, "srvprobe", NULL, NULL, "main", "cs_5_0", 0, 0, &cs, NULL);
    if (FAILED(hr)) { logmsg("srv-test: compile failed %#lx", hr); goto done; }
    memset(rp, 0, sizeof(rp));
    memset(&range, 0, sizeof(range));
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1;
    rp[0].DescriptorTable.pDescriptorRanges = &range;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    memset(&rsd, 0, sizeof(rsd));
    rsd.NumParameters = 2;
    rsd.pParameters = rp;
    if (SUCCEEDED(hr)) hr = real_serialize_rs(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, NULL);
    if (SUCCEEDED(hr)) hr = ((create_rs_fn)dev_vt_orig[16])(dev, 0, ID3D10Blob_GetBufferPointer(rsb),
                                                           ID3D10Blob_GetBufferSize(rsb), &IID_ID3D12RootSignature, (void **)&rs);
    memset(&cd, 0, sizeof(cd));
    cd.pRootSignature = rs;
    cd.CS.pShaderBytecode = ID3D10Blob_GetBufferPointer(cs);
    cd.CS.BytecodeLength = ID3D10Blob_GetBufferSize(cs);
    if (SUCCEEDED(hr)) hr = ((create_cpso_fn)dev_vt_orig[11])(dev, &cd, &IID_ID3D12PipelineState, (void **)&ps);
    if (SUCCEEDED(hr)) hr = ((create_queue_fn)dev_vt_orig[8])(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&a);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommittedResource(dev, &custom, D3D12_HEAP_FLAG_NONE, &rd,
                                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
                                                                &IID_ID3D12Resource, (void **)&buf);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateDescriptorHeap(dev, &dh, &IID_ID3D12DescriptorHeap, (void **)&heap);
    if (SUCCEEDED(hr)) hr = ((create_fence_fn)dev_vt_orig[36])(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&f);
    if (SUCCEEDED(hr)) hr = ID3D12Resource_Map(buf, 0, NULL, (void **)&m);
    if (FAILED(hr)) { logmsg("srv-test: setup failed %#lx", hr); goto done; }
    /* 4 elements of 16 floats each: element e holds (100+e, 200+e, 300+e, 400+e) in
     * every float4, so a wrong element index is unmistakable */
    /* distinct value at each byte offset the shader loads */
    {
        static const UINT offs[] = {0, 4, 20, 36, 52, 132, 148, 164};
        for (UINT k = 0; k < 8; k++) {
            float v = 1000.f * (float)(k + 1);
            memcpy((uint8_t *)m + offs[k], &v, 4);
        }
    }
    memset((uint8_t *)m + 0x4000, 0, 0x100);
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap, &cpu);
        memset(&sd, 0, sizeof(sd));
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = 0x1000 / 4;          /* structured, one uint per element */
        sd.Buffer.StructureByteStride = 4;
        ID3D12Device_CreateShaderResourceView(dev, buf, &sd, cpu);
    }
    ((create_list_fn)dev_vt_orig[12])(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, ps,
                                     &IID_ID3D12GraphicsCommandList, (void **)&l);
    ID3D12GraphicsCommandList_Reset(l, a, ps);
    ID3D12GraphicsCommandList_SetComputeRootSignature(l, rs);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(l, 1, &heap);
    {
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(heap, &gpu);
        ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(l, 0, gpu);
    }
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(l, 1, ID3D12Resource_GetGPUVirtualAddress(buf) + 0x4000);
    ID3D12GraphicsCommandList_Dispatch(l, 1, 1, 1);
    ID3D12GraphicsCommandList_Close(l);
    ((execute_fn)queue_vt_orig[10])(q, 1, (ID3D12CommandList *const *)&l);
    ((signal_fn)queue_vt_orig[14])(q, f, 11);
    ID3D12Fence_SetEventOnCompletion(f, 11, ev);
    WaitForSingleObject(ev, 5000);
    {
        float *out = (float *)((uint8_t *)m + 0x4000);
        /* b[0] = StructuredBuffer elements 1,5,9,13 -> bytes 4,20,36,52 -> 2000,3000,4000,5000
         * b[1] = elements 33,37,41,0 -> bytes 132,148,164,0 -> 6000,7000,8000,1000
         * b[2] = round 0.5 1.5 2.5 -0.5 -> 0 2 2 0   (round half to even)
         * b[3] = saturate(1/0, -1/0, 3/2, -3/2) -> 1 0 1 0 */
        static const float want[16] = {2000, 3000, 4000, 5000, 6000, 7000, 8000, 1000,
                                       0, 2, 2, 0, 1, 0, 1, 0};
        char line[512] = "";
        for (int k = 0; k < 16; k++) {
            if (out[k] != want[k]) bad++;
            sprintf(line + strlen(line), " %g/%.0f", out[k], want[k]);
        }
        logmsg("srv-test structured/byte loads at 288-stride offsets + round + saturate:%s%s",
               line, bad ? "  BAD" : "  all ok");
    }
done:
    if (ps) ID3D12PipelineState_Release(ps);
    if (l) ID3D12GraphicsCommandList_Release(l);
    if (f) ID3D12Fence_Release(f);
    if (heap) ID3D12DescriptorHeap_Release(heap);
    if (buf) ID3D12Resource_Release(buf);
    if (a) ID3D12CommandAllocator_Release(a);
    if (q) ID3D12CommandQueue_Release(q);
    if (rs) ID3D12RootSignature_Release(rs);
    if (rsb) ID3D10Blob_Release(rsb);
    if (cs) ID3D10Blob_Release(cs);
    CloseHandle(ev);
}

/* the wrapper rewrites constant accesses through CB13 offsets, so the same physical
 * buffer is read at several byte offsets. If a CBV's offset is ignored, every
 * transform in that draw reads the wrong matrix: props land scaled and rotated
 * wrongly while draws that happen to sit at offset 0 stay correct. */
static const char cbv_probe_hlsl[] =
    "cbuffer base : register(b0) { float4 a; };\n"
    "cbuffer shifted : register(b1) { float4 b; };\n"
    "cbuffer faraway : register(b2) { float4 c; };\n"
    "RWStructuredBuffer<float4> o : register(u0);\n"
    "[numthreads(1,1,1)] void main() { o[0] = a; o[1] = b; o[2] = c; o[3] = float4(0x1234, 0, 0, 0); }\n";

static void cbv_test(ID3D12Device *dev)
{
    HMODULE comp = LoadLibraryA("d3dcompiler_47.dll");
    d3dcompile_fn compile = comp ? (d3dcompile_fn)GetProcAddress(comp, "D3DCompile") : NULL;
    D3D12_HEAP_PROPERTIES custom = {D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0, 0, 0};
    D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 0x10000, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    D3D12_ROOT_PARAMETER rp[4];
    D3D12_ROOT_SIGNATURE_DESC rsd;
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd;
    ID3DBlob *cs = NULL, *rsb = NULL;
    ID3D12RootSignature *rs = NULL;
    ID3D12PipelineState *ps = NULL;
    ID3D12CommandQueue *q = NULL;
    ID3D12CommandAllocator *a = NULL;
    ID3D12GraphicsCommandList *l = NULL;
    ID3D12Resource *cb = NULL, *outb = NULL;
    ID3D12Fence *f = NULL;
    uint8_t *m = NULL, *mo = NULL;
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    HRESULT hr;
    int bad = 0;

    if (!compile) { logmsg("cbv-test: no D3DCompile"); return; }
    hr = compile(cbv_probe_hlsl, sizeof(cbv_probe_hlsl) - 1, "cbvprobe", NULL, NULL, "main", "cs_5_0", 0, 0, &cs, NULL);
    if (FAILED(hr)) { logmsg("cbv-test: compile failed %#lx", hr); goto done; }
    memset(rp, 0, sizeof(rp));
    for (int i2 = 0; i2 < 3; i2++) rp[i2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    memset(&rsd, 0, sizeof(rsd));
    rsd.NumParameters = 4;
    rsd.pParameters = rp;
    if (SUCCEEDED(hr)) hr = real_serialize_rs(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, NULL);
    if (SUCCEEDED(hr)) hr = ((create_rs_fn)dev_vt_orig[16])(dev, 0, ID3D10Blob_GetBufferPointer(rsb),
                                                           ID3D10Blob_GetBufferSize(rsb), &IID_ID3D12RootSignature, (void **)&rs);
    memset(&cd, 0, sizeof(cd));
    cd.pRootSignature = rs;
    cd.CS.pShaderBytecode = ID3D10Blob_GetBufferPointer(cs);
    cd.CS.BytecodeLength = ID3D10Blob_GetBufferSize(cs);
    if (SUCCEEDED(hr)) hr = ((create_cpso_fn)dev_vt_orig[11])(dev, &cd, &IID_ID3D12PipelineState, (void **)&ps);
    if (SUCCEEDED(hr)) hr = ((create_queue_fn)dev_vt_orig[8])(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&a);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommittedResource(dev, &custom, D3D12_HEAP_FLAG_NONE, &rd,
                                                                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, NULL,
                                                                &IID_ID3D12Resource, (void **)&cb);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommittedResource(dev, &custom, D3D12_HEAP_FLAG_NONE, &rd,
                                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
                                                                &IID_ID3D12Resource, (void **)&outb);
    if (SUCCEEDED(hr)) hr = ((create_fence_fn)dev_vt_orig[36])(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&f);
    if (SUCCEEDED(hr)) hr = ID3D12Resource_Map(cb, 0, NULL, (void **)&m);
    if (SUCCEEDED(hr)) hr = ID3D12Resource_Map(outb, 0, NULL, (void **)&mo);
    if (FAILED(hr)) {
        logmsg("cbv-test: setup failed %#lx (rs %p ps %p cb %p out %p)", hr, rs, ps, cb, outb);
        goto done;
    }
    /* three CBVs into one buffer at 0, 256 and 1024 (all 256-byte aligned) */
    {
        float v[4] = {11, 12, 13, 14}; memcpy(m + 0, v, 16);
        float w[4] = {21, 22, 23, 24}; memcpy(m + 256, w, 16);
        float x[4] = {31, 32, 33, 34}; memcpy(m + 1024, x, 16);
    }
    memset(mo, 0, 0x100);
    ((create_list_fn)dev_vt_orig[12])(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, ps,
                                     &IID_ID3D12GraphicsCommandList, (void **)&l);
    ID3D12GraphicsCommandList_Reset(l, a, ps);
    ID3D12GraphicsCommandList_SetComputeRootSignature(l, rs);
    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(l, 0, ID3D12Resource_GetGPUVirtualAddress(cb) + 0);
    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(l, 1, ID3D12Resource_GetGPUVirtualAddress(cb) + 256);
    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(l, 2, ID3D12Resource_GetGPUVirtualAddress(cb) + 1024);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(l, 3, ID3D12Resource_GetGPUVirtualAddress(outb));
    ID3D12GraphicsCommandList_Dispatch(l, 1, 1, 1);
    ID3D12GraphicsCommandList_Close(l);
    ((execute_fn)queue_vt_orig[10])(q, 1, (ID3D12CommandList *const *)&l);
    ((signal_fn)queue_vt_orig[14])(q, f, 13);
    ID3D12Fence_SetEventOnCompletion(f, 13, ev);
    WaitForSingleObject(ev, 5000);
    {
        float *out = (float *)mo;
        static const float want[12] = {11, 12, 13, 14, 21, 22, 23, 24, 31, 32, 33, 34};
        char line[256] = "";
        for (int k = 0; k < 12; k++) {
            if (out[k] != want[k]) bad++;
            sprintf(line + strlen(line), " %g/%.0f", out[k], want[k]);
        }
        logmsg("cbv-test hr %#lx fence %llu marker %g (want 4660); offsets 0/256/1024:%s%s", hr,
               ID3D12Fence_GetCompletedValue(f), out[12], line, bad ? "  BAD" : "  all ok");
    }
done:
    if (ps) ID3D12PipelineState_Release(ps);
    if (l) ID3D12GraphicsCommandList_Release(l);
    if (f) ID3D12Fence_Release(f);
    if (outb) ID3D12Resource_Release(outb);
    if (cb) ID3D12Resource_Release(cb);
    if (a) ID3D12CommandAllocator_Release(a);
    if (q) ID3D12CommandQueue_Release(q);
    if (rs) ID3D12RootSignature_Release(rs);
    if (rsb) ID3D10Blob_Release(rsb);
    if (cs) ID3D10Blob_Release(cs);
    CloseHandle(ev);
}

/* StartInstanceLocation and BaseVertexLocation feed the *attributes*, not just the
 * SV_ semantics. D3D12 makes the vertex/instance data start at those offsets, while
 * SV_VertexID/SV_InstanceID stay zero-based (already measured as excluded).
 * A translator that drops them reads the wrong element for every instance: props
 * land with the previous instance's transform, which is a huge wall or a tilted
 * tree, while a single-instance object such as the car stays correct. */
static void start_offset_test(ID3D12Device *dev)
{
    static const char vs_src[] =
        "RWStructuredBuffer<float4> outb : register(u0);\n"
        "cbuffer c : register(b0) { uint base; };\n"
        "float4 main(float4 p : POS, float4 ip : IPOS, uint iid : SV_InstanceID, uint vid : SV_VertexID)\n"
        "  : SV_Position {\n"
        "  outb[base + iid] = float4(p.x, ip.x, ip.y, ip.z); return float4(0, 0, 0, 1); }\n";
    static const char ps_src[] = "float4 main() : SV_Target { return 0; }\n";
    /* 10 vertices at x = 100 + k, 10 instances with ip.x = k */
    float verts[40], insts[40];
    HMODULE comp = LoadLibraryA("d3dcompiler_47.dll");
    d3dcompile_fn compile = comp ? (d3dcompile_fn)GetProcAddress(comp, "D3DCompile") : NULL;
    D3D12_HEAP_PROPERTIES custom = {D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0, 0, 0};
    D3D12_HEAP_PROPERTIES def = {D3D12_HEAP_TYPE_DEFAULT, 0, 0, 0, 0};
    D3D12_RESOURCE_DESC bd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 0x10000, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                              D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC rh = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    D3D12_ROOT_PARAMETER rp[2];
    D3D12_ROOT_SIGNATURE_DESC rsd;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gd;
    D3D12_INPUT_ELEMENT_DESC el[2];
    D3D12_VIEWPORT vp = {0, 0, 4, 4, 0, 1};
    D3D12_RECT sc = {0, 0, 4, 4};
    D3D12_VERTEX_BUFFER_VIEW vb[2];
    ID3DBlob *vs = NULL, *ps = NULL, *rsb = NULL;
    ID3D12RootSignature *rs = NULL;
    ID3D12CommandQueue *q = NULL;
    ID3D12CommandAllocator *a = NULL;
    ID3D12GraphicsCommandList *l = NULL;
    ID3D12Resource *buf = NULL, *rt = NULL;
    ID3D12DescriptorHeap *rtvh = NULL;
    ID3D12Fence *f = NULL;
    ID3D12PipelineState *pso = NULL;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    uint32_t *m = NULL;
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    HRESULT hr;

    if (!compile) return;
    for (int k = 0; k < 10; k++) {
        verts[k * 4 + 0] = 100.f + k; verts[k * 4 + 1] = verts[k * 4 + 2] = verts[k * 4 + 3] = 0;
        insts[k * 4 + 0] = (float)k;  insts[k * 4 + 1] = insts[k * 4 + 2] = insts[k * 4 + 3] = 0;
    }
    hr = compile(vs_src, sizeof(vs_src) - 1, "sovs", NULL, NULL, "main", "vs_5_0", 0, 0, &vs, NULL);
    if (SUCCEEDED(hr)) hr = compile(ps_src, sizeof(ps_src) - 1, "sops", NULL, NULL, "main", "ps_5_0", 0, 0, &ps, NULL);
    memset(rp, 0, sizeof(rp));
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.Num32BitValues = 1;
    memset(&rsd, 0, sizeof(rsd));
    rsd.NumParameters = 2;
    rsd.pParameters = rp;
    if (SUCCEEDED(hr)) hr = real_serialize_rs(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, NULL);
    if (SUCCEEDED(hr)) hr = ((create_rs_fn)dev_vt_orig[16])(dev, 0, ID3D10Blob_GetBufferPointer(rsb),
                                                           ID3D10Blob_GetBufferSize(rsb), &IID_ID3D12RootSignature, (void **)&rs);
    memset(el, 0, sizeof(el));
    el[0].SemanticName = "POS";  el[0].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; el[0].InputSlot = 0;
    el[1].SemanticName = "IPOS"; el[1].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; el[1].InputSlot = 1;
    el[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
    el[1].InstanceDataStepRate = 1;
    memset(&gd, 0, sizeof(gd));
    gd.pRootSignature = rs;
    gd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs);
    gd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
    gd.PS.pShaderBytecode = ps ? ID3D10Blob_GetBufferPointer(ps) : NULL;
    gd.PS.BytecodeLength = ps ? ID3D10Blob_GetBufferSize(ps) : 0;
    gd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gd.SampleMask = ~0u;
    gd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gd.RasterizerState.DepthClipEnable = TRUE;
    gd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    gd.NumRenderTargets = 1;
    gd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    gd.SampleDesc.Count = 1;
    gd.InputLayout.NumElements = 2;
    gd.InputLayout.pInputElementDescs = el;
    if (SUCCEEDED(hr)) hr = ((create_gpso_fn)dev_vt_orig[10])(dev, &gd, &IID_ID3D12PipelineState, (void **)&pso);
    if (SUCCEEDED(hr)) hr = ((create_queue_fn)dev_vt_orig[8])(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&a);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommittedResource(dev, &custom, D3D12_HEAP_FLAG_NONE, &bd,
                                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
                                                                &IID_ID3D12Resource, (void **)&buf);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommittedResource(dev, &def, D3D12_HEAP_FLAG_NONE, &td,
                                                                D3D12_RESOURCE_STATE_RENDER_TARGET, NULL,
                                                                &IID_ID3D12Resource, (void **)&rt);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateDescriptorHeap(dev, &rh, &IID_ID3D12DescriptorHeap, (void **)&rtvh);
    if (SUCCEEDED(hr)) hr = ((create_fence_fn)dev_vt_orig[36])(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&f);
    if (SUCCEEDED(hr)) hr = ID3D12Resource_Map(buf, 0, NULL, (void **)&m);
    if (FAILED(hr)) { logmsg("start-offset-test: setup failed %#lx", hr); goto done; }
    rtvh->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtvh, &rtv);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);
    memcpy((uint8_t *)m + 0x1000, verts, sizeof(verts));
    memcpy((uint8_t *)m + 0x2000, insts, sizeof(insts));
    memset((uint8_t *)m + 0x3000, 0, 0x400);
    vb[0].BufferLocation = ID3D12Resource_GetGPUVirtualAddress(buf) + 0x1000;
    vb[0].SizeInBytes = sizeof(verts); vb[0].StrideInBytes = 16;
    vb[1].BufferLocation = ID3D12Resource_GetGPUVirtualAddress(buf) + 0x2000;
    vb[1].SizeInBytes = sizeof(insts); vb[1].StrideInBytes = 16;
    ((create_list_fn)dev_vt_orig[12])(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, pso,
                                     &IID_ID3D12GraphicsCommandList, (void **)&l);
    ID3D12GraphicsCommandList_Reset(l, a, pso);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(l, rs);
    ID3D12GraphicsCommandList_SetGraphicsRootUnorderedAccessView(l, 0, ID3D12Resource_GetGPUVirtualAddress(buf) + 0x3000);
    ID3D12GraphicsCommandList_OMSetRenderTargets(l, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_RSSetViewports(l, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(l, D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D12GraphicsCommandList_IASetVertexBuffers(l, 0, 2, vb);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstant(l, 1, 0, 0);
    /* two instances starting at instance 3 and two vertices starting at vertex 4:
     * D3D wants (vertex 104, instance 3) then (vertex 104, instance 4) */
    ID3D12GraphicsCommandList_DrawInstanced(l, 2, 2, 4, 3);
    /* the same offsets through ExecuteIndirect: the world props are drawn this way,
     * and the indirect argument path is a separate decode in a translator */
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstant(l, 1, 8, 0);
    {
        ID3D12CommandSignature *sig = NULL;
        D3D12_INDIRECT_ARGUMENT_DESC iad = {D3D12_INDIRECT_ARGUMENT_TYPE_DRAW};
        D3D12_COMMAND_SIGNATURE_DESC sd = {16, 1, &iad, 0};
        if (SUCCEEDED(((create_signature_fn2)dev_vt_orig[41])(dev, &sd, NULL, &IID_ID3D12CommandSignature, (void **)&sig))) {
            ((uint32_t *)((uint8_t *)m + 0x2800))[0] = 2;   /* vertex count */
            ((uint32_t *)((uint8_t *)m + 0x2800))[1] = 2;   /* instance count */
            ((uint32_t *)((uint8_t *)m + 0x2800))[2] = 4;   /* start vertex */
            ((uint32_t *)((uint8_t *)m + 0x2800))[3] = 3;   /* start instance */
            ID3D12GraphicsCommandList_ExecuteIndirect(l, sig, 1, buf, 0x2800, NULL, 0);
            ID3D12CommandSignature_Release(sig);
        }
    }
    ID3D12GraphicsCommandList_Close(l);
    ((execute_fn)queue_vt_orig[10])(q, 1, (ID3D12CommandList *const *)&l);
    ((signal_fn)queue_vt_orig[14])(q, f, 9);
    ID3D12Fence_SetEventOnCompletion(f, 9, ev);
    WaitForSingleObject(ev, 5000);
    {
        uint8_t *out = (uint8_t *)m + 0x3000;
        char line[256] = "";
        for (int b = 0; b < 2; b++) {
            for (int k = 0; k < 2; k++) {
                /* direct draw writes slots 0..1, the indirect one starts at slot 8 */
                float *got = (float *)(out + 16 * (b ? 8 + k : k));
                sprintf(line + strlen(line), " %s i%d vertex %g instance %g%s", b ? "indirect" : "direct", k,
                        got[0], got[1], (got[0] == 104.f && got[1] == 3.f + k) ? " ok" : " BAD");
            }
        }
        logmsg("start-offset-test start vertex 4, start inst 3 (D3D wants vertex 104, instance 3 and 4):%s", line);
    }
done:
    if (pso) ID3D12PipelineState_Release(pso);
    if (l) ID3D12GraphicsCommandList_Release(l);
    if (f) ID3D12Fence_Release(f);
    if (rtvh) ID3D12DescriptorHeap_Release(rtvh);
    if (rt) ID3D12Resource_Release(rt);
    if (buf) ID3D12Resource_Release(buf);
    if (a) ID3D12CommandAllocator_Release(a);
    if (q) ID3D12CommandQueue_Release(q);
    if (rs) ID3D12RootSignature_Release(rs);
    if (rsb) ID3D10Blob_Release(rsb);
    if (vs) ID3D10Blob_Release(vs);
    if (ps) ID3D10Blob_Release(ps);
    CloseHandle(ev);
}

/* the wrapper may describe a structured buffer with a view stride (e.g. 4) that
 * differs from the shader's declaration (t64, 288). D3D drivers address
 * ld_structured with the shader's stride; if D3DMetal uses the view's, element
 * i > 0 reads from i * 4 instead of i * 288 and props take garbage transforms.
 * Each float in the buffer holds its own byte offset, so the output says
 * exactly which byte was read. */
static const char stride_probe_hlsl[] =
    "struct S { float4 v[18]; };\n"
    "StructuredBuffer<S> sb : register(t0);\n"
    "RWStructuredBuffer<float4> b : register(u0);\n"
    "[numthreads(1,1,1)] void main() {\n"
    "  for (uint i = 0; i < 4; i++) { b[i] = sb[i].v[12]; b[4 + i] = sb[i].v[16]; }\n"
    "}\n";

static void stride_test(ID3D12Device *dev)
{
    static const struct { UINT stride; UINT64 first; UINT num; const char *what; } views[] = {
        {288, 0, 32, "view stride 288"},
        {4, 0, 32 * 72, "view stride 4"},
        {288, 2, 30, "view stride 288 first 2"},
        {4, 144, 30 * 72, "view stride 4 first 144 (=2*288 bytes)"},
    };
    HMODULE comp = LoadLibraryA("d3dcompiler_47.dll");
    d3dcompile_fn compile = comp ? (d3dcompile_fn)GetProcAddress(comp, "D3DCompile") : NULL;
    D3D12_HEAP_PROPERTIES custom = {D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0, 0, 0};
    D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 0x10000, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC dh = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    D3D12_ROOT_PARAMETER rp[2];
    D3D12_DESCRIPTOR_RANGE range;
    D3D12_ROOT_SIGNATURE_DESC rsd;
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd;
    ID3DBlob *cs = NULL, *rsb = NULL;
    ID3D12RootSignature *rs = NULL;
    ID3D12PipelineState *ps = NULL;
    ID3D12CommandQueue *q = NULL;
    ID3D12CommandAllocator *a = NULL;
    ID3D12GraphicsCommandList *l = NULL;
    ID3D12Resource *buf = NULL;
    ID3D12DescriptorHeap *heap = NULL;
    ID3D12Fence *f = NULL;
    float *m = NULL;
    UINT inc;
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    HRESULT hr;

    if (!compile) { logmsg("stride-test: no D3DCompile"); return; }
    hr = compile(stride_probe_hlsl, sizeof(stride_probe_hlsl) - 1, "strideprobe", NULL, NULL, "main", "cs_5_0", 0, 0, &cs, NULL);
    if (FAILED(hr)) { logmsg("stride-test: compile failed %#lx", hr); goto done; }
    memset(rp, 0, sizeof(rp));
    memset(&range, 0, sizeof(range));
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1;
    rp[0].DescriptorTable.pDescriptorRanges = &range;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    memset(&rsd, 0, sizeof(rsd));
    rsd.NumParameters = 2;
    rsd.pParameters = rp;
    if (SUCCEEDED(hr)) hr = real_serialize_rs(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, NULL);
    if (SUCCEEDED(hr)) hr = ((create_rs_fn)dev_vt_orig[16])(dev, 0, ID3D10Blob_GetBufferPointer(rsb),
                                                           ID3D10Blob_GetBufferSize(rsb), &IID_ID3D12RootSignature, (void **)&rs);
    memset(&cd, 0, sizeof(cd));
    cd.pRootSignature = rs;
    cd.CS.pShaderBytecode = ID3D10Blob_GetBufferPointer(cs);
    cd.CS.BytecodeLength = ID3D10Blob_GetBufferSize(cs);
    if (SUCCEEDED(hr)) hr = ((create_cpso_fn)dev_vt_orig[11])(dev, &cd, &IID_ID3D12PipelineState, (void **)&ps);
    if (SUCCEEDED(hr)) hr = ((create_queue_fn)dev_vt_orig[8])(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&a);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommittedResource(dev, &custom, D3D12_HEAP_FLAG_NONE, &rd,
                                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
                                                                &IID_ID3D12Resource, (void **)&buf);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateDescriptorHeap(dev, &dh, &IID_ID3D12DescriptorHeap, (void **)&heap);
    if (SUCCEEDED(hr)) hr = ((create_fence_fn)dev_vt_orig[36])(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&f);
    if (SUCCEEDED(hr)) hr = ID3D12Resource_Map(buf, 0, NULL, (void **)&m);
    if (FAILED(hr)) { logmsg("stride-test: setup failed %#lx", hr); goto done; }
    for (UINT k = 0; k < 0x8000 / 4; k++) m[k] = (float)(k * 4);   /* each float = its byte offset */
    memset((uint8_t *)m + 0x8000, 0xff, 0x400);
    inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (UINT v = 0; v < 4; v++) {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap, &cpu);
        cpu.ptr += (SIZE_T)v * inc;
        memset(&sd, 0, sizeof(sd));
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.FirstElement = views[v].first;
        sd.Buffer.NumElements = views[v].num;
        sd.Buffer.StructureByteStride = views[v].stride;
        ID3D12Device_CreateShaderResourceView(dev, buf, &sd, cpu);
    }
    ((create_list_fn)dev_vt_orig[12])(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, ps,
                                     &IID_ID3D12GraphicsCommandList, (void **)&l);
    ID3D12GraphicsCommandList_Reset(l, a, ps);
    ID3D12GraphicsCommandList_SetComputeRootSignature(l, rs);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(l, 1, &heap);
    for (UINT v = 0; v < 4; v++) {
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(heap, &gpu);
        gpu.ptr += (UINT64)v * inc;
        ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(l, 0, gpu);
        ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(l, 1, ID3D12Resource_GetGPUVirtualAddress(buf) + 0x8000 + v * 0x100);
        ID3D12GraphicsCommandList_Dispatch(l, 1, 1, 1);
    }
    ID3D12GraphicsCommandList_Close(l);
    ((execute_fn)queue_vt_orig[10])(q, 1, (ID3D12CommandList *const *)&l);
    ((signal_fn)queue_vt_orig[14])(q, f, 11);
    ID3D12Fence_SetEventOnCompletion(f, 11, ev);
    WaitForSingleObject(ev, 5000);
    for (UINT v = 0; v < 4; v++) {
        const float *out = (const float *)((uint8_t *)m + 0x8000 + v * 0x100);
        UINT base = (UINT)(views[v].first * views[v].stride);
        char line[256] = "";
        int bad = 0;
        /* shader stride semantics: element i of the view starts at base + i * 288 */
        for (UINT i = 0; i < 4; i++) {
            UINT want12 = base + i * 288 + 192, want16 = base + i * 288 + 256;
            if (out[i * 4] != (float)want12 || out[16 + i * 4] != (float)want16) bad++;
            sprintf(line + strlen(line), " [%u] %g/%u %g/%u", i, out[i * 4], want12, out[16 + i * 4], want16);
        }
        logmsg("stride-test %s: read/want byte offsets%s%s", views[v].what, line, bad ? "  BAD" : "  all ok");
    }
done:
    if (ps) ID3D12PipelineState_Release(ps);
    if (l) ID3D12GraphicsCommandList_Release(l);
    if (f) ID3D12Fence_Release(f);
    if (heap) ID3D12DescriptorHeap_Release(heap);
    if (buf) ID3D12Resource_Release(buf);
    if (a) ID3D12CommandAllocator_Release(a);
    if (q) ID3D12CommandQueue_Release(q);
    if (rs) ID3D12RootSignature_Release(rs);
    if (rsb) ID3D10Blob_Release(rsb);
    if (cs) ID3D10Blob_Release(cs);
    CloseHandle(ev);
}

/* The exact input layout of the phantom-wall PSO (shader hash 9d72bc4d): 11
 * elements on 3 slots, all APPEND_ALIGNED_ELEMENT, TEXCOORD2 in the layout but
 * not in the shader, five per-instance elements packed in 24 bytes on slot 2, and
 * a DrawIndexedInstanced with StartInstanceLocation 98 as the game issues it. The
 * VS copies the five instance attributes of each instance into a UAV. Forcing the
 * wall shader onto its t64 path fixes the wall, so this compact-attribute path is
 * the suspect; the earlier probes only covered three elements on one slot. */
static const char layout_probe_vs[] =
    "RWStructuredBuffer<float4> o : register(u0);\n"
    "struct In { float3 p : POSITION; float3 n : NORMAL; float4 tg : TANGENT; float2 uv : TEXCOORD0; float4 c : COLOR;\n"
    "  float4 ip : I_POS; float4 iu : I_UVSCALE; float4 ix : I_XAXIS; float4 iy : I_YAXIS; float4 ic : I_COLOUR;\n"
    "  uint iid : SV_InstanceID; };\n"
    "float4 main(In i) : SV_Position {\n"
    "  uint b = i.iid * 6;\n"
    "  o[b] = i.ip; o[b + 1] = i.iu; o[b + 2] = i.ix; o[b + 3] = i.iy; o[b + 4] = i.ic;\n"
    "  o[b + 5] = float4(i.p.x + i.n.x + i.tg.x + i.uv.x + i.c.x, i.p.y, i.n.y, i.uv.y);\n"
    "  return float4(0, 0, 0, 1); }\n";

static void layout_record(UINT i, uint8_t *r)
{
    uint16_t pos[4] = {(uint16_t)(i * 500 + 1), (uint16_t)(i * 500 + 2), (uint16_t)(i * 500 + 3), (uint16_t)(655 + i)};
    uint32_t xa = ((i * 3 + 1) & 1023) | (((i * 3 + 2) & 1023) << 10) | (((i * 3 + 3) & 1023) << 20) | ((i & 3u) << 30);
    uint32_t ya = ((i * 3 + 500) & 1023) | (((i * 3 + 501) & 1023) << 10) | (((i * 3 + 502) & 1023) << 20) | (((i + 1) & 3u) << 30);
    memcpy(r, pos, 8);
    r[8] = (uint8_t)i; r[9] = (uint8_t)(i + 1); r[10] = (uint8_t)(i + 2); r[11] = (uint8_t)(i + 3);
    memcpy(r + 12, &xa, 4);
    memcpy(r + 16, &ya, 4);
    r[20] = (uint8_t)(200 - i); r[21] = (uint8_t)(201 - i); r[22] = (uint8_t)(202 - i); r[23] = (uint8_t)(203 - i);
}

static void layout_expect(UINT i, float w[5][4])
{
    uint8_t r[24];
    uint32_t xa, ya;
    layout_record(i, r);
    for (int k = 0; k < 4; k++) w[0][k] = ((const uint16_t *)r)[k] / 65535.f;
    for (int k = 0; k < 4; k++) w[1][k] = r[8 + k] / 255.f;
    memcpy(&xa, r + 12, 4);
    memcpy(&ya, r + 16, 4);
    for (int k = 0; k < 3; k++) { w[2][k] = ((xa >> (10 * k)) & 1023) / 1023.f; w[3][k] = ((ya >> (10 * k)) & 1023) / 1023.f; }
    w[2][3] = (xa >> 30) / 3.f;
    w[3][3] = (ya >> 30) / 3.f;
    for (int k = 0; k < 4; k++) w[4][k] = r[20 + k] / 255.f;
}

static void layout_test(ID3D12Device *dev, BOOL explicit_offsets)
{
    static const D3D12_INPUT_ELEMENT_DESC els[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R16G16_UNORM, 1, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R16G16_UNORM, 1, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"I_POS", 0, DXGI_FORMAT_R16G16B16A16_UNORM, 2, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"I_UVSCALE", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 2, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"I_XAXIS", 0, DXGI_FORMAT_R10G10B10A2_UNORM, 2, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"I_YAXIS", 0, DXGI_FORMAT_R10G10B10A2_UNORM, 2, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"I_COLOUR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 2, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    };
    static const struct { UINT start, count; } draws[] = {{0, 5}, {98, 9}, {7, 3}};
    static const char ps_src[] = "float4 main() : SV_Target { return 0; }\n";
    HMODULE comp = LoadLibraryA("d3dcompiler_47.dll");
    d3dcompile_fn compile = comp ? (d3dcompile_fn)GetProcAddress(comp, "D3DCompile") : NULL;
    D3D12_HEAP_PROPERTIES custom = {D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0, 0, 0};
    D3D12_HEAP_PROPERTIES def = {D3D12_HEAP_TYPE_DEFAULT, 0, 0, 0, 0};
    D3D12_RESOURCE_DESC bd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 0x10000, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                              D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC rh = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    D3D12_ROOT_PARAMETER rp[1];
    D3D12_ROOT_SIGNATURE_DESC rsd;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gd;
    D3D12_VIEWPORT vp = {0, 0, 4, 4, 0, 1};
    D3D12_RECT sc = {0, 0, 4, 4};
    ID3DBlob *vs = NULL, *ps = NULL, *err = NULL, *rsb = NULL;
    ID3D12RootSignature *rs = NULL;
    ID3D12PipelineState *pso = NULL;
    ID3D12CommandQueue *q = NULL;
    ID3D12CommandAllocator *a = NULL;
    ID3D12GraphicsCommandList *l = NULL;
    ID3D12Resource *buf = NULL, *rt = NULL;
    ID3D12DescriptorHeap *rtvh = NULL;
    ID3D12Fence *f = NULL;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    uint8_t *m = NULL;
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    HRESULT hr;

    if (!compile) { logmsg("layout-test: no d3dcompiler_47"); return; }
    hr = compile(ps_src, sizeof(ps_src) - 1, "layoutps", NULL, NULL, "main", "ps_5_0", 0, 0, &ps, NULL);
    if (SUCCEEDED(hr)) hr = compile(layout_probe_vs, sizeof(layout_probe_vs) - 1, "layoutvs", NULL, NULL, "main", "vs_5_0", 0, 0, &vs, &err);
    if (FAILED(hr)) { logmsg("layout-test: compile failed %s", err ? (char *)ID3D10Blob_GetBufferPointer(err) : ""); goto done; }
    memset(rp, 0, sizeof(rp));
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    memset(&rsd, 0, sizeof(rsd));
    rsd.NumParameters = 1;
    rsd.pParameters = rp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    hr = real_serialize_rs(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, NULL);
    if (SUCCEEDED(hr)) hr = ((create_rs_fn)dev_vt_orig[16])(dev, 0, ID3D10Blob_GetBufferPointer(rsb),
                                                           ID3D10Blob_GetBufferSize(rsb), &IID_ID3D12RootSignature, (void **)&rs);
    memset(&gd, 0, sizeof(gd));
    gd.pRootSignature = rs;
    gd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs);
    gd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
    gd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps);
    gd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
    gd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gd.SampleMask = ~0u;
    gd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gd.RasterizerState.DepthClipEnable = TRUE;
    gd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    gd.NumRenderTargets = 1;
    gd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    gd.SampleDesc.Count = 1;
    gd.InputLayout.NumElements = sizeof(els) / sizeof(els[0]);
    gd.InputLayout.pInputElementDescs = els;
    if (explicit_offsets) {
        static D3D12_INPUT_ELEMENT_DESC fixed[sizeof(els) / sizeof(els[0])];
        resolve_append_offsets(&gd.InputLayout, fixed, sizeof(els) / sizeof(els[0]));
        gd.InputLayout.pInputElementDescs = fixed;
    }
    if (SUCCEEDED(hr)) hr = ((create_gpso_fn)dev_vt_orig[10])(dev, &gd, &IID_ID3D12PipelineState, (void **)&pso);
    if (SUCCEEDED(hr)) hr = ((create_queue_fn)dev_vt_orig[8])(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&a);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommittedResource(dev, &custom, D3D12_HEAP_FLAG_NONE, &bd,
                                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
                                                                &IID_ID3D12Resource, (void **)&buf);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommittedResource(dev, &def, D3D12_HEAP_FLAG_NONE, &td,
                                                                D3D12_RESOURCE_STATE_RENDER_TARGET, NULL,
                                                                &IID_ID3D12Resource, (void **)&rt);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateDescriptorHeap(dev, &rh, &IID_ID3D12DescriptorHeap, (void **)&rtvh);
    if (SUCCEEDED(hr)) hr = ((create_fence_fn)dev_vt_orig[36])(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&f);
    if (SUCCEEDED(hr)) hr = ID3D12Resource_Map(buf, 0, NULL, (void **)&m);
    if (FAILED(hr)) { logmsg("layout-test: setup failed %#lx", hr); goto done; }
    rtvh->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtvh, &rtv);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);

    /* buffer map: 0x0000 vertex slot 0 (12 B), 0x0100 slot 1 (28 B), 0x0200 index,
     * 0x1000 instance records (120 x 24 B), 0x4000.. UAV output per draw */
    memset(m, 0, 0x10000);
    { float p[3] = {0.5f, 0.25f, 0.125f}; memcpy(m, p, 12); }
    { uint16_t idx[1] = {0}; memcpy(m + 0x200, idx, 2); }
    for (UINT i = 0; i < 120; i++) layout_record(i, m + 0x1000 + i * 24);

    for (UINT dr = 0; dr < sizeof(draws) / sizeof(draws[0]); dr++) {
        D3D12_GPU_VIRTUAL_ADDRESS va = ID3D12Resource_GetGPUVirtualAddress(buf);
        D3D12_VERTEX_BUFFER_VIEW vb[3] = {{va, 12, 12}, {va + 0x100, 28, 28}, {va + 0x1000, 120 * 24, 24}};
        D3D12_INDEX_BUFFER_VIEW ib = {va + 0x200, 2, DXGI_FORMAT_R16_UINT};
        UINT outo = 0x4000 + dr * 0x1000;
        int bad = 0;
        char line[1024] = "";
        memset(m + outo, 0xff, 0x1000);
        ((create_list_fn)dev_vt_orig[12])(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, pso, &IID_ID3D12GraphicsCommandList, (void **)&l);
        ID3D12GraphicsCommandList_Reset(l, a, pso);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(l, rs);
        ID3D12GraphicsCommandList_SetGraphicsRootUnorderedAccessView(l, 0, va + outo);
        ID3D12GraphicsCommandList_OMSetRenderTargets(l, 1, &rtv, FALSE, NULL);
        ID3D12GraphicsCommandList_RSSetViewports(l, 1, &vp);
        ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(l, D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
        ID3D12GraphicsCommandList_IASetVertexBuffers(l, 0, 3, vb);
        ID3D12GraphicsCommandList_IASetIndexBuffer(l, &ib);
        ID3D12GraphicsCommandList_DrawIndexedInstanced(l, 1, draws[dr].count, 0, 0, draws[dr].start);
        ID3D12GraphicsCommandList_Close(l);
        ((execute_fn)queue_vt_orig[10])(q, 1, (ID3D12CommandList *const *)&l);
        ((signal_fn)queue_vt_orig[14])(q, f, dr + 1);
        ID3D12Fence_SetEventOnCompletion(f, dr + 1, ev);
        WaitForSingleObject(ev, 5000);
        for (UINT k = 0; k < draws[dr].count; k++) {
            const float *got = (const float *)(m + outo + k * 96);
            float want[5][4];
            int ebad = 0;
            layout_expect(draws[dr].start + k, want);
            for (int e = 0; e < 5; e++)
                for (int c = 0; c < 4; c++) {
                    float d = got[e * 4 + c] - want[e][c];
                    if (d < 0) d = -d;
                    if (d > 0.002f) ebad |= 1 << e;
                }
            if (ebad) {
                bad++;
                if (strlen(line) < sizeof(line) - 200)
                    sprintf(line + strlen(line), " [iid %u] bad elems %#x y %.4f %.4f %.4f %.4f want %.4f %.4f %.4f %.4f"
                            " col %.4f %.4f %.4f %.4f want %.4f %.4f %.4f %.4f", k, ebad,
                            got[12], got[13], got[14], got[15], want[3][0], want[3][1], want[3][2], want[3][3],
                            got[16], got[17], got[18], got[19], want[4][0], want[4][1], want[4][2], want[4][3]);
            }
        }
        logmsg("layout-test (%s offsets) DrawIndexed start %u count %u: %d of %u instances wrong%s",
               explicit_offsets ? "explicit" : "append-aligned", draws[dr].start, draws[dr].count, bad, draws[dr].count, line);
        ID3D12GraphicsCommandList_Release(l);
        l = NULL;
    }
done:
    if (pso) ID3D12PipelineState_Release(pso);
    if (buf) ID3D12Resource_Release(buf);
    if (f) ID3D12Fence_Release(f);
    if (rtvh) ID3D12DescriptorHeap_Release(rtvh);
    if (rt) ID3D12Resource_Release(rt);
    if (l) ID3D12GraphicsCommandList_Release(l);
    if (a) ID3D12CommandAllocator_Release(a);
    if (q) ID3D12CommandQueue_Release(q);
    if (rs) ID3D12RootSignature_Release(rs);
    if (rsb) ID3D10Blob_Release(rsb);
    if (vs) ID3D10Blob_Release(vs);
    if (ps) ID3D10Blob_Release(ps);
    if (err) ID3D10Blob_Release(err);
    CloseHandle(ev);
}

/* standalone entry for fh2test.exe: device + hooked queue/list, then both probes */
void WINAPI fh2_run_tests(void)
{
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    ID3D12Device *dev = NULL;
    ID3D12CommandQueue *q = NULL;
    ID3D12CommandAllocator *a = NULL;
    ID3D12GraphicsCommandList *l = NULL;
    if (FAILED(proxy_create_device(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev))) {
        logmsg("tests: no device");
        return;
    }
    ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q);
    ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&a);
    ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, NULL, &IID_ID3D12GraphicsCommandList, (void **)&l);
    self_test(dev);
    op_test(dev);
    gfx_test(dev);
    inst_test(dev);
    append_test(dev);
    start_offset_test(dev);
    srv_test(dev);
    cbv_test(dev);
    stride_test(dev);
    layout_test(dev, FALSE);
    layout_test(dev, TRUE);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    HMODULE real;
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);
    InitializeCriticalSection(&log_cs);
    QueryPerformanceFrequency(&slow_freq);
    InitializeCriticalSection(&list_cs);
    logf = fopen("data\\logs\\fh2port.log", "a");
    /* this DLL is itself named d3d12.dll (loaded by a native override), so the
     * real one is loaded from the system directory by full path */
    {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);
        if (!n || n > MAX_PATH - 12) { logmsg("no system directory"); return FALSE; }
        strcat(path, "\\d3d12.dll");
        real = LoadLibraryA(path);
        if (real == inst) { logmsg("system d3d12.dll resolved to this proxy"); return FALSE; }
    }
    if (!real) { logmsg("cannot load d3d12.dll"); return FALSE; }
    real_create_device = (create_device_fn)GetProcAddress(real, "D3D12CreateDevice");
    real_get_debug_interface = (debug_iface_fn)GetProcAddress(real, "D3D12GetDebugInterface");
    real_serialize_rs = (serialize_rs_fn)GetProcAddress(real, "D3D12SerializeRootSignature");
    resolve_passthrough(real);
    {
        char v[8] = "";
        minimal = GetEnvironmentVariableA("FH2_MIN", v, sizeof(v)) && v[0] == '1';
        logmsg("minimal mode: %s", minimal ? "on (no D3D12 interception)" : "off");
        hide_tiled = !(GetEnvironmentVariableA("FH2_TILED", v, sizeof(v)) && v[0] == '1');
        msaa_cap = !(GetEnvironmentVariableA("FH2_MSAA8", v, sizeof(v)) && v[0] == '0');
        skip_vid = GetEnvironmentVariableA("FH2_SKIP_VID", v, sizeof(v)) && v[0] == '1';
        logmsg("skip base-vertex draws on vertex_id PSOs: %s", skip_vid ? "yes" : "no");
        skip_ei = GetEnvironmentVariableA("FH2_SKIP_EI", v, sizeof(v)) && v[0] == '1';
        logmsg("skip ExecuteIndirect: %s", skip_ei ? "yes" : "no");
        count_on = GetEnvironmentVariableA("FH2_COUNT", v, sizeof(v)) && v[0] == '1';
        logmsg("per-PSO draw counting: %s", count_on ? "on" : "off");
        tint_build = GetEnvironmentVariableA("FH2_TINT", v, sizeof(v)) && v[0] == '1';
        fix_append = !(GetEnvironmentVariableA("FH2_APPEND", v, sizeof(v)) && v[0] == '0');
        quiet = GetEnvironmentVariableA("FH2_QUIET", v, sizeof(v)) && v[0] == '1';
        if (quiet) logmsg("quiet: no periodic log lines, control file not polled");
        {
            char lv[32] = "";
            if (GetEnvironmentVariableA("FH2_LABEL", lv, sizeof(lv))) {
                label_addr = (volatile LONG *)(uintptr_t)strtoull(lv, NULL, 16);
                logmsg("label watch at %p", (void *)label_addr);
            }
        }
        logmsg("input layouts: APPEND_ALIGNED offsets %s", fix_append ? "written out per slot" : "passed through");
        {
            char pv[64] = "";
            if (GetEnvironmentVariableA("FH2_PATCH", pv, sizeof(pv)) && !strcmp(pv, "auto")) {
                patch_build = patch_auto = TRUE;
                logmsg("patch experiment: every VS with the t64/cb9 path switch forced to the compact path");
            } else if (GetEnvironmentVariableA("FH2_PATCH", pv, sizeof(pv)) &&
                sscanf(pv, "%x:%u:%x", &patch_hash, &patch_dword, &patch_value) == 3) {
                patch_build = TRUE;
                logmsg("patch experiment: hash %08x VS dword %u := %08x", patch_hash, patch_dword, patch_value);
            }
        }
        logmsg("magenta twins of instanced PSOs: %s", tint_build ? "built" : "off");
        logmsg("tiled resources %s", hide_tiled ? "hidden from the wrapper" : "reported");
    }
    {
        char v[8] = "";
        if (!(GetEnvironmentVariableA("FH2_FIBER", v, sizeof(v)) && v[0] == '0') && GetModuleHandleA("azimuth.exe")) {
            AddVectoredExceptionHandler(1, fiber_fault_handler);
            CloseHandle(CreateThread(NULL, 0, game_fixup_thread, NULL, 0, NULL));
        }
        prio_target = GetEnvironmentVariableA("FH2_PRIO", v, sizeof(v)) ? atoi(v) : 1;
        if (prio_target > 0 && GetModuleHandleA("azimuth.exe"))
            CloseHandle(CreateThread(NULL, 0, prio_thread, NULL, 0, NULL));
        flush_nolock = GetEnvironmentVariableA("FH2_FLUSH", v, sizeof(v)) && !strcmp(v, "nolock");
        flush_skip = GetEnvironmentVariableA("FH2_FLUSH", v, sizeof(v)) && !strcmp(v, "skip");
        if (!(GetEnvironmentVariableA("FH2_FLUSH", v, sizeof(v)) && !strcmp(v, "real"))) {
            flush_page = VirtualAlloc(NULL, FLUSH_PAGE_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
            if (!flush_page) logmsg("flush: VirtualAlloc failed, using the real call");
        }
        {
            int mask = GetEnvironmentVariableA("FH2_VMSTATS", v, sizeof(v)) ? atoi(v) : 0;
            BOOL place = !(GetEnvironmentVariableA("FH2_PLACE", v, sizeof(v)) && v[0] == '0');
            if ((mask > 0 || place) && GetModuleHandleA("azimuth.exe")) hook_vm(mask, place);
        }
        if (!(GetEnvironmentVariableA("FH2_FLUSH", v, sizeof(v)) && v[0] == '0') && GetModuleHandleA("azimuth.exe") &&
            !hook_flush_ntdll() && !hook_flush(GetModuleHandleA("azimuth.exe")))
            logmsg("FlushProcessWriteBuffers: azimuth.exe's import not found, left alone");
    }
    logmsg("fh2port d3d12 proxy loaded, real d3d12 %p", real);
    return real_create_device && real_get_debug_interface && real_serialize_rs;
}
