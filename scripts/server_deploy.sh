#!/usr/bin/env bash
set -euo pipefail

# Legacy meowmaint must not publish application code or execute uploaded tests.
# Fail before packaging, SSH, SCP or invoking the installed remote entry point.
printf '%s\n' '旧维护账号部署已停用：不得通过 root 入口执行上传代码。请使用经过验收并另行授权的镜像发布流程。' >&2
exit 1
