#!/usr/bin/env bash
# ==============================================================================
# TeamTalk Native Media Daemon - Systemd Service Installer
# Author: Ivan (Ivankolp) | Space Project (@Space_project_fl) | @flastik_blog
# ==============================================================================

set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
SERVICE_NAME="tt-native-bot.service"
SERVICE_PATH="/etc/systemd/system/${SERVICE_NAME}"

if [ "$EUID" -ne 0 ]; then
    echo "[x] Please run as root (sudo ./install_service.sh)"
    exit 1
fi

echo "[*] Creating systemd service at ${SERVICE_PATH}..."

cat <<EOF > "${SERVICE_PATH}"
[Unit]
Description=TeamTalk Native Media Daemon (C++ Edition by Ivan)
After=network.target teamtalk.service
Wants=network.target

[Service]
Type=simple
User=root
WorkingDirectory=${DIR}
ExecStart=${DIR}/run.sh
Restart=always
RestartSec=5
LimitNOFILE=65535
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable "${SERVICE_NAME}"

echo "[✓] Service ${SERVICE_NAME} installed and enabled!"
echo "[*] You can start it with:   systemctl start ${SERVICE_NAME}"
echo "[*] Check status with:       systemctl status ${SERVICE_NAME}"
echo "[*] View live logs with:     journalctl -u ${SERVICE_NAME} -f"
