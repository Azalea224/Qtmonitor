pkgname=qtmonitor-git
pkgver=r0.0000000
pkgrel=1
pkgdesc="A Qt6/KF6 resource and task monitor for Linux"
arch=('x86_64')
url="https://github.com/Azalea224/Qtmonitor"
license=('GPL-3.0-only')
depends=('qt6-base' 'kauth' 'kconfig' 'kcoreaddons' 'hicolor-icon-theme')
makedepends=('cmake' 'ninja' 'git')
optdepends=('nvidia-utils: NVIDIA GPU metrics via nvidia-smi'
            'hwdata: GPU model names from pci.ids'
            'qt6-wayland: native Wayland session support')
provides=('qtmonitor')
conflicts=('qtmonitor')
source=("$pkgname::git+$url.git")
sha256sums=('SKIP')

pkgver() {
	cd "$srcdir/$pkgname"
	printf "r%s.g%s" "$(git rev-list --count HEAD)" "$(git rev-parse --short HEAD)"
}

build() {
	cmake -B build -S "$srcdir/$pkgname" -G Ninja \
		-DCMAKE_BUILD_TYPE=None \
		-DCMAKE_INSTALL_PREFIX=/usr \
		-DCMAKE_SKIP_INSTALL_RPATH=ON \
		-Wno-dev
	cmake --build build
}

package() {
	DESTDIR="$pkgdir" cmake --install build
	install -Dm644 "$srcdir/$pkgname/src/icons/qtmonitor.svg" \
		"$pkgdir/usr/share/icons/hicolor/scalable/apps/qtmonitor.svg"
	install -Dm644 "$srcdir/$pkgname/LICENSE" \
		"$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
