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
# Dạng payload đầy đủ (khi detect được IP LAN và các biến non-secret khác rỗng):
#   {"schemaVersion":1,"kind":"credentials","mqttUsername":"...","mqttPassword":"...",
#    "influxToken":"...","mqttHost":"192.168.x.x","mqttPort":9001,"mqttPrefix":"smarthome",
#    "influxUrl":"http://192.168.x.x:8086","influxOrg":"smarthome","influxBucket":"telemetry"}
# Các field non-secret (mqttHost/mqttPort/mqttPrefix/influxUrl/influxOrg/influxBucket)
# chỉ xuất hiện khi giá trị KHÔNG rỗng — app cũ (zod strip) bỏ qua, an toàn.
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

# Detect IP LAN — copy nguyên bản detect_lan_ip từ scripts/smarthome-deploy.sh
# (hostname -I, bỏ IPv6/loopback/link-local/docker/tailscale; cùng luật
# detect_lan_ip trong scripts/pairing-code.sh của repo). Không detect được
# (mạng lạ/WSL...) → mqttHost + influxUrl sẽ bị bỏ khỏi JSON.
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
LAN_IP="$(detect_lan_ip || true)"

MQTT_PREFIX="${TOPIC_PREFIX:-smarthome}"
INFLUX_ORG_VAL="${INFLUX_ORG:-smarthome}"
INFLUX_BUCKET_VAL="${INFLUX_BUCKET:-telemetry}"

# python3 json.dumps — password/token có dấu " / \ không làm vỡ JSON.
# Token truyền qua biến môi trường, không qua tham số dòng lệnh.
JSON="$(APP_USER="$APP_USER" APP_PASSWORD="$APP_PASSWORD" INFLUX_APP_TOKEN="$INFLUX_TOKEN" \
  MQTT_HOST="$LAN_IP" MQTT_PORT="9001" MQTT_PREFIX="$MQTT_PREFIX" \
  INFLUX_URL_IP="$LAN_IP" INFLUX_ORG="$INFLUX_ORG_VAL" INFLUX_BUCKET="$INFLUX_BUCKET_VAL" \
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
# Non-secret: chỉ thêm field khi giá trị không rỗng (app strip field lạ — an toàn).
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
