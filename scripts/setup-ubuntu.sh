#!/bin/bash
# =============================================================================
# LibAnyar — Development Environment Setup (Ubuntu 22.04)
# =============================================================================
# Run with: sudo bash scripts/setup-ubuntu.sh
#
# This script installs all dependencies needed to build LibAnyar:
#   - Boost 1.81 (fiber, context, asio, url, date_time, atomic, filesystem)
#   - SOCI 4.0.3 (PostgreSQL + SQLite3 backends)
#   - WebKitGTK 4.0 development headers
#   - OpenSSL development headers
#   - SQLite3 + PostgreSQL client libraries
#   - GTK3 development headers
#   - Node.js 20 LTS + npm
#   - nlohmann-json
#   - Build LibAsyik from source and install
# =============================================================================

set -e

BOLD='\033[1m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

step() { echo -e "\n${BOLD}${GREEN}[STEP]${NC} $1\n"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }

if [ "$EUID" -ne 0 ]; then
    echo "Please run with sudo: sudo bash $0"
    exit 1
fi

ACTUAL_USER=${SUDO_USER:-$USER}
ACTUAL_HOME=$(eval echo "~$ACTUAL_USER")
NPROC=$(nproc)
TEMP_DIR=$(mktemp -d)
trap "rm -rf $TEMP_DIR" EXIT

# ─── 1. System packages ─────────────────────────────────────────────────────
step "Installing system packages"
apt-get update -y
apt-get install -y \
    build-essential \
    cmake \
    git \
    wget \
    curl \
    unzip \
    pkg-config \
    libssl-dev \
    libsqlite3-dev \
    libpq-dev \
    libgtk-3-dev \
    libwebkit2gtk-4.0-dev \
    gstreamer1.0-plugins-bad \
    libappindicator3-dev \
    libnotify-dev

# ─── 2. Boost 1.81 ──────────────────────────────────────────────────────────
if [ -f /usr/local/include/boost/version.hpp ]; then
    INSTALLED_BOOST=$(grep "define BOOST_VERSION " /usr/local/include/boost/version.hpp | awk '{print $3}')
    if [ "$INSTALLED_BOOST" -ge 108100 ] 2>/dev/null; then
        warn "Boost >= 1.81 already installed (version code: $INSTALLED_BOOST), skipping"
    else
        warn "Boost installed but version $INSTALLED_BOOST < 108100, rebuilding"
        NEED_BOOST=1
    fi
else
    NEED_BOOST=1
fi

if [ "${NEED_BOOST:-0}" = "1" ]; then
    step "Building and installing Boost 1.81.0"
    cd "$TEMP_DIR"
    wget -q --show-progress https://sourceforge.net/projects/boost/files/boost/1.81.0/boost_1_81_0.tar.gz
    tar -zxf boost_1_81_0.tar.gz
    cd boost_1_81_0
    ./bootstrap.sh
    ./b2 -j"$NPROC" cxxflags="-std=c++17" \
         --with-fiber \
         --with-context \
         --with-atomic \
         --with-date_time \
         --with-filesystem \
         --with-url \
         install
    ldconfig
fi

# ─── 3. SOCI 4.0.3 ──────────────────────────────────────────────────────────
if [ -f /usr/local/lib/cmake/SOCI/SOCIConfig.cmake ]; then
    warn "SOCI already installed, skipping"
else
    step "Building and installing SOCI 4.0.3"
    cd "$TEMP_DIR"
    wget -q --show-progress https://github.com/SOCI/soci/archive/refs/tags/v4.0.3.zip
    unzip -q v4.0.3.zip
    cd soci-4.0.3
    mkdir build && cd build
    cmake .. \
        -DSOCI_WITH_BOOST=ON \
        -DSOCI_WITH_POSTGRESQL=ON \
        -DSOCI_WITH_SQLITE3=ON \
        -DCMAKE_CXX_STANDARD=17 \
        -DSOCI_CXX11=ON
    make -j"$NPROC"
    make install
    ldconfig
fi

# ─── 4. LibAsyik ────────────────────────────────────────────────────────────
if [ -f /usr/local/lib/cmake/libasyik/libasyik-config.cmake ]; then
    warn "LibAsyik already installed, skipping (to reinstall: rm -rf /usr/local/lib/cmake/libasyik)"
else
    step "Building and installing LibAsyik"
    # Always clone a fresh copy from GitHub into the temp directory
    # (never rely on a local checkout which may be on a dev branch)
    LIBASYIK_BUILD_DIR="$TEMP_DIR/libasyik"
    git clone --depth=1 https://github.com/okyfirmansyah/libasyik "$LIBASYIK_BUILD_DIR"
    cd "$LIBASYIK_BUILD_DIR"
    git submodule update --init --recursive

    # Build in a clean directory
    mkdir build_install && cd build_install
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_STANDARD=17 \
        -DLIBASYIK_ENABLE_SOCI=ON
    make -j"$NPROC"
    make install
    ldconfig
fi

# ─── 5. Node.js 20 LTS ──────────────────────────────────────────────────────
if command -v node &>/dev/null; then
    NODE_VER=$(node --version | cut -d. -f1 | tr -d 'v')
    if [ "$NODE_VER" -ge 18 ] 2>/dev/null; then
        warn "Node.js $(node --version) already installed, skipping"
    else
        NEED_NODE=1
    fi
else
    NEED_NODE=1
fi

if [ "${NEED_NODE:-0}" = "1" ]; then
    step "Installing Node.js 20 LTS"
    curl -fsSL https://deb.nodesource.com/setup_20.x | bash -
    apt-get install -y nodejs
fi

# ─── 6. nlohmann-json (header only) ─────────────────────────────────────────
if [ -f /usr/local/include/nlohmann/json.hpp ]; then
    warn "nlohmann/json already installed, skipping"
else
    step "Installing nlohmann/json"
    cd "$TEMP_DIR"
    wget -q https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
    mkdir -p /usr/local/include/nlohmann
    cp json.hpp /usr/local/include/nlohmann/
fi

# ─── Done ────────────────────────────────────────────────────────────────────
step "Setup complete! Installed dependencies:"
echo "  - Boost 1.81    (fiber, context, asio, url)"
echo "  - SOCI 4.0.3    (PostgreSQL + SQLite3)"
echo "  - LibAsyik       (HTTP, WS, SQL, Fibers)"
echo "  - WebKitGTK 4.0 (webview rendering)"
echo "  - OpenSSL        (TLS)"
echo "  - nlohmann/json  (JSON)"
echo "  - Node.js $(node --version 2>/dev/null || echo 'N/A')"
echo ""
echo "You can now build LibAnyar:"
echo "  cd $ACTUAL_HOME/libanyar"
echo "  mkdir build && cd build"
echo "  cmake .. -DCMAKE_BUILD_TYPE=Debug"
echo "  make -j$NPROC"
