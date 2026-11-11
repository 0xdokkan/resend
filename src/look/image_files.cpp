// PNG and ICO files for the app icon, written without a library: PNG
// (ISO/IEC 15948) wraps the pixels in zlib (RFC 1950) whose deflate data
// (RFC 1951) is made only of stored blocks, which need no compressor.
#include "look/look.hpp"

#include <array>
#include <stdexcept>

namespace resend::look {
namespace {

void put32(std::string& out, std::uint32_t value) {  // big-endian, as PNG
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<char>((value >> shift) & 0xff));
}
void put16le(std::string& out, std::uint32_t value) {  // little-endian, as ICO
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
}
void put32le(std::string& out, std::uint32_t value) {
    put16le(out, value & 0xffff);
    put16le(out, value >> 16);
}

void chunk(std::string& out, std::string_view type, std::string_view data) {
    put32(out, static_cast<std::uint32_t>(data.size()));
    std::string body(type);
    body += data;
    out += body;
    put32(out, crc32(body));
}

} // namespace

std::uint32_t crc32(std::string_view bytes) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            result[n] = c;
        }
        return result;
    }();
    std::uint32_t c = 0xffffffffu;
    for (const char byte : bytes) c = table[(c ^ static_cast<std::uint8_t>(byte)) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

std::uint32_t adler32(std::string_view bytes) {
    std::uint32_t a = 1, b = 0;
    for (const char byte : bytes) {
        a = (a + static_cast<std::uint8_t>(byte)) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

std::string png(std::span<const std::uint8_t> rgba, int width, int height) {
    if (width <= 0 || height <= 0 || rgba.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4)
        throw std::invalid_argument("png: the pixels do not match the size");
    std::string out = "\x89PNG\r\n\x1a\n";
    std::string header;
    put32(header, static_cast<std::uint32_t>(width));
    put32(header, static_cast<std::uint32_t>(height));
    header += std::string{'\x08', '\x06', '\x00', '\x00', '\x00'};  // 8 bits, RGBA, deflate, no filter, no interlace
    chunk(out, "IHDR", header);

    // Each row starts with its filter type, 0: none.
    std::string raw;
    const auto row = static_cast<std::size_t>(width) * 4;
    for (int y = 0; y < height; ++y) {
        raw.push_back('\0');
        raw.append(reinterpret_cast<const char*>(rgba.data()) + static_cast<std::size_t>(y) * row, row);
    }
    std::string data = "\x78\x01";  // deflate, 32 KiB window, no dictionary, fastest
    for (std::size_t at = 0; at < raw.size() || at == 0;) {
        const auto size = std::min<std::size_t>(65535, raw.size() - at);
        const bool last = at + size == raw.size();
        data.push_back(last ? '\x01' : '\x00');
        put16le(data, static_cast<std::uint32_t>(size));
        put16le(data, static_cast<std::uint32_t>(~size & 0xffff));
        data.append(raw, at, size);
        at += size;
        if (last) break;
    }
    put32(data, adler32(raw));
    chunk(out, "IDAT", data);
    chunk(out, "IEND", "");
    return out;
}

std::string ico(std::span<const IconImage> images) {
    std::string out;
    put16le(out, 0);  // reserved
    put16le(out, 1);  // an icon
    put16le(out, static_cast<std::uint32_t>(images.size()));
    std::uint32_t offset = 6 + 16 * static_cast<std::uint32_t>(images.size());
    for (const auto& image : images) {
        out.push_back(static_cast<char>(image.size >= 256 ? 0 : image.size));  // 0 means 256
        out.push_back(static_cast<char>(image.size >= 256 ? 0 : image.size));
        out.push_back('\0');  // no palette
        out.push_back('\0');
        put16le(out, 1);   // colour planes
        put16le(out, 32);  // bits per pixel
        put32le(out, static_cast<std::uint32_t>(image.png.size()));
        put32le(out, offset);
        offset += static_cast<std::uint32_t>(image.png.size());
    }
    for (const auto& image : images) out += image.png;
    return out;
}

std::vector<std::uint8_t> enlarge(const Sprite& sprite, const Theme& theme, int factor, int size) {
    const auto pixels = paint(sprite, theme);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4, 0);
    const int left = (size - sprite.width * factor) / 2, top = (size - sprite.height * factor) / 2;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const int sx = (x - left) / factor, sy = (y - top) / factor;
            if (x < left || y < top || sx >= sprite.width || sy >= sprite.height) continue;
            const auto from = static_cast<std::size_t>(sy * sprite.width + sx) * 4;
            const auto to = static_cast<std::size_t>(y * size + x) * 4;
            for (int c = 0; c < 4; ++c) out[to + c] = pixels[from + c];
        }
    return out;
}

std::vector<IconFile> app_icons() {
    const auto& theme = palettes().find(palettes().default_id);
    const auto* logo = find_sprite("logo");
    const auto* icon = find_sprite("app-icon");
    if (!logo || !icon) throw std::logic_error("app_icons: the logo or the app icon is missing");
    // 48 px holds the 32 px drawing with a margin, as icon themes expect.
    struct Size {
        int size;
        const Sprite* sprite;
        int factor;
    };
    const Size sizes[] = {{16, logo, 1}, {24, logo, 1}, {32, icon, 1}, {48, icon, 1}, {64, icon, 2}, {128, icon, 4}, {256, icon, 8}};
    std::vector<IconFile> files;
    for (const auto& [size, sprite, factor] : sizes)
        files.push_back({size, png(enlarge(*sprite, theme, factor, size), size, size)});
    return files;
}

} // namespace resend::look
