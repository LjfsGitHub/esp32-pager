# Pager G0 技术验证报告

日期：2026-10-21
验证对象：本机 Hermes Agent 0.21.4（via ACP stdio）
结论：**通过。** 外部设备可以完整控制本机 Hermes：发送指令、接收流式状态与工具事件、弹出审批、批准/拒绝真实生效。同时发现一个必须写进产品定义的能力边界（见"关键发现 2"）。

---

## 1. 验证目标

1. 设备（模拟器）能否把语音/文字指令送到本机 Hermes，并让它真的操作电脑。
2. 能否拿到归一化的状态流（思考中 / 执行工具 / 等待审批 / 完成）。
3. 审批能否在设备侧完成"批准"和"拒绝"，并且拒绝真的能阻止执行。
4. 延迟是否落在产品定义的可接受区间。

## 2. 架构

```
浏览器设备模拟器 ──WebSocket──▶ Pager bridge (FastAPI, 127.0.0.1:8765)
                                     │  ACP = JSON-RPC 2.0 over stdio（换行分隔）
                                     ▼
                              hermes-acp.exe  ──▶  Hermes Agent ──▶ DeepSeek
                                     │
                          工具事件 / session/request_permission
```

- `pager_bridge/acp_client.py` — ACP 客户端：进程管理、请求/通知分发、审批响应。
- `pager_bridge/bridge.py` — 状态机、风险分级、WebSocket/HTTP、Hermes 启动与 Git Bash 探测。
- `pager_bridge/static/index.html` — 设备模拟器（状态灯、屏幕、审批卡、PTT、指标）。
- `pager_bridge/drive.py` — 命令行驱动，可脚本化跑"下指令 → 审批 → 等完成"。
- `pager_bridge/test_risk.py` — L0–L3 风险分级单测（纯函数，不执行命令）。
- `pager_bridge/probe_acp.py` — 最小 ACP 探针。

选 ACP 而不是模拟键鼠：ACP 直接给出结构化工具调用与审批事件，语义清晰、可审计、跨平台。

## 3. 环境

| 项 | 值 |
|---|---|
| Hermes | 0.21.4（`hermes-acp --check` → OK） |
| 模型 | deepseek-v4-flash（DeepSeek） |
| ACP 协议 | v1（initialize / session/new / session/prompt / session/update / session/request_permission） |
| 系统 | Windows 10，Python 3.12.7 |
| 审批模式 | 验证终端审批时临时设为 `approvals.mode=manual`，验证后已恢复 `smart` |
| 依赖修复 | `HERMES_GIT_BASH_PATH` 指向不存在的路径 → bridge 自动探测到 `PortableGit\bin\bash.exe` |
| 工作区 | `pager_workspace/`（ACP 会话 cwd，风险分级的"工作区"边界） |

## 4. 验证结果

| # | 场景 | 指令要点 | 审批 | 结果 | 耗时 |
|---|---|---|---|---|---|
| 1 | 只读任务 | 列出目录并总结 | 无 | 成功（当时 Git Bash 缺失，走了 execute_code 兜底） | 22.1 s |
| 2 | 写文件 ×2 | `write_file` 创建文件 | L1，allow | 文件创建成功（17 B / 14 B，内容校验通过） | 5.69 s / 4.97 s |
| 3 | 删除目录 ×2 | `rm -rf trash_demo` | L2，deny | 命令未执行，目录与文件完好，Hermes 明确回复"被拒绝" | 4.14 s / 6.19 s |
| 4 | 代码执行 | `execute_code` 打印 | **无审批** | 执行成功（覆盖缺口，见下） | 2.1–2.9 s |
| 5 | 风险分级 | 28 个样本（含 18 个 L3） | — | 28/28 通过 | — |

### 延迟指标（bridge 进程内实测）

| 指标 | 值 | 说明 |
|---|---|---|
| ACP 会话建立 | 冷 15.2 s / 热 4.9 s | 开机一次性成本，可在后台预建 |
| 松手 → 首个反馈 | **~2.0 s** | 状态转入"执行工具"（产品定义要求 p50 < 2 s，基本达标） |
| 审批弹出 → 决策生效 | **375 ms** | 含 CLI 轮询（400 ms）；设备端 WebSocket 实时推送会更低 |
| 简单任务端到端 | 4–6 s | 写文件 / 删目录（含 1 次审批） |

## 5. 关键发现

1. **ACP 是可用的控制面。** 流式消息、工具调用（search/read/write/terminal/execute_code）、审批请求、会话标题、用量全部能拿到，字段结构稳定。
2. **审批覆盖率 = host agent（Hermes）自己的审批面，不是设备决定的。** 实测：
   - 文件编辑（`write_file` / `patch`）：**必问**，设备可批可拒。✓
   - `terminal`：只有命中 Hermes 危险模式（如 `rm -rf`、`reg delete`、`shutdown`）才问。**smart 模式下守护 LLM 会替人放行"看起来安全"的命令**（如工作区内的 `rm -rf trash_demo`），要完整人工审批必须 `approvals.mode=manual`。
   - `execute_code`：ACP 模式下按设计**不做整段审批**（Hermes 上游测试注释明确：`HERMES_EXEC_ASK` 是 gateway 专用，不适用 ACP）。脚本里直接调 `subprocess` 可绕过 terminal 的逐条检查。**这是"物理审批"叙事的最大技术风险。**
3. **L3 策略在真机上成立。** 分级器把"工作区外路径 / 硬线危险命令"评为 L3，设备侧禁用"批准"，只能拒绝并要求回电脑处理；拒绝路径已验证有效。
4. **Git Bash 是 Windows 下的硬依赖。** 路径错了会导致 file/terminal 工具全部失败（工具返回 error，agent 退回 execute_code）。bridge 已加入自动探测。
5. **中文 Windows 的 ACP stdio 是 GBK。** 解决：发送纯 ASCII JSON（`ensure_ascii=True`），接收按 UTF-8 → GBK 回退解码。
6. **一个 ACP 进程一个会话。** busy 时新指令被拒绝；队列、多会话、取消的产品级行为还没定型。
7. **设备屏只显示归一化摘要，不显示原始命令。** bridge 侧把工具事件转成"运行命令 · taskkill"这类人话摘要，屏幕上最多显示 2 行进行中的工具 + "✓ n 个工具已完成"；完整命令只出现在审批卡（决策必需）和 PC 侧日志。已在浏览器模拟器中验证。

## 6. 对产品定义的影响（修正）

1. "物理审批"作为核心差异化**成立**，但对外表述必须是"**接管 agent 的审批**"，不能承诺"拦截一切操作"。
2. 产品需要内置 **"Pager 模式"预设**：安装向导里把 host agent 配成人工审批（Hermes：`approvals.mode=manual`），并提供"限制高风险工具（如 execute_code）"选项或沙箱建议。
3. L3 分级（设备不可批）、审批卡片信息密度（命令 + 影响 + 风险理由）在 2.8 吋屏幕上信息量足够。
4. 语音链路未验证（G0 走文字与浏览器语音识别）；本地 STT/TTS 仍是 G1 的技术验证项。
5. "首反馈 < 2 s"基本达到；会话冷启动 15 s 需要靠"常驻 bridge"隐藏（已实现，开机后常驻）。

## 7. 复现方式

```powershell
# 1) 启动 bridge（后台常驻）
python -m uvicorn pager_bridge.bridge:app --host 127.0.0.1 --port 8765

# 2) 打开设备模拟器
#    http://127.0.0.1:8765/

# 3) 命令行驱动（可自动化）
python pager_bridge/drive.py "列出当前目录的文件" --approve none
python pager_bridge/drive.py "创建 demo.txt" --approve allow
python pager_bridge/drive.py "删除 trash_demo 目录" --approve deny

# 4) 风险分级单测
python pager_bridge/test_risk.py

# 5) 完整人工审批（演示用；不改 Hermes 默认行为）
hermes config set approvals.mode manual   # 演示终端审批
hermes config set approvals.mode smart    # 恢复默认
```

## 8. 已知限制与 G1 待办

- [ ] 设备是浏览器模拟器，未跑 ESP32 固件；WebSocket 协议需固化版本并实现到固件。
- [ ] 未验证 USB 串口路径（产品定义里的 USB 连接方式）。
- [ ] `execute_code` 审批缺口：G1 需决定（禁用工具 / 沙箱 / 或接受并改写产品承诺）。
- [ ] 审批超时策略未定：ACP 默认 300 s，bridge 目前无限等待。
- [ ] 取消、排队、多会话的实际行为未充分测试。
- [ ] smart 模式守护 LLM 的放行行为需要产品化解释（"为什么这个操作没问我"）。
