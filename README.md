# Visual Window App — 背景窗口控件管理

一个 **C11 + SDL2 背景窗口**（容器内 noVNC 展示），窗口标题与
**「开始 / 设置 / 退出」三个按钮**纳入统一控件管理，配套：

* **网页编辑器**（`web/index.html`，单文件）：编辑标题、字号、开始/设置布局，
  展示可用区域预览、**草稿冲突(409)**、设备实际布局与未完成操作恢复记录；
* **Python 标准库后端**（`server/`）：可用区域校验、SQLite 落库、
  单调布局版本、**带版本命令**、幂等任务、断网恢复记录；
* **可宿主测试的 C 内核**（`src/core/`，不依赖 SDL）：布局编译（浅/深背景
  自适应暗化、字体回退、触摸命中不重叠、键盘焦点序）、按下/抬起跨版本抓取、
  按钮四态+单飞幂等、服务器/本地可用性策略、受控退出、操作日志。

## 架构

```text
Browser editor ──draft/publish──▶ Python server ──versioned commands──▶ C SDL app
(web/index.html)                  (SQLite; validation)   ▲ libcurl (src/vw_net_client.c)
                                                          │ polling /commands /tasks /journal
主机可测内核: src/core/{vw_layout,vw_command,vw_fsm,vw_policy,vw_exit,vw_journal}.c
```

详见 **[docs/design.md](docs/design.md)**（含验收用例映射表）。

## 本机快速验证（无需 SDL/显示器）

```bash
make check                          # C 内核单元测试（布局/命令/状态机/策略/退出/日志）
python3 server/server.py --self-test # 服务器自测（校验/冲突/版本/幂等/恢复）
```

C 网络客户端 ↔ Python 服务器真实联调（libcurl）可在本机直接做：

```bash
VW_PORT=8091 python3 server/server.py     # 另开一个终端
# 用 src/vw_net_client.c 对 /commands、/tasks 发请求，验证 409 与幂等重放
```

## 运行网页编辑器 + 后端

```bash
python3 server/server.py          # 默认 0.0.0.0:8080
# 浏览器打开 http://localhost:8080/
```

## 一键启动完整可视化链路（窗口 + noVNC + 后端）

```bash
docker compose up --build
# noVNC: http://localhost:6080   编辑器/API: http://localhost:8080
```

C 客户端通过环境变量 `VW_API` 指定后端地址；不可达时按终端本地策略运行，
退出仍走“业务确认保存 + 渲染资源释放”的受控路径，未完成操作写入本地日志，
恢复后补传（见设计文档第 8–10 节）。

## 关键行为

* 长标题在退出保留带前按码点省略，**永不盖住退出入口**；
* 触摸命中区 ≥44px 且互不重叠，键盘 Tab 序随同一布局更新（开始→设置→退出）；
* 按下起记录布局版本，**按下到抬起间换版则取消该次操作**，服务端再做 409 兜底；
* 「开始」四态 idle/executing/confirming/success/failed；
  网络超时后重按复用同一幂等键，**不可能启动两份任务**；
* 断网：开始（无在跑任务时）与退出允许，设置仅在有缓存时允许；
* 退出：等待业务确认保存 → 尝试同步（失败不阻塞）→ 释放渲染资源；
  长任务时再按一次退出为受控强制退出。

## 目录

```text
src/core/        宿主可测 C 内核（无 SDL 依赖）
src/vw_net_client.*  libcurl 短超时 HTTP 客户端
src/widgets.*    SDL/SDL_ttf 控件渲染与输入胶合
src/{main,window,renderer}.c  现有 SDL 应用（已接入控件层）
server/          Python 后端 + schema.sql
web/index.html   单文件网页编辑器
docs/design.md   设计文档与验收映射
tests/core_test.c 内核测试
```
