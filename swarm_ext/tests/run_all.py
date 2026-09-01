#!/usr/bin/env python3
# ruff: noqa: RUF001, RUF002, RUF003, UP036
"""Layered test orchestrator for the QGroundControl Swarm extension."""

from __future__ import annotations

import argparse
import json
import os
import secrets
import shlex
import shutil
import subprocess
import sys
import time
from collections.abc import Callable
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SWARM_ROOT = REPO_ROOT / "swarm_ext"
PROFILE_LEVEL = {"smoke": 0, "standard": 1, "full": 2}
PASS = "PASS"
FAIL = "FAIL"
BLOCKED = "BLOCKED"
SKIP = "SKIP"
SENSITIVE_ARGUMENTS = {"--api-key", "--qgc-token"}


Prerequisite = Callable


@dataclass
class TestSpec:
    test_id: str
    layer: str
    description: str
    command: list[str]
    cwd: Path = REPO_ROOT
    timeout_sec: int = 300
    min_profile: str = "smoke"
    required: bool = True
    prerequisite: Prerequisite | None = None
    environment: dict[str, str] = field(default_factory=dict)


@dataclass
class TestResult:
    test_id: str
    layer: str
    description: str
    status: str
    required: bool
    duration_sec: float
    command: str
    log: str
    detail: str = ""


def _executable(name: str) -> Prerequisite:
    def check() -> tuple[bool, str]:
        path = shutil.which(name)
        return (bool(path), path or f"未找到可执行文件：{name}")

    return check


def _path_exists(path: Path, description: str) -> Prerequisite:
    def check() -> tuple[bool, str]:
        return (path.exists(), str(path) if path.exists() else f"缺少{description}：{path}")

    return check


def _all_prerequisites(*checks: Prerequisite) -> Prerequisite:
    def check_all() -> tuple[bool, str]:
        details = []
        for check in checks:
            ok, detail = check()
            details.append(detail)
            if not ok:
                return False, detail
        return True, "; ".join(details)

    return check_all


def _redacted_command(command: list[str]) -> str:
    redacted = list(command)
    for index, argument in enumerate(redacted[:-1]):
        if argument in SENSITIVE_ARGUMENTS:
            redacted[index + 1] = "<redacted>"
    return shlex.join(redacted)


def _docker_image(image: str) -> Prerequisite:
    def check() -> tuple[bool, str]:
        if not shutil.which("docker"):
            return False, "未找到 docker"
        result = subprocess.run(
            ["docker", "image", "inspect", image],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        return (result.returncode == 0, image if result.returncode == 0 else f"缺少镜像：{image}")

    return check


def _find_qt_root(explicit: str) -> Path | None:
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    for key in ("QGC_QT_ROOT", "QT_ROOT", "QT_DIR"):
        if os.environ.get(key):
            candidates.append(Path(os.environ[key]))
    qmake = shutil.which("qmake6")
    if qmake:
        result = subprocess.run(
            [qmake, "-query", "QT_INSTALL_PREFIX"],
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode == 0:
            candidates.append(Path(result.stdout.strip()))
    candidates.extend(
        [
            Path("/opt/Qt/6.11.1/gcc_64"),
            Path.home() / "Qt" / "6.11.1" / "gcc_64",
            Path("/usr"),
        ]
    )
    for candidate in candidates:
        if (candidate / "lib" / "cmake" / "Qt6" / "Qt6Config.cmake").exists():
            return candidate.resolve()
    return None


def _container_command(
    image: str,
    entrypoint: str,
    arguments: list[str],
    mounts: list[tuple[Path, str, bool]],
    workdir: str = "",
    environment: dict[str, str] | None = None,
    user: str = "",
) -> list[str]:
    command = ["docker", "run", "--rm"]
    for host_path, container_path, read_only in mounts:
        mount = f"{host_path.resolve()}:{container_path}"
        if read_only:
            mount += ":ro"
        command.extend(["-v", mount])
    if workdir:
        command.extend(["-w", workdir])
    if user:
        command.extend(["--user", user])
    for key, value in sorted((environment or {}).items()):
        command.extend(["-e", f"{key}={value}"])
    command.extend(["--entrypoint", entrypoint, image, *arguments])
    return command


def _source_paths(suffixes: set[str]) -> list[str]:
    return [
        str(path)
        for path in sorted(SWARM_ROOT.rglob("*"))
        if path.is_file() and path.suffix in suffixes and "__pycache__" not in path.parts
    ]


def _build_specs(args: argparse.Namespace, report_dir: Path) -> list[TestSpec]:
    python_files = _source_paths({".py"})
    shell_files = _source_paths({".sh"})
    cpp_files = [
        str(path)
        for path in sorted((REPO_ROOT / "src" / "Swarm").glob("*"))
        if path.suffix in {".cc", ".h"}
    ] + _source_paths({".cc", ".h"})
    qt_root = _find_qt_root(args.qt_root)
    cpp_build = report_dir / "cpp-build"
    if sys.version_info >= (3, 10):
        vehicle_null_check_command = [
            sys.executable,
            str(REPO_ROOT / "tools" / "analyzers" / "vehicle_null_check.py"),
            str(REPO_ROOT / "src" / "Swarm"),
        ]
        vehicle_null_check_prerequisite = None
    else:
        vehicle_null_check_command = _container_command(
            args.container_image,
            "/usr/bin/python3",
            [
                "/project/source/tools/analyzers/vehicle_null_check.py",
                "/project/source/src/Swarm",
            ],
            [(REPO_ROOT, "/project/source", True)],
            workdir="/project/source",
        )
        vehicle_null_check_prerequisite = _docker_image(args.container_image)
    specs = [
        TestSpec(
            "source.diff_check",
            "L0 源码契约",
            "Git 差异无空白和冲突标记错误",
            ["git", "diff", "--check"],
            prerequisite=_executable("git"),
        ),
        TestSpec(
            "source.asset_contracts",
            "L0 源码契约",
            "场景、模板、CMake、LLM 数据集与 MAVLink XML 单源一致",
            [
                sys.executable,
                str(SWARM_ROOT / "tests" / "validate_assets.py"),
                "--report",
                str(report_dir / "asset-contracts.json"),
            ],
        ),
        TestSpec(
            "source.python_compile",
            "L0 源码契约",
            "所有 Swarm Python 脚本可编译",
            [sys.executable, "-m", "py_compile", *python_files],
        ),
        TestSpec(
            "source.ruff",
            "L0 源码契约",
            "Python lint 通过",
            ["ruff", "check", *python_files],
            prerequisite=_executable("ruff"),
        ),
        TestSpec(
            "source.ruff_format",
            "L0 源码契约",
            "Python 格式检查通过",
            ["ruff", "format", "--check", *python_files],
            prerequisite=_executable("ruff"),
        ),
        TestSpec(
            "source.shellcheck",
            "L0 源码契约",
            "Shell 脚本静态检查通过",
            ["shellcheck", "-e", "SC1091", *shell_files],
            prerequisite=_executable("shellcheck"),
        ),
        TestSpec(
            "source.clang_format",
            "L0 源码契约",
            "Swarm C++ 格式检查通过",
            ["clang-format", "--dry-run", "--Werror", *cpp_files],
            prerequisite=_executable("clang-format"),
        ),
        TestSpec(
            "source.vehicle_null_check",
            "L0 源码契约",
            "Vehicle 指针使用符合 QGC 空指针规则",
            vehicle_null_check_command,
            prerequisite=vehicle_null_check_prerequisite,
        ),
        TestSpec(
            "python.acceptance_metrics",
            "L1 Python/算法",
            "编队几何、距离与 COP 真值匹配",
            [sys.executable, "-m", "unittest", "-v", "test_qgc_acceptance.py"],
            cwd=SWARM_ROOT / "sim",
        ),
        TestSpec(
            "python.perception",
            "L1 Python/算法",
            "感知编解码、几何、跟踪和离线端到端",
            [sys.executable, "test_perception.py"],
            cwd=SWARM_ROOT / "services" / "perception",
            timeout_sec=180,
        ),
        TestSpec(
            "python.llm_eval_sanity",
            "L1 Python/算法",
            "559 条 LLM 评测数据与工具 Schema 自检",
            [sys.executable, "run_eval.py", "--dry-run"],
            cwd=SWARM_ROOT / "tests" / "llm_eval",
        ),
        TestSpec(
            "sim.scenario_dry_run",
            "L2 场景解析",
            "全部 SITL/QGC 场景动作解析与报告编排",
            [
                sys.executable,
                "strict_regression.py",
                "--dry-run",
                "--out",
                str(report_dir / "scenario-dry-run"),
            ],
            cwd=SWARM_ROOT / "sim",
            timeout_sec=120,
        ),
    ]

    if qt_root:
        configure = [
            "cmake",
            "-S",
            str(SWARM_ROOT / "tests" / "cpp"),
            "-B",
            str(cpp_build),
            f"-DCMAKE_PREFIX_PATH={qt_root}",
            "-DCMAKE_BUILD_TYPE=Debug",
        ]
        build = ["cmake", "--build", str(cpp_build), "--parallel", str(args.jobs)]
        ctest = ["ctest", "--test-dir", str(cpp_build), "--output-on-failure"]
        cpp_prerequisite = _executable("cmake")
    else:
        mounts = [(REPO_ROOT, "/project/source", True), (report_dir, "/project/report", False)]
        configure = _container_command(
            args.container_image,
            "/usr/bin/cmake",
            [
                "-S",
                "/project/source/swarm_ext/tests/cpp",
                "-B",
                "/project/report/cpp-build",
                "-DCMAKE_PREFIX_PATH=/opt/Qt/6.11.1/gcc_64",
                "-DCMAKE_BUILD_TYPE=Debug",
            ],
            mounts,
        )
        build = _container_command(
            args.container_image,
            "/usr/bin/cmake",
            ["--build", "/project/report/cpp-build", "--parallel", str(args.jobs)],
            mounts,
        )
        ctest = _container_command(
            args.container_image,
            "/usr/bin/ctest",
            ["--test-dir", "/project/report/cpp-build", "--output-on-failure"],
            mounts,
        )
        cpp_prerequisite = _docker_image(args.container_image)

    specs.extend(
        [
            TestSpec(
                "cpp.configure",
                "L3 C++ 模块",
                "配置独立 Swarm C++ 测试工程",
                configure,
                timeout_sec=180,
                min_profile="standard",
                prerequisite=cpp_prerequisite,
            ),
            TestSpec(
                "cpp.build",
                "L3 C++ 模块",
                "编译 P1/P2/P3、战术、管线、租约、持久化和 COP 测试",
                build,
                timeout_sec=900,
                min_profile="standard",
                prerequisite=cpp_prerequisite,
            ),
            TestSpec(
                "cpp.ctest",
                "L3 C++ 模块",
                "执行全部独立 C++ 测试",
                ctest,
                timeout_sec=600,
                min_profile="standard",
                prerequisite=cpp_prerequisite,
            ),
        ]
    )

    build_dir = Path(args.build_dir).resolve()
    qgc_mounts = [(REPO_ROOT, "/project/source", False), (build_dir, "/project/build", False)]
    qgc_test_mounts = [(REPO_ROOT, "/project/source", True), (build_dir, "/project/build", False)]
    qgc_prerequisite = _all_prerequisites(
        _docker_image(args.container_image),
        _path_exists(build_dir / "CMakeCache.txt", "QGC CMakeCache"),
    )
    specs.extend(
        [
            TestSpec(
                "qgc.build",
                "L4 QGC 集成",
                "在原配置容器路径中增量构建测试版 QGC",
                _container_command(
                    args.container_image,
                    "/usr/bin/cmake",
                    [
                        "--build",
                        "/project/build",
                        "--config",
                        "Debug",
                        "--parallel",
                        str(args.jobs),
                    ],
                    qgc_mounts,
                    workdir="/project/source",
                ),
                timeout_sec=7200,
                min_profile="full",
                prerequisite=qgc_prerequisite,
            ),
            TestSpec(
                "qgc.ctest_unit",
                "L4 QGC 集成",
                "按 CI 入口并行执行 QGC Unit，排除 Flaky/Network",
                _container_command(
                    args.container_image,
                    "/usr/bin/python3",
                    [
                        "/project/source/.github/scripts/cmake_helper.py",
                        "ctest",
                        "--junit-output",
                        "/project/build/qgc-swarm-unit.xml",
                        "--ctest-output",
                        "/project/build/qgc-swarm-unit.txt",
                        "--jobs",
                        str(args.jobs),
                        "--include-labels",
                        "Unit",
                        "--exclude-labels",
                        "Flaky|Network",
                    ],
                    qgc_test_mounts,
                    workdir="/project/build",
                    user=f"{os.getuid()}:{os.getgid()}",
                ),
                timeout_sec=7200,
                min_profile="full",
                prerequisite=_all_prerequisites(
                    qgc_prerequisite,
                    _path_exists(build_dir / "Debug" / "QGroundControl", "QGC 测试可执行文件"),
                ),
            ),
            TestSpec(
                "qgc.ctest_integration",
                "L4 QGC 集成",
                "按 CI 入口串行执行 QGC Integration，排除 Flaky/Network",
                _container_command(
                    args.container_image,
                    "/usr/bin/python3",
                    [
                        "/project/source/.github/scripts/cmake_helper.py",
                        "ctest",
                        "--junit-output",
                        "/project/build/qgc-swarm-integration.xml",
                        "--ctest-output",
                        "/project/build/qgc-swarm-integration.txt",
                        "--jobs",
                        "1",
                        "--include-labels",
                        "Integration",
                        "--exclude-labels",
                        "Flaky|Network",
                    ],
                    qgc_test_mounts,
                    workdir="/project/build",
                    user=f"{os.getuid()}:{os.getgid()}",
                ),
                timeout_sec=7200,
                min_profile="full",
                prerequisite=_all_prerequisites(
                    qgc_prerequisite,
                    _path_exists(build_dir / "Debug" / "QGroundControl", "QGC 测试可执行文件"),
                ),
            ),
        ]
    )

    token = args.qgc_token or secrets.token_urlsafe(24)
    px4_source = Path(args.px4_src).expanduser().resolve()
    local_px4 = px4_source / "build" / "px4_sitl_default" / "bin" / "px4"
    local_gz_env = px4_source / "build" / "px4_sitl_default" / "rootfs" / "gz_env.sh"
    if args.sitl_backend == "auto":
        sitl_backend = "local" if local_px4.is_file() and local_gz_env.is_file() else "official"
    else:
        sitl_backend = args.sitl_backend
    sitl_checks = [
        _executable("docker"),
        _path_exists(px4_source, "PX4 源码目录"),
        _path_exists(build_dir / "Debug" / "QGroundControl", "QGC 测试可执行文件"),
    ]
    if sitl_backend == "local":
        sitl_checks.extend(
            [
                _path_exists(local_px4, "PX4 SITL 可执行文件"),
                _path_exists(local_gz_env, "PX4 gz 环境脚本"),
                _executable("gz"),
                _executable("Xvfb"),
            ]
        )
    sitl_prerequisite = _all_prerequisites(*sitl_checks)
    specs.append(
        TestSpec(
            "sitl.headless_acceptance",
            "L5 SITL/QGC 验收",
            "逐场景隔离执行覆盖规划、编队、安全故障、租约和任务编排",
            [
                sys.executable,
                "strict_regression.py",
                "--px4-src",
                str(px4_source),
                "--qgc-build",
                str(build_dir),
                "--qgc-token",
                token,
                "--sitl-backend",
                sitl_backend,
                "--only",
                "01,02,03,04,05,07,08",
                "--out",
                str(report_dir / "sitl-headless"),
            ],
            cwd=SWARM_ROOT / "sim",
            timeout_sec=14400,
            min_profile="full",
            prerequisite=sitl_prerequisite,
        )
    )
    external_qgc_ok = bool(args.external_qgc and args.qgc_token)
    specs.append(
        TestSpec(
            "sitl.qml_acceptance",
            "L6 GUI/QML 验收",
            "真实 GUI QGC 中验证 COP 模型、时延、地理误差和地图 delegate",
            [
                sys.executable,
                "strict_regression.py",
                "--only",
                "06",
                "--external-qgc",
                "--qgc-host",
                args.qgc_host,
                "--qgc-port",
                str(args.qgc_port),
                "--qgc-token",
                args.qgc_token,
                "--out",
                str(report_dir / "sitl-qml"),
            ],
            cwd=SWARM_ROOT / "sim",
            timeout_sec=1800,
            min_profile="full",
            prerequisite=lambda: (
                external_qgc_ok,
                "已配置外部 GUI QGC"
                if external_qgc_ok
                else "场景 06 需要 --external-qgc 和 --qgc-token",
            ),
        )
    )

    if args.with_llm:
        llm_ready = bool(args.llm_base_url and args.llm_api_key and args.llm_model)
        specs.append(
            TestSpec(
                "llm.live_eval",
                "L7 外部模型验收",
                "对 OpenAI 兼容模型运行完整自然语言指令评测集",
                [
                    sys.executable,
                    "run_eval.py",
                    "--base-url",
                    args.llm_base_url,
                    "--api-key",
                    args.llm_api_key,
                    "--model",
                    args.llm_model,
                    "--out",
                    str(report_dir / "llm-live.json"),
                ],
                cwd=SWARM_ROOT / "tests" / "llm_eval",
                timeout_sec=14400,
                min_profile="full",
                prerequisite=lambda: (
                    llm_ready,
                    "LLM 参数已配置" if llm_ready else "缺少 LLM base-url/api-key/model",
                ),
            )
        )
    return specs


def _run_spec(spec: TestSpec, logs_dir: Path) -> TestResult:
    log_path = logs_dir / f"{spec.test_id.replace('.', '_')}.log"
    started = time.monotonic()
    if spec.prerequisite:
        ok, detail = spec.prerequisite()
        if not ok:
            status = BLOCKED if spec.required else SKIP
            log_path.write_text(detail + "\n", encoding="utf-8")
            return TestResult(
                spec.test_id,
                spec.layer,
                spec.description,
                status,
                spec.required,
                0.0,
                _redacted_command(spec.command),
                str(log_path),
                detail,
            )
    environment = os.environ.copy()
    environment.update(spec.environment)
    try:
        result = subprocess.run(
            spec.command,
            cwd=spec.cwd,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
            timeout=spec.timeout_sec,
        )
        output = result.stdout or ""
        status = PASS if result.returncode == 0 else FAIL
        detail = "" if status == PASS else f"退出码 {result.returncode}"
    except subprocess.TimeoutExpired as exc:
        output = (exc.stdout or "") if isinstance(exc.stdout, str) else ""
        status = FAIL
        detail = f"超过 {spec.timeout_sec}s 超时"
    except OSError as exc:
        output = str(exc)
        status = BLOCKED
        detail = str(exc)
    log_path.write_text(output, encoding="utf-8")
    return TestResult(
        spec.test_id,
        spec.layer,
        spec.description,
        status,
        spec.required,
        round(time.monotonic() - started, 2),
        _redacted_command(spec.command),
        str(log_path),
        detail,
    )


def _write_reports(
    report_dir: Path, args: argparse.Namespace, results: list[TestResult], started_at: str
) -> None:
    counts = {
        status: sum(result.status == status for result in results)
        for status in (PASS, FAIL, BLOCKED, SKIP)
    }
    payload = {
        "version": 1,
        "profile": args.profile,
        "started_at": started_at,
        "finished_at": datetime.now(timezone.utc).isoformat(),
        "repository": str(REPO_ROOT),
        "counts": counts,
        "results": [asdict(result) for result in results],
    }
    (report_dir / "report.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    lines = [
        "# QGC Swarm 全面测试报告",
        "",
        f"- Profile: `{args.profile}`",
        f"- 结果：PASS {counts[PASS]} / FAIL {counts[FAIL]} / BLOCKED {counts[BLOCKED]} / SKIP {counts[SKIP]}",
        "",
        "| 层级 | 测试 | 结果 | 耗时(s) | 说明 |",
        "|---|---|---:|---:|---|",
    ]
    for result in results:
        detail = result.detail.replace("|", "\\|").replace("\n", " ")
        lines.append(
            f"| {result.layer} | `{result.test_id}` {result.description} | {result.status} | "
            f"{result.duration_sec:.2f} | {detail} |"
        )
    lines.extend(
        [
            "",
            "## 判定规则",
            "",
            "- `FAIL`：测试已运行但断言或命令失败。",
            "- `BLOCKED`：所选 profile 的必测项因环境或外部依赖未运行，不能宣称全面通过。",
            "- `SKIP`：显式可选项未启用。",
            "",
        ]
    )
    (report_dir / "SUMMARY.md").write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--profile", choices=sorted(PROFILE_LEVEL, key=PROFILE_LEVEL.get), default="standard"
    )
    parser.add_argument("--report-dir", default="")
    parser.add_argument("--build-dir", default=str(REPO_ROOT / "build"))
    parser.add_argument("--qt-root", default="")
    parser.add_argument("--container-image", default="qgc-ubuntu-docker:latest")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) // 2))
    parser.add_argument("--px4-src", default=str(Path.home() / "PX4-Autopilot"))
    parser.add_argument(
        "--sitl-backend",
        choices=("auto", "official", "local"),
        default="auto",
        help="SITL 后端；auto 优先复用已构建的本地 PX4/gz",
    )
    parser.add_argument("--qgc-host", default="127.0.0.1")
    parser.add_argument("--qgc-port", type=int, default=15610)
    parser.add_argument("--qgc-token", default=os.environ.get("QGC_SWARM_ACCEPTANCE_TOKEN", ""))
    parser.add_argument("--external-qgc", action="store_true")
    parser.add_argument("--with-llm", action="store_true")
    parser.add_argument("--llm-base-url", default=os.environ.get("SWARM_LLM_BASE_URL", ""))
    parser.add_argument("--llm-api-key", default=os.environ.get("SWARM_LLM_API_KEY", ""))
    parser.add_argument("--llm-model", default=os.environ.get("SWARM_LLM_MODEL", ""))
    parser.add_argument("--only", default="", help="逗号分隔测试 ID")
    parser.add_argument("--list", action="store_true", help="列出所选 profile 的测试而不执行")
    parser.add_argument("--allow-blocked", action="store_true")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs 必须大于 0")
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    report_dir = Path(args.report_dir or f"/tmp/qgc-swarm-tests-{timestamp}").resolve()
    report_dir.mkdir(parents=True, exist_ok=True)
    logs_dir = report_dir / "logs"
    logs_dir.mkdir(parents=True, exist_ok=True)
    specs = [
        spec
        for spec in _build_specs(args, report_dir)
        if PROFILE_LEVEL[spec.min_profile] <= PROFILE_LEVEL[args.profile]
    ]
    if args.only:
        selected = {item.strip() for item in args.only.split(",") if item.strip()}
        unknown = selected - {spec.test_id for spec in specs}
        if unknown:
            parser.error(f"未知或不属于当前 profile 的测试：{', '.join(sorted(unknown))}")
        specs = [spec for spec in specs if spec.test_id in selected]
    if args.list:
        for spec in specs:
            print(f"{spec.test_id:32} {spec.layer:16} {spec.description}")
        return 0

    started_at = datetime.now(timezone.utc).isoformat()
    results = []
    for index, spec in enumerate(specs, start=1):
        print(f"[{index}/{len(specs)}] {spec.test_id}: {spec.description}", flush=True)
        result = _run_spec(spec, logs_dir)
        results.append(result)
        suffix = f" — {result.detail}" if result.detail else ""
        print(f"  {result.status} ({result.duration_sec:.2f}s){suffix}", flush=True)
    _write_reports(report_dir, args, results, started_at)
    print(f"报告：{report_dir / 'SUMMARY.md'}")
    if any(result.status == FAIL and result.required for result in results):
        return 1
    if not args.allow_blocked and any(
        result.status == BLOCKED and result.required for result in results
    ):
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
