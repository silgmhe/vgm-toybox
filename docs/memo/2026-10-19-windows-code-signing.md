# 备忘录：Windows 代码签名的两条路线

- **状态**：暂缓；发布工作流与 SignPath 配置已回退，只保留 README 的许可证/上游说明
- **日期**：2026-10-19
- **来源**：从 GitHub Release 下载的 Windows 包总是提示“未知的应用程序”，本地构建的包不提示
- **一句话**：本地构建没有 Mark-of-the-Web，所以 Windows 不查 SmartScreen；公开下载的包要走
  Authenticode，但 SignPath Foundation 卡在项目声誉，Azure Artifact Signing 卡在中国大陆的
  Public Trust 地区限制——两条路目前都不适合接入，先只把事实和方案记下来。

---

## 1. 现象与根因

| 事实 | 说明 |
|---|---|
| 本地构建直接运行不提示 | 本地 `dist\Standalone\VGMToybox.exe` 没有 `Zone.Identifier`，Windows 不触发 SmartScreen |
| GitHub Release 的 ZIP 运行会提示 | 浏览器下载时给 ZIP 写入 `Zone.Identifier`，解压后通常传给 EXE；SmartScreen 看到“来自 Internet + 无签名 + 无信誉”就提示 |
| 本地 EXE 与 VST3 都没有签名 | `Get-AuthenticodeSignature` 对两者都返回 `NotSigned` |
| 签名不等于立刻没有提示 | SmartScreen 信誉按文件哈希和下载历史自动积累；新证书/新版本仍可能先警告，Azure FAQ 明确说需要下载历史 |

本地检查命令：

```powershell
Get-AuthenticodeSignature dist\Standalone\VGMToybox.exe
Get-AuthenticodeSignature build\plugin\VGMToybox_artefacts\Release\VST3\VGMToybox.vst3\Contents\x86_64-win\VGMToybox.vst3
Get-Item .\VGMToybox.exe -Stream Zone.Identifier   # 看下载标记
Unblock-File .\VGMToybox-windows-x64.zip           # 解压前解除锁定
```

---

## 2. 两条候选路线

### 2.1 SignPath Foundation

- 免费，证书由 SignPath Foundation 持有并签发给项目；私钥在 SignPath 的 HSM 中；
- 每次签名请求需要人工批准，不是全自动；
- 需要项目公开代码签名策略、团队角色/MFA、OSI 许可证、持续维护、已发布版本；
- 对“可执行应用”要求可验证的声誉（库/组件/包的豁免不适用于一个独立播放器）；
- CI 用 `signpath/github-action-submit-signing-request@v3`，输入 GitHub artifact id，返回签名后的目录。

当时仓库数据（2026-10-19，GitHub API）：0 star、0 fork、10 个 release，总下载量约 20，仓库创建于
2026-10-16。基金会条款要求应用类项目“widely used or trusted”，这是最可能被拒的原因。项目基于
libvgm/VGMPlay 的历史可以写进 Reputation，但不等同于本项目自己的用户量。

### 2.2 Azure Artifact Signing（原 Azure Trusted Signing）

- Basic $9.99/月，包含 5,000 次签名，超出 $0.005/次；Premium $99.99/月，100,000 次；
- 需要付费 Azure 订阅，免费/试用订阅不行；
- 身份验证（Identity Validation）通常 1–20 个工作日；
- **Public Trust 地区限制**：组织须在美国、加拿大、欧盟、英国、澳大利亚、新西兰、日本、韩国、新加坡、瑞士、挪威或以色列；个人开发者只限美国或加拿大；
- 中国大陆的个人/公司无法申请 Public Trust；Private Trust 只对安装了信任策略的机器有效，解决不了公开下载的 SmartScreen；
- 官方 GitHub Action：`azure/artifact-signing-action@v2`（旧名 `trusted-signing-action`），只支持 Windows x64 runner，不能跑在 `windows-11-arm` 上；
- 证书有效期约 3 天，必须用 RFC3161 时间戳（`http://timestamp.acs.microsoft.com`）。

**结论**：在当前所在地和主体条件下，Azure 这条路线不可用，不再继续投入。

---

## 3. 当时准备好的实现（已回退，不在当前工作区）

为避免影响发布构建，以下改动已撤回；需要恢复时按本节重建。

### 3.1 发布工作流

曾在 `.github/workflows/release.yml` 的 Windows 矩阵中加入：

1. 构建后只暂存 `dist\release\`（不立即压缩）；
2. 检查 `SIGNPATH_API_TOKEN`、`SIGNPATH_ORGANIZATION_ID`、`SIGNPATH_PROJECT_SLUG`、
   `SIGNPATH_SIGNING_POLICY_SLUG`；缺失时打印警告并继续未签名发布，`SIGNPATH_SIGNING_REQUIRED=true`
   时直接失败；
3. 把 `dist\release\*` 上传为临时 artifact（`unsigned-<package>`），用
   `signpath/github-action-submit-signing-request@v3` 提交，`output-artifact-directory: dist\signed`，
   等待超时 3600 秒；
4. 把签名结果复制回 `dist\release`，逐个 `Get-AuthenticodeSignature` 校验为 `Valid`，再压缩；
5. 最终 artifact 名保持 `VGMToybox-windows-x64.zip` / `VGMToybox-windows-arm64.zip`；发布 job 的
   `download-artifact` 加 `pattern: VGMToybox-*`，排除 `unsigned-*`；
6. 工作流权限加 `actions: read`。

### 3.2 SignPath Artifact Configuration

文件路径曾为 `.signpath/artifact-configurations/windows.xml`，Slug 用 `windows`：

```xml
<?xml version="1.0" encoding="utf-8"?>
<artifact-configuration xmlns="http://signpath.io/artifact-configuration/v1">
  <zip-file>
    <pe-file path="VGMToybox.exe">
      <authenticode-sign />
    </pe-file>
    <pe-file path="VGMToybox.vst3/Contents/*/VGMToybox.vst3" max-matches="unbounded">
      <authenticode-sign />
    </pe-file>
  </zip-file>
</artifact-configuration>
```

注意：上传的是 `dist\release` 的**内容**，不是最终 ZIP；`upload-artifact` 会再包一层 ZIP，
所以根元素是 `<zip-file>`。VST3 的 x64/arm64 内部二进制都靠 `Contents/*/VGMToybox.vst3` 覆盖。

### 3.3 申请的字段与链接

申请入口：<https://signpath.org/apply>

- Project Name：`VGMToybox`
- Repository URL：`https://github.com/silgmhe/vgm-toybox`
- Homepage URL：`https://github.com/silgmhe/vgm-toybox`
- Download URL：需指向一个提到 SignPath 的下载页；当时准备用 README 的 `## Download` 小节
- Privacy Policy URL：可用 `CODE_SIGNING.md#privacy`，或说明软件不收集数据
- Category：`Audio`
- Maintainer Type：`Individual maintainer(s)`
- Build System：`GitHub Actions`
- 其余个人信息与同意条款由申请人本人填写

### 3.4 CODE_SIGNING.md

当时写过 `CODE_SIGNING.md`，包含：免费签名由 SignPath.io 提供、证书由 SignPath Foundation 提供；
签名范围（Windows 的 EXE + VST3 内部二进制）；团队/审批人；隐私说明；以及启用步骤。该文件已删除，
恢复时需重新写，并在 README 增加 “Code signing policy” 链接。

### 3.5 GitHub 配置

- Secret：`SIGNPATH_API_TOKEN`
- Variables：`SIGNPATH_ORGANIZATION_ID`、`SIGNPATH_PROJECT_SLUG`、
  `SIGNPATH_SIGNING_POLICY_SLUG`
- 可选：`SIGNPATH_ARTIFACT_CONFIGURATION_SLUG`（默认 `windows`）
- 首次签名验证成功后：`SIGNPATH_SIGNING_REQUIRED=true`

---

## 4. 当前保留的文档改动

只保留与签名无关、对项目本身有用的许可证说明：

- `README.md` / `README.zh-CN.md` 增加 `## Licence` / `## 许可证`：
  - 项目为 AGPL-3.0-or-later；
  - 选择 AGPL 的决定性原因是 JUCE 的双许可政策（AGPLv3 或商业），本项目使用 AGPLv3 路径；
    若取得 JUCE 商业许可证，才可能采用其他许可证；
  - 上游项目表：libvgm（按文件 BSD-3/GPL-2.0/LGPL-2.1/MIT）、JUCE 8.0.9、inih（BSD-3）、
    wingetopt（ISC + BSD-3）、zlib 1.3.1、CPM.cmake（MIT，仅构建期）。
- 构建说明修正为：JUCE 与 zlib 都由 CPM 拉取并固定版本，不是“只下载 zlib”。

这些改动不影响构建，也不需要任何签名凭据。

---

## 5. 若以后重启

1. 先积累项目声誉：在 VGM/chiptune/VST 社区发布 v0.5.x，收集 star、下载、讨论；至少让
   Reputation 有可写的第三方证据。
2. 申请 SignPath Foundation：先公开 `CODE_SIGNING.md` 和 README 的下载/策略链接，再提交申请。
3. 批准后创建 SignPath 项目、`release-signing` 策略（限制 `refs/tags/v*`）和 `windows` artifact
   configuration，生成 API Token。
4. 恢复 §3 的工作流与配置，先做一次不设 `SIGNPATH_SIGNING_REQUIRED` 的 tag 验证；签名包通过
   `Get-AuthenticodeSignature` 后，再把开关设为 `true`。
5. 若 Foundation 因声誉拒绝：考虑付费 OV/EV 证书，或在有美国/加拿大/欧盟等合格实体时再评估
   Azure Artifact Signing；不要用 Private Trust 冒充公开信任。

---

## 6. 决策记录

| 日期 | 结论 | 理由 |
|---|---|---|
| 2026-10-19 | 暂缓代码签名；回退工作流与 SignPath 配置，只保留许可证/上游文档 | 无可用凭据；SignPath 声誉不足；Azure Public Trust 对中国大陆主体不可用 |
| 2026-10-19 | 不把签名步骤留在 release.yml | 避免在未配置凭据时影响现有发布构建 |

## 7. 什么时候重开

- 项目获得可验证的外部采用（star、下载、社区讨论、媒体/博客）；
- SignPath Foundation 明确表示可以接受当前项目；
- 主体迁移到 Public Trust 支持的国家/地区，或取得当地实体；
- 决定购买 OV/EV 证书并需要把签名接入 CI。
