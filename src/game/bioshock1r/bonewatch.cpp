// Route B probe - see bonewatch.h.

#include "game/bioshock1r/bonewatch.h"

#include "core/util/log.h"

#include <windows.h>

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <vector>

namespace bvr::b1r::bonewatch {
namespace {

// Left-hand bones to watch (patterns.h: wrist 6, elbow 5, fingers 7-21).
constexpr int kBones[4] = {6, 5, 10, 16};
constexpr size_t kQtsBytes = 48;
constexpr uint64_t kCaptureMs = 2000;

enum : uint8_t { kHit = 0, kDriveBegin = 1, kDriveEnd = 2 };
struct Ev {
    uint8_t kind, dr, inExe, n;
    uint32_t eip;      // RVA in its module
    uint32_t stack[6]; // game-exe return RVAs
};
constexpr uint32_t kRing = 1u << 18; // 8 MB: the renderer reads these bones many times a frame
Ev g_ring[kRing];
std::atomic<uint32_t> g_next{0};
std::atomic<bool> g_recording{false};

uintptr_t g_exeLo = 0, g_exeHi = 0;
uintptr_t g_modLo = 0; // bioshockvr.dll
PVOID g_veh = nullptr;

// Capture state machine (game thread).
enum { kIdle, kPending, kRunning, kStopping, kFlush };
std::atomic<int> g_state{kIdle};
std::atomic<int> g_requestCasting{-1}; // -1 none, 0 baseline, 1 casting, 2 custom
uintptr_t g_customAddr[4] = {};
char g_label[4][40] = {"bone 6", "bone 5", "bone 10", "bone 16"};
bool g_customRun = false;
bool g_casting = false;
uint64_t g_startMs = 0;
std::atomic<bool> g_helperDone{false};
std::set<std::vector<uint32_t>> g_baselineKeys;
char g_status[160] = "idle";

void push(uint8_t kind, uint8_t dr, const CONTEXT* c) {
    if (!g_recording.load(std::memory_order_relaxed)) return;
    const uint32_t i = g_next.fetch_add(1, std::memory_order_relaxed);
    if (i >= kRing) return;
    Ev& e = g_ring[i];
    e.kind = kind;
    e.dr = dr;
    e.n = 0;
    e.eip = 0;
    e.inExe = 0;
    if (!c) return;
    const uintptr_t eip = c->Eip;
    if (eip >= g_exeLo && eip < g_exeHi) {
        e.inExe = 1;
        e.eip = static_cast<uint32_t>(eip - g_exeLo);
    } else {
        e.eip = static_cast<uint32_t>(eip - g_modLo);
    }
    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    uintptr_t lo = c->Esp & ~uintptr_t(3);
    uintptr_t hi = lo + 1536;
    const uintptr_t base = reinterpret_cast<uintptr_t>(tib->StackBase);
    if (hi > base) hi = base;
    for (uintptr_t p = lo; p + 4 <= hi && e.n < 6; p += 4) {
        const uintptr_t v = *reinterpret_cast<const uint32_t*>(p);
        if (v < g_exeLo + 0x1006 || v >= g_exeHi) continue;
        const uint8_t* cc = reinterpret_cast<const uint8_t*>(v);
        if (!(cc[-5] == 0xE8 || cc[-2] == 0xFF || cc[-3] == 0xFF || cc[-6] == 0xFF)) continue;
        e.stack[e.n++] = static_cast<uint32_t>(v - g_exeLo);
    }
}

LONG CALLBACK veh(EXCEPTION_POINTERS* x) {
    if (x->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = x->ContextRecord;
    const DWORD hits = c->Dr6 & 0xF;
    if (!hits) return EXCEPTION_CONTINUE_SEARCH; // a real single-step: not ours
    for (int i = 0; i < 4; ++i)
        if (hits & (1u << i)) push(kHit, static_cast<uint8_t>(i), c);
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

struct HelperArgs {
    DWORD tid;
    uintptr_t addr[4];
    bool enable;
};

DWORD WINAPI helper(LPVOID p) {
    HelperArgs* a = static_cast<HelperArgs*>(p);
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE,
                          a->tid);
    if (t) {
        if (SuspendThread(t) != static_cast<DWORD>(-1)) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c)) {
                if (a->enable) {
                    c.Dr0 = a->addr[0];
                    c.Dr1 = a->addr[1];
                    c.Dr2 = a->addr[2];
                    c.Dr3 = a->addr[3];
                    DWORD dr7 = 0;
                    for (int i = 0; i < 4; ++i)
                        dr7 |= (1u << (2 * i)) | (3u << (16 + 4 * i)) | (3u << (18 + 4 * i));
                    c.Dr7 = dr7; // local enable, read/write, 4 bytes
                } else {
                    c.Dr0 = c.Dr1 = c.Dr2 = c.Dr3 = 0;
                    c.Dr7 = 0;
                }
                c.Dr6 = 0;
                SetThreadContext(t, &c);
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    delete a;
    g_helperDone.store(true, std::memory_order_release);
    return 0;
}

void spawn_helper(bool enable, const uintptr_t addr[4]) {
    auto* a = new HelperArgs{GetCurrentThreadId(), {}, enable};
    if (addr) memcpy(a->addr, addr, sizeof a->addr);
    g_helperDone.store(false, std::memory_order_relaxed);
    HANDLE h = CreateThread(nullptr, 0, helper, a, 0, nullptr);
    if (h) CloseHandle(h);
    else {
        delete a;
        g_helperDone.store(true, std::memory_order_relaxed);
    }
}

std::vector<uint32_t> key_of(const Ev& e) {
    std::vector<uint32_t> k{e.inExe, e.eip};
    for (int i = 0; i < e.n && i < 4; ++i) k.push_back(e.stack[i]);
    return k;
}

void flush() {
    const uint32_t n = std::min(g_next.load(), kRing);
    struct Agg {
        uint32_t count = 0, drMask = 0, inDrive = 0;
        const Ev* sample = nullptr;
    };
    std::map<std::vector<uint32_t>, Agg> agg;
    bool inDrive = false;
    uint32_t drives = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const Ev& e = g_ring[i];
        if (e.kind == kDriveBegin) { inDrive = true; ++drives; continue; }
        if (e.kind == kDriveEnd) { inDrive = false; continue; }
        Agg& a = agg[key_of(e)];
        ++a.count;
        a.drMask |= 1u << e.dr;
        if (inDrive) ++a.inDrive;
        if (!a.sample) a.sample = &e;
    }
    BVR_LOG("[bonewatch] %s capture: %u events, %u drive passes, %u distinct access sites%s",
            g_customRun ? "EFFECTS" : g_casting ? "CASTING" : "BASELINE", n, drives,
            static_cast<unsigned>(agg.size()),
            n >= kRing ? " (ring FULL - truncated)" : "");
    std::vector<std::pair<std::vector<uint32_t>, Agg>> rows(agg.begin(), agg.end());
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
    int printed = 0;
    for (const auto& [k, a] : rows) {
        const bool isNew = g_casting && !g_baselineKeys.count(k);
        if (g_casting && !isNew && printed > 40) continue;
        char bones[180];
        int bl = 0;
        for (int i = 0; i < 4; ++i)
            if (a.drMask & (1u << i))
                bl += _snprintf_s(bones + bl, sizeof bones - bl, _TRUNCATE, "%s%s", bl ? "," : "",
                                  g_label[i]);
        char line[400];
        int len = _snprintf_s(line, sizeof line, _TRUNCATE,
                              "[bonewatch] %s x%u [%s] | %s eip %s+0x%X | stack:",
                              isNew ? "NEW-WHILE-CASTING" : "site", a.count, bones,
                              a.inDrive == a.count ? "IN our drive"
                              : a.inDrive      ? "partly in drive"
                                               : "outside drive",
                              a.sample->inExe ? "exe" : "mod", a.sample->eip);
        for (int i = 0; i < a.sample->n && len > 0 && len < 380; ++i)
            len += _snprintf_s(line + len, sizeof line - len, _TRUNCATE, " 0x%X",
                               a.sample->stack[i]);
        BVR_LOG("%s", line);
        if (++printed >= 60) break;
    }
    // One frame's order, from the middle of the capture: every access site in
    // sequence between two drive-begin marks (consecutive repeats collapsed).
    uint32_t mid = n / 2, s0 = 0, s1 = 0;
    for (uint32_t i = mid; i < n; ++i)
        if (g_ring[i].kind == kDriveBegin) { s0 = i; break; }
    for (uint32_t i = s0 + 1; i < n; ++i)
        if (g_ring[i].kind == kDriveBegin) { s1 = i; break; }
    if (s1 > s0) {
        std::map<std::vector<uint32_t>, int> ids;
        for (const auto& r : rows) ids.emplace(r.first, static_cast<int>(ids.size()));
        char line[900];
        int len = _snprintf_s(line, sizeof line, _TRUNCATE,
                              "[bonewatch] one frame (site # = row order above; B/E = our "
                              "drive):");
        std::vector<uint32_t> last;
        for (uint32_t i = s0; i <= s1 && len > 0 && len < 880; ++i) {
            const Ev& e = g_ring[i];
            if (e.kind == kDriveBegin) { len += _snprintf_s(line + len, sizeof line - len, _TRUNCATE, " B"); last.clear(); continue; }
            if (e.kind == kDriveEnd) { len += _snprintf_s(line + len, sizeof line - len, _TRUNCATE, " E"); last.clear(); continue; }
            auto k = key_of(e);
            if (k == last) continue;
            last = k;
            len += _snprintf_s(line + len, sizeof line - len, _TRUNCATE, " #%d", ids[k]);
        }
        BVR_LOG("%s", line);
    }
    if (!g_casting && !g_customRun) {
        g_baselineKeys.clear();
        for (const auto& r : rows) g_baselineKeys.insert(r.first);
    }
    _snprintf_s(g_status, sizeof g_status, _TRUNCATE, "%s capture logged (%u events)",
                g_casting ? "casting" : "baseline", n);
}

} // namespace

void mark_drive_begin() { push(kDriveBegin, 0, nullptr); }
void mark_drive_end() { push(kDriveEnd, 0, nullptr); }

void request(bool casting) {
    g_requestCasting.store(casting ? 1 : 0, std::memory_order_relaxed);
}

void request_custom(const uintptr_t addr[4], const char* const labels[4]) {
    if (g_state.load(std::memory_order_relaxed) != kIdle) return;
    for (int i = 0; i < 4; ++i) {
        g_customAddr[i] = addr[i];
        strncpy_s(g_label[i], labels[i] ? labels[i] : "-", _TRUNCATE);
    }
    g_requestCasting.store(2, std::memory_order_relaxed);
}

void tick(void* boneArrayBase, int boneCount) {
    const uint64_t now = GetTickCount64();
    const int st = g_state.load(std::memory_order_relaxed);
    if (st == kIdle) {
        const int req = g_requestCasting.exchange(-1, std::memory_order_relaxed);
        if (req < 0) return;
        if (!boneArrayBase || boneCount <= 16) {
            strcpy_s(g_status, "no hands skeleton yet - raise a plasmid first");
            return;
        }
        if (!g_exeLo) {
            const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
            auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(exe + dos->e_lfanew);
            g_exeLo = exe;
            g_exeHi = exe + nt->OptionalHeader.SizeOfImage;
            HMODULE me = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&veh), &me);
            g_modLo = reinterpret_cast<uintptr_t>(me);
        }
        if (!g_veh) g_veh = AddVectoredExceptionHandler(1, veh);
        uintptr_t addr[4];
        g_customRun = req == 2;
        for (int i = 0; i < 4; ++i) {
            if (g_customRun) {
                addr[i] = g_customAddr[i];
            } else {
                addr[i] = reinterpret_cast<uintptr_t>(boneArrayBase) + kBones[i] * kQtsBytes;
                _snprintf_s(g_label[i], sizeof g_label[i], _TRUNCATE, "bone %d", kBones[i]);
            }
        }
        g_casting = req == 1;
        g_next.store(0, std::memory_order_relaxed);
        g_recording.store(true, std::memory_order_relaxed);
        spawn_helper(true, addr);
        g_startMs = now;
        g_state.store(kRunning, std::memory_order_relaxed);
        _snprintf_s(g_status, sizeof g_status, _TRUNCATE, "recording %s...",
                    g_casting ? "while casting" : "baseline");
        BVR_LOG("[bonewatch] %s capture armed on [%s | %s | %s | %s] (game thread %lu)",
                g_customRun ? "EFFECTS" : g_casting ? "CASTING" : "BASELINE", g_label[0],
                g_label[1], g_label[2], g_label[3], GetCurrentThreadId());
    } else if (st == kRunning && now - g_startMs >= kCaptureMs) {
        spawn_helper(false, nullptr);
        g_state.store(kStopping, std::memory_order_relaxed);
    } else if (st == kStopping && g_helperDone.load(std::memory_order_acquire)) {
        g_recording.store(false, std::memory_order_relaxed);
        flush();
        g_state.store(kIdle, std::memory_order_relaxed);
    }
}

void draw_debug_ui() {
    ImGui::Text("Plasmid-effects timing probe (Route B):");
    if (ImGui::Button("Watch hand bones: baseline (2 s, don't cast)")) request(false);
    if (ImGui::Button("Watch hand bones: while casting (2 s)")) request(true);
    ImGui::TextWrapped("%s", g_status);
}

} // namespace bvr::b1r::bonewatch
