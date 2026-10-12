#!/usr/bin/env bash
# scripts/credentials-qr.sh — in QR username/password MQTT cho app mobile.
#
# App (settings-secrets-qr) chỉ nhận JSON thuần, không nhận khối text có IP.
# Payload schemaVersion 1, kind "credentials", hai dạng:
#   1) INFLUX_APP_TOKEN trống — chỉ MQTT:
#      {"schemaVersion":1,"kind":"credentials","mqttUsername":"...","mqttPassword":"..."}
#   2) INFLUX_APP_TOKEN khác rỗng — MQTT + influxToken (token giữ nguyên văn):
#      {"schemaVersion":1,"kind":"credentials","mqttUsername":"...","mqttPassword":"...","influxToken":"..."}
# Quét xong app điền draft rồi user tự bấm Lưu.
#
# Dạng payload đầy đủ (khi INFLUX_APP_TOKEN khác rỗng):
#   {"schemaVersion":1,"kind":"credentials","mqttUsername":"...","mqttPassword":"...",
#    "influxToken":"...","mqttPort":9001,"mqttPrefix":"smarthome",
#    "influxOrg":"smarthome","influxBucket":"telemetry"}
# M29: QR là mã chung ổn định vĩnh viễn — KHÔNG chứa địa chỉ. IP LAN là biến
# (đổi mạng/reboot là sai), nên không có mqttHost/influxUrl trong QR; app tự dò
# địa chỉ máy chủ qua mDNS (nút "Tìm máy chủ trong mạng") hoặc nhập tay fallback
# (emulator). QR chỉ mang thông tin đăng nhập + hằng cấu hình (port/prefix/org/bucket).
#
# Chạy:  bash scripts/credentials-qr.sh
# Secret chỉ ra stdout, không ghi file.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${REPO_ROOT}/.env"

if [[ -f "$ENV_FILE" ]]; then
  set -a
  # shellcheck disable=SC1090
  . "$ENV_FILE"
  set +a
else
  echo "credentials-qr: LỖI: không thấy ${ENV_FILE}" >&2
  exit 1
fi

APP_USER="${MQTT_APP_USER:-app}"
APP_PASSWORD="${MQTT_APP_PASSWORD:-}"
INFLUX_TOKEN="${INFLUX_APP_TOKEN:-}"

if [[ -z "$APP_PASSWORD" ]]; then
  echo "credentials-qr: LỖI: MQTT_APP_PASSWORD trống trong .env" >&2
  echo "  Nếu broker tự sinh: docker compose logs amqtt | grep MQTT_APP_PASSWORD" >&2
  echo "  Rồi ghi MQTT_APP_PASSWORD=... vào .env và chạy lại." >&2
  exit 1
fi

if [[ -z "$INFLUX_TOKEN" ]]; then
  echo "credentials-qr: LƯU Ý: INFLUX_APP_TOKEN trống — mã QR chỉ chứa thông tin MQTT, bỏ qua influxToken." >&2
fi

MQTT_PREFIX="${TOPIC_PREFIX:-smarthome}"
INFLUX_ORG_VAL="${INFLUX_ORG:-smarthome}"
INFLUX_BUCKET_VAL="${INFLUX_BUCKET:-telemetry}"

# python3 json.dumps — password/token có dấu " / \ không làm vỡ JSON.
# Token truyền qua biến môi trường, không qua tham số dòng lệnh.
JSON="$(APP_USER="$APP_USER" APP_PASSWORD="$APP_PASSWORD" INFLUX_APP_TOKEN="$INFLUX_TOKEN" \
  MQTT_PORT="9001" MQTT_PREFIX="$MQTT_PREFIX" \
  INFLUX_ORG="$INFLUX_ORG_VAL" INFLUX_BUCKET="$INFLUX_BUCKET_VAL" \
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
# Non-secret hằng cấu hình: chỉ thêm field khi giá trị không rỗng (app cũ zod-strip field lạ — an toàn).
# M29: KHÔNG có mqttHost/influxUrl — IP là biến, địa chỉ app tự dò qua mDNS hoặc nhập tay.
# mqttPort 9001 = port WebSocket MQTT của app, cùng giá trị service mDNS quảng bá.
payload["mqttPort"] = int(os.environ["MQTT_PORT"])
if os.environ.get("MQTT_PREFIX"):
    payload["mqttPrefix"] = os.environ["MQTT_PREFIX"]
if os.environ.get("INFLUX_ORG"):
    payload["influxOrg"] = os.environ["INFLUX_ORG"]
if os.environ.get("INFLUX_BUCKET"):
    payload["influxBucket"] = os.environ["INFLUX_BUCKET"]
print(json.dumps(payload, ensure_ascii=False, separators=(",", ":")))
PY
)"

printf '%s\n' "$JSON"

if command -v qrencode >/dev/null 2>&1; then
  echo
  if ! printf '%s' "$JSON" | qrencode -t UTF8 -o - 2>/dev/null; then
    echo "credentials-qr: in QR thất bại — dùng JSON ở trên." >&2
  fi
else
  echo >&2
  echo "credentials-qr: máy chưa có qrencode — chỉ in JSON. Cài: sudo apt install qrencode" >&2
fi
