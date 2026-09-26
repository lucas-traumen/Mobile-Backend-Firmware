#!/usr/bin/env bash
# scripts/board-qr.sh — sinh nhãn QR board cho app mobile (M18).
#
# Chạy trên host (thư mục nào cũng được):
#   bash scripts/board-qr.sh                          # tự dò MAC qua esptool
#   bash scripts/board-qr.sh -p /dev/ttyUSB0          # chỉ định cổng serial
#   bash scripts/board-qr.sh -m 5c:01:3b:6b:af:6c     # MAC cho sẵn (không cần board nối)
#   bash scripts/board-qr.sh --board-id 3b6baf6c      # id cho sẵn, bỏ qua derive
#   bash scripts/board-qr.sh -m 5c:01:3b:6b:af:6c -t IoT_ESP32-S2R3
#
# Từ M18, khi CONFIG_DEVICE_ID rỗng (default) firmware tự sinh boardId =
# hex-8 của 4 byte CUỐI MAC Wi-Fi STA lúc boot — cùng thuật toán
# board_id_from_mac() trong firmware/esp32-telemetry/main/board_id.c.
# MAC được đốt sẵn trong eFuse nên id unique theo board, ổn định qua
# reflash; script in lại JSON Device Info đúng shape để in nhãn QR dán
# lên board SAU khi flash.
#
# Cần môi trường ESP-IDF activated cho chế độ tự dò:
#   . ~/.espressif/tools/activate_idf_v6.0.1.sh
# (esptool in MAC eFuse base — trên ESP32 cấu hình mặc định, MAC Wi-Fi STA
# trùng base MAC, đúng nguồn firmware dùng: esp_read_mac ESP_MAC_WIFI_STA.)
#
# QR chứa JSON 3 field THUẦN, không bọc URL — không dùng QR generator kiểu
# "wifi:" hay link (tiền lệ bug me-qr, xem README mục 10.5).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

DEFAULT_BOARD_TYPE="IoT_ESP32-S2R3"
BOARD_TYPE="$DEFAULT_BOARD_TYPE"
PORT=""
MAC=""
BOARD_ID=""

MAC_RE='^([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$'
ID_RE='^[a-zA-Z0-9_-]+$'   # boardId phải là một segment topic hợp lệ

usage() {
  cat <<EOF
Cách dùng: bash scripts/board-qr.sh [-p PORT] [-m MAC] [--board-id ID] [-t BOARD_TYPE]
  (không tham số)       dò MAC qua esptool (cần ESP-IDF activated; port tự dò)
  -p /dev/ttyUSB0       cổng serial của board (tùy chọn)
  -m 5c:01:3b:6b:af:6c  MAC cho sẵn — không cần board nối
  --board-id 3b6baf6c   boardId cho sẵn — bỏ qua derive từ MAC
  -t IoT_ESP32-S2R3     boardType (mặc định: $DEFAULT_BOARD_TYPE)
  -h, --help            hướng dẫn
EOF
}

# Cảnh báo/thông tin ra stderr để stdout giữ sạch JSON + QR (dễ pipe/lưu file).
info()  { echo "board-qr: $*" >&2; }
die()   { echo "board-qr: LỖI: $*" >&2; exit 1; }

# --- Parse tham số -----------------------------------------------------------------
while [[ $# -gt 0 ]]; do
  case "$1" in
    -p|-m|-t|--board-id)
      [[ $# -ge 2 ]] || die "'$1' cần một giá trị (xem --help)."
      case "$1" in
        -p) PORT="$2" ;;
        -m) MAC="$2" ;;
        -t) BOARD_TYPE="$2" ;;
        --board-id) BOARD_ID="$2" ;;
      esac
      shift 2
      ;;
    -h|--help) usage; exit 0 ;;
    *) die "tham số không hiểu: '$1' (xem --help)." ;;
  esac
done

# --- Derive boardId từ MAC — CÙNG thuật toán firmware main/board_id.c (M18):
#     4 byte CUỐI của MAC, hex-8 lowercase (5c:01:3b:6b:af:6c -> "3b6baf6c").
derive_board_id() {
  local hex="${1//:/}"   # bỏ dấu ':'
  hex="${hex,,}"         # lowercase
  printf '%s' "${hex: -8}"   # 8 ký tự cuối = 4 byte cuối
}

if [[ -z "$BOARD_ID" ]]; then
  if [[ -z "$MAC" ]]; then
    # --- Chế độ tự dò: esptool đọc MAC trực tiếp từ board ------------------------
    command -v python >/dev/null 2>&1 || die "không có 'python' — kích hoạt ESP-IDF: . ~/.espressif/tools/activate_idf_v6.0.1.sh"
    info "đọc MAC qua esptool${PORT:+ (port $PORT)} — đảm bảo ESP-IDF đã activated và board nối cổng serial..."
    local_esptool_out=""
    if ! local_esptool_out="$(python -m esptool --chip esp32 read_mac ${PORT:+-p "$PORT"} 2>&1)"; then
      printf '%s\n' "$local_esptool_out" >&2
      die "esptool không đọc được MAC. Kiểm tra: board đã nối, port đúng (-p), ESP-IDF activated (. ~/.espressif/tools/activate_idf_v6.0.1.sh). Hoặc dùng -m MAC cho sẵn."
    fi
    MAC="$(awk '/^MAC:/ {print $2; exit}' <<<"$local_esptool_out")"
    [[ -n "$MAC" ]] || { printf '%s\n' "$local_esptool_out" >&2; die "không parse được dòng 'MAC:' từ output esptool."; }
  fi

  # --- Validate MAC trước khi xử lý --------------------------------------------
  if [[ ! "$MAC" =~ $MAC_RE ]]; then
    echo "board-qr: LỖI: MAC không đúng định dạng: '$MAC'" >&2
    echo "  Định dạng bắt buộc: 6 cặp hex cách nhau dấu ':' — ví dụ: 5c:01:3b:6b:af:6c" >&2
    echo "  (MAC lấy từ log boot firmware — dòng 'Wi-Fi STA MAC', hoặc từ esptool read_mac.)" >&2
    exit 1
  fi

  BOARD_ID="$(derive_board_id "$MAC")"
  info "MAC $MAC -> boardId '$BOARD_ID' (hex-8 của 4 byte cuối, cùng thuật toán firmware)"
else
  info "dùng boardId cho sẵn: '$BOARD_ID' (bỏ qua derive từ MAC)"
fi

# --- Validate boardId: một segment topic (backend chấp nhận [a-zA-Z0-9_-]+) --------
if [[ ! "$BOARD_ID" =~ $ID_RE ]]; then
  echo "board-qr: LỖI: boardId '$BOARD_ID' chứa ký tự không hợp lệ." >&2
  echo "  boardId phải khớp [a-zA-Z0-9_-]+ (chữ/số/_/-) — backend từ chối giá trị khác." >&2
  exit 1
fi

# --- In JSON + QR ra terminal --------------------------------------------------------
JSON="$(printf '{"schemaVersion":1,"boardId":"%s","boardType":"%s"}' "$BOARD_ID" "$BOARD_TYPE")"
echo "$JSON"

if command -v qrencode >/dev/null 2>&1; then
  echo
  printf '%s' "$JSON" | qrencode -t UTF8 -o -
else
  echo
  info "máy chưa có 'qrencode' — chỉ in JSON. Cài để có QR: sudo apt install qrencode"
fi
