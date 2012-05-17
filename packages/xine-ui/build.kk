#!/bin/sh

. config/options

if ! pkg_uses $1 xorg && ! pkg_uses $1 fb; then
  echo "xine-ui needs either xorg or fb support: please enable one of them."
  exit 1
fi

if [ "$DEBUG" = yes ]; then
  DEBUG_CONFIG="--enable-debug"
else
  DEBUG_CONFIG="--disable-debug"
fi

if pkg_uses $1 xorg; then
  XORG_CONFIG="--with-x"
else
  XORG_CONFIG="--without-x"
fi

if pkg_uses $1 vdr; then
  VDR_CONFIG="--enable-vdr-keys"
else
  VDR_CONFIG="--disable-vdr-keys"
fi

if pkg_uses $1 fb; then
  FB_CONFIG="--with-fb"
else
  FB_CONFIG="--without-fb"
fi

cd $BUILD/$1*

#do_autoconf


make
make_install
