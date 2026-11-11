// The look in the window: the theme's colours on the Theme global, the
// sprites painted in them on UiSprites, and the art pixel snapped to the
// window's scale factor (docs/decisions/0003-visual-direction.md).
#include "app_controller.hpp"

#include "app/scene.hpp"
#include "core/text.hpp"
#include "look/look.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace resend {
namespace {
using netsim::text::tr;

slint::Color colour(look::Rgb rgb) { return slint::Color::from_rgb_uint8(rgb.r, rgb.g, rgb.b); }

Tones tones(const look::Family& family) {
    Tones result;
    result.dd = colour(family.tones[0]);
    result.d = colour(family.tones[1]);
    result.base = colour(family.tones[2]);
    result.l = colour(family.tones[3]);
    result.text = colour(family.text);
    result.chip = colour(family.chip);
    result.chip_ink = colour(family.chip_ink);
    return result;
}

slint::Image image(const std::vector<std::uint8_t>& pixels, int width, int height) {
    std::vector<slint::Rgba8Pixel> buffer(pixels.size() / 4);
    for (std::size_t i = 0; i < buffer.size(); ++i)
        buffer[i] = {pixels[i * 4], pixels[i * 4 + 1], pixels[i * 4 + 2], pixels[i * 4 + 3]};
    return slint::Image(slint::SharedPixelBuffer<slint::Rgba8Pixel>(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                                                                  buffer.data()));
}
slint::Image image(const look::Sprite& sprite, const look::Theme& theme) { return image(look::paint(sprite, theme), sprite.width, sprite.height); }

const look::Sprite& sprite_named(std::string_view name) {
    const auto* sprite = look::find_sprite(name);
    if (!sprite) throw std::logic_error("no sprite named " + std::string(name));
    return *sprite;
}

// The sprites painted in every state, into the arrays of UiSprites.
constexpr std::string_view device_sprites[] = {"host", "switch", "router"};
constexpr std::string_view small_sprites[] = {"host-small", "switch-small", "router-small"};

std::shared_ptr<slint::VectorModel<slint::Image>> device_images(std::span<const std::string_view> names, const look::Theme& theme) {
    std::vector<slint::Image> images;
    for (const auto name : names) {
        const auto& sprite = sprite_named(name);
        for (const auto state : look::device_states) images.push_back(image(look::paint(sprite, theme, state), sprite.width, sprite.height));
    }
    return std::make_shared<slint::VectorModel<slint::Image>>(images);
}

// Every sprite of assets/sprites/ and its property on UiSprites.
using Setter = void (UiSprites::*)(const slint::Image&) const;
struct SpriteSetter {
    std::string_view name;
    Setter set;
};
const SpriteSetter sprite_setters[] = {
    {"host", &UiSprites::set_host},
    {"switch", &UiSprites::set_switch},
    {"router", &UiSprites::set_router},
    {"host-small", &UiSprites::set_host_small},
    {"switch-small", &UiSprites::set_switch_small},
    {"router-small", &UiSprites::set_router_small},
    {"arp-request", &UiSprites::set_arp_request},
    {"arp-reply", &UiSprites::set_arp_reply},
    {"echo-request", &UiSprites::set_echo_request},
    {"echo-reply", &UiSprites::set_echo_reply},
    {"logo", &UiSprites::set_logo},
    {"app-icon", &UiSprites::set_app_icon},
    {"play", &UiSprites::set_play},
    {"pause", &UiSprites::set_pause},
    {"step", &UiSprites::set_step},
    {"reset", &UiSprites::set_reset},
    {"sound", &UiSprites::set_sound},
    {"mute", &UiSprites::set_mute},
    {"effects", &UiSprites::set_effects},
    {"goal-done", &UiSprites::set_goal_done},
    {"goal-open", &UiSprites::set_goal_open},
    {"caret", &UiSprites::set_caret},
    {"undo", &UiSprites::set_undo},
    {"redo", &UiSprites::set_redo},
    {"chevron-right", &UiSprites::set_chevron_right},
    {"chevron-left", &UiSprites::set_chevron_left},
    {"chevron-down", &UiSprites::set_chevron_down},
    {"chevron-up", &UiSprites::set_chevron_up},
    {"close", &UiSprites::set_close},
    {"check", &UiSprites::set_check},
    {"warning", &UiSprites::set_warning},
    {"power", &UiSprites::set_power},
    {"plus", &UiSprites::set_plus},
    {"minus", &UiSprites::set_minus},
    {"fit", &UiSprites::set_fit},
    {"trash", &UiSprites::set_trash},
    {"menu", &UiSprites::set_menu},
    {"search", &UiSprites::set_search},
    {"arrow-right", &UiSprites::set_arrow_right},
    {"arrow-left", &UiSprites::set_arrow_left},
    {"traceroute", &UiSprites::set_traceroute},
    {"home", &UiSprites::set_home},
    {"terminal", &UiSprites::set_terminal},
    {"sliders", &UiSprites::set_sliders},
    {"table", &UiSprites::set_table},
    {"keyboard", &UiSprites::set_keyboard},
    {"theme", &UiSprites::set_theme},
    {"lock", &UiSprites::set_lock},
    {"hint", &UiSprites::set_hint},
    {"lab", &UiSprites::set_lab},
};
} // namespace

void AppController::apply_theme() {
    const auto& theme = look::palettes().find(session_theme_ ? *session_theme_ : settings_.theme);
    const auto& global = ui_->global<Theme>();
    global.set_dark(theme.dark);
    global.set_outline(colour(theme.surface("outline")));
    global.set_shade(colour(theme.surface("shade")));
    global.set_base(colour(theme.surface("base")));
    global.set_canvas(colour(theme.surface("canvas")));
    global.set_grid(colour(theme.surface("grid")));
    global.set_panel(colour(theme.surface("panel")));
    global.set_raised(colour(theme.surface("raised")));
    global.set_raised_edge(colour(theme.surface("raised-edge")));
    global.set_hover(colour(theme.surface("hover")));
    global.set_sunken(colour(theme.surface("sunken")));
    global.set_edge(colour(theme.surface("edge")));
    global.set_line(colour(theme.surface("line")));
    global.set_screen(colour(theme.surface("screen")));
    global.set_ink(colour(theme.ink("ink")));
    global.set_muted(colour(theme.ink("muted")));
    global.set_on_accent(colour(theme.ink("on-accent")));
    global.set_screen_ink(colour(theme.ink("screen-ink")));
    global.set_metal(tones(theme.family("metal")));
    global.set_accent(tones(theme.family("accent")));
    global.set_ok(tones(theme.family("ok")));
    global.set_link(tones(theme.family("link")));
    global.set_ask(tones(theme.family("ask")));
    global.set_route(tones(theme.family("route")));
    global.set_drop(tones(theme.family("drop")));
    global.set_guard(tones(theme.family("guard")));

    const auto& sprites = ui_->global<UiSprites>();
    for (const auto& [name, set] : sprite_setters) {
        // The app icon is the same in every theme, as on the desktop.
        (sprites.*set)(image(sprite_named(name), name == "app-icon" ? look::palettes().find(look::palettes().default_id) : theme));
    }
    sprites.set_devices(device_images(device_sprites, theme));
    sprites.set_devices_small(device_images(small_sprites, theme));
    std::vector<slint::Image> ports, packets;
    const auto& port = sprite_named("port");
    for (const auto state : look::port_states) ports.push_back(image(look::paint(port, theme, state), port.width, port.height));
    for (const auto kind : pdu_kinds) packets.push_back(image(sprite_named(packet_sprite(kind)), theme));
    sprites.set_ports(std::make_shared<slint::VectorModel<slint::Image>>(ports));
    sprites.set_packets(std::make_shared<slint::VectorModel<slint::Image>>(packets));
    paint_grid();

    std::vector<ThemeView> themes;
    for (const auto& item : look::palettes().themes) themes.push_back({item.id.c_str(), item.title.c_str()});
    const auto& app = ui_->global<UiApp>();
    app.set_themes(std::make_shared<slint::VectorModel<ThemeView>>(themes));
    app.set_theme(theme.id.c_str());
}

void AppController::set_theme(const std::string& id) {
    const auto& theme = look::palettes().find(id);
    settings_.theme = theme.id;
    session_theme_.reset();
    save_settings();
    apply_theme();
    note(tr("Theme: {0}.", {theme.title}));  // the window itself shows it
}

// The art pixel: about 2 logical pixels on the HUD and 1 for an icon, and
// on the canvas the largest whole number of physical pixels that the zoom
// allows, so sprites stay square and never outgrow their cards; far away,
// the small set (look::canvas_art).
void AppController::refresh_units() {
    const float factor = ui_->window().scale_factor();
    const float zoom = canvas_.zoom();
    if (factor == shown_scale_ && zoom == shown_zoom_) return;
    shown_scale_ = factor;
    shown_zoom_ = zoom;
    const auto& sprites = ui_->global<UiSprites>();
    sprites.set_scale(factor);
    sprites.set_unit(look::art_pixel(2, factor));
    sprites.set_icon_unit(look::art_pixel(1, factor));
    const auto art = look::canvas_art(zoom, factor);
    sprites.set_canvas_small(art.small);
    sprites.set_canvas_unit(art.unit);
    sprites.set_packet_unit(art.packet_unit);
    sprites.set_port_unit(art.port_unit);
    paint_grid();
}

// The grid's tile follows the zoom in whole logical pixels: the grid is
// decoration, so its step may differ from 28 canvas units by a fraction.
void AppController::paint_grid() {
    const auto& theme = look::palettes().find(session_theme_ ? *session_theme_ : settings_.theme);
    const int size = std::max(8, static_cast<int>(std::lround(28 * canvas_.zoom())));
    const auto& sprites = ui_->global<UiSprites>();
    sprites.set_grid_tile(image(look::grid_tile(theme, size), size, size));
    sprites.set_grid_size(static_cast<float>(size));
}

std::vector<std::string_view> AppController::painted_sprites() {
    std::vector<std::string_view> names(sprite_properties().begin(), sprite_properties().end());
    names.insert(names.end(), std::begin(device_sprites), std::end(device_sprites));
    names.insert(names.end(), std::begin(small_sprites), std::end(small_sprites));
    names.push_back("port");
    for (const auto kind : pdu_kinds) names.push_back(packet_sprite(kind));
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

std::span<const std::string_view> AppController::sprite_properties() {
    static const std::vector<std::string_view> names = [] {
        std::vector<std::string_view> result;
        for (const auto& [name, set] : sprite_setters) result.push_back(name);
        return result;
    }();
    return names;
}

} // namespace resend
