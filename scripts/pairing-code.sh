#!/usr/bin/env bash
# scripts/pairing-code.sh — in KHỐI mã ghép nối cho app mobile (M10).
#
# Chạy trên host (cùng thư mục repo):  bash scripts/pairing-code.sh
#
# Script đọc .env ở thư mục gốc repo (nếu có), tự detect IP LAN của máy chạy
# Docker, in broker WS + user/pass MQTT user `app` + Influx query read-only.
# Nếu máy có `qrencode` thì in thêm QR (ANSI) của cùng khối mã; không có thì chỉ
# in text — qrencode KHÔNG phải dependency bắt buộc.
#
# Bảo mật: token/password CHỈ xuất ra stdout, không ghi ra file nào.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${REPO_ROOT}/.env"

if [[ -f "$ENV_FILE" ]]; then
  # shellcheck disable=SC1090
  set -a
  # shellcheck source=/dev/null
  . "$ENV_FILE"
  set +a
else
  echo "Cảnh báo: không thấy ${ENV_FILE} — dùng giá trị mặc định/trống (xem .env.example)." >&2
fi

# --- Detect IP LAN: hostname -I, bỏ loopback/docker/tailscale/link-local --------
# Docker bridge  : 172.16.0.0/12  (172.16–172.31.x.x)
# Tailscale CGNAT: 100.64.0.0/10   (100.64–100.127.x.x)
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
  # Fallback: interface đầu tiên ngoài loopback nếu mọi IP đều bị lọc.
  ip -4 -o addr show scope global 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -n1
}

LAN_IP="$(detect_lan_ip)"
LAN_IP="${LAN_IP:-127.0.0.1}"

TOPIC_PREFIX="${TOPIC_PREFIX:-smarthome}"
INFLUX_ORG_VAL="${INFLUX_ORG:-smarthome}"
APP_USER="${MQTT_APP_USER:-app}"
APP_PASSWORD="${MQTT_APP_PASSWORD:-<chưa đặt — xem .env, hoặc docker compose logs mosquitto>}"
READ_TOKEN="${INFLUX_APP_TOKEN:-}"

if [[ -z "$READ_TOKEN" ]]; then
  READ_TOKEN_TEXT="<chưa có — xem hướng dẫn bên dưới>"
else
  READ_TOKEN_TEXT="$READ_TOKEN"
fi

BLOCK="==== SMART HOME PAIRING ====
Broker (MQTT over WebSocket): ws://${LAN_IP}:9001
MQTT user: ${APP_USER}
MQTT pass: ${APP_PASSWORD}
Influx query URL: http://${LAN_IP}:8086
Influx org: ${INFLUX_ORG_VAL}
Influx read token: ${READ_TOKEN_TEXT}
Topic prefix: ${TOPIC_PREFIX}
==========================="

printf '%s\n' "$BLOCK"

# QR (tuỳ chọn) — cùng nội dung khối trên, in ra stdout, không ghi file.
if command -v qrencode >/dev/null 2>&1; then
  echo
  if printf '%s\n' "$BLOCK" | qrencode -t ANSIUTF8 -o - 2>/dev/null; then
    :
  else
    echo "(qrencode có nhưng in QR thất bại — dùng text block ở trên.)" >&2
  fi
else
  echo
  echo "(Máy chưa có 'qrencode' — bỏ qua QR. Cài tuỳ chọn: sudo apt install qrencode)"
fi

# --- Hướng dẫn khi thiếu token read-only ---------------------------------------
if [[ -z "$READ_TOKEN" ]]; then
  cat <<EOF

--- Lấy Influx read token cho app ---
Đặt INFLUX_APP_TOKEN=<token> vào .env để lần sau script in sẵn (KHÔNG commit .env).
Tạo token read-only (user app-mobile đã có trên instance đang chạy):

  docker exec -it \$(docker compose ps -q influxdb) influx auth create \\
    --user app-mobile --read-bucket "\$INFLUX_BUCKET" \\
    --description "mobile app read-only"

Copy token in ra vào .env và chạy lại script này.
EOF
fi
