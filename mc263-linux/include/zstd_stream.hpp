// zstd_stream.hpp -- 使用系统 libzstd 对 payload 做流式压缩/解压
// Fedora / openSUSE Tumbleweed 均自带 libzstd（pkg-config libzstd）。
// 只在 MC263_WITH_ZSTD 时编译；未启用时单文件退化为 v1（纯 STORED，零第三方运行库）。
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include <zstd.h>

#include "fs_util.hpp"

namespace mc263::zstd {

namespace fs = std::filesystem;
constexpr size_t kChunk = 1u << 20;

// 把 src 压缩为 dst（zstd 帧内带 checksum 与原始大小，解压端无需额外字段）
inline bool compressFile(const fs::path& src, const fs::path& dst,
                         uint64_t& outSize, std::string& err) {
    FILE* in = std::fopen(src.string().c_str(), "rb");
    FILE* out = std::fopen(dst.string().c_str(), "wb");
    if (!in || !out) { err = "打开压缩输入/输出失败"; return false; }
    ZSTD_CCtx* ctx = ZSTD_createCCtx();
    if (!ctx) { err = "ZSTD_createCCtx 失败"; std::fclose(in); std::fclose(out); return false; }
    uint64_t srcSize = mc263::fsx::fileSize(src);
    ZSTD_CCtx_setParameter(ctx, ZSTD_c_compressionLevel, 3); // 快速档：jar/ogg 已压缩
    ZSTD_CCtx_setParameter(ctx, ZSTD_c_checksumFlag, 1);
    ZSTD_CCtx_setPledgedSrcSize(ctx, srcSize);

    std::vector<uint8_t> ibuf(kChunk), obuf(kChunk);
    size_t got;
    while ((got = std::fread(ibuf.data(), 1, ibuf.size(), in)) > 0) {
        ZSTD_inBuffer input{ibuf.data(), got, 0};
        while (input.pos < input.size) {
            ZSTD_outBuffer output{obuf.data(), obuf.size(), 0};
            size_t ret = ZSTD_compressStream2(ctx, &output, &input, ZSTD_e_continue);
            if (ZSTD_isError(ret)) {
                err = std::string("zstd 压缩失败: ") + ZSTD_getErrorName(ret);
                ZSTD_freeCCtx(ctx); std::fclose(in); std::fclose(out);
                return false;
            }
            if (output.pos && std::fwrite(obuf.data(), 1, output.pos, out) != output.pos) {
                err = "写压缩数据失败";
                ZSTD_freeCCtx(ctx); std::fclose(in); std::fclose(out);
                return false;
            }
        }
    }
    ZSTD_inBuffer empty{nullptr, 0, 0};
    size_t ret;
    do {
        ZSTD_outBuffer output{obuf.data(), obuf.size(), 0};
        ret = ZSTD_compressStream2(ctx, &output, &empty, ZSTD_e_end);
        if (ZSTD_isError(ret)) {
            err = std::string("zstd 结束帧失败: ") + ZSTD_getErrorName(ret);
            ZSTD_freeCCtx(ctx); std::fclose(in); std::fclose(out);
            return false;
        }
        if (output.pos && std::fwrite(obuf.data(), 1, output.pos, out) != output.pos) {
            err = "写压缩尾帧失败";
            ZSTD_freeCCtx(ctx); std::fclose(in); std::fclose(out);
            return false;
        }
    } while (ret > 0);

    outSize = static_cast<uint64_t>(ftello(out));
    ZSTD_freeCCtx(ctx);
    std::fclose(in);
    std::fclose(out);
    err.clear();
    return true;
}

// 把文件中 [off, off+compSize) 的 zstd 帧解压写入已打开的临时文件 out，
// 完成后 out 会被 rewind 到开头，origSize 为解压后大小。
inline bool decompressRegion(FILE* in, uint64_t off, uint64_t compSize,
                             FILE* out, uint64_t& origSize, std::string& err) {
    if (fseeko(in, static_cast<off_t>(off), SEEK_SET) != 0) { err = "定位压缩 payload 失败"; return false; }
    ZSTD_DCtx* ctx = ZSTD_createDCtx();
    if (!ctx) { err = "ZSTD_createDCtx 失败"; return false; }
    std::vector<uint8_t> ibuf(kChunk), obuf(kChunk);
    uint64_t left = compSize;
    while (left > 0) {
        size_t want = static_cast<size_t>(std::min<uint64_t>(left, ibuf.size()));
        size_t got = std::fread(ibuf.data(), 1, want, in);
        if (got == 0) { err = "压缩 payload 读取中断"; ZSTD_freeDCtx(ctx); return false; }
        left -= got;
        ZSTD_inBuffer input{ibuf.data(), got, 0};
        while (input.pos < input.size) {
            ZSTD_outBuffer output{obuf.data(), obuf.size(), 0};
            size_t ret = ZSTD_decompressStream(ctx, &output, &input);
            if (ZSTD_isError(ret)) {
                err = std::string("zstd 解压失败: ") + ZSTD_getErrorName(ret);
                ZSTD_freeDCtx(ctx);
                return false;
            }
            if (output.pos && std::fwrite(obuf.data(), 1, output.pos, out) != output.pos) {
                err = "写解压数据失败（磁盘空间不足？）";
                ZSTD_freeDCtx(ctx);
                return false;
            }
            if (ret == 0) { input.pos = input.size; break; } // 单帧结束
        }
    }
    ZSTD_freeDCtx(ctx);
    if (std::fflush(out) != 0) { err = "刷新解压文件失败"; return false; }
    off_t sz = ftello(out);
    if (sz < 0) { err = "获取解压大小失败"; return false; }
    origSize = static_cast<uint64_t>(sz);
    if (fseeko(out, 0, SEEK_SET) != 0) { err = "rewind 解压文件失败"; return false; }
    err.clear();
    return true;
}

} // namespace mc263::zstd
