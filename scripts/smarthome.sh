#!/usr/bin/env bash
# scripts/smarthome.sh — entrypoint host cho server Smart Home (M24).
#
# Chạy trên host sau khi repo đã clone (thư mục nào cũng được):
#   bash scripts/smarthome.sh              # menu tương tác
#   bash scripts/smarthome.sh init         # direct mode
#   bash scripts/smarthome.sh board-qr -m 5c:01:3b:6b:af:6c
#
# Menu:
#   1) Init server mới  — tạo .env từ .env.example nếu thiếu (secret random bằng
#      openssl, file mode 600, temp file + atomic mv, KHÔNG ghi đè .env sẵn có),
#      validate compose, preload image (pull/build — KHÔNG up), rồi gọi
#      scripts/server-init.sh (avahi + compose up + healthy + verify mDNS).
#   2) QR credentials   — bash scripts/credentials-qr.sh (secret ra stdout).
#   3) QR board         — bash scripts/board-qr.sh; server nhúng thường không có
#      ESP-IDF/esptool nên menu hỏi boardId hoặc MAC, KHÔNG dò serial.
#   4) Khối mã ghép nối — bash scripts/pairing-code.sh.
#   5) Trạng thái       — docker compose ps + quét mDNS _smarthome._tcp.
#   0) Thoát.
#
# Direct mode: init|1, credentials-qr|2, board-qr|3, pairing-code|4, status|5,
# 0|exit. Args sau `board-qr` truyền nguyên vẹn cho board-qr.sh.
#
# Bảo mật: không eval, không source .env, secret không nằm trong argv/log;
# thiếu tool thì chỉ báo cách cài, KHÔNG tự apt/sudo cài package.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
ENV_FILE="${REPO_ROOT}/.env"
ENV_EXAMPLE="${REPO_ROOT}/.env.example"

BOARD_ID_RE='^[a-zA-Z0-9_-]+$'                       # một segment topic hợp lệ
MAC_RE='^([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$'

TMP_ENV=""      # temp .env đang tạo (cleanup trap xóa nếu init dở)
BQ_KIND=""      # 'board-id' | 'mac'
BQ_VALUE=""

cleanup() {
  if [[ -n "$TMP_ENV" ]]; then
    rm -f -- "$TMP_ENV"
    TMP_ENV=""
  fi
}
trap cleanup EXIT
trap 'echo; echo "smarthome: nhận Ctrl-C — thoát."; exit 130' INT
# SIGTERM (systemd stop, kill mặc định): thoát non-zero — EXIT trap phía trên
# vẫn chạy và xóa temp .env chứa secret nếu đang giữa chừng tạo file.
trap 'echo; echo "smarthome: nhận SIGTERM — dọn dẹp và thoát."; exit 143' TERM

die()  { echo "smarthome: LỖI: $*" >&2; exit 1; }
note() { echo "smarthome: $*"; }

require_helper() {
  local f="scripts/$1"
  if [[ ! -f "$f" ]]; then
    echo "smarthome: LỖI: không thấy ${REPO_ROOT}/${f} — repo clone thiếu file? Checkout lại đúng tag." >&2
    return 1
  fi
}

usage() {
  cat <<'USAGE'
Cách dùng: bash scripts/smarthome.sh [lệnh]
  (không lệnh)          mở menu tương tác
  init | 1              init server mới (.env + preload + server-init.sh)
  credentials-qr | 2    in QR credentials cho app mobile (chứa secret)
  board-qr | 3          nhãn QR board (args truyền thẳng cho board-qr.sh,
                        không có args thì hỏi boardId/MAC)
  pairing-code | 4      in khối mã ghép nối
  status | 5            docker compose ps + quét mDNS _smarthome._tcp
  exit | 0              thoát
USAGE
}

# --- Bước dựng .env lần đầu (chỉ gọi khi .env CHƯA tồn tại) -----------------------
create_env_from_example() {
  local tmp mqtt_pass app_pass influx_pass admin_token

  tmp="$(mktemp "${REPO_ROOT}/.env.XXXXXX")" || die "không tạo được temp file trong ${REPO_ROOT}."
  TMP_ENV="$tmp"
  # chmod 600 TRƯỚC khi ghi secret vào temp file.
  chmod 600 -- "$tmp" || die "không chmod 600 được temp file."

  note "sinh secret bằng 'openssl rand -hex' (không in giá trị ra màn hình)..."
  mqtt_pass="$(openssl rand -hex 16)"   || die "openssl rand thất bại."
  app_pass="$(openssl rand -hex 16)"    || die "openssl rand thất bại."
  influx_pass="$(openssl rand -hex 16)" || die "openssl rand thất bại."
  admin_token="$(openssl rand -hex 32)" || die "openssl rand thất bại."

  note "tạo .env từ .env.example (temp file cùng thư mục + atomic mv)..."
  # Secret truyền qua biến môi trường (không qua argv). Python thay ĐÚNG từng
  # dòng KEY=placeholder bằng literal (không regex, không eval/source), rồi tự
  # xác nhận: đủ 5 khóa, không còn placeholder nào, token admin đồng nhất.
  if MQTT_PASSWORD="$mqtt_pass" \
     MQTT_APP_PASSWORD="$app_pass" \
     INFLUXDB_INIT_PASSWORD="$influx_pass" \
     ADMIN_TOKEN="$admin_token" \
     python3 - "$ENV_EXAMPLE" "$tmp" <<'PY'
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
                    f"smarthome-init: '{key}' trong .env.example không còn là "
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
    sys.exit("smarthome-init: .env.example thiếu khóa bắt buộc: " + ", ".join(missing))

joined = "".join(out)
for ph in ("change-me-strong-password", "change-me-admin-password", "change-me-openssl-rand-hex-32"):
    if ph in joined:
        sys.exit("smarthome-init: vẫn còn placeholder sau khi thay — dừng.")

final = {}
for line in out:
    k, _, v = line.partition("=")
    final[k] = v.strip()
if final.get("INFLUXDB_INIT_ADMIN_TOKEN") != final.get("INFLUX_TOKEN"):
    sys.exit("smarthome-init: INFLUXDB_INIT_ADMIN_TOKEN và INFLUX_TOKEN không đồng nhất — dừng.")
for key in values:
    if not final.get(key):
        sys.exit(f"smarthome-init: '{key}' rỗng sau khi thay — dừng.")

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
  local mode
  mode="$(stat -c '%a' "$ENV_FILE" 2>/dev/null || true)"
  if [[ "$mode" != "600" ]]; then
    chmod 600 -- "$ENV_FILE" || die "không chmod 600 được .env."
    mode="$(stat -c '%a' "$ENV_FILE" 2>/dev/null || true)"
    [[ "$mode" == "600" ]] || die ".env cuối cùng không ở mode 600 (thấy: '${mode:-?}')."
  fi

  note "đã tạo .env (mode 600): MQTT_PASSWORD, MQTT_APP_PASSWORD, INFLUXDB_INIT_PASSWORD random;"
  note "một token random dùng chung cho INFLUXDB_INIT_ADMIN_TOKEN và INFLUX_TOKEN (bắt buộc đồng nhất)."
  note "INFLUX_APP_TOKEN (read-only cho app mobile) KHÔNG tự sinh — tạo tay rồi thêm vào .env (README mục 14.4)."
}

# --- Preload image: pull/build trước khi up, KHÔNG chạy 'up' ở bước này -----------
list_pullable_services() {
  # Service trong compose config có 'image:' mà không có 'build:' = pullable.
  docker compose config 2>/dev/null | awk '
    /^[^ \t#]/ { in_services = ($0 ~ /^services:[[:space:]]*$/); current = ""; next }
    in_services && /^  [A-Za-z0-9._-]+:[[:space:]]*$/ {
      svc = $0; sub(/^  /, "", svc); sub(/:.*/, "", svc); current = svc; next
    }
    in_services && current != "" && /^    / {
      if ($0 ~ /^    image:[[:space:]]*[^[:space:]]/) has_image[current] = 1
      else if ($0 ~ /^    build:/)                   has_build[current] = 1
    }
    END { for (s in has_image) if (!has_build[s]) print s }
  '
}

preload_images() {
  note "preload image (pull/build TRƯỚC khi up)..."
  if docker compose pull --help 2>&1 | grep -q -- --ignore-buildable; then
    note "docker compose pull --ignore-buildable..."
    docker compose pull --ignore-buildable || die "pull thất bại — kiểm tra mạng/registry rồi chạy lại; chưa gọi server-init."
  else
    note "Compose không hỗ trợ --ignore-buildable — pull từng service dùng image có sẵn..."
    local -a pullable=()
    mapfile -t pullable < <(list_pullable_services)
    if (( ${#pullable[@]} == 0 )); then
      note "không xác định được service pullable — bỏ qua pull (build --pull vẫn chạy)."
    else
      local svc
      for svc in "${pullable[@]}"; do
        note "docker compose pull ${svc}..."
        docker compose pull "$svc" || die "pull service '${svc}' thất bại — kiểm tra mạng rồi chạy lại; chưa gọi server-init."
      done
    fi
  fi
  note "docker compose build --pull (các service buildable + refresh base image)..."
  docker compose build --pull || die "build thất bại — KHÔNG gọi server-init."
}

# --- Action: init -----------------------------------------------------------------
do_init() {
  note "[init] kiểm tra điều kiện..."
  command -v docker >/dev/null 2>&1 \
    || die "chưa có 'docker' — cài Docker Engine + Compose v2 trên HOST theo hướng dẫn chính thức docs.docker.com."
  docker compose version >/dev/null 2>&1 \
    || die "'docker compose' không chạy được — cần Docker Compose v2 (plugin compose của Engine mới)."

  if [[ -f "$ENV_FILE" ]]; then
    note ".env đã tồn tại — GIỮ NGUYÊN, không ghi đè, không sinh lại secret."
    # Warn-only: chỉ grep -q (không in dòng nào của .env — không lộ giá trị).
    if grep -q 'change-me-' "$ENV_FILE"; then
      note "CẢNH BÁO: .env hiện có vẫn còn giá trị placeholder 'change-me-*' — compose sẽ chạy với secret mặc định."
      note "  Tự đổi các giá trị đó trong .env rồi chạy lại 'smarthome.sh init' (script không bao giờ ghi đè .env)."
    fi
  else
    command -v openssl >/dev/null 2>&1 \
      || die "chưa có 'openssl' — cần để sinh secret (cài: sudo apt-get install -y openssl)."
    command -v python3 >/dev/null 2>&1 \
      || die "chưa có 'python3' — cần để thay placeholder .env an toàn (cài: sudo apt-get install -y python3)."
    [[ -f "$ENV_EXAMPLE" ]] || die "không thấy ${ENV_EXAMPLE} — repo clone thiếu file?"
    create_env_from_example
  fi

  note "validate docker compose + .env (config --quiet)..."
  docker compose config --quiet \
    || die "docker compose config sai — kiểm tra docker-compose.yml / .env (giá trị thiếu/sai format)."

  preload_images

  # || return 1 tường minh: khi do_init được gọi từ menu (do_init || true) bash
  # TẮT errexit bên trong function — gọi trần sẽ vẫn chạy tiếp lệnh mù phía sau.
  require_helper "server-init.sh" || return 1
  note "[init] gọi scripts/server-init.sh (avahi + compose up -d --build + đợi healthy + verify mDNS)..."
  bash scripts/server-init.sh
}

# --- Action: wrappers giữ nguyên logic gốc ----------------------------------------
do_credentials_qr() {
  require_helper "credentials-qr.sh" || return 1
  # Secret (MQTT pass, influx token) in ra stdout/stderr do wrapper — người dùng
  # chủ động gọi mới thấy; script này không ghi secret vào đâu khác.
  bash scripts/credentials-qr.sh
}

do_pairing_code() {
  require_helper "pairing-code.sh" || return 1
  bash scripts/pairing-code.sh
}

# --- Action: board QR ---------------------------------------------------------------
call_board_qr_with_identity() {
  if [[ "$BQ_KIND" == "board-id" ]]; then
    bash scripts/board-qr.sh --board-id "$BQ_VALUE"
  else
    bash scripts/board-qr.sh -m "$BQ_VALUE"
  fi
}

# Hỏi boardId hoặc MAC. Trả 0 + set BQ_KIND/BQ_VALUE nếu nhập hợp lệ;
# trả 1 nếu user hủy (input trống); EOF thoát sạch cả script.
read_board_identity() {
  local reply="" val=""
  while true; do
    echo "board-qr: nhập định danh board (server không cần ESP-IDF/esptool):"
    echo "  1) boardId cho sẵn   (vd 3b6baf6c — hex-8 4 byte cuối MAC)"
    echo "  2) MAC Wi-Fi STA     (vd 5c:01:3b:6b:af:6c — script tự derive boardId)"
    echo "  (Enter trống = hủy)"
    if ! read -r -p "board-qr> " reply; then
      echo
      echo "smarthome: kết thúc input (EOF) — thoát."
      exit 0
    fi
    case "$reply" in
      1)
        if ! read -r -p "boardId: " val; then
          echo
          echo "smarthome: kết thúc input (EOF) — thoát."
          exit 0
        fi
        if [[ -z "$val" ]]; then
          echo "smarthome: bỏ trống — hủy."
          return 1
        fi
        if [[ "$val" =~ $BOARD_ID_RE && "$val" != -* ]]; then
          BQ_KIND="board-id"; BQ_VALUE="$val"
          return 0
        fi
        echo "smarthome: boardId phải khớp [a-zA-Z0-9_-]+ và không bắt đầu bằng '-' — thử lại."
        ;;
      2)
        if ! read -r -p "MAC: " val; then
          echo
          echo "smarthome: kết thúc input (EOF) — thoát."
          exit 0
        fi
        if [[ -z "$val" ]]; then
          echo "smarthome: bỏ trống — hủy."
          return 1
        fi
        if [[ "$val" =~ $MAC_RE ]]; then
          BQ_KIND="mac"; BQ_VALUE="$val"
          return 0
        fi
        echo "smarthome: MAC phải là 6 cặp hex cách nhau dấu ':' — thử lại."
        ;;
      "")
        echo "smarthome: hủy — không sinh QR board."
        return 1
        ;;
      *)
        echo "smarthome: lựa chọn không rõ: '${reply}' — thử lại."
        ;;
    esac
  done
}

# Menu mode: hủy chỉ quay lại menu. Direct mode (không args): hủy = thoát sạch.
interactive_board_qr() {
  require_helper "board-qr.sh" || return 1
  if read_board_identity; then
    call_board_qr_with_identity
  fi
}

cmd_board_qr() {
  require_helper "board-qr.sh" || return 1
  if (( $# > 0 )); then
    # Args truyền nguyên vẹn (đã quote) cho board-qr.sh — không xuyên qua eval.
    bash scripts/board-qr.sh "$@"
    return
  fi
  if read_board_identity; then
    call_board_qr_with_identity
  else
    # Direct mode: hủy = thoát sạch (0), không phải lỗi.
    echo "smarthome: đã hủy — không sinh QR board."
    return 0
  fi
}

# --- Action: status -----------------------------------------------------------------
# Chỉ đọc: KHÔNG source/đọc .env. Thiếu tool / compose chưa chạy chỉ WARN.
do_status() {
  if ! command -v docker >/dev/null 2>&1; then
    note "CẢNH BÁO: chưa có 'docker' — không xem được trạng thái stack."
    note "  Cài Docker Engine + Compose v2 theo hướng dẫn chính thức docs.docker.com."
    return 0
  fi
  if ! docker compose version >/dev/null 2>&1; then
    note "CẢNH BÁO: 'docker compose' không chạy được — cần Docker Compose v2."
    return 0
  fi

  # Phân biệt daemon với CLI: 'docker compose version' chỉ test CLI; daemon
  # xuống / user thiếu quyền thì 'docker info' fail — báo đúng nguyên nhân.
  local daemon_ok=1
  if ! docker info >/dev/null 2>&1; then
    daemon_ok=0
    note "CẢNH BÁO: Docker daemon không reachable — daemon chưa chạy hoặc user không có quyền docker."
    note "  Kiểm tra: sudo systemctl status docker; groups (cần group 'docker'; đăng nhập lại sau 'usermod -aG docker')."
  fi

  if (( daemon_ok )); then
    echo "== docker compose ps =="
    docker compose ps || true

    local -a running=()
    mapfile -t running < <(docker compose ps -q 2>/dev/null || true)
    if (( ${#running[@]} == 0 )); then
      note "CẢNH BÁO: chưa có container nào chạy — chạy 'bash scripts/smarthome.sh init'."
    fi
    echo
  fi
  if command -v avahi-browse >/dev/null 2>&1; then
    if command -v timeout >/dev/null 2>&1; then
      echo "== mDNS: quét _smarthome._tcp (tối đa 10s) =="
      timeout 10 avahi-browse -rt _smarthome._tcp || true
    else
      note "CẢNH BÁO: có 'avahi-browse' nhưng thiếu 'timeout' — bỏ qua quét mDNS."
    fi
  else
    note "CẢNH BÁO: chưa có 'avahi-browse' — bỏ qua kiểm tra mDNS. Cài: sudo apt-get install -y avahi-utils"
  fi
  return 0
}

# --- Menu ---------------------------------------------------------------------------
print_menu() {
  cat <<'MENU'

==== SMART HOME — MENU HOST ====
  1) Init server mới (.env nếu thiếu → pull/build → server-init)
  2) QR credentials cho app mobile (chứa secret — cẩn thận khi hiển thị)
  3) Nhãn QR board (nhập boardId hoặc MAC)
  4) Khối mã ghép nối
  5) Trạng thái (docker compose ps + mDNS)
  0) Thoát
================================
MENU
}

menu_loop() {
  local reply=""
  while true; do
    print_menu
    if ! read -r -p "Chọn [0-5]: " reply; then
      echo
      echo "smarthome: kết thúc input (EOF) — thoát."
      exit 0
    fi
    reply="${reply//[[:space:]]/}"
    case "$reply" in
      1|init)             do_init          || true ;;
      2|credentials-qr)   do_credentials_qr || true ;;
      3|board-qr)         interactive_board_qr || true ;;
      4|pairing-code)     do_pairing_code  || true ;;
      5|status)           do_status        || true ;;
      0|exit)             echo "smarthome: tạm biệt."; exit 0 ;;
      "")
        echo "smarthome: chưa chọn gì — quay lại menu."
        ;;
      *)
        echo "smarthome: lựa chọn không hợp lệ: '${reply}' — quay lại menu."
        ;;
    esac
    echo
  done
}

# --- Main ---------------------------------------------------------------------------
cd -- "$REPO_ROOT" || die "không cd được vào ${REPO_ROOT}."

if (( $# == 0 )); then
  menu_loop
else
  case "$1" in
    init|1)              do_init ;;
    credentials-qr|2)    do_credentials_qr ;;
    board-qr|3)          shift; cmd_board_qr "$@" ;;
    pairing-code|4)      do_pairing_code ;;
    status|5)            do_status ;;
    0|exit)              echo "smarthome: tạm biệt." ;;
    -h|--help|help)      usage ;;
    *)
      echo "smarthome: không hiểu lệnh: '$1'" >&2
      usage >&2
      exit 2
      ;;
  esac
fi
