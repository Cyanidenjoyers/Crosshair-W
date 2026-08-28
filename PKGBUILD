# Maintainer: Cyanidenjoyers
pkgname=crosshair-w
pkgver=0.1.0
pkgrel=1
pkgdesc="Simple Wayland crosshair overlay with a GTK3 settings GUI"
arch=('x86_64')
url="https://github.com/Cyanidenjoyers/Crosshair-W"
license=('MIT')
depends=('gtk3' 'gtk-layer-shell' 'json-c')
makedepends=('pkgconf')
optdepends=('wlsunset: gamma/color-temperature adjustment')

# The repo has no tags yet, so this points at where the v$pkgver release
# archive will live once one exists. Tag it (`git tag v$pkgver && git push
# --tags`, or cut a GitHub Release) before this will actually resolve.
source=("$pkgname-$pkgver.tar.gz::https://github.com/Cyanidenjoyers/Crosshair-W/archive/refs/tags/v$pkgver.tar.gz")

# Placeholder until v$pkgver is tagged. SKIP builds fine locally with
# makepkg, but the AUR does not accept SKIP for a regular tarball source
# (only VCS packages using a pkgver() function get that pass) -- run
# `updpkgsums` (or `makepkg -g` and paste the result in) once the tag
# exists, and commit the real hash before submitting.
sha256sums=('SKIP')

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
