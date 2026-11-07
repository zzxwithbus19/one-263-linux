// mini_zip.hpp -- 零第三方依赖的 ZIP32 读写（payload 一律 STORED）
//
// 设计取舍：
//   * 构建器写出的 game.zip 全部使用 STORED（不压缩），因此启动器不需要任何
//     解压库，也不必链接 zlib —— 单文件 ELF 可以扔到任意 Linux 发行版上跑。
//   * 构建器需要读取官方/Maven 下载的 jar（DEFLATE 压缩）来提取 .so 时，
//     以 -DMC263_WITH_ZLIB 编译并链接 -lz，即可额外支持 method=8。
//   * 用 central directory 的 external attributes 高 16 位保存 unix 权限，
//     解压后 chmod，保证内嵌 runtime/bin/java 可执行。
//
// C++17, POSIX, header-only。
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

#ifdef MC263_WITH_ZLIB
#include <zlib.h>
#endif

namespace mc263 {
namespace zip {

namespace fs = std::filesystem;

// ---------------------------------------------------------------- CRC32 ----
inline const std::array<uint32_t, 256>& crcTable() {
    static std::array<uint32_t, 256> tab{};
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tab[i] = c;
        }
        ready = true;
    }
    return tab;
}

inline uint32_t crc32(const uint8_t* data, size_t len, uint32_t init = 0) {
    uint32_t c = init ^ 0xFFFFFFFFu;
    const auto& tab = crcTable();
    for (size_t i = 0; i < len; ++i) c = tab[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// 对整个文件（或文件中的一个区域）算 CRC32，流式读取，不占内存。
inline uint32_t crc32Region(FILE* f, uint64_t base, uint64_t len, std::string& err) {
    if (fseeko(f, static_cast<off_t>(base), SEEK_SET) != 0) { err = "seek 失败"; return 0; }
    uint32_t c = 0;
    std::vector<uint8_t> buf(1 << 20);
    uint64_t left = len;
    while (left > 0) {
        size_t want = static_cast<size_t>(std::min<uint64_t>(left, buf.size()));
        size_t got = fread(buf.data(), 1, want, f);
        if (got == 0) { err = "读取中断（payload 不完整？）"; return 0; }
        c = crc32(buf.data(), got, c);
        left -= got;
    }
    err.clear();
    return c;
}

// ------------------------------------------------------- little endian ----
inline void put16(std::ostream& o, uint16_t v) {
    uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
    o.write(reinterpret_cast<const char*>(b), 2);
}
inline void put32(std::ostream& o, uint32_t v) {
    uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
                    static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
    o.write(reinterpret_cast<const char*>(b), 4);
}
inline uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

struct Entry {
    std::string name;      // zip 内路径，'/' 分隔
    uint16_t method = 0;   // 0=STORED, 8=DEFLATE
    uint32_t crc = 0;
    uint64_t csize = 0;
    uint64_t usize = 0;
    uint64_t localOff = 0; // 相对 zip 起点
    uint32_t unixMode = 0100644;
};

// DOS 时间/日期（zip 规范要求，取构建时刻）
inline std::pair<uint16_t, uint16_t> dosDateTime() {
    std::time_t t = std::time(nullptr);
    std::tm* lt = std::localtime(&t);
    if (!lt) return {0, 0x0021}; // 1980-01-01
    uint16_t tm = static_cast<uint16_t>((lt->tm_hour << 11) | (lt->tm_min << 5) | (lt->tm_sec / 2));
    uint16_t dt = static_cast<uint16_t>(((lt->tm_year + 1900 - 1980) << 9) |
                                        ((lt->tm_mon + 1) << 5) | lt->tm_mday);
    return {tm, dt};
}

// ------------------------------------------------------------- ZipWriter ----
class Writer {
public:
    bool open(const fs::path& path, std::string& err) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        out_.open(path, std::ios::binary | std::ios::trunc);
        if (!out_) { err = "无法创建 " + path.string(); return false; }
        path_ = path;
        pos_ = 0;
        entries_.clear();
        err.clear();
        return true;
    }

    // 从磁盘文件添加一个条目（STORED）。arcName 必须是 / 分隔的相对路径。
    bool addFile(const std::string& arcName, const fs::path& src, uint32_t unixMode, std::string& err) {
        std::ifstream in(src, std::ios::binary);
        if (!in) { err = "无法读取 " + src.string(); return false; }
        in.seekg(0, std::ios::end);
        std::streamoff sz = in.tellg();
        in.seekg(0);
        if (sz < 0) { err = "无法获取大小 " + src.string(); return false; }
        uint64_t size = static_cast<uint64_t>(sz);
        if (size > 0xFFFFFFFFull || pos_ > 0xFFFFFFFFull || entries_.size() >= 65535) {
            err = "ZIP32 容量超限（单文件 >4GB / 条目 >65535），请拆分 payload";
            return false;
        }

        // 边读边写并计算 CRC，避免把大文件读进内存
        std::vector<uint8_t> buf(1 << 20);
        uint32_t crc = 0;
        uint64_t left = size;
        while (left > 0) {
            size_t want = static_cast<size_t>(std::min<uint64_t>(left, buf.size()));
            in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(want));
            size_t got = static_cast<size_t>(in.gcount());
            if (got == 0) { err = "读取中断 " + src.string(); return false; }
            crc = crc32(buf.data(), got, crc);
            left -= got;
        }
        if (!in.eof() && in.fail() && size > 0) { /* 上面已按字节数读满，忽略 eof 状态 */ }

        const auto [mtime, mdate] = dosDateTime();
        Entry e;
        e.name = arcName;
        e.method = 0;
        e.crc = crc;
        e.csize = e.usize = size;
        e.localOff = pos_;
        e.unixMode = unixMode;

        const uint16_t flags = 0x0800; // UTF-8 文件名
        put32(out_, 0x04034b50);       // local file header
        put16(out_, 20);               // version needed
        put16(out_, flags);
        put16(out_, 0);                // method = STORED
        put16(out_, mtime);
        put16(out_, mdate);
        put32(out_, crc);
        put32(out_, static_cast<uint32_t>(size));
        put32(out_, static_cast<uint32_t>(size));
        put16(out_, static_cast<uint16_t>(arcName.size()));
        put16(out_, 0);                // extra len
        out_.write(arcName.data(), static_cast<std::streamsize>(arcName.size()));
        pos_ += 30 + arcName.size();

        in.clear();
        in.seekg(0);
        left = size;
        while (left > 0) {
            size_t want = static_cast<size_t>(std::min<uint64_t>(left, buf.size()));
            in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(want));
            size_t got = static_cast<size_t>(in.gcount());
            if (got == 0) { err = "二次读取中断 " + src.string(); return false; }
            out_.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(got));
            pos_ += got;
            left -= got;
        }
        entries_.push_back(std::move(e));
        err.clear();
        return true;
    }

    // 通用条目写入：data 为原始内容，stored 为实际落盘字节（method=8 时是压缩结果）
    bool addEntry(const std::string& arcName, const uint8_t* data, size_t size,
                  uint32_t unixMode, uint16_t method, const std::vector<uint8_t>& stored,
                  std::string& err) {
        if (size > 0xFFFFFFFFull || stored.size() > 0xFFFFFFFFull ||
            pos_ > 0xFFFFFFFFull || entries_.size() >= 65535) {
            err = "ZIP32 容量超限（单文件 >4GB / 条目 >65535）";
            return false;
        }
        uint32_t crc = crc32(data, size);
        const auto [mtime, mdate] = dosDateTime();
        Entry e;
        e.name = arcName;
        e.method = method;
        e.crc = crc;
        e.csize = stored.size();
        e.usize = size;
        e.localOff = pos_;
        e.unixMode = unixMode;

        put32(out_, 0x04034b50);
        put16(out_, 20);
        put16(out_, 0x0800);
        put16(out_, method);
        put16(out_, mtime);
        put16(out_, mdate);
        put32(out_, crc);
        put32(out_, static_cast<uint32_t>(stored.size()));
        put32(out_, static_cast<uint32_t>(size));
        put16(out_, static_cast<uint16_t>(arcName.size()));
        put16(out_, 0);
        out_.write(arcName.data(), static_cast<std::streamsize>(arcName.size()));
        if (!stored.empty()) out_.write(reinterpret_cast<const char*>(stored.data()),
                                        static_cast<std::streamsize>(stored.size()));
        pos_ += 30 + arcName.size() + stored.size();
        entries_.push_back(std::move(e));
        err.clear();
        return true;
    }

    // 写入 config.json 等小文本（STORED）
    bool addBytes(const std::string& arcName, const std::string& data, uint32_t unixMode, std::string& err) {
        std::vector<uint8_t> raw(data.begin(), data.end());
        return addEntry(arcName, raw.data(), raw.size(), unixMode, 0, raw, err);
    }

#ifdef MC263_WITH_ZLIB
    // 重写 jar 时使用：DEFLATE 压缩，压缩反而变大时自动退回 STORED
    bool addBytesDeflate(const std::string& arcName, const std::vector<uint8_t>& data,
                         uint32_t unixMode, std::string& err) {
        std::vector<uint8_t> comp;
        if (!deflateRaw(data, comp, err)) return false;
        if (comp.size() >= data.size())
            return addEntry(arcName, data.data(), data.size(), unixMode, 0, data, err);
        return addEntry(arcName, data.data(), data.size(), unixMode, 8, comp, err);
    }

    static bool deflateRaw(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, std::string& err) {
        z_stream zs{};
        if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
                         Z_DEFAULT_STRATEGY) != Z_OK) {
            err = "deflateInit2 失败";
            return false;
        }
        out.resize(deflateBound(&zs, static_cast<uLong>(in.size())));
        zs.next_in = const_cast<Bytef*>(in.data());
        zs.avail_in = static_cast<uInt>(in.size());
        zs.next_out = out.data();
        zs.avail_out = static_cast<uInt>(out.size());
        int rc = deflate(&zs, Z_FINISH);
        uLong produced = zs.total_out;
        deflateEnd(&zs);
        if (rc != Z_STREAM_END) { err = "raw deflate 压缩失败 (" + std::to_string(rc) + ")"; return false; }
        out.resize(static_cast<size_t>(produced));
        err.clear();
        return true;
    }
#endif

    bool finish(std::string& err) {
        uint64_t cdStart = pos_;
        for (const Entry& e : entries_) {
            const auto [mtime, mdate] = dosDateTime();
            put32(out_, 0x02014b50);       // central directory header
            put16(out_, 0x0314);           // made by: UNIX, zip 2.0
            put16(out_, 20);               // needed
            put16(out_, 0x0800);
            put16(out_, e.method);
            put16(out_, mtime);
            put16(out_, mdate);
            put32(out_, e.crc);
            put32(out_, static_cast<uint32_t>(e.csize));
            put32(out_, static_cast<uint32_t>(e.usize));
            put16(out_, static_cast<uint16_t>(e.name.size()));
            put16(out_, 0);                // extra
            put16(out_, 0);                // comment
            put16(out_, 0);                // disk start
            put16(out_, 0);                // internal attrs
            put32(out_, e.unixMode << 16); // external attrs: unix mode
            put32(out_, static_cast<uint32_t>(e.localOff));
            out_.write(e.name.data(), static_cast<std::streamsize>(e.name.size()));
            pos_ += 46 + e.name.size();
        }
        uint64_t cdSize = pos_ - cdStart;
        if (cdSize > 0xFFFFFFFFull || cdStart > 0xFFFFFFFFull) {
            err = "central directory 超出 ZIP32 范围";
            return false;
        }
        put32(out_, 0x06054b50); // EOCD
        put16(out_, 0);
        put16(out_, 0);
        put16(out_, static_cast<uint16_t>(entries_.size()));
        put16(out_, static_cast<uint16_t>(entries_.size()));
        put32(out_, static_cast<uint32_t>(cdSize));
        put32(out_, static_cast<uint32_t>(cdStart));
        put16(out_, 0);
        pos_ += 22;
        out_.flush();
        if (!out_) { err = "写 ZIP 失败"; return false; }
        out_.close();
        err.clear();
        return true;
    }

    uint64_t bytesWritten() const { return pos_; }
    const std::vector<Entry>& entries() const { return entries_; }

private:
    std::ofstream out_;
    fs::path path_;
    uint64_t pos_ = 0;
    std::vector<Entry> entries_;
};

// ------------------------------------------------------------ ZipArchive ----
// 可从普通 zip 文件读，也可从「自身 ELF 尾部的一片区域」读（base 偏移）。
class Archive {
public:
    ~Archive() { close(); }

    bool openFile(const fs::path& path, std::string& err) {
        FILE* f = std::fopen(path.string().c_str(), "rb");
        if (!f) { err = "无法打开 " + path.string(); return false; }
        if (fseeko(f, 0, SEEK_END) != 0) { std::fclose(f); err = "seek 失败"; return false; }
        off_t sz = ftello(f);
        if (sz < 0) { std::fclose(f); err = "无法取大小"; return false; }
        own_ = true;
        f_ = f;
        return parse(0, static_cast<uint64_t>(sz), err);
    }

    // f 由调用方持有；base/size 描述 zip 在文件中的区域
    bool openRegion(FILE* f, uint64_t base, uint64_t size, std::string& err) {
        own_ = false;
        f_ = f;
        return parse(base, size, err);
    }

    void close() {
        if (own_ && f_) std::fclose(f_);
        f_ = nullptr;
    }

    const std::vector<Entry>& entries() const { return entries_; }

    // 解出单个条目到内存。method=8 需要 MC263_WITH_ZLIB。
    bool read(const Entry& e, std::vector<uint8_t>& out, std::string& err) const {
        if (!f_) { err = "archive 未打开"; return false; }
        if (fseeko(f_, static_cast<off_t>(base_ + e.localOff), SEEK_SET) != 0) {
            err = "定位 local header 失败"; return false;
        }
        uint8_t lh[30];
        if (std::fread(lh, 1, 30, f_) != 30 || get32(lh) != 0x04034b50) {
            err = "local header 损坏: " + e.name; return false;
        }
        uint16_t nameLen = get16(lh + 26), extraLen = get16(lh + 28);
        if (fseeko(f_, static_cast<off_t>(nameLen + extraLen), SEEK_CUR) != 0) {
            err = "跳过文件名失败"; return false;
        }
        std::vector<uint8_t> comp(static_cast<size_t>(e.csize));
        if (e.csize > 0 && std::fread(comp.data(), 1, static_cast<size_t>(e.csize), f_) != e.csize) {
            err = "读取压缩数据失败: " + e.name; return false;
        }
        if (e.method == 0) {
            out.assign(comp.begin(), comp.end());
        } else if (e.method == 8) {
#ifdef MC263_WITH_ZLIB
            if (!inflateRaw(comp, e.usize, out, err)) return false;
#else
            err = "该条目为 DEFLATE 压缩，但本程序未启用 zlib: " + e.name;
            return false;
#endif
        } else {
            err = "不支持的 ZIP method " + std::to_string(e.method) + ": " + e.name;
            return false;
        }
        if (out.size() != e.usize) { err = "解压后大小不符: " + e.name; return false; }
        if (crc32(out.data(), out.size()) != e.crc) { err = "CRC 校验失败: " + e.name; return false; }
        err.clear();
        return true;
    }

    // 解压全部条目。cb(done, total, name) 用于进度输出。
    bool extractAll(const fs::path& dest,
                    const std::function<void(size_t, size_t, const std::string&)>& cb,
                    std::string& err) const {
        std::error_code ec;
        fs::create_directories(dest, ec);
        size_t done = 0;
        for (const Entry& e : entries_) {
            if (!safeName(e.name)) { err = "zip 内含非法路径: " + e.name; return false; }
            fs::path target = dest / fs::path(e.name);
            if (!e.name.empty() && e.name.back() == '/') {
                fs::create_directories(target, ec);
                ++done;
                continue;
            }
            fs::create_directories(target.parent_path(), ec);
            std::vector<uint8_t> data;
            if (!read(e, data, err)) return false;
            {
                std::ofstream o(target, std::ios::binary | std::ios::trunc);
                if (!o) { err = "无法写 " + target.string(); return false; }
                if (!data.empty()) o.write(reinterpret_cast<const char*>(data.data()),
                                           static_cast<std::streamsize>(data.size()));
                o.flush();
                if (!o) { err = "写文件失败 " + target.string(); return false; }
            }
            uint32_t mode = (e.unixMode & 0777);
            if (mode == 0) mode = 0644;
            ::chmod(target.c_str(), mode);
            ++done;
            if (cb) cb(done, entries_.size(), e.name);
        }
        err.clear();
        return true;
    }

private:
    static bool safeName(const std::string& n) {
        if (n.empty() || n[0] == '/' || n.find('\\') != std::string::npos) return false;
        if (n.find(':') != std::string::npos && n.size() > 1 && n[1] == ':') return false;
        for (size_t i = 0; i < n.size();) {
            size_t j = n.find('/', i);
            std::string part = n.substr(i, j == std::string::npos ? j : j - i);
            if (part == "..") return false;
            if (j == std::string::npos) break;
            i = j + 1;
        }
        return true;
    }

    bool parse(uint64_t base, uint64_t size, std::string& err) {
        base_ = base;
        size_ = size;
        entries_.clear();
        if (size < 22) { err = "ZIP 区域太小"; return false; }
        uint64_t scan = std::min<uint64_t>(size, 65557 + 22);
        uint64_t start = size - scan;
        std::vector<uint8_t> tail(static_cast<size_t>(scan));
        if (fseeko(f_, static_cast<off_t>(base + start), SEEK_SET) != 0 ||
            std::fread(tail.data(), 1, tail.size(), f_) != tail.size()) {
            err = "读取 ZIP 尾部失败"; return false;
        }
        int64_t eocd = -1;
        for (int64_t i = static_cast<int64_t>(tail.size()) - 22; i >= 0; --i) {
            if (get32(tail.data() + i) == 0x06054b50 &&
                get16(tail.data() + i + 20) == static_cast<uint16_t>(tail.size() - i - 22)) {
                eocd = i;
                break;
            }
        }
        if (eocd < 0) { err = "找不到 ZIP EOCD（payload 损坏或 trailer 偏移错误）"; return false; }
        const uint8_t* p = tail.data() + eocd;
        uint16_t total = get16(p + 10);
        uint32_t cdSize = get32(p + 12);
        uint32_t cdOff = get32(p + 16);
        if (total == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOff == 0xFFFFFFFFu) {
            err = "暂不支持 ZIP64"; return false;
        }
        if (static_cast<uint64_t>(cdOff) + cdSize > size) { err = "central directory 越界"; return false; }
        std::vector<uint8_t> cd(cdSize);
        if (cdSize > 0 && (fseeko(f_, static_cast<off_t>(base + cdOff), SEEK_SET) != 0 ||
                           std::fread(cd.data(), 1, cdSize, f_) != cdSize)) {
            err = "读取 central directory 失败"; return false;
        }
        size_t off = 0;
        for (uint16_t i = 0; i < total; ++i) {
            if (off + 46 > cd.size() || get32(cd.data() + off) != 0x02014b50) {
                err = "central directory 条目损坏"; return false;
            }
            const uint8_t* q = cd.data() + off;
            Entry e;
            e.method = get16(q + 10);
            e.crc = get32(q + 16);
            e.csize = get32(q + 20);
            e.usize = get32(q + 24);
            uint16_t nl = get16(q + 28), xl = get16(q + 30), cl = get16(q + 32);
            e.unixMode = get32(q + 38) >> 16;
            e.localOff = get32(q + 42);
            if (off + 46 + nl + xl + cl > cd.size()) { err = "central directory 截断"; return false; }
            e.name.assign(reinterpret_cast<const char*>(q + 46), nl);
            entries_.push_back(std::move(e));
            off += 46 + nl + xl + cl;
        }
        err.clear();
        return true;
    }

#ifdef MC263_WITH_ZLIB
    static bool inflateRaw(const std::vector<uint8_t>& in, uint64_t expected,
                           std::vector<uint8_t>& out, std::string& err) {
        out.assign(static_cast<size_t>(expected), 0);
        if (expected == 0) { err.clear(); return true; }
        z_stream zs{};
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) { err = "inflateInit2 失败"; return false; }
        zs.next_in = const_cast<Bytef*>(in.data());
        zs.avail_in = static_cast<uInt>(in.size());
        zs.next_out = out.data();
        zs.avail_out = static_cast<uInt>(out.size());
        int rc = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);
        if (rc != Z_STREAM_END) { err = "raw deflate 解压失败 (" + std::to_string(rc) + ")"; return false; }
        err.clear();
        return true;
    }
#endif

    FILE* f_ = nullptr;
    bool own_ = false;
    uint64_t base_ = 0, size_ = 0;
    std::vector<Entry> entries_;
};

} // namespace zip
} // namespace mc263
