#pragma once

// The look of resend without Slint: the themes of assets/palettes.json, the
// sprites of assets/sprites/, painted for a theme, and the rule that keeps
// pixel art square on any screen. Both files are built into the program;
// see docs/decisions/0003-visual-direction.md.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace resend::look {

struct Rgb {
    std::uint8_t r = 0, g = 0, b = 0;
    bool operator==(const Rgb&) const = default;
};

// "#rrggbb", lower or upper case.
std::optional<Rgb> parse_colour(std::string_view text);
std::string format_colour(Rgb colour);
// Relative luminance and contrast ratio as WCAG 2 defines them.
double luminance(Rgb colour);
double contrast(Rgb a, Rgb b);

// The names a theme must give a colour, in the order the file lists them.
inline constexpr std::array<std::string_view, 13> surface_names = {
    "outline", "shade", "base", "canvas", "grid", "panel", "raised", "raised-edge", "hover", "sunken", "edge", "line", "screen"};
inline constexpr std::array<std::string_view, 4> ink_names = {"ink", "muted", "on-accent", "screen-ink"};
// Families of four tones, darkest to lightest: the devices' metal, the
// interface's accent, then one for each kind of thing the engine reports.
inline constexpr std::array<std::string_view, 8> family_names = {"metal", "accent", "ok", "link", "ask", "route", "drop", "guard"};
// The most colours a theme may hold, all names together.
inline constexpr std::size_t max_colours = 48;

struct Family {
    std::array<Rgb, 4> tones{};  // dd, d, base, l
    // Derived: text in this family's colour on a panel, and a chip, a small
    // plate in its colour with text on it. Dark themes write in the base
    // tone and plate in the darkest; light themes the other way round.
    Rgb text, chip, chip_ink;
};

struct Theme {
    std::string id, title, credit;
    bool dark = true;
    std::array<Rgb, surface_names.size()> surfaces{};
    std::array<Rgb, ink_names.size()> inks{};
    std::array<Family, family_names.size()> families{};

    Rgb surface(std::string_view name) const;
    Rgb ink(std::string_view name) const;
    const Family& family(std::string_view name) const;
    // Every distinct colour of the theme.
    std::vector<Rgb> colours() const;
};

struct Palettes {
    std::vector<Theme> themes;
    std::string default_id;
    // The theme `id`, or the default one if there is none by that name.
    const Theme& find(std::string_view id) const;
};

// Reads the text of a palettes file. On failure returns nothing and says why
// in `error`.
std::optional<Palettes> read_palettes(std::string_view text, std::string& error);
// The palettes built into the program (assets/palettes.json).
const Palettes& palettes();

// A sprite is a grid of role characters (assets/sprites/legend.txt).
struct Sprite {
    std::string name, sheet;
    int width = 0, height = 0;
    std::string cells;  // row by row, `width` × `height` characters
    char at(int x, int y) const { return cells[static_cast<std::size_t>(y * width + x)]; }
};

// The role a character draws: "outline", "screen", "well", "ink", or a
// family's tone as "ok-dd", "ok-d", "ok" and "ok-l". Empty for '.', which is
// transparent, and for any character the legend does not know.
std::string role_of(char cell);
// The colour of a role character in a theme; none for '.' or an unknown one.
std::optional<Rgb> colour_of(char cell, const Theme& theme);

// Reads a sheet: `sprite NAME WIDTH HEIGHT` and then its rows. Lines that
// start with '#' between sprites are comments. Problems go to `errors`, one
// line each, with the sheet's name and the line number.
std::vector<Sprite> read_sheet(std::string_view sheet, std::string_view text, std::vector<std::string>& errors);

struct SheetText {
    std::string_view name, text;
};
// The sheets built into the program and the legend's text.
std::span<const SheetText> sheet_texts();
std::string_view legend_text();
std::string_view palettes_text();
// Every sprite of every built-in sheet.
const std::vector<Sprite>& sprites();
const Sprite* find_sprite(std::string_view name);

// The sprite in a theme's colours, as RGBA bytes row by row: transparent
// cells are (0, 0, 0, 0), the others opaque.
std::vector<std::uint8_t> paint(const Sprite& sprite, const Theme& theme);

// States drawn from one drawing (G3): a cell takes another role in a state.
// Status lights ('o') take the state's colour; an off device loses its
// colours; a host's screen shows what it does.
enum class DeviceState : std::uint8_t { Off, On, Selected, Sending, Receiving, Error };
inline constexpr std::array<DeviceState, 6> device_states = {DeviceState::Off,     DeviceState::On,        DeviceState::Selected,
                                                             DeviceState::Sending, DeviceState::Receiving, DeviceState::Error};
// A port light: no cable, a cable that carries frames, a cable that does not
// (disabled, or its port is), a frame passing now.
enum class PortState : std::uint8_t { Free, Connected, Down, Active };
inline constexpr std::array<PortState, 4> port_states = {PortState::Free, PortState::Connected, PortState::Down, PortState::Active};
std::string_view state_name(DeviceState state);
std::string_view state_name(PortState state);
char in_state(char cell, DeviceState state);
char in_state(char cell, PortState state);
std::vector<std::uint8_t> paint(const Sprite& sprite, const Theme& theme, DeviceState state);
std::vector<std::uint8_t> paint(const Sprite& sprite, const Theme& theme, PortState state);

// The canvas's grid as one tile of `size` × `size` pixels, repeated: a line
// along its top and left edges, with a stud where they cross.
std::vector<std::uint8_t> grid_tile(const Theme& theme, int size);

// Image files, for the app icon. The pixels `factor` times larger, each art
// pixel a square, centred on a transparent square of `size` pixels.
std::vector<std::uint8_t> enlarge(const Sprite& sprite, const Theme& theme, int factor, int size);
// A PNG file of RGBA pixels, row by row, in zlib's stored blocks: no
// compression, so the writer stays a page long; icons are small.
std::string png(std::span<const std::uint8_t> rgba, int width, int height);
// An ICO file holding PNG images, as Windows Vista and later read them.
struct IconImage {
    int size = 0;
    std::string png;
};
std::string ico(std::span<const IconImage> images);
// The checksums those files use: CRC-32 (ISO 3309) and Adler-32 (RFC 1950).
std::uint32_t crc32(std::string_view bytes);
std::uint32_t adler32(std::string_view bytes);
// The icon's files at every size installers ask for: 16 px is the logo,
// larger ones the app icon, enlarged by whole numbers.
struct IconFile {
    int size = 0;
    std::string png;
};
std::vector<IconFile> app_icons();

// Square pixels. The logical size of one art pixel when `wanted` logical
// pixels are asked for, on a window whose scale factor is `scale_factor`: a
// whole number of physical pixels, at least one.
float art_pixel(float wanted, float scale_factor);
// The canvas's art pixel at a zoom, each the nearest whole number of
// physical pixels: the 32 px drawings about as large as the 36 canvas units
// a card gives its icon, and the 16 px small set when that would be less
// than one physical pixel; the 8 px port lights at about the zoom; packets,
// 16 px, at about one and a half times the zoom, so a frame reads at a
// glance next to a card.
struct CanvasArt {
    bool small = false;
    float unit = 1;         // logical pixels
    float port_unit = 1;    // logical pixels
    float packet_unit = 1;  // logical pixels
    int size() const { return small ? 16 : 32; }
};
inline constexpr float icon_room = 36;  // canvas units
CanvasArt canvas_art(float zoom, float scale_factor);
// A logical position moved to the nearest physical pixel.
float snap(float logical, float scale_factor);

} // namespace resend::look
