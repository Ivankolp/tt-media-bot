#!/usr/bin/env bash
# ==============================================================================
# TeamTalk Native Media Daemon - Systemd Service Installer
# Author: Flastik | @flastik_blog
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
Description=TeamTalk Native Media Daemon (C++ Edition by Flastik)
After=network.target teamtalk.service
Wants=network.target

[Service]
Type=simple
User=root
WorkingDirectory=${DIR}
ExecStart=${DIR}/run.sh
Restart=on-failure
RestartPreventExitStatus=0
RestartForceExitStatus=42
RestartSec=1s
LimitNOFILE=65535
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable "${SERVICE_NAME}"

echo "[OK] Service ${SERVICE_NAME} installed and enabled."
echo "[*] Start with:   systemctl start ${SERVICE_NAME}"
echo "[*] Status with:  systemctl status ${SERVICE_NAME}"
echo "[*] Logs with:    journalctl -u ${SERVICE_NAME} -f"
