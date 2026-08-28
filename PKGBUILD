# Maintainer: Cyanidenjoyers
#
pkgname=crosshair-w
pkgver=0.2.2
pkgrel=1
pkgdesc="Simple Wayland crosshair overlay with a GTK3 settings GUI"
arch=('x86_64')
url="https://github.com/Cyanidenjoyers/Crosshair-W"
license=('MIT')
depends=('gtk3' 'gtk-layer-shell' 'json-c')
makedepends=('pkgconf')
optdepends=('wlsunset: gamma/color-temperature adjustment')

source=("$pkgname-$pkgver.tar.gz::https://github.com/Cyanidenjoyers/Crosshair-W/archive/refs/tags/v$pkgver.tar.gz")

sha256sums=('9e9ce92b678b163313f1578453d9d24170d2da9e39688f6630d83e2d5badebcb')

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



  # Install desktop file
  install -Dm644 /dev/stdin "$pkgdir/usr/share/applications/Crosshair-W.desktop" <<EOF
[Desktop Entry]
Name=Crosshair W
Comment=Simple Crosshair GUI for Wayland
Exec=crosshair-w
Icon=icon.svg
Terminal=false
Type=Application
Categories=Utility;
Keywords=crosshair;overlay;wayland;
EOF
}
