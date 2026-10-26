@echo off
cd /d D:\ESP32\esp32-pc\qa
python -m pip install -q -r requirements.txt
python run.py %*
