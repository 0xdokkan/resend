#!/usr/bin/env bash
# Makes the AppImage from a build: installs it into an AppDir, adds the
# libraries the Slint library needs that not every distribution has, and
# packs it with appimagetool, downloaded at a pinned version and checked.
#
#   scripts/make-appimage.sh BUILD_DIR [OUT_DIR]   writes OUT_DIR/resend-VERSION-x86_64.AppImage
#   scripts/make-appimage.sh --appdir BUILD_DIR DIR  only fills the AppDir DIR (nothing is downloaded)
#
# Build on the oldest system the AppImage is for: Ubuntu 22.04 (glibc 2.35,
# the Slint SDK's minimum). Messages go to stderr.
set -euo pipefail

task_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
task_id="io.github._0xdokkan.resend"

# appimagetool and the runtime it puts in front of the image, with the
# digests GitHub publishes for these release assets.
task_tool_url="https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage"
task_tool_sha256="ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"
task_runtime_url="https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64"
task_runtime_sha256="2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d"

# Libraries the AppImage expects from the system: the AppImage project's
# exclude list (github.com/AppImageCommunity/pkg2appimage, file excludelist),
# plus xkbcommon, which must match the system's xkbcommon-x11.
task_excluded=" ld-linux.so.2 ld-linux-x86-64.so.2 libanl.so.1 libBrokenLocale.so.1 libcidn.so.1 libc.so.6 libdl.so.2
 libm.so.6 libmvec.so.1 libnss_compat.so.2 libnss_dns.so.2 libnss_files.so.2 libnss_hesiod.so.2 libnss_nisplus.so.2
 libnss_nis.so.2 libpthread.so.0 libresolv.so.2 librt.so.1 libthread_db.so.1 libutil.so.1 libstdc++.so.6 libGL.so.1
 libEGL.so.1 libGLdispatch.so.0 libGLX.so.0 libOpenGL.so.0 libdrm.so.2 libglapi.so.0 libgbm.so.1 libxcb.so.1
 libX11.so.6 libX11-xcb.so.1 libwayland-client.so.0 libasound.so.2 libfontconfig.so.1 libfreetype.so.6
 libharfbuzz.so.0 libcom_err.so.2 libexpat.so.1 libgcc_s.so.1 libgpg-error.so.0 libICE.so.6 libSM.so.6
 libusb-1.0.so.0 libuuid.so.1 libz.so.1 libjack.so.0 libpipewire-0.3.so.0 libxcb-dri3.so.0 libxcb-dri2.so.0
 libfribidi.so.0 libgmp.so.10 libxkbcommon.so.0 "
task_excluded="${task_excluded//$'\n'/ }"

fail() { printf 'make-appimage: %s\n' "$*" >&2; exit 1; }

appdir_only=0
if [[ "${1:-}" == "--appdir" ]]; then
    appdir_only=1
    shift
    [[ $# -eq 2 ]] || fail "usage: $0 --appdir BUILD_DIR APPDIR"
else
    [[ $# -ge 1 && $# -le 2 ]] || fail "usage: $0 BUILD_DIR [OUT_DIR]"
fi
task_build="$(cd -- "$1" && pwd)"
[[ -f "$task_build/CMakeCache.txt" ]] || fail "$1 is not a CMake build directory"
task_version="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$task_build/CMakeCache.txt")"
[[ -n "$task_version" ]] || fail "no project version in $1/CMakeCache.txt"
for task_command in cmake ldd readelf; do
    command -v "$task_command" >/dev/null || fail "missing command: $task_command"
done

if (( appdir_only )); then
    task_appdir="$2"
    rm -rf -- "$task_appdir"
    mkdir -p -- "$task_appdir"
    task_appdir="$(cd -- "$task_appdir" && pwd)"
else
    task_out="$(mkdir -p -- "${2:-$task_root/dist}" && cd -- "${2:-$task_root/dist}" && pwd)"
    task_temp="$(mktemp -d)"
    trap 'rm -rf -- "$task_temp"' EXIT
    task_appdir="$task_temp/AppDir"
fi

printf 'Installing resend %s into the AppDir\n' "$task_version" >&2
DESTDIR="$task_appdir" cmake --install "$task_build" --prefix /usr --strip >/dev/null
[[ -x "$task_appdir/usr/bin/resend" ]] || fail "the build has no resend program (configured with RESEND_BUILD_GUI=OFF?)"

# Every library a program or a library of the AppDir needs, and where the
# system has it; only what is not excluded is copied, and then followed.
declare -A task_where=()
task_resolve() {
    local line name path
    while IFS= read -r line; do
        if [[ "$line" =~ ^[[:space:]]*([^[:space:]]+)\ =\>\ not\ found ]]; then
            fail "$1 needs ${BASH_REMATCH[1]}, which this system does not have"
        elif [[ "$line" =~ ^[[:space:]]*([^[:space:]]+)\ =\>\ (/[^[:space:]]+) ]]; then
            name="${BASH_REMATCH[1]}"
            path="${BASH_REMATCH[2]}"
            [[ -n "${task_where[$name]:-}" ]] || task_where[$name]="$path"
        fi
    done < <(LD_LIBRARY_PATH="$task_appdir/usr/lib" ldd "$1")
}
task_needed() { readelf -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; }

task_queue=("$task_appdir/usr/bin/resend" "$task_appdir/usr/bin/resend-cli" "$task_appdir/usr/lib/libslint_cpp.so")
task_bundled=()
while (( ${#task_queue[@]} )); do
    task_file="${task_queue[0]}"
    task_queue=("${task_queue[@]:1}")
    task_resolve "$task_file"
    while IFS= read -r task_name; do
        [[ "$task_excluded" == *" $task_name "* ]] && continue
        [[ -e "$task_appdir/usr/lib/$task_name" ]] && continue
        task_path="${task_where[$task_name]:-}"
        [[ -n "$task_path" ]] || fail "cannot find $task_name, needed by $task_file"
        cp -L -- "$task_path" "$task_appdir/usr/lib/$task_name"
        chmod 0755 "$task_appdir/usr/lib/$task_name"
        task_bundled+=("$task_name")
        task_queue+=("$task_appdir/usr/lib/$task_name")
    done < <(task_needed "$task_file")
done

# What was bundled, from which package, with each package's copyright file
# (THIRD-PARTY-NOTICES.md).
task_notices="$task_appdir/usr/share/doc/resend/bundled"
mkdir -p -- "$task_notices"
{
    printf 'Libraries carried by the resend %s AppImage, copied from the build system.\n\n' "$task_version"
    if [[ -r /etc/os-release ]]; then (. /etc/os-release && printf 'Build system: %s\n\n' "${PRETTY_NAME:-unknown}"); fi
} >"$task_notices/README.txt"
for task_name in "${task_bundled[@]}"; do
    task_real="$(readlink -f -- "${task_where[$task_name]}")"
    if command -v dpkg-query >/dev/null; then
        task_package=""
        for task_candidate in "$task_real" "${task_real#/usr}" "${task_where[$task_name]}"; do
            task_package="$(dpkg-query -S "$task_candidate" 2>/dev/null | head -n 1 | cut -d: -f1)" && [[ -n "$task_package" ]] && break
        done
        [[ -n "$task_package" ]] || fail "no Debian package owns $task_real"
        task_package_version="$(dpkg-query -W -f='${Version}' "$task_package")"
        task_source="$(dpkg-query -W -f='${source:Package}' "$task_package")"
        printf '%s  package %s %s, source package %s\n' "$task_name" "$task_package" "$task_package_version" "$task_source" \
            >>"$task_notices/README.txt"
        if [[ -r "/usr/share/doc/$task_package/copyright" ]]; then
            cp -- "/usr/share/doc/$task_package/copyright" "$task_notices/$task_package.copyright"
        else
            fail "the package $task_package has no copyright file"
        fi
    else
        printf '%s  from %s (no package database on the build system)\n' "$task_name" "$task_real" >>"$task_notices/README.txt"
        printf 'make-appimage: warning: no dpkg-query, so %s has no copyright file in the AppDir\n' "$task_name" >&2
    fi
done
printf 'Bundled: %s\n' "${task_bundled[*]:-nothing}" >&2

# The AppDir's own entry point, launcher and icon.
cat >"$task_appdir/AppRun" <<'EOF'
#!/bin/sh
# Starts resend from the AppImage, with the libraries it carries.
here="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$here/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$here/usr/bin/resend" "$@"
EOF
chmod 0755 "$task_appdir/AppRun"
cp -- "$task_appdir/usr/share/applications/$task_id.desktop" "$task_appdir/$task_id.desktop"
cp -- "$task_appdir/usr/share/icons/hicolor/256x256/apps/$task_id.png" "$task_appdir/$task_id.png"
ln -sf -- "$task_id.png" "$task_appdir/.DirIcon"

if (( appdir_only )); then
    printf '%s\n' "$task_appdir"
    exit 0
fi

# appimagetool, kept in .deps/ once checked.
for task_command in curl sha256sum; do
    command -v "$task_command" >/dev/null || fail "missing command: $task_command"
done
task_tools="$task_root/.deps/appimage"
mkdir -p -- "$task_tools"
task_fetch() {  # URL SHA256 FILE
    if [[ -f "$3" ]] && printf '%s  %s\n' "$2" "$3" | sha256sum --check --status; then return; fi
    printf 'Downloading %s\n' "$1" >&2
    curl --fail --location --retry 2 --output "$3.part" "$1"
    printf '%s  %s\n' "$2" "$3.part" | sha256sum --check --status || { rm -f -- "$3.part"; fail "checksum mismatch for $1"; }
    mv -- "$3.part" "$3"
}
task_tool="$task_tools/appimagetool-1.9.1-x86_64.AppImage"
task_runtime="$task_tools/runtime-20251108-x86_64"
task_fetch "$task_tool_url" "$task_tool_sha256" "$task_tool"
task_fetch "$task_runtime_url" "$task_runtime_sha256" "$task_runtime"
chmod 0755 "$task_tool"

# Extracted and run, appimagetool needs no FUSE. The AppStream metadata is
# validated by CI on its own (appstreamcli), with no network.
task_image="$task_out/resend-$task_version-x86_64.AppImage"
ARCH=x86_64 VERSION="$task_version" "$task_tool" --appimage-extract-and-run --no-appstream \
    --runtime-file "$task_runtime" "$task_appdir" "$task_image" >&2
(cd -- "$task_out" && sha256sum -- "$(basename -- "$task_image")" >"$(basename -- "$task_image").sha256")
printf '%s\n' "$task_image"
