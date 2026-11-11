// Writes the app icon's files from the sprites built into the program, for
// the build to install: resend-icons DIR writes DIR/hicolor/NxN/apps/ID.png
// for every size and DIR/resend.ico for Windows.
#include "look/look.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
constexpr const char* app_id = "io.github._0xdokkan.resend";

bool write(const std::filesystem::path& path, const std::string& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: resend-icons DIR\n";
        return 2;
    }
    const std::filesystem::path dir = argv[1];
    std::vector<resend::look::IconImage> images;
    for (const auto& [size, png] : resend::look::app_icons()) {
        const auto name = std::to_string(size) + "x" + std::to_string(size);
        if (!write(dir / "hicolor" / name / "apps" / (std::string(app_id) + ".png"), png)) {
            std::cerr << "resend-icons: cannot write the " << name << " icon in " << dir.string() << '\n';
            return 1;
        }
        images.push_back({size, png});
    }
    if (!write(dir / "resend.ico", resend::look::ico(images))) {
        std::cerr << "resend-icons: cannot write resend.ico in " << dir.string() << '\n';
        return 1;
    }
    return 0;
}
