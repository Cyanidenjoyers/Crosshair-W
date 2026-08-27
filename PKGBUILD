# Maintainer: Your Name <your.email@example.com>
pkgname=crosshair-w
pkgver=0.1.0
pkgrel=1
pkgdesc="Simple Crosshair GUI for Wayland, built on GTK3 and gtk-layer-shell"
arch=('x86_64')
url="https://github.com/Cyanidenjoyers/Crosshair-W"
license=('MIT')
depends=('gtk3' 'gtk-layer-shell' 'json-c' 'wlsunset')
makedepends=('gcc' 'make' 'pkg-config' 'gtk3' 'gtk-layer-shell' 'json-c')
source=("$pkgname-$pkgver.tar.gz::https://github.com/Cyanidenjoyers/Crosshair-W/archive/v$pkgver.tar.gz")
sha256sums=('SKIP')

build() {
  cd "$srcdir/$pkgname-$pkgver"
  make
}

package() {
  cd "$srcdir/$pkgname-$pkgver"
  make DESTDIR="$pkgdir" PREFIX=/usr install

  # Install desktop file
  install -Dm644 /dev/stdin "$pkgdir/usr/share/applications/crosshair-w.desktop" <<EOF
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