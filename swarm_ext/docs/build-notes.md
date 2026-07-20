# Docker 构建摸底笔记(`run-docker.sh ubuntu`)

调研范围:`deploy/docker/run-docker.sh`、`deploy/docker/variants.json`、
`deploy/docker/entrypoint.sh`、`deploy/docker/_docker-exec.sh`、
`deploy/docker/_variant_info.py`、`deploy/docker/Dockerfile`、
`deploy/docker/gen_compose.py`、`deploy/docker/docker-compose.yml`、
`deploy/docker/lib/*.sh`,以及 CI 侧 `.github/workflows/docker.yml`、
`.github/actions/docker/action.yml`、`.github/scripts/find_artifact.py`
(用于交叉验证产物路径/代理是否透传)。

本笔记只做只读调研,**未执行任何 `docker build`/`docker run`**。

---

## 1. 产物路径

- `run-docker.sh` 的 `SOURCE_DIR` 就是仓库根目录:
  `SOURCE_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"`(`deploy/docker/run-docker.sh:16`)。
- `BUILD_DIR` 的**实际默认值来自 `_docker-exec.sh`**,不是 `run-docker.sh`:
  `BUILD_DIR="${BUILD_DIR:-${SOURCE_DIR}/build}"`(`deploy/docker/_docker-exec.sh:35`)。
  对 `ubuntu` variant(非 aarch64)来说,默认宿主产物目录就是
  **`<repo_root>/build`**。
- `run-docker.sh` 头部注释里写的 `BUILD_DIR` 默认值(`<repo>/build-aarch64`)
  **只对 `aarch64` variant 生效**:只有当 `VARIANT == "aarch64"` 时脚本才会显式赋值
  `BUILD_DIR="${BUILD_DIR:-${SOURCE_DIR}/build-aarch64}"` 并通过 `run_env` 传给
  `_docker-exec.sh`(`deploy/docker/run-docker.sh:11`, `run-docker.sh:50-56`)。对
  `ubuntu` variant,`run-docker.sh` 的 `run_env` 只塞了 `SOURCE_DIR`
  (`deploy/docker/run-docker.sh:49`),完全不碰 `BUILD_DIR`。
- **可以覆盖**:因为 `run-docker.sh` 最终是用
  `env "${run_env[@]}" "${SCRIPT_DIR}/_docker-exec.sh" ...`
  (`deploy/docker/run-docker.sh:58-59`)拉起子进程,`env NAME=VAL cmd` 只会
  设置/覆盖列出的变量,不会清空调用者 shell 里其它已 `export` 的环境变量。
  所以只要在调用 `run-docker.sh` 之前 `export BUILD_DIR=/some/path`,
  这个值会原样透传进 `_docker-exec.sh`,被其 `"${BUILD_DIR:-...}"` 展开式接住
  (`deploy/docker/_docker-exec.sh:35`)。CI 侧同样把产物目录钉死为
  `${{ github.workspace }}/build`(`.github/workflows/docker.yml:136`,
  `docker.yml:159`),与本地默认值一致,可交叉验证。
- 容器内挂载点:`-v "${BUILD_DIR}:/project/build"`
  (`deploy/docker/_docker-exec.sh:53`),源码挂载点是
  `-v "${SOURCE_DIR}:/project/source"`(`_docker-exec.sh:52`)。
- `ubuntu` variant 对应 Dockerfile 的 `linux` target(`native x86_64 build`
  分支,`deploy/docker/Dockerfile:65-81`)。`entrypoint.sh` 在容器内执行:
  `qt-cmake -S /project/source -B /project/build ...`
  (`deploy/docker/entrypoint.sh:83-85`)、
  `cmake --build /project/build --target all ...`(`entrypoint.sh:86`)、
  `cmake --install /project/build --config "${BUILD_TYPE}"`(`entrypoint.sh:87`)、
  最后 `cmake --build /project/build --target qgc-package`(`entrypoint.sh:92`)
  生成 AppImage/.deb 等打包产物。因为 `/project/build` 就是宿主 `BUILD_DIR`
  的 bind mount,所有产物(可执行文件、install 树、AppImage、.deb)都直接落在
  **`<repo_root>/build/`** 下(具体子目录由 CMake/CPack 决定,不是固定顶层路径)。
- 产物文件名/后缀由 `variants.json` 里 `ubuntu` 条目定义:
  `"artifact_pattern": "*.AppImage"`、`"package_pattern": "*.deb"`
  (`deploy/docker/variants.json:12-13`)。CI 用
  `find_artifact.py` 通过 `build_dir.rglob(pattern)` 递归查找
  (`.github/scripts/find_artifact.py:21-26`),`--build-dir` 就是
  `${{ github.workspace }}/build`(`.github/workflows/docker.yml:136`),
  说明产物确实散落在 `build/` 目录树内某处,需要用同样的 glob 递归找,而不是
  假设一个固定文件名路径。

**结论**:`ubuntu` variant 产物写回 `<repo_root>/build/`(即
`/media/hello/data1/Jinqiang/QGC/qgroundcontrol/build/`),`BUILD_DIR` 默认值
由 `_docker-exec.sh:35` 决定为 `${SOURCE_DIR}/build`,可通过在调用
`run-docker.sh` 前 `export BUILD_DIR=<自定义路径>` 来覆盖。

---

## 2. docker 调用形式

- variant 元数据来自 `_variant_info.py`,读取 `variants.json` 后
  eval 出 `target`/`default_image`/`fuse`/`build_args`
  (`deploy/docker/run-docker.sh:32-35`,`deploy/docker/_variant_info.py:34-37`)。
- `ubuntu` 在 `variants.json` 中的定义(`deploy/docker/variants.json:4-15`):
  ```json
  {
    "id": "ubuntu",
    "target": "linux",
    "image": "qgc-ubuntu-docker",
    "fuse": true,
    "build_args": {}
  }
  ```
  即:`target=linux`、镜像 tag 默认 `qgc-ubuntu-docker`
  (可用 `IMAGE_NAME` 环境变量覆盖,`run-docker.sh:40`)、`fuse=true`、
  **`build_args` 为空**——`ubuntu` variant 不传任何 `--build-arg`,直接用
  Dockerfile 里烘焙的默认基础镜像
  `ubuntu:24.04@sha256:786a8b558f7be160c6c8c4a54f9a57274f3b4fb1491cf65146521ae77ff1dc54`
  (`deploy/docker/Dockerfile:12`)。

- **`docker build` 的确切形式**(`deploy/docker/run-docker.sh:42-47`):
  ```bash
  docker build \
      --file deploy/docker/Dockerfile \
      --target linux \
      -t qgc-ubuntu-docker \
      <repo_root>          # SOURCE_DIR,构建 context
  ```
  (若 `build_args` 非空,`--build-arg KEY=VALUE` 会插在 `--target` 和 `-t` 之间,
  顺序与 `variants.json` 里 key 的声明顺序一致——见文件头注释
  `variants.json:2` 和 `_variant_info.py:30-33`;但 `ubuntu` 的 `build_args={}`
  所以此处没有任何 `--build-arg`)。
  没有出现 `--network` 参数(在 `run-docker.sh`/`Dockerfile`/
  `docker-compose.yml`/`gen_compose.py` 全文里 grep `network` 均无命中),
  即走 Docker 默认的构建网络。

- **`docker run` 的确切形式**(由 `run-docker.sh:58-59` 调用
  `_docker-exec.sh`,实际拼装在 `deploy/docker/_docker-exec.sh:45-55`):
  ```bash
  docker run \
      --rm \
      --user "$(id -u):$(id -g)" \
      --env HOME=/tmp \
      --env CLEAN_BUILD=0 \
      --env JOBS= \
      --cap-add SYS_ADMIN --device /dev/fuse --security-opt apparmor:unconfined \
      -v "<repo_root>:/project/source" \
      -v "<repo_root>/build:/project/build" \
      qgc-ubuntu-docker \
      Release            # 或调用方传入的 BUILD_TYPE
  ```
  其中 `--cap-add SYS_ADMIN --device /dev/fuse --security-opt apparmor:unconfined`
  这组 `FUSE_FLAGS` 只有在 `--fuse` 被传入时才加(`_docker-exec.sh:15-20`);
  `ubuntu` variant 因为 `variants.json:11` 里 `"fuse": true`,
  `run-docker.sh:38` 会设 `run_flags=(--fuse)` 并传给 `_docker-exec.sh`,
  所以 **`ubuntu` 构建实际上会带上这组 FUSE 挂载参数**(AppImage 打包工具
  linuxdeploy/appimagetool 需要)。
  同样,`docker run` 这里也没有 `--network` 参数(同一次 grep 确认),用的是
  Docker 默认 bridge 网络。

**结论**:`ubuntu` 使用 image tag `qgc-ubuntu-docker`、`--target linux`、
挂载 `-v <repo>:/project/source -v <repo>/build:/project/build`、
不带任何 `--network`、`build_args` 为空(不产生 `--build-arg`)。

---

## 3. 代理注入方式

- **现状:仓库里没有任何代理透传逻辑。** 对
  `deploy/docker/{run-docker.sh,_docker-exec.sh,entrypoint.sh,Dockerfile,
  variants.json,gen_compose.py,docker-compose.yml,lib/*.sh}` 以及
  `.github/actions/docker/action.yml`、`.github/workflows/docker.yml`
  全文 grep `proxy`/`network`,**零命中**。即:
  - `Dockerfile` 没有声明 `ARG http_proxy`/`ARG https_proxy` 等(全文搜索无结果)。
  - `run-docker.sh` 的 `docker build`(`run-docker.sh:42-47`)不会附加
    `--build-arg http_proxy=...` 之类的参数。
  - `_docker-exec.sh` 的 `docker run`(`_docker-exec.sh:45-55`)也没有
    `--network host` 或 `-e http_proxy=...`。
  - `.github/actions/docker/action.yml` 用 `docker/build-push-action@v7`,
    `build-args` 只透传调用方显式给的 `matrix.build_args`
    (来自 `variants.json`),同样不含代理变量。

- **需要联网的阶段只在 `docker build` 时**(镜像构建期),不在
  `docker run`(entrypoint 执行期):
  - `apt-get`(`deploy/docker/lib/setup-base.sh:20`、
    `lib/setup-base-dnf.sh`、`lib/setup-base-pacman.sh`)
  - `uv sync` / `uv pip install`(拉 PyPI,`deploy/docker/lib/setup-venv.sh:12-18`)
  - `aqt install-qt`(拉 Qt 官方 CDN,`deploy/docker/lib/install-qt.sh:21-22`,
    被 `retry.sh` 包了 3 次重试)
  - Android SDK cmdline-tools 的 `wget`(`deploy/docker/Dockerfile:128`)
  `entrypoint.sh` 里只跑 `qt-cmake`/`ninja`/`cpack`(`entrypoint.sh:83-92`),
  用的是镜像里已装好的工具链,**不需要联网**,因此 `docker run` 阶段原则上
  不需要代理。

- **Docker 客户端本身有"预定义代理 ARG"的内建行为**(这是 Docker CLI/daemon
  的通用能力,不是 QGC 仓库代码):如果在执行 `docker build` 的那个 shell
  里 `export http_proxy=... https_proxy=... HTTP_PROXY=... HTTPS_PROXY=...`,
  Docker(经典 builder 和 BuildKit 都支持)会把这几个变量自动当作
  predefined build-arg 注入构建过程,**不需要**在 Dockerfile 里显式
  `ARG http_proxy`。但这依赖调用 `docker build` 的 shell 环境本身带着这些
  变量——而 `run-docker.sh` 是用子进程 `env SOURCE_DIR=... docker build ...`
  的方式调用的,只要外层 shell 导出了代理变量,`docker` 命令进程会继承到,
  应该能触发这个自动注入(未实测,建议按下面第 2 点先做一次最小实验验证)。

- **本仓库脚本不透传代理,需要在调用点手动注入,具体做法如下:**
  1. **最直接**:在跑 `run-docker.sh ubuntu Debug` 之前,在**同一个 shell**里
     `export http_proxy=... https_proxy=... HTTP_PROXY=... HTTPS_PROXY=...`,
     依赖 Docker 内建的 predefined-ARG 透传机制。这是"零改代码"方案,
     但依赖 Docker 版本/builder 驱动的行为,不保证 100% 生效
     (例如用了独立的 buildx remote/docker-container driver 时可能不继承
     客户端环境),需要用一次小实验(T1)验证。
  2. **如果第 1 步不生效**:需要改 `deploy/docker/run-docker.sh:42-47`,
     在 `docker build` 命令里显式追加
     `--build-arg http_proxy="$http_proxy" --build-arg https_proxy="$https_proxy"`
     (以及可选的 `no_proxy`),这是本任务范围外的代码改动,留给后续
     T1/T3 步骤按需修改。
  3. **容器如何连到宿主机的本地代理端口**:按用户全局规则,宿主机上真正能用
     的 HTTP 代理是 gost 监听的 `127.0.0.1:8118`(仅供本机 HTTP 客户端用,
     **不是** `/etc/proxychains4.conf` 里的 socks5:10808——apt/uv/aqt/wget
     走的是 `http_proxy`/`https_proxy` 这种标准环境变量,原生只认
     HTTP/HTTPS 代理协议,不认 SOCKS5,所以不能直接把 proxychains 的
     socks5 端口塞进 `http_proxy`)。但容器内的 `127.0.0.1` 指向的是
     **容器自己**,不是宿主机,所以必须解决"容器→宿主 127.0.0.1:8118"
     的可达性,两种可行做法(都需要在 `run-docker.sh`/`_docker-exec.sh`
     里加参数,目前完全没有):
     - **`--network host`**:给 `docker build`(经典 builder 支持
       `--network=host`;BuildKit 需要 `--network=host` 且
       Dockerfile 对应 `RUN --network=host` 或整体用 host 网络)和
       `docker run`(`_docker-exec.sh:45-55` 里加 `--network host`)都加上,
       这样容器与宿主机共享网络namespace,直接访问
       `127.0.0.1:8118` 即可(本机是原生 Docker Engine on Linux,没有
       Docker Desktop 的 VM 隔离层,`--network host` 在这里是可行的)。
     - **`host.docker.internal`**:给 `docker build`/`docker run` 都加
       `--add-host=host.docker.internal:host-gateway`(Docker Engine
       ≥20.10 支持,**Linux 上默认不会自动定义这个域名**,必须手动加
       这个 `--add-host`),然后把 `http_proxy`/`https_proxy` 设成
       `http://host.docker.internal:8118`,保留网络命名空间隔离。
  4. 两种方式选一即可;鉴于本机是原生 Linux Docker Engine(非 Desktop),
     `--network host` 更简单直接,推荐作为 T1 小实验的首选方案,
     `host.docker.internal` + `--add-host` 作为备选。

**结论**:仓库现有脚本/Dockerfile 完全不做代理透传(经全文 grep 确认零命中);
联网只发生在 `docker build` 阶段;需要在调用 `run-docker.sh` 之前手动
`export http_proxy/https_proxy` 依赖 Docker 内建 predefined-ARG 机制,若不生效
则需改 `run-docker.sh:42-47` 显式加 `--build-arg`;容器访问宿主机
`127.0.0.1:8118` 这个 gost HTTP 代理,需要给 `docker build`/`docker run`
都加 `--network host`(推荐)或 `--add-host=host.docker.internal:host-gateway`
+ 代理地址改成 `host.docker.internal:8118`,这两处目前都不存在,需要后续步骤
(T1)显式加上并验证。

---

## 4. 代理通路实测结论(T1)

本节基于三次实测(非只读调研),验证第 3 节推导出的方案是否真的可行。
过程中用到的临时文件(`/tmp/proxytest/`)和测试镜像(`qgc-proxytest`)已在
实验结束后清理,不留痕迹。

### 4.1 实验 1:宿主代理本身可用 — **通过**

```bash
curl -s -x http://127.0.0.1:8118 -o /dev/null -w "host->8118 %{http_code}\n" https://github.com
# → host->8118 200
```

宿主机上 gost 监听的 `127.0.0.1:8118` HTTP 代理工作正常。

### 4.2 实验 2:docker daemon 拉取境外镜像 — **先失败后修复,通过**

- 直接 `docker pull hello-world`(不做任何改动)**意外地成功了**——但排查后
  发现这不代表代理链路健康:`hello-world` 命中的是 `/etc/docker/daemon.json`
  里预置的 registry mirror `https://docker.xuanyuan.me`,这个 mirror 本身可以
  直连,不经过 daemon 的 `HTTP_PROXY`/`HTTPS_PROXY`。
- 深挖发现 daemon 早就存在一份**与本任务无关的遗留** systemd drop-in
  `/etc/systemd/system/docker.service.d/proxy.conf`:
  ```
  Environment="HTTP_PROXY=socks5://127.0.0.1:10808"
  Environment="HTTPS_PROXY=socks5://127.0.0.1:10808"
  ```
  用 `ss -tlnp` 确认 **10808 端口当前根本没有进程监听**(`curl` 直连测试报
  `Connection refused`),即这条 daemon 代理配置是失效的僵尸配置。
- 用 QGC 实际需要的基础镜像 `ubuntu:24.04`(对应
  `deploy/docker/Dockerfile:12` 锁定的
  `ubuntu:24.04@sha256:786a8b558f7be160c6c8c4a54f9a57274f3b4fb1491cf65146521ae77ff1dc54`)
  复测,**复现了故障**:mirror 返回 `429 Too Many Requests`,直连
  `registry-1.docker.io` 因为走的是死掉的 socks5:10808,报
  `context deadline exceeded` / `request canceled while waiting for connection`。
- 修复(可逆改动):
  1. 备份原文件:
     `sudo cp /etc/systemd/system/docker.service.d/proxy.conf /etc/systemd/system/docker.service.d/proxy.conf.bak-T1`
  2. **没有像任务模板里建议的那样新建 `http-proxy.conf`**——因为已经存在
     同名 `proxy.conf`,systemd drop-in 按文件名字典序合并、同名变量以
     后加载者为准(`http-proxy.conf` 字典序在 `proxy.conf` 之前,会被
     后加载的旧 `proxy.conf` 覆盖回失效的 socks5),所以直接改写既有
     `proxy.conf`,内容改为宿主已验证可用的 gost HTTP 代理:
     ```
     [Service]
     Environment="HTTP_PROXY=http://127.0.0.1:8118"
     Environment="HTTPS_PROXY=http://127.0.0.1:8118"
     Environment="NO_PROXY=localhost,127.0.0.1"
     ```
  3. `sudo systemctl daemon-reload && sudo systemctl restart docker`
  4. 复测:`docker pull ubuntu:24.04` → **成功**
     (`Status: Downloaded newer image for ubuntu:24.04`)。
- **当前状态:daemon 代理已改为 8118 并保持生效(未回滚)**。原因:T2 真正
  跑 `run-docker.sh` 时,`docker build` 第一步 `FROM ubuntu:24.04@sha256:...`
  同样需要 daemon 能拉到这个 pinned 基础镜像;若回滚回失效的 socks5:10808,
  T2 一开始就会在拉基础镜像这一步失败。
- **回滚方法**(如后续确认不再需要 docker 联网,想恢复原状):
  ```bash
  sudo cp /etc/systemd/system/docker.service.d/proxy.conf.bak-T1 /etc/systemd/system/docker.service.d/proxy.conf
  sudo systemctl daemon-reload && sudo systemctl restart docker
  ```
  (注:回滚后会恢复到失效的 socks5:10808,daemon 级镜像拉取会重新失败,
  仅在明确不再需要联网时才建议这样做;备份文件保留在
  `/etc/systemd/system/docker.service.d/proxy.conf.bak-T1`。)

### 4.3 实验 3:docker build(RUN 指令)能否经宿主代理联网 — **通过,方案是 `--network=host`**

最小测试 Dockerfile(测试完已删除,`/tmp/proxytest/` 已清空):

```dockerfile
FROM ubuntu:24.04
RUN apt-get update -o Acquire::http::Timeout=30 -qq && apt-get install -y -qq curl >/dev/null && echo "APT_UPDATE_OK"
RUN echo "proxy env inside build: http_proxy=$http_proxy https_proxy=$https_proxy"
RUN curl -s -o /dev/null -w "container->github via proxy: %{http_code}\n" --max-time 20 https://github.com
```

构建命令:

```bash
export http_proxy=http://127.0.0.1:8118 https_proxy=http://127.0.0.1:8118 \
       HTTP_PROXY=http://127.0.0.1:8118 HTTPS_PROXY=http://127.0.0.1:8118
docker build --network=host --build-arg http_proxy --build-arg https_proxy \
    -t qgc-proxytest /tmp/proxytest
```

结果:

- `apt-get update` 成功(`APT_UPDATE_OK`)。但 `archive.ubuntu.com` /
  `security.ubuntu.com` 在本机网络环境下**即使不挂代理也能直连**(不是被墙
  的域名),所以这一步本身不能单独证明代理透传生效,只能证明网络能通。
- 关键证据是后两条 RUN:
  - `echo` 打印出 `http_proxy=http://127.0.0.1:8118 https_proxy=http://127.0.0.1:8118`
    ——证明调用 `docker build` 那个 shell 里 `export` 的代理变量,经 Docker
    的 predefined proxy build-arg 机制(**不需要**在 Dockerfile 里显式声明
    `ARG http_proxy`),已经原样进入了容器内 RUN 的环境变量。
  - 容器内 `curl --max-time 20 https://github.com` 返回 **`200`**——
    github.com 在没有代理时在本机是连不通的(会被墙),这就证明:
    ① `--network=host` 让容器内的 `127.0.0.1` 确实指向宿主机自身,能摸到
    宿主机监听在 `127.0.0.1:8118` 的 gost 代理;
    ② 容器内的 `http_proxy`/`https_proxy` 环境变量确实生效并被 `curl` 使用。
- 因为首选方案(`--network=host`)已经验证通过,**没有再测试备选方案
  `host.docker.internal` + `--add-host=host.docker.internal:host-gateway`**。
- 清理:`docker rmi qgc-proxytest`、`rm -rf /tmp/proxytest` 均已执行。

### 4.4 对 T2(实际跑 `run-docker.sh`)的结论

- **daemon 级代理**:必须保留当前已生效的 8118 代理配置(见 4.2),否则
  `docker build` 的 `FROM ubuntu:24.04@sha256:...` 这一步就会失败
  (mirror 429 / 死代理超时)。
- **`docker build` 阶段**:需要在 `run-docker.sh:42-47` 组装的 `docker build`
  命令上追加 `--network=host`,并在调用 `run-docker.sh` **之前、同一个
  shell** 里
  `export http_proxy=http://127.0.0.1:8118 https_proxy=http://127.0.0.1:8118 HTTP_PROXY=http://127.0.0.1:8118 HTTPS_PROXY=http://127.0.0.1:8118`。
  依赖 Docker 内建的 predefined-ARG 机制,这些变量会自动透传成 build-arg
  (已实测生效,见 4.3),不强制要求额外手写 `--build-arg`;但显式加上
  `--build-arg http_proxy --build-arg https_proxy`(不给值,让 Docker 从
  调用环境读取同名变量)更保险、效果等价,建议保留。
- **`docker run`(entrypoint 执行期)**:第 3 节已确认 entrypoint 只跑
  `qt-cmake`/`ninja`/`cpack`,不联网。本次 T1 没有单独验证这一点(不在任务
  范围内),沿用第 3 节的结论,即 `docker run` 原则上不需要 `--network host`。
- **是否需要修改 `run-docker.sh`/`_docker-exec.sh` 加 `--network host`**:
  **需要,且只需要在 `docker build` 那一处(`run-docker.sh:42-47`)加
  `--network=host`**——这是本次实测后的明确结论。本任务按约束**未实际
  修改**这两个脚本,留给 T2/T3 按此结论去改;若 T2 选择不改脚本而是手工
  复现等价的 `docker build`/`docker run` 命令来跑首次构建,也可以直接照抄
  4.3 里验证过的命令形式(加 `--network=host` + 4 个代理环境变量)。

---

## 5. 整机编译打通结果(T2–T4,2026-07-20)

> 更正 §4 的一处判断:T1 曾结论"docker run 不需要 `--network host`",实际 cmake
> configure 阶段 CPM/FetchContent 会在 docker run(entrypoint)里 `git clone github`,
> 因此 **docker build 和 docker run 都需要** `--network=host`+代理透传。

### 5.1 结论
QGC + Swarm 用官方 Docker(`ubuntu` variant / Qt 6.11.1 / GCC / C++20 / -Werror)**整机编译链接成功**,产物 `build/Debug/QGroundControl`(633M,ELF,not stripped)。Swarm 21 个 C++ 模块与 3 个 QML 面板全部编入。

### 5.2 依次攻克的构建环境问题(controller 直接修复)
| # | 问题 | 修复 | commit |
|---|---|---|---|
| 1 | docker build 装 Qt 无法联网 | run-docker.sh env 门控 `--network=host`+代理透传 | 93f95ccdd |
| 2 | docker run 里 configure 的 CPM `git clone github(geographiclib)` 直连被墙超时 | _docker-exec.sh env 门控 `--network=host`+代理透传 | 1a8a099a5 |
| 3 | qmlimportscanner 缺 ICU 73(Qt6.11 链接 ICU73,ubuntu24.04 只有 ICU74,ICU 符号版本化→软链无效) | Dockerfile 装 github 预编译 ICU 73.2 | dffe1bfa2 |
| — | 提速技巧 | 复用 build-02 完整镜像叠加 ICU,避免重下 Qt / 绕过 apt 503 | (临时 overlay) |

### 5.3 Swarm 代码真实编译错误(T3,4 类,均 KIMI 无 Qt 环境未发现)
| 类别 | 错误 | 修复 | commit |
|---|---|---|---|
| A | 嵌套 Config NSDMI + 默认参数 `=Config()`(~124 处) | 拆构造/重载 + .cc 委托构造 | 3a73b5fde |
| B | FactGroup 无 heading/altitude 成员 | `qobject_cast<VehicleFactGroup*>` | caff1ca32 |
| C | 1 参信号 connect 到 2 参 showAppMessage(5 处) | 改 lambda connect | 5af1b4318 |
| D | 未使用参数 widthMeters 触发 -Werror | Q_UNUSED | 1f173e65b |

### 5.4 验证证据(T4)
- 编译 `[577/577]` 0 error / 0 undefined reference。
- `nm -C build/Debug/QGroundControl` 抽查 Swarm 符号 3613 个(`SwarmManager::instance()`/`staticMetaObject`、SwarmAIAgent/TaskController/ConstraintChecker 等)。
- `build/qml/QGroundControl/FlyView/qmldir` 注册 `SwarmAIPanel`/`SwarmControlPanel`/`SwarmSituationLayer` 1.0。

### 5.5 遗留
- **AppImage 打包**未完成:`qgc-package` 阶段下载 `appimagelint`(github continuous release)经代理失败,**不影响可执行文件**。需 AppImage 时再解决该工具下载或跳过 lint。
- **GUI 实跑**:可执行文件已生成,实际运行需 X11(容器 `--env DISPLAY` + X socket 挂载,或宿主运行),属"运行验证"阶段,非本次编译目标。
- **从零重建成本**:docker build cache 可能被 BuildKit 回收,从零重建会重下 Qt(~43min)且 apt 装依赖可能遇代理 503;建议后续给容器 apt 配国内镜像。当前完整镜像 `qgc-ubuntu-docker`(含 Qt+依赖+ICU73)已就绪,增量重编(改 Swarm 代码)只需几分钟:
  ```bash
  cd <repo> && export http_proxy=http://127.0.0.1:8118 https_proxy=http://127.0.0.1:8118 \
    HTTP_PROXY=http://127.0.0.1:8118 HTTPS_PROXY=http://127.0.0.1:8118
  deploy/docker/_docker-exec.sh --fuse qgc-ubuntu-docker Debug
  ```
