# Maintainer: Cyanidenjoyers
pkgname=crosshair-w
pkgver=0.2.1
pkgrel=1
pkgdesc="Simple Wayland crosshair overlay with a GTK3 settings GUI"
arch=('x86_64')
url="https://github.com/Cyanidenjoyers/Crosshair-W"
license=('MIT')
depends=('gtk3' 'gtk-layer-shell' 'json-c')
makedepends=('pkgconf')
optdepends=('wlsunset: gamma/color-temperature adjustment')
install=crosshair-w.install

source=("$pkgname-$pkgver.tar.gz::https://github.com/Cyanidenjoyers/Crosshair-W/archive/refs/tags/v$pkgver.tar.gz")

sha256sums=('c67d8f013cc47b359cac400a2dc67e3ee62c928ad1f8d6fafd4b1526a2e115e2')

build() {
# GitHub names the extracted folder after the repo, not pkgname --
# and pkgname must be lowercase per Arch policy while the actual repo
# is "Crosshair-W", so these deliberately don't match.
cd "$srcdir/Crosshair-W-$pkgver"
make
}

package() {
cd "$srcdir/Crosshair-W-$pkgver"
make DESTDIR="$pkgdir" PREFIX=/usr install
install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
install -Dm644 README.md "$pkgdir/usr/share/doc/$pkgname/README.md"

# Install the icon into the hicolor theme so icon-name lookup can find it.
# Icon= line must reference - with no path and no extension.
install -Dm644 icon.svg \
  "$pkgdir/usr/share/icons/hicolor/scalable/apps/crosshair-w.svg"

# Install desktop file
install -Dm644 /dev/stdin "$pkgdir/usr/share/applications/crosshair-w.desktop" <<EOF
[Desktop Entry]
Name=Crosshair W
Comment=Simple Crosshair GUI for Wayland
Exec=crosshair-w
Icon=crosshair-w
Terminal=false
Type=Application
Categories=Utility;
Keywords=crosshair;overlay;wayland;
EOF
}
