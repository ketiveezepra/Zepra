#!/bin/bash

# Zepra Core Browser Build Script
# Builds the enhanced Zepra Core browser with integrated authentication system

set -e

echo "🦓 Building Zepra Core Browser with Authentication System"
echo "========================================================"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Function to print colored output
print_status() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if we're in the right directory
if [ ! -f "CMakeLists.txt" ]; then
    print_error "CMakeLists.txt not found. Please run this script from the zepra directory."
    exit 1
fi

# Check for required dependencies
print_status "Checking dependencies..."

# Check for CMake
if ! command -v cmake &> /dev/null; then
    print_error "CMake not found. Please install CMake."
    exit 1
fi

# Check for compiler
if command -v g++ &> /dev/null; then
    COMPILER="g++"
elif command -v clang++ &> /dev/null; then
    COMPILER="clang++"
else
    print_error "No C++ compiler found. Please install g++ or clang++."
    exit 1
fi

print_success "Using compiler: $COMPILER"

# Check for SDL2
if ! pkg-config --exists sdl2; then
    print_warning "SDL2 not found via pkg-config. Trying to find manually..."
    if [ ! -d "/usr/include/SDL2" ] && [ ! -d "/usr/local/include/SDL2" ]; then
        print_error "SDL2 development libraries not found. Please install SDL2."
        exit 1
    fi
fi

# Check for SDL2_ttf
if ! pkg-config --exists SDL2_ttf; then
    print_warning "SDL2_ttf not found via pkg-config. Trying to find manually..."
    if [ ! -f "/usr/include/SDL2/SDL_ttf.h" ] && [ ! -f "/usr/local/include/SDL2/SDL_ttf.h" ]; then
        print_error "SDL2_ttf development libraries not found. Please install SDL2_ttf."
        exit 1
    fi
fi

# Check for SDL2_image
if ! pkg-config --exists SDL2_image; then
    print_warning "SDL2_image not found via pkg-config. Trying to find manually..."
    if [ ! -f "/usr/include/SDL2/SDL_image.h" ] && [ ! -f "/usr/local/include/SDL2/SDL_image.h" ]; then
        print_error "SDL2_image development libraries not found. Please install SDL2_image."
        exit 1
    fi
fi

# Check for libcurl
if ! pkg-config --exists libcurl; then
    print_warning "libcurl not found via pkg-config. Trying to find manually..."
    if [ ! -f "/usr/include/curl/curl.h" ] && [ ! -f "/usr/local/include/curl/curl.h" ]; then
        print_error "libcurl development libraries not found. Please install libcurl."
        exit 1
    fi
fi

# Check for OpenSSL
if ! pkg-config --exists openssl; then
    print_warning "OpenSSL not found via pkg-config. Trying to find manually..."
    if [ ! -f "/usr/include/openssl/ssl.h" ] && [ ! -f "/usr/local/include/openssl/ssl.h" ]; then
        print_error "OpenSSL development libraries not found. Please install OpenSSL."
        exit 1
    fi
fi

# Check for json-c
if ! pkg-config --exists json-c; then
    print_warning "json-c not found via pkg-config. Trying to find manually..."
    if [ ! -f "/usr/include/json-c/json.h" ] && [ ! -f "/usr/local/include/json-c/json.h" ]; then
        print_error "json-c development libraries not found. Please install json-c."
        exit 1
    fi
fi

print_success "All dependencies found!"

# Create build directory
print_status "Creating build directory..."
if [ -d "build" ]; then
    print_warning "Build directory already exists. Cleaning..."
    rm -rf build
fi
mkdir -p build
cd build

# Configure with CMake
print_status "Configuring with CMake..."
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_STANDARD=17 \
    -DCMAKE_CXX_FLAGS="-Wall -Wextra -O2" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

if [ $? -ne 0 ]; then
    print_error "CMake configuration failed!"
    exit 1
fi

print_success "CMake configuration completed!"

# Build the project
print_status "Building Zepra Core Browser..."
make -j$(nproc)

if [ $? -ne 0 ]; then
    print_error "Build failed!"
    exit 1
fi

print_success "Build completed successfully!"

# Create assets directory if it doesn't exist
print_status "Setting up assets..."
mkdir -p ../assets/fonts
mkdir -p ../assets/icons
mkdir -p ../assets/themes

# Copy default font if available
if [ -f "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf" ]; then
    cp /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf ../assets/fonts/
    print_success "Copied default font"
elif [ -f "/System/Library/Fonts/Arial.ttf" ]; then
    cp /System/Library/Fonts/Arial.ttf ../assets/fonts/
    print_success "Copied default font"
fi

# Create default configuration
print_status "Creating default configuration..."
cat > ../config/zepra_config.json << 'EOF'
{
    "browser": {
        "name": "Zepra Core Browser",
        "version": "1.0.0",
        "userAgent": "Zepra Core/1.0",
        "defaultSearchEngine": "ketivee",
        "homepage": "https://ketivee.com",
        "startupPage": "newtab"
    },
    "authentication": {
        "serverUrl": "https://auth.ketivee.com",
        "sessionTimeout": 604800,
        "autoRefresh": true,
        "secureMode": true,
        "allowedDomains": [
            "ketivee.com",
            "auth.ketivee.com",
            "ketiveeai.com",
            "docs.ketivee.com"
        ]
    },
    "ui": {
        "theme": "dark",
        "fontSize": 16,
        "showBookmarksBar": true,
        "showStatusBar": true,
        "enableAnimations": true
    },
    "security": {
        "enableSandbox": true,
        "enableContentFiltering": true,
        "blockAds": true,
        "blockTrackers": true,
        "enableHttpsOnly": true
    },
    "performance": {
        "maxTabs": 50,
        "memoryLimit": 2048,
        "enableHardwareAcceleration": true,
        "enableWebGL": true
    }
}
EOF

print_success "Default configuration created!"

# Create run script
print_status "Creating run script..."
cat > ../run-zepra.sh << 'EOF'
#!/bin/bash

# Zepra Core Browser Run Script

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Change to the script directory
cd "$SCRIPT_DIR"

# Check if the binary exists
if [ ! -f "build/bin/zepra" ]; then
    echo "Error: Zepra Core Browser binary not found. Please build the project first."
    exit 1
fi

# Set environment variables
export ZEPRA_CONFIG_PATH="$SCRIPT_DIR/config"
export ZEPRA_ASSETS_PATH="$SCRIPT_DIR/assets"
export ZEPRA_DATA_PATH="$SCRIPT_DIR/data"

# Create data directory if it doesn't exist
mkdir -p "$ZEPRA_DATA_PATH"

# Run the browser
echo "🦓 Starting Zepra Core Browser..."
./build/bin/zepra "$@"
EOF

chmod +x ../run-zepra.sh

print_success "Run script created!"

# Create installation script
print_status "Creating installation script..."
cat > ../install-zepra.sh << 'EOF'
#!/bin/bash

# Zepra Core Browser Installation Script

set -e

echo "🦓 Installing Zepra Core Browser..."

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Check if the binary exists
if [ ! -f "$SCRIPT_DIR/build/bin/zepra" ]; then
    echo "Error: Zepra Core Browser binary not found. Please build the project first."
    exit 1
fi

# Create installation directory
INSTALL_DIR="/usr/local/bin"
if [ "$EUID" -ne 0 ]; then
    INSTALL_DIR="$HOME/.local/bin"
    mkdir -p "$INSTALL_DIR"
fi

# Copy binary
echo "Installing binary to $INSTALL_DIR..."
cp "$SCRIPT_DIR/build/bin/zepra" "$INSTALL_DIR/"

# Create desktop entry
if command -v xdg-desktop-menu &> /dev/null; then
    echo "Creating desktop entry..."
    mkdir -p "$HOME/.local/share/applications"
    
    cat > "$HOME/.local/share/applications/zepra-core.desktop" << 'DESKTOP'
[Desktop Entry]
Name=Zepra Core Browser
Comment=Fast and secure web browser with integrated authentication
Exec=zepra %u
Terminal=false
Type=Application
Icon=zepra-core
Categories=Network;WebBrowser;
MimeType=text/html;text/xml;application/xhtml+xml;application/xml;application/vnd.mozilla.xul+xml;application/rss+xml;application/rdf+xml;image/gif;image/jpeg;image/png;image/webp;x-scheme-handler/http;x-scheme-handler/https;
StartupNotify=true
DESKTOP
fi

echo "Installation completed successfully!"
echo "You can now run 'zepra' from anywhere in your system."
EOF

chmod +x ../install-zepra.sh

print_success "Installation script created!"

# Print summary
echo ""
echo "========================================================"
print_success "Zepra Core Browser build completed successfully!"
echo ""
echo "📁 Build artifacts:"
echo "   - Binary: build/bin/zepra"
echo "   - Configuration: config/zepra_config.json"
echo "   - Assets: assets/"
echo ""
echo "🚀 To run the browser:"
echo "   ./run-zepra.sh"
echo ""
echo "📦 To install system-wide:"
echo "   sudo ./install-zepra.sh"
echo ""
echo "🔧 To rebuild:"
echo "   ./build-zepra-core.sh"
echo ""
echo "🦓 Zepra Core Browser - Where speed meets elegance!"
echo "========================================================" 