#!/bin/bash
# Oracle Cloud VPS setup script for Bitget headless scraper
# Run this on a fresh Ubuntu 22.04+ instance

set -e

echo "=== Updating system ==="
sudo apt update && sudo apt upgrade -y

echo "=== Installing Node.js 20 ==="
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt install -y nodejs

echo "=== Installing Chrome dependencies ==="
sudo apt install -y \
  ca-certificates fonts-liberation libappindicator3-1 libasound2 \
  libatk-bridge2.0-0 libatk1.0-0 libcups2 libdbus-1-3 libdrm2 \
  libgbm1 libgtk-3-0 libnspr4 libnss3 libx11-xcb1 libxcomposite1 \
  libxdamage1 libxrandr2 xdg-utils wget libxss1 libgconf-2-4 \
  libxshmfence1 libglu1-mesa

echo "=== Setting up project ==="
mkdir -p ~/bitget-scraper
cd ~/bitget-scraper

# Copy files (or git clone)
echo "Copy your headless/ folder contents here, then run:"
echo "  npm install"
echo "  cp .env.example .env"
echo ""
echo "=== First-time login ==="
echo "You need to log in once. Options:"
echo ""
echo "  Option A: Login from your PC, copy session"
echo "    1. On your PC:  cd headless && npm run login"
echo "    2. Log in to Bitget in the browser, press ENTER"
echo "    3. Copy browser-data/ folder to VPS:"
echo "       scp -r browser-data/ user@your-vps-ip:~/bitget-scraper/"
echo ""
echo "  Option B: Login on VPS via VNC/X11 forwarding"
echo "    1. ssh -X user@your-vps-ip"
echo "    2. npm run login"
echo "    3. Log in and press ENTER"
echo ""
echo "=== After login, install the systemd service ==="
echo "  sudo cp bitget-scraper.service /etc/systemd/system/"
echo "  sudo systemctl daemon-reload"
echo "  sudo systemctl enable bitget-scraper"
echo "  sudo systemctl start bitget-scraper"
echo ""
echo "  Check logs: sudo journalctl -u bitget-scraper -f"
