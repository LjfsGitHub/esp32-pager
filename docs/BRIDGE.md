# Pager Bridge（PC）

## 启动

```powershell
cd bridge
pip install -e .
python -m pager_bridge.serve
```

- 管理 UI：`http://127.0.0.1:8765/`
- 设备 WSS：LAN `8766`（TLS，证书目录 `bridge/certs/` 或环境变量 `PAGER_CERT_DIR`）

仅 UI 开发（不监听设备端口）：

```powershell
python -m uvicorn pager_bridge.bridge:app --host 127.0.0.1 --port 8765
```

## Agent 配置

编辑 `bridge/agents.json`。各 Profile 的 `command` / `args` 支持 Windows 环境变量展开（`%LOCALAPPDATA%`、`%USERPROFILE%`、`%ProgramFiles%`）。

本仓库**不附带** Hermes、Cursor、Codex、Claude、WorkBuddy 本体；请在本机安装后调整路径。WorkBuddy 需要 `pywin32`（已列入 `requirements.txt`）。

## 离线语音转写（可选）

```powershell
pip install -r bridge/requirements-stt.txt
```

模型目录默认 `bridge/models/faster-whisper-medium`（需自行下载或运行 `scripts/dist/install.ps1` 中的 STT 步骤）。Git 仓库不包含该模型。

## 环境变量（常用）

| 变量 | 含义 |
|------|------|
| `PAGER_PORT` | UI 端口，默认 8765 |
| `PAGER_DEVICE_PORT` | 设备 WSS 端口，默认 8766 |
| `PAGER_CERT_DIR` | TLS 证书目录 |
| `PAGER_DEVICES_FILE` | 已配对设备 JSON |
| `PAGER_FFMPEG` | ffmpeg 可执行文件路径 |
| `PAGER_INSECURE_DEVICE` | 设为 `1` 时设备端口不用 TLS（仅调试） |

## 配对与安全

- 配对码 6 位、短时有效；token 明文仅下发设备一次，服务端存 hash
- 审计日志默认 `bridge/data/audit.log`（本地运行时生成，**勿提交 Git**）
