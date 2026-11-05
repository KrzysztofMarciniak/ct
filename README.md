# ct

![usage](ct.png)

## Requirements

- C compiler, `make`, `pkg-config`, and `curl` (`make get` downloads Clay 0.14)
- X11, Xft, and FreeType development libraries
- libpng, libjpeg, and libwebp development libraries
- libmpv development library and `libutil`
- An X11 display and a usable monospace font to run

## Build

```sh
make get
make
make install
```

Use `make uninstall` to remove it. Set `PREFIX` to change the install prefix.

## View media

In the running terminal, use `.image /path/to/photo.png` for PNG, JPEG, or WebP
images, or `.video /path/to/video.mp4` for video. Media appears inline in the
terminal.
