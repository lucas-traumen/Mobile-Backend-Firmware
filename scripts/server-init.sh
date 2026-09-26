#!/usr/bin/env bash
# scripts/server-init.sh — bootstrap server Smart Home (M17: mDNS discovery).
#
# Chạy trên host (thư mục nào cũng được):  bash scripts/server-init.sh
#
# Script idempotent — chạy lại vô hại (cp đè service file OK, compose up lại OK).
# Lần lượt:
#   1. Đảm bảo avahi-daemon active (chưa thì thử `sudo systemctl enable --now`).
#   2. Kiểm tra `.env` ở gốc repo (chỉ kiểm tra tồn tại — KHÔNG đọc/in secret).
#   3. `docker compose config --quiet` — validate compose + .env trước khi build.
#   4. `sudo cp avahi/smarthome.service /etc/avahi/services/` — avahi-daemon tự
#      nhận file service mới, KHÔNG cần restart.
#   5. `docker compose up -d --build` (3 service: amqtt, influxdb, backend).
#   6. Đợi container healthy tối đa 120 s — hết giờ chỉ in trạng thái + cảnh báo,
#      KHÔNG abort (build chậm / InfluxDB setup lần đầu vẫn đang khởi động).
#   7. Verify quảng bá bằng `avahi-browse -rt _smarthome._tcp`; thiếu avahi-utils thì
#      chỉ WARN + gợi ý cài, không fail script.
#   8. In tóm tắt địa chỉ + 3 port + gợi ý firewall.
#
# Script do USER chạy tương tác: có gọi `sudo` (bước 1 + 4). KHÔNG tự cài
# package nào — thiếu gì chỉ in gợi ý lệnh cài. KHÔNG in password/token/secret.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVICE_FILE="${REPO_ROOT}/avahi/smarthome.service"
SERVICE_DEST="/etc/avahi/services/smarthome.service"
WAIT_TIMEOUT=120
WAIT_INTERVAL=3

cd "${REPO_ROOT}"

# --- Bước 1: avahi-daemon phải active -------------------------------------------
if ! command -v systemctl >/dev/null 2>&1; then
  echo "server-init: LỖI: không tìm thấy systemctl — server cần systemd + avahi-daemon." >&2
  echo "  Cài avahi-daemon: sudo apt-get install -y avahi-daemon" >&2
  exit 1
fi
if ! systemctl is-active --quiet avahi-daemon; then
  echo "server-init: avahi-daemon chưa active — thử 'sudo systemctl enable --now avahi-daemon'..."
  if ! sudo systemctl enable --now avahi-daemon; then
    echo "server-init: LỖI: không bật được avahi-daemon." >&2
    echo "  Cài avahi-daemon: sudo apt-get install -y avahi-daemon rồi chạy lại script này." >&2
    exit 1
  fi
fi
echo "server-init: avahi-daemon active."

# --- Bước 2: .env phải tồn tại (chỉ kiểm tra, không đọc nội dung) -----------------
if [[ ! -f ".env" ]]; then
  echo "server-init: LỖI: không thấy ${REPO_ROOT}/.env — copy mẫu rồi điền giá trị thật:" >&2
  echo "  cp .env.example .env" >&2
  exit 1
fi

# --- Bước 3: validate compose + .env trước khi build ------------------------------
if ! docker compose version >/dev/null 2>&1; then
  echo "server-init: LỖI: không chạy được 'docker compose' — cần Docker Compose v2." >&2
  exit 1
fi
echo "server-init: validate docker compose + .env (config --quiet)..."
if ! docker compose config --quiet; then
  echo "server-init: LỖI: docker compose config sai — kiểm tra docker-compose.yml / .env (giá trị thiếu/sai format)." >&2
  exit 1
fi

# --- Bước 4: cài service file mDNS ------------------------------------------------
if [[ ! -f "$SERVICE_FILE" ]]; then
  echo "server-init: LỖI: không thấy template ${SERVICE_FILE} trong repo." >&2
  exit 1
fi
echo "server-init: cài quảng bá mDNS — sudo cp avahi/smarthome.service → ${SERVICE_DEST}"
sudo cp "$SERVICE_FILE" "$SERVICE_DEST"
echo "server-init: avahi-daemon tự nhận file service mới trong /etc/avahi/services/ — KHÔNG cần restart."

# --- Bước 5: dựng/đổi mới stack ----------------------------------------------------
echo "server-init: docker compose up -d --build (lần đầu build image có thể mất vài phút)..."
docker compose up -d --build

# --- Bước 6: đợi container healthy --------------------------------------------------
# amqtt + influxdb có healthcheck trong docker-compose.yml (đợi 'healthy');
# backend không có healthcheck — chỉ cần state 'running'.
service_ok() {
  local service="$1" cid state health
  cid="$(docker compose ps -q "$service" 2>/dev/null | head -n1 || true)"
  [[ -n "$cid" ]] || return 1
  state="$(docker inspect -f '{{.State.Status}}' "$cid" 2>/dev/null || true)"
  [[ "$state" == "running" ]] || return 1
  health="$(docker inspect -f '{{if .State.Health}}{{.State.Health.Status}}{{else}}running{{end}}' "$cid" 2>/dev/null || true)"
  [[ "$health" == "healthy" || "$health" == "running" ]]
}

all_ready() {
  service_ok amqtt && service_ok influxdb && service_ok backend
}

echo "server-init: đợi container healthy (tối đa ${WAIT_TIMEOUT}s: amqtt + influxdb healthy, backend running)..."
deadline=$(( $(date +%s) + WAIT_TIMEOUT ))
while ! all_ready; do
  if (( $(date +%s) >= deadline )); then
    echo "server-init: CẢNH BÁO: hết ${WAIT_TIMEOUT}s mà chưa đủ điều kiện — trạng thái hiện tại:" >&2
    docker compose ps || true
    echo "server-init: CẢNH BÁO: container vẫn có thể đang khởi động bình thường (build chậm, InfluxDB setup lần đầu) — không abort; theo dõi thêm: docker compose logs -f" >&2
    break
  fi
  sleep "$WAIT_INTERVAL"
done
if all_ready; then
  echo "server-init: cả 3 service sẵn sàng (amqtt healthy, influxdb healthy, backend running)."
fi

# --- Bước 7: verify quảng bá mDNS ----------------------------------------------------
echo "server-init: verify quảng bá mDNS (_smarthome._tcp)..."
BROWSE_OUT=""
if command -v avahi-browse >/dev/null 2>&1; then
  # -r: resolve (in hostname/IP + port), -t: dừng sau một vòng quét; chặn 10s.
  BROWSE_OUT="$(timeout 10 avahi-browse -rt _smarthome._tcp 2>&1 || true)"
  if printf '%s' "$BROWSE_OUT" | grep -q '_smarthome'; then
    echo "server-init: đã thấy quảng bá _smarthome._tcp trên mạng:"
    printf '%s\n' "$BROWSE_OUT"
  else
    echo "server-init: CẢNH BÁO: 10s chưa thấy _smarthome._tcp — avahi vừa nhận file service mới có thể cần vài giây; kiểm tra: journalctl -u avahi-daemon -n 20" >&2
    printf '%s\n' "$BROWSE_OUT"
  fi
else
  echo "server-init: CẢNH BÁO: chưa có 'avahi-browse' — bỏ qua bước verify quảng bá." >&2
  echo "  Cài để verify: sudo apt-get install -y avahi-utils" >&2
fi

# --- Bước 8: tóm tắt ------------------------------------------------------------------
# Detect IP Wi-Fi LAN giống pairing-code.sh: hostname -I, bỏ loopback/docker/tailscale.
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

LAN_IP="$(detect_lan_ip)"
LAN_IP="${LAN_IP:-<không detect được — chạy 'hostname -I' xem>}"

cat <<EOF

==== SMART HOME SERVER — KHỞI TẠO XONG ====
Địa chỉ (IP Wi-Fi LAN — DHCP có thể đổi; app dò tìm qua mDNS, không cần gõ tay):
  ${LAN_IP}
Port:
  1883  MQTT thường     — board ESP32 + backend
  9001  MQTT WebSocket  — app mobile
  8086  InfluxDB HTTP   — query Flux (app + CLI)
Quảng bá mDNS: _smarthome._tcp port 9001 (MQTT WebSocket) + TXT prefix=smarthome, influx_port=8086, influx_org=smarthome, influx_bucket=telemetry
Kiểm tra từ máy khác:  avahi-browse -rt _smarthome._tcp   (cần avahi-utils)
===========================================
EOF

if systemctl is-active --quiet ufw 2>/dev/null; then
  echo "server-init: ufw đang bật — mở 3 port cho LAN (app/board mới nối được):"
  echo "  sudo ufw allow 1883/tcp   # MQTT board + backend"
  echo "  sudo ufw allow 9001/tcp   # MQTT WebSocket app"
  echo "  sudo ufw allow 8086/tcp   # InfluxDB HTTP"
fi

echo "server-init: nhớ: app/điện thoại phải nối CÙNG Wi-Fi với server; laptop không được sleep khi demo (sleep = server mất với toàn mạng)."
