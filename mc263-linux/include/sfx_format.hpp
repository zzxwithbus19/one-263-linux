// sfx_format.hpp -- 单文件布局：[launcher ELF][game.zip][32 字节 trailer]
//
// ELF 加载器只按 program headers 映射段，尾部追加的字节会被忽略，所以
// 追加 payload 不会破坏可执行文件。trailer 固定 32 字节，启动器从文件末尾
// 反查 magic 得到 payload 的偏移 / 大小 / CRC32。
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ostream>
#include <string>

namespace mc263 {

constexpr char     SFX_MAGIC[8] = {'M', 'C', '2', '6', '3', 'S', 'F', 'X'};
constexpr char     SFX_END[8]   = {'M', 'C', '2', '6', '3', 'E', 'N', 'D'};
constexpr uint32_t SFX_VERSION       = 1; // payload = 原始 STORED zip（零第三方运行库）
constexpr uint32_t SFX_VERSION_ZSTD  = 2; // payload = zstd 压缩的 zip（使用系统 libzstd）
constexpr size_t   SFX_TRAILER_SIZE  = 32;

struct SfxTrailer {
    uint64_t payloadSize = 0;
    uint32_t payloadCrc = 0;
    uint32_t version = 0;
};

inline void encodeLe64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
inline void encodeLe32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
inline uint64_t decodeLe64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}
inline uint32_t decodeLe32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}

inline std::string makeTrailer(uint64_t payloadSize, uint32_t payloadCrc,
                               uint32_t version = SFX_VERSION) {
    std::string t(SFX_TRAILER_SIZE, '\0');
    uint8_t* p = reinterpret_cast<uint8_t*>(&t[0]);
    std::memcpy(p, SFX_MAGIC, 8);
    encodeLe64(p + 8, payloadSize);
    encodeLe32(p + 16, payloadCrc); // 始终存「原始 game.zip」的 CRC32
    encodeLe32(p + 20, version);
    std::memcpy(p + 24, SFX_END, 8);
    return t;
}

// hasPayload=false 表示这是个「裸」启动器（开发调试 / rpm 分离安装场景）
inline bool readTrailer(FILE* f, uint64_t fileSize, SfxTrailer& out, bool& hasPayload, std::string& err) {
    hasPayload = false;
    err.clear();
    if (fileSize < SFX_TRAILER_SIZE) return true;
    uint8_t p[SFX_TRAILER_SIZE];
    if (fseeko(f, static_cast<off_t>(fileSize - SFX_TRAILER_SIZE), SEEK_SET) != 0 ||
        std::fread(p, 1, SFX_TRAILER_SIZE, f) != SFX_TRAILER_SIZE) {
        err = "无法读取文件尾部";
        return false;
    }
    if (std::memcmp(p, SFX_MAGIC, 8) != 0 || std::memcmp(p + 24, SFX_END, 8) != 0) return true;
    out.payloadSize = decodeLe64(p + 8);
    out.payloadCrc = decodeLe32(p + 16);
    out.version = decodeLe32(p + 20);
    if (out.version != SFX_VERSION && out.version != SFX_VERSION_ZSTD) {
        err = "payload 格式版本不匹配 (" + std::to_string(out.version) + ")";
        return false;
    }
    if (out.payloadSize == 0 || out.payloadSize + SFX_TRAILER_SIZE > fileSize) {
        err = "payload 大小异常";
        return false;
    }
    hasPayload = true;
    return true;
}

} // namespace mc263
