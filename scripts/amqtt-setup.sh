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
