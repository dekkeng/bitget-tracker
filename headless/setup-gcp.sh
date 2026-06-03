#!/bin/bash
# Google Cloud e2-micro setup script for Bitget headless scraper
# Run this on a fresh Debian 12 / Ubuntu 22.04 instance (GCP default)

set -e

echo "=== 1. Adding 2GB swap (critical for 1GB RAM instance) ==="
sudo fallocate -l 2G /swapfile
sudo chmod 600 /swapfile
sudo mkswap /swapfile
sudo swapon /swapfile
echo '/swapfile none swap sw 0 0' | sudo tee -a /etc/fstab
echo "vm.swappiness=60" | sudo tee -a /etc/sysctl.conf
sudo sysctl vm.swappiness=60
echo "Swap enabled:"
free -h

echo ""
echo "=== 2. Installing Node.js 20 ==="
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt install -y nodejs

echo ""
echo "=== 3. Installing Chrome dependencies ==="
sudo apt install -y \
  ca-certificates fonts-liberation libasound2 libatk-bridge2.0-0 \
  libatk1.0-0 libcups2 libdbus-1-3 libdrm2 libgbm1 libgtk-3-0 \
  libnspr4 libnss3 libx11-xcb1 libxcomposite1 libxdamage1 \
  libxrandr2 xdg-utils wget libxss1 libxshmfence1 libglu1-mesa

echo ""
echo "=== 4. Setting up project ==="
mkdir -p ~/bitget-scraper
echo ""
echo "=== DONE! Next steps: ==="
echo ""
echo "  1. On your PC, log in first:"
echo "     cd headless"
echo "     npm install"
echo "     cp .env.example .env"
echo "     npm run login"
echo "     (Log in to Bitget, press ENTER)"
echo ""
echo "  2. Copy files to this VPS:"
echo "     scp -i ~/.ssh/gcp-key -r headless/* YOUR_USER@EXTERNAL_IP:~/bitget-scraper/"
echo ""
echo "  3. On VPS, install and start:"
echo "     cd ~/bitget-scraper"
echo "     npm install"
echo "     cp .env.example .env"
echo "     node scraper.js          # test first"
echo ""
echo "  4. If working, enable auto-start:"
echo "     sudo cp bitget-scraper.service /etc/systemd/system/"
echo "     sudo systemctl daemon-reload"
echo "     sudo systemctl enable bitget-scraper"
echo "     sudo systemctl start bitget-scraper"
echo "     sudo journalctl -u bitget-scraper -f"
