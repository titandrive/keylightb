# keylightb

Automatic keyboard backlight for Framework laptops on Linux. The backlight fades on while you type and fades off when you stop, at the level you pick with Fn+Space.

keylightb is a small system daemon written in C with no dependencies. It's a fork of [keylightc](https://gitlab.com/mamarley/keylightc) by Michael Marley, changed to work *with* Fn+Space, the firmware's auto brightness mode and desktop environments like KDE Plasma instead of overriding them.

## Features

- **Fades on with activity.** Typing, and optionally the touchpad, turns the backlight on. It fades out after a few seconds without input.
- **Follows Fn+Space.** Pick a level with Fn+Space and keylightb uses it from then on.
- **Custom levels.** Swap the firmware's Fn+Space steps for levels you choose, for example a dimmer first step.
- **Manual off.** Turn the backlight off with Fn+Space and it stays off until you turn it back on.
- **Firmware auto brightness.** On models with an auto mode in the Fn+Space cycle, keylightb steps aside while it's active, with no flashing.
- **Settings file.** Everything is configured in `/etc/keylightb.conf`.
- **Lightweight.** Sleeps until there's input, works without a desktop session, and runs in a tightly sandboxed systemd service.

## Requirements

- A Framework laptop with a backlit keyboard
- Linux 6.11 or newer (for the `chromeos::kbd_backlight` interface)
- systemd

Tested on the Framework Laptop 13 Pro (Intel Core Ultra Series 3). keylightb watches the built-in keyboard (`AT Translated Set 2 keyboard`) and any input device whose name ends in "Touchpad", so it covers the touchpads on both older and newer models.

## Installation

### Arch Linux / CachyOS

```sh
git clone https://github.com/titandrive/keylightb.git
cd keylightb/pkg
makepkg -si
sudo systemctl enable --now keylightb.service
```

This installs keylightb as a normal pacman package (`keylightb-git`), so it can be removed cleanly and your settings file is kept across upgrades.

### Other distributions

```sh
git clone https://github.com/titandrive/keylightb.git
cd keylightb
make
sudo make install
sudo systemctl enable --now keylightb.service
```

`make install` won't overwrite an existing `/etc/keylightb.conf`.

## Configuration

Settings live in `/etc/keylightb.conf`. Every setting is listed there with its default value and an explanation. Edit it, then restart the service:

```sh
sudo nano /etc/keylightb.conf
sudo systemctl restart keylightb
```

| Setting | What it does | Default |
|---|---|---|
| `brightness` | Brightness (1-100) when keylightb starts. After you pick a level with Fn+Space, that level is used instead. | `30` |
| `levels` | Replace Fn+Space steps with your own levels, as `<firmware level>=<your level>` pairs, e.g. `20=5, 75=40`. Leave empty for none. | empty |
| `timeout` | Seconds without input before the backlight fades out. | `10` |
| `fadeduration` | How long a fade takes, in microseconds (`100000` = 0.1 s). | `100000` |
| `touchpad` | Whether using the touchpad turns the backlight on: `yes` or `no`. | `yes` |

Example:

```ini
brightness = 5
levels = 20=5
timeout = 10
fadeduration = 100000
touchpad = no
```

On the Framework Laptop 13 Pro the Fn+Space steps are **20 → 75 → 100 → auto → off**. Setting `brightness` to the level you use most (or its remapped value) means the keyboard lights at that level right after a reboot too.

The same settings exist as command-line options, which override the file:

```
keylightb [--brightness <1-100>] [--levels <from>=<to>,...] [--timeout <seconds>]
          [--fadeduration <microseconds>] [--touchpad yes|no]
```

## How it behaves

| Fn+Space mode | What happens |
|---|---|
| A fixed level | Fades on while you type and off after `timeout`, at that level (or its `levels` replacement) |
| Auto (if your model has it) | The firmware sets the brightness from the light sensor; keylightb leaves it alone |
| Off | Stays off; typing doesn't turn it on until you press Fn+Space again |

Things to know:

- **Fn+Space is invisible to Linux.** The laptop's embedded controller handles it without sending any input event. keylightb therefore notices a new level, and applies your `levels` replacement, at your **next keypress or touchpad movement** after Fn+Space. If you turn the light on with Fn+Space while it's faded out, it stays on until you next type.
- **Auto mode detection.** In auto mode the firmware ignores brightness changes from the OS. Before fading out, keylightb nudges the brightness by 1% and checks whether it sticks. If it doesn't, keylightb stops controlling the backlight until you leave auto mode. It re-checks whenever you start typing, so a misdetection fixes itself at your next keypress.
- **Levels aren't saved across restarts.** After a reboot keylightb starts at `brightness` until you next press Fn+Space.

## Troubleshooting

See what keylightb is doing:

```sh
journalctl -u keylightb -f
```

It logs the devices it watches, your settings, level changes and auto mode detection at startup and as they happen.

- **The service won't start after editing the settings:** a setting is invalid. The log names the line, e.g. `Invalid setting on line 7 of /etc/keylightb.conf`.
- **The touchpad doesn't turn the backlight on:** check that `touchpad = yes`, and that the log lists a `Touchpad` device under "Using device".
- **The backlight stays on:** you're probably in Fn+Space auto mode, where the firmware is in control. Press Fn+Space to switch to a fixed level.

For more detail (fade on/off decisions), add debug logging with `sudo systemctl edit keylightb`:

```ini
[Service]
LogLevelMax=debug
```

## Uninstalling

```sh
sudo systemctl disable --now keylightb.service
sudo pacman -Rns keylightb-git
```

## Credits

Based on [keylightc](https://gitlab.com/mamarley/keylightc) by Michael Marley, a C take on keylightd.

## License

GPL-2.0-or-later, same as keylightc.
