#!/bin/sh

. config/options

cd $BUILD/$1*

if [ "$DEBUG" = yes ]; then
  DEBUG_CONFIG="--enable-debug"
else
  DEBUG_CONFIG="--disable-debug"
fi

if pkg_uses $1 xorg; then
  XORG_CONFIG="--with-x --with-xcb --enable-opengl"
else
  XORG_CONFIG="--without-x --without-xcb --disable-opengl"
fi

if [ "$CONSOLE" = yes ]; then
  CONSOLE_CONFIG="--enable-fb"
else
  CONSOLE_CONFIG="--disable-fb"
fi

if pkg_uses $1 sdl; then
  SDL_CONFIG="--with-sdl"
else
  SDL_CONFIG="--without-sdl"
fi

if pkg_uses $1 vdr; then
  VDR_CONFIG="--enable-vdr"
else
  VDR_CONFIG="--disable-vdr"
fi

if pkg_uses $1 vdpau; then
  VDPAU_CONFIG="--enable-vdpau"
  LDFLAGS="$LDFLAGS -lvdpau"
else
  VDPAU_CONFIG="--disable-vdpau"
fi

cd $BUILD/$1*
export LDFLAGS="$LDFLAGS -lm -ldl -lavcodec -lavformat -lavutil -lpostproc -lswscale"


do_configure \
	--enable-pic \
	--enable-shared \
	--enable-pthread \
	--enable-asm \
make
make_install
