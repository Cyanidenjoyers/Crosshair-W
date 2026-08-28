# Crosshair-W

Simple Crosshair Utility for Wayland Compositors.

Crosshair-W is a real `wlr-layer-shell` surface, it's click-through, it
never takes keyboard focus, and it comes with a GTK3 settings GUI so you
don't have to hand-edit a config file to change the color or size. Tested and
confirmed working on KDE Plasma (Wayland) and MangoWM. Should work on any
compositor that supports `wlr-layer-shell`.

## Features

- Four shapes: dot, cross, circle, or dot + cross
- Live color picker, opacity, size, thickness, and center gap
- Size, thickness, and gap sliders adjust automatically to the selected
  shape — e.g. thickness is grayed out for the dot (it has no effect there),
  and cross-based shapes enforce a sane minimum size
- Each shape remembers its own settings, so switching from cross to circle
  and back doesn't overwrite what you tuned — still just one config file
- Optional gamma / color-temperature control via `wlsunset`
- Settings GUI with a live preview against selectable backgrounds
- The GUI starts the overlay daemon automatically if it isn't already
  running

## Installing

### Arch Linux (PKGBUILD)

Not yet on the AUR — build the PKGBUILD directly from the repo:

```
git clone https://github.com/Cyanidenjoyers/Crosshair-W.git
cd Crosshair-W
makepkg -si
```

### Anything else

You'll need a C compiler, `pkg-config`, and the development packages for
GTK3, `gtk-layer-shell`, and `json-c` (`wlsunset` is optional, only needed
for gamma control). Package names vary by distro — the above are the Arch
names; check your distro's package manager for the equivalent `-dev` or
`-devel` packages elsewhere.

```
git clone https://github.com/Cyanidenjoyers/Crosshair-W.git
cd Crosshair-W
make
sudo make install
```

Installs to `/usr/local/bin` by default. Override with `make PREFIX=/usr
install` if you'd rather it go to `/usr/bin`.

## Using it

Launch the settings GUI:

```
crosshair-w
```

Opening it starts the overlay automatically if it isn't running already.
Change anything in the window and the overlay updates live.

The overlay daemon also responds to signals directly, if you want to script
it or bind a key without going through the GUI:

```
pkill -HUP crosshaird   # reload config.json and redraw
pkill crosshaird        # stop the overlay
```

## Config file

Lives at `~/.config/crosshair/config.json`. The GUI writes this for you, so
you generally shouldn't need to touch it by hand, but here's roughly what's
in it:

```json
{
  "style": 1,
  "red": 0.0,
  "green": 1.0,
  "blue": 0.0,
  "alpha": 1.0,
  "size": 10.0,
  "thickness": 2.0,
  "gap": 3.0,
  "enabled": 1,
  "use_gamma": 0,
  "gamma": 1.2,
  "preview_bg": "#ffffff",
  "theme": "default",
  "styles": [
    { "red": 0.0, "green": 1.0, "blue": 0.0, "alpha": 1.0, "size": 3.0, "thickness": 1.0, "gap": 0.0 },
    { "red": 0.0, "green": 1.0, "blue": 0.0, "alpha": 1.0, "size": 10.0, "thickness": 2.0, "gap": 3.0 },
    { "red": 0.0, "green": 1.0, "blue": 0.0, "alpha": 1.0, "size": 3.0, "thickness": 1.0, "gap": 0.0 },
    { "red": 0.0, "green": 1.0, "blue": 0.0, "alpha": 1.0, "size": 3.0, "thickness": 1.0, "gap": 0.0 }
  ]
}
```

The top-level fields (`style`, `red`, `size`, etc.) are always the currently
active shape's values — that's what the daemon actually renders. The
`styles` array underneath is the GUI's memory of each of the four shapes'
settings, indexed 0–3 in the same order as the shape dropdown (dot, cross,
circle, dot + cross), so you can switch between them without losing your
tuning.

## License

MIT. See [LICENSE](LICENSE).
