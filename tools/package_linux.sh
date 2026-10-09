#!/usr/bin/env bash
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
version="${PXC_PACKAGE_VERSION:-0.1.0}"
client="${1:-$root/build/apps/client/pxc-client}"
stage="$root/artifacts/linux-deb-stage"
metadata="$root/artifacts/linux-deb-metadata"
output="$root/dist/releases"

[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo 'Invalid version' >&2; exit 1; }
[[ -f "$client" ]] || { echo 'Build the Qt client first' >&2; exit 1; }
[[ "$(dpkg --print-architecture)" == amd64 ]] || { echo 'This package recipe currently targets amd64' >&2; exit 1; }
if [[ -d "$stage" ]] && [[ -n "$(find "$stage" -mindepth 1 -print -quit)" ]]; then
    echo 'Staging directory must be empty; user data is never copied or deleted' >&2
    exit 1
fi
mkdir -p "$stage/DEBIAN" "$metadata/debian" "$output"
install -Dm755 "$client" "$stage/usr/lib/pixelconnection/pxc-client"
strip --strip-unneeded "$stage/usr/lib/pixelconnection/pxc-client"
install -Dm644 "$root/src/icon.png" "$stage/usr/share/pixmaps/pixelconnection.png"
mkdir -p "$stage/usr/bin" "$stage/usr/share/applications" "$stage/usr/share/doc/pixelconnection/licenses"
cat > "$stage/usr/bin/pixelconnection" <<'EOF'
#!/bin/sh
exec /usr/lib/pixelconnection/pxc-client "$@"
EOF
chmod 755 "$stage/usr/bin/pixelconnection"
cat > "$stage/usr/share/applications/pixelconnection.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=PixelConnection
Comment=Self-hosted remote desktop and file transfer
Exec=pixelconnection
Icon=pixelconnection
Terminal=false
Categories=Network;RemoteAccess;
StartupWMClass=pxc-client
EOF
for name in LICENSE THIRD_PARTY_NOTICES.md README.md README_EN.md SECURITY.md; do
    install -m644 "$root/$name" "$stage/usr/share/doc/pixelconnection/$name"
done
for dependency in "$root"/build/_deps/*-src \
    "$root/build/_deps/libdatachannel-src/deps/libjuice" \
    "$root/build/_deps/libdatachannel-src/deps/usrsctp"; do
    [[ -d "$dependency" ]] || continue
    while IFS= read -r -d '' license; do
        install -m644 "$license" "$stage/usr/share/doc/pixelconnection/licenses/$(basename "$dependency")-$(basename "$license")"
    done < <(find "$dependency" -maxdepth 1 -type f \( -iname 'license*' -o -iname 'copying*' -o -iname 'copyright*' \) -print0)
done
printf 'Source: https://github.com/PixelStudioforEveryone/Pixel_Connection/tree/%s\n' \
    "${PXC_SOURCE_COMMIT:-$(git -C "$root" rev-parse HEAD)}" > "$stage/usr/share/doc/pixelconnection/SOURCE.txt"
cat > "$metadata/debian/control" <<'EOF'
Source: pixelconnection
Section: net
Priority: optional
Maintainer: PixelStudioforEveryone <noreply@github.com>

Package: pixelconnection
Architecture: amd64
Description: Self-hosted remote desktop
EOF
deps="$(cd "$metadata" && dpkg-shlibdeps -O -e"$stage/usr/lib/pixelconnection/pxc-client")"
deps="${deps#shlibs:Depends=}"
[[ "$deps" == *libqt6widgets6* ]] || { echo 'Qt dependency detection failed' >&2; exit 1; }
installed_size="$(du -sk "$stage/usr" | cut -f1)"
cat > "$stage/DEBIAN/control" <<EOF
Package: pixelconnection
Version: $version
Section: net
Priority: optional
Architecture: amd64
Maintainer: PixelStudioforEveryone <noreply@github.com>
Installed-Size: $installed_size
Depends: $deps, libqt6svg6
Homepage: https://github.com/PixelStudioforEveryone/Pixel_Connection
Description: Free self-hosted remote desktop and file transfer
 Windows, Linux and HarmonyOS clients share account and signaling protocols.
 Supports desktop control, monitor selection, text clipboard and file transfer.
 This amd64 package is built against Ubuntu 22.04 system libraries.
EOF
chmod 755 "$stage/DEBIAN"
chmod 644 "$stage/DEBIAN/control"
dpkg-deb --root-owner-group -Zxz --build "$stage" "$output/pixelconnection_${version}_ubuntu22.04_amd64.deb"
echo "DEB ready in dist/releases/"
