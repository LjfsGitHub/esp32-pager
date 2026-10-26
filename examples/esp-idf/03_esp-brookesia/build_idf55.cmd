@echo off
setlocal
set IDF_SKIP_CHECK_SUBMODULES=1
set IDF_GITHUB_ASSETS=dl.espressif.com/github_assets
call D:\ESP32\.tools\esp-idf-v5.5.5\export.bat
cd /d "%~dp0"
idf.py set-target esp32s3
idf.py build %*
