#!/usr/bin/env bash
# scripts/smarthome-deploy.sh — deployment IMAGE-ONLY cho server mới (M25).
#
# Script self-contained: server mới tinh KHÔNG cần clone repo, không cần
# source TypeScript/firmware, không Node/npm, không có thao tác build image
# nào. Stack chạy từ image đã build sẵn trên Docker Hub:
#   trilucas/app_smarthome:backend-latest   (linux/amd64)
#   trilucas/app_smarthome:amqtt-latest     (linux/amd64)
#   influxdb:2.7.10                          (upstream public)
#
# Cách dùng:
#   bash smarthome-deploy.sh                 # menu tương tác
#   bash smarthome-deploy.sh init            # cài server mới: bundle + .env + pull + mDNS + up
#   bash smarthome-deploy.sh update          # pull image mới + up -d (không đụng .env/volume)
#   bash smarthome-deploy.sh credentials-qr  # QR credentials cho app mobile (chứa secret)
#   bash smarthome-deploy.sh board-qr -m 5c:01:3b:6b:af:6c   # nhãn QR board
#   bash smarthome-deploy.sh pairing-code    # khối mã ghép nối
#   bash smarthome-deploy.sh status | logs | stop | start | help
#
# Script tự tạo runtime bundle trong $SMART_HOME_DIR (mặc định ~/smarthome):
#   docker-compose.yml (image-only), broker.yaml, helpers/amqtt-setup.sh,
#   helpers/amqtt-passwd.py, avahi/smarthome.service, .env (mode 600,
#   sinh lần đầu rồi KHÔNG BAO GIỜ ghi đè).
#
# Bảo mật: không eval, không tự apt/sudo cài package (chỉ in gợi ý), không
# git clone, không build image; secret sinh bằng openssl và chèn vào .env
# qua biến môi trường python3 (không argv, không in ra màn hình); QR in
# secret chỉ khi user chủ động gọi credentials-qr/pairing-code.
if [ -z "${BASH_VERSION:-}" ]; then
  echo "smarthome-deploy: hãy chạy bằng bash — bash smarthome-deploy.sh [lệnh]" >&2
  exit 1
fi
set -euo pipefail

# --- Image override (pin tag/digest) --------------------------------------------
# Script KHÔNG set/export default cho BACKEND_IMAGE/AMQTT_IMAGE/INFLUX_IMAGE —
# compose tự resolve theo precedence chuẩn: env của caller (nếu caller set) >
# biến cùng tên trong .env runtime > default `${VAR:-...}` đã viết sẵn trong
# file compose render. Nhờ vậy pin digest bằng cách thêm dòng KEY=... vào
# .env trong runtime dir có hiệu lực với cả script lẫn docker compose thủ công.

RUNTIME_DIR="${SMART_HOME_DIR:-$HOME/smarthome}"
COMPOSE_FILE="$RUNTIME_DIR/docker-compose.yml"
ENV_FILE="$RUNTIME_DIR/.env"
SERVICE_DEST="/etc/avahi/services/smarthome.service"
WAIT_TIMEOUT=120
WAIT_INTERVAL=3

BOARD_ID_RE='^[a-zA-Z0-9_-]+$'                       # một segment topic hợp lệ
MAC_RE='^([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$'
BOARD_TYPE_RE='^[A-Za-z0-9_.-]+$'                    # rỗng bị từ chối (không sinh JSON boardType:"")
DEFAULT_BOARD_TYPE="IoT_ESP32-S2R3"

TMP_ENV=""        # temp .env đang tạo (cleanup trap xóa nếu init dở)
TMP_TEMPLATE=""   # temp template .env (không chứa secret nhưng cũng dọn)

cleanup() {
  if [[ -n "$TMP_ENV" ]]; then
    rm -f -- "$TMP_ENV"
    TMP_ENV=""
  fi
  if [[ -n "$TMP_TEMPLATE" ]]; then
    rm -f -- "$TMP_TEMPLATE"
    TMP_TEMPLATE=""
  fi
}
trap cleanup EXIT
trap 'echo; echo "smarthome-deploy: nhận Ctrl-C — thoát."; exit 130' INT
# SIGTERM: thoát non-zero — EXIT trap vẫn chạy và xóa temp .env chứa secret
# nếu đang giữa chừng tạo file.
trap 'echo; echo "smarthome-deploy: nhận SIGTERM — dọn dẹp và thoát."; exit 143' TERM

die()  { echo "smarthome-deploy: LỖI: $*" >&2; exit 1; }
note() { echo "smarthome-deploy: $*"; }
warn() { echo "smarthome-deploy: CẢNH BÁO: $*" >&2; }

# docker compose luôn trỏ đúng runtime dir (project name = tên thư mục,
# mặc định "smarthome" — volume prefix smarthome_*).
compose_cmd() {
  docker compose --project-directory "$RUNTIME_DIR" --file "$COMPOSE_FILE" "$@"
}

require_compose_cli() {
  command -v docker >/dev/null 2>&1 \
    || die "chưa có 'docker' — cài Docker Engine + Compose v2 trên HOST theo hướng dẫn chính thức docs.docker.com."
  docker compose version >/dev/null 2>&1 \
    || die "'docker compose' không chạy được — cần Docker Compose v2 (plugin compose của Engine mới)."
}

# Detect IP LAN: hostname -I, bỏ IPv6/loopback/link-local/docker/tailscale
# (cùng luật detect_lan_ip trong scripts/pairing-code.sh của repo).
detect_lan_ip() {
  local ip
  for ip in $(hostname -I 2>/dev/null); do
    [[ "$ip" == *:* ]] && continue # bỏ IPv6
    [[ "$ip" == 127.* ]] && continue
    [[ "$ip" == 169.254.* ]] && continue
    if [[ "$ip" =~ ^172\.(1[6-9]|2[0-9]|3[01])\. ]]; then continue; fi
    if [[ "$ip" =~ ^100\.(6[4-9]|[7-9][0-9]|1[01][0-9]|12[0-7])\. ]]; then continue; fi
    echo "$ip"
    return 0
  done
  ip -4 -o addr show scope global 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -n1
}

# QR tuỳ chọn — payload truyền qua stdin, không qua argv.
qr_print() { # $1 = payload, $2 = kiểu qrencode (UTF8 | ANSIUTF8)
  if command -v qrencode >/dev/null 2>&1; then
    if printf '%s' "$1" | qrencode -t "$2" -o - 2>/dev/null; then
      :
    else
      warn "qrencode có nhưng in QR thất bại — dùng phần text/JSON phía trên."
    fi
  else
    warn "máy chưa có 'qrencode' — chỉ in text/JSON. Cài (tuỳ chọn): sudo apt install qrencode"
  fi
}

usage() {
  cat <<'USAGE'
Cách dùng: bash smarthome-deploy.sh [lệnh]
  (không lệnh)          mở menu tương tác
  init                  cài server mới: bundle + .env + pull + mDNS + up -d
  update                docker compose pull + up -d (không đụng .env/volume)
  credentials-qr        QR credentials cho app mobile (chứa secret)
  board-qr [args]       nhãn QR board: --board-id ID | -m MAC | -t TYPE | -p PORT
                        (không có args thì hỏi boardId/MAC; không dò serial)
  pairing-code          khối mã ghép nối cho app mobile
  status                docker compose ps + quét mDNS _smarthome._tcp
  logs [args]           docker compose logs --tail=100 (thêm -f để theo dõi)
  stop                  dừng stack (SIGINT qua stop_signal; giữ volume)
  start                 chạy lại stack (start; chưa tạo container thì up -d)
  help | --help         hướng dẫn

Biến môi trường:
  SMART_HOME_DIR        thư mục runtime (mặc định ~/smarthome)
  BACKEND_IMAGE         override image backend (mặc định trilucas/app_smarthome:backend-latest)
  AMQTT_IMAGE           override image amqtt (mặc định trilucas/app_smarthome:amqtt-latest)
  INFLUX_IMAGE          override image influxdb (mặc định influxdb:2.7.10)
                        Precedence: env khi gọi lệnh > dòng KEY=... trong .env
                        của runtime dir > default trong file compose render.
USAGE
}

# --- Ghi runtime bundle (idempotent: file bundle ghi đè an toàn mỗi init;      #
# --- RIÊNG .env KHÔNG BAO GIỜ ghi đè — xem create_env_file) -------------------
write_bundle() {
  mkdir -p "$RUNTIME_DIR/helpers" "$RUNTIME_DIR/avahi" \
    || die "không tạo được thư mục runtime ${RUNTIME_DIR}."

  # Compose IMAGE-ONLY: giống docker-compose.yml của repo nhưng thay build:
  # bằng image: từ Docker Hub. Contract giữ nguyên: ports 1883/9001/8086,
  # volumes amqtt-data/influxdb-data, healthcheck amqtt+influxdb,
  # stop_signal SIGINT cho amqtt (graceful shutdown), restart unless-stopped.
  # Image render qua interpolation ${...:-default} — precedence chuẩn compose:
  # env của caller > biến cùng tên trong .env runtime > default trong file này.
  cat > "$COMPOSE_FILE" <<'DEPLOY_COMPOSE_EOF'
# Smart Home stack — IMAGE-ONLY (sinh bởi smarthome-deploy.sh).
# Stack chạy từ image đã build sẵn trên Docker Hub; máy chạy file này KHÔNG
# cần source/Dockerfile. Đừng trộn với docker-compose.yml trong repo developer
# (bản đó dựng image từ source). Contract giữ nguyên bản repo: ports
# 1883/9001/8086, volume amqtt-data/influxdb-data, healthcheck amqtt+influxdb,
# stop_signal SIGINT cho amqtt, restart unless-stopped cho mọi service.
#
# Override image (pin tag/digest khi cần ổn định): đặt env BACKEND_IMAGE,
# AMQTT_IMAGE, INFLUX_IMAGE khi gọi docker compose / smarthome-deploy.sh, hoặc
# thêm dòng KEY=... vào .env của runtime dir.

services:
  amqtt:
    image: ${AMQTT_IMAGE:-trilucas/app_smarthome:amqtt-latest}
    restart: unless-stopped
    ports:
      - "1883:1883" # ESP32 nối broker qua IP LAN của server
      - "9001:9001" # app mobile nối MQTT over WebSocket (cùng auth với 1883)
    volumes:
      - amqtt-data:/app/data
      - ./broker.yaml:/app/conf/broker.yaml:ro
      # Sinh passwd lúc start từ env — mount ro vào đúng path mặc định mà
      # scripts/amqtt-setup.sh (nhúng trong bundle) expect.
      - ./helpers/amqtt-setup.sh:/usr/local/bin/amqtt-setup.sh:ro
      - ./helpers/amqtt-passwd.py:/usr/local/bin/amqtt-passwd.py:ro
    environment:
      # Secret không nằm trong image: passwd sinh lúc start từ .env
      # (xem helpers/amqtt-setup.sh). MQTT_APP_PASSWORD rỗng → script tự
      # sinh random và in ra log container (dùng cho user `app` của mobile).
      MQTT_USER: ${MQTT_USER:-}
      MQTT_PASSWORD: ${MQTT_PASSWORD:-}
      MQTT_APP_USER: ${MQTT_APP_USER:-app}
      MQTT_APP_PASSWORD: ${MQTT_APP_PASSWORD:-}
      PASSWD_FILE: /app/data/passwd
      PASSWD_HELPER: /usr/local/bin/amqtt-passwd.py
    # Sinh passwd từ env rồi exec broker — `exec` để amqtt trở thành PID 1.
    command: ["/bin/sh", "-c", "/usr/local/bin/amqtt-setup.sh && exec amqtt -c /app/conf/broker.yaml"]
    # amqtt 0.12 chỉ graceful-shutdown trên SIGINT — docker stop/restart/down
    # gửi thẳng đúng signal amqtt cần (chi tiết trong docker-compose.yml repo).
    stop_signal: SIGINT
    stop_grace_period: 15s
    healthcheck:
      # Image amqtt là python base, không có `nc`: gửi 1 packet MQTT CONNECT
      # rồi chờ CONNACK (byte đầu 0x20 = 32) — xác nhận cả tầng MQTT.
      test:
        - CMD-SHELL
        - >-
          python -c "import socket;s=socket.create_connection(('127.0.0.1',1883),3);s.sendall(b'\x10\x0e\x00\x04MQTT\x04\x02\x00<\x00\x02hc\xe0\x00');s.settimeout(3);assert s.recv(4)[0]==32"
      interval: 10s
      timeout: 5s
      retries: 5
      start_period: 10s

  influxdb:
    image: ${INFLUX_IMAGE:-influxdb:2.7.10}
    restart: unless-stopped
    ports:
      - "8086:8086" # query CLI/app mobile trên host: http://localhost:8086
    volumes:
      - influxdb-data:/var/lib/influxdb2
    environment:
      # Image influxdb bắt buộc prefix DOCKER_INFLUXDB_INIT_*; mode setup tạo
      # org/bucket/admin token ngay lần start đầu. Backend đọc bộ INFLUX_*
      # riêng trong .env — hai bộ trỏ cùng org/bucket/token.
      DOCKER_INFLUXDB_INIT_MODE: setup
      DOCKER_INFLUXDB_INIT_USERNAME: ${INFLUXDB_INIT_USERNAME:-}
      DOCKER_INFLUXDB_INIT_PASSWORD: ${INFLUXDB_INIT_PASSWORD:-}
      DOCKER_INFLUXDB_INIT_ORG: ${INFLUXDB_INIT_ORG:-}
      DOCKER_INFLUXDB_INIT_BUCKET: ${INFLUXDB_INIT_BUCKET:-}
      DOCKER_INFLUXDB_INIT_ADMIN_TOKEN: ${INFLUXDB_INIT_ADMIN_TOKEN:-}
    healthcheck:
      test: ["CMD-SHELL", "influx ping"]
      interval: 10s
      timeout: 5s
      retries: 5
      start_period: 10s

  backend:
    image: ${BACKEND_IMAGE:-trilucas/app_smarthome:backend-latest}
    restart: unless-stopped
    env_file:
      # .env bắt buộc khi chạy thật; required: false để compose config không
      # lỗi trước khi init kịp tạo .env
      - path: .env
        required: false
    depends_on:
      amqtt:
        condition: service_healthy
      influxdb:
        condition: service_healthy

volumes:
  amqtt-data:
  influxdb-data:
DEPLOY_COMPOSE_EOF

  # broker.yaml — nhúng NGUYÊN VĂN amqtt/broker.yaml của repo (giữ comment).
  cat > "$RUNTIME_DIR/broker.yaml" <<'DEPLOY_BROKER_EOF'
# Config amqtt 0.12.x cho broker production — service `amqtt` trong
# docker-compose.yml mount file này ro vào /app/conf/broker.yaml.
#
# Schema config 0.12.x là dataclass: key trong section `plugins` PHẢI snake_case
# khớp field của `Config` từng plugin (PluginManager hydrate bằng dacite strict,
# không chuyển kebab-case) — đã đọc source image 0.12.1 để chốt.
#
# Listener (port host giữ nguyên như stack cũ, ESP32/app không đổi cổng):
#   default (TCP 1883)   : ESP32 + backend
#   ws-mobile (WS 9001)  : app mobile
# Cấm anonymous: KHÔNG khai AnonymousAuthPlugin — chỉ FileAuthPlugin
#   (client anonymous → authenticate() trả None → broker CONNACK từ chối).
# Persistence: amqtt.contrib.persistence.SessionDBPlugin (sqlite qua restart).
listeners:
  # "default" là listener bắt buộc của schema (BrokerConfig.__post_init__ đọc
  # listeners["default"]); các listener khác kế thừa trường không đặt riêng.
  default:
    type: tcp
    bind: 0.0.0.0:1883
  ws-mobile:
    type: ws
    bind: 0.0.0.0:9001

plugins:
  # Log connect/disconnect. Value rỗng = dùng default config.
  amqtt.plugins.logging_amqtt.EventLoggerPlugin:

  # Auth bắt buộc — file `username:hash` mỗi dòng, hash argon2id (sha512-crypt
  # đã deprecated từ 0.12, Python 3.13 bỏ `crypt`). File sinh lúc start bởi
  # scripts/amqtt-setup.sh vào /app/data (volume, sống qua restart).
  amqtt.plugins.authentication.FileAuthPlugin:
    password_file: /app/data/passwd

  # Lưu session + retained vào sqlite — sống qua restart container.
  # connection: 4 dấu "/" (sqlite+aiosqlite:/// + /app/data/amqtt.db tuyệt đối).
  # clear_on_shutdown=false: retained phải sống qua restart — kể cả khi broker
  # bị tắt đột ngột (SIGKILL khi vượt stop_grace_period) thì dữ liệu cũng
  # không bị xoá.
  amqtt.contrib.persistence.SessionDBPlugin:
    connection: sqlite+aiosqlite:////app/data/amqtt.db
    clear_on_shutdown: false
DEPLOY_BROKER_EOF

  # helpers/amqtt-setup.sh — nhúng NGUYÊN VĂN scripts/amqtt-setup.sh của repo.
  # Đường dẫn mặc định khớp mount của compose image-only ở trên:
  #   PASSWD_FILE   = /app/data/passwd            (volume amqtt-data:/app/data)
  #   PASSWD_HELPER = /usr/local/bin/amqtt-passwd.py (mount helpers/amqtt-passwd.py)
  # → không cần sửa gì so với bản repo.
  cat > "$RUNTIME_DIR/helpers/amqtt-setup.sh" <<'DEPLOY_SETUP_EOF'
#!/bin/sh
# Sinh password file argon2 cho amqtt FileAuthPlugin.
#
# Trong container: command của service `amqtt` (docker-compose.yml) gọi script
# này trước khi `exec amqtt`.
# Trên host (cần python + argon2-cffi):
#   MQTT_USER=esp32 MQTT_PASSWORD=... MQTT_APP_PASSWORD=... \
#     PASSWD_FILE=./passwd PASSWD_HELPER=./amqtt-passwd.py ./scripts/amqtt-setup.sh
#
# Hai user trong CÙNG một passwd file (dùng chung cả listener TCP và WS):
#   - MQTT_USER (mặc định `esp32`)  : ESP32 + backend ingest telemetry
#   - MQTT_APP_USER (mặc định `app`): app mobile, nối qua WebSocket
# Nếu MQTT_APP_PASSWORD trống → sinh random và IN RA STDOUT (đọc bằng
# `docker compose logs amqtt`), đồng thời cảnh báo nên lưu vào .env.
#
# Ghi chú kỹ thuật (amqtt >= 0.12, xem amqtt/README.md):
#   - hash là argon2id (FileAuthPlugin xác minh qua pwdlib; sha512-crypt đã
#     deprecated — Python 3.13 bỏ module `crypt`);
#   - password KHÔNG truyền qua argv (lộ process list) mà pipeline vào stdin
#     của scripts/amqtt-passwd.py;
#   - File tạo MỚI mỗi lần chạy: passwd luôn khớp env hiện tại, đổi mật khẩu
#     trong .env rồi restart là hiệu lực.
set -eu

: "${MQTT_USER:?MQTT_USER is required (đặt trong .env)}"
: "${MQTT_PASSWORD:?MQTT_PASSWORD is required (đặt trong .env)}"

PASSWD_FILE="${PASSWD_FILE:-/app/data/passwd}"
APP_USER="${MQTT_APP_USER:-app}"
PASSWD_HELPER="${PASSWD_HELPER:-/usr/local/bin/amqtt-passwd.py}"

# Sinh mật khẩu random khi env không đặt MQTT_APP_PASSWORD.
# Ưu tiên openssl; fallback /dev/urandom bằng busybox/coreutils tr.
generate_password() {
  if command -v openssl >/dev/null 2>&1; then
    openssl rand -hex 24
    return 0
  fi
  LC_ALL=C tr -dc 'A-Za-z0-9' </dev/urandom | head -c 32
  printf '\n'
}

APP_PASSWORD_GENERATED=0
if [ -z "${MQTT_APP_PASSWORD:-}" ]; then
  MQTT_APP_PASSWORD="$(generate_password)"
  APP_PASSWORD_GENERATED=1
fi

# python (image amqtt) hoặc python3 (host)
if command -v python >/dev/null 2>&1; then
  PYTHON="$(command -v python)"
elif command -v python3 >/dev/null 2>&1; then
  PYTHON="$(command -v python3)"
else
  echo "amqtt-setup: không tìm thấy python trong PATH (cần để hash argon2)" >&2
  exit 1
fi

if [ ! -f "$PASSWD_HELPER" ]; then
  echo "amqtt-setup: không tìm thấy helper ${PASSWD_HELPER} (mount scripts/amqtt-passwd.py vào container)" >&2
  exit 1
fi

mkdir -p "$(dirname "$PASSWD_FILE")"

# Tạo file mới mỗi lần chạy — passwd luôn khớp env hiện tại.
: > "$PASSWD_FILE"
# Password pipeline qua stdin — KHÔNG nằm trong argv → không lộ vào process list/log.
printf '%s\n' "$MQTT_PASSWORD" | "$PYTHON" "$PASSWD_HELPER" "$MQTT_USER" >> "$PASSWD_FILE"
printf '%s\n' "$MQTT_APP_PASSWORD" | "$PYTHON" "$PASSWD_HELPER" "$APP_USER" >> "$PASSWD_FILE"

# File chỉ chứa hash argon2 (không plaintext) nhưng vẫn hạn chế quyền đọc.
chmod 640 "$PASSWD_FILE"

echo "amqtt-setup: passwd sẵn sàng tại ${PASSWD_FILE} (user: ${MQTT_USER}, ${APP_USER})"
if [ "$APP_PASSWORD_GENERATED" = "1" ]; then
  echo "amqtt-setup: MQTT_APP_PASSWORD trống — đã sinh mật khẩu random cho user '${APP_USER}':"
  echo "amqtt-setup:     ${MQTT_APP_PASSWORD}"
  echo "amqtt-setup: hãy lưu giá trị này vào .env (MQTT_APP_PASSWORD=...) để ổn định qua các lần restart."
fi
DEPLOY_SETUP_EOF
  chmod 755 "$RUNTIME_DIR/helpers/amqtt-setup.sh"

  # helpers/amqtt-passwd.py — nhúng NGUYÊN VĂN scripts/amqtt-passwd.py của repo.
  cat > "$RUNTIME_DIR/helpers/amqtt-passwd.py" <<'DEPLOY_PASSWD_EOF'
#!/usr/bin/env python3
"""Sinh một dòng `username:argon2hash` cho FileAuthPlugin của amqtt.

- hash là argon2id format PHC ($argon2id$v=19$...) — FileAuthPlugin của amqtt
  >= 0.12 xác minh argon2/bcrypt qua pwdlib; sha512-crypt đã deprecated
  (Python 3.13 bỏ module `crypt`);
- password đọc từ STDIN, KHÔNG truyền qua argv → không lộ vào process list/log.

Chạy trong container amqtt (có sẵn argon2-cffi) hoặc môi trường host có
argon2-cffi. Script bọc shell scripts/amqtt-setup.sh gọi helper này.

Ví dụ:
    printf '%s\n' "$MQTT_PASSWORD" | amqtt-passwd.py "$MQTT_USER" >> "$PASSWD_FILE"
"""

import sys

try:
    from argon2 import PasswordHasher
except ImportError:
    print("amqtt-passwd: thiếu argon2-cffi (pip install argon2-cffi)", file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: amqtt-passwd.py USERNAME < password", file=sys.stderr)
        return 2

    username = sys.argv[1]
    if not username:
        print("amqtt-passwd: username không được rỗng", file=sys.stderr)
        return 2
    if ":" in username:
        # File passwd định dạng `username:hash` — username chứa ':' phá vỡ format
        # (FileAuthPlugin split lần đầu gặp ':' maxsplit=1).
        print("amqtt-passwd: username không được chứa ':'", file=sys.stderr)
        return 2

    # readline: lấy đúng một dòng stdin; rstrip chỉ bỏ \n/\r cuối dòng,
    # giữ nguyên mọi ký tự khác của password (kể cả dấu cách đầu/cuối).
    password = sys.stdin.readline().rstrip("\r\n")
    if not password:
        print("amqtt-passwd: password rỗng (truyền qua stdin)", file=sys.stderr)
        return 1

    # Default argon2-cffi: argon2id, time_cost=3, memory_cost=64MiB,
    # parallelism=4, hash_len=32 — đúng format pwdlib.Argon2Hasher của
    # FileAuthPlugin xác minh được.
    print(f"{username}:{PasswordHasher().hash(password)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
DEPLOY_PASSWD_EOF
  chmod 755 "$RUNTIME_DIR/helpers/amqtt-passwd.py"

  # avahi/smarthome.service — nhúng NGUYÊN VĂN avahi/smarthome.service của repo
  # (contract mDNS: _smarthome._tcp port 9001 + TXT prefix/influx_port/org/bucket;
  # không secret qua mDNS). install_mdns_service sẽ sudo cp sang /etc/avahi/services/.
  cat > "$RUNTIME_DIR/avahi/smarthome.service" <<'DEPLOY_AVAHI_EOF'
<?xml version="1.0" standalone='no'?>
<!--
  TEMPLATE — KHÔNG sửa trực tiếp trên máy; file gốc nằm trong repo.
  scripts/server-init.sh sẽ `sudo cp` file này sang
  /etc/avahi/services/smarthome.service; avahi-daemon tự nhận file service
  mới trong /etc/avahi/services/ — KHÔNG cần restart daemon.

  Quảng bá mDNS/DNS-SD (M17b) để app mobile dò tìm server trong LAN:
    _smarthome._tcp port 9001 (MQTT WebSocket — app mobile nối thẳng)
  Type là contract riêng của app (custom type tránh đụng nhầm broker MQTT
  thường port 1883 khác cùng trên LAN) — hai bên khớp theo
  mdnsDiscoveryContract.ts bên repo Mobile_Frontend. TXT keys DÙNG GẠCH
  DƯỚI đúng contract app:
    TXT prefix=smarthome       (TOPIC_PREFIX mặc định của contract topic)
    TXT influx_port=8086       (InfluxDB HTTP — query Flux)
    TXT influx_org=smarthome
    TXT influx_bucket=telemetry
  Secret (MQTT pass, token Influx) KHÔNG đi qua mDNS — app vẫn nhập tay.
-->
<!DOCTYPE service-group SYSTEM "avahi-service.dtd">
<service-group>
  <name>Smart Home Server</name>
  <service>
    <type>_smarthome._tcp</type>
    <port>9001</port>
    <txt-record>prefix=smarthome</txt-record>
    <txt-record>influx_port=8086</txt-record>
    <txt-record>influx_org=smarthome</txt-record>
    <txt-record>influx_bucket=telemetry</txt-record>
  </service>
</service-group>
DEPLOY_AVAHI_EOF

  note "đã ghi bundle vào ${RUNTIME_DIR} (docker-compose.yml, broker.yaml, helpers/, avahi/)."
}

# --- Tạo .env lần đầu (CHỈ gọi khi .env CHƯA tồn tại; không bao giờ ghi đè) ---
create_env_file() {
  local tmp template mqtt_pass app_pass influx_pass admin_token mode

  template="$(mktemp "$RUNTIME_DIR/.env.template.XXXXXX")" \
    || die "không tạo được temp file trong ${RUNTIME_DIR}."
  TMP_TEMPLATE="$template"
  cat > "$template" <<'DEPLOY_ENVTEMPLATE_EOF'
# ===== Smart Home runtime config — sinh bởi smarthome-deploy.sh =====
# File này chứa secret: KHÔNG commit, KHÔNG copy vào repo, KHÔNG chia sẻ.
# Compose (interpolation) và backend container (env_file) đọc file này.

# --- MQTT (amqtt) ---
# User/pass dùng chung cho ESP32 và backend; passwd sinh lúc container start.
MQTT_USER=esp32
MQTT_PASSWORD=change-me-strong-password
# User riêng cho app mobile (listener WebSocket 9001)
MQTT_APP_USER=app
MQTT_APP_PASSWORD=
# Backend nối broker qua service name nội compose
MQTT_URL=mqtt://amqtt:1883
# Prefix contract boards ({prefix}/boards/{boardId}/...)
TOPIC_PREFIX=smarthome

# --- InfluxDB 2.7 local (image bắt buộc prefix INFLUXDB_INIT_* lần setup đầu) ---
INFLUXDB_INIT_USERNAME=admin
INFLUXDB_INIT_PASSWORD=change-me-admin-password
INFLUXDB_INIT_ORG=smarthome
INFLUXDB_INIT_BUCKET=telemetry
INFLUXDB_INIT_ADMIN_TOKEN=change-me-openssl-rand-hex-32

# --- Backend đọc InfluxDB (src/env.ts) — cùng org/bucket/token với bộ trên ---
INFLUX_URL=http://influxdb:8086
INFLUX_TOKEN=change-me-openssl-rand-hex-32
INFLUX_ORG=smarthome
INFLUX_BUCKET=telemetry

# Token read-only cho app mobile (INFLUX_APP_TOKEN) KHÔNG tự sinh — tạo sau
# khi InfluxDB chạy (hướng dẫn: bash smarthome-deploy.sh pairing-code khi
# chưa có token, hoặc README mục triển khai). Bỏ comment và điền token:
# INFLUX_APP_TOKEN=
DEPLOY_ENVTEMPLATE_EOF

  tmp="$(mktemp "$RUNTIME_DIR/.env.XXXXXX")" || die "không tạo được temp file trong ${RUNTIME_DIR}."
  TMP_ENV="$tmp"
  # chmod 600 TRƯỚC khi ghi secret vào temp file.
  chmod 600 -- "$tmp" || die "không chmod 600 được temp file."

  note "sinh secret bằng 'openssl rand -hex' (không in giá trị ra màn hình)..."
  mqtt_pass="$(openssl rand -hex 16)"   || die "openssl rand thất bại."
  app_pass="$(openssl rand -hex 16)"    || die "openssl rand thất bại."
  influx_pass="$(openssl rand -hex 16)" || die "openssl rand thất bại."
  admin_token="$(openssl rand -hex 32)" || die "openssl rand thất bại."

  note "thay secret vào template (python3 — secret qua biến môi trường, không argv)..."
  # Python thay ĐÚNG từng dòng KEY=placeholder bằng literal (không regex, không
  # eval/source), rồi tự xác nhận: đủ khóa, không còn placeholder nào, token
  # admin đồng nhất với INFLUX_TOKEN, MQTT_APP_PASSWORD đã có giá trị.
  if MQTT_PASSWORD="$mqtt_pass" \
     MQTT_APP_PASSWORD="$app_pass" \
     INFLUXDB_INIT_PASSWORD="$influx_pass" \
     ADMIN_TOKEN="$admin_token" \
     python3 - "$template" "$tmp" <<'PY'
import os
import sys

src, dst = sys.argv[1], sys.argv[2]
with open(src, "r", encoding="utf-8") as f:
    lines = f.readlines()

values = {
    "MQTT_PASSWORD": os.environ["MQTT_PASSWORD"],
    "MQTT_APP_PASSWORD": os.environ["MQTT_APP_PASSWORD"],
    "INFLUXDB_INIT_PASSWORD": os.environ["INFLUXDB_INIT_PASSWORD"],
    "INFLUXDB_INIT_ADMIN_TOKEN": os.environ["ADMIN_TOKEN"],
    "INFLUX_TOKEN": os.environ["ADMIN_TOKEN"],
}
placeholders = {
    "MQTT_PASSWORD": "change-me-strong-password",
    "MQTT_APP_PASSWORD": None,
    "INFLUXDB_INIT_PASSWORD": "change-me-admin-password",
    "INFLUXDB_INIT_ADMIN_TOKEN": "change-me-openssl-rand-hex-32",
    "INFLUX_TOKEN": "change-me-openssl-rand-hex-32",
}
allowed_old = {
    key: {""} if ph is None else {"", ph}
    for key, ph in placeholders.items()
}

seen = set()
out = []
for line in lines:
    replaced = False
    for key, newval in values.items():
        prefix = key + "="
        if line.startswith(prefix):
            old = line[len(prefix):].strip()
            if old not in allowed_old[key]:
                sys.exit(
                    f"smarthome-deploy: '{key}' trong template không còn là "
                    "placeholder/trống — không thay thế an toàn được; dừng."
                )
            out.append(f"{key}={newval}\n")
            seen.add(key)
            replaced = True
            break
    if not replaced:
        out.append(line)

missing = [k for k in values if k not in seen]
if missing:
    sys.exit("smarthome-deploy: template thiếu khóa bắt buộc: " + ", ".join(missing))

joined = "".join(out)
for ph in ("change-me-strong-password", "change-me-admin-password", "change-me-openssl-rand-hex-32"):
    if ph in joined:
        sys.exit("smarthome-deploy: vẫn còn placeholder sau khi thay — dừng.")

final = {}
for line in out:
    k, _, v = line.partition("=")
    final[k] = v.strip()
if final.get("INFLUXDB_INIT_ADMIN_TOKEN") != final.get("INFLUX_TOKEN"):
    sys.exit("smarthome-deploy: INFLUXDB_INIT_ADMIN_TOKEN và INFLUX_TOKEN không đồng nhất — dừng.")
for key in values:
    if not final.get(key):
        sys.exit(f"smarthome-deploy: '{key}' rỗng sau khi thay — dừng.")

# Khóa non-secret bắt buộc phải có sẵn trong template
for key in ("MQTT_USER", "MQTT_APP_USER", "MQTT_URL", "TOPIC_PREFIX",
            "INFLUXDB_INIT_USERNAME", "INFLUXDB_INIT_ORG", "INFLUXDB_INIT_BUCKET",
            "INFLUX_URL", "INFLUX_ORG", "INFLUX_BUCKET"):
    if key not in final or not final[key]:
        sys.exit(f"smarthome-deploy: template thiếu/rỗng khóa non-secret '{key}' — dừng.")

with open(dst, "w", encoding="utf-8") as f:
    f.writelines(out)
PY
  then
    :
  else
    die "thay placeholder .env thất bại (xem lỗi phía trên) — không có .env nào được tạo."
  fi

  # Người khác vừa tạo .env giữa chừng? Không ghi đè.
  [[ -e "$ENV_FILE" ]] && die ".env vừa xuất hiện giữa chừng — không ghi đè; xóa temp và dừng."

  mv -f -- "$tmp" "$ENV_FILE" || die "không mv được temp file → .env."
  TMP_ENV=""
  mode="$(stat -c '%a' "$ENV_FILE" 2>/dev/null || true)"
  if [[ "$mode" != "600" ]]; then
    chmod 600 -- "$ENV_FILE" || die "không chmod 600 được .env."
    mode="$(stat -c '%a' "$ENV_FILE" 2>/dev/null || true)"
    [[ "$mode" == "600" ]] || die ".env cuối cùng không ở mode 600 (thấy: '${mode:-?}')."
  fi

  note "đã tạo ${ENV_FILE} (mode 600): MQTT_PASSWORD, MQTT_APP_PASSWORD, INFLUXDB_INIT_PASSWORD random;"
  note "một token random dùng chung cho INFLUXDB_INIT_ADMIN_TOKEN và INFLUX_TOKEN (bắt buộc đồng nhất)."
  note "INFLUX_APP_TOKEN (read-only cho app mobile) KHÔNG tự sinh — tạo tay sau khi InfluxDB chạy rồi thêm vào .env."
}

# --- Preflight init ---------------------------------------------------------------
preflight() {
  require_compose_cli
  command -v openssl >/dev/null 2>&1 \
    || die "chưa có 'openssl' — cần để sinh secret (cài: sudo apt-get install -y openssl)."
  command -v python3 >/dev/null 2>&1 \
    || die "chưa có 'python3' — cần để thay placeholder .env an toàn (cài: sudo apt-get install -y python3)."

  local arch
  arch="$(uname -m 2>/dev/null || echo unknown)"
  if [[ "$arch" != "x86_64" ]]; then
    warn "uname -m = '${arch}' — image hiện tại chỉ linux/amd64 (x86-64): docker compose pull sẽ FAIL trên kiến trúc này (vd Raspberry Pi/ARM) tới khi có multi-arch build."
  fi
}

# --- mDNS: avahi-daemon active + sudo cp service file ------------------------------
install_mdns_service() {
  if ! command -v systemctl >/dev/null 2>&1; then
    die "không tìm thấy systemctl — server cần systemd + avahi-daemon (cài: sudo apt-get install -y avahi-daemon)."
  fi
  if ! systemctl is-active --quiet avahi-daemon; then
    note "avahi-daemon chưa active — thử 'sudo systemctl enable --now avahi-daemon'..."
    if ! sudo systemctl enable --now avahi-daemon; then
      die "không bật được avahi-daemon (cài: sudo apt-get install -y avahi-daemon rồi chạy lại init)."
    fi
  fi
  note "cài quảng bá mDNS — sudo cp ${RUNTIME_DIR}/avahi/smarthome.service → ${SERVICE_DEST}"
  sudo cp "$RUNTIME_DIR/avahi/smarthome.service" "$SERVICE_DEST" \
    || die "không copy được avahi service file vào ${SERVICE_DEST}."
  note "avahi-daemon tự nhận file service mới trong /etc/avahi/services/ — KHÔNG cần restart."
}

# --- Đợi healthy (pattern service_ok của scripts/server-init.sh) -------------------
service_ok() {
  local service="$1" cid state health
  cid="$(compose_cmd ps -q "$service" 2>/dev/null | head -n1 || true)"
  [[ -n "$cid" ]] || return 1
  state="$(docker inspect -f '{{.State.Status}}' "$cid" 2>/dev/null || true)"
  [[ "$state" == "running" ]] || return 1
  health="$(docker inspect -f '{{if .State.Health}}{{.State.Health.Status}}{{else}}running{{end}}' "$cid" 2>/dev/null || true)"
  [[ "$health" == "healthy" || "$health" == "running" ]]
}

all_ready() {
  service_ok amqtt && service_ok influxdb && service_ok backend
}

wait_healthy() {
  note "đợi container healthy (tối đa ${WAIT_TIMEOUT}s: amqtt + influxdb healthy, backend running)..."
  local deadline
  deadline=$(( $(date +%s) + WAIT_TIMEOUT ))
  while ! all_ready; do
    if (( $(date +%s) >= deadline )); then
      warn "hết ${WAIT_TIMEOUT}s mà chưa đủ điều kiện — trạng thái hiện tại:"
      compose_cmd ps || true
      warn "container vẫn có thể đang khởi động bình thường (pull chậm, InfluxDB setup lần đầu) — không abort; theo dõi thêm: bash smarthome-deploy.sh logs -f"
      return 0
    fi
    sleep "$WAIT_INTERVAL"
  done
  note "cả 3 service sẵn sàng (amqtt healthy, influxdb healthy, backend running)."
}

verify_mdns() {
  note "verify quảng bá mDNS (_smarthome._tcp)..."
  if ! command -v avahi-browse >/dev/null 2>&1; then
    warn "chưa có 'avahi-browse' — bỏ qua verify. Cài để verify: sudo apt-get install -y avahi-utils"
    return 0
  fi
  local out
  out="$(timeout 10 avahi-browse -rt _smarthome._tcp 2>&1 || true)"
  if printf '%s' "$out" | grep -q '_smarthome'; then
    note "đã thấy quảng bá _smarthome._tcp trên mạng:"
    printf '%s\n' "$out"
  else
    warn "10s chưa thấy _smarthome._tcp — avahi vừa nhận file service mới có thể cần vài giây; kiểm tra: journalctl -u avahi-daemon -n 20"
    printf '%s\n' "$out"
  fi
}

print_summary() {
  local lan_ip
  lan_ip="$(detect_lan_ip)"
  lan_ip="${lan_ip:-<không detect được — chạy 'hostname -I' xem>}"

  cat <<EOF

==== SMART HOME SERVER — KHỞI TẠO XONG ====
Thư mục runtime: ${RUNTIME_DIR} (compose image-only; quản thủ công: cd vào thư mục này rồi chạy docker compose ...)
Địa chỉ (IP LAN — DHCP có thể đổi; app dò tìm qua mDNS, không cần gõ tay):
  ${lan_ip}
Port:
  1883  MQTT thường     — board ESP32 + backend
  9001  MQTT WebSocket  — app mobile
  8086  InfluxDB HTTP   — query Flux (app + CLI)
Quảng bá mDNS: _smarthome._tcp port 9001 + TXT prefix=smarthome, influx_port=8086, influx_org=smarthome, influx_bucket=telemetry
Kiểm tra từ máy khác:  avahi-browse -rt _smarthome._tcp   (cần avahi-utils)
===========================================
EOF

  if systemctl is-active --quiet ufw 2>/dev/null; then
    note "ufw đang bật — mở 3 port cho LAN (app/board mới nối được):"
    echo "  sudo ufw allow 1883/tcp   # MQTT board + backend"
    echo "  sudo ufw allow 9001/tcp   # MQTT WebSocket app"
    echo "  sudo ufw allow 8086/tcp   # InfluxDB HTTP"
  fi

  note "nhớ: app/điện thoại phải nối CÙNG Wi-Fi với server; server không được sleep (sleep = mất với toàn mạng)."
  note "bước tiếp theo thường dùng: 'bash $(basename "$0") credentials-qr' (QR cho app), 'bash $(basename "$0") status'."
}

# --- Action: init -------------------------------------------------------------------
# Thứ tự bắt buộc: preflight → bundle + .env → config --quiet → pull → mDNS
# → up -d (KHÔNG có gì để build) → đợi healthy → verify mDNS → tóm tắt.
cmd_init() {
  note "[init] preflight: docker + compose v2 + openssl + python3 + kiến trúc CPU..."
  preflight

  note "[init] tạo runtime bundle trong ${RUNTIME_DIR} (file bundle ghi đè an toàn; .env chỉ tạo khi chưa có)..."
  write_bundle

  if [[ -f "$ENV_FILE" ]]; then
    note ".env đã tồn tại — GIỮ NGUYÊN, không ghi đè, không sinh lại secret."
    # Warn-only: chỉ grep -q (không in dòng nào của .env — không lộ giá trị).
    if grep -q 'change-me-' "$ENV_FILE"; then
      warn ".env hiện có vẫn còn giá trị placeholder 'change-me-*' — compose sẽ chạy với secret mặc định."
      warn "  Tự đổi các giá trị đó trong ${ENV_FILE} rồi chạy lại init (script không bao giờ ghi đè .env)."
    fi
  else
    create_env_file
  fi

  note "validate docker compose + .env (config --quiet)..."
  compose_cmd config --quiet \
    || die "docker compose config sai — kiểm tra ${COMPOSE_FILE} / ${ENV_FILE} (giá trị thiếu/sai format)."

  note "pull image TRƯỚC khi up (docker compose pull)..."
  compose_cmd pull || die "pull thất bại — kiểm tra mạng/registry (repo private thì 'docker login' trước) rồi chạy lại; chưa up gì cả."

  install_mdns_service

  note "docker compose up -d (image-only — không có gì để dựng image trên server)..."
  compose_cmd up -d

  wait_healthy
  verify_mdns
  print_summary
}

# --- Action: update ------------------------------------------------------------------
cmd_update() {
  require_compose_cli
  note "update: docker compose pull (giữ nguyên .env và volume)..."
  compose_cmd pull || die "pull thất bại — kiểm tra mạng/registry (repo private thì 'docker login' trước) rồi chạy lại."
  note "docker compose up -d (chỉ thay container bằng image mới — không down, không đụng volume)..."
  compose_cmd up -d
  note "update xong — .env và volume giữ nguyên. Update lớn: backup volume InfluxDB trước (README mục 'Backup volume InfluxDB')."
}

# --- Action: credentials-qr ------------------------------------------------------------
cmd_credentials_qr() {
  if [[ ! -f "$ENV_FILE" ]]; then
    die "không thấy ${ENV_FILE} — chạy 'bash smarthome-deploy.sh init' trước (script không đọc .env ở nơi khác)."
  fi
  # Toàn bộ phần source .env + sinh output chạy trong SUBSHELL: biến MQTT_*/
  # INFLUX_* không bị export vào shell chính — ở chế độ menu, action kế tiếp
  # (init/update/start qua compose) luôn thấy .env hiện tại, không giá trị stale.
  (
    # shellcheck disable=SC1090
    set -a
    . "$ENV_FILE"
    set +a

    app_user="${MQTT_APP_USER:-app}"
    app_pass="${MQTT_APP_PASSWORD:-}"
    influx_token="${INFLUX_APP_TOKEN:-}"

    if [[ -z "$app_pass" ]]; then
      die "MQTT_APP_PASSWORD trống trong ${ENV_FILE}.
  Nếu broker tự sinh: docker compose logs amqtt | grep MQTT_APP_PASSWORD
  Rồi ghi MQTT_APP_PASSWORD=... vào ${ENV_FILE} và chạy lại."
    fi

    if [[ -z "$influx_token" ]]; then
      note "INFLUX_APP_TOKEN trống — mã QR chỉ chứa thông tin MQTT, bỏ qua influxToken."
    fi

    lan_ip="$(detect_lan_ip || true)"
    mqtt_prefix="${TOPIC_PREFIX:-smarthome}"
    influx_org="${INFLUX_ORG:-smarthome}"
    influx_bucket="${INFLUX_BUCKET:-telemetry}"

    # python3 json.dumps — password/token có dấu " / \ không làm vỡ JSON.
    # Secret truyền qua biến môi trường, không qua tham số dòng lệnh.
    json="$(APP_USER="$app_user" APP_PASSWORD="$app_pass" INFLUX_APP_TOKEN="$influx_token" \
      MQTT_HOST="$lan_ip" MQTT_PORT="9001" MQTT_PREFIX="$mqtt_prefix" \
      INFLUX_URL_IP="$lan_ip" INFLUX_ORG="$influx_org" INFLUX_BUCKET="$influx_bucket" \
      python3 - <<'PY'
import json, os

payload = {
    "schemaVersion": 1,
    "kind": "credentials",
    "mqttUsername": os.environ["APP_USER"],
    "mqttPassword": os.environ["APP_PASSWORD"],
}
influx_token = os.environ.get("INFLUX_APP_TOKEN", "")
if influx_token:
    payload["influxToken"] = influx_token
# Non-secret: chỉ thêm field khi giá trị không rỗng (app cũ zod-strip field lạ — an toàn).
# mqttPort 9001 = port WebSocket MQTT của app, cùng giá trị service mDNS quảng bá.
if os.environ.get("MQTT_HOST"):
    payload["mqttHost"] = os.environ["MQTT_HOST"]
    payload["mqttPort"] = int(os.environ["MQTT_PORT"])
if os.environ.get("MQTT_PREFIX"):
    payload["mqttPrefix"] = os.environ["MQTT_PREFIX"]
if os.environ.get("INFLUX_URL_IP"):
    # 8086 = port HTTP InfluxDB, cùng giá trị TXT influx_port của mDNS.
    payload["influxUrl"] = f"http://{os.environ['INFLUX_URL_IP']}:8086"
if os.environ.get("INFLUX_ORG"):
    payload["influxOrg"] = os.environ["INFLUX_ORG"]
if os.environ.get("INFLUX_BUCKET"):
    payload["influxBucket"] = os.environ["INFLUX_BUCKET"]
print(json.dumps(payload, ensure_ascii=False, separators=(",", ":")))
PY
)"

    printf '%s\n' "$json"
    qr_print "$json" "UTF8"
  )
}

# --- Action: board-qr -----------------------------------------------------------------
# Self-contained (bundle không kèm scripts/board-qr.sh của repo — server deploy
# không có ESP-IDF/esptool nên cũng không dò serial được).
derive_board_id() {
  # Cùng thuật toán firmware main/board_id.c: 4 byte CUỐI MAC → hex-8 lowercase
  # (5c:01:3b:6b:af:6c -> "3b6baf6c").
  local hex="${1//:/}"   # bỏ dấu ':'
  hex="${hex,,}"         # lowercase
  printf '%s' "${hex: -8}"   # 8 ký tự cuối = 4 byte cuối
}

board_qr_usage() {
  cat <<'BQUSAGE'
Cách dùng: bash smarthome-deploy.sh board-qr [-m MAC] [--board-id ID] [-t BOARD_TYPE] [-p PORT]
  -m 5c:01:3b:6b:af:6c  MAC Wi-Fi STA cho sẵn — tự derive boardId (không cần board nối)
  --board-id 3b6baf6c   boardId cho sẵn — bỏ qua derive từ MAC
  -t IoT_ESP32-S2R3     boardType (mặc định: IoT_ESP32-S2R3)
  -p /dev/ttyUSB0       chỉ để tương thích cú pháp — server deploy không có esptool,
                        KHÔNG dò serial; dùng -m hoặc --board-id
  -h, --help            hướng dẫn
BQUSAGE
}

# Hỏi boardId hoặc MAC. Trả 0 + set BQ_KIND/BQ_VALUE nếu nhập hợp lệ;
# trả 1 nếu user hủy (input trống); EOF thoát sạch cả script.
read_board_identity() {
  local reply="" val=""
  while true; do
    echo "board-qr: nhập định danh board (server deploy không có esptool — không dò serial):"
    echo "  1) boardId cho sẵn   (vd 3b6baf6c — hex-8 4 byte cuối MAC)"
    echo "  2) MAC Wi-Fi STA     (vd 5c:01:3b:6b:af:6c — tự derive boardId)"
    echo "  (Enter trống = hủy)"
    if ! read -r -p "board-qr> " reply; then
      echo
      note "kết thúc input (EOF) — thoát."
      exit 0
    fi
    case "$reply" in
      1)
        if ! read -r -p "boardId: " val; then
          echo
          note "kết thúc input (EOF) — thoát."
          exit 0
        fi
        if [[ -z "$val" ]]; then
          note "bỏ trống — hủy."
          return 1
        fi
        if [[ "$val" =~ $BOARD_ID_RE && "$val" != -* ]]; then
          BQ_KIND="board-id"; BQ_VALUE="$val"
          return 0
        fi
        note "boardId phải khớp [a-zA-Z0-9_-]+ và không bắt đầu bằng '-' — thử lại."
        ;;
      2)
        if ! read -r -p "MAC: " val; then
          echo
          note "kết thúc input (EOF) — thoát."
          exit 0
        fi
        if [[ -z "$val" ]]; then
          note "bỏ trống — hủy."
          return 1
        fi
        if [[ "$val" =~ $MAC_RE ]]; then
          BQ_KIND="mac"; BQ_VALUE="$val"
          return 0
        fi
        note "MAC phải là 6 cặp hex cách nhau dấu ':' — thử lại."
        ;;
      "")
        note "hủy — không sinh QR board."
        return 1
        ;;
      *)
        note "lựa chọn không rõ: '${reply}' — thử lại."
        ;;
    esac
  done
}

cmd_board_qr() {
  local board_type="$DEFAULT_BOARD_TYPE" mac="" board_id="" port=""

  while [[ $# -gt 0 ]]; do
    case "$1" in
      -m|-t|-p|--board-id)
        [[ $# -ge 2 ]] || die "'$1' cần một giá trị (xem --help)."
        case "$1" in
          -m) mac="$2" ;;
          -t) board_type="$2" ;;
          -p) port="$2" ;;
          --board-id) board_id="$2" ;;
        esac
        shift 2
        ;;
      -h|--help) board_qr_usage; return 0 ;;
      *) die "tham số không hiểu: '$1' (xem --help)." ;;
    esac
  done

  if [[ -z "$board_id" ]]; then
    if [[ -n "$mac" ]]; then
      if [[ ! "$mac" =~ $MAC_RE ]]; then
        die "MAC không đúng định dạng: '$mac' — bắt buộc 6 cặp hex cách nhau ':' (vd 5c:01:3b:6b:af:6c). MAC lấy từ log boot firmware (dòng 'Wi-Fi STA MAC') hoặc esptool read_mac."
      fi
      board_id="$(derive_board_id "$mac")"
      note "MAC $mac -> boardId '$board_id' (hex-8 của 4 byte cuối, cùng thuật toán firmware)"
    elif [[ -n "$port" ]]; then
      die "server deploy không có esptool/ESP-IDF — không dò MAC qua serial (-p). Dùng -m MAC hoặc --board-id."
    else
      if read_board_identity; then
        if [[ "$BQ_KIND" == "board-id" ]]; then
          board_id="$BQ_VALUE"
        else
          mac="$BQ_VALUE"
          board_id="$(derive_board_id "$mac")"
          note "MAC $mac -> boardId '$board_id' (hex-8 của 4 byte cuối, cùng thuật toán firmware)"
        fi
      else
        # Direct mode: hủy = thoát sạch (0), không phải lỗi.
        note "đã hủy — không sinh QR board."
        return 0
      fi
    fi
  else
    note "dùng boardId cho sẵn: '$board_id' (bỏ qua derive từ MAC)"
  fi

  # Validate boardId: một segment topic (backend chấp nhận [a-zA-Z0-9_-]+,
  # không bắt đầu '-'). Validate boardType: bắt buộc khác rỗng rồi mới regex —
  # tránh sinh JSON/QR với "boardType":"".
  if [[ ! "$board_id" =~ $BOARD_ID_RE || "$board_id" == -* ]]; then
    die "boardId '$board_id' chứa ký tự không hợp lệ — phải khớp [a-zA-Z0-9_-]+ và không bắt đầu bằng '-' (backend từ chối giá trị khác)."
  fi
  if [[ -z "$board_type" ]]; then
    die "boardType không được rỗng — -t cần một giá trị (vd IoT_ESP32-S2R3) hoặc bỏ hẳn -t để dùng mặc định."
  fi
  if [[ ! "$board_type" =~ $BOARD_TYPE_RE ]]; then
    die "boardType '$board_type' chứa ký tự không hợp lệ (cho phép chữ/số/_/.-)."
  fi

  local json
  json="$(printf '{"schemaVersion":1,"boardId":"%s","boardType":"%s"}' "$board_id" "$board_type")"
  printf '%s\n' "$json"
  qr_print "$json" "UTF8"
}

# --- Action: pairing-code --------------------------------------------------------------
cmd_pairing_code() {
  # Toàn bộ phần source .env + sinh output chạy trong SUBSHELL — biến MQTT_*/
  # INFLUX_* không rò rỉ vào shell chính (xem ghi chú ở cmd_credentials_qr).
  (
    if [[ -f "$ENV_FILE" ]]; then
      # shellcheck disable=SC1090
      set -a
      . "$ENV_FILE"
      set +a
    else
      warn "không thấy ${ENV_FILE} — dùng giá trị mặc định/trống (chạy 'bash smarthome-deploy.sh init' để tạo)."
    fi

    lan_ip="$(detect_lan_ip)"
    lan_ip="${lan_ip:-127.0.0.1}"
    topic_prefix="${TOPIC_PREFIX:-smarthome}"
    influx_org="${INFLUX_ORG:-smarthome}"
    influx_bucket="${INFLUX_BUCKET:-telemetry}"
    app_user="${MQTT_APP_USER:-app}"
    app_pass="${MQTT_APP_PASSWORD:-<chưa đặt — xem hướng dẫn credentials-qr>}"
    read_token="${INFLUX_APP_TOKEN:-}"

    if [[ -z "$read_token" ]]; then
      read_token_text="<chưa có — xem hướng dẫn bên dưới>"
    else
      read_token_text="$read_token"
    fi

    block="==== SMART HOME PAIRING ====
Broker (MQTT over WebSocket): ws://${lan_ip}:9001
MQTT user: ${app_user}
MQTT pass: ${app_pass}
Influx query URL: http://${lan_ip}:8086
Influx org: ${influx_org}
Influx read token: ${read_token_text}
Topic prefix: ${topic_prefix}
==========================="

    printf '%s\n' "$block"
    qr_print "$block" "ANSIUTF8"

    # --- Hướng dẫn khi thiếu token read-only (KHÔNG dùng admin token cho app) ---
    if [[ -z "$read_token" ]]; then
      cat <<EOF

--- Lấy Influx read token cho app ---
Tạo token read-only (user app-mobile) rồi thêm vào .env để lần sau
credentials-qr/pairing-code in sẵn (KHÔNG dùng admin token cho app):

  cd "${RUNTIME_DIR}"
  docker exec -it \$(docker compose ps -q influxdb) influx auth create \\
    --user app-mobile --read-bucket "${influx_bucket}" \\
    --description "mobile app read-only"

Mở ${ENV_FILE} (mode 600), thêm dòng INFLUX_APP_TOKEN=<token> rồi chạy lại
'bash $(basename "$0") credentials-qr' — lúc này QR sẽ kèm influxToken.
EOF
    fi
  )
}

# --- Action: status ---------------------------------------------------------------------
# Chỉ đọc: KHÔNG đọc .env. Thiếu tool / daemon xuống / stack chưa chạy chỉ WARN, exit 0.
cmd_status() {
  if ! command -v docker >/dev/null 2>&1; then
    warn "chưa có 'docker' — không xem được trạng thái stack."
    warn "  Cài Docker Engine + Compose v2 theo hướng dẫn chính thức docs.docker.com."
    return 0
  fi
  if ! docker compose version >/dev/null 2>&1; then
    warn "'docker compose' không chạy được — cần Docker Compose v2."
    return 0
  fi

  # Phân biệt daemon với CLI: 'docker compose version' chỉ test CLI; daemon
  # xuống / user thiếu quyền thì 'docker info' fail — báo đúng nguyên nhân.
  local daemon_ok=1
  if ! docker info >/dev/null 2>&1; then
    daemon_ok=0
    warn "Docker daemon không reachable — daemon chưa chạy hoặc user không có quyền docker."
    warn "  Kiểm tra: sudo systemctl status docker; groups (cần group 'docker'; đăng nhập lại sau 'usermod -aG docker')."
  fi

  if (( daemon_ok )); then
    echo "== docker compose ps =="
    compose_cmd ps || true

    local -a running=()
    mapfile -t running < <(compose_cmd ps -q 2>/dev/null || true)
    if (( ${#running[@]} == 0 )); then
      warn "chưa có container nào chạy — chạy 'bash smarthome-deploy.sh init'."
    fi
    echo
  fi
  if command -v avahi-browse >/dev/null 2>&1; then
    if command -v timeout >/dev/null 2>&1; then
      echo "== mDNS: quét _smarthome._tcp (tối đa 10s) =="
      timeout 10 avahi-browse -rt _smarthome._tcp || true
    else
      warn "có 'avahi-browse' nhưng thiếu 'timeout' — bỏ qua quét mDNS."
    fi
  else
    warn "chưa có 'avahi-browse' — bỏ qua kiểm tra mDNS. Cài: sudo apt-get install -y avahi-utils"
  fi
  return 0
}

# --- Action: logs / stop / start ----------------------------------------------------------
cmd_logs() {
  require_compose_cli
  note "mẹo: theo dõi liên tục thêm cờ -f — bash smarthome-deploy.sh logs -f" >&2
  compose_cmd logs --tail=100 "$@"
}

cmd_stop() {
  require_compose_cli
  note "docker compose stop (amqtt nhận SIGINT qua stop_signal — tắt sạch; volume giữ nguyên)..."
  compose_cmd stop
  note "đã stop. TUYỆT ĐỐI không dùng 'down -v' — xóa volume là mất dữ liệu telemetry/session."
}

cmd_start() {
  require_compose_cli
  local ids
  ids="$(compose_cmd ps -q 2>/dev/null || true)"
  if [[ -z "${ids//[[:space:]]/}" ]]; then
    note "chưa có container nào được tạo — docker compose up -d..."
    compose_cmd up -d
  else
    note "docker compose start..."
    compose_cmd start
  fi
}

# --- Menu ----------------------------------------------------------------------------------
print_menu() {
  cat <<'MENU'

==== SMART HOME DEPLOY — MENU (image-only) ====
  1) init            cài server mới (bundle + .env + pull + mDNS + up)
  2) update          pull image mới + up -d (không đụng .env/volume)
  3) credentials-qr  QR credentials cho app mobile (chứa secret — cẩn thận khi hiển thị)
  4) board-qr        nhãn QR board (nhập boardId hoặc MAC)
  5) pairing-code    khối mã ghép nối
  6) status          docker compose ps + quét mDNS _smarthome._tcp
  7) logs            log 100 dòng gần nhất (thêm -f để theo dõi)
  8) stop            dừng stack (giữ volume)
  9) start           chạy lại stack
  0) Thoát
===============================================
MENU
}

menu_loop() {
  local reply=""
  while true; do
    print_menu
    if ! read -r -p "Chọn [0-9]: " reply; then
      echo
      note "kết thúc input (EOF) — thoát."
      exit 0
    fi
    reply="${reply//[[:space:]]/}"
    case "$reply" in
      1|init)             cmd_init           || true ;;
      2|update)           cmd_update         || true ;;
      3|credentials-qr)   cmd_credentials_qr || true ;;
      4|board-qr)         cmd_board_qr       || true ;;
      5|pairing-code)     cmd_pairing_code   || true ;;
      6|status)           cmd_status         || true ;;
      7|logs)             cmd_logs           || true ;;
      8|stop)             cmd_stop           || true ;;
      9|start)            cmd_start          || true ;;
      0|exit)             note "tạm biệt."; exit 0 ;;
      "")
        note "chưa chọn gì — quay lại menu."
        ;;
      *)
        note "lựa chọn không hợp lệ: '${reply}' — quay lại menu."
        ;;
    esac
    echo
  done
}

# --- Main ------------------------------------------------------------------------------------
if (( $# == 0 )); then
  menu_loop
else
  case "$1" in
    init)              cmd_init ;;
    update)            cmd_update ;;
    credentials-qr)    cmd_credentials_qr ;;
    board-qr)          shift; cmd_board_qr "$@" ;;
    pairing-code)      cmd_pairing_code ;;
    status)            cmd_status ;;
    logs)              shift; cmd_logs "$@" ;;
    stop)              cmd_stop ;;
    start)             cmd_start ;;
    help|-h|--help)    usage ;;
    0|exit)            note "tạm biệt." ;;
    *)
      echo "smarthome-deploy: không hiểu lệnh: '$1'" >&2
      usage >&2
      exit 2
      ;;
  esac
fi
