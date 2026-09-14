# Mobile_Backend — luồng telemetry SHT30/SHT31 → MQTT → InfluxDB 2 local

## 1. Tổng quan

Hệ thống thu thập nhiệt độ/độ ẩm Smart Home với luồng dữ liệu:

```
ESP32 + SHT30/SHT31 (I2C)  --Wi-Fi/MQTT-->  Mosquitto (broker)  -->  backend Node.js/TypeScript (Docker)  -->  InfluxDB 2 local (Docker)
```

- **Firmware** (`firmware/esp32-telemetry/`): ESP-IDF v6.0.1 native (C, CMake, FreeRTOS). Đọc SHT30/SHT31 qua I2C, publish JSON lên topic `smarthome/{deviceId}/telemetry` (QoS 1) và quản lý trạng thái online/offline qua topic `smarthome/{deviceId}/status` (LWT retained).
- **Backend** (`src/`): subscribe `smarthome/+/telemetry`, kiểm tra payload bằng schema Zod, ghi điểm vào InfluxDB local (measurement `sensors`, tag `roomId`, fields `temperature`/`humidity`).
- **Bridge** (`src/bridge/`): MQTT client riêng dịch 2 chiều giữa contract firmware `smarthome/...` và contract frontend `<prefix>/room/{roomId}/...` mà app mobile dùng (mục 13).
- **Hạ tầng** (Docker Compose, 3 service): `mosquitto`, `influxdb`, `backend` — toàn bộ server-side chạy trong container.

> **M11 — revert InfluxDB Cloud Serverless → InfluxDB 2.7 local:** user chốt kiến trúc local-only (dễ bảo hành, backup volume). Đường **ghi** giữ nguyên write API v2 (`src/influx/influx-writer.ts`), chỉ đổi schema cho khớp frontend; đường **đọc** (`src/query/*.ts`) quay lại **Flux** qua `@influxdata/influxdb-client` để app mobile query trực tiếp InfluxDB local.

> **Schema mới (M11):** measurement `environment` → `sensors`, tag `device_id`/`room_id` → `roomId` (mỗi phòng một ESP, `deviceId` ≡ `roomId`). App mobile query trực tiếp Flux API v2 bằng token read-only — không có HTTP API backend.

**Phạm vi backend: luồng cảm biến → database.** Không có UI, không HTTP API — truy vấn dữ liệu bằng script CLI (mục 10). Phần điều khiển relay (mục 9) do firmware ESP32 nhận lệnh qua MQTT.

## 2. Yêu cầu phần cứng

- 1 board **ESP32 classic** (DevKit dạng thường, chip ESP32-D0WD trở lên).
- 1 module cảm biến **SHT30 hoặc SHT31** giao tiếp **I2C** (breakout có sẵn pull-up hoặc cảm biến trần).
- Dây nối (jumper), breadboard nếu cần.
- Nguồn: cảm biến và I2C hoạt động ở **3V3** (xem cảnh báo ở mục 3).
- Máy tính chạy Docker (Docker Compose v2) cho phần server; cáp USB để flash firmware.

**Ghi chú pull-up I2C:** firmware đã bật internal pull-up của ESP32 cho SDA/SCL, nhưng internal pull-up **yếu (~45 kΩ)** — đủ cho dây ngắn, tín sạch. Nếu module SHT3x **không có sẵn** pull-up trên board (cảm biến trần), cần thêm **2 điện trở external 4.7 kΩ** kéo SDA và SCL lên 3V3. Dấu hiệu thiếu pull-up: lỗi CRC, timeout hoặc NACK khi đọc cảm biến.

## 3. Đấu dây

| Chân SHT3x | Chân ESP32 | Ghi chú |
|---|---|---|
| VCC | 3V3 | Chỉ cấp **3.3 V** |
| GND | GND | Chung mass |
| SDA | GPIO21 | Mặc định `CONFIG_SHT3X_I2C_SDA_GPIO` |
| SCL | GPIO22 | Mặc định `CONFIG_SHT3X_I2C_SCL_GPIO` |
| ADDR | GND hoặc 3V3 | GND → địa chỉ **0x44** (mặc định); 3V3/VCC → địa chỉ **0x45** |

> **Cảnh báo cấp nguồn:** SHT30/SHT31 chỉ chịu được **3.3 V**. Nối VCC vào 5V/VIN có thể phá hủy cảm biến. Nếu module breakout của bạn có regulator on-board thì kiểm tra datasheet trước khi dùng.

## 4. Cấu hình firmware

Tất cả cấu hình nằm trong menu **`SHT3x Telemetry Configuration`** của `idf.py menuconfig` (xem `firmware/esp32-telemetry/main/Kconfig.projbuild`):

| Mục Kconfig | Ý nghĩa | Mặc định |
|---|---|---|
| `SHT3X_I2C_SDA_GPIO` | GPIO nối SDA | 21 |
| `SHT3X_I2C_SCL_GPIO` | GPIO nối SCL | 22 |
| `SHT3X_I2C_ADDR` | Địa chỉ SHT3x: 0x44 (ADDR→GND) hoặc 0x45 (ADDR→VCC) | 0x44 |
| `WIFI_SSID` | Tên Wi-Fi — **bắt buộc, không có default** | rỗng |
| `WIFI_PASSWORD` | Mật khẩu Wi-Fi (rỗng chỉ hợp lệ cho mạng mở) | rỗng |
| `MQTT_BROKER_URI` | URI broker, ví dụ `mqtt://192.168.1.50:1883` (IP LAN của máy chạy Docker) | `mqtt://192.168.1.100:1883` |
| `MQTT_USER` / `MQTT_PASSWORD` | Tài khoản MQTT — cùng giá trị với `MQTT_USER`/`MQTT_PASSWORD` trong `.env` của server | rỗng |
| `DEVICE_ID` | Dùng trong topic `smarthome/{deviceId}/...` và làm MQTT client id | `esp32-01` |
| `ROOM_ID` | Ghi kèm telemetry (tag `roomId`; mỗi phòng một ESP, deviceId ≡ roomId) | `living-room` |
| `SENSOR_PERIOD_MS` | Chu kỳ đo (ms), khoảng 1000–3600000 | 5000 |
| `MQTT_OUTBOX_LIMIT` | Giới hạn outbox esp-mqtt (byte) khi mất kết nối | 4096 |

**Fail fast khi thiếu cấu hình:** nếu `WIFI_SSID` rỗng, firmware **từ chối khởi động** với log rõ ràng:

```
E (xxx) wifi_conn: CONFIG_WIFI_SSID is empty — configure it via menuconfig
```

SSID/pass không còn là bí mật khi flash vì `sdkconfig` (chứa giá trị đã cấu hình) không được commit — đã đưa vào `.gitignore`.

## 5. Build & flash firmware

Yêu cầu ESP-IDF **v6.0.1** đã cài sẵn qua EIM (script activate tại `~/.espressif/tools/activate_idf_v6.0.1.sh`):

```bash
. ~/.espressif/tools/activate_idf_v6.0.1.sh
cd firmware/esp32-telemetry
idf.py set-target esp32
idf.py menuconfig   # cấu hình Wi-Fi, broker, GPIO (mục 4)
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

- Cổng serial có thể là `/dev/ttyUSB0` hoặc `/dev/ttyACM0` (xem `ls /dev/ttyUSB* /dev/ttyACM*`); user cần thuộc group `dialout`.
- Thoát monitor: `Ctrl-]`.
- Log mong đợi khi chạy tốt: `mqtt_app` connected → `main: started: device=esp32-01 ...` → `telemetry sent: T=... C, RH=... %` mỗi chu kỳ đo.

> **Lưu ý `idf.py` trong script non-interactive:** `idf.py` là alias bash do script activate tạo ra, không tồn tại trong môi trường shell riêng biệt (CI, script chạy nền). Khi đó gọi trực tiếp:
>
> ```bash
> /home/lucas/.espressif/tools/python/v6.0.1/venv/bin/python /home/lucas/.espressif/v6.0.1/esp-idf/tools/idf.py build
> ```

## 6. Chạy server (Docker Compose)

```bash
cp .env.example .env
# điền giá trị thật vào .env:
#   MQTT_USER, MQTT_PASSWORD                  — tài khoản broker (ESP32 dùng cùng cặp này)
#   MQTT_APP_PASSWORD                         — (tuỳ chọn) user app mobile cho WS 9001; để trống thì setup tự sinh + in log
#   TOPIC_PREFIX                              — (tuỳ chọn) prefix contract app, default smarthome
#   INFLUXDB_INIT_USERNAME, INFLUXDB_INIT_PASSWORD — tài khoản admin của InfluxDB local
#   INFLUXDB_INIT_ORG, INFLUXDB_INIT_BUCKET, INFLUXDB_INIT_ADMIN_TOKEN — org/bucket/token tạo lúc setup
#   INFLUX_TOKEN, INFLUX_ORG, INFLUX_BUCKET   — backend dùng lại đúng org/bucket/token trên
# giữ nguyên MQTT_URL=mqtt://mosquitto:1883 và INFLUX_URL=http://influxdb:8086 (tên service nội compose)

docker compose up -d --build
docker compose ps        # mosquitto + influxdb phải ở trạng thái Up (healthy), backend Up
docker compose logs backend   # backend subscribe smarthome/+/telemetry, nối InfluxDB local thành công
```

Lưu ý:

- Broker chỉ chấp nhận user/pass (anonymous bị cấm); password file được sinh lúc container start từ `.env` (xem `scripts/mosquitto-setup.sh`), không nằm trong repo.
- **Hai listener**: `1883` (MQTT thường — ESP32 + backend) và `9001` (MQTT over WebSocket — app mobile), dùng chung một password file. Script setup sinh **hai user**: `MQTT_USER` (mặc định `esp32`) và `MQTT_APP_USER` (mặc định `app`). `MQTT_APP_PASSWORD` để trống thì script tự sinh random và in ra `docker compose logs mosquitto` — lưu lại giá trị đó vào `.env` cho ổn định (xem mục 13).
- **ESP32 nối broker bằng IP LAN của máy chạy Docker** (port 1883 đã map ra host), ví dụ `mqtt://192.168.1.50:1883` trong menuconfig — *không* dùng `localhost` vì `localhost` với ESP32 là chính nó.
- **InfluxDB local** (`influxdb:2.7.10`) tự setup org/bucket/admin token lần start đầu từ bộ `INFLUXDB_INIT_*`; volume `influxdb-data` giữ dữ liệu giữa các lần `up`. Port 8086 map ra host để query CLI/app mobile nối `http://localhost:8086`.
- Bộ `INFLUX_*` (backend đọc) phải trỏ CÙNG org/bucket/token với bộ `INFLUXDB_INIT_*` — image influxdb chỉ hiểu prefix `INFLUXDB_INIT_*`, còn backend đọc `INFLUX_*`.
- Backend trong container nối broker/influx bằng service name (`mqtt://mosquitto:1883`, `http://influxdb:8086`).

### Backup volume InfluxDB

`influxdb-data` là named volume của compose (tên thực tế có prefix tên project, thường `<project>_influxdb-data` — kiểm tra bằng `docker volume ls`). Backup toàn bộ thư mục dữ liệu:

```bash
# Tên volume thực tế: docker volume ls | grep influxdb-data
docker run --rm \
  -v mobile_backend_influxdb-data:/data \
  -v "$(pwd):/backup" \
  alpine tar czf /backup/influxdb-backup.tar.gz -C /data .
```

Khôi phục (dừng stack trước để tránh ghi dở):

```bash
docker compose down
docker run --rm \
  -v mobile_backend_influxdb-data:/data \
  -v "$(pwd):/backup" \
  alpine sh -c "rm -rf /data/* && tar xzf /backup/influxdb-backup.tar.gz -C /data"
docker compose up -d
```

## 7. Chạy backend trên host (debug)

Backend không dùng dotenv khi chạy trực tiếp trên host — cần export biến môi trường trong shell trước:

```bash
npm install

export MQTT_URL=mqtt://localhost:1883
export MQTT_USER=esp32
export MQTT_PASSWORD=<giá trị MQTT_PASSWORD trong .env>
export INFLUX_URL=http://localhost:8086
export INFLUX_TOKEN=<giá trị INFLUX_TOKEN trong .env>
export INFLUX_ORG=<giá trị INFLUX_ORG trong .env>
export INFLUX_BUCKET=<giá trị INFLUX_BUCKET trong .env>

npm run start
```

- Khác với chạy trong compose: `MQTT_URL` và `INFLUX_URL` phải là `localhost` vì port 1883/8086 đã map ra host (service name `mosquitto`/`influxdb` chỉ resolve trong mạng compose).
- Thiếu biến nào sẽ báo lỗi ngay: `Missing required env var: <TÊN>`.
- Dừng: `Ctrl-C` (graceful shutdown — đóng writeApi Influx rồi disconnect MQTT).

## 8. Kiểm thử giả lập (không cần phần cứng)

Sau khi stack đã chạy (mục 6), publish trực tiếp vào broker từ trong container mosquitto. Thay `<password>` bằng giá trị `MQTT_PASSWORD` trong `.env`:

```bash
# Publish payload HỢP LỆ — backend ghi vào InfluxDB
docker exec -it "$(docker compose ps -q mosquitto)" mosquitto_pub \
  -u esp32 -P <password> \
  -t "smarthome/esp32-01/telemetry" \
  -m '{"schemaVersion":1,"deviceId":"esp32-01","roomId":"living-room","temperature":25.5,"humidity":60.0}'

# Payload LỖI (sai schema: schemaVersion phải là 1) — backend log lỗi Zod + bỏ qua, không crash
docker exec -it "$(docker compose ps -q mosquitto)" mosquitto_pub \
  -u esp32 -P <password> \
  -t "smarthome/esp32-01/telemetry" \
  -m '{"schemaVersion":2,"deviceId":"wrong"}'
```

Xác nhận:

- `docker compose logs backend` — payload hợp lệ được nhận; payload lỗi có log chi tiết lỗi validation rồi bỏ qua.
- Lưu ý: `deviceId` trong payload phải khớp segment topic — ví dụ topic `smarthome/esp32-01/telemetry` nhưng payload ghi `deviceId: "esp32-1"` là lệch, bị bỏ qua.
- Truy vấn kết quả theo mục 10.

## 9. Điều khiển relay

Firmware nhận lệnh qua MQTT và điều khiển 3 kênh relay; trạng thái được publish retained để app đọc lại.

### 9.1. Đấu dây

| Relay | GPIO ESP32 | Kconfig | Ghi chú |
|---|---|---|---|
| K1 | GPIO32 | `RELAY_K1_GPIO` | Chân RTC — **khuyến nghị pull-down ngoài** |
| K2 | GPIO33 | `RELAY_K2_GPIO` | Chân RTC — **khuyến nghị pull-down ngoài** |
| K3 | GPIO25 | `RELAY_K3_GPIO` | |

- Logic **Active High** (`RELAY_ACTIVE_HIGH=y`, mặc định): HIGH = đóng (ON), LOW = nhả (OFF). Board Active Low thì tắt Kconfig này — firmware đảo mức tương ứng và vẫn boot OFF.
- `relay_init()` chạy đầu `app_main` và **kéo mọi kênh về OFF trước khi bật pad**, nên relay không nhảy khi boot. Riêng G32/G33 thuộc nhóm RTC: pad pull không active khi chip đang giữ reset, nên nếu board nhạy, thêm **điện trở pull-down ngoài (~10 kΩ) về GND** để coil chắc chắn nhả lúc reset/nguồn lên.
- Sau **reboot, tất cả relay về OFF** (trạng thái không lưu trong NVS).

### 9.2. Topic

| Topic | Chiều | Payload |
|---|---|---|
| `smarthome/{deviceId}/relay/set` | gửi lệnh | JSON `{"schemaVersion":1,"relay":"K1","state":"ON"}` |
| `smarthome/{deviceId}/relay/{K}/set` | gửi lệnh | `ON`/`OFF` thuần (test nhanh) |
| `smarthome/{deviceId}/relay/state` | nhận trạng thái | JSON `{"schemaVersion":1,"K1":"OFF","K2":"OFF","K3":"ON"}` retained |

- `relay`: `K1`/`K2`/`K3` (mở rộng `K4`…`Kn` không đổi protocol); `state`: `ON`/`OFF`. Field lạ (ví dụ `durationMs`, `seq`, alias thân thiện) bị **bỏ qua** — firmware không crash.
- Sai schema / kênh lạ / state lạ → log WARN + bỏ, **không publish gì**.
- State publish **QoS 1 + retained** sau mỗi lệnh hợp lệ và mỗi lần reconnect; payload build từ bảng kênh thực tế (`relay_get_all()`) nên thêm K4 không phải sửa code phần này.

### 9.3. Ví dụ CLI

Thay `<host>`, `<user>`, `<pass>` bằng giá trị broker; `{deviceId}` ví dụ `0`.

```bash
# Dạng JSON (chuẩn)
mosquitto_pub -h <host> -u <user> -P <pass> \
  -t smarthome/0/relay/set \
  -m '{"schemaVersion":1,"relay":"K1","state":"ON"}'

# Dạng per-channel, payload thuần (test nhanh)
mosquitto_pub -h <host> -u <user> -P <pass> -t smarthome/0/relay/K1/set -m ON

# Xem state retained (app đọc topic này để vẽ UI nhiều phòng)
mosquitto_sub -h <host> -u <user> -P <pass> -t 'smarthome/0/relay/state' -v
```

**Ghi chú mở rộng:** tên relay thân thiện (ví dụ "Đèn phòng khách") do **tầng app** đặt — firmware chỉ hiểu kênh vật lý `K1`/`K2`/`K3`, nên đổi tên không cần reflash. Thêm kênh K4+: thêm một dòng trong `components/relay/relay.c` + một Kconfig `RELAY_K4_GPIO`, không đổi topic/protocol.

## 10. Truy vấn dữ liệu

Chạy trên host, nối thẳng vào **InfluxDB local** qua port 8086 đã map ra host (không qua container nào). Cần 4 biến môi trường (xem mục 7 — các script không tự đọc `.env`):

```bash
export INFLUX_URL=http://localhost:8086
export INFLUX_TOKEN=<giá trị INFLUX_TOKEN trong .env>
export INFLUX_ORG=<giá trị INFLUX_ORG trong .env>
export INFLUX_BUCKET=<giá trị INFLUX_BUCKET trong .env>

# Bản ghi mới nhất của một room
npx tsx src/query/latest.ts living-room

# Lịch sử 1 giờ gần nhất của một room
npx tsx src/query/history.ts living-room
```

- **M11:** các script dùng **Flux** qua `@influxdata/influxdb-client` (query API v2). Trước M8 chúng cũng dùng Flux; M8 chuyển sang SQL vì Cloud Serverless không hỗ trợ Flux — M11 revert về local nên quay lại Flux.
- Query latest: `from(bucket) |> range(start: 0) |> filter(_measurement=="sensors" and roomId=="...") |> filter(_field=="temperature" or _field=="humidity") |> last()`; history dùng `range(start: -1h)` + `sort(_time, desc: true)`.
- `roomId` và tên bucket ghép thẳng vào chuỗi Flux (client v2 không bind param cho tag value) nên CLI vẫn validate bằng regex `^[a-zA-Z0-9_-]+$` trước khi ghép (defense-in-depth).
- Đầu ra mỗi dòng: `<thời điểm UTC>  <roomId>  <field>=<value>` với `temperature` (°C) và `humidity` (%RH).

### Query bằng Flux CLI / API v2 (app mobile)

App mobile query trực tiếp InfluxDB local bằng Flux API v2 (`POST /api/v2/query`), chỉ cần token read-only. Kiểm tra nhanh bằng `curl` trên host:

```bash
curl -s --request POST "http://localhost:8086/api/v2/query?org=smarthome" \
  --header "Authorization: Token <INFLUX_TOKEN>" \
  --header "Content-Type: application/vnd.flux" \
  --data 'from(bucket:"telemetry") |> range(start:-1h) |> filter(fn:(r)=> r._measurement=="sensors" and r.roomId=="living-room") |> filter(fn:(r)=> r._field=="temperature" or r._field=="humidity")'
```

## 11. Phần cần ESP32 thật

Các kiểm thử tự động (mục 8) và unit test không mô phỏng được phần cứng — những việc dưới đây **bắt buộc có ESP32 thật** để xác minh:

1. **Đọc SHT30/SHT31 thật qua I2C** — CRC-8 (poly 0x31, init 0xFF) và timing thực tế (độ trễ single-shot, clock stretching) chỉ đúng khi gặp cảm biến thật; cần external pull-up 4.7 kΩ nếu module không có sẵn.
2. **Wi-Fi LAN thật** — kết nối router thật, retry/backoff khi AP rớt, xử lý đổi IP/DHCP.
3. **LWT offline** — rút nguồn ESP32 → sau keep-alive, broker publish retained `offline` lên `smarthome/{deviceId}/status`; khi ESP32 kết nối lại sẽ publish retained `online`.
4. **Chuỗi QoS 1 end-to-end** — mất mạng rồi có lại: message được retransmit đến khi broker nhận (at-least-once), outbox không tăng vô hạn (`MQTT_OUTBOX_LIMIT` — đầy thì drop + log).
5. **Chọn đúng chân SDA/SCL** cho loại module cụ thể — một số breakout expose chân khác; đổi qua `SHT3X_I2C_SDA_GPIO`/`SHT3X_I2C_SCL_GPIO` trong menuconfig nếu cần.

## 12. Cấu trúc thư mục

```
firmware/esp32-telemetry/          Firmware ESP-IDF v6.0.1 (C, CMake, FreeRTOS)
  main/                            app_main (composition root), Kconfig.projbuild, idf_component.yml
  components/sht3x/                Driver I2C master (driver/i2c_master.h) + CRC-8 + convert giá trị
  components/wifi_conn/            Wi-Fi station event-driven, retry backoff, fail fast khi thiếu SSID
  components/mqtt_app/             esp-mqtt (managed component espressif/mqtt), LWT + QoS 1 + outbox limit
  sdkconfig.defaults               Cấu hình mặc định (không chứa secret)

src/                               Backend TypeScript (ESM strict)
  env.ts                           Đọc + validate biến môi trường bắt buộc (gồm TOPIC_PREFIX)
  telemetry/                       Schema Zod + validate topic/JSON (smarthome/+/telemetry)
  mqtt/                            mqtt-service: subscribe, route message hợp lệ vào writer
  bridge/                          bridge-service + topic-mapper: dịch contract firmware ↔ app mobile
  influx/                          influx-writer: retry backoff, queue 1000 điểm, drop oldest khi đầy
  query/latest.ts                  CLI: bản ghi telemetry mới nhất theo roomId (Flux)
  query/history.ts                 CLI: lịch sử 1 giờ theo roomId (Flux)
  main.ts                          Composition root + graceful shutdown (SIGINT/SIGTERM)

docker-compose.yml                 3 service: mosquitto, influxdb (2.7.10 local), backend
Dockerfile                         Multi-stage node:24-alpine: build tsc → runtime chỉ dist + deps production
mosquitto/config/mosquitto.conf    Cấu hình broker (chặn anonymous, listener 1883 MQTT + 9001 WebSocket)
scripts/mosquitto-setup.sh         Sinh password file lúc container start (user MQTT_USER + user app)
scripts/pairing-code.sh            In khối mã ghép nối cho app mobile (LAN IP, WS, token — chỉ ra stdout)
.env.example                       Mẫu biến môi trường — copy thành .env và điền giá trị thật (InfluxDB local)
```

## 13. App mobile kết nối

App mobile **không** nói chuyện trực tiếp với contract firmware. Backend chạy một **bridge** (`src/bridge/`) subscribe topic firmware rồi re-publish sang contract frontend `<prefix>/room/{roomId}/...`, và ngược lại. Đổi contract phía app chỉ cần sửa bridge — **không bao giờ phải reflash firmware**.

### 13.1. Điểm kết nối

| Thứ | Giá trị | Ghi chú |
|---|---|---|
| MQTT over WebSocket | `ws://<LAN_IP>:9001` | Listener 9001 trong `mosquitto.conf`, `protocol websockets` |
| MQTT user | `app` (`MQTT_APP_USER`) | Mật khẩu `MQTT_APP_PASSWORD` trong `.env`; để trống thì `docker compose logs mosquitto` in ra bản random |
| MQTT topic prefix | `<TOPIC_PREFIX>`, default `smarthome` | Đặt trong `.env` |
| History (Flux) | `POST http://<LAN_IP>:8086/api/v2/query` | Token **read-only**, không qua backend (mục 10) |
| Influx org / bucket | `INFLUX_ORG` / `INFLUX_BUCKET` | Mặc định `smarthome` / `telemetry` |

### 13.2. Contract topic (firmware ↔ frontend)

`{prefix}` = `TOPIC_PREFIX` (default `smarthome`); `{roomId}` ≡ `deviceId`.

| Hướng | Topic | Payload | QoS / retained |
|---|---|---|---|
| Sensor → app | `{prefix}/room/{roomId}/sensor/temperature` | số (°C, thuần) | 1 / **retained** |
| Sensor → app | `{prefix}/room/{roomId}/sensor/humidity` | số (%RH, thuần) | 1 / **retained** |
| Trạng thái → app | `{prefix}/room/{roomId}/status` | `online` / `offline` | 1 / **retained** |
| Relay state → app | `{prefix}/room/{roomId}/stat/relay/{n}` | `ON` / `OFF` | 1 / **retained** |
| App → relay | `{prefix}/room/{roomId}/cmnd/relay/{n}` | `ON` / `OFF` (thuần) | 1 / không retained |

Nguồn firmware tương ứng (không đổi):

| Firmware | Bridge dịch sang |
|---|---|
| `smarthome/{deviceId}/telemetry` (JSON, có `roomId`) | 2 topic `sensor/temperature` + `sensor/humidity` (tách mỗi field một số) |
| `smarthome/{deviceId}/status` (`online`/`offline`) | `{prefix}/room/{deviceId}/status` (payload không có roomId ⇒ dùng deviceId) |
| `smarthome/{deviceId}/relay/state` (retained JSON `K1..K3`) | `{prefix}/room/{deviceId}/stat/relay/{1..3}` |
| `{prefix}/room/{roomId}/cmnd/relay/{1..3}` | `smarthome/{roomId}/relay/K{n}/set` (payload thuần) |

- Relay slot `1↔K1`, `2↔K2`, `3↔K3`. Slot **4–10 bridge bỏ qua** (firmware hiện chỉ có 3 kênh) — publish vào vẫn không lỗi, chỉ log DEBUG.
- Message sai shape (JSON hỏng, schema sai, payload không phải `ON`/`OFF`) → bridge log + bỏ, **không crash**.

### 13.3. In mã ghép nối

Chạy trên host, cùng thư mục repo:

```bash
bash scripts/pairing-code.sh
```

Script tự detect LAN IP (bỏ loopback/docker/tailscale), đọc `.env`, in khối mã ghép nối cho app. Nếu máy có `qrencode` thì in thêm QR (không bắt buộc):

```
==== SMART HOME PAIRING ====
Broker (MQTT over WebSocket): ws://192.168.1.50:9001
MQTT user: app
MQTT pass: <MQTT_APP_PASSWORD>
Influx query URL: http://192.168.1.50:8086
Influx org: smarthome
Influx read token: <INFLUX_APP_TOKEN>
Topic prefix: smarthome
===========================
```

> Token/password **chỉ** in ra stdout, không ghi ra file nào. Không commit giá trị thật vào repo.

### 13.4. Token Influx read-only cho app

App query Flux trực tiếp InfluxDB local bằng token **read-only** (không dùng token admin). Trên instance đang chạy đã có user `app-mobile`; tạo token và gán vào `.env` biến `INFLUX_APP_TOKEN` để `pairing-code.sh` in sẵn:

```bash
docker exec -it "$(docker compose ps -q influxdb)" influx auth create \
  --user app-mobile --read-bucket "$INFLUX_BUCKET" \
  --description "mobile app read-only"
```

Copy token in ra vào `.env` (`INFLUX_APP_TOKEN=...`) rồi chạy lại `bash scripts/pairing-code.sh`.

