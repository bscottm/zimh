#!/bin/sh

install_osx() {
    brew update
    brew install pkgconf pcre2 libpng libedit sdl2 freetype sdl2_ttf \
        vde cmake zlib cmocka ninja
}

install_macports() {
    sudo port install pkgconf pcre2 libpng libedit libsdl2 freetype libsdl2_ttf \
        vde2 cmake util-linux zlib cmocka ninja
}

install_arch_linux() {
    sudo pacman -S --noconfirm base-devel git bison python
    sudo pacman -S --noconfirm pkgconf
    sudo pacman -S --noconfirm pcre2 libpng libedit util-linux zlib
    sudo pacman -S --noconfirm mesa
    sudo pacman -S --noconfirm libsm
    sudo pacman -S --noconfirm freetype2 sdl2-compat sdl2_ttf
    sudo pacman -S --noconfirm libpcap libslirp vde2
    sudo pacman -S --noconfirm cmake cmocka ninja
}

install_linux() {
    sudo apt-get update -yqqm
    sudo apt-get install -ym pkg-config
    sudo apt-get install -ym libpcre2-dev libpng-dev libedit-dev uuid-dev
    sudo apt-get install -ym libegl1-mesa-dev libgles2-mesa-dev
    sudo apt-get install -ym libsdl2-dev libfreetype6-dev libsdl2-ttf-dev
    sudo apt-get install -ym libpcap-dev libvdeplug-dev
    sudo apt-get install -ym cmake cmake-data ninja-build libcmocka-dev
}

install_mingw32() {
    ## Doesn't have libpcap or cmake's extra modules. Not that this
    ## makes much of a difference.
    pacman -S --needed mingw-w64-i686-ninja \
        mingw-w64-i686-cmake \
        mingw-w64-i686-gcc \
	mingw-w64-i686-make \
        mingw-w64-i686-pcre2 \
	mingw-w64-i686-freetype \
        mingw-w64-i686-SDL2 \
	mingw-w64-i686-SDL2_ttf \
	mingw-w64-i686-cmocka
}

install_mingw64() {
    pacman -S --needed mingw-w64-x86_64-ninja \
	mingw-w64-x86_64-cmake \
        mingw-w64-x86_64-extra-cmake-modules \
        mingw-w64-x86_64-gcc \
	mingw-w64-x86_64-make \
        mingw-w64-x86_64-pcre2 \
	mingw-w64-x86_64-freetype \
        mingw-w64-x86_64-SDL2 \
	mingw-w64-x86_64-SDL2_ttf \
	mingw-w64-x86_64-libpcap \
	mingw-w64-x86_64-cmocka
}

install_ucrt64() {
    pacman -S --needed mingw-w64-ucrt-x86_64-ninja \
	mingw-w64-ucrt-x86_64-cmake \
        mingw-w64-ucrt-x86_64-extra-cmake-modules \
        mingw-w64-ucrt-x86_64-gcc \
	mingw-w64-ucrt-x86_64-make \
        mingw-w64-ucrt-x86_64-pcre2 \
	mingw-w64-ucrt-x86_64-freetype \
        mingw-w64-ucrt-x86_64-SDL2 \
	mingw-w64-ucrt-x86_64-SDL2_ttf \
	mingw-w64-ucrt-x86_64-libpcap \
	mingw-w64-ucrt-x86_64-cmocka
}

install_clang64() {
    pacman -S --needed mingw-w64-clang-x86_64-ninja \
        mingw-w64-clang-x86_64-cmake \
	mingw-w64-clang-x86_64-extra-cmake-modules \
        mingw-w64-clang-x86_64-clang \
	mingw-w64-clang-x86_64-make \
        mingw-w64-clang-x86_64-pcre2 \
	mingw-w64-clang-x86_64-freetype \
        mingw-w64-clang-x86_64-SDL2 \
	mingw-w64-clang-x86_64-SDL2_ttf \
	mingw-w64-clang-x86_64-libpcap \
	mingw-w64-clang-x86_64-cmocka
}

install_netbsd_pkgin() {
    pkgin update
    pkgin install pkgconf pcre2 png editline SDL2 freetype SDL2_ttf \
	cmake zlib cmocka ninja bison libslirp
}


case "$1" in
  osx|macports|linux|mingw32|mingw64|ucrt64|clang64)
    install_"$1"
    ;;
  netbsd-pkgin)
    install_netbsd_pkgin
    ;;
  arch-linux)
    install_arch_linux
    ;;
  *)
    echo "$0: Need an operating system name:"
    typeset -f | sed -e '/^install_/!d' -e 's/^install_/  - /' -e 's/ ()//' | sort
    exit 1
    ;;
esac
