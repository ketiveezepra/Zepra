@echo off
REM Zepra Browser Development Workflow Runner (Windows)
REM This script runs the complete Python-based development workflow

setlocal enabledelayedexpansion

REM Set error handling
set "EXIT_ON_ERROR=1"

REM Colors for output (Windows 10+)
set "RED=[91m"
set "GREEN=[92m"
set "YELLOW=[93m"
set "BLUE=[94m"
set "NC=[0m"

REM Function to print colored output
:print_status
echo %BLUE%[INFO]%NC% %~1
goto :eof

:print_success
echo %GREEN%[SUCCESS]%NC% %~1
goto :eof

:print_warning
echo %YELLOW%[WARNING]%NC% %~1
goto :eof

:print_error
echo %RED%[ERROR]%NC% %~1
goto :eof

REM Function to check if command exists
:command_exists
where %1 >nul 2>&1
if %errorlevel% equ 0 (
    exit /b 0
) else (
    exit /b 1
)

REM Function to check Python version
:check_python_version
python --version >nul 2>&1
if %errorlevel% neq 0 (
    call :print_error "Python not found. Please install Python 3.8 or higher."
    exit /b 1
)

python -c "import sys; exit(0 if sys.version_info >= (3, 8) else 1)" >nul 2>&1
if %errorlevel% neq 0 (
    call :print_error "Python version too old. Please install Python 3.8 or higher."
    exit /b 1
)

for /f "tokens=2" %%i in ('python --version 2^>^&1') do set "PYTHON_VERSION=%%i"
call :print_success "Python %PYTHON_VERSION% found (>= 3.8 required)"
exit /b 0

REM Function to setup Python environment
:setup_python_env
call :print_status "Setting up Python environment..."

REM Check if virtual environment exists
if not exist "venv" (
    call :print_status "Creating virtual environment..."
    python -m venv venv
    if %errorlevel% neq 0 (
        call :print_error "Failed to create virtual environment"
        exit /b 1
    )
)

REM Activate virtual environment
call :print_status "Activating virtual environment..."
call venv\Scripts\activate.bat
if %errorlevel% neq 0 (
    call :print_error "Failed to activate virtual environment"
    exit /b 1
)

REM Upgrade pip
call :print_status "Upgrading pip..."
python -m pip install --upgrade pip
if %errorlevel% neq 0 (
    call :print_warning "Failed to upgrade pip, continuing..."
)

REM Install requirements
call :print_status "Installing Python dependencies..."
pip install -r requirements.txt
if %errorlevel% neq 0 (
    call :print_error "Failed to install requirements"
    exit /b 1
)

call :print_success "Python environment setup complete"
exit /b 0

REM Function to run validation
:run_validation
call :print_status "Running validation workflow..."
python main_orchestrator.py --workflow validation
if %errorlevel% equ 0 (
    call :print_success "Validation completed successfully"
    exit /b 0
) else (
    call :print_error "Validation failed"
    exit /b 1
)

REM Function to run configuration
:run_configuration
call :print_status "Running configuration workflow..."
python main_orchestrator.py --workflow config
if %errorlevel% equ 0 (
    call :print_success "Configuration completed successfully"
    exit /b 0
) else (
    call :print_error "Configuration failed"
    exit /b 1
)

REM Function to run build
:run_build
call :print_status "Running build workflow..."
python main_orchestrator.py --workflow build
if %errorlevel% equ 0 (
    call :print_success "Build completed successfully"
    exit /b 0
) else (
    call :print_error "Build failed"
    exit /b 1
)

REM Function to run tests
:run_tests
call :print_status "Running test workflow..."
python main_orchestrator.py --workflow test
if %errorlevel% equ 0 (
    call :print_success "Tests completed successfully"
    exit /b 0
) else (
    call :print_error "Tests failed"
    exit /b 1
)

REM Function to run analytics
:run_analytics
call :print_status "Running analytics workflow..."
python main_orchestrator.py --workflow analytics
if %errorlevel% equ 0 (
    call :print_success "Analytics completed successfully"
    exit /b 0
) else (
    call :print_error "Analytics failed"
    exit /b 1
)

REM Function to run full workflow
:run_full_workflow
call :print_status "Running full development workflow..."
python main_orchestrator.py --workflow full
if %errorlevel% equ 0 (
    call :print_success "Full workflow completed successfully"
    exit /b 0
) else (
    call :print_error "Full workflow failed"
    exit /b 1
)

REM Function to show help
:show_help
echo Zepra Browser Development Workflow Runner
echo.
echo Usage: %0 [OPTIONS] [WORKFLOW]
echo.
echo WORKFLOWS:
echo   validation    Run validation checks only
echo   config        Run configuration setup only
echo   build         Run build process only
echo   test          Run tests only
echo   analytics     Run analytics only
echo   full          Run complete workflow (default)
echo.
echo OPTIONS:
echo   --setup       Setup Python environment only
echo   --clean       Clean build artifacts
echo   --help        Show this help message
echo.
echo EXAMPLES:
echo   %0                    # Run full workflow
echo   %0 validation         # Run validation only
echo   %0 --setup            # Setup environment only
echo   %0 --clean            # Clean build artifacts
exit /b 0

REM Function to clean build artifacts
:clean_build
call :print_status "Cleaning build artifacts..."

REM Remove build directories
if exist "build" (
    rmdir /s /q build
    call :print_status "Removed build directory"
)

if exist "bin" (
    rmdir /s /q bin
    call :print_status "Removed bin directory"
)

if exist "test_output" (
    rmdir /s /q test_output
    call :print_status "Removed test_output directory"
)

if exist "analytics_data" (
    rmdir /s /q analytics_data
    call :print_status "Removed analytics_data directory"
)

REM Remove generated files
if exist "workflow_report.json" (
    del workflow_report.json
    call :print_status "Removed workflow_report.json"
)

call :print_success "Build artifacts cleaned"
exit /b 0

REM Main script logic
:main
REM Parse command line arguments
set "WORKFLOW=full"
set "SETUP_ONLY=false"
set "CLEAN_ONLY=false"

:parse_args
if "%~1"=="" goto :args_done
if "%~1"=="--setup" (
    set "SETUP_ONLY=true"
    shift
    goto :parse_args
)
if "%~1"=="--clean" (
    set "CLEAN_ONLY=true"
    shift
    goto :parse_args
)
if "%~1"=="--help" (
    call :show_help
    exit /b 0
)
if "%~1"=="-h" (
    call :show_help
    exit /b 0
)
if "%~1"=="validation" (
    set "WORKFLOW=validation"
    shift
    goto :parse_args
)
if "%~1"=="config" (
    set "WORKFLOW=config"
    shift
    goto :parse_args
)
if "%~1"=="build" (
    set "WORKFLOW=build"
    shift
    goto :parse_args
)
if "%~1"=="test" (
    set "WORKFLOW=test"
    shift
    goto :parse_args
)
if "%~1"=="analytics" (
    set "WORKFLOW=analytics"
    shift
    goto :parse_args
)
if "%~1"=="full" (
    set "WORKFLOW=full"
    shift
    goto :parse_args
)
call :print_error "Unknown option: %~1"
call :show_help
exit /b 1

:args_done
REM Print header
echo ==========================================
echo Zepra Browser Development Workflow Runner
echo ==========================================
echo.

REM Check if we're only cleaning
if "%CLEAN_ONLY%"=="true" (
    call :clean_build
    exit /b 0
)

REM Check Python version
call :print_status "Checking Python version..."
call :check_python_version
if %errorlevel% neq 0 (
    exit /b 1
)

REM Setup Python environment
call :setup_python_env
if %errorlevel% neq 0 (
    exit /b 1
)

REM Check if we're only setting up
if "%SETUP_ONLY%"=="true" (
    call :print_success "Setup completed successfully"
    exit /b 0
)

REM Change to script directory
cd /d "%~dp0"

REM Run selected workflow
if "%WORKFLOW%"=="validation" (
    call :run_validation
    set "WORKFLOW_RESULT=%errorlevel%"
) else if "%WORKFLOW%"=="config" (
    call :run_configuration
    set "WORKFLOW_RESULT=%errorlevel%"
) else if "%WORKFLOW%"=="build" (
    call :run_build
    set "WORKFLOW_RESULT=%errorlevel%"
) else if "%WORKFLOW%"=="test" (
    call :run_tests
    set "WORKFLOW_RESULT=%errorlevel%"
) else if "%WORKFLOW%"=="analytics" (
    call :run_analytics
    set "WORKFLOW_RESULT=%errorlevel%"
) else if "%WORKFLOW%"=="full" (
    call :run_full_workflow
    set "WORKFLOW_RESULT=%errorlevel%"
) else (
    call :print_error "Unknown workflow: %WORKFLOW%"
    call :show_help
    exit /b 1
)

REM Final status
if %WORKFLOW_RESULT% equ 0 (
    call :print_success "Workflow '%WORKFLOW%' completed successfully!"
    echo.
    echo Generated files:
    if exist "workflow_report.json" (
        echo   - workflow_report.json
    )
    if exist "test_output" (
        echo   - test_output\ (test reports)
    )
    if exist "analytics_data" (
        echo   - analytics_data\ (analytics data)
    )
    if exist "configs" (
        echo   - configs\ (configuration files)
    )
) else (
    call :print_error "Workflow '%WORKFLOW%' failed!"
    exit /b 1
)

exit /b 0

REM Run main function
call :main %* 