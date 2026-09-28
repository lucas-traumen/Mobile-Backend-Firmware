# Developer Guide — Mobile_Backend

Tài liệu cho người phát triển/người kỹ thuật. README chính là [hướng dẫn sử dụng cho người dùng cuối](../README.md); tài liệu này chứa phần kỹ thuật: kiến trúc, triển khai từ source, firmware ESP32, contract dữ liệu MQTT, truy vấn dữ liệu, vận hành InfluxDB và kiểm thử.

## Kiến trúc tổng quan

Luồng dữ liệu:

```
ESP32 + SHT30/SHT31 (I2C)
        │  Wi-Fi → MQTT TCP 1883
        ▼
amqtt broker ──► backend Node.js/TypeScript (Docker) ──► InfluxDB 2.7 local (Docker)
        │  MQTT over WebSocket 9001
        ▼
App mobile (realtime qua broker; lịch sử qua Flux API 8086, token read-only)
```

- Backend **chỉ** ingest telemetry vào InfluxDB — không UI, không HTTP API. Lệnh relay đi thẳng app → broker → firmware, backend không đứng giữa.
- App mobile query lịch sử trực tiếp InfluxDB local bằng Flux API v2 với token **read-only**.
- Toàn bộ server-side chạy trong Docker Compose (3 service: `amqtt`, `influxdb`, `backend`).

| Port trên host | Dùng cho |
|---|---|
| 1883 | MQTT TCP — board ESP32 + backend |
| 9001 | MQTT over WebSocket — app mobile |
| 8086 | InfluxDB HTTP — query Flux (app + CLI) |

mDNS: server quảng bá `_smarthome._tcp` port 9001 kèm TXT `prefix=smarthome`, `influx_port=8086`, `influx_org=smarthome`, `influx_bucket=telemetry` (template `avahi/smarthome.service`). Đây là contract riêng của app — bên Mobile_Frontend khớp theo `mdnsDiscoveryContract.ts`; TXT keys dùng gạch dưới. Secret (mật khẩu MQTT, token Influx) **không** đi qua mDNS — app vẫn nhập trong màn hình ghép nối.

## Cấu trúc thư mục

```
firmware/esp32-telemetry/          Firmware ESP-IDF v6.0.1 (C, CMake, FreeRTOS)
  main/                            app_main (composition root), board_id.c/h (boardId từ MAC), Kconfig.projbuild
  components/sht3x/                Driver I2C master + CRC-8 + convert giá trị
  components/wifi_conn/            Wi-Fi station event-driven, retry backoff, classify disconnect reason
  components/ble_prov/             BLE provisioning NimBLE: GATT 9 UUID, Just Works + bond, NVS "bleprov"
  components/mqtt_app/             esp-mqtt, contract boards (descriptor/telemetry/sensor+relay state/LWT) + QoS 1 + outbox limit
  sdkconfig.defaults               Cấu hình mặc định (không chứa secret)

src/                               Backend TypeScript (ESM strict)
  env.ts                           Đọc + validate biến môi trường bắt buộc (gồm TOPIC_PREFIX)
  telemetry/                       Schema Zod + validate telemetry v2
  boards/                          Descriptor registry (cache boardId → descriptor) + ingest telemetry v2 (map theo descriptor)
  mqtt/                            mqtt-service: một MQTT connection — subscribe + route descriptor/telemetry
  influx/                          influx-writer: retry backoff, queue 1000 điểm, drop oldest khi đầy
  query/latest.ts                  CLI: bản ghi telemetry mới nhất theo roomId (Flux)
  query/history.ts                 CLI: lịch sử 1 giờ theo roomId (Flux)
  main.ts                          Composition root + graceful shutdown (SIGINT/SIGTERM)

docker-compose.yml                 3 service — bản repo có build:, CHỈ dùng trên máy dev
Dockerfile                         Backend: multi-stage node:24-alpine (build tsc → dist + deps production)
amqtt/broker.yaml                  Cấu hình broker (cấm anonymous, listener 1883 TCP + 9001 WS)
amqtt/Dockerfile                   amqtt 0.12.1 + persistence sqlite (SessionDBPlugin)
scripts/smarthome-deploy.sh        Deploy IMAGE-ONLY cho server thật — không cần clone repo
scripts/smarthome.sh               Entrypoint developer trên host (sau khi clone repo)
scripts/server-init.sh             Bootstrap mDNS + docker compose up --build (được smarthome.sh gọi)
scripts/board-qr.sh                Nhãn QR board (dò serial qua esptool, hoặc truyền -m/--board-id)
scripts/pairing-code.sh            In khối mã ghép nối cho app mobile
scripts/credentials-qr.sh          In QR credentials cho app mobile (chứa secret)
scripts/amqtt-setup.sh             Sinh password file argon2 lúc container start
scripts/amqtt-passwd.py            Sinh một dòng username:argon2hash cho passwd file broker
.env.example                       Mẫu biến môi trường — copy thành .env và điền giá trị thật
```

## Triển khai server thật — image-only (`scripts/smarthome-deploy.sh`)

Luồng user-facing đã tóm tắt trong README; dưới đây là nội bộ script (self-contained, server không cần repo, không Node/npm, không ESP-IDF, không thao tác build image nào):

**Images:**

| Service | Image | Ghi chú |
|---|---|---|
| `amqtt` | `trilucas/app_smarthome:amqtt-latest` | amqtt 0.12.1 + deps persistence |
| `influxdb` | `influxdb:2.7.10` | image upstream public |
| `backend` | `trilucas/app_smarthome:backend-latest` | backend Node dựng sẵn |

⚠️ Cả hai image `trilucas/app_smarthome` hiện chỉ build cho `linux/amd64` — server đích phải x86-64; ARM/Raspberry Pi sẽ pull fail tới khi có multi-arch build. `init` kiểm tra `uname -m` và cảnh báo (không chặn).

**Chuỗi `init`** (fail bước nào dừng tại đó):

1. Preflight: `docker` + `docker compose version`, `openssl`, `python3`; `uname -m` ≠ `x86_64` → chỉ WARN.
2. Ghi runtime bundle vào `$SMART_HOME_DIR` (mặc định `~/smarthome`): `docker-compose.yml` (image-only), `broker.yaml`, `helpers/amqtt-setup.sh`, `helpers/amqtt-passwd.py`, `avahi/smarthome.service`; tạo `.env` (mode 600, secret `openssl rand -hex` qua biến môi trường python3 — không argv, không in ra màn hình) nếu chưa có.
3. `docker compose config --quiet` — validate trước mọi thao tác image.
4. `docker compose pull` — pull 3 image trước khi up.
5. mDNS: bảo đảm `avahi-daemon` active, `sudo cp` template service sang `/etc/avahi/services/smarthome.service`.
6. `docker compose up -d` (không `--build` — không có gì để dựng).
7. Đợi healthy tối đa 120 s (amqtt + influxdb `healthy`, backend `running`) — hết giờ chỉ WARN, không abort.
8. Verify mDNS bằng `avahi-browse -rt _smarthome._tcp` (thiếu tool chỉ WARN).
9. Tóm tắt: LAN IP + 3 port + gợi ý mở port ufw. Không in secret.

`.env` **không bao giờ bị ghi đè**; các file bundle khác được ghi lại đúng nội dung chuẩn mỗi lần `init` (idempotent). Nếu `.env` còn placeholder `change-me-*`, script cảnh báo và hướng dẫn tự sửa.

**Override image (pin tag/digest):** precedence compose chuẩn — env của lệnh > dòng `KEY=...` trong `.env` runtime > default trong compose render:

```bash
BACKEND_IMAGE=trilucas/app_smarthome:<tag-hoặc-digest> \
AMQTT_IMAGE=trilucas/app_smarthome:<tag-hoặc-digest> \
INFLUX_IMAGE=influxdb:2.7.10 \
bash smarthome-deploy.sh update
```

Script không tự set default nào cho các biến này — pin bằng dòng `KEY=...` trong `.env` có hiệu lực cho cả script lẫn `docker compose` thủ công.

Tag `latest` là mutable — khi cần ổn định hãy pin digest. Docker Hub repo `trilucas/app_smarthome` là public (không cần `docker login`); nếu chuyển private thì login bằng tài khoản có quyền pull trước khi `init`/`update`.

## Developer: build từ source (thư mục repo)

> `docker-compose.yml` trong repo dùng `build:` — chỉ chạy trên máy dev. Server thật dùng `smarthome-deploy.sh`, không clone repo, không build.

### Chuẩn bị `.env`

```bash
cp .env.example .env
```

| Khóa | Ý nghĩa |
|---|---|
| `MQTT_USER`, `MQTT_PASSWORD` | Tài khoản broker — ESP32 dùng cùng cặp này |
| `MQTT_APP_PASSWORD` | Mật khẩu user app mobile cho WS 9001; để trống thì setup tự sinh + in log |
| `TOPIC_PREFIX` | Prefix contract boards, mặc định `smarthome` |
| `INFLUXDB_INIT_USERNAME/PASSWORD`, `INFLUXDB_INIT_ORG`, `INFLUXDB_INIT_BUCKET`, `INFLUXDB_INIT_ADMIN_TOKEN` | Tạo org/bucket/admin token lúc setup InfluxDB lần đầu |
| `INFLUX_TOKEN`, `INFLUX_ORG`, `INFLUX_BUCKET` | Backend dùng lại đúng org/bucket/token trên |
| `MQTT_URL`, `INFLUX_URL` | Giữ nguyên `mqtt://amqtt:1883` và `http://influxdb:8086` (service name nội compose) |

Bộ `INFLUX_*` phải trỏ **cùng** org/bucket/token với bộ `INFLUXDB_INIT_*` — image influxdb chỉ hiểu prefix `INFLUXDB_INIT_*`, còn backend đọc `INFLUX_*`.

### Chạy stack

```bash
docker compose up -d --build
docker compose ps              # amqtt + influxdb Up (healthy), backend Up
docker compose logs backend    # subscribe {prefix}/boards/+/descriptor + +/telemetry, nối InfluxDB OK
```

- Broker cấm anonymous; password file (argon2) sinh lúc container start từ `.env` bởi `scripts/amqtt-setup.sh`, không nằm trong repo. Hai user trong cùng một passwd file: `MQTT_USER` (mặc định `esp32` — board + backend) và `MQTT_APP_USER` (mặc định `app` — mobile, qua WS). `MQTT_APP_PASSWORD` trống → tự sinh random, in ra `docker compose logs amqtt` — chép vào `.env` cho ổn định qua restart.
- ESP32 nối broker bằng **IP LAN** của máy chạy Docker (port 1883 đã map ra host), ví dụ `mqtt://192.168.1.50:1883` — không dùng `localhost` (với ESP32, `localhost` là chính nó). Scheme `mqtt://` tùy chọn, firmware tự bù khi thiếu.
- InfluxDB tự setup org/bucket/admin token lần start đầu từ bộ `INFLUXDB_INIT_*`; volume `influxdb-data` giữ dữ liệu giữa các lần `up`. Port 8086 map ra host để query `http://localhost:8086`.

### Runbook một trạm với `scripts/smarthome.sh`

`init` tự bọc chuỗi thủ công + `server-init.sh`; script không tự cài package — thiếu gì chỉ in cách cài.

1. **Tool host**: Docker Engine + Compose v2 (plugin `docker compose` đi kèm Engine mới — `apt install docker.io` kiểu distro cũ không đảm bảo có Compose v2) + `openssl python3 avahi-daemon avahi-utils qrencode`.
2. **Clone + pin bản phát hành**:

   ```bash
   git clone <URL_REPO> Mobile_Backend
   cd Mobile_Backend
   git checkout <TAG_HOẶC_COMMIT>
   ```

3. **Init**: `bash scripts/smarthome.sh init` — tạo `.env` từ `.env.example` nếu chưa có (secret random, mode 600, `INFLUXDB_INIT_ADMIN_TOKEN` ≡ `INFLUX_TOKEN`), validate compose, `pull --ignore-buildable` + `build --pull`, rồi gọi `server-init.sh` (avahi + `up -d --build` + đợi healthy + verify `_smarthome._tcp`).
4. **Menu / direct mode**: `bash scripts/smarthome.sh` không đối số mở menu (`1 init`, `2 credentials-qr`, `3 board-qr`, `4 pairing-code`, `5 status`, `0 exit`); direct mode cũng chạy được: `bash scripts/smarthome.sh init|credentials-qr|board-qr|pairing-code|status`.
5. **Kiểm tra sau init**: `bash scripts/smarthome.sh status` — amqtt + influxdb `Up (healthy)`, backend `Up`, thấy `_smarthome._tcp`. Từ máy khác cùng Wi-Fi: `avahi-browse -rt _smarthome._tcp`.
6. **Update**: `git fetch` + checkout tag mới + `bash scripts/smarthome.sh init` (`.env` giữ nguyên, pull/build bản mới rồi `up --build`).

### Hạn chế mDNS cần biết

- **AP isolation** (Wi-Fi quán cà phê/văn phòng/khách sạn): client không thấy nhau qua multicast — app phải nhập IP tay.
- **Laptop sleep** = server mất với toàn mạng — tắt sleep khi demo.
- Server quảng bá **mọi IP** của máy (gồm docker bridge 172.x, tailscale 100.x) — app phải tự lọc IP LAN (cùng luật `detect_lan_ip` trong `scripts/pairing-code.sh`).
- Thiếu `avahi-utils` → script chỉ WARN, không verify được quảng bá.
- `ufw` bật → mở 3 port: `sudo ufw allow 1883/tcp`, `sudo ufw allow 9001/tcp`, `sudo ufw allow 8086/tcp`.

## Chạy backend trên host (debug)

Backend không dùng dotenv khi chạy trực tiếp trên host — export biến môi trường trước:

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

- Trên host dùng `localhost` (port 1883/8086 đã map ra host); trong compose mới dùng service name.
- Thiếu biến nào báo ngay: `Missing required env var: <TÊN>`.
- Dừng `Ctrl-C` — graceful shutdown (đóng writeApi Influx rồi disconnect MQTT).

## Firmware ESP32 (`firmware/esp32-telemetry`)

### Phần cứng và đấu dây

| Chân SHT3x | Chân ESP32 | Ghi chú |
|---|---|---|
| VCC | 3V3 | Chỉ cấp **3.3 V** — 5V/VIN có thể phá cảm biến |
| GND | GND | Chung mass |
| SDA | GPIO21 | Mặc định `SHT3X_I2C_SDA_GPIO` |
| SCL | GPIO22 | Mặc định `SHT3X_I2C_SCL_GPIO` |
| ADDR | GND hoặc 3V3 | GND → 0x44 (mặc định); 3V3 → 0x45 |

Relay:

| Relay | GPIO | Kconfig | Ghi chú |
|---|---|---|---|
| K1 | GPIO32 | `RELAY_K1_GPIO` | Chân RTC — khuyến nghị pull-down ngoài |
| K2 | GPIO33 | `RELAY_K2_GPIO` | Chân RTC — khuyến nghị pull-down ngoài |
| K3 | GPIO25 | `RELAY_K3_GPIO` | |

- Logic **Active High** (`RELAY_ACTIVE_HIGH=y` mặc định); board Active Low thì tắt Kconfig — firmware tự đảo mức và vẫn boot OFF.
- `relay_init()` kéo mọi kênh về OFF trước khi bật pad → relay không nhảy lúc boot. G32/G33 (RTC): pad pull không active khi chip giữ reset — board nhạy thì thêm pull-down ngoài ~10 kΩ về GND.
- Sau reboot, tất cả relay về OFF (không lưu NVS).

**Pull-up I2C:** firmware bật internal pull-up nhưng yếu (~45 kΩ) — đủ cho dây ngắn, tín sạch. Module SHT3x không có pull-up sẵn (cảm biến trần) cần 2 điện trở ngoài 4.7 kΩ kéo SDA/SCL lên 3V3. Dấu hiệu thiếu pull-up: lỗi CRC, timeout, NACK.

### Cấu hình (`idf.py menuconfig`)

Tất cả nằm trong menu `SHT3x Telemetry Configuration` (`main/Kconfig.projbuild`):

| Mục Kconfig | Ý nghĩa | Mặc định |
|---|---|---|
| `SHT3X_I2C_SDA_GPIO` / `SHT3X_I2C_SCL_GPIO` | GPIO SDA/SCL | 21 / 22 |
| `SHT3X_I2C_ADDR` | 0x44 (ADDR→GND) hoặc 0x45 (ADDR→VCC) | 0x44 |
| `WIFI_SSID` / `WIFI_PASSWORD` | Cấu hình Wi-Fi cứng. **Rỗng (default) + NVS trống → board boot vào BLE provisioning** | rỗng |
| `MQTT_BROKER_URI` | URI broker, vd `mqtt://192.168.1.50:1883`; thiếu scheme firmware tự bù | `mqtt://192.168.1.100:1883` |
| `MQTT_USER` / `MQTT_PASSWORD` | Tài khoản MQTT — cùng giá trị với `.env` của server | rỗng |
| `MQTT_TOPIC_PREFIX` | Phải khớp `TOPIC_PREFIX` của backend | `smarthome` |
| `DEVICE_ID` | boardId trong topic + client id + tên BLE `IoTBoard-{boardId}`. **Rỗng (default, khuyến nghị)** = tự sinh hex-8 từ 4 byte cuối MAC Wi-Fi STA | rỗng |
| `BOARD_TYPE` | Mã loại board — `boardType` trong descriptor + Device Info BLE | `IoT_ESP32-S2R3` |
| `BOARD_DISPLAY_NAME` | Tên hiển thị trong descriptor (`displayName`); rỗng = bỏ field | rỗng |
| `ROOM_ID` | Legacy firmware v1 — v2 không gửi; giữ option cho `sdkconfig` cũ | `living-room` |
| `SENSOR_PERIOD_MS` | Chu kỳ đo (ms), 1000–3600000 | 5000 |
| `MQTT_OUTBOX_LIMIT` | Giới hạn outbox esp-mqtt khi mất kết nối (byte) | 4096 |

### Build & flash

Cần ESP-IDF v6.0.1 cài qua EIM (script activate tại `~/.espressif/tools/activate_idf_v6.0.1.sh`):

```bash
. ~/.espressif/tools/activate_idf_v6.0.1.sh
cd firmware/esp32-telemetry
idf.py set-target esp32
idf.py menuconfig   # cấu hình Wi-Fi, broker, GPIO
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

- Cổng serial: `/dev/ttyUSB0` hoặc `/dev/ttyACM0` (`ls /dev/ttyUSB* /dev/ttyACM*`); user cần thuộc group `dialout`. Thoát monitor: `Ctrl-]`.
- Log mong đợi: `mqtt_app` connected → `main: started: board=... prefix=smarthome ...` → `telemetry sent: T=... C, RH=... %` mỗi chu kỳ đo.
- `idf.py` là alias bash do script activate tạo — trong môi trường non-interactive (CI) gọi trực tiếp: `/home/lucas/.espressif/tools/python/v6.0.1/venv/bin/python /home/lucas/.espressif/v6.0.1/esp-idf/tools/idf.py build`.
- Reflash firmware v2 lên board đang chạy firmware v1 làm board **ngừng phát mọi topic contract room cũ** ngay lập tức và chỉ nói contract boards — app phải đã dùng contract boards, nếu không board sẽ "biến mất" khỏi app. Retained v1 còn sót trên broker muốn dọn thì publish payload rỗng (`amqtt_pub ... -n -r`) lên đúng topic đó.
- `sdkconfig` chứa giá trị cấu hình (kể cả Wi-Fi nếu điền cứng) — không commit (đã trong `.gitignore`).

### boardId — sinh từ MAC

Khi `DEVICE_ID` rỗng (khuyến nghị), firmware tự sinh `boardId` lúc boot: **4 byte cuối của MAC Wi-Fi STA, hex-8 lowercase** (vd MAC `5c:01:3b:6b:af:6c` → `3b6baf6c`). MAC đốt sẵn trong eFuse nên id unique theo phần cứng, không đổi qua reboot/reflash. Log boot in rõ: `boardId="3b6baf6c" (from MAC)`.

Điền `DEVICE_ID` khác rỗng thì firmware dùng nguyên giá trị đó (back-compat, ưu tiên hơn MAC).

Nhãn QR board cho app quét — JSON 3 field thuần, không bọc URL:

```bash
bash scripts/board-qr.sh                  # tự dò board qua esptool (cần ESP-IDF)
bash scripts/board-qr.sh -p /dev/ttyUSB0  # chỉ định cổng serial
bash scripts/board-qr.sh -m 5c:01:3b:6b:af:6c      # MAC cho sẵn, không cần board nối
bash scripts/board-qr.sh --board-id 3b6baf6c       # id cho sẵn
bash scripts/board-qr.sh -m 5c:01:3b:6b:af:6c -t IoT_ESP32-S2R3
```

- Script derive bằng đúng thuật toán firmware (`main/board_id.c`) nên tự khớp `boardId` mà board publish.
- `boardId` phải là một segment topic hợp lệ (chữ/số/`_`/`-`); QR lệch `boardId` làm app cấu hình nhầm board.
- `boardType` = giá trị `BOARD_TYPE` (default `IoT_ESP32-S2R3`); app hiển thị làm tên board.
- Trên server deploy không có ESP-IDF/esptool — dùng `bash smarthome-deploy.sh board-qr -m <MAC>` (không dò serial).

### BLE provisioning

Board chưa có credentials Wi-Fi (NVS namespace `bleprov` trống và `WIFI_SSID` rỗng) tự bật BLE provisioning: quảng bá `IoTBoard-{boardId}`, app quét QR board, nối GATT, đẩy SSID/pass + broker URI + tài khoản MQTT rồi gửi lệnh `PROVISION`. Board thử Wi-Fi (tối đa 30 s) và báo kết quả qua characteristic Status; thành công thì credentials lưu NVS, BLE tự tắt 30 s sau và board chạy telemetry như board cấu hình bằng menuconfig.

GATT contract — service + 8 characteristics, base UUID `e5f4a3b2-1c0d-4e2f-9a8b-7c6d5e4f3aXX` (app mobile là nguồn chuẩn của contract):

| Item | UUID | Thuộc tính |
|---|---|---|
| Provisioning Service | `…3a01` | — |
| WiFi SSID | `…3a02` | WRITE, encrypted |
| WiFi Password | `…3a03` | WRITE, encrypted |
| Command | `…3a04` | WRITE — ASCII `PROVISION` |
| Device Info | `…3a05` | READ — JSON `{"schemaVersion":1,"boardId":"…","boardType":"…"}` |
| Status | `…3a06` | NOTIFY — ASCII |
| Broker URI | `…3a07` | WRITE |
| MQTT User | `…3a08` | WRITE |
| MQTT Pass | `…3a09` | WRITE, encrypted |

Hành vi chính:

- Pairing **Just Works + bonding** (không MITM); ghi encrypted tự kích hoạt pairing, bond lưu NVS.
- Flow `PROVISION`: ghi đủ trường → viết `PROVISION` vào `…3a04` → notify `CONNECTING` → tối đa 30 s → `CONNECTED` hoặc `FAILED:BAD_AUTH` / `FAILED:NO_SSID` / `FAILED:TIMEOUT` / `FAILED:ERROR`. Thất bại → nhận cấu hình mới, không giới hạn số lần.
- Giới hạn độ dài (byte; sai → ATT *Invalid Attribute Value Length*): SSID 1–32; WiFi pass 0–63 (rỗng = mạng mở); broker URI 4–128; MQTT user 1–64; MQTT pass 0–64.
- NVS `bleprov` (`ssid`, `wifi_pass`, `broker_uri`, `mqtt_user`, `mqtt_pass`) chỉ ghi **khi Wi-Fi connect thành công**; lưu plain-text (không flash encryption — hạn chế đã chấp nhận).
- Write đều dùng write-with-response — giá trị vượt MTU đi qua prepared-write, stack tự gom; app viết nguyên khối không cần chia gói.
- BLE tự tắt 30 s sau `CONNECTED` (NimBLE dừng để nhả RAM); từ boot sau board dùng NVS nên BLE không bật lại.

**Ba chế độ boot** (thứ tự ưu tiên credentials):

1. **NVS `bleprov`** đủ 5 keys — board đã provision: chạy như board Kconfig, BLE tắt. Kèm một check có trần lúc boot: Wi-Fi phải có IP trong 15 s rồi MQTT connected trong 10 s — fail thì xử lý theo [Khôi phục credentials hỏng](#khôi-phục-credentials-hỏng).
2. **Kconfig** — không NVS nhưng `WIFI_SSID` khác rỗng: flow classic.
3. **BLE provisioning** — không NVS và `WIFI_SSID` rỗng: quảng bá chờ app, lặp vô hạn tới khi connect được.

→ Build board mới "rút hộp": giữ mặc định `WIFI_SSID=""` → board boot thẳng vào BLE mode.

**Nút BOOT recovery:** giữ **BOOT (GPIO0) ≥ 5 s khi board đang chạy** → firmware xóa NVS `bleprov` rồi tự reboot (về BLE mode nếu `WIFI_SSID` rỗng, về Kconfig mode nếu không). Dùng khi: đổi mật khẩu router, provision nhầm broker/user. Giữ nút BẤM qua lúc reset vẫn vào ROM download mode như thường — recovery chỉ đọc mức GPIO0 khi app đang chạy (debounce 20 ms).

### Khôi phục credentials hỏng

- Board đã provision được check một lần lúc boot (15 s Wi-Fi IP + 10 s MQTT). Không thấy AP (reason 200/201) hoặc broker không tới được → firmware dừng MQTT, xóa NVS `bleprov`, tự reboot vào BLE provisioning (không có vòng xóa-reboot lặp — erase chỉ chạy khi NVS thực sự còn credentials).
- **Ngoại lệ — mật khẩu Wi-Fi bị từ chối (reason 202/BAD_AUTH là fail cuối):** firmware **không xóa NVS, không reboot** — chỉ dừng vòng retry Wi-Fi, đứng yên và log hướng dẫn giữ nút **BOOT ≥ 5 s** để xóa credentials thủ công.
- Nhánh **Kconfig không tự xóa** NVS và retry nền với mọi disconnect reason (latch "AP mất thì ngừng retry" chỉ nhánh NVS bật lúc boot).

**BLE fail lúc boot — safe mode:** `ble_prov_start` thử tối đa 3 lần cách nhau 5 s, vẫn fail → board vào safe mode: đứng yên, không provisioning/không telemetry, relay giữ nguyên, **nút BOOT vẫn hoạt động**. Log init chia bước: `step 1/5 ok: auto-stop timer ready` → … → `step 5/5 ok: NimBLE host task started`; kèm diagnostics `BLE diagnostics: …` (free heap, MAC eFuse/STA, `BT_ENABLED`/`BT_NIMBLE_ENABLED`). Mã lỗi hay gặp:

| Log | Ý nghĩa | Xử lý |
|---|---|---|
| `step 2/5 … BLE_INIT: controller init failed` (0x101 NO_MEM, 0x103 INVALID_STATE) | BT controller init lỗi | Chụp log serial; kiểm tra build đúng target `esp32` |
| `step 2/5 … BLE_INIT: controller enable failed` | Radio bật thất bại | Thử nguồn USB khác |
| `step 2/5 … BLE_INIT: nimble host init failed` | Hết mbuf/HCI buffer | Xem `free heap` trong diagnostics |
| `step 4/5 (service registration) failed; rc=0x…` | Lỗi đăng ký service/GAP | Gửi `rc` kèm log |
| `BT_ENABLED is NOT set` / `BT_NIMBLE_ENABLED is NOT set` | Image build thiếu BLE | `idf.py menuconfig` bật lại rồi build/flash |
| `wifi_conn_apply_credentials failed: ESP_ERR_WIFI_STATE` (0x3006) | PROVISION lần 2 bị từ chối vì driver đang giữa attempt cũ | Cập nhật firmware bản có fix (apply mới hủy attempt trước khi nạp credentials) |

Bắt serial khi board lỗi:

```bash
. ~/.espressif/tools/activate_idf_v6.0.1.sh
cd firmware/esp32-telemetry
python "$IDF_PATH/tools/idf.py" -p /dev/ttyUSB0 monitor
```

## Contract MQTT (boards)

`{prefix}` = `TOPIC_PREFIX` (default `smarthome`); `{boardId}` do firmware đặt. Firmware tự publish mọi topic dưới đây; app mobile nói thẳng contract này qua WS 9001 — backend không đứng giữa app và firmware (backend chỉ ingest telemetry).

| Hướng | Topic | Payload | QoS / retained |
|---|---|---|---|
| Descriptor → app/backend | `{prefix}/boards/{boardId}/descriptor` | JSON descriptor | 1 / **retained** |
| Telemetry → backend | `{prefix}/boards/{boardId}/telemetry` | JSON telemetry v2 | 1 / không |
| Sensor state → app | `{prefix}/boards/{boardId}/sensors/{S}/state` | số thuần (vd `25.74`) | 1 / **retained** |
| Trạng thái (LWT) → app | `{prefix}/boards/{boardId}/status` | `online` / `offline` thuần | 1 / **retained** |
| Relay state → app | `{prefix}/boards/{boardId}/relays/{K}/state` | `ON` / `OFF` thuần | 1 / **retained** |
| App → relay | `{prefix}/boards/{boardId}/relays/{K}/set` | `ON` / `OFF` thuần | 1 / không |

Descriptor (retained, firmware publish lại mỗi lần connect — kể cả reconnect):

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

Backend subscribe `{prefix}/boards/+/descriptor` (nạp registry cache) và `{prefix}/boards/+/telemetry`. Telemetry v2 map **data-driven** theo descriptor — ví dụ trên ghi point measurement `sensors`, tags `{boardId="0", roomId="0"}`, fields `temperature=28.5, humidity=71`. Thêm kênh/cảm biến mới chỉ cần descriptor mới, không đổi logic backend. Điểm v2 ghi với tags `{boardId, roomId: boardId}` (quy ước 1:1 — dữ liệu cũ không backfill).

Telemetry v2:

```json
{"schemaVersion":2,"boardId":"0","values":{"S1":28.5,"S2":71}}
```

- `boardId` trong payload phải khớp segment topic; `schemaVersion` phải là `2`; mọi giá trị phải là số finite (schema v2 không range-check vật lý).
- Hành vi WARN — đều bỏ an toàn, không crash: kênh lạ → WARN + bỏ kênh đó (các kênh hợp lệ còn lại vẫn ghi); sau lọc không còn kênh hợp lệ → bỏ point; telemetry đến trước descriptor (cold-start) → bỏ point, **không queue** — chu kỳ 5 s sau tự ổn; descriptor sai shape/JSON hỏng/boardId lệch topic → bỏ.

### Điều khiển relay qua CLI

Thay `<user>`, `<pass>` bằng giá trị broker (URL-encode password nếu chứa ký tự đặc biệt); CLI chạy trong container `amqtt` — host không cần client MQTT riêng:

```bash
# Gửi lệnh per-channel (payload thuần ON/OFF)
docker exec -it "$(docker compose ps -q amqtt)" amqtt_pub \
  --url "mqtt://<user>:<pass>@127.0.0.1:1883" \
  -t smarthome/boards/0/relays/K1/set -m ON

# Xem state retained (app đọc topic này để vẽ UI) — Ctrl+C dừng
docker exec -it "$(docker compose ps -q amqtt)" amqtt_sub \
  --url "mqtt://<user>:<pass>@127.0.0.1:1883" \
  -t 'smarthome/boards/0/relays/+/state'
```

- Firmware subscribe `…/relays/+/set` (QoS 1); kênh (`K1`/`K2`/`K3`, mở rộng `K4+` không đổi protocol) parse từ segment topic. Kênh/payload lạ → WARN + bỏ, không crash.
- State publish QoS 1 + retained sau mỗi lệnh; khi connect firmware publish lại state **tất cả** kênh.
- Tên relay thân thiện ("Đèn phòng khách") do **tầng app** đặt — firmware chỉ hiểu kênh vật lý; đổi tên không cần reflash. Thêm kênh K4+: một dòng trong `components/relay/relay.c` + một Kconfig `RELAY_K4_GPIO`.

### Giả lập không cần phần cứng

Sau khi stack chạy, publish trực tiếp vào broker từ trong container `amqtt` (password chứa `@ : / ? # %`… phải URL-encode trong `--url` — `amqtt_pub` không có flag user/pass riêng). Phải publish **descriptor trước** (retained), rồi telemetry:

```bash
# Descriptor (retained)
docker exec -it "$(docker compose ps -q amqtt)" amqtt_pub \
  --url "mqtt://esp32:<password>@127.0.0.1:1883" -r \
  -t "smarthome/boards/0/descriptor" \
  -m '{"schemaVersion":1,"boardId":"0","boardType":"A","sensors":[{"channel":"S1","field":"temperature","unit":"°C"},{"channel":"S2","field":"humidity","unit":"%"}],"relays":[{"channel":"K1"},{"channel":"K2"},{"channel":"K3"}]}'

# Telemetry v2 hợp lệ (KHÔNG retained)
docker exec -it "$(docker compose ps -q amqtt)" amqtt_pub \
  --url "mqtt://esp32:<password>@127.0.0.1:1883" \
  -t "smarthome/boards/0/telemetry" \
  -m '{"schemaVersion":2,"boardId":"0","values":{"S1":28.5,"S2":71}}'

# Payload LỖI (schemaVersion phải là 2) — backend log + bỏ qua, không crash
docker exec -it "$(docker compose ps -q amqtt)" amqtt_pub \
  --url "mqtt://esp32:<password>@127.0.0.1:1883" \
  -t "smarthome/boards/0/telemetry" \
  -m '{"schemaVersion":1,"deviceId":"0"}'
```

Xác nhận qua `docker compose logs backend` (descriptor nạp registry, telemetry hợp lệ ghi Influx, payload lỗi log chi tiết rồi bỏ qua); truy vấn kết quả theo mục [Truy vấn dữ liệu](#truy-vấn-dữ-liệu).

## Truy vấn dữ liệu

Chạy trên host, nối thẳng InfluxDB local qua port 8086. Cần 4 biến môi trường (script không tự đọc `.env`):

```bash
export INFLUX_URL=http://localhost:8086
export INFLUX_TOKEN=<giá trị INFLUX_TOKEN trong .env>
export INFLUX_ORG=<giá trị INFLUX_ORG trong .env>
export INFLUX_BUCKET=<giá trị INFLUX_BUCKET trong .env>

npx tsx src/query/latest.ts living-room    # bản ghi mới nhất của một room
npx tsx src/query/history.ts living-room   # lịch sử 1 giờ gần nhất
```

- Script dùng Flux qua `@influxdata/influxdb-client` (query API v2). Measurement `sensors`, tags `{boardId, roomId}`.
- `roomId`/bucket ghép thẳng vào chuỗi Flux (client v2 không bind param cho tag value) — script validate regex `^[a-zA-Z0-9_-]+$` trước khi ghép.
- Đầu ra mỗi dòng: `<thời điểm UTC>  <roomId>  <field>=<value>`.

App mobile query trực tiếp Flux API v2 (`POST /api/v2/query`) bằng token read-only — kiểm tra nhanh bằng `curl`:

```bash
curl -s --request POST "http://localhost:8086/api/v2/query?org=smarthome" \
  --header "Authorization: Token <INFLUX_TOKEN>" \
  --header "Content-Type: application/vnd.flux" \
  --data 'from(bucket:"telemetry") |> range(start:-1h) |> filter(fn:(r)=> r._measurement=="sensors" and r.roomId=="living-room") |> filter(fn:(r)=> r._field=="temperature" or r._field=="humidity")'
```

## InfluxDB vận hành

### Token read-only cho app

App query Flux bằng token **read-only** — không dùng token admin. Token gắn vào user riêng `app-mobile` (không có mật khẩu — chỉ để gắn token, không đăng nhập UI) và chỉ cho phép **đọc** bucket `telemetry`.

```bash
cd ~/smarthome   # hoặc thư mục repo nếu chạy stack developer

# 1) Tạo user app-mobile (chạy lại báo "already exists" — vô hại; WARN thiếu password là bình thường):
docker exec "$(docker compose ps -q influxdb)" influx user create \
  --name app-mobile --org smarthome

# 2) Tạo token — influx CLI yêu cầu BUCKET ID, không nhận tên, nên lệnh tự tra ID bucket telemetry:
docker exec "$(docker compose ps -q influxdb)" sh -c \
  'influx auth create --user app-mobile --read-bucket "$(influx bucket list --name telemetry --hide-headers | cut -f1)" --description "mobile app read-only"'
```

Ghi chú:

- `--read-bucket` chỉ nhận **ID bucket 16 ký tự** — truyền tên (`--read-bucket telemetry`) sẽ lỗi `invalid bucket ID ... (did you pass a bucket name instead of an ID?)`. Xem ID thủ công: `docker exec "$(docker compose ps -q influxdb)" influx bucket list --name telemetry --hide-headers` (cột đầu).
- Token in ra chỉ có permission `[read:orgs/<org-id>/buckets/<bucket-id>]` — đúng nghĩa read-only. Chạy lại lệnh 2 tạo thêm token mới; xem/thu hồi token: `influx auth list`, `influx auth delete <ID>`.
- Đổi `smarthome` (org) và `telemetry` (bucket) theo `INFLUXDB_INIT_ORG`/`INFLUXDB_INIT_BUCKET` nếu cài với giá trị khác mặc định.
- Gợi ý in kèm `pairing-code` khi thiếu token hiển thị `--read-bucket <tên-bucket>` — dùng lệnh ở trên thay vì gợi ý đó (gợi ý sẽ lỗi khi user chưa tồn tại và `--read-bucket` không nhận tên).
- Copy token vào `.env` (`INFLUX_APP_TOKEN=...`) rồi chạy lại `bash smarthome-deploy.sh credentials-qr` để QR cho app kèm `influxToken`.

### Sao lưu và phục hồi dữ liệu

Volume `influxdb-data` là named volume — tên thực tế có prefix tên project (`docker volume ls | grep influxdb-data`; image-only deploy mặc định project `smarthome` → `smarthome_influxdb-data`). Backup toàn bộ thư mục dữ liệu:

```bash
VOL="$(docker volume ls --format '{{.Name}}' | grep influxdb-data | head -n1)"
docker run --rm \
  -v "$VOL":/data \
  -v "$(pwd):/backup" \
  alpine tar czf /backup/influxdb-backup.tar.gz -C /data .
```

Khôi phục (dừng stack trước để tránh ghi dở):

```bash
VOL="$(docker volume ls --format '{{.Name}}' | grep influxdb-data | head -n1)"
docker compose stop influxdb
docker run --rm \
  -v "$VOL":/data \
  -v "$(pwd):/backup" \
  alpine sh -c "rm -rf /data/* && tar xzf /backup/influxdb-backup.tar.gz -C /data"
docker compose start influxdb
```

**Tuyệt đối không `docker compose down -v`** — xóa volume là mất dữ liệu telemetry/session. Update lớn (đổi major version InfluxDB…) luôn backup trước.

## Kiểm thử

Tự động (không cần phần cứng):

```bash
npm run typecheck
npm test
```

Các phần bắt buộc phải có ESP32 thật để xác minh:

1. **Đọc SHT3x thật qua I2C** — CRC-8 và timing thực tế; cần external pull-up 4.7 kΩ nếu module không có sẵn.
2. **Wi-Fi LAN thật** — retry/backoff khi AP rớt, đổi IP/DHCP.
3. **LWT offline** — rút nguồn ESP32 → broker publish retained `offline` lên `{prefix}/boards/{boardId}/status`; connect lại → `online` (kèm descriptor + state relay).
4. **QoS 1 end-to-end** — mất mạng rồi có lại: message retransmit tới khi broker nhận; outbox không tăng vô hạn (`MQTT_OUTBOX_LIMIT` — đầy thì drop + log).
5. **Chân SDA/SCL** theo loại module cụ thể — đổi qua `SHT3X_I2C_SDA_GPIO`/`SHT3X_I2C_SCL_GPIO`.
6. **BLE provisioning trên board thật** — RF/BLE stack không mô phỏng được trên host: build `WIFI_SSID=""`, quét `IoTBoard-*` bằng app, pair Just Works, nhận `CONNECTED`/`FAILED:*`, BLE tự tắt 30 s, boot sau dùng NVS, nút BOOT 5 s quay lại provisioning.

## Tài liệu liên quan

- [../README.md](../README.md) — hướng dẫn sử dụng cho người dùng cuối.
- [frontend-setup.md](frontend-setup.md) — cấu hình kết nối cho app/web frontend.
- `PLAN.md`, `PROJECT_MEMORY.md` — kế hoạch và ghi chú kiến trúc nội bộ dự án.
