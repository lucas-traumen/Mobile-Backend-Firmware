#!/usr/bin/env bash
# scripts/credentials-qr.sh — in QR username/password MQTT cho app mobile.
#
# App (settings-secrets-qr) chỉ nhận JSON thuần, không nhận khối text có IP.
# Payload schemaVersion 1, kind "credentials", hai dạng:
#   1) INFLUX_APP_TOKEN trống — chỉ MQTT:
#      {"schemaVersion":1,"kind":"credentials","mqttUsername":"...","mqttPassword":"..."}
#   2) INFLUX_APP_TOKEN khác rỗng — MQTT + influxToken (token giữ nguyên văn):
#      {"schemaVersion":1,"kind":"credentials","mqttUsername":"...","mqttPassword":"...","influxToken":"..."}
# Quét xong app điền draft rồi user tự bấm Lưu. Host/port lấy từ mDNS hoặc nhập tay.
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

# python3 json.dumps — password/token có dấu " / \ không làm vỡ JSON.
# Token truyền qua biến môi trường, không qua tham số dòng lệnh.
JSON="$(APP_USER="$APP_USER" APP_PASSWORD="$APP_PASSWORD" INFLUX_APP_TOKEN="$INFLUX_TOKEN" python3 - <<'PY'
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
print(json.dumps(payload, ensure_ascii=False, separators=(",", ":")))
PY
)"

printf '%s\n' "$JSON"

if command -v qrencode >/dev/null 2>&1; then
  echo
  printf '%s' "$JSON" | qrencode -t UTF8 -o -
else
  echo >&2
  echo "credentials-qr: máy chưa có qrencode — chỉ in JSON. Cài: sudo apt install qrencode" >&2
fi
