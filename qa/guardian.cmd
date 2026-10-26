@echo off
cd /d D:\ESP32\esp32-pc\qa
python -m pip install -q -r requirements.txt
python guardian.py --until-pass --port COM7 --interval-sec 0 --monitor-sec 120
