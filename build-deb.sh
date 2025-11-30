#!/bin/bash

# Compile the binary
gcc -o kat kat.c -std=c11 -lX11 -lXtst -lpthread -Wall -lm -Wextra -O2

# Create package structure
mkdir -p kat_1.0-1/DEBIAN
mkdir -p kat_1.0-1/usr/local/bin

# Copy binary
cp kat kat_1.0-1/usr/local/bin/kat

# Create control file
cat << EOF > kat_1.0-1/DEBIAN/control
Package: kat
Version: 1.0-1
Section: utils
Priority: optional
Architecture: amd64
Depends: libx11-6, libxtst6, x11-utils, sudo
Maintainer: Your Name <your.email@example.com>
Description: Kat's Numpad Mouse Daemon
 A daemon that uses the numpad as a mouse input device.
 It runs as a user systemd service and requires root privileges for input simulation.
EOF

# Create postinst script
cat << EOF > kat_1.0-1/DEBIAN/postinst
#!/bin/sh
set -e

# Set binary permissions
chown root:root /usr/local/bin/kat
chmod 0755 /usr/local/bin/kat

if [ -z "\$SUDO_USER" ]; then
  SUDO_USER=\$(who | grep '(:0)' | awk '{print \$1}' | head -n1)
  if [ -z "\$SUDO_USER" ]; then
    echo "Warning: Could not detect installing user. Skipping sudoers and service setup. Please configure manually."
    exit 0
  fi
fi

# Set up sudoers
echo "\$SUDO_USER ALL=(root) SETENV: NOPASSWD: /usr/local/bin/kat" > /etc/sudoers.d/kat
chmod 0440 /etc/sudoers.d/kat

# Get user home and group
USER_HOME=\$(getent passwd \$SUDO_USER | cut -d: -f6)
USER_GROUP=\$(id -gn \$SUDO_USER)

# Set up systemd service
mkdir -p \$USER_HOME/.config/systemd/user
cat << EOT > \$USER_HOME/.config/systemd/user/kat.service
[Unit]
Description=Kat's Numpad Mouse Daemon
After=graphical-session.target
Wants=graphical-session.target

[Service]
Type=simple
ExecStart=/usr/bin/sudo -E /usr/local/bin/kat
Restart=always
RestartSec=3
Environment=DISPLAY=:0
Environment=XAUTHORITY=%h/.Xauthority
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=default.target

ExecStartPre=/usr/bin/xhost +local:
EOT

# Set ownership
chown -R \$SUDO_USER:\$USER_GROUP \$USER_HOME/.config/systemd

echo "Kat package installed. To complete setup, log in as \$SUDO_USER and run:"
echo "systemctl --user daemon-reload"
echo "systemctl --user enable --now kat.service"
echo "If needed, reboot or log out/in for the graphical session to start the service automatically."
EOF

chmod +x kat_1.0-1/DEBIAN/postinst

# Build deb
dpkg-deb --build --root-owner-group kat_1.0-1

# Cleanup temp
rm -rf kat_1.0-1 kat

echo "Built kat_1.0-1.deb"
