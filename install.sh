#!/usr/bin/env bash
#
# 🔥 fdash - Modern systemd process manager & dashboard
# One-line installer: curl -fsSL https://raw.githubusercontent.com/Milta-Technologies/fire-dash/main/install.sh | bash
#

set -euo pipefail

REPO="Milta-Technologies/fire-dash"
COLOR_RESET="\033[0m"
COLOR_BOLD="\033[1m"
COLOR_RED="\033[31;1m"
COLOR_GREEN="\033[32;1m"
COLOR_YELLOW="\033[33;1m"
COLOR_CYAN="\033[36;1m"
COLOR_ORANGE="\033[38;5;208;1m"

echo -e "${COLOR_ORANGE}"
cat << "EOF"
  __     _           _     
 / _| __| | __ _ ___| |__  
| |_ / _` |/ _` / __| '_ \ 
|  _| (_| | (_| \__ \ | | |
|_|  \__,_|\__,_|___/_| |_|
EOF
echo -e "${COLOR_RESET}"
echo -e "${COLOR_BOLD}🔥 Installing fdash (Systemd Process Dashboard)${COLOR_RESET}"
echo "------------------------------------------------"

# 1. Detect OS
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
case "${OS}" in
    linux)   OS="linux" ;;
    darwin)  OS="darwin" ;;
    *)
        echo -e "${COLOR_RED}Error: Unsupported operating system: ${OS}${COLOR_RESET}" >&2
        exit 1
        ;;
esac

# 2. Detect Architecture
ARCH="$(uname -m)"
case "${ARCH}" in
    x86_64|amd64)   ARCH="amd64" ;;
    aarch64|arm64)  ARCH="arm64" ;;
    *)
        echo -e "${COLOR_RED}Error: Unsupported architecture: ${ARCH}${COLOR_RESET}" >&2
        exit 1
        ;;
esac

echo -e "Detected platform: ${COLOR_CYAN}${OS}-${ARCH}${COLOR_RESET}"

# 3. Resolve latest version
VERSION="${1:-}"
if [ -z "${VERSION}" ]; then
    echo "Resolving latest version..."
    # Try fetching VERSION from main branch first
    VERSION="$(curl -fsSL --connect-timeout 4 "https://raw.githubusercontent.com/${REPO}/main/VERSION" 2>/dev/null || true)"
    if [ -z "${VERSION}" ]; then
        # Fallback to GitHub releases API
        VERSION="$(curl -fsSL --connect-timeout 4 "https://api.github.com/repos/${REPO}/releases/latest" 2>/dev/null | grep '"tag_name":' | head -n1 | cut -d'"' -f4 || true)"
    fi
fi

# Clean up version string
VERSION="${VERSION#v}"
if [ -z "${VERSION}" ]; then
    VERSION="1.1.0"
fi

echo -e "Target version:    ${COLOR_CYAN}v${VERSION}${COLOR_RESET}"

# 4. Download binary
BINARY_NAME="fdash-${OS}-${ARCH}"
DOWNLOAD_URL="https://github.com/${REPO}/releases/download/v${VERSION}/${BINARY_NAME}"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

TMP_BIN="${TMP_DIR}/fdash"
echo -e "Downloading ${COLOR_BOLD}${BINARY_NAME}${COLOR_RESET}..."
if ! curl -fSL --progress-bar -o "${TMP_BIN}" "${DOWNLOAD_URL}"; then
    # Fallback to latest tag download if explicit tag failed
    LATEST_URL="https://github.com/${REPO}/releases/latest/download/${BINARY_NAME}"
    echo "Retrying download from latest release..."
    if ! curl -fSL --progress-bar -o "${TMP_BIN}" "${LATEST_URL}"; then
        echo -e "${COLOR_RED}Error: Failed to download binary from ${DOWNLOAD_URL}${COLOR_RESET}" >&2
        exit 1
    fi
fi

chmod +x "${TMP_BIN}"

# 5. Quick smoke test of downloaded binary
if ! "${TMP_BIN}" --version >/dev/null 2>&1; then
    echo -e "${COLOR_RED}Error: Downloaded binary failed verification smoke test.${COLOR_RESET}" >&2
    exit 1
fi

# 6. Determine install location
INSTALL_DIR="/usr/local/bin"
USE_SUDO=""

if [ ! -w "${INSTALL_DIR}" ]; then
    if [ "$(id -u)" -eq 0 ]; then
        INSTALL_DIR="/usr/local/bin"
    elif command -v sudo >/dev/null 2>&1; then
        USE_SUDO="sudo"
    else
        INSTALL_DIR="${HOME}/.local/bin"
        mkdir -p "${INSTALL_DIR}"
    fi
fi

TARGET_BIN="${INSTALL_DIR}/fdash"
TARGET_LINK="${INSTALL_DIR}/fire-dash"

echo -e "Installing to ${COLOR_CYAN}${TARGET_BIN}${COLOR_RESET}..."
${USE_SUDO} mkdir -p "${INSTALL_DIR}"
${USE_SUDO} mv "${TMP_BIN}" "${TARGET_BIN}"
${USE_SUDO} chmod 755 "${TARGET_BIN}"

# Create convenience alias symlink fire-dash -> fdash
${USE_SUDO} ln -sf "${TARGET_BIN}" "${TARGET_LINK}" 2>/dev/null || true

# 7. Check PATH
echo ""
echo -e "${COLOR_GREEN}✔ Successfully installed fdash v${VERSION}!${COLOR_RESET}"
echo ""

if [[ ":$PATH:" != *":${INSTALL_DIR}:"* ]]; then
    echo -e "${COLOR_YELLOW}Warning: ${INSTALL_DIR} is not in your \$PATH.${COLOR_RESET}"
    echo -e "Add this to your shell profile (~/.bashrc or ~/.zshrc):"
    echo -e "  export PATH=\"${INSTALL_DIR}:\$PATH\""
    echo ""
fi

echo -e "To get started:"
echo -e "  ${COLOR_BOLD}fdash${COLOR_RESET}               # Open interactive TUI dashboard"
echo -e "  ${COLOR_BOLD}fdash start ./app.js${COLOR_RESET}  # Start an application under systemd"
echo -e "  ${COLOR_BOLD}fdash update${COLOR_RESET}        # Check for and apply self-updates"
echo ""
