#!/bin/sh
# Sinh password file cho Mosquitto từ MQTT_USER/MQTT_PASSWORD + user app riêng.
#
# Trong container: entrypoint của service mosquitto (docker-compose.yml) gọi
# script này trước khi start broker.
# Trên host:
#   MQTT_USER=esp32 MQTT_PASSWORD=... MQTT_APP_PASSWORD=... \
#     PASSWD_FILE=./passwd ./scripts/mosquitto-setup.sh
#
# Hai user trong CÙNG một passwd file (dùng chung cho cả listener 1883 và 9001):
#   - MQTT_USER (mặc định `esp32`)  : ESP32 + backend telemetry/bridge
#   - MQTT_APP_USER (mặc định `app`): app mobile, nối qua WebSocket 9001
# Nếu MQTT_APP_PASSWORD trống → script tự sinh random và IN RA LOG (đọc bằng
# `docker compose logs mosquitto`), đồng thời cảnh báo nên lưu vào .env.
#
# Lưu ý: -c tạo file MỚI mỗi lần chạy — passwd luôn khớp với env hiện tại,
# đổi mật khẩu trong .env rồi restart container là có hiệu lực. Chỉ dùng -c cho
# user đầu tiên, các user sau dùng -b (không -c) để không ghi đè file.
set -eu

: "${MQTT_USER:?MQTT_USER is required (đặt trong .env)}"
: "${MQTT_PASSWORD:?MQTT_PASSWORD is required (đặt trong .env)}"

PASSWD_FILE="${PASSWD_FILE:-/mosquitto/data/passwd}"
APP_USER="${MQTT_APP_USER:-app}"

# Sinh mật khẩu random khi .env không đặt MQTT_APP_PASSWORD.
# Ưu tiên openssl; image eclipse-mosquitto (alpine) không có openssl nên
# fallback lấy từ /dev/urandom bằng busybox tr (luôn có sẵn).
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

if ! command -v mosquitto_passwd >/dev/null 2>&1; then
  echo "mosquitto-setup: không tìm thấy mosquitto_passwd trong PATH (chạy trong container eclipse-mosquitto, hoặc cài package mosquitto trên host)" >&2
  exit 1
fi

mkdir -p "$(dirname "$PASSWD_FILE")"

# -b: mật khẩu truyền qua command line; -c: tạo mới passwd file (chỉ user đầu).
mosquitto_passwd -b -c "$PASSWD_FILE" "$MQTT_USER" "$MQTT_PASSWORD"
# Không -c: thêm user thứ hai vào file vừa tạo, không ghi đè user đầu.
mosquitto_passwd -b "$PASSWD_FILE" "$APP_USER" "$MQTT_APP_PASSWORD"

# File chỉ chứa hash (không plaintext) nhưng vẫn hạn chế quyền đọc
chmod 640 "$PASSWD_FILE"
if [ "$(id -u)" = "0" ] && id mosquitto >/dev/null 2>&1; then
  chown mosquitto:mosquitto "$PASSWD_FILE"
fi

echo "mosquitto-setup: passwd sẵn sàng tại ${PASSWD_FILE} (user: ${MQTT_USER}, ${APP_USER})"
if [ "$APP_PASSWORD_GENERATED" = "1" ]; then
  echo "mosquitto-setup: MQTT_APP_PASSWORD trống — đã sinh mật khẩu random cho user '${APP_USER}':"
  echo "mosquitto-setup:     ${MQTT_APP_PASSWORD}"
  echo "mosquitto-setup: hãy lưu giá trị này vào .env (MQTT_APP_PASSWORD=...) để ổn định qua các lần restart."
fi
