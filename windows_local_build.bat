@echo off
setlocal enabledelayedexpansion
:: This script replicates the GitHub Actions compile_prerelease.yml workflow

echo.
echo     ####  #   #  ####   ####  #####    ####  ####  ####
echo    #      #   # #    # #        #     #     #     #    #
echo    #  ### ##### #    #  ####    #     ####   ####  ####
echo    #   #  #   # #    #      #   #     #         # #
echo     ####  #   #  ####   ####    #     ##### ####  #
echo.
echo        +======================================+
echo        ^|        Windows Build Script         ^|
echo        +======================================+
echo.

:: ESP-IDF Path Configuration
echo.
if defined IDF_PATH (
    echo Found existing ESP-IDF environment: %IDF_PATH%
    echo Do you want to use this path? [Y/n]
    set /p "use_existing="
    if /i "%use_existing%"=="n" goto auto_search_esp_idf
    if /i "%use_existing%"=="no" goto auto_search_esp_idf
    goto validate_esp_idf
)

:auto_search_esp_idf
echo Searching for ESP-IDF installation...
echo.

:: Common ESP-IDF installation paths
set "search_paths[0]=C:\esp\esp-idf"
set "search_paths[1]=C:\Users\%USERNAME%\esp\esp-idf"
set "search_paths[2]=C:\espressif\esp-idf"
set "search_paths[3]=C:\Users\%USERNAME%\espressif\esp-idf"
set "search_paths[4]=C:\esp-idf"
set "search_paths[5]=D:\esp\esp-idf"
set "search_paths[6]=D:\espressif\esp-idf"
set "search_paths[7]=C:\Program Files\esp-idf"
set "search_paths[8]=C:\Program Files (x86)\esp-idf"
set "search_paths[9]=C:\tools\esp-idf"
set "search_paths[10]=S:\Espressif\frameworks\esp-idf-v5.5"

:: Search for ESP-IDF in common locations
set "found_path="
for /L %%i in (0,1,10) do (
    call set "current_path=%%search_paths[%%i]%%"
    if exist "!current_path!\export.bat" (
        echo Found ESP-IDF at: !current_path!
        set "found_path=!current_path!"
        goto found_esp_idf
    )
)

:: If not found in common locations, search in version-specific folders
echo Searching for version-specific installations...
for %%d in (C: D:) do (
    if exist "%%d\" (
        for /d %%p in ("%%d\esp\esp-idf-*" "%%d\espressif\esp-idf-*" "%%d\Users\%USERNAME%\esp\esp-idf-*") do (
            if exist "%%p\export.bat" (
                echo Found ESP-IDF at: %%p
                set "found_path=%%p"
                goto found_esp_idf
            )
        )
    )
)

:: If still not found, ask user for manual input
echo ESP-IDF not found in common locations.
echo.
echo Please enter the path to your ESP-IDF installation:
echo Example: C:\esp\esp-idf
echo Example: C:\Users\%USERNAME%\esp\esp-idf
echo.
set /p "custom_idf_path=ESP-IDF Path (or press Enter to exit): "

if "%custom_idf_path%"=="" (
    echo Exiting build script.
    pause
    exit /b 1
)

:: Remove quotes if present
set "custom_idf_path=%custom_idf_path:"=%"
set "found_path=%custom_idf_path%"

:found_esp_idf
:: Set the IDF_PATH
set "IDF_PATH=%found_path%"
echo.
echo Using ESP-IDF path: %IDF_PATH%

:validate_esp_idf
:: Validate the ESP-IDF installation
if not exist "%IDF_PATH%\export.bat" (
    echo ERROR: Invalid ESP-IDF path. export.bat not found in %IDF_PATH%
    echo Please ensure you have ESP-IDF v5.4.1 installed.
    echo Download from: https://github.com/espressif/esp-idf/releases/tag/v5.4.1
    pause
    exit /b 1
)

:: Check for tools directory (indicates proper installation)
if not exist "%IDF_PATH%\tools" (
    echo WARNING: ESP-IDF tools directory not found. Installation may be incomplete.
)

echo ESP-IDF validation successful!

:init_esp_idf

:: Initialize ESP-IDF environment
echo.
echo Initializing ESP-IDF environment...
call "%IDF_PATH%\export.bat"
if errorlevel 1 (
    echo ERROR: Failed to initialize ESP-IDF environment
    pause
    exit /b 1
)

echo Using ESP-IDF from: %IDF_PATH%

:: Ensure we're in the correct project directory
echo.
echo Checking project directory...
if not exist "configs" (
    echo ERROR: configs directory not found. Please run this script from the Ghost ESP project root directory.
    echo Current directory: %CD%
    pause
    exit /b 1
)

if not exist "CMakeLists.txt" (
    echo ERROR: CMakeLists.txt not found. Please run this script from the Ghost ESP project root directory.
    echo Current directory: %CD%
    pause
    exit /b 1
)

echo Project directory verified: %CD%

:: Build configuration matrix - same as GitHub Actions
set "targets[0].name=esp32-generic"
set "targets[0].idf_target=esp32"
set "targets[0].sdkconfig_file=configs/sdkconfig.default.esp32"
set "targets[0].zip_name=esp32-generic.zip"

set "targets[1].name=esp32s2-generic"
set "targets[1].idf_target=esp32s2"
set "targets[1].sdkconfig_file=configs/sdkconfig.default.esp32s2"
set "targets[1].zip_name=esp32s2-generic.zip"

set "targets[2].name=esp32s3-generic"
set "targets[2].idf_target=esp32s3"
set "targets[2].sdkconfig_file=configs/sdkconfig.default.esp32s3"
set "targets[2].zip_name=esp32s3-generic.zip"

set "targets[3].name=esp32c3-generic"
set "targets[3].idf_target=esp32c3"
set "targets[3].sdkconfig_file=configs/sdkconfig.default.esp32c3"
set "targets[3].zip_name=esp32c3-generic.zip"

set "targets[4].name=esp32c5-generic"
set "targets[4].idf_target=esp32c5"
set "targets[4].sdkconfig_file=configs/sdkconfig.default.esp32c5"
set "targets[4].zip_name=esp32c5-generic-v01.zip"

set "targets[5].name=esp32c6-generic"
set "targets[5].idf_target=esp32c6"
set "targets[5].sdkconfig_file=configs/sdkconfig.default.esp32c6"
set "targets[5].zip_name=esp32c6-generic.zip"

set "targets[6].name=Awok V5"
set "targets[6].idf_target=esp32s2"
set "targets[6].sdkconfig_file=configs/sdkconfig.default.esp32s2"
set "targets[6].zip_name=esp32v5_awok.zip"

set "targets[7].name=ghostboard"
set "targets[7].idf_target=esp32c6"
set "targets[7].sdkconfig_file=configs/sdkconfig.ghostboard"
set "targets[7].zip_name=ghostboard.zip"

set "targets[8].name=MarauderV4_FlipperHub"
set "targets[8].idf_target=esp32"
set "targets[8].sdkconfig_file=configs/sdkconfig.marauderv4"
set "targets[8].zip_name=MarauderV4_FlipperHub.zip"

set "targets[9].name=MarauderV6&AwokDual"
set "targets[9].idf_target=esp32"
set "targets[9].sdkconfig_file=configs/sdkconfig.marauderv6"
set "targets[9].zip_name=MarauderV6_AwokDual.zip"

set "targets[10].name=AwokMini"
set "targets[10].idf_target=esp32s2"
set "targets[10].sdkconfig_file=configs/sdkconfig.awokmini"
set "targets[10].zip_name=AwokMini.zip"

set "targets[11].name=ESP32-S3-Cardputer"
set "targets[11].idf_target=esp32s3"
set "targets[11].sdkconfig_file=configs/sdkconfig.cardputer"
set "targets[11].zip_name=ESP32-S3-Cardputer.zip"

set "targets[12].name=CYD2USB"
set "targets[12].idf_target=esp32"
set "targets[12].sdkconfig_file=configs/sdkconfig.CYD2USB"
set "targets[12].zip_name=CYD2USB.zip"

set "targets[13].name=CYDMicroUSB"
set "targets[13].idf_target=esp32"
set "targets[13].sdkconfig_file=configs/sdkconfig.CYDMicroUSB"
set "targets[13].zip_name=CYDMicroUSB.zip"

set "targets[14].name=CYDDualUSB"
set "targets[14].idf_target=esp32"
set "targets[14].sdkconfig_file=configs/sdkconfig.CYDDualUSB"
set "targets[14].zip_name=CYDDualUSB.zip"

set "targets[15].name=CYD2USB2.4_Inch"
set "targets[15].idf_target=esp32"
set "targets[15].sdkconfig_file=configs/sdkconfig.CYD2USB2.4Inch"
set "targets[15].zip_name=CYD2USB2.4Inch.zip"

set "targets[16].name=CYD2USB2.4_Inch_C"
set "targets[16].idf_target=esp32"
set "targets[16].sdkconfig_file=configs/sdkconfig.CYD2USB2.4Inch_C_Varient"
set "targets[16].zip_name=CYD2USB2.4Inch_C.zip"

set "targets[17].name=CYD2432S028R"
set "targets[17].idf_target=esp32"
set "targets[17].sdkconfig_file=configs/sdkconfig.CYD2432S028R"
set "targets[17].zip_name=CYD2432S028R.zip"

set "targets[18].name=Waveshare_LCD"
set "targets[18].idf_target=esp32s3"
set "targets[18].sdkconfig_file=configs/sdkconfig.waveshare7inch"
set "targets[18].zip_name=Waveshare_LCD.zip"

set "targets[19].name=Crowtech_LCD"
set "targets[19].idf_target=esp32s3"
set "targets[19].sdkconfig_file=configs/sdkconfig.crowtech7inch"
set "targets[19].zip_name=Crowtech_LCD.zip"

set "targets[20].name=Sunton_LCD"
set "targets[20].idf_target=esp32s3"
set "targets[20].sdkconfig_file=configs/sdkconfig.sunton7inch"
set "targets[20].zip_name=Sunton_LCD.zip"

set "targets[21].name=JC3248W535EN_LCD"
set "targets[21].idf_target=esp32s3"
set "targets[21].sdkconfig_file=configs/sdkconfig.JC3248W535EN"
set "targets[21].zip_name=JC3248W535EN_LCD.zip"

set "targets[22].name=Flipper_JCMK_GPS"
set "targets[22].idf_target=esp32s2"
set "targets[22].sdkconfig_file=configs/sdkconfig.flipper.jcmk_gps"
set "targets[22].zip_name=Flipper_JCMK_GPS.zip"

set "targets[23].name=T-Deck"
set "targets[23].idf_target=esp32s3"
set "targets[23].sdkconfig_file=configs/sdkconfig.tdeck"
set "targets[23].zip_name=LilyGo-T-Deck.zip"

set "targets[24].name=TEmbedC1101"
set "targets[24].idf_target=esp32s3"
set "targets[24].sdkconfig_file=configs/sdkconfig.TEmbedC1101"
set "targets[24].zip_name=LilyGo-TEmbedC1101.zip"

set "targets[25].name=S3TWatch"
set "targets[25].idf_target=esp32s3"
set "targets[25].sdkconfig_file=configs/sdkconfig.S3TWatch"
set "targets[25].zip_name=LilyGo-S3TWatch-2020.zip"

:: Create output directory for builds
if not exist "local_builds" mkdir local_builds

:: Ask user which targets to build
echo.
echo Available build targets:
for /l %%i in (0,1,25) do (
    call echo %%i: %%targets[%%i].name%%
)
echo.
echo Enter target numbers to build (space-separated), or 'all' for all targets:
set /p "user_targets="

if /i "%user_targets%"=="all" (
    set "build_targets=0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25"
) else (
    set "build_targets=%user_targets%"
)

:: Build each selected target
for %%t in (%build_targets%) do (
    call :build_target %%t
)

echo.
echo ========================================
echo Build process completed!
echo Check the local_builds directory for output files.
echo ========================================
pause
exit /b 0

:build_target
set target_idx=%1
call set target_name=%%targets[%target_idx%].name%%
call set idf_target=%%targets[%target_idx%].idf_target%%
call set sdkconfig_file=%%targets[%target_idx%].sdkconfig_file%%
call set zip_name=%%targets[%target_idx%].zip_name%%

echo.
echo ========================================
echo Building: %target_name%
echo Target: %idf_target%
echo Config: %sdkconfig_file%
echo ========================================

:: Check if config file exists
echo Checking for config file: %sdkconfig_file%
if not exist "%sdkconfig_file%" (
    echo ERROR: Config file %sdkconfig_file% not found!
    echo Current directory: %CD%
    echo Directory contents:
    dir /b configs
    goto :eof
)
echo Config file found: %sdkconfig_file%

:: Apply custom SDK config
echo Copying %sdkconfig_file% to sdkconfig.defaults...

:: Remove existing sdkconfig.defaults if it exists
if exist sdkconfig.defaults del sdkconfig.defaults

:: Use PowerShell to copy the file (most reliable approach)
powershell -command "Copy-Item '%sdkconfig_file%' 'sdkconfig.defaults' -Force"
if errorlevel 1 (
    echo ERROR: Failed to copy config file using PowerShell
    echo Source: %sdkconfig_file%
    echo Destination: sdkconfig.defaults
    echo Current directory: %CD%
    goto :eof
)

:: Verify the file was created and has content
if not exist sdkconfig.defaults (
    echo ERROR: sdkconfig.defaults was not created
    goto :eof
)

:: Check if the file has content
for %%A in (sdkconfig.defaults) do if %%~zA==0 (
    echo ERROR: sdkconfig.defaults is empty
    goto :eof
)

echo Config file copied successfully.

:: Set target
echo Setting IDF target to %idf_target%...
idf.py set-target %idf_target%
if errorlevel 1 (
    echo ERROR: Failed to set target %idf_target%
    goto :eof
)

:: Clean and build
echo Cleaning previous build...
idf.py clean
if errorlevel 1 (
    echo ERROR: Failed to clean
    goto :eof
)

echo Building project...
idf.py build
if errorlevel 1 (
    echo ERROR: Build failed for %target_name%
    goto :eof
)

:: Verify bootloader exists
if not exist "build\bootloader\bootloader.bin" (
    echo ERROR: Bootloader not found. Build may have failed.
    goto :eof
)

:: Package artifacts
echo Packaging artifacts...
set artifact_dir=local_builds\%target_name%
if exist "%artifact_dir%" rmdir /s /q "%artifact_dir%"
mkdir "%artifact_dir%"

:: Copy required files
copy "build\partition_table\partition-table.bin" "%artifact_dir%\partitions.bin" >nul
if errorlevel 1 (
    echo ERROR: Failed to copy partition table
    goto :eof
)

copy "build\bootloader\bootloader.bin" "%artifact_dir%\bootloader.bin" >nul
if errorlevel 1 (
    echo ERROR: Failed to copy bootloader
    goto :eof
)

:: Find and copy the main firmware binary
for %%f in (build\*.bin) do (
    if not "%%~nf"=="bootloader" (
        if not "%%~nf"=="partition-table" (
            copy "%%f" "%artifact_dir%\firmware.bin" >nul
            goto :found_firmware
        )
    )
)
echo ERROR: Failed to find firmware binary
goto :eof

:found_firmware
echo Contents of %artifact_dir%:
dir "%artifact_dir%"

:: Create ZIP file using PowerShell (built into Windows)
echo Creating ZIP file: local_builds\%zip_name%
powershell -command "Compress-Archive -Path '%artifact_dir%\*' -DestinationPath 'local_builds\%zip_name%' -Force"
if errorlevel 1 (
    echo ERROR: Failed to create ZIP file
    goto :eof
)

echo Successfully created: local_builds\%zip_name%
goto :eof
