# Pager 硬件与分支

## 分支选择

- **方屏 2.16"** → `git checkout main`
- **圆屏 1.75C** → `git checkout round-amoled-1_75c`

## 差异摘要

| 项目 | main (2.16) | round-amoled-1_75c |
|------|-------------|---------------------|
| 分辨率 | 480×480 | 466×466（圆屏 safe region） |
| PTT GPIO | 18，低有效 | 3，高有效（`pager_board.h`） |
| 电源 | 板载默认 | AXP2101 @ I2C 0x34（`pager_pmu.c`） |
| BSP | 2.16 官方 | 1.75C + `buffer_height=4` 等（见补丁文档） |

## 圆屏补丁清单

完整步骤见：

`examples/esp-idf/03_esp-brookesia/patches/ROUND_BSP_PATCH.md`

`managed_components/` 被 gitignore，换机或 `idf.py` 拉组件后需按该文档核对 BSP 与 QA 串口。

## 编译

```bash
cd examples/esp-idf/03_esp-brookesia
idf.py set-target esp32s3
idf.py build flash monitor
```

设备端嵌入的 TLS 信任根与 `bridge/certs/` 中开发证书对应；更换证书后需同步更新固件中的 `pager_cert.pem` 并重新烧录。
