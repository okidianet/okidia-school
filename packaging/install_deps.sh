#!/bin/bash

# OKIDIA Build Dependencies Installation Script
# This script installs the necessary packages for building OKIDIA on Debian/Ubuntu systems.

set -e

# Check if running as root
if [ "$EUID" -ne 0 ]; then
  echo "Please run as root or using sudo:"
  echo "sudo $0"
  exit 1
fi

echo "Updating package lists..."
apt-get update

echo "Installing build dependencies..."
apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    qt6-base-dev \
    qt6-multimedia-dev \
    qt6-tools-dev \
    libssl-dev \
    libprotobuf-dev \
    protobuf-compiler \
    libssh2-1-dev \
    libsystemd-dev \
    libavahi-client-dev \
    libxtst-dev \
    libx11-dev \
    libsqlite3-dev

# JPEG dev paketi farklı dağıtımlarda farklı isimlerde olabiliyor (libjpeg-turbo8-dev, libjpeg-dev, vb.)
echo "Checking for JPEG development package..."
JPEG_PKG=""
for pkg in libjpeg-turbo8-dev libjpeg-dev libjpeg8-dev; do
    if apt-cache show "$pkg" >/dev/null 2>&1; then
        JPEG_PKG="$pkg"
        break
    fi
done

if [ -n "$JPEG_PKG" ]; then
    echo "Installing $JPEG_PKG..."
    apt-get install -y "$JPEG_PKG"
else
    echo "Warning: No JPEG development package found. Build might fail."
fi

echo "-------------------------------------------------------"
echo "All dependencies installed successfully!"
echo "You can now build the project using:"
echo "  mkdir -p build && cd build"
echo "  cmake .."
echo "  make -j$(nproc)"
echo "-------------------------------------------------------"
