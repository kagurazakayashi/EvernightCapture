#include "WorkerFraming.h"

namespace ecapture {
namespace worker {

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
void PutI32(std::vector<uint8_t>* b, int32_t v) { PutU32(b, static_cast<uint32_t>(v)); }
bool TakeI32(const uint8_t** p, size_t* left, int32_t* out) {
    uint32_t v = 0;
    if (!TakeU32(p, left, &v)) return false;
    *out = static_cast<int32_t>(v);
    return true;
}

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

}  // namespace worker
}  // namespace ecapture
