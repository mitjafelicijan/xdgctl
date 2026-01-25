`xdgctl` is a TUI for managing XDG default applications. View and set defaults for file categories without using `xdg-mime` directly.

Built with C using [GLib/GIO](https://docs.gtk.org/gio/) and [termbox2](https://github.com/termbox/termbox2).

https://github.com/user-attachments/assets/076c9934-f373-486d-9595-eec480e3a429

## Features

- Browse by category (Browsers, Text Editors, etc.)
- Current default marked with `*`

## Navigation & Controls

| Key                 | Action                                                        |
|---------------------|---------------------------------------------------------------|
| **Arrow Up/Down**   | Navigate through categories or applications                   |
| **Arrow Right/Tab** | Switch from category list to application list                 |
| **Arrow Left**      | Switch back to category list                                  |
| **Enter**           | Set selected application as default for current category      |
| **Esc / q**         | Quit the application                                          |

## Prerequisites

To build `xdgctl`, you need the following development libraries:

- `glib-2.0`
- `gio-2.0`
- `gio-unix-2.0`
- `clang` or `gcc`

```bash
# On Void Linux
sudo xbps-install glibc-devel pkg-config
```

## Installation

```bash
git clone https://github.com/mitjafelicijan/xdgctl.git
cd xdgctl

# Build
make
sudo make install

# Using prefix
sudo make PREFIX=/usr/local install
make PREFIX=~/.local install
```

If you manually add new applications to your `~/.local/share/applications` directory, you might need to run `update-desktop-database` again.

## More about XDG

### Application directories

```bash
ls /usr/share/applications
ls ~/.local/share/applications
```

### Querying defaults

```bash
xdg-mime query default text/plain
xdg-mime query default text/html
xdg-mime query default x-scheme-handler/http
xdg-mime query default x-scheme-handler/https
xdg-mime query default inode/directory
```

### Setting defaults manually

```bash
xdg-mime default brave.desktop x-scheme-handler/http
xdg-mime default brave.desktop x-scheme-handler/https
```

### Desktop Entry example

```ini
# ~/.local/share/applications/brave.desktop
[Desktop Entry]
Exec=/home/m/Applications/brave
Type=Application
Categories=Applications
Name=Brave Browser
MimeType=text/html;text/xml;application/xhtml+xml;x-scheme-handler/http;x-scheme-handler/https;
```

### Other useful commands/files
```bash
update-desktop-database ~/.local/share/applications
less ~/.config/mimeapps.list
less /usr/share/applications/mimeapps.list
```

## More material

- https://commandmasters.com/commands/xdg-mime-linux/
- https://noman.sh/en/pages/xdg-mime
- https://linux.die.net/man/1/xdg-mime
- https://wiki.archlinux.org/title/XDG_MIME_Applications
- https://gnome.pages.gitlab.gnome.org/libsoup/gio/
- https://docs.gtk.org/gio/
