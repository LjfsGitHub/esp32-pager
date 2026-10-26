@echo off
set IDF_SKIP_CHECK_SUBMODULES=1
set IDF_GITHUB_ASSETS=dl.espressif.com/github_assets
call D:\ESP32\.tools\esp-idf-v5.5.5\export.bat
cd /d D:\pager_firmware_2_16\examples\esp-idf\03_esp-brookesia
REM CONFIG_PAGER_QA is enabled via qa/run.py patching sdkconfig, or sdkconfig.defaults.qa
idf.py %*
