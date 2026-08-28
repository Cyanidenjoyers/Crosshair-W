# Maintainer: Cyanidenjoyers
#
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

source=("$pkgname-$pkgver.tar.gz::https://github.com/Cyanidenjoyers/Crosshair-W/archive/refs/tags/v$pkgver.tar.gz")

sha256sums=('87e7f39d02a023795b284fa38cb03f45474aab6dcd9805a0e07d9e62b53e7e1e')

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
Icon=accessories-calculator
Terminal=false
Type=Application
Categories=Utility;
Keywords=crosshair;overlay;wayland;
EOF
}
