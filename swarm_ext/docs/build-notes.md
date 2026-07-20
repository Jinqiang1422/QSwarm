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
