# Pager Bridge 新电脑部署问题与处理记录

记录日期：2026-10-26
目标电脑：Windows 11，用户 `10023`，安装目录 `C:\Users\10023\Documents\pager-app`，运行 Python 3.14（`C:\Users\10023\AppData\Local\Programs\Python\Python314\python.exe`）。

这份文档写给下次打包的人（或 AI）：说明原始安装包为什么在新电脑上没法直接用、这次怎么修的、下次打包前该改什么。

---

## 一、总结

根本原因只有两类：

1. **路径写死成打包机的路径。** `agents.json`、`speech.py` 里多处写的是 `C:\Users\Administrator\...`、`C:\Program Files\WorkBuddy\...`、`H:\Projects\...`，新电脑的用户名和安装盘都不一样，这些路径全部不存在。
2. **运行依赖没打进包，也没写进 `requirements.txt`。** `pywin32`、`psutil` 没装；Codex、Claude 的 ACP 适配器和 Cursor 的命令行在新电脑上根本不存在（`build-package.ps1` 注释里写明了不打包智能体后端，但安装脚本也没有任何检查或提示）。

另外修了桥接本身的两个 bug（登录判断失效、连接时序），它们在打包机上碰巧不触发，换电脑后才暴露。

---

## 二、逐项问题与处理

### 1. WorkBuddy：不弹登录界面

**现象：** 手表/网页显示“连接失败：WorkBuddy 凭据引导需要 pywin32（win32pipe）”，不弹登录。

**原因：**
- `acp_client.py` 启动 WorkBuddy 时要先建 Windows 命名管道传凭据密钥，需要 `pywin32`。新电脑的 Python 里没装，启动在这一步就抛错，根本走不到登录。
- `agents.json` 里 WorkBuddy 的三条路径都是打包机的：
  - `C:\Users\Administrator\.workbuddy\binaries\node\versions\22.22.2-6\node.exe`
  - `C:\Program Files\WorkBuddy\resources\app.asar.unpacked\cli\bin\codebuddy`
  - `WORKBUDDY_CONFIG_DIR` / `CODEBUDDY_CONFIG_DIR` = `C:\Users\Administrator\.workbuddy`

**处理：**
- `pip install pywin32`，并把 `pywin32>=308` 加进 `requirements.txt`。
- 路径改成本机实际位置（WorkBuddy 装在 D 盘）：
  - `C:\Users\10023\.workbuddy\binaries\node\versions\22.22.2-6\node.exe`
  - `D:\Program Files\WorkBuddy\resources\app.asar.unpacked\cli\bin\codebuddy`
  - 配置目录 `C:\Users\10023\.workbuddy`
- 本机 WorkBuddy 的实际安装位置是通过开始菜单快捷方式 `C:\ProgramData\Microsoft\Windows\Start Menu\Programs\WorkBuddy.lnk` 的目标找到的。

### 2. WorkBuddy：登录完成后又显示“连接被拒绝”

**现象：** 浏览器里微信登录成功了，桥接状态却变成“WorkBuddy ACP connect 失败：WinError 10061 由于目标计算机积极拒绝”。

**原因：** WorkBuddy 命令行先在标准输出打印 `http://127.0.0.1:端口`，过一会儿才真正开始监听。`acp_client.py` 的 `_http_connect` 看到地址后立刻连一次，失败就放弃。

**处理：** `_http_connect` 遇到“连接被拒绝”时每 0.3 秒重试，最多 8 秒。

### 3. 所有智能体：登录失效后不会自动补登录

**现象：** 截图显示“错误 · 指令被拒绝”，回复是“WorkBuddy 需要先登录……”，但不弹登录窗口。

**原因：** `bridge.py` 的 `prompt_refusal_text` 先把智能体返回的英文 `Authentication required` 改写成中文提示，`_run_prompt` 再用 `auth_required_text` 判断要不要打开登录。可这个函数只认英文，于是永远判定“不需要登录”，补登录的分支是死代码。

**处理：**
- `auth_required_text` 增加识别“需要先登录”。
- Claude 把“没登录”报成发指令时的异常，而不是拒绝结果。`_run_prompt` 现在会把这种异常也当成“需要登录”，打开登录后重发同一条指令。
- 提示文字原来写死“WorkBuddy 需要先登录。桌面版开着……”，改成按当前智能体名字显示：“{名字} 需要先登录，所以没有回复。”

验证：发测试指令后状态依次为 已发送，正在打开登录窗口，已登录继续等待，正在回复，本轮完成。

### 4. 手表左滑“电脑资源”页没数据

**原因：** 采样线程依赖 `psutil`，没装时只打一行 `host sampler disabled: psutil unavailable` 就退出，页面一直是空的。

**处理：** `pip install psutil`，并把 `psutil>=5.9` 加进 `requirements.txt`。显卡数据靠 `nvidia-smi`，本机有 NVIDIA 显卡，正常。

验证：`/api/state` 的 `host` 已有 CPU、内存、显卡、显存、温度、网速数据。

### 5. Hermes

**原因：** 配置写的 `%LOCALAPPDATA%\hermes\bin\hermes-acp.exe` 不存在。本机 Hermes 新版装在虚拟环境里。

**处理：** 改成 `%LOCALAPPDATA%\hermes\hermes-agent\venv\Scripts\hermes-acp.exe`。验证能回复“收到”。

### 6. Cursor

**原因：** 配置用 `%LOCALAPPDATA%\pager-agents\cursor\dist-package\node.exe` + `index.js`，新电脑上整个 `pager-agents` 目录都不存在。

**处理：** Cursor 编辑器自带同一个命令行，在
`%APPDATA%\Cursor\User\globalStorage\anysphere.cursor-agent-worker\agent-cli\.local\share\cursor-agent\versions\<最新版本>\`。
把最新版本目录（本次是 `2026.10.22-27dcf5f`）整个复制到 `%LOCALAPPDATA%\pager-agents\cursor\dist-package\`，`agents.json` 不用改。

没有直接指向 Cursor 的版本目录，是因为 Cursor 升级会清理旧版本，路径会失效。代价是这份副本不会跟着自动更新，Cursor 大版本升级后可能要重新复制一次。

验证：`node.exe index.js status` 显示已登录，桥接能建立会话。

### 7. Codex

**原因一：** ACP 适配器 `@agentclientprotocol/codex-acp` 没装（`MODULE_NOT_FOUND`）。
**原因二：** 装上后连 `wss://chatgpt.com` 被重置（os error 10054）。本机靠 Clash 系统代理 `127.0.0.1:7890` 上网，Codex 不读 Windows 系统代理，只认 `HTTPS_PROXY` 环境变量。

**处理：**
- `npm install --prefix %LOCALAPPDATA%\pager-agents @agentclientprotocol/codex-acp @agentclientprotocol/claude-agent-acp`
  官方源也被重置，最后用国内镜像 `--registry=https://registry.npmmirror.com` 装成功。
- `agents.json` 的 codex 配置加上 `env`：`HTTP_PROXY` / `HTTPS_PROXY` = `http://127.0.0.1:7890`，`NO_PROXY` = `localhost,127.0.0.1`。

验证：能回复“收到。”，一轮大约 1 分钟（模型较慢，加上走代理）。

### 8. Claude

**原因：** ACP 适配器没装（同上）；本机 Claude Code 从没登录过（`claude auth status` 显示 `loggedIn: false`）；配置里也没有登录命令，桥接没法帮用户打开登录。

**处理：**
- 适配器随上一步一起装好。
- 加同样的代理环境变量。
- 加登录命令：`loginCommand` = `%APPDATA%\npm\claude.cmd`，`loginArgs` = `["auth", "login"]`。

**未完成：** 账号登录要用户本人完成。在下拉框选 Claude 后发一句话会打开登录页，也可以在终端运行一次 `claude auth login`。登录后自动重发那一步没有实测。

---

## 三、未处理的问题

| 问题 | 原因 | 影响 | 建议 |
|---|---|---|---|
| 在线语音合成失败（日志 `neural tts failed`） | `speech.py` 第 136 行 `_FFMPEG = Path(r"C:\Users\Administrator\ffmpeg\current\bin\ffmpeg.exe")` 写死，本机也没装 ffmpeg | 自动退回 Windows 自带语音，手表照样能播报，只是音色不是 Edge 神经语音 | 安装时用 `winget install Gyan.FFmpeg` 之类装 ffmpeg，代码改成先找 PATH 里的 `ffmpeg`，找不到再用默认路径 |
| `speech.py` 第 335 行 `_STT_PY` 默认值是 `H:\Projects\local-stt\...` | 打包机路径 | 安装脚本会用正则改写成新的虚拟环境路径，本机已被改写，目前正常 | 默认值改成相对安装目录的 `stt-venv`，不要靠正则改写 |
| 代理端口写死 7890 | 本机 Clash 的端口 | 别的电脑代理端口不同、或不用代理时，Codex/Claude 会连不上或绕路 | 见下一节第 4 条 |

---

## 四、下次打包需要做的事（给打包 AI 的清单）

1. **`agents.json` 不要写任何绝对用户路径。**
   - 只用环境变量：`%LOCALAPPDATA%`、`%APPDATA%`、`%USERPROFILE%`。
   - WorkBuddy 的 `node.exe` 改成 `%USERPROFILE%\.workbuddy\binaries\node\versions\...`；配置目录改成 `%USERPROFILE%\.workbuddy`。
   - WorkBuddy 安装目录不固定（本机在 D 盘），安装脚本应从开始菜单快捷方式 `WorkBuddy.lnk` 或注册表找到实际位置，再写进 `agents.json`。
   - `node` 版本目录（`22.22.2-6`）也会随 WorkBuddy 升级变化，最好安装时扫描 `binaries\node\versions\` 取最新。

2. **`requirements.txt` 必须包含 `pywin32` 和 `psutil`**（这次已加上）。`install.ps1` 走 `pip install -r requirements.txt`，会自动装上。

3. **安装脚本要检查智能体后端，缺了就装或明确提示**，不能装完后静默失败：
   - Hermes：检查 `%LOCALAPPDATA%\hermes\hermes-agent\venv\Scripts\hermes-acp.exe`，旧位置 `%LOCALAPPDATA%\hermes\bin\hermes-acp.exe` 作为备选。
   - Codex / Claude 适配器：`npm install --prefix %LOCALAPPDATA%\pager-agents @agentclientprotocol/codex-acp @agentclientprotocol/claude-agent-acp`，官方源失败时自动换 `https://registry.npmmirror.com`。
   - Cursor：从 `%APPDATA%\Cursor\User\globalStorage\anysphere.cursor-agent-worker\agent-cli\.local\share\cursor-agent\versions\` 取最新版本目录（排除 `.tmp-*`），复制到 `%LOCALAPPDATA%\pager-agents\cursor\dist-package\`。
   - 找不到的智能体在 `agents.json` 里设 `"enabled": false`，避免下拉框里出现选了就报错的选项。

4. **代理不要写死。** 两种做法任选：
   - 安装时读注册表 `HKCU\Software\Microsoft\Windows\CurrentVersion\Internet Settings` 的 `ProxyEnable` / `ProxyServer`，有代理才写进 Codex/Claude 的 `env`；
   - 或者桥接启动子进程时，如果没有 `HTTPS_PROXY` 而系统代理开着，自动补上（只给走海外服务的智能体）。

5. **ffmpeg**：安装脚本装上 ffmpeg，`speech.py` 改为优先用 PATH 里的。

6. **打包前在一台干净电脑（或另一个 Windows 用户）上跑一遍验收**，至少确认：
   - 网页 `http://127.0.0.1:8765/` 状态不是“离线”；
   - 下拉框里每个智能体都能切过去并回复一句话（未登录的会打开登录页）；
   - 手表左滑“电脑资源”有数字；
   - 日志里没有 `psutil unavailable`、`pywin32`、`MODULE_NOT_FOUND`、`Administrator`。

---

## 五、本次改动的文件

项目目录 `D:\pager-bridge\app\pager_bridge\` 和安装目录 `C:\Users\10023\Documents\pager-app\pager_bridge\` 两份都已同步修改：

| 文件 | 改动 |
|---|---|
| `agents.json` | Hermes、WorkBuddy 路径改为本机；Codex、Claude 加代理环境变量；Claude 加登录命令和说明 |
| `acp_client.py` | `_http_connect` 遇到连接被拒绝时重试最多 8 秒 |
| `bridge.py` | `auth_required_text` 识别中文“需要先登录”；`_run_prompt` 把发指令时的登录异常也当成需要登录；`prompt_refusal_text` 按智能体名字显示提示 |
| `requirements.txt` | 新增 `pywin32>=308`、`psutil>=5.9` |

本机环境上的改动（不在代码里，换电脑要重做）：
- Python 3.14 装了 `pywin32`、`psutil`；
- `%LOCALAPPDATA%\pager-agents\` 下装了 Codex、Claude 两个 ACP 适配器；
- `%LOCALAPPDATA%\pager-agents\cursor\dist-package\` 复制了 Cursor 命令行。

注意：`agents.json` 现在写的是本机路径（`C:\Users\10023\...`、`D:\Program Files\WorkBuddy\...`），**不能原样打包给别的电脑**，打包前按第四节第 1 条改回通用写法。
