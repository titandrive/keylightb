keylightb
=========
Keyboard backlight daemon for Framework laptops in C, that follows your brightness choice

keylightb is a small system daemon for Framework laptops that listens to keyboard and touchpad input, and turns on the keyboard backlight while either is being used. It is a fork of [keylightc](https://gitlab.com/mamarley/keylightc) by Michael Marley that cooperates with Fn+Space and desktop environments like KDE Plasma instead of overriding them.

Differences from keylightc
--------------------------
* **Remembers your brightness.** If you change the backlight level with Fn+Space or your desktop's brightness control, keylightb uses that level from then on instead of jumping back to `--brightness`. `--brightness` is only the starting level.
* **Manual off switch.** If you turn the backlight off yourself while it's lit, keylightb leaves it off until you turn it back on.
* **Plays nice with firmware auto brightness.** On models where Fn+Space includes an auto mode (e.g. the Framework Laptop 13 Pro), the firmware sets the level from the ambient light sensor and ignores the OS. keylightb detects this without any visible change (it nudges the level by 1% and checks whether it sticks) and leaves the backlight alone until you switch out of auto.
* **Smooth fades after outside changes.** Fades start from the brightness actually applied, so they don't glitch after something else changed the level.
* **Works with more touchpads.** Any input device whose name ends in "Touchpad" is watched, so it supports the touchpads on both older and newer Framework models (e.g. `PIXA3854:00 093A:0274` and `PIXA3854:00 093A:1343`).

Installation
------------
### Arch Linux / CachyOS
A PKGBUILD is included in `pkg/`:

	$ cd pkg
	$ makepkg -si
	$ sudo systemctl enable --now keylightb.service

### From source
keylightb has no dependencies you have to install first. It works using the sysfs keyboard backlight interface introduced in Linux 6.11 and therefore requires at least that version. It uses kernel event data directly and does not depend on a desktop environment or display server.

	$ make
	$ sudo make install
	$ sudo systemctl enable --now keylightb.service

Running
-------
keylightb must either be run as root or using the included systemd unit.

	Usage: keylightb [--brightness <brightness>] [--fadeduration <fadeduration>] [--timeout <timeout>]
	Options:
	  --brightness          starting brightness level when active (1-100) [default=30]
	  --fadeduration        fade time in microseconds (1-1000000) [default=100000]
	  --timeout             activity timeout in seconds (1-2147483647) [default=10]
	  --help                display usage information

To change the options, override the service with `sudo systemctl edit keylightb.service`:

	[Service]
	ExecStart=
	ExecStart=/usr/bin/keylightb --timeout 15

### How Fn+Space modes behave

| Fn+Space mode | Behavior |
|---|---|
| Fixed level | Fades on while you type and off after the timeout, at that level |
| Auto (if your model has it) | The firmware's light-sensor brightness, left alone by keylightb |
| Off | Stays off; typing doesn't turn it on |

Fn+Space is handled entirely by the embedded controller and doesn't produce an input event, so if you turn the backlight on with Fn+Space while it's faded out, keylightb notices at your next keypress or touchpad movement.

Brightness changes are logged, so you can watch what it's doing with `journalctl -u keylightb -f`.

License
-------
GPL-2.0-or-later, same as keylightc.
