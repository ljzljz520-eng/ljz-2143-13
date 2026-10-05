# 背景窗口控件管理（C/SDL2 + Python/SQLite + noVNC）

本项目把背景窗口中的**窗口标题**和 **开始 / 设置 / 退出** 三个按钮接入同一套控件管理：

- 网页控制台编辑标题、字号、字体回退、浅/深/自适应背景与三个按钮布局。
- Python 服务端校验屏幕、可用区域、48×48 最小触摸命中区、焦点顺序和退出入口保护，校验通过后发布带版本的布局并写入 SQLite。
- C/SDL2 终端轮询并应用版本化布局；所有开始/设置/退出命令均携带布局版本和幂等命令 ID。
- 按钮具备可用、执行中、成功、失败/禁用四类可视状态；开始命令有本地与服务器双重防重。
- 服务器策略与断网本地策略分离；长任务、受控退出、服务不可达和未完成操作均写入恢复记录。
- noVNC 只作为远程显示通道，实际渲染与命中处理仍由 C 客户端执行。

## 端口

| 端口 | 用途 |
| --- | --- |
| `6080` | noVNC，查看真实 C/SDL2 窗口 |
| `8080` | 网页布局控制台和 HTTP API |

启动：

```bash
docker compose up --build
```

然后打开：

- 控件编辑台：<http://localhost:8080/>
- 真实终端画面：<http://localhost:6080/>

本地开发：

```bash
make
make server      # 终端 1：Python + SQLite 服务
CONTROL_SERVER=http://127.0.0.1:8080 make run
python3 -m unittest discover -s tests -v
```

## 数据与接口

SQLite 默认位于 `data/control.db`，核心表：

- `layouts`：草稿、草稿修订号、已发布布局和发布版本。
- `device_policy`：服务器对每个动作的可用性策略。
- `devices`：设备心跳与终端实际上报布局。
- `commands`：幂等命令、版本、状态、进度和结果。
- `events`：布局发布、策略变更、草稿冲突之外的恢复审计记录。

主要接口：

```text
GET  /api/layout/published
GET  /api/layout/draft
PUT  /api/layout/draft                 # body 必须包含 base_revision
POST /api/layout/publish               # 发布后 version 增加
POST /api/layout/validate
POST /api/devices/bootstrap            # 终端一次性获取布局+策略，并可对齐旧命令
PUT  /api/devices/actual               # 终端上报设备实际布局、字体和暗化
PUT  /api/policy                       # start/settings/exit 服务器策略
POST /api/commands/{commandId}         # 动作命令，携带 layout_version
PUT  /api/commands/{commandId}/status
POST /api/commands/reconcile
GET  /api/commands
POST /api/recovery
GET  /api/recovery
```

## 布局校验规则

- 固定逻辑屏幕为 `1280×720`；所有视觉按钮必须位于服务端计算的安全/可用区域内。
- 触摸命中区域由服务端根据视觉矩形计算，并保证至少 `48×48`；网页不能绕过服务端手改命中区。
- 任意两个命中区不得重叠；标题矩形不得覆盖退出入口及其命中区，长标题在终端截断显示省略号。
- 焦点顺序固定为 `开始 -> 设置 -> 退出`，焦点环和触摸命中区来自同一份发布布局。
- 字体配置为回退链：`Noto Sans CJK SC -> Noto Sans -> DejaVu Sans -> embedded`。
  缺失字形会逐字回退；所有 TTF 都不可用时使用内置 ASCII 与 tofu 占位，保证仍能显示缺失字体状态。
- 背景支持 `light`、`dark`、`adaptive`。自适应模式采样背景亮度，浅色图自动增加遮罩，深色图降低遮罩。

## 可用性策略与断网动作

| 动作 | 在线且服务器允许 | 在线但服务器禁止 | 断网 |
| --- | --- | --- | --- |
| 开始 | 允许 | 禁止 | 禁止，避免无法确认时产生两份任务 |
| 设置 | 允许并记录命令 | 禁止 | 允许查看本地策略，不伪装成服务器命令 |
| 退出 | 允许 | 安全覆盖为允许 | 允许受控退出 |

即使错误策略试图禁用退出，服务端也会强制把 `exit` 覆盖为 `true`。

## 幂等开始、状态与退出

- 每次“开始”生成一个命令 ID；网络超时后重按仍可复用该 ID。
- 终端在确认期间设置本地 `start_confirming` 守卫，服务器的活动命令表拒绝第二个新 ID，因此不会启动两份任务。
- 命令状态使用 `CONFIRMING/RUNNING/SUCCESS/FAILURE/INTERRUPTED`，映射为按钮的执行中、成功、失败状态。
- 按下时记录布局版本；如果在按下到抬起之间发布了新版，旧版本命令返回 `layout_version_stale`，终端不执行并提示按新布局重按。
- 长任务运行中点击退出会先中止任务、写入未完成操作恢复记录，再进入退出保存。
- 退出最多等待约 3 秒业务保存确认；服务不可达时保留本地 `data/active-journal.json` 和 `data/recovery.log`，走受控退出。
- 退出末尾销毁文字纹理、背景纹理、字体、渲染器和窗口资源。

## 验收场景

1. **按下到抬起之间换版**：打开开始按钮按下不松开，另一控制台发布布局；抬起后旧版本被拒绝。
2. **长任务期间退出**：点击开始后点击退出，任务被标记为 `INTERRUPTED`，恢复页显示未完成操作。
3. **字体缺失**：把字体改成不存在的族名，终端逐字回退；极端缺失时显示内置 tofu，实际字体链随设备快照上报。
4. **两个控制台同时改标题**：第二个保存收到 `draft_revision_conflict`，网页展示服务器修订并允许放弃或按最新修订重提。
5. **设备实际布局**：终端定时上报真实矩形、命中区、状态、背景亮度、有效暗化和字体链，在网页“设备实际布局”查看。
6. **恢复记录**：断网、长任务中止、退出超时和重启后的未完成 journal 都会出现在服务端事件或终端本地日志中。

## 目录

```text
├── server/app.py          # HTTP API + SQLite + 布局校验
├── web/                   # 网页编辑台
├── src/                   # C/SDL2 终端、渲染、网络、存储、字体回退
├── assets/background.png
├── tests/test_server.py   # API 验收测试
└── docker/                # Xvfb/x11vnc/noVNC 编排
```
