#include "WorkerProtocol.h"

#include <cstring>
#include <string>
#include <vector>

namespace ecapture {
namespace worker {
namespace {

// 一律显式小端：同一台机器上两边字节序本来一致，但把字节序写死在编码里，
// 解码就不需要"猜这台机器的对齐和字节序"，也方便离线判据直接按字节构造畸形消息。
void PutU16(std::vector<uint8_t>* b, uint16_t v) {
    b->push_back(static_cast<uint8_t>(v & 0xFF));
    b->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
void PutU32(std::vector<uint8_t>* b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b->push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}
void PutU64(std::vector<uint8_t>* b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b->push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}

bool TakeU16(const uint8_t** p, size_t* left, uint16_t* out) {
    if (*left < 2) return false;
    *out = static_cast<uint16_t>((*p)[0]) | static_cast<uint16_t>((*p)[1] << 8);
    *p += 2; *left -= 2;
    return true;
}
bool TakeU32(const uint8_t** p, size_t* left, uint32_t* out) {
    if (*left < 4) return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>((*p)[i]) << (i * 8);
    *out = v;
    *p += 4; *left -= 4;
    return true;
}
bool TakeU64(const uint8_t** p, size_t* left, uint64_t* out) {
    if (*left < 8) return false;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>((*p)[i]) << (i * 8);
    *out = v;
    *p += 8; *left -= 8;
    return true;
}

// 字符串：长度按 UTF-16 码元数记，超过上限就是整条消息作废（而不是截断后继续用）。
void PutString(std::vector<uint8_t>* b, const std::wstring& s) {
    PutU32(b, static_cast<uint32_t>(s.size()));
    if (!s.empty()) {
        const auto* src = reinterpret_cast<const uint8_t*>(s.data());
        b->insert(b->end(), src, src + s.size() * sizeof(wchar_t));
    }
}

bool TakeString(const uint8_t** p, size_t* left, std::wstring* out) {
    uint32_t units = 0;
    if (!TakeU32(p, left, &units)) return false;
    if (units > kMaxStringUnits) return false;
    const size_t bytes = static_cast<size_t>(units) * sizeof(wchar_t);
    if (*left < bytes) return false;
    out->assign(reinterpret_cast<const wchar_t*>(*p), units);
    *p += bytes; *left -= bytes;
    return true;
}

void PutI32(std::vector<uint8_t>* b, int32_t v) { PutU32(b, static_cast<uint32_t>(v)); }
bool TakeI32(const uint8_t** p, size_t* left, int32_t* out) {
    uint32_t v = 0;
    if (!TakeU32(p, left, &v)) return false;
    *out = static_cast<int32_t>(v);
    return true;
}

void PutWindowInfo(std::vector<uint8_t>* b, const WindowInfo& w) {
    PutU64(b, w.hwnd);
    PutU32(b, w.pid);
    // 枚举那一刻的进程创建时间：父进程拿它做身份复核的基线，所以它必须与 hwnd/pid 同源，
    // 不能让父进程事后自己再问一次（见 WindowIdentity.h）。
    PutU64(b, w.processStartTicks);
    PutI32(b, w.x);
    PutI32(b, w.y);
    PutI32(b, w.width);
    PutI32(b, w.height);
    PutI32(b, w.zOrder);
    b->push_back(w.iconic ? 1 : 0);
    PutString(b, w.title);
    PutString(b, w.className);
    PutString(b, w.imageName);
    PutString(b, w.imagePath);
}

bool TakeWindowInfo(const uint8_t** p, size_t* left, WindowInfo* out) {
    WindowInfo w;
    uint8_t iconic = 0;
    if (!TakeU64(p, left, &w.hwnd)) return false;
    if (!TakeU32(p, left, &w.pid)) return false;
    if (!TakeU64(p, left, &w.processStartTicks)) return false;
    if (!TakeI32(p, left, &w.x) || !TakeI32(p, left, &w.y)) return false;
    if (!TakeI32(p, left, &w.width) || !TakeI32(p, left, &w.height)) return false;
    if (!TakeI32(p, left, &w.zOrder)) return false;
    if (*left < 1) return false;
    iconic = **p;
    *p += 1; *left -= 1;
    if (!TakeString(p, left, &w.title)) return false;
    if (!TakeString(p, left, &w.className)) return false;
    if (!TakeString(p, left, &w.imageName)) return false;
    if (!TakeString(p, left, &w.imagePath)) return false;
    // HWND 为 0 的条目没有意义，而它是父进程后面拿去 reinterpret_cast 的句柄：
    // 在解码这一层就挡掉，比让 0 号窗口变成"一个目标"安全。
    if (w.hwnd == 0) return false;
    w.iconic = iconic != 0;
    *out = std::move(w);
    return true;
}

// 条件组：条数有上限，逐条按字符串读（读不出就是整条消息作废）
bool TakeStringList(const uint8_t** p, size_t* left, std::vector<std::wstring>* out) {
    uint32_t n = 0;
    if (!TakeU32(p, left, &n)) return false;
    if (n > kMaxListItems) return false;
    out->clear();
    out->reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        std::wstring s;
        if (!TakeString(p, left, &s)) return false;
        out->push_back(std::move(s));
    }
    return true;
}

template <typename T>
void PutNumberList(std::vector<uint8_t>* b, const std::vector<T>& items) {
    PutU32(b, static_cast<uint32_t>(items.size()));
    for (const T& v : items) PutU64(b, static_cast<uint64_t>(v));
}

template <typename T>
bool TakeNumberList(const uint8_t** p, size_t* left, std::vector<T>* out) {
    uint32_t n = 0;
    if (!TakeU32(p, left, &n)) return false;
    if (n > kMaxListItems) return false;
    out->clear();
    out->reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t raw = 0;
        if (!TakeU64(p, left, &raw)) return false;
        out->push_back(static_cast<T>(raw));
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// 报头
// ---------------------------------------------------------------------------

bool EncodeHeader(const Header& h, std::vector<uint8_t>* out) {
    if (h.payloadLen > kMaxReplyPayload) return false;
    out->clear();
    out->reserve(kHeaderSize);
    PutU32(out, h.magic);
    PutU16(out, h.version);
    PutU16(out, h.kind);
    PutU32(out, h.payloadLen);
    PutU32(out, 0);   // 保留段：写 0，读的时候必须也是 0
    PutU64(out, h.nonce);
    return out->size() == kHeaderSize;
}

bool DecodeHeader(const uint8_t* data, size_t size, Header* out) {
    if (!data || size < kHeaderSize) return false;
    const uint8_t* p = data;
    size_t left = size;
    Header h;
    uint32_t reserved = 0;
    if (!TakeU32(&p, &left, &h.magic)) return false;
    if (!TakeU16(&p, &left, &h.version)) return false;
    if (!TakeU16(&p, &left, &h.kind)) return false;
    if (!TakeU32(&p, &left, &h.payloadLen)) return false;
    if (!TakeU32(&p, &left, &reserved) || reserved != 0) return false;
    if (!TakeU64(&p, &left, &h.nonce)) return false;
    if (h.magic != kMagic) return false;
    if (h.version != kProtocolVersion) return false;
    if (h.payloadLen > kMaxReplyPayload) return false;
    *out = h;
    return true;
}

// ---------------------------------------------------------------------------
// 任务（父 -> 子）
// ---------------------------------------------------------------------------

bool EncodeTask(const Task& task, std::vector<uint8_t>* out) {
    std::vector<uint8_t> b;
    PutU16(&b, task.kind);
    PutU16(&b, 0);   // 对齐用，保留
    PutU64(&b, task.hwnd);
    PutU32(&b, task.waitMs);
    PutU32(&b, 0);

    PutNumberList(&b, task.match.hwnds);
    PutNumberList(&b, task.match.pids);
    PutU32(&b, static_cast<uint32_t>(task.match.processes.size()));
    for (const auto& s : task.match.processes) PutString(&b, s);
    PutU32(&b, static_cast<uint32_t>(task.match.exePaths.size()));
    for (const auto& s : task.match.exePaths) PutString(&b, s);
    PutU32(&b, static_cast<uint32_t>(task.match.titles.size()));
    for (const auto& s : task.match.titles) PutString(&b, s);
    PutU32(&b, static_cast<uint32_t>(task.match.titleContains.size()));
    for (const auto& s : task.match.titleContains) PutString(&b, s);
    PutU32(&b, static_cast<uint32_t>(task.match.titleRegexes.size()));
    for (const auto& s : task.match.titleRegexes) PutString(&b, s);
    PutU32(&b, static_cast<uint32_t>(task.match.classes.size()));
    for (const auto& s : task.match.classes) PutString(&b, s);

    PutU32(&b, static_cast<uint32_t>(task.onScreens.size()));
    for (const RECT& r : task.onScreens) {
        PutI32(&b, r.left);
        PutI32(&b, r.top);
        PutI32(&b, r.right);
        PutI32(&b, r.bottom);
    }
    if (b.size() > kMaxTaskPayload) return false;
    *out = std::move(b);
    return true;
}

bool DecodeTask(const uint8_t* data, size_t size, Task* out) {
    if (!data || size > kMaxTaskPayload) return false;   // 报头已经限过，这里再核一次
    const uint8_t* p = data;
    size_t left = size;
    Task t;
    uint16_t pad16 = 0;
    uint32_t pad32 = 0;
    if (!TakeU16(&p, &left, &t.kind)) return false;
    if (!TakeU16(&p, &left, &pad16) || pad16 != 0) return false;
    if (!TakeU64(&p, &left, &t.hwnd)) return false;
    if (!TakeU32(&p, &left, &t.waitMs)) return false;
    if (t.waitMs > kMaxTaskWaitMs) return false;   // 不许用等待时长把辅助进程按在原地
    if (!TakeU32(&p, &left, &pad32) || pad32 != 0) return false;

    if (!TakeNumberList(&p, &left, &t.match.hwnds)) return false;
    if (!TakeNumberList(&p, &left, &t.match.pids)) return false;
    if (!TakeStringList(&p, &left, &t.match.processes)) return false;
    if (!TakeStringList(&p, &left, &t.match.exePaths)) return false;
    if (!TakeStringList(&p, &left, &t.match.titles)) return false;
    if (!TakeStringList(&p, &left, &t.match.titleContains)) return false;
    if (!TakeStringList(&p, &left, &t.match.titleRegexes)) return false;
    if (!TakeStringList(&p, &left, &t.match.classes)) return false;

    uint32_t screens = 0;
    if (!TakeU32(&p, &left, &screens)) return false;
    if (screens > kMaxListItems) return false;
    for (uint32_t i = 0; i < screens; ++i) {
        // RECT 的成员是 LONG，与 int32_t 同宽但不是同一个类型：先读到局部变量再落进矩形
        int32_t v[4] = {0, 0, 0, 0};
        for (int32_t& item : v) {
            if (!TakeI32(&p, &left, &item)) return false;
        }
        RECT r{v[0], v[1], v[2], v[3]};
        if (r.right <= r.left || r.bottom <= r.top) return false;   // 空矩形不是合法屏幕
        t.onScreens.push_back(r);
    }
    // 字节必须正好读完：多一个字节意味着对方发的是另一种（或更新版本的）格式，
    // 少一个字节是被截断。两种都不能当成"解析成功"。
    if (left != 0) return false;

    if (t.kind != kTaskPrintWindow && t.kind != kTaskDwmThumbnail && t.kind != kTaskMatchWindows) {
        return false;
    }
    *out = std::move(t);
    return true;
}

// ---------------------------------------------------------------------------
// 应答（子 -> 父）
// ---------------------------------------------------------------------------

bool FrameBytes(const Reply& reply, uint64_t* out) {
    if (reply.width == 0 || reply.height == 0) { *out = 0; return true; }
    if (reply.width > kMaxFrameSide || reply.height > kMaxFrameSide) return false;
    if (reply.stride < static_cast<uint64_t>(reply.width) * 4u) return false;
    const uint64_t bytes = static_cast<uint64_t>(reply.stride) * reply.height;
    if (bytes > kMaxFrameBytes) return false;
    *out = bytes;
    return true;
}

bool EncodeReply(const Reply& reply, std::vector<uint8_t>* out) {
    std::vector<uint8_t> b;
    PutU32(&b, static_cast<uint32_t>(reply.status));
    PutU32(&b, reply.win32);
    PutU64(&b, static_cast<uint64_t>(static_cast<int64_t>(reply.hresult)));
    PutString(&b, reply.detail);
    PutU32(&b, reply.width);
    PutU32(&b, reply.height);
    PutU32(&b, reply.stride);

    uint64_t frameBytes = 0;
    if (!FrameBytes(reply, &frameBytes)) return false;
    if (reply.pixels.size() != frameBytes) return false;   // 行距 x 行数与实际字节数必须自洽

    PutU32(&b, static_cast<uint32_t>(reply.hits.size()));
    PutU32(&b, static_cast<uint32_t>(reply.iconic.size()));
    if (reply.hits.size() + reply.iconic.size() > kMaxWindowEntries) return false;
    for (const WindowInfo& w : reply.hits) PutWindowInfo(&b, w);
    for (const WindowInfo& w : reply.iconic) PutWindowInfo(&b, w);
    b.insert(b.end(), reply.pixels.begin(), reply.pixels.end());
    if (b.size() > kMaxReplyPayload) return false;
    *out = std::move(b);
    return true;
}

bool DecodeReply(const uint8_t* data, size_t size, Reply* out) {
    if (!data) return false;
    if (size > kMaxReplyPayload) return false;
    const uint8_t* p = data;
    size_t left = size;
    Reply r;
    uint32_t status = 0;
    if (!TakeU32(&p, &left, &status)) return false;
    // 状态码必须是已列出的那几种之一：未知状态说明两边协议不同，整条作废，
    // 不能"当成失败但不知道失败在哪"，更不能反过来当成 kOk。
    if (status > kMaxWireStatus) return false;
    r.status = static_cast<BlockedStatus>(status);
    uint32_t win32 = 0;
    if (!TakeU32(&p, &left, &win32)) return false;
    r.win32 = win32;
    uint64_t hr64 = 0;
    if (!TakeU64(&p, &left, &hr64)) return false;
    r.hresult = static_cast<HRESULT>(static_cast<int32_t>(static_cast<uint32_t>(hr64)));
    if (!TakeString(&p, &left, &r.detail)) return false;
    if (!TakeU32(&p, &left, &r.width)) return false;
    if (!TakeU32(&p, &left, &r.height)) return false;
    if (!TakeU32(&p, &left, &r.stride)) return false;

    // 形状先于内容判定：宽高不合法就没有"剩下的字节是像素"这回事
    if (r.width > kMaxFrameSide || r.height > kMaxFrameSide) return false;
    if ((r.width == 0) != (r.height == 0)) return false;
    if (r.status != BlockedStatus::kOk && (r.width != 0 || r.height != 0)) return false;

    uint32_t hitCount = 0;
    uint32_t iconicCount = 0;
    if (!TakeU32(&p, &left, &hitCount)) return false;
    if (!TakeU32(&p, &left, &iconicCount)) return false;
    if (hitCount + iconicCount > kMaxWindowEntries) return false;

    std::vector<WindowInfo> hits;
    std::vector<WindowInfo> iconic;
    hits.reserve(hitCount);
    iconic.reserve(iconicCount);
    for (uint32_t i = 0; i < hitCount; ++i) {
        WindowInfo w;
        if (!TakeWindowInfo(&p, &left, &w)) return false;
        hits.push_back(std::move(w));
    }
    for (uint32_t i = 0; i < iconicCount; ++i) {
        WindowInfo w;
        if (!TakeWindowInfo(&p, &left, &w)) return false;
        iconic.push_back(std::move(w));
    }

    // 最后剩下的字节全部是像素；字节数与"行距 x 行数"必须严丝合缝，
    // 少一个字节就是被截断的帧 —— 那比报错更糟，它看起来像一张图。
    const uint64_t frameBytes = static_cast<uint64_t>(r.stride) * r.height;
    if (r.height > 0 && r.stride < static_cast<uint64_t>(r.width) * 4u) return false;
    if (frameBytes > kMaxFrameBytes) return false;
    if (left != static_cast<size_t>(frameBytes)) return false;
    r.pixels.assign(p, p + left);
    r.hits = std::move(hits);
    r.iconic = std::move(iconic);
    *out = std::move(r);
    return true;
}

}  // namespace worker
}  // namespace ecapture
