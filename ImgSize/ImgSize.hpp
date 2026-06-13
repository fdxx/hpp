#pragma once
//
// imgsize.hpp — C++ port of github.com/Roughsketch/imagesize
//
// Design goals (performance-first for local HDD use):
//   • pread() throughout — no fseek/fread, no BufReader overhead
//   • Single open() + fd shared across all reads
//   • Fixed-size stack buffers, zero heap allocation for simple formats
//   • JPEG: exact SOF marker scan with lseek skip (same algorithm as Roughsketch)
//   • TIFF: reads only IFD entries until width+height found, then stops
//   • HDR/PNM/EXR: scan text/attr headers byte by byte via small read buffer
//   • Header-only, no dependencies beyond POSIX
//
// Supported formats (identical set to imagesize 0.14):
//   JPEG, PNG, GIF, WebP, BMP, TIFF/BigTIFF, PSD, QOI, ICO,
//   Aseprite, ASTC, Farbfeld, VTF, KTX2, JPEG XL (raw + container),
//   HDR (Radiance), EXR (OpenEXR), PNM (P1-P6), TGA (heuristic),
//   ILBM, HEIF/HEIC (ISO BMFF box scan)
//
// Usage:
//   auto r = imgsize::from_file("photo.jpg");
//   if (r) std::println("{}x{}", r->width, r->height);
//
// Thread safety: each call is independent (own fd, stack state).
//

#include <cstdint>
#include <cstring>
#include <string>
#include <optional>
#include <fcntl.h>
#include <unistd.h>

namespace imgsize {

struct Size { int width; int height; };

// ─────────────────────────────────────────────────────────────────────────────
// Internal I/O helpers — all through pread(), no seek state
// ─────────────────────────────────────────────────────────────────────────────
namespace detail {

// Read exactly n bytes at offset; returns false on short read or error.
[[nodiscard]] inline bool pread_exact(int fd, void* buf, std::size_t n, off_t off) noexcept
{
    auto* p = static_cast<uint8_t*>(buf);
    while (n > 0) {
        ssize_t r = ::pread(fd, p, n, off);
        if (r <= 0) return false;
        p += r; off += r; n -= static_cast<std::size_t>(r);
    }
    return true;
}

// Typed reads — all big-endian unless _le suffix
inline uint16_t u16be(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0] << 8 | p[1]);
}
inline uint16_t u16le(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0] | p[1] << 8);
}
inline uint32_t u32be(const uint8_t* p) noexcept {
    return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
}
inline uint32_t u32le(const uint8_t* p) noexcept {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
inline uint64_t u64le(const uint8_t* p) noexcept {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
inline uint64_t u64be(const uint8_t* p) noexcept {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}
inline uint32_t u24le(const uint8_t* p) noexcept {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16;
}

// ─── Format parsers ──────────────────────────────────────────────────────────

inline std::optional<Size> jpeg(int fd) noexcept
{
    // Walk JPEG segments: skip SOI (2 bytes), then read marker+length pairs.
    // Valid SOFn: C0–C3, C5–C7, C9–CB, CD–CF (i.e. not C4/C8/CC).
    // Nested JPEG (APP1 thumb) is tracked by depth counter.
    uint8_t buf[4];
    off_t pos = 2;
    int depth = 0;

    for (;;) {
        if (!pread_exact(fd, buf, 4, pos)) return std::nullopt;
        if (buf[0] != 0xFF) return std::nullopt;

        uint8_t marker = buf[1];

        // SOFn dimension markers
        bool is_sof = (marker >= 0xC0 && marker <= 0xCF)
                   && marker != 0xC4   // DHT — not a SOFn
                   && marker != 0xC8   // JPG extension
                   && marker != 0xCC;  // DAC

        if (is_sof) {
            if (depth == 0) {
                // SOF layout: length(2) + precision(1) + height(2) + width(2)
                // pos+4 = precision byte, pos+5 = height MSB
                if (!pread_exact(fd, buf, 4, pos + 5)) return std::nullopt;
                return Size{ u16be(buf + 2), u16be(buf) };  // width, height
            }
        } else if (marker == 0xD8) {
            ++depth;
        } else if (marker == 0xD9) {
            if (--depth < 0) return std::nullopt;
        }

        uint16_t seg_len = u16be(buf + 2);
        pos += 2 + seg_len;
    }
}

inline std::optional<Size> png(int fd) noexcept
{
    // IHDR chunk: signature(8) + chunk_len(4) + "IHDR"(4) + width(4) + height(4)
    uint8_t buf[8];
    if (!pread_exact(fd, buf, 8, 0x10)) return std::nullopt;
    return Size{ (int)u32be(buf), (int)u32be(buf + 4) };
}

inline std::optional<Size> gif(int fd) noexcept
{
    // Logical screen descriptor: offset 6 = width(u16le), offset 8 = height(u16le)
    uint8_t buf[4];
    if (!pread_exact(fd, buf, 4, 6)) return std::nullopt;
    return Size{ u16le(buf), u16le(buf + 2) };
}

inline std::optional<Size> bmp(int fd) noexcept
{
    // DIB header at offset 0x12: width(i32le), height(i32le)
    uint8_t buf[8];
    if (!pread_exact(fd, buf, 8, 0x12)) return std::nullopt;
    return Size{ (int)u32le(buf), std::abs((int)u32le(buf + 4)) };
}

inline std::optional<Size> webp(int fd) noexcept
{
    // RIFF????WEBP + VP8 chunk (4 bytes "VP8 "/"VP8L"/"VP8X")
    uint8_t buf[4];
    if (!pread_exact(fd, buf, 4, 12)) return std::nullopt;  // chunk type at 12

    if (buf[3] == ' ') {
        // VP8 (lossy): frame tag(3) + start code(3) at [20..], width at [26], height at [28]
        uint8_t vp8[4];
        if (!pread_exact(fd, vp8, 4, 0x1A)) return std::nullopt;
        return Size{ u16le(vp8) & 0x3FFF, u16le(vp8 + 2) & 0x3FFF };
    } else if (buf[3] == 'L') {
        // VP8L (lossless): signature(1) at 0x14, packed u32 at 0x15
        uint8_t vp8l[4];
        if (!pread_exact(fd, vp8l, 4, 0x15)) return std::nullopt;
        uint32_t d = u32le(vp8l);
        return Size{ (int)(d & 0x3FFF) + 1, (int)((d >> 14) & 0x3FFF) + 1 };
    } else if (buf[3] == 'X') {
        // VP8X (extended): canvas at 0x18 as u24le each
        uint8_t vp8x[6];
        if (!pread_exact(fd, vp8x, 6, 0x18)) return std::nullopt;
        return Size{ (int)u24le(vp8x) + 1, (int)u24le(vp8x + 3) + 1 };
    }
    return std::nullopt;
}

inline std::optional<Size> psd(int fd) noexcept
{
    // PSD header: "8BPS"(4) + version(2) + reserved(6) + channels(2)
    //             + height(4BE) + width(4BE) at offsets 0x0E, 0x12
    uint8_t buf[8];
    if (!pread_exact(fd, buf, 8, 0x0E)) return std::nullopt;
    return Size{ (int)u32be(buf + 4), (int)u32be(buf) };
}

inline std::optional<Size> qoi(int fd) noexcept
{
    // "qoif" + width(4BE) + height(4BE)
    uint8_t buf[8];
    if (!pread_exact(fd, buf, 8, 4)) return std::nullopt;
    return Size{ (int)u32be(buf), (int)u32be(buf + 4) };
}

inline std::optional<Size> farbfeld(int fd) noexcept
{
    // "farbfeld" + width(4BE) + height(4BE)
    uint8_t buf[8];
    if (!pread_exact(fd, buf, 8, 8)) return std::nullopt;
    return Size{ (int)u32be(buf), (int)u32be(buf + 4) };
}

inline std::optional<Size> vtf(int fd) noexcept
{
    // "VTF\0" + version(8) + header_size(4) + width(2LE) + height(2LE) at 0x10
    uint8_t buf[4];
    if (!pread_exact(fd, buf, 4, 0x10)) return std::nullopt;
    return Size{ u16le(buf), u16le(buf + 2) };
}

inline std::optional<Size> ktx2(int fd) noexcept
{
    // KTX2: 12-byte identifier + vkFormat(4) + typeSize(4) + width(4LE) + height(4LE) at offset 20
    uint8_t buf[8];
    if (!pread_exact(fd, buf, 8, 20)) return std::nullopt;
    return Size{ (int)u32le(buf), (int)u32le(buf + 4) };
}

inline std::optional<Size> aseprite(int fd) noexcept
{
    // ASE header: file_size(4) + magic(2=0xA5E0) + frames(2) + width(2LE) + height(2LE)
    uint8_t buf[4];
    if (!pread_exact(fd, buf, 4, 0x08)) return std::nullopt;
    return Size{ u16le(buf), u16le(buf + 2) };
}

inline std::optional<Size> astc(int fd) noexcept
{
    // ASTC: magic(4) + blockdim_x(1) + blockdim_y(1) + blockdim_z(1)
    //       + xsize(3LE) + ysize(3LE) + zsize(3LE)
    // xsize starts at offset 7
    uint8_t buf[6];
    if (!pread_exact(fd, buf, 6, 7)) return std::nullopt;
    uint32_t w = (uint32_t)buf[0] | (uint32_t)buf[1]<<8 | (uint32_t)buf[2]<<16;
    uint32_t h = (uint32_t)buf[3] | (uint32_t)buf[4]<<8 | (uint32_t)buf[5]<<16;
    return Size{ (int)w, (int)h };
}

inline std::optional<Size> ico(int fd) noexcept
{
    // ICO: reserved(2) + type(2) + count(2LE), then ICONDIRENTRY[count]: width(1) height(1) ...
    // Return the largest image (same as imagesize).
    uint8_t buf[2];
    if (!pread_exact(fd, buf, 2, 4)) return std::nullopt;
    int count = u16le(buf);

    Size best{0, 0};
    for (int i = 0; i < count; ++i) {
        uint8_t entry[2];
        // Each ICONDIRENTRY is 16 bytes; first two are width, height (0 = 256)
        if (!pread_exact(fd, entry, 2, 6 + i * 16)) break;
        int w = entry[0] == 0 ? 256 : entry[0];
        int h = entry[1] == 0 ? 256 : entry[1];
        if (w * h > best.width * best.height) best = {w, h};
    }
    if (best.width == 0) return std::nullopt;
    return best;
}

inline std::optional<Size> tiff(int fd) noexcept
{
    // Read endianness marker and TIFF/BigTIFF type
    uint8_t hdr[8];
    if (!pread_exact(fd, hdr, 8, 0)) return std::nullopt;

    bool le;
    if (hdr[0] == 'I' && hdr[1] == 'I') le = true;
    else if (hdr[0] == 'M' && hdr[1] == 'M') le = false;
    else return std::nullopt;

    auto u16 = [&](const uint8_t* p) { return le ? u16le(p) : u16be(p); };
    auto u32 = [&](const uint8_t* p) { return le ? u32le(p) : u32be(p); };
    auto u64 = [&](const uint8_t* p) { return le ? u64le(p) : u64be(p); };

    uint16_t type_marker = u16(hdr + 2);
    bool is_bigtiff = (type_marker == 43);
    if (type_marker != 42 && type_marker != 43) return std::nullopt;

    uint64_t ifd_offset;
    if (!is_bigtiff) {
        ifd_offset = u32(hdr + 4);
    } else {
        // BigTIFF: offset_bytesize(2) + extra(2) + offset(8)
        uint8_t bthdr[12];
        if (!pread_exact(fd, bthdr, 12, 4)) return std::nullopt;
        if (u16(bthdr) != 8 || u16(bthdr + 2) != 0) return std::nullopt;
        ifd_offset = u64(bthdr + 4);
    }
    if (ifd_offset == 0) return std::nullopt;

    // Read IFD entry count
    uint8_t cnt_buf[8];
    uint64_t ifd_count;
    if (!is_bigtiff) {
        if (!pread_exact(fd, cnt_buf, 2, (off_t)ifd_offset)) return std::nullopt;
        ifd_count = u16(cnt_buf);
    } else {
        if (!pread_exact(fd, cnt_buf, 8, (off_t)ifd_offset)) return std::nullopt;
        ifd_count = u64(cnt_buf);
    }

    // Each IFD entry for classic TIFF: 12 bytes; BigTIFF: 20 bytes
    off_t entry_base = (off_t)ifd_offset + (is_bigtiff ? 8 : 2);
    std::optional<uint32_t> width, height;

    for (uint64_t i = 0; i < ifd_count; ++i) {
        uint8_t entry[20];
        std::size_t entry_sz = is_bigtiff ? 20 : 12;
        if (!pread_exact(fd, entry, entry_sz, entry_base + (off_t)(i * entry_sz)))
            break;

        uint16_t tag  = u16(entry);
        uint16_t kind = u16(entry + 2);

        // Bytes per value for the IFD type
        static constexpr uint8_t KIND_SIZES[19] = {
            0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4, 0, 0, 8, 8, 8
        };
        uint32_t value_bytes = (kind < 19) ? KIND_SIZES[kind] : 0;

        // Tag 0x100 = ImageWidth, 0x101 = ImageLength (height)
        if (tag == 0x100 || tag == 0x101) {
            // Value is inline in the entry at offset 8 (classic) or 12 (big)
            const uint8_t* vp = entry + (is_bigtiff ? 12 : 8);
            uint32_t v = 0;
            if (value_bytes == 2) v = u16(vp);
            else if (value_bytes == 4) v = u32(vp);
            else if (value_bytes == 8) v = (uint32_t)u64(vp);

            if (tag == 0x100) width  = v;
            else              height = v;

            if (width && height)
                return Size{ (int)*width, (int)*height };
        }
    }
    return std::nullopt;
}

// ─── ILBM (IFF Interleaved Bitmap) ───────────────────────────────────────────
inline std::optional<Size> ilbm(int fd) noexcept
{
    // IFF structure: "FORM" + size(4BE) + "ILBM"/"PBM " + chunks
    // Look for "BMHD" chunk which contains width(2BE) + height(2BE)
    uint8_t buf[12];
    if (!pread_exact(fd, buf, 12, 0)) return std::nullopt;
    if (std::memcmp(buf, "FORM", 4) != 0) return std::nullopt;

    off_t pos = 12; // after FORM + size + subtype
    for (int guard = 0; guard < 64; ++guard) {
        uint8_t chunk[8];
        if (!pread_exact(fd, chunk, 8, pos)) return std::nullopt;
        uint32_t chunk_size = u32be(chunk + 4);

        if (std::memcmp(chunk, "BMHD", 4) == 0) {
            // BMHD: width(2BE) + height(2BE) at chunk data start
            uint8_t dims[4];
            if (!pread_exact(fd, dims, 4, pos + 8)) return std::nullopt;
            return Size{ u16be(dims), u16be(dims + 2) };
        }

        // Chunks are padded to even byte boundary
        pos += 8 + chunk_size + (chunk_size & 1);
    }
    return std::nullopt;
}

// ─── HEIF/HEIC (ISO Base Media File Format box scan) ─────────────────────────
// Strategy matches imagesize: skip ftyp, enter meta→iprp→ipco linearly,
// pick the ispe with the largest area, swap w/h if irot==1 or 3.
inline std::optional<Size> heif(int fd) noexcept
{
    // Read ftyp box size so we can skip it
    uint8_t ftyp_hdr[4];
    if (!pread_exact(fd, ftyp_hdr, 4, 0)) return std::nullopt;
    off_t pos = (off_t)u32be(ftyp_hdr); // skip past ftyp

    // Helper: scan forward for a box with given 4-byte type tag.
    // Returns the offset of the box's data (after size+type header), or -1.
    // Advances pos to the box's data start on success.
    auto skip_to_tag = [&](const char tag[4]) -> off_t {
        for (int guard = 0; guard < 512; ++guard) {
            uint8_t hdr[8];
            if (!pread_exact(fd, hdr, 8, pos)) return -1;
            uint32_t size = u32be(hdr);
            if (size < 8) return -1;
            if (std::memcmp(hdr + 4, tag, 4) == 0) {
                pos += 8;
                return pos; // data starts here
            }
            pos += size;
        }
        return -1;
    };

    // Navigate: meta → iprp → ipco
    if (skip_to_tag("meta") < 0) return std::nullopt;
    pos += 4; // meta has 4 bytes version+flags before sub-boxes

    if (skip_to_tag("iprp") < 0) return std::nullopt;

    off_t ipco_data = skip_to_tag("ipco");
    if (ipco_data < 0) return std::nullopt;

    // ipco_data is now positioned inside ipco. Read its declared size
    // (we already consumed the 8-byte header, so reconstruct ipco end from pos).
    // Re-read: at ipco_data-8 we have the size field.
    uint8_t sz_buf[4];
    if (!pread_exact(fd, sz_buf, 4, ipco_data - 8)) return std::nullopt;
    off_t ipco_end = (ipco_data - 8) + u32be(sz_buf);

    int max_w = 0, max_h = 0;
    bool found = false;
    uint8_t rotation = 0;

    while (pos < ipco_end) {
        uint8_t hdr[8];
        if (!pread_exact(fd, hdr, 8, pos)) break;
        uint32_t size = u32be(hdr);
        if (size < 8) break;

        if (std::memcmp(hdr + 4, "ispe", 4) == 0) {
            // ispe: version+flags(4) + width(4BE) + height(4BE)
            uint8_t dims[8];
            if (pread_exact(fd, dims, 8, pos + 12)) {
                int w = (int)u32be(dims);
                int h = (int)u32be(dims + 4);
                if (w * h > max_w * max_h) { max_w = w; max_h = h; }
                found = true;
            }
        } else if (std::memcmp(hdr + 4, "irot", 4) == 0) {
            // irot: 1 byte (0–3 for 0°/90°/180°/270° anti-clockwise)
            uint8_t r;
            if (pread_exact(fd, &r, 1, pos + 8)) rotation = r;
        }

        pos += size;
    }

    if (!found) return std::nullopt;

    // irot 1 (90°CCW) or 3 (270°CCW) means width/height are swapped
    if (rotation == 1 || rotation == 3)
        return Size{ max_h, max_w };
    return Size{ max_w, max_h };
}

// ─── OpenEXR ─────────────────────────────────────────────────────────────────
// EXR stores attributes as null-terminated name + null-terminated type + u32 size + data.
// We scan until we find "dataWindow" of type "box2i" (4× i32LE: xmin ymin xmax ymax).
inline std::optional<Size> exr(int fd) noexcept
{
    // Skip magic(4) + version(4)
    off_t pos = 8;
    uint8_t flags_buf[4];
    if (!pread_exact(fd, flags_buf, 4, 4)) return std::nullopt;
    uint32_t flags = u32le(flags_buf);
    int max_name = (flags & 0x400) ? 255 : 31;

    // Read null-terminated string from file, capped at max_name
    auto read_str = [&](off_t& p, int cap, std::string& out) -> bool {
        out.clear();
        for (int i = 0; i < cap + 1; ++i) {
            uint8_t c;
            if (!pread_exact(fd, &c, 1, p)) return false;
            ++p;
            if (c == 0) return true;
            out += (char)c;
        }
        return false; // exceeded cap
    };

    std::string attr_name, attr_type;
    for (int guard = 0; guard < 256; ++guard) {
        if (!read_str(pos, max_name, attr_name)) return std::nullopt;
        if (attr_name.empty()) break; // end of header sentinel

        if (!read_str(pos, max_name, attr_type)) return std::nullopt;

        uint8_t size_buf[4];
        if (!pread_exact(fd, size_buf, 4, pos)) return std::nullopt;
        uint32_t attr_size = u32le(size_buf);
        pos += 4;

        if (attr_name == "dataWindow" && attr_type == "box2i") {
            uint8_t box[16];
            if (!pread_exact(fd, box, 16, pos)) return std::nullopt;
            int32_t xmin, ymin, xmax, ymax;
            std::memcpy(&xmin, box + 0, 4); xmin = (int32_t)u32le(box + 0);
            std::memcpy(&ymin, box + 4, 4); ymin = (int32_t)u32le(box + 4);
            std::memcpy(&xmax, box + 8, 4); xmax = (int32_t)u32le(box + 8);
            std::memcpy(&ymax, box +12, 4); ymax = (int32_t)u32le(box +12);
            if (xmin > xmax || ymin > ymax) { pos += attr_size; continue; }
            return Size{ (int)(xmax - xmin + 1), (int)(ymax - ymin + 1) };
        }
        pos += (off_t)attr_size;
    }
    return std::nullopt;
}

// ─── HDR (Radiance RGBE) ─────────────────────────────────────────────────────
// Text header followed by dimension line: "-Y H +X W"
inline std::optional<Size> hdr(int fd) noexcept
{
    // Read into a small rolling buffer; scan for newlines
    char line[512];
    int lp = 0;
    off_t pos = 0;
    int lines_seen = 0;

    for (;;) {
        uint8_t c;
        if (!pread_exact(fd, &c, 1, pos++)) return std::nullopt;
        if (c == '\n' || lp == (int)sizeof(line) - 1) {
            line[lp] = '\0';
            std::string_view sv(line, lp);
            lp = 0;
            ++lines_seen;

            if (lines_seen > 200) return std::nullopt; // malformed guard

            // Dimension line starts with ±X or ±Y
            if (sv.starts_with("-Y") || sv.starts_with("+Y") ||
                sv.starts_with("-X") || sv.starts_with("+X"))
            {
                // Format: "±A N ±B M" where the two numbers are height and width
                // (when starting with ±Y: first number = height, second = width)
                // (when starting with ±X: first number = width,  second = height)
                // We parse only the two numeric tokens.
                bool starts_x = (sv[0] != '-' ? sv[1] : sv[1]) == 'X' ||
                                 sv.starts_with("-X") || sv.starts_with("+X");
                int nums[2], ni = 0;
                bool in_tok = false;
                int cur = 0;
                for (char ch : sv) {
                    if (ch >= '0' && ch <= '9') {
                        in_tok = true; cur = cur * 10 + (ch - '0');
                    } else if (in_tok) {
                        if (ni < 2) nums[ni++] = cur;
                        in_tok = false; cur = 0;
                    }
                }
                if (in_tok && ni < 2) nums[ni++] = cur;
                if (ni == 2) {
                    // ±Y: nums[0]=height, nums[1]=width
                    // ±X: nums[0]=width,  nums[1]=height (rotated 90°)
                    int w = starts_x ? nums[0] : nums[1];
                    int h = starts_x ? nums[1] : nums[0];
                    return Size{ w, h };
                }
                return std::nullopt;
            }
        } else {
            line[lp++] = (char)c;
        }
    }
}

// ─── PNM (P1–P6) ─────────────────────────────────────────────────────────────
// ASCII header: magic, then whitespace-delimited tokens (comments with '#' to EOL)
inline std::optional<Size> pnm(int fd) noexcept
{
    off_t pos = 2; // skip magic (P1..P6)

    auto skip_comment_and_ws = [&]() {
        for (int guard = 0; guard < 65536; ++guard) {
            uint8_t c;
            if (!pread_exact(fd, &c, 1, pos)) return;
            if (c == '#') {
                // Skip to end of line
                for (;;) {
                    if (!pread_exact(fd, &c, 1, ++pos)) return;
                    if (c == '\n') { ++pos; break; }
                }
            } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos;
            } else {
                break; // non-whitespace, non-comment
            }
        }
    };

    auto read_int = [&]() -> std::optional<int> {
        skip_comment_and_ws();
        int v = 0; bool got = false;
        for (int guard = 0; guard < 24; ++guard) {
            uint8_t c;
            if (!pread_exact(fd, &c, 1, pos)) break;
            if (c < '0' || c > '9') break;
            v = v * 10 + (c - '0');
            got = true; ++pos;
        }
        return got ? std::optional<int>(v) : std::nullopt;
    };

    auto w = read_int();
    auto h = read_int();
    if (!w || !h) return std::nullopt;
    return Size{ *w, *h };
}

// ─── TGA (heuristic, no magic bytes) ─────────────────────────────────────────
// Must be called only after other formats are ruled out.
// Port of imagesize's TGA heuristic: check footer, then validate header fields.
inline bool tga_matches(int fd) noexcept
{
    // Try Version 2 footer at EOF - 18
    off_t file_end = ::lseek(fd, 0, SEEK_END);
    if (file_end < 18) return false;

    uint8_t footer[18];
    if (pread_exact(fd, footer, 18, file_end - 18))
        if (std::memcmp(footer, "TRUEVISION-XFILE.\0", 18) == 0) return true;

    // Heuristic: validate header fields
    uint8_t hdr[18];
    if (!pread_exact(fd, hdr, 18, 0)) return false;

    uint8_t colormap_type = hdr[1];
    uint8_t image_type    = hdr[2];

    // Valid image types
    if (image_type != 1 && image_type != 2 && image_type != 3 &&
        image_type != 9 && image_type != 10 && image_type != 11)
        return false;
    if (colormap_type >= 2) return false;

    // Color-mapped consistency
    if ((image_type == 1 || image_type == 9) && colormap_type != 1) return false;
    if (colormap_type == 0) {
        // origin + length (bytes 3-6) and entry size (byte 7) must be 0
        uint32_t cm_offset = u32le(hdr + 3);
        if (cm_offset != 0 || hdr[7] != 0) return false;
    }

    uint8_t pixel_size = hdr[16];
    uint8_t descriptor = hdr[17];
    uint8_t alpha_bits = descriptor & 0x0F;
    if (descriptor & 0x10) return false; // reserved bit must be 0

    static constexpr uint8_t VALID_PS[] = {8, 16, 24, 32};
    bool valid_ps = false;
    for (auto p : VALID_PS) if (pixel_size == p) { valid_ps = true; break; }
    if (!valid_ps) return false;

    if ((pixel_size == 8 || pixel_size == 24) && alpha_bits != 0) return false;
    if (pixel_size == 16 && alpha_bits >= 2) return false;
    if (pixel_size == 32 && alpha_bits != 8 && alpha_bits != 0) return false;

    return true;
}

inline std::optional<Size> tga(int fd) noexcept
{
    uint8_t buf[4];
    if (!pread_exact(fd, buf, 4, 12)) return std::nullopt;
    return Size{ u16le(buf), u16le(buf + 2) };
}

// ─── JPEG XL ─────────────────────────────────────────────────────────────────
// Direct port of imagesize's bitfield parsing for the JXL codestream header.
inline std::optional<Size> jxl(int fd) noexcept
{
    uint8_t raw[16] = {};
    std::size_t header_size = 0;

    uint8_t sig[2];
    if (!pread_exact(fd, sig, 2, 0)) return std::nullopt;

    if (sig[0] == 0xFF && sig[1] == 0x0A) {
        // Raw codestream
        if (!pread_exact(fd, raw + 2, 14, 2)) return std::nullopt;
        header_size = 16;
        raw[0] = sig[0]; raw[1] = sig[1];
    } else {
        // Container: scan boxes for 'jxlc' or 'jxlp'
        off_t pos = 12;
        uint8_t buf[16];
        for (int guard = 0; guard < 32 && header_size < 16; ++guard) {
            if (!pread_exact(fd, buf, 8, pos)) return std::nullopt;
            uint64_t box_size = u32be(buf);
            off_t data_off = pos + 8;
            if (box_size == 1) {
                if (!pread_exact(fd, buf + 8, 8, pos + 8)) return std::nullopt;
                box_size = u64be(buf + 8);
                data_off = pos + 16;
            }

            bool is_jxlc = std::memcmp(buf + 4, "jxlc", 4) == 0;
            bool is_jxlp = std::memcmp(buf + 4, "jxlp", 4) == 0;

            if (is_jxlc) {
                // jxlc: complete codestream
                std::size_t can_read = 16 - header_size;
                if (!pread_exact(fd, raw + header_size, can_read, data_off)) return std::nullopt;
                header_size = 16;
                break;
            }

            if (is_jxlp) {
                // jxlp: 4-byte index field (high bit = last), then codestream fragment
                uint8_t idx[4];
                if (!pread_exact(fd, idx, 4, data_off)) return std::nullopt;
                bool is_last = (idx[0] & 0x80) != 0;
                off_t frag_off = data_off + 4;
                std::size_t frag_max = 16 - header_size;
                // Only read what we still need
                std::size_t frag_avail = (box_size >= 12) ? (std::size_t)(box_size - 12) : 0;
                std::size_t to_read = std::min(frag_max, frag_avail);
                if (to_read > 0) {
                    if (!pread_exact(fd, raw + header_size, to_read, frag_off)) return std::nullopt;
                    header_size += to_read;
                }
                if (is_last || header_size >= 16) break;
            }

            if (box_size == 0 || box_size < 8) break;
            pos += (off_t)box_size;
        }
    }

    if (header_size < 2) return std::nullopt;
    if (raw[0] != 0xFF || raw[1] != 0x0A) return std::nullopt;

    // Pack into u128 for bitfield extraction (little-endian bit order)
    uint64_t lo, hi;
    std::memcpy(&lo, raw,     8);
    std::memcpy(&hi, raw + 8, 8);
    // Simulate u128 as {lo, hi} — access via bit index from LSB
    auto read_bits = [&](int num_bits, int offset) -> std::optional<uint32_t> {
        if (offset + num_bits > (int)(header_size * 8)) return std::nullopt;
        uint32_t v = 0;
        for (int b = 0; b < num_bits; ++b) {
            int abs_bit = offset + b;
            uint64_t word = (abs_bit < 64) ? lo : hi;
            int bit_in_word = abs_bit & 63;
            v |= (uint32_t)((word >> bit_in_word) & 1) << b;
        }
        return v;
    };

    auto rb = [&](int nb, int off) -> uint32_t {
        auto v = read_bits(nb, off);
        return v.value_or(0);
    };
    auto rb_opt = [&](int nb, int off) { return read_bits(nb, off); };

    bool is_small = rb(1, 16) != 0;
    uint32_t hs = rb(2, 17);

    int height_bits, height_off, height_shift;
    if (is_small)        { height_bits = 5;  height_off = 17; height_shift = 3; }
    else if (hs == 0)    { height_bits = 9;  height_off = 19; height_shift = 0; }
    else if (hs == 1)    { height_bits = 13; height_off = 19; height_shift = 0; }
    else if (hs == 2)    { height_bits = 18; height_off = 19; height_shift = 0; }
    else                 { height_bits = 30; height_off = 19; height_shift = 0; }

    auto hv = rb_opt(height_bits, height_off);
    if (!hv) return std::nullopt;
    uint32_t height = (*hv + 1) << height_shift;

    uint32_t ratio = rb(3, height_bits + height_off);
    uint32_t ws = rb(2, height_bits + height_off + 3);

    uint32_t width;
    if (ratio >= 1 && ratio <= 7) {
        static constexpr struct { int num, den; } RATIOS[8] = {
            {0,0},{1,1},{12,10},{4,3},{3,2},{16,9},{5,4},{2,1}
        };
        width = (height / (uint32_t)RATIOS[ratio].den) * (uint32_t)RATIOS[ratio].num;
    } else {
        int w_off = height_bits + height_off + 5;
        int w_bits, w_shift;
        if (is_small)     { w_bits = 5;  w_shift = 3; }
        else if (ws == 0) { w_bits = 9;  w_shift = 0; }
        else if (ws == 1) { w_bits = 13; w_shift = 0; }
        else if (ws == 2) { w_bits = 18; w_shift = 0; }
        else              { w_bits = 30; w_shift = 0; }
        auto wv = rb_opt(w_bits, w_off);
        if (!wv) return std::nullopt;
        width = (*wv + 1) << w_shift;
    }

    // Check orientation (determines whether w/h are swapped)
    int meta_off = (ratio == 0) ? (height_bits + height_off + 5 + (is_small ? 5 : [&]{
        int wb; if(ws==0)wb=9;else if(ws==1)wb=13;else if(ws==2)wb=18;else wb=30;
        return wb;
    }()))
                                : (height_bits + height_off + 3);
    bool all_default = rb(1, meta_off) != 0;
    uint32_t orientation = 0;
    if (!all_default) {
        bool extra = rb(1, meta_off + 1) != 0;
        if (extra) orientation = rb(3, meta_off + 2);
    }

    if (orientation >= 4)
        return Size{ (int)height, (int)width };
    return Size{ (int)width, (int)height };
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/// Read image dimensions from an open file descriptor.
/// fd must be readable; the function uses pread() and does not change the fd offset.
[[nodiscard]] inline std::optional<Size> from_fd(int fd) noexcept
{
    // Read enough of the header to identify the format (12 bytes like imagesize)
    uint8_t hdr[12] = {};
    if (!detail::pread_exact(fd, hdr, sizeof(hdr), 0)) return std::nullopt;

    // ── Format dispatch (order matches imagesize priority) ──────────────────
    // JPEG
    if (hdr[0] == 0xFF && hdr[1] == 0xD8 && hdr[2] == 0xFF)
        return detail::jpeg(fd);

    // PNG
    if (hdr[0] == 0x89 && hdr[1] == 'P' && hdr[2] == 'N' && hdr[3] == 'G')
        return detail::png(fd);

    // GIF
    if (hdr[0] == 'G' && hdr[1] == 'I' && hdr[2] == 'F' && hdr[3] == '8')
        return detail::gif(fd);

    // TIFF (II/MM + 42/43)
    {
        bool ii = hdr[0]=='I' && hdr[1]=='I';
        bool mm = hdr[0]=='M' && hdr[1]=='M';
        if ((ii || mm) && (hdr[2]==0x2A || hdr[2]==0x2B || hdr[3]==0x2A || hdr[3]==0x2B))
            return detail::tiff(fd);
    }

    // WebP
    if (std::memcmp(hdr, "RIFF", 4) == 0 && std::memcmp(hdr + 8, "WEBP", 4) == 0)
        return detail::webp(fd);

    // HEIF / HEIC: ftyp box with compatible brands
    // Any ISOBMFF file starting with a box whose type == "ftyp"
    if (hdr[4]=='f' && hdr[5]=='t' && hdr[6]=='y' && hdr[7]=='p')
        return detail::heif(fd);

    // JPEG XL — raw codestream
    if (hdr[0] == 0xFF && hdr[1] == 0x0A)
        return detail::jxl(fd);

    // JPEG XL — container (JXL ISO BMFF box: 00 00 00 0C 4A 58 4C 20 0D 0A 87 0A)
    if (hdr[0]==0x00 && hdr[1]==0x00 && hdr[2]==0x00 && hdr[3]==0x0C &&
        hdr[4]=='J' && hdr[5]=='X' && hdr[6]=='L' && hdr[7]==' ')
        return detail::jxl(fd);

    // BMP
    if (hdr[0] == 'B' && hdr[1] == 'M')
        return detail::bmp(fd);

    // PSD
    if (hdr[0]=='8' && hdr[1]=='B' && hdr[2]=='P' && hdr[3]=='S')
        return detail::psd(fd);

    // ICO
    if (hdr[0]==0 && hdr[1]==0 && hdr[2]==1 && hdr[3]==0)
        return detail::ico(fd);

    // Aseprite (magic at offset 4-5: 0xA5 0xE0 little-endian = 0xE0A5)
    if (hdr[4] == 0xE0 && hdr[5] == 0xA5)
        return detail::aseprite(fd);

    // ASTC (magic: 13 AB A0/A1 5C)
    if (hdr[0]==0x13 && hdr[1]==0xAB && (hdr[2]==0xA0 || hdr[2]==0xA1) && hdr[3]==0x5C)
        return detail::astc(fd);

    // Farbfeld
    if (std::memcmp(hdr, "farbfeld", 8) == 0)
        return detail::farbfeld(fd);

    // VTF
    if (std::memcmp(hdr, "VTF\0", 4) == 0)
        return detail::vtf(fd);

    // KTX2 (first 12 bytes of identifier)
    {
        static constexpr uint8_t KTX2_SIG[12] = {
            0xAB,0x4B,0x54,0x58,0x20,0x32,0x30,0xBB,0x0D,0x0A,0x1A,0x0A
        };
        if (std::memcmp(hdr, KTX2_SIG, 12) == 0)
            return detail::ktx2(fd);
    }

    // QOI
    if (std::memcmp(hdr, "qoif", 4) == 0)
        return detail::qoi(fd);

    // EXR
    {
        static constexpr uint8_t EXR_MAGIC[4] = {0x76,0x2F,0x31,0x01};
        if (std::memcmp(hdr, EXR_MAGIC, 4) == 0)
            return detail::exr(fd);
    }

    // HDR (Radiance)
    if (std::memcmp(hdr, "#?RADIANCE\n", 11) == 0 ||
        std::memcmp(hdr, "#?RGBE\n",     7) == 0)
        return detail::hdr(fd);

    // ILBM
    if (std::memcmp(hdr, "FORM", 4) == 0)
        return detail::ilbm(fd);

    // PNM (P1–P6)
    if (hdr[0] == 'P' && hdr[1] >= '1' && hdr[1] <= '6')
        return detail::pnm(fd);

    // TGA — last resort (heuristic, highest false-positive risk)
    if (detail::tga_matches(fd))
        return detail::tga(fd);

    return std::nullopt;
}

/// Read image dimensions from a file path.
[[nodiscard]] inline std::optional<Size> from_file(const char* path) noexcept
{
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    auto r = from_fd(fd);
    ::close(fd);
    return r;
}

} // namespace imgsize
