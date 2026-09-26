# Hướng dẫn kết nối app/web frontend — MQTT broker + InfluxDB

> Tài liệu bàn giao cho Mobile_Frontend (web mô phỏng + Android). Cập nhật theo
> trạng thái M22 (2026-09-24). Tham khảo contract đầy đủ: `README.md` mục 14,
> quyết định kiến trúc: `PROJECT_MEMORY.md`, kế hoạch: `PLAN.md`.

## 1. Kiến trúc tổng quan

```
ESP32 (firmware v2)
   │  MQTT TCP 1883 — contract {prefix}/boards/{boardId}/...
   ▼
amqtt broker ──── WebSocket 9001 ◄──── App / Web (realtime — cùng contract boards)
   │
   └── Backend (ingest): telemetry v2 → InfluxDB ──── HTTP 8086 ◄──── App (history)
```

- **Realtime** (giá trị hiện tại, trạng thái board/relay): MQTT over WebSocket, port **9001** — app subscribe/publish cùng contract boards mà firmware dùng.
- **History** (biểu đồ, truy vấn quá khứ): query **trực tiếp InfluxDB** qua HTTP `POST /api/v2/query` — không qua backend.

## 2. Thông số kết nối theo môi trường

### 2.1. Web mô phỏng — `npm start` trên CHÍNH máy chạy Docker

| Tham số | Giá trị |
|---|---|
| MQTT URL | `ws://localhost:9001` |
| Influx URL | `http://localhost:8086` |

### 2.2. Android emulator (AVD)

| Tham số | Giá trị |
|---|---|
| MQTT URL | `ws://10.0.2.2:9001` |
| Influx URL | `http://10.0.2.2:8086` |

`10.0.2.2` là alias của máy host trong Android emulator — KHÔNG dùng được trên máy thật.

### 2.3. Android/iOS máy thật trong LAN

| Tham số | Giá trị |
|---|---|
| MQTT URL | `ws://<IP-LAN-máy-chạy-Docker>:9001` (vd `ws://192.168.2.34:9001`) |
| Influx URL | `http://<IP-LAN-máy-chạy-Docker>:8086` |

Điện thoại phải cùng subnet với máy Docker (vd 192.168.2.x). Toàn bộ plaintext `ws://`/`http://` — chỉ dùng trong LAN, chưa có TLS.

> **Điền vào app mobile — host và port là 2 trường riêng.** Các bảng mục 2.1–2.3 liệt kê URL
> đầy đủ (`ws://host:port`) để tham chiếu và test từ browser; trong app thì điền tách biệt:
>
> - **"Địa chỉ máy chủ" (host):** CHỈ hostname hoặc IP, KHÔNG kèm `ws://` — vd `localhost`, `10.0.2.2`, `192.168.2.34`.
> - **"Cổng" (port):** `9001` (đã là mặc định của app).
>
> Dán cả URL `ws://…` vào ô host sẽ ghép thành URL hỏng dạng `ws://ws://localhost:9001:9001` —
> app không kết nối được. Phiên bản app sau này (kế hoạch M15a phía Mobile_Frontend) sẽ tự tách
> host/port khi dán cả URL; hiện tại chưa có, phải điền host trần.

### 2.4. Thông số chung mọi môi trường

| Tham số | Giá trị | Nguồn |
|---|---|---|
| MQTT username | `app` | user do `scripts/amqtt-setup.sh` sinh |
| MQTT password | — | biến `MQTT_APP_PASSWORD` trong `.env` (broker chặn anonymous) |
| MQTT client | clientId duy nhất (vd `app-<random>`), clean session, keepalive 30–60 s | tự sinh phía app |
| QoS | subscribe **1**; publish lệnh **1** | — |
| Topic prefix | `smarthome` | biến `TOPIC_PREFIX` trong `.env` |
| Influx org | `smarthome` | — |
| Influx bucket | `telemetry` | — |
| Influx token | — | biến `INFLUX_APP_TOKEN` trong `.env` — token **read-only** (user `app-mobile`, query OK / write bị 403) |

Lấy nhanh 2 secret (chỉ in ra màn hình, không ghi file):

```bash
bash scripts/pairing-code.sh
# hoặc riêng lẻ:
grep -E 'MQTT_APP_PASSWORD|INFLUX_APP_TOKEN' .env
```

Lưu ý: port **1883** là MQTT TCP cho firmware (ESP32) — web/app bắt buộc dùng **9001** (WebSocket).

## 3. Contract topic — boards (`{prefix}/boards/{boardId}/...`)

Đây là contract duy nhất — app và firmware cùng dùng, backend không đứng giữa. `{boardId}` do firmware đặt (`DEVICE_ID` trong menuconfig, hoặc tự sinh từ MAC — mục 4 `README.md`).

### 3.1. App SUBSCRIBE (nhận)

| Topic | Payload | QoS / retained | Ý nghĩa |
|---|---|---|---|
| `{prefix}/boards/+/descriptor` | JSON (shape dưới) | 1 / retained | **Discovery**: mọi board xuất hiện qua topic này |
| `{prefix}/boards/{boardId}/descriptor` | JSON | 1 / retained | Khai báo phần cứng board |
| `{prefix}/boards/{boardId}/sensors/{S}/state` | số thuần (vd `25.74`) | 1 / retained | Giá trị hiện tại kênh S (S1, S2, …) |
| `{prefix}/boards/{boardId}/status` | plain `online` / `offline` | 1 / retained | Trạng thái kết nối (LWT: offline tự publish khi board rút nguồn/mất mạng) |
| `{prefix}/boards/{boardId}/relays/{K}/state` | `ON` / `OFF` thuần | 1 / retained | Trạng thái relay kênh K (K1–K3) |

### 3.2. App PUBLISH (gửi)

| Topic | Payload | QoS / retained | Ý nghĩa |
|---|---|---|---|
| `{prefix}/boards/{boardId}/relays/{K}/set` | `ON` / `OFF` thuần | 1 / không retained | Lệnh bật/tắt relay |

### 3.3. Descriptor shape

```json
{
  "schemaVersion": 1,
  "boardId": "0",
  "boardType": "A",
  "sensors": [
    { "channel": "S1", "field": "temperature", "unit": "°C" },
    { "channel": "S2", "field": "humidity", "unit": "%" }
  ],
  "relays": [
    { "channel": "K1" },
    { "channel": "K2" },
    { "channel": "K3" }
  ]
}
```

- `displayName` (string, optional) chỉ xuất hiện khi Kconfig `BOARD_DISPLAY_NAME` của firmware được đặt — app phải xử lý thiếu field này.
- `field` chính là tên field trong Influx — dùng khi query history cho kênh đó.
- Muốn vẽ UI nhiều board: subscribe wildcard `{prefix}/boards/+/descriptor`, rồi với từng boardId subscribe `{prefix}/boards/{boardId}/#`.

### 3.4. Quy tắc xử lý phía app

- **Lệnh relay không có error topic**: gửi `set` rồi chờ `relays/{K}/state` đổi trong timeout (khuyến nghị 2–3 s); quá hạn coi là lỗi (board offline/board không có kênh đó).
- **Board offline vẫn hiện giá trị cũ** (retained) — chỉ đổi badge trạng thái theo topic `status`.
- **Chỉ hiển thị kênh có trong descriptor** — kênh không khai báo thì ẩn.
- **`status` là plain text `online`/`offline`, KHÔNG bọc JSON** `{"status":"online"}`. Nếu code app đang parse JSON thì phải sửa — đây là rủi ro khớp contract đã ghi nhận 2026-09-13.

## 4. History — query InfluxDB trực tiếp

Request:

```http
POST http://<host>:8086/api/v2/query?org=smarthome
Authorization: Token <INFLUX_APP_TOKEN>
Accept: application/csv
Content-Type: application/vnd.flux

from(bucket: "telemetry")
  |> range(start: -1h)
  |> filter(fn: (r) => r._measurement == "sensors" and r.boardId == "0" and r._field == "temperature")
```

- Dữ liệu: measurement `sensors`, tags `boardId` + `roomId` (roomId = boardId, quy ước 1:1), fields tên ngữ nghĩa theo descriptor (`temperature`, `humidity`, …).
- Kết quả CSV: mỗi dòng một field `_field` + `_value` + `_time` — app tự pivot nếu cần nhiều field cùng thời điểm.
- CORS: InfluxDB 2.7 cho phép cross-origin trên `/api/v2/query` — fetch từ web `localhost:<port>` không cần cấu hình thêm.
- Token read-only: query được, ghi bị 403 — an toàn khi nhúng trong app LAN.

## 5. Mô phỏng không cần phần cứng

Board thật thường offline khi không cấp điện ESP32. Muốn dữ liệu chảy để test UI, publish **trực tiếp contract v2** (đúng như firmware v2 làm) với boardId riêng (vd `sim1`) để không đụng retained của board thật:

### 5.1. Mô phỏng board v2 giả

```bash
set -a; . ./.env; set +a
# Descriptor (retained) — backend nạp vào registry để map kênh → field:
docker compose exec -T amqtt amqtt_pub \
  --url "mqtt://$MQTT_USER:$MQTT_PASSWORD@127.0.0.1:1883" -q 1 -r \
  -t 'smarthome/boards/sim1/descriptor' \
  -m '{"schemaVersion":1,"boardId":"sim1","boardType":"A","displayName":"Board giả lập","sensors":[{"channel":"S1","field":"temperature","unit":"°C"},{"channel":"S2","field":"humidity","unit":"%"}],"relays":[{"channel":"K1"},{"channel":"K2"},{"channel":"K3"}]}'
# Telemetry v2 mỗi 5 s (KHÔNG retained):
while true; do
  docker compose exec -T amqtt amqtt_pub \
    --url "mqtt://$MQTT_USER:$MQTT_PASSWORD@127.0.0.1:1883" -q 1 \
    -t 'smarthome/boards/sim1/telemetry' \
    -m "{\"schemaVersion\":2,\"boardId\":\"sim1\",\"values\":{\"S1\":$((24 + RANDOM % 3)).$((RANDOM % 100)),\"S2\":$((55 + RANDOM % 10)).$((RANDOM % 100))}}"
  sleep 5
done
```

Lưu ý: board giả không publish `status` nên app thấy nó "chưa có trạng thái" — muốn hiện online thì publish thêm `smarthome/boards/sim1/status` payload `online` (retained). Lệnh relay cho board giả sẽ timeout (không có gì phản hồi state) — đúng hành vi contract.

### 5.2. Dọn sau khi test

Board giả để lại retained + điểm history. Xóa retained (`-n` = publish payload
rỗng — broker xóa retained khi nhận payload rỗng):

```bash
docker compose exec -T amqtt amqtt_pub \
  --url "mqtt://$MQTT_USER:$MQTT_PASSWORD@127.0.0.1:1883" \
  -t 'smarthome/boards/sim1/descriptor' -n -r -q 1
docker compose exec -T amqtt amqtt_pub \
  --url "mqtt://$MQTT_USER:$MQTT_PASSWORD@127.0.0.1:1883" \
  -t 'smarthome/boards/sim1/status' -n -r -q 1
```

Xóa điểm Influx của board giả (lưu ý influx CLI dùng `--stop`, không phải `--end`):

```bash
set -a; . ./.env; set +a
docker compose exec -T influxdb influx delete \
  --bucket "$INFLUX_BUCKET" --org "$INFLUX_ORG" --token "$INFLUX_TOKEN" \
  --start '2026-09-15T00:00:00Z' --stop '2026-09-16T00:00:00Z' \
  --predicate '_measurement="sensors" AND boardId="sim1"'
```

## 6. Xử lý sự cố nhanh

| Triệu chứng | Nguyên nhân thường gặp | Cách xử lý |
|---|---|---|
| MQTT connect bị từ chối ngay | sai password / sai username | đối chiếu `MQTT_APP_PASSWORD` trong `.env`; chạy `bash scripts/pairing-code.sh` |
| Web không nối được `ws://localhost:9001` | stack chưa chạy | `docker compose up -d` rồi `docker compose ps` (3 service phải Up) |
| App máy thật không nối được | khác subnet / sai IP | điện thoại cùng LAN máy Docker, dùng IP LAN của máy (không phải `10.0.2.2`) |
| App không nối được, URL ghép sai dạng `ws://ws://…` | dán cả URL có `ws://` vào ô "Địa chỉ máy chủ" | host chỉ điền hostname/IP trần (không kèm scheme), port để `9001` — xem lưu ý sau bảng mục 2 |
| Board không hiện giá trị mới | board thật offline (LWT) | xem `smarthome/boards/0/status` = `offline`? → cấp điện ESP32 hoặc chạy vòng mô phỏng mục 5 |
| Relay bấm không ăn | board offline → không có state phản hồi | đúng hành vi (timeout = lỗi); test thật phải cấp điện board |
| History rỗng | chưa có dữ liệu trong range | query `range(start: -7d)` trước; board offline thì chỉ có dữ liệu lúc board chạy |
| Status không hiện online | app parse JSON `{"status":"online"}` | status là plain text `online`/`offline` — sửa parser phía app (mục 3.4) |

## 7. Bảng tham chiếu nhanh theo môi trường

| Môi trường | MQTT | Influx |
|---|---|---|
| Web trên máy Docker | `ws://localhost:9001` | `http://localhost:8086` |
| Android emulator | `ws://10.0.2.2:9001` | `http://10.0.2.2:8086` |
| Điện thoại thật LAN | `ws://<IP-LAN>:9001` | `http://<IP-LAN>:8086` |

URL MQTT trong bảng này là dạng đầy đủ cho browser/tham chiếu — trong app mobile điền vào 2 trường riêng: host (không kèm `ws://`) và port `9001` (chi tiết: lưu ý sau bảng mục 2).

Username/token/org/bucket giống nhau mọi môi trường: `app` / `INFLUX_APP_TOKEN` / `smarthome` / `telemetry`.
