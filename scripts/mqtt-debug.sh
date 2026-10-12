#!/usr/bin/env bash
# M28 — bộ công cụ debug một cửa cho broker amqtt (chạy trên host, gọi docker compose).
#
#   tap      : xem dữ liệu MQTT đang chạy qua hệ thống (đọc topic trực tiếp)
#   events   : xem sự kiện broker (log container, mức INFO)
#   retained : xem trạng thái broker lưu trong sqlite (retained + session)
#
# Script chỉ ĐỌC (trừ tap là subscribe tạm với --clean-session, không để lại
# session rác trong sqlite của broker). Không restart/đụng container nào.
set -euo pipefail
cd "$(dirname "$0")/.."

usage() {
  cat <<'USAGE'
mqtt-debug.sh — công cụ debug broker amqtt (smart home)

Cách dùng: bash scripts/mqtt-debug.sh <lệnh> [đối số...]

  tap [filter...] [-n SỐ]   Xem dữ liệu MQTT đang chạy qua hệ thống (subscribe
                            rồi in từng message ra màn hình, mỗi dòng dạng
                            `topic  payload`; chỉ hiện message khớp filter).
                            Mặc định: smarthome/#
                            Ví dụ:
                              bash scripts/mqtt-debug.sh tap                    # mọi topic smarthome/#
                              bash scripts/mqtt-debug.sh tap smarthome/+/state  # chỉ state
                              bash scripts/mqtt-debug.sh tap -n 5 smarthome/#   # đọc 5 message rồi thoát
                            Thoát: Ctrl-C (khi không dùng -n).
  events [pattern]          Xem sự kiện broker (log container, chạy liên tục).
                            Ví dụ:
                              bash scripts/mqtt-debug.sh events
                              bash scripts/mqtt-debug.sh events 'connect|disconnect'
  retained                  Xem trạng thái broker lưu trong database:
                            danh sách topic retained (kèm kích thước dữ liệu,
                            KHÔNG hiện nội dung thô) và các client đang có
                            session lưu trong broker.
  help                      Hiện hướng dẫn này.

Chi tiết 3 lớp quan sát + bảng "triệu chứng → lệnh":
xem docs/developer-guide.md (mục "Công cụ debug amqtt").
USAGE
}

# Lấy MQTT_USER / MQTT_PASSWORD từ .env (nếu có) — không bao giờ in password.
read_env_credentials() {
  if [[ -f .env ]]; then
    # `|| true`: .env tồn tại nhưng thiếu dòng key → grep exit 1 + pipefail
    # làm script chết im lặng; giá trị rỗng sẽ rơi vào nhánh kiểm tra dưới.
    MQTT_USER="${MQTT_USER:-$(grep -E '^MQTT_USER=' .env | head -n1 | cut -d= -f2- || true)}"
    MQTT_PASSWORD="${MQTT_PASSWORD:-$(grep -E '^MQTT_PASSWORD=' .env | head -n1 | cut -d= -f2- || true)}"
  fi
  : "${MQTT_USER:=}"
  : "${MQTT_PASSWORD:=}"
  if [[ -z "$MQTT_USER" || -z "$MQTT_PASSWORD" ]]; then
    echo "LỖI: thiếu MQTT_USER hoặc MQTT_PASSWORD trong .env (không đọc password từ đâu khác)." >&2
    exit 1
  fi
  if ! command -v python3 >/dev/null 2>&1; then
    echo "LỖI: cần python3 trên máy host để URL-encode mật khẩu an toàn." >&2
    exit 1
  fi
  # URL-encode password — không bao giờ đặt thẳng password thô vào URL.
  local encoded
  encoded="$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1], safe=""))' "$MQTT_PASSWORD")"
  MQTT_URL="mqtt://${MQTT_USER}:${encoded}@localhost:1883"
}

cmd="${1:-help}"

case "$cmd" in
  tap)
    shift || true
    filters=()
    count_opt=()
    while [[ $# -gt 0 ]]; do
      if [[ "$1" == "-n" ]]; then
        # Validate: thiếu giá trị hoặc không phải số nguyên dương → lỗi rõ
        # thay vì "unbound variable" của bash / traceback ValueError python.
        if [[ $# -lt 2 || ! "$2" =~ ^[0-9]+$ || "$2" -eq 0 ]]; then
          echo "LỖI: -n cần một số nguyên dương (số message cần đọc), vd: tap -n 5 smarthome/#" >&2
          exit 1
        fi
        count_opt=(-n "$2")
        shift 2
      else
        filters+=("$1")
        shift
      fi
    done
    if [[ ${#filters[@]} -eq 0 ]]; then
      filters=("smarthome/#")
    fi
    read_env_credentials
    # Password KHÔNG qua argv (lộ process list): URL truyền bằng env-prefix —
    # biến môi trường của lệnh + `docker compose exec -e VAR` DẠNG KHÔNG kèm
    # `=value` (compose tự chuyển tiếp từ môi trường hiện tại). Dạng
    # `-e VAR=value` sẽ đưa value vào argv của process docker trên host.
    # Filters/-n là giá trị không nhạy cảm, truyền argv cho python bình thường.
    # Lọc client-side: broker amqtt 0.12.1 có hành vi replay retained cho
    # client mới theo filter của MỌI subscription đang tồn tại (kể cả session
    # khôi từ sqlite) ngay sau CONNACK — chi tiết trong docs/developer-guide.md.
    # Nên ta tự match wildcard MQTT và chỉ in message khớp filter user yêu cầu
    # (retained khớp filter vẫn in — có chủ đích), mỗi dòng dạng `topic  payload`.
    if [[ ${#count_opt[@]} -eq 0 ]]; then
      echo "(Đang lắng nghe — nhấn Ctrl-C để dừng)"
    fi
    AMQTT_DEBUG_URL="$MQTT_URL" docker compose exec -T -e AMQTT_DEBUG_URL amqtt python -u - \
      "${count_opt[@]}" "${filters[@]}" <<'PYEOF'
import asyncio
import os
import sys
from pathlib import Path

import amqtt.scripts
from amqtt.client import MQTTClient
from amqtt.utils import read_yaml_config

url = os.environ["AMQTT_DEBUG_URL"]

argv = sys.argv[1:]
max_count = None
filters = []
i = 0
while i < len(argv):
    if argv[i] == "-n":
        max_count = int(argv[i + 1])
        i += 2
    else:
        filters.append(argv[i])
        i += 1


def matches(topic: str, pattern: str) -> bool:
    """Wildcard MQTT: `+` = đúng 1 level, `#` = đuôi (chỉ ở level cuối)."""
    t = topic.split("/")
    p = pattern.split("/")
    if p and p[-1] == "#":
        return len(t) >= len(p) - 1 and all(
            pp == "+" or pp == tt for tt, pp in zip(t[: len(p) - 1], p)
        )
    if len(t) != len(p):
        return False
    return all(pp == "+" or pp == tt for tt, pp in zip(t, p))


async def main() -> None:
    # Config mặc định giống amqtt_sub (default_client.yaml trong image).
    config = read_yaml_config(str(Path(amqtt.scripts.__file__).parent / "default_client.yaml"))
    client = MQTTClient(config=config)
    try:
        await client.connect(uri=url, cleansession=True)  # = --clean-session
    except Exception as exc:
        print(
            "LỖI: không connect được broker (sai user/password, broker chưa chạy, "
            "hoặc mạng lỗi): %r" % (exc,),
            file=sys.stderr,
        )
        raise SystemExit(1)

    try:
        await client.subscribe([(f, 0) for f in filters])
        printed = 0
        while True:
            if max_count is not None and printed >= max_count:
                break
            message = await client.deliver_message()
            topic = message.topic if message else None
            data = message.publish_packet.data if message and message.publish_packet else b""
            if topic is None or not any(matches(topic, f) for f in filters):
                continue  # junk từ replay retained của broker — lọc client-side
            try:
                payload = data.decode("utf-8")
            except UnicodeDecodeError:
                payload = repr(data)
            print("%s  %s" % (topic, payload))
            printed += 1
    except (KeyboardInterrupt, asyncio.CancelledError):
        pass  # Ctrl-C — thoát sạch, không traceback
    finally:
        try:
            await client.disconnect()
        except Exception:
            pass


try:
    asyncio.run(main())
except KeyboardInterrupt:
    pass
PYEOF
    ;;
  events)
    shift || true
    if [[ $# -gt 0 ]]; then
      docker compose logs -f --timestamps amqtt | grep --line-buffered -E "$1"
    else
      docker compose logs -f --timestamps amqtt
    fi
    ;;
  retained)
    # Đọc sqlite broker READ-ONLY — không ghi gì vào database.
    docker compose exec -T amqtt python - <<'PYEOF'
import sqlite3
conn = sqlite3.connect("file:/app/data/amqtt.db?mode=ro", uri=True)
cur = conn.cursor()
# Lưu ý: schema sqlite thật của amqtt 0.12.1 dùng bảng `stored_messages` /
# `stored_sessions` (số nhiều, có gạch dưới) — đã kiểm tra trong container.
rows = cur.execute("SELECT topic, qos, length(data) FROM stored_messages ORDER BY topic").fetchall()
print("== Retained topics (%d) ==" % len(rows))
if rows:
    width = max(len(r[0]) for r in rows)
    for topic, qos, size in rows:
        print("  %-*s  qos=%d  %6d bytes" % (width, topic, qos, size))
else:
    print("  (không có topic retained nào)")
sessions = cur.execute("SELECT client_id FROM stored_sessions ORDER BY client_id").fetchall()
print("\n== Sessions (%d) ==" % len(sessions))
for (client_id,) in sessions:
    print("  " + client_id)
if not sessions:
    print("  (không có session nào)")
conn.close()
PYEOF
    ;;
  help|-h|--help|*)
    usage
    ;;
esac

