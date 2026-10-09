#!/bin/bash
# Zepra Browser Development Workflow Runner
# This script runs the complete Python-based development workflow

set -e  # Exit on any error

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

# Function to check if command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Function to check Python version
check_python_version() {
    if command_exists python3; then
        PYTHON_VERSION=$(python3 -c 'import sys; print(".".join(map(str, sys.version_info[:2])))')
        REQUIRED_VERSION="3.8"
        
        if python3 -c "import sys; exit(0 if sys.version_info >= (3, 8) else 1)"; then
            print_success "Python $PYTHON_VERSION found (>= $REQUIRED_VERSION required)"
            return 0
        else
            print_error "Python $PYTHON_VERSION found, but $REQUIRED_VERSION+ is required"
            return 1
        fi
    else
        print_error "Python 3 not found. Please install Python 3.8 or higher."
        return 1
    fi
}

# Function to setup Python environment
setup_python_env() {
    print_status "Setting up Python environment..."
    
    # Check if virtual environment exists
    if [ ! -d "venv" ]; then
        print_status "Creating virtual environment..."
        python3 -m venv venv
    fi
    
    # Activate virtual environment
    print_status "Activating virtual environment..."
    source venv/bin/activate
    
    # Upgrade pip
    print_status "Upgrading pip..."
    pip install --upgrade pip
    
    # Install requirements
    print_status "Installing Python dependencies..."
    pip install -r requirements.txt
    
    print_success "Python environment setup complete"
}

# Function to run validation
run_validation() {
    print_status "Running validation workflow..."
    python main_orchestrator.py --workflow validation
    if [ $? -eq 0 ]; then
        print_success "Validation completed successfully"
    else
        print_error "Validation failed"
        return 1
    fi
}

# Function to run configuration
run_configuration() {
    print_status "Running configuration workflow..."
    python main_orchestrator.py --workflow config
    if [ $? -eq 0 ]; then
        print_success "Configuration completed successfully"
    else
        print_error "Configuration failed"
        return 1
    fi
}

# Function to run build
run_build() {
    print_status "Running build workflow..."
    python main_orchestrator.py --workflow build
    if [ $? -eq 0 ]; then
        print_success "Build completed successfully"
    else
        print_error "Build failed"
        return 1
    fi
}

# Function to run tests
run_tests() {
    print_status "Running test workflow..."
    python main_orchestrator.py --workflow test
    if [ $? -eq 0 ]; then
        print_success "Tests completed successfully"
    else
        print_error "Tests failed"
        return 1
    fi
}

# Function to run analytics
run_analytics() {
    print_status "Running analytics workflow..."
    python main_orchestrator.py --workflow analytics
    if [ $? -eq 0 ]; then
        print_success "Analytics completed successfully"
    else
        print_error "Analytics failed"
        return 1
    fi
}

# Function to run full workflow
run_full_workflow() {
    print_status "Running full development workflow..."
    python main_orchestrator.py --workflow full
    if [ $? -eq 0 ]; then
        print_success "Full workflow completed successfully"
    else
        print_error "Full workflow failed"
        return 1
    fi
}

# Function to show help
show_help() {
    echo "Zepra Browser Development Workflow Runner"
    echo ""
    echo "Usage: $0 [OPTIONS] [WORKFLOW]"
    echo ""
    echo "WORKFLOWS:"
    echo "  validation    Run validation checks only"
    echo "  config        Run configuration setup only"
    echo "  build         Run build process only"
    echo "  test          Run tests only"
    echo "  analytics     Run analytics only"
    echo "  full          Run complete workflow (default)"
    echo ""
    echo "OPTIONS:"
    echo "  --setup       Setup Python environment only"
    echo "  --clean       Clean build artifacts"
    echo "  --help        Show this help message"
    echo ""
    echo "EXAMPLES:"
    echo "  $0                    # Run full workflow"
    echo "  $0 validation         # Run validation only"
    echo "  $0 --setup            # Setup environment only"
    echo "  $0 --clean            # Clean build artifacts"
}

# Function to clean build artifacts
clean_build() {
    print_status "Cleaning build artifacts..."
    
    # Remove build directories
    if [ -d "build" ]; then
        rm -rf build
        print_status "Removed build directory"
    fi
    
    if [ -d "bin" ]; then
        rm -rf bin
        print_status "Removed bin directory"
    fi
    
    if [ -d "test_output" ]; then
        rm -rf test_output
        print_status "Removed test_output directory"
    fi
    
    if [ -d "analytics_data" ]; then
        rm -rf analytics_data
        print_status "Removed analytics_data directory"
    fi
    
    # Remove generated files
    if [ -f "workflow_report.json" ]; then
        rm workflow_report.json
        print_status "Removed workflow_report.json"
    fi
    
    print_success "Build artifacts cleaned"
}

# Main script logic
main() {
    # Parse command line arguments
    WORKFLOW="full"
    SETUP_ONLY=false
    CLEAN_ONLY=false
    
    while [[ $# -gt 0 ]]; do
        case $1 in
            --setup)
                SETUP_ONLY=true
                shift
                ;;
            --clean)
                CLEAN_ONLY=true
                shift
                ;;
            --help|-h)
                show_help
                exit 0
                ;;
            validation|config|build|test|analytics|full)
                WORKFLOW="$1"
                shift
                ;;
            *)
                print_error "Unknown option: $1"
                show_help
                exit 1
                ;;
        esac
    done
    
    # Print header
    echo "=========================================="
    echo "Zepra Browser Development Workflow Runner"
    echo "=========================================="
    echo ""
    
    # Check if we're only cleaning
    if [ "$CLEAN_ONLY" = true ]; then
        clean_build
        exit 0
    fi
    
    # Check Python version
    print_status "Checking Python version..."
    if ! check_python_version; then
        exit 1
    fi
    
    # Setup Python environment
    setup_python_env
    
    # Check if we're only setting up
    if [ "$SETUP_ONLY" = true ]; then
        print_success "Setup completed successfully"
        exit 0
    fi
    
    # Change to script directory
    cd "$(dirname "$0")"
    
    # Run selected workflow
    case $WORKFLOW in
        validation)
            run_validation
            ;;
        config)
            run_configuration
            ;;
        build)
            run_build
            ;;
        test)
            run_tests
            ;;
        analytics)
            run_analytics
            ;;
        full)
            run_full_workflow
            ;;
        *)
            print_error "Unknown workflow: $WORKFLOW"
            show_help
            exit 1
            ;;
    esac
    
    # Final status
    if [ $? -eq 0 ]; then
        print_success "Workflow '$WORKFLOW' completed successfully!"
        echo ""
        echo "Generated files:"
        if [ -f "workflow_report.json" ]; then
            echo "  - workflow_report.json"
        fi
        if [ -d "test_output" ]; then
            echo "  - test_output/ (test reports)"
        fi
        if [ -d "analytics_data" ]; then
            echo "  - analytics_data/ (analytics data)"
        fi
        if [ -d "configs" ]; then
            echo "  - configs/ (configuration files)"
        fi
    else
        print_error "Workflow '$WORKFLOW' failed!"
        exit 1
    fi
}

# Run main function
main "$@" 