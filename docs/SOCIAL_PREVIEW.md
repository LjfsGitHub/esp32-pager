# GitHub 社交预览图（Social preview）

推荐尺寸：**1280 × 640** PNG。

仓库已生成：`docs/gallery/github-social-preview.png`（Web 界面 + 方屏 UI + 文案）。

## 上传步骤（仓库所有者）

1. 打开 https://github.com/LjfsGitHub/esp32-pager/settings  
2. **General** → **Social preview** → **Upload an image**  
3. 选择本仓库中的 `docs/gallery/github-social-preview.png`  
4. 保存后，在 Twitter / 微信 / Slack 粘贴仓库链接即可看到大图卡片。

## 重新生成

需先启动 Bridge：`python -m pager_bridge.serve`，然后：

```powershell
python scripts/capture_web_screenshots.py
python scripts/make_social_preview.py
```
