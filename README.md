# Kat: Numpad Mouse Daemon

Kat is a lightweight daemon written in C that turns your full numpad into a mouse input device on Linux systems. It supports mouse movement, clicks, scrolling, autoscrolling, and configurable jumps/margins. The daemon runs in the background as a systemd user service and requires root privileges for input simulation.

## Features
- Toggle mouse mode with double Ctrl press.
- Toggle config panel with double Alt press.
- Use numpad keys (8/2/4/6/7/9/1/3) for directional mouse movement.
- Use Ctrl+Num for larger mouse jumps, or Shift+Ctrl+Num for jumping to edges, corners, or center of screen
- Left-click[5 or NumLock], toggle drag mode[/], middle-click[*] and right-click[-]
- Scroll wheel[+, Enter] and autoscroll mode for continuous scrolling Ctrl+[+,Enter]
- Adjustable speed, acceleration, jump distances, and scroll rates via hotkeys, config panel or directly in `~/.config/kat/config.ini`
- Overlays for visualizing jump margins and intervals.

## Requirements
- Linux (tested on Debian-based systems like Ubuntu).
- X11 (for display interactions).
- Dependencies: `libx11-dev`, `libxtst-dev`, `build-essential` (for building).
- Runtime: `libx11-6`, `libxtst6`, `x11-utils`, `sudo`.

## Installation

### Option 1: Install via .deb Package (Recommended for Debian/Ubuntu)
1. Download the latest `kat_1.0-1.deb` from the [Releases](https://github.com/slipknotxxx/kat-numpad-mouse-daemon/releases) page.
2. Install it: sudo apt install ./kat_1.0-1.deb
    - This handles dependencies automatically.
    - During installation, it sets up the binary, sudoers entry (for your user), and systemd user service file.
3. Complete setup (run as your regular user, no sudo):
   systemctl --user daemon-reload
   systemctl --user enable --now kat.service
4. Reboot or log out/in for the service to start automatically in graphical sessions.
5. To uninstall: sudo apt remove kat
- Manually clean up: `sudo rm /etc/sudoers.d/kat` and `rm ~/.config/systemd/user/kat.service`, then `systemctl --user daemon-reload`.

### Option 2: Build and Install from Source
1. Clone the repo:
   git clone https://github.com/slipknotxxx/kat-numpad-mouse-daemon.git
   cd kat-numpad-mouse-daemon
2. Install build dependencies:
   sudo apt update
   sudo apt install build-essential libx11-dev libxtst-dev
3. Compile the source:
    gcc -o kat kat.c -std=c11 -lX11 -lXtst -lpthread -Wall -lm -Wextra -O2
4. Build the .deb package (using the provided script): ./build-deb.sh
    - This creates `kat_1.0-1.deb` in the current directory.
5. Follow the .deb installation steps above.

#### Manual Installation (Without .deb)
If you prefer not to use the .deb:
1. After compiling `kat`, install it manually:
    sudo install -m 0755 kat /usr/local/bin/kat
    sudo chown root:root /usr/local/bin/kat
2. Set up sudoers (replace `yourusername`):
    sudo visudo -f /etc/sudoers.d/kat
    Add: `yourusername ALL=(root) NOPASSWD:SETENV /usr/local/bin/kat`
3. Create the systemd service:
    mkdir -p ~/.config/systemd/user
    Paste the service content into `~/.config/systemd/user/kat.service` (see the original setup document for the exact [Unit]/[Service]/[Install] sections).
4. Reload and enable:
    systemctl --user daemon-reload
    systemctl --user enable --now kat.service


## Usage
- Start the daemon automatically via systemd (as above).
- Toggle mouse mode: Double-press Ctrl.
- In mouse mode, use the following numpad keys:
  - Numpad 8/2/4/6/7/9/1/3: Move mouse (accelerates over time).
  - Ctrl+Numpad 8/2/4/6/7/9/1/3: Move mouse (larger jumps)
  - Numpad 5 / NumLock: Hold left click.
  - /: Toggle left click.
  - *: Middle click.
  - -: Right click.
  - +/Enter: Manual scroll up/down (hold for continuous).
  - Ctrl + +/Enter: Toggle autoscroll up/down.
  - Alt + various numpad keys: Adjust settings (e.g., Alt+NumLock for speed).
  - Double Alt Press: Open centralised config panel for adjustments.
- Config file: `~/.config/kat/config.ini` (auto-created with defaults).

## Troubleshooting
- If the daemon doesn't start: Check `systemctl --user status kat.service` for errors.
- Compiler errors: Ensure dependencies are installed.
- Permissions: The daemon needs sudo for uinput; verify `/etc/sudoers.d/kat`.
- GUI installer issues: Use terminal install for full automation.

## Building the .deb
The `build-deb.sh` script automates compiling and packaging. Customize `DEBIAN/control` fields (e.g., maintainer) if needed.

## License
MIT License (see LICENSE file).

## Contributing
Pull requests welcome! For major changes, open an issue first.
