#!/usr/bin/env bash
set -euo pipefail

if (( $# == 0 )); then
  cat >&2 <<'EOF'
用法：scripts/server_maint.sh COMMAND [ARG...]

可用命令：
  status
  logs [1..500]
  memo DEVICE_ID RECORDING_ID
  pairing-code [60..3600]
  revoke-device DEVICE_ID
  list-backups
  restart
EOF
  exit 2
fi

case "$1" in
  deploy|rollback)
    printf '%s\n' '旧维护账号代码部署与回滚已停用；本地入口不会连接服务器。' >&2
    exit 1
    ;;
  status|logs|memo|pairing-code|revoke-device|list-backups|restart)
    ;;
  *)
    printf '%s\n' '不支持的维护命令。' >&2
    exit 2
    ;;
esac

project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
known_hosts="$project_root/private/server-access/known_hosts"
server_host="${VOICE_MEMO_MAINT_HOST:-meow-relay}"
server_user="meowmaint"

if [[ ! -f "$known_hosts" ]]; then
  printf '缺少服务器主机指纹：%s\n' "$known_hosts" >&2
  exit 1
fi

# SSH ultimately invokes a remote shell. Quote every fixed-wrapper argument before handing
# it to ssh so a recording or device identifier cannot turn into another shell command.
printf -v remote_command '%q ' \
  sudo -n /usr/local/sbin/meow-relay-maint "$@"

exec ssh \
  -o BatchMode=yes \
  -o ConnectTimeout=10 \
  -o UserKnownHostsFile="$known_hosts" \
  -o StrictHostKeyChecking=yes \
  "$server_user@$server_host" \
  "$remote_command"
