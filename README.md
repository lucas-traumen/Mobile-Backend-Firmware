# Mobile_Backend — luồng telemetry SHT30/SHT31 → MQTT → InfluxDB 2 local

## 1. Tổng quan

Hệ thống thu thập nhiệt độ/độ ẩm Smart Home với luồng dữ liệu:

```
ESP32 + SHT30/SHT31 (I2C)  --Wi-Fi/MQTT-->  amqtt (broker)  -->  backend Node.js/TypeScript (Docker)  -->  InfluxDB 2 local (Docker)
```

- **Firmware v2** (`firmware/esp32-telemetry/`, M16b — **bản hiện tại**): ESP-IDF v6.0.1 native (C, CMake, FreeRTOS). Đọc SHT30/SHT31 qua I2C và nói thẳng **contract board-centric**: tự publish descriptor (kèm `displayName` optional), telemetry v2 theo kênh, sensor/relay state, status lên `{prefix}/boards/{boardId}/...` (mục 14.2). Board chưa có credentials thì tự bật BLE provisioning cho app cấu hình (mục 10).
- **Backend** (`src/`): ingest telemetry v2 (M14a) — subscribe `{prefix}/boards/{boardId}/telemetry` (contract board-centric), map `values` → fields Influx theo descriptor từng board (mục 8.1), tag `{boardId, roomId: boardId}`; descriptor do firmware publish được cache vào registry.
- **Hạ tầng** (Docker Compose, 3 service): `amqtt`, `influxdb`, `backend` — toàn bộ server-side chạy trong container.

> **M11 — revert InfluxDB Cloud Serverless → InfluxDB 2.7 local:** user chốt kiến trúc local-only (dễ bảo hành, backup volume). Đường **ghi** giữ nguyên write API v2 (`src/influx/influx-writer.ts`), chỉ đổi schema cho khớp frontend; đường **đọc** (`src/query/*.ts`) quay lại **Flux** qua `@influxdata/influxdb-client` để app mobile query trực tiếp InfluxDB local.

> **Schema mới (M11):** measurement `environment` → `sensors`, tag `device_id`/`room_id` → `roomId` (mỗi phòng một ESP, `deviceId` ≡ `roomId`). App mobile query trực tiếp Flux API v2 bằng token read-only — không có HTTP API backend.

**Phạm vi backend: luồng cảm biến → database.** Không có UI, không HTTP API — truy vấn dữ liệu bằng script CLI (mục 11). Phần điều khiển relay (mục 9) do firmware ESP32 nhận lệnh qua MQTT.

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
| `WIFI_SSID` | Tên Wi-Fi cho flow classic; **rỗng (default)** = board boot vào BLE provisioning khi NVS cũng trống (mục 10) | rỗng |
| `WIFI_PASSWORD` | Mật khẩu Wi-Fi (rỗng chỉ hợp lệ cho mạng mở) | rỗng |
| `MQTT_BROKER_URI` | URI broker, ví dụ `mqtt://192.168.1.50:1883` (IP LAN của máy chạy Docker); scheme tùy chọn — thiếu `mqtt://` firmware tự bù (M19) | `mqtt://192.168.1.100:1883` |
| `MQTT_USER` / `MQTT_PASSWORD` | Tài khoản MQTT — cùng giá trị với `MQTT_USER`/`MQTT_PASSWORD` trong `.env` của server | rỗng |
| `MQTT_TOPIC_PREFIX` | Prefix contract boards — phải khớp `TOPIC_PREFIX` của backend (`.env`) | `smarthome` |
| `DEVICE_ID` | Dùng làm **boardId** trong topic `{prefix}/boards/{boardId}/...`, làm MQTT client id và trong tên BLE `IoTBoard-{boardId}` (mục 10). **Rỗng (default, M18)** = board tự sinh lúc boot: hex-8 của 4 byte cuối MAC Wi-Fi STA (vd `5c:01:3b:6b:af:6c` → `3b6baf6c`) — unique theo phần cứng, khuyến nghị cho board mới; điền giá trị chỉ khi muốn id cố định tự chọn | rỗng |
| `BOARD_TYPE` | Mã loại board — `boardType` trong descriptor MQTT và Device Info BLE (mục 10.2); frontend hiển thị làm tên board | `IoT_ESP32-S2R3` |
| `BOARD_DISPLAY_NAME` | Tên hiển thị đưa vào descriptor (`displayName`); rỗng = bỏ hẳn field | rỗng |
| `ROOM_ID` | Legacy firmware v1 — v2 không gửi roomId trong topic/payload nào; giữ option để `sdkconfig` cũ không cảnh báo | `living-room` |
| `SENSOR_PERIOD_MS` | Chu kỳ đo (ms), khoảng 1000–3600000 | 5000 |
| `MQTT_OUTBOX_LIMIT` | Giới hạn outbox esp-mqtt (byte) khi mất kết nối | 4096 |

**Kể từ M16b, `WIFI_SSID` rỗng không còn làm firmware từ chối khởi động** — khi NVS cũng không có credentials, board boot vào chế độ BLE provisioning (mục 10) và chờ app mobile đẩy cấu hình. Chỉ đặt `WIFI_SSID` qua menuconfig khi muốn fix cứng Wi-Fi vào image; board provision qua app giữ giá trị mặc định rỗng (mục 10.3).

**Kể từ M18, `DEVICE_ID` rỗng (default) là cấu hình khuyến nghị:** board tự sinh `boardId` lúc boot từ phần cứng — 4 byte cuối của MAC Wi-Fi STA dạng hex-8 lowercase (vd MAC `5c:01:3b:6b:af:6c` → `boardId` `3b6baf6c`), log boot in rõ `boardId="3b6baf6c" (from MAC)`. MAC được đốt sẵn trong eFuse Espressif nên id unique theo board, không đổi qua reboot/reflash, không cần lưu NVS. Một binary generic phục vụ cả hai chế độ: điền `DEVICE_ID` khác rỗng thì firmware dùng nguyên giá trị đó (back-compat, ưu tiên hơn MAC). Cả 3 đường boot (NVS/Kconfig/BLE) đều dùng chung boardId đã resolve — lấy MAC từ board bằng `scripts/board-qr.sh` (mục 10.5).

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
- Log mong đợi khi chạy tốt: `mqtt_app` connected → `main: started: board=... prefix=smarthome ...` → `telemetry sent: T=... C, RH=... %` mỗi chu kỳ đo.

> **Firmware v2 (M14b) — trước khi flash một board đang chạy v1:** reflash làm board **ngừng phát mọi topic contract room cũ** (`smarthome/{deviceId}/telemetry|status|relay/...`) ngay lập tức và chỉ nói contract boards `{prefix}/boards/{boardId}/...` — app phải đã dùng boards contract (mục 14.2), nếu không board sẽ "biến mất" khỏi app. Retained v1 còn sót trên broker nếu muốn dọn thì publish payload rỗng (`amqtt_pub ... -n -r`) lên đúng topic đó.

> **Lưu ý `idf.py` trong script non-interactive:** `idf.py` là alias bash do script activate tạo ra, không tồn tại trong môi trường shell riêng biệt (CI, script chạy nền). Khi đó gọi trực tiếp:
>
> ```bash
> /home/lucas/.espressif/tools/python/v6.0.1/venv/bin/python /home/lucas/.espressif/v6.0.1/esp-idf/tools/idf.py build
> ```

## 6. Chạy server (Docker Compose)

> Server mới cài từ OS trắng? Nhảy thẳng tới mục con **[Khởi động server mới từ số 0 (`smarthome.sh`)](#khởi-động-server-mới-từ-số-0-smarthomesh)** ở cuối mục 6 — một entrypoint gộp tất cả các bước dưới đây.

```bash
cp .env.example .env
# điền giá trị thật vào .env:
#   MQTT_USER, MQTT_PASSWORD                  — tài khoản broker (ESP32 dùng cùng cặp này)
#   MQTT_APP_PASSWORD                         — (tuỳ chọn) user app mobile cho WS 9001; để trống thì setup tự sinh + in log
#   TOPIC_PREFIX                              — (tuỳ chọn) prefix contract boards, default smarthome
#   INFLUXDB_INIT_USERNAME, INFLUXDB_INIT_PASSWORD — tài khoản admin của InfluxDB local
#   INFLUXDB_INIT_ORG, INFLUXDB_INIT_BUCKET, INFLUXDB_INIT_ADMIN_TOKEN — org/bucket/token tạo lúc setup
#   INFLUX_TOKEN, INFLUX_ORG, INFLUX_BUCKET   — backend dùng lại đúng org/bucket/token trên
# giữ nguyên MQTT_URL=mqtt://amqtt:1883 và INFLUX_URL=http://influxdb:8086 (tên service nội compose)

docker compose up -d --build
docker compose ps        # amqtt + influxdb phải ở trạng thái Up (healthy), backend Up
docker compose logs backend   # backend subscribe {prefix}/boards/+/descriptor + {prefix}/boards/+/telemetry, nối InfluxDB local thành công
```

Lưu ý:

- Broker chỉ chấp nhận user/pass (anonymous bị cấm); password file được sinh lúc container start từ `.env` (xem `scripts/amqtt-setup.sh`), không nằm trong repo.
- **Hai listener**: `1883` (MQTT thường — ESP32 + backend) và `9001` (MQTT over WebSocket — app mobile), dùng chung một password file. Script setup sinh **hai user**: `MQTT_USER` (mặc định `esp32`) và `MQTT_APP_USER` (mặc định `app`). `MQTT_APP_PASSWORD` để trống thì script tự sinh random và in ra `docker compose logs amqtt` — lưu lại giá trị đó vào `.env` cho ổn định (xem mục 14).
- **ESP32 nối broker bằng IP LAN của máy chạy Docker** (port 1883 đã map ra host), ví dụ `mqtt://192.168.1.50:1883` trong menuconfig — *không* dùng `localhost` vì `localhost` với ESP32 là chính nó. Scheme `mqtt://` là tùy chọn (M19): gửi `192.168.1.50:1883` hay `mqtt://192.168.1.50:1883` đều như nhau — firmware tự bù scheme khi thiếu.
- **InfluxDB local** (`influxdb:2.7.10`) tự setup org/bucket/admin token lần start đầu từ bộ `INFLUXDB_INIT_*`; volume `influxdb-data` giữ dữ liệu giữa các lần `up`. Port 8086 map ra host để query CLI/app mobile nối `http://localhost:8086`.
- Bộ `INFLUX_*` (backend đọc) phải trỏ CÙNG org/bucket/token với bộ `INFLUXDB_INIT_*` — image influxdb chỉ hiểu prefix `INFLUXDB_INIT_*`, còn backend đọc `INFLUX_*`.
- Backend trong container nối broker/influx bằng service name (`mqtt://amqtt:1883`, `http://influxdb:8086`).

### Khởi tạo server (mDNS discovery)

Một lệnh thay cho chuỗi lệnh thủ công ở trên — bootstrap idempotent (chạy lại vô hại), do user chạy tương tác vì có hỏi sudo password:

```bash
bash scripts/server-init.sh
```

Script lần lượt:

1. Kiểm tra `avahi-daemon` active; chưa thì thử `sudo systemctl enable --now avahi-daemon` (vẫn fail → dừng, hướng dẫn cài `avahi-daemon`).
2. Kiểm tra `.env` ở gốc repo — thiếu thì dừng, hướng dẫn `cp .env.example .env` (chỉ kiểm tra tồn tại, không đọc/in secret).
3. `docker compose config --quiet` — validate compose + `.env` trước khi build.
4. `sudo cp avahi/smarthome.service /etc/avahi/services/` — cài file quảng bá mDNS; `avahi-daemon` tự nhận file service mới, không cần restart.
5. `docker compose up -d --build` (3 service: amqtt, influxdb, backend).
6. Đợi container healthy tối đa 120 s — hết giờ thì in trạng thái + cảnh báo, không abort (build chậm, InfluxDB setup lần đầu vẫn đang khởi động là bình thường).
7. Verify quảng bá bằng `avahi-browse -rt _smarthome._tcp` (in hostname/IP + port); thiếu `avahi-utils` thì chỉ WARN + gợi ý `sudo apt-get install -y avahi-utils`, không fail.
8. In tóm tắt địa chỉ + 3 port (1883 board/backend MQTT, 9001 app WebSocket, 8086 Influx) + gợi ý firewall nếu ufw đang bật.

**mDNS quảng bá gì:** service `_smarthome._tcp` port **9001** (MQTT WebSocket — app mobile nối thẳng) kèm TXT record `prefix=smarthome`, `influx_port=8086`, `influx_org=smarthome`, `influx_bucket=telemetry`, tên hiển thị **Smart Home Server** (template tại `avahi/smarthome.service`). Type `_smarthome._tcp` là contract riêng của app (custom type, không trùng broker MQTT thường — tránh nhầm với broker MQTT khác cùng trên LAN; hai bên khớp theo `mdnsDiscoveryContract.ts` bên repo Mobile_Frontend); TXT keys dùng **gạch dưới** (`influx_port`, `influx_org`, `influx_bucket` — không phải kiểu gạch ngang). App mobile trong cùng Wi-Fi dò tìm server qua DNS-SD (mDNS/Bonjour/NSD) — app tự lấy địa chỉ server + port, không ai phải gõ IP tay; DHCP đổi IP thì quảng bá tự bám theo địa chỉ mới.

> **Giới hạn + sự cố thường gặp:**
>
> - **AP isolation** (Wi-Fi quán cà phê/văn phòng/khách sạn chặn multicast giữa các client): app không dò tìm được server → nhập IP tay như cũ (mục 14.3).
> - **Laptop sleep** = server mất với toàn mạng — tắt sleep khi demo.
> - Server quảng bá **mọi IP của laptop** (gồm docker bridge 172.x, tailscale 100.x) — chỉ IP Wi-Fi LAN dùng được, app phải tự lọc (cùng luật với `detect_lan_ip` trong `scripts/pairing-code.sh`).
> - Thiếu `avahi-utils` → script không verify được quảng bá (chỉ cảnh báo); cài: `sudo apt-get install -y avahi-utils`.
> - **ufw bật** → mở 3 port: `sudo ufw allow 1883/tcp`, `sudo ufw allow 9001/tcp`, `sudo ufw allow 8086/tcp`.

> Secret (MQTT pass, token Influx) **không** đi qua mDNS — TXT record chỉ chứa prefix + thông số Influx (port/org/bucket), không secret; app vẫn nhập trong màn hình ghép nối (mục 14.3/14.4).

### Khởi động server mới từ số 0 (`smarthome.sh`)

Runbook một trạm cho server mới: OS trắng → cài tool host → clone repo → `init` → kiểm tra → reboot/update. `scripts/smarthome.sh` là entrypoint trên host, chạy SAU khi repo đã clone (script nằm trong repo nên không thể tồn tại trước khi clone); `init` tự bọc đúng chuỗi thủ công + `server-init.sh` hai mục phía trên. Script **không tự cài package** — thiếu gì chỉ in cách cài.

**Bước 1 — cài tool trên host.** Docker Engine + Compose v2 và Git cài trực tiếp trên host — **Docker không dùng để cài Docker**. Compose v2 phải là plugin `docker compose` đi kèm Docker Engine mới; `apt install docker`/`docker.io` kiểu distro cũ **không** đảm bảo có Compose v2 — cài Engine + plugin theo hướng dẫn chính thức Docker. Lệnh mẫu Ubuntu/Debian (đối chiếu lại với docs.docker.com theo bản distro của bạn):

```bash
# Docker Engine + Compose v2 (repo chính thức của Docker):
sudo apt-get update
sudo apt-get install -y ca-certificates curl git
sudo install -m 0755 -d /etc/apt/keyrings
sudo curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc
sudo chmod a+r /etc/apt/keyrings/docker.asc
echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc] \
https://download.docker.com/linux/ubuntu $(. /etc/os-release && echo "$VERSION_CODENAME") stable" \
  | sudo tee /etc/apt/sources.list.d/docker.list > /dev/null
sudo apt-get update
sudo apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
sudo usermod -aG docker "$USER"   # đăng xuất/đăng nhập lại để chạy docker không cần sudo

# Các gói host còn lại cho init/mDNS/QR:
sudo apt-get install -y openssl python3 avahi-daemon avahi-utils qrencode
```

(Thiếu `docker compose`, `openssl` hoặc `python3` thì `init` **dừng** ngay ở bước tương ứng và in đúng lệnh cài cần chạy. Thiếu `avahi-daemon`/`avahi-utils`/`qrencode` thì `server-init`/`status`/các wrapper QR chỉ WARN hoặc tự báo lỗi theo script của chúng — ví dụ thiếu `qrencode` chỉ mất phần QR, JSON vẫn in.)

**Bước 2 — clone + checkout bản muốn chạy:**

```bash
git clone <URL_REPO> Mobile_Backend
cd Mobile_Backend
git checkout <TAG_HOẶC_COMMIT>   # pin đúng bản phát hành, không chạy tay từ nhánh dev
```

**Bước 3 — init.** `bash scripts/smarthome.sh init` (hoặc mở menu, chọn 1). Chuỗi bên trong, fail bước nào dừng tại đó và không gọi server-init:

1. `.env` **đã có** → giữ nguyên, không ghi đè, không sinh lại secret. **Chưa có** → tạo từ `.env.example` qua temp file cùng thư mục + atomic `mv`, `chmod 600` từ trước khi ghi secret: `MQTT_PASSWORD`, `MQTT_APP_PASSWORD`, `INFLUXDB_INIT_PASSWORD` random bằng `openssl rand -hex`; một token random dùng chung cho `INFLUXDB_INIT_ADMIN_TOKEN` và `INFLUX_TOKEN` (hai biến này bắt buộc đồng nhất — xem `.env.example`). Secret không nằm trong argv/log; script xác nhận không còn placeholder nào trước khi nhận file. `INFLUX_APP_TOKEN` (token read-only cho app) **không** tự sinh — tạo tay rồi thêm vào `.env` sau (mục 14.4).
2. `docker compose config --quiet` — validate compose + `.env` trước mọi thao tác image.
3. Preload, **chưa `up`**: `docker compose pull --ignore-buildable` (Compose cũ không có flag này → pull từng service dùng image có sẵn, vd `influxdb`) rồi `docker compose build --pull`.
4. Gọi `scripts/server-init.sh` — avahi + `docker compose up -d --build` + đợi amqtt/influxdb healthy + verify quảng bá `_smarthome._tcp` (chi tiết 8 bước ở mục `Khởi tạo server (mDNS discovery)` phía trên).

**Bước 4 — dùng menu / direct mode.** Chạy `bash scripts/smarthome.sh` không đối số:

| Chọn | Tác vụ |
|---|---|
| `1` / `init` | Init server mới (chuỗi 4 bước trên) |
| `2` / `credentials-qr` | QR credentials cho app mobile (chứa secret) |
| `3` / `board-qr` | Nhãn QR board — hỏi boardId hoặc MAC |
| `4` / `pairing-code` | Khối mã ghép nối |
| `5` / `status` | `docker compose ps` + quét mDNS `_smarthome._tcp` |
| `0` / `exit` | Thoát |

Chọn sai/Enter trống quay lại menu; EOF/Ctrl-C thoát sạch. Direct mode cho script/lệnh một lần:

```bash
bash scripts/smarthome.sh init
bash scripts/smarthome.sh credentials-qr
bash scripts/smarthome.sh pairing-code
bash scripts/smarthome.sh status
```

**Bước 5 — nhãn QR board ngay trên server (không cần ESP-IDF/esptool).** Server thường không có môi trường ESP-IDF nên `board-qr` qua menu hỏi nhập thẳng `boardId` (vd `3b6baf6c`) hoặc MAC Wi-Fi STA (vd `5c:01:3b:6b:af:6c` — lấy từ log boot firmware, dòng `Wi-Fi STA MAC`), **không dò serial**. Truyền thẳng cũng được, args đi nguyên vẹn tới `board-qr.sh`:

```bash
bash scripts/smarthome.sh board-qr --board-id 3b6baf6c
bash scripts/smarthome.sh board-qr -m 5c:01:3b:6b:af:6c
bash scripts/smarthome.sh board-qr -m 5c:01:3b:6b:af:6c -t IoT_ESP32-S2R3
```

Giá trị nhập qua menu được validate regex (boardId `[a-zA-Z0-9_-]+` không bắt đầu bằng `-`; MAC 6 cặp hex) trước khi gọi — không truyền nổi flag lạ vào script gốc. Chi tiết thuật toán derive boardId từ MAC: mục 10.5.

**Bước 6 — kiểm tra sau init.** `bash scripts/smarthome.sh status`: `amqtt` + `influxdb` phải `Up (healthy)`, `backend` `Up`; phần mDNS phải thấy `_smarthome._tcp`. Từ máy khác cùng Wi-Fi: `avahi-browse -rt _smarthome._tcp`. App mobile chỉ cần cùng Wi-Fi là tự dò server (mục 14).

**Reboot.** Cả 3 service đều `restart: unless-stopped` và Docker chạy theo systemd → reboot xong stack tự sống lại, không cần chạy lệnh nào; kiểm tra bằng `status`. Server không được sleep (mục `Khởi tạo server` phía trên).

**Update an toàn:**

```bash
cd Mobile_Backend
git fetch
git checkout <TAG_MỚI>
bash scripts/smarthome.sh init   # .env giữ nguyên; pull/build bản mới rồi up --build
```

`.env` và volume `influxdb-data` không bị đụng; backup volume trước update lớn (mục `Backup volume InfluxDB`).

**Secret safety.** `credentials-qr`/`pairing-code` in MQTT password + Influx token nguyên văn ra stdout/QR — chỉ chạy khi chủ động ghép nối board/app mới; không chụp màn hình public, không dán vào chat/commit/log. `.env` mode `600`, đã gitignore — không commit, không copy kèm repo.

**Troubleshooting nhanh:**

| Hiện tượng | Kiểm tra / xử lý |
|---|---|
| `init` dừng ngay "docker compose" | Docker Engine/Compose v2 chưa cài, hoặc user chưa vào group `docker` (đăng nhập lại sau `usermod -aG docker`) |
| `init` dừng ở `docker compose config` | `.env` thiếu/sai giá trị — sửa rồi chạy lại `init` (`.env` hiện có không bị ghi đè) |
| Pull/build fail | Mạng/registry; `init` idempotent — sửa xong chạy lại |
| Container chưa healthy sau 120 s | `docker compose logs -f amqtt` / `influxdb` / `backend`; lần đầu InfluxDB setup có thể chậm — theo dõi rồi `status` lại |
| Không thấy `_smarthome._tcp` | Hạn chế mDNS (AP isolation, laptop sleep, thiếu `avahi-utils`…) — xem mục `Khởi tạo server (mDNS discovery)` phía trên |
| `credentials-qr` báo `MQTT_APP_PASSWORD` trống | `docker compose logs amqtt \| grep MQTT_APP_PASSWORD` rồi ghi giá trị đó vào `.env` |
| Menu chọn gì cũng báo thiếu file `scripts/*` | Repo clone thiếu/ sai tag — checkout lại đúng bản phát hành |

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

- Khác với chạy trong compose: `MQTT_URL` và `INFLUX_URL` phải là `localhost` vì port 1883/8086 đã map ra host (service name `amqtt`/`influxdb` chỉ resolve trong mạng compose).
- Thiếu biến nào sẽ báo lỗi ngay: `Missing required env var: <TÊN>`.
- Dừng: `Ctrl-C` (graceful shutdown — đóng writeApi Influx rồi disconnect MQTT).

## 8. Kiểm thử giả lập (không cần phần cứng)

Sau khi stack đã chạy (mục 6), publish trực tiếp vào broker từ trong container `amqtt`. Thay `<password>` bằng giá trị `MQTT_PASSWORD` trong `.env` (password chứa ký tự đặc biệt `@ : / ? # %`… phải URL-encode khi nhét vào `--url` — `amqtt_pub` không có flag user/pass riêng). Backend ingest telemetry v2 theo descriptor (chi tiết mục 8.1) nên phải publish **descriptor trước** (retained), rồi telemetry:

```bash
# Descriptor (retained) — backend nạp vào registry để map kênh → field
docker exec -it "$(docker compose ps -q amqtt)" amqtt_pub \
  --url "mqtt://esp32:<password>@127.0.0.1:1883" -r \
  -t "smarthome/boards/0/descriptor" \
  -m '{"schemaVersion":1,"boardId":"0","boardType":"A","sensors":[{"channel":"S1","field":"temperature","unit":"°C"},{"channel":"S2","field":"humidity","unit":"%"}],"relays":[{"channel":"K1"},{"channel":"K2"},{"channel":"K3"}]}'

# Telemetry v2 HỢP LỆ (KHÔNG retained) — backend map theo descriptor rồi ghi InfluxDB
docker exec -it "$(docker compose ps -q amqtt)" amqtt_pub \
  --url "mqtt://esp32:<password>@127.0.0.1:1883" \
  -t "smarthome/boards/0/telemetry" \
  -m '{"schemaVersion":2,"boardId":"0","values":{"S1":28.5,"S2":71}}'

# Payload LỖI (sai schema: schemaVersion phải là 2) — backend log + bỏ qua, không crash
docker exec -it "$(docker compose ps -q amqtt)" amqtt_pub \
  --url "mqtt://esp32:<password>@127.0.0.1:1883" \
  -t "smarthome/boards/0/telemetry" \
  -m '{"schemaVersion":1,"deviceId":"0"}'
```

Xác nhận:

- `docker compose logs backend` — descriptor được nạp vào registry; telemetry hợp lệ được map + ghi; payload lỗi có log chi tiết lỗi validation rồi bỏ qua.
- Lưu ý: `boardId` trong payload phải khớp segment topic — ví dụ topic `smarthome/boards/0/telemetry` nhưng payload ghi `boardId: "5"` là lệch, bị bỏ qua. Telemetry đến **trước** descriptor → WARN + bỏ point (không queue), chu kỳ kế tiếp tự ổn khi descriptor đã nạp.
- Truy vấn kết quả theo mục 11.

### 8.1. Telemetry v2 — contract boards, map theo descriptor (M14a)

Firmware v2 (M14b — bản hiện tại) nói thẳng contract board-centric: descriptor + telemetry v2 do **chính firmware** publish. Backend ingest đường này trên một MQTT connection duy nhất (một connection cho cả descriptor lẫn telemetry):

| Topic backend subscribe | Dùng cho |
|---|---|
| `{prefix}/boards/+/descriptor` | Nạp (retained) descriptor từng board vào registry — cache `boardId → descriptor` theo message mới nhất |
| `{prefix}/boards/+/telemetry` | Telemetry v2 — validate rồi ghi Influx |

Payload v2 và cách backend map:

```bash
# Descriptor (retained) — firmware v2 publish mỗi lần connect; backend cache bản mới nhất
docker exec -it "$(docker compose ps -q amqtt)" amqtt_pub \
  --url "mqtt://esp32:<password>@127.0.0.1:1883" -r \
  -t "smarthome/boards/0/descriptor" \
  -m '{"schemaVersion":1,"boardId":"0","boardType":"A","sensors":[{"channel":"S1","field":"temperature","unit":"°C"},{"channel":"S2","field":"humidity","unit":"%"}],"relays":[{"channel":"K1"},{"channel":"K2"},{"channel":"K3"}]}'

# Telemetry v2 (KHÔNG retained) — giá trị theo KÊNH, ý nghĩa kênh do descriptor khai báo
docker exec -it "$(docker compose ps -q amqtt)" amqtt_pub \
  --url "mqtt://esp32:<password>@127.0.0.1:1883" \
  -t "smarthome/boards/0/telemetry" \
  -m '{"schemaVersion":2,"boardId":"0","values":{"S1":28.5,"S2":71}}'
```

- **Map data-driven**: backend tra `descriptor.sensors` (channel → field) — ví dụ trên cho point measurement `sensors`, tags `{boardId="0", roomId="0"}`, fields `temperature=28.5, humidity=71`. Thêm kênh/cảm biến mới chỉ cần descriptor mới, không đổi logic backend.
- **boardId** trong payload phải khớp segment topic; `schemaVersion` phải là `2`; mọi giá trị phải là số finite (schema v2 **không range-check vật lý** — kênh generic và descriptor không mang khoảng giới hạn).
- Điểm v2 ghi với **tags `{boardId, roomId: boardId}`** (quy ước 1:1 còn hiệu lực — Lịch sử app cũ liên tục).

Hành vi WARN (đều bỏ an toàn, không crash):

| Tình huống | Hành vi |
|---|---|
| Kênh trong `values` không có trong descriptor | WARN từng kênh + bỏ kênh đó; các kênh hợp lệ còn lại vẫn ghi |
| Sau lọc không còn kênh hợp lệ | WARN + bỏ point |
| Descriptor chưa có (cold-start — board v2 mới connect, telemetry đến trước descriptor) | WARN + bỏ point, **không queue** — chu kỳ telemetry 5 s kế tiếp tự ổn khi descriptor đã nạp |
| Descriptor sai shape / JSON hỏng / boardId lệch segment topic | WARN + bỏ descriptor/point |

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

Firmware v2 (M14b) nhận lệnh và phát trạng thái theo **contract boards, per-channel**:

| Topic | Chiều | Payload |
|---|---|---|
| `{prefix}/boards/{boardId}/relays/{K}/set` | gửi lệnh | `ON`/`OFF` thuần (trim khoảng trắng) |
| `{prefix}/boards/{boardId}/relays/{K}/state` | nhận trạng thái | `ON`/`OFF` thuần, retained |

- Firmware subscribe `{prefix}/boards/{boardId}/relays/+/set` (QoS 1); kênh (`K1`/`K2`/`K3`, mở rộng `K4`…`Kn` không đổi protocol) được parse từ segment topic.
- Kênh lạ / payload lạ → log WARN + bỏ, **không crash, không publish gì**.
- State publish **QoS 1 + retained** sau mỗi lệnh hợp lệ; khi connect (kể cả reconnect) firmware publish lại state của **tất cả** kênh — app vẽ UI từ retained.
- Các topic relay v1 (`smarthome/{deviceId}/relay/set` JSON, `.../relay/K*/set`, `.../relay/state` JSON gộp) **đã bỏ khỏi firmware v2** — không còn đường nào dùng chúng.

### 9.3. Ví dụ CLI

Thay `<user>`, `<pass>` bằng giá trị broker (URL-encode password nếu chứa ký tự đặc biệt); `{boardId}` ví dụ `0`. CLI chạy trong container `amqtt` — host không cần cài client MQTT riêng:

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

**Ghi chú mở rộng:** tên relay thân thiện (ví dụ "Đèn phòng khách") do **tầng app** đặt — firmware chỉ hiểu kênh vật lý `K1`/`K2`/`K3`, nên đổi tên không cần reflash. Thêm kênh K4+: thêm một dòng trong `components/relay/relay.c` + một Kconfig `RELAY_K4_GPIO`, không đổi topic/protocol.

## 10. BLE provisioning (M16) — cấu hình board từ app mobile

Board chưa có credentials Wi-Fi (NVS namespace `bleprov` trống và `WIFI_SSID` rỗng — mục 4) tự bật **BLE provisioning**: quảng bá tên `IoTBoard-{boardId}`, app mobile quét QR board (mục 10.5), nối GATT, đẩy SSID/pass + broker URI + tài khoản MQTT rồi gửi lệnh `PROVISION`. Board thử nối Wi-Fi (tối đa 30 s) và báo kết quả qua characteristic Status; thành công thì credentials được lưu NVS, BLE tự tắt và board chạy telemetry như board cấu hình bằng menuconfig.

### 10.1. GATT contract — 9 UUID

Service + 8 characteristics trên base UUID `e5f4a3b2-1c0d-4e2f-9a8b-7c6d5e4f3aXX` (byte cuối `XX` phân biệt từng item). App mobile là nguồn chuẩn (source of truth) của contract — mọi UUID/payload phải khớp từng byte:

| Item | UUID | Thuộc tính |
|---|---|---|
| Provisioning Service | `e5f4a3b2-1c0d-4e2f-9a8b-7c6d5e4f3a01` | — |
| WiFi SSID | `…3a02` | WRITE, **encrypted** |
| WiFi Password | `…3a03` | WRITE, **encrypted** |
| Command | `…3a04` | WRITE — ASCII `PROVISION` |
| Device Info | `…3a05` | READ — JSON |
| Status | `…3a06` | NOTIFY — ASCII |
| Broker URI | `…3a07` | WRITE |
| MQTT User | `…3a08` | WRITE |
| MQTT Pass | `…3a09` | WRITE, **encrypted** |

### 10.2. Hành vi firmware

- **Advertising** bật ngay khi board vào chế độ provisioning: advertisement chứa complete service UUID `…3a01`; local name `IoTBoard-{boardId}` nằm trong scan response (giới hạn AD 29 byte — boardId dài hơn bị cắt bớt, không lỗi).
- **Device Info** (READ) trả JSON đúng shape 3 field, ví dụ `{"schemaVersion":1,"boardId":"0","boardType":"IoT_ESP32-S2R3"}` — `boardId` = `DEVICE_ID`, `boardType` = `BOARD_TYPE` (mục 4). Ký tự `"`/`\` được escape; byte control trong hai chuỗi thành `\?`.
- **Just Works + bonding**: board không có IO nên pairing kiểu Just Works (LE Secure Connections khi peer hỗ trợ); ghi vào characteristic encrypted (SSID / WiFi pass / MQTT pass) tự kích hoạt pairing, bond lưu NVS và dùng lại các lần sau (app cài lại → pair đè bond cũ được chấp nhận).
- **Flow `PROVISION`**: app ghi đủ trường rồi viết ASCII `PROVISION` vào `…3a04` → notify `CONNECTING` → board thử Wi-Fi (tối đa 30 s, sai mật khẩu thoát sớm) → notify `CONNECTED` hoặc `FAILED:BAD_AUTH` / `FAILED:NO_SSID` (PROVISION khi chưa ghi SSID) / `FAILED:TIMEOUT` / `FAILED:ERROR`. Thất bại → board quay lại nhận write + `PROVISION` mới, không giới hạn số lần; `PROVISION` gửi giữa chừng bị bỏ qua.
- **Giới hạn độ dài** (byte; sai độ dài → lỗi ATT *Invalid Attribute Value Length* ngay, không lưu gì): SSID 1–32; WiFi pass 0–63 (rỗng = mạng mở); broker URI 4–128; MQTT user 1–64; MQTT pass 0–64. Byte NUL chèn giữa chuỗi bị từ chối.
- **NVS** namespace `bleprov` (keys `ssid`, `wifi_pass`, `broker_uri`, `mqtt_user`, `mqtt_pass`) chỉ được ghi KHI Wi-Fi connect thành công. Giá trị lưu **plain-text** vì board không bật flash encryption — hạn chế đã chấp nhận cho v1 (mục 10.6).
- **Tự tắt BLE 30 s sau `CONNECTED`** (đủ cho app đọc kết quả rồi ngắt nối) — stack NimBLE được dừng để nhả RAM cho Wi-Fi + MQTT; từ lần boot sau board dùng NVS nên BLE không bật lại.
- **Ghi dài hơn MTU**: mọi characteristic write đều dùng write-with-response (không có WRITE_NO_RSP) — giá trị vượt MTU đi qua ATT prepared-write và stack NimBLE tự gom lại; firmware chỉ thấy giá trị đầy đủ. App viết nguyên khối (kể cả SSID/broker dài) không cần tự chia gói.

### 10.3. Ba chế độ boot

Thứ tự ưu tiên nguồn credentials trong `app_main` (M16b):

1. **NVS `bleprov`** đủ 5 keys — board đã provision qua BLE trước đó: chạy như board Kconfig, BLE tắt — trừ một check có trần ngay lúc boot: Wi-Fi phải có IP trong 15 s rồi MQTT phải connected trong 10 s, fail thì quay về BLE provisioning (chi tiết + ngoại lệ mật khẩu Wi-Fi bị từ chối ở mục 10.6).
2. **Kconfig** — không có NVS nhưng `CONFIG_WIFI_SSID` khác rỗng: flow classic như trước (mục 4–5).
3. **BLE provisioning** — không có NVS và `CONFIG_WIFI_SSID` rỗng: quảng bá và chờ app; mỗi attempt thất bại được báo về rồi chờ gửi lại, lặp vô hạn lần cho tới khi connect được.

→ **Build board mới "rút hộp":** giữ mặc định `CONFIG_WIFI_SSID=""` (mục 4) khi `idf.py menuconfig` → board boot thẳng vào BLE mode, không cần gõ Wi-Fi trước.

### 10.4. Nút BOOT recovery

Giữ nút **BOOT (GPIO0) ≥ 5 s khi board đang chạy** → firmware xóa NVS `bleprov` rồi tự reboot (về BLE mode nếu `WIFI_SSID` rỗng, về Kconfig mode nếu không). Use cases:

- Đổi mật khẩu router — board vẫn giữ credentials cũ nhưng không nối được nữa.
- Provision nhầm broker/user — muốn đẩy cấu hình đúng lại từ app.

Giữ nút BẤM qua lúc reset vẫn vào ROM download mode như thường — recovery chỉ đọc mức GPIO0 khi app đang chạy (có debounce 20 ms).

### 10.5. Sinh nhãn QR cho board

Frontend quét QR chứa JSON 3 field **thuần, không bọc URL** — không dùng QR generator kiểu "wifi:" hay link (tiền lệ bug me-qr). Từ M18, `DEVICE_ID` rỗng (default) → `boardId` = hex-8 của 4 byte cuối MAC (mục 4), nên lấy MAC **trực tiếp từ board** bằng script — chạy SAU khi flash:

```bash
bash scripts/board-qr.sh                  # tự dò board qua esptool (cần ESP-IDF activated)
bash scripts/board-qr.sh -p /dev/ttyUSB0  # chỉ định cổng serial
bash scripts/board-qr.sh -m 5c:01:3b:6b:af:6c      # MAC cho sẵn, không cần board nối
bash scripts/board-qr.sh --board-id 3b6baf6c       # id cho sẵn, bỏ qua derive
bash scripts/board-qr.sh -m 5c:01:3b:6b:af:6c -t IoT_ESP32-S2R3  # boardType tùy chỉnh
```

Script in JSON + QR ra terminal:

```
{"schemaVersion":1,"boardId":"3b6baf6c","boardType":"IoT_ESP32-S2R3"}
```

- Script derive bằng đúng thuật toán firmware (`main/board_id.c`: 4 byte cuối MAC → hex-8 lowercase) nên tự khớp `boardId` mà board publish; không có board nối và không nhớ MAC thì đọc từ log boot — dòng `BLE diagnostics: Wi-Fi STA MAC … (boardId candidate)`.
- `boardId` phải ĐÚNG chuỗi firmware publish (một segment topic — chỉ chữ/số/`_`/`-`); QR lệch `boardId` làm app cấu hình nhầm board.
- `boardType` = giá trị `BOARD_TYPE` trong menuconfig (default `IoT_ESP32-S2R3`) — app hiển thị làm tên board.
- In nhãn dán lên vỏ board sau khi flash: app quét là biết boardId/boardType mà không cần nối BLE trước.

### 10.6. Hạn chế v1 (đã chốt)

- **Just Works không MITM**: bất kỳ ai trong tầm BLE đều pair được khi board ở chế độ cấu hình — nhưng chế độ này chỉ tồn tại khi board **chưa có** credentials trong NVS; board đã provision thì BLE tắt hẳn (muốn cấu hình lại → nút BOOT, mục 10.4).
- **NVS plain-text**: ai có quyền đọc flash đọc được credentials (board không bật flash encryption).
- Không provision cấu hình mạng phức tạp (static IP, 802.1X/enterprise) — v1 chỉ SSID/pass + broker + MQTT auth.
- BLE chỉ bật khi chưa có Wi-Fi đã lưu; credentials đã lưu mà sai → không sửa được qua BLE trực tiếp, phải qua nút BOOT (mục 10.4).
- Reason 210/211 (không thấy AP với security/authmode khớp) xếp vào **`FAILED:TIMEOUT`**, không phải `FAILED:BAD_AUTH` — nếu app báo TIMEOUT trong khi user chắc chắn đúng mật khẩu, khả năng cao là case này.

**Từ M22 — credentials NVS stale tự recovery (hiệu chỉnh M23)**: board đã provision (nhánh NVS, mục 10.3) được một lần check có trần lúc boot — Wi-Fi phải có IP trong **15 s**, rồi MQTT phải connected trong **10 s**. Không thấy AP (disconnect reason **200/201**) thì firmware fail ngay trong cửa sổ 15 s (không ngồi hết 15 s trong khi backoff retry đang leo thang) — case này vẫn tự xóa + reboot như cũ. **Ngoại lệ — mật khẩu Wi-Fi bị từ chối (reason 202 / BAD_AUTH là fail cuối)**: firmware **không xóa NVS, không reboot** — nó chỉ dừng vòng retry Wi-Fi (radio ngừng churn credential bị từ chối), giữ nguyên credentials trong NVS rồi đứng yên (relay OFF, không sensor task, không MQTT), đồng thời log hướng dẫn giữ nút **BOOT (GPIO0) ≥ 5 s** (mục 10.4) để xóa credentials và vào lại BLE provisioning. Các case fail còn lại (hết 15 s không có IP với reason khác BAD_AUTH, broker không tới được trong 10 s, hoặc MQTT bring-up lỗi) → firmware **dừng client MQTT (stop + destroy) trước**, rồi xóa NVS `bleprov` (log `stale NVS — erased, BLE provisioning`) và tự reboot — boot sau (NVS trống, `WIFI_SSID` rỗng) vào thẳng BLE provisioning; không có vòng xóa-reboot lặp vì erase chỉ chạy khi NVS thực sự còn credentials. Nghĩa là câu "board đã provision thì BLE tắt hẳn" ở bullet đầu mục này chỉ đúng khi credentials **còn dùng được**; credentials bị AP từ chối thì nút **BOOT ≥ 5 s** (mục 10.4) là đường recovery duy nhất. Nhánh **Kconfig** vẫn **không tự xóa** NVS và vẫn retry nền với **mọi** disconnect reason — kể cả **200/201** (AP mất) vẫn backoff 1→30 s như cũ: latch "AP mất thì ngừng retry" là opt-in (`wifi_conn_stop_retries_when_ap_gone`) và chỉ nhánh NVS bật lúc boot.

### 10.7. Luồng end-to-end với app

1. Server quảng bá mDNS `_smarthome._tcp` (mục 6) → app dò được địa chỉ server + port WS.
2. App quét QR board (mục 10.5) → mở modal BLE, nối `IoTBoard-{boardId}`.
3. App đẩy SSID/pass + broker URI + MQTT user/pass → `PROVISION` → đọc Status (mục 10.2).
4. `CONNECTED` → 30 s sau BLE tắt, board lên MQTT với contract boards (mục 14.2) → card tự xuất hiện trong app.

### 10.8. Sự cố BLE — board KHÔNG boot loop nữa (M16d)

Trước M16d, `ble_prov_start` fail lúc boot → `abort()` → reboot → lặp vô hạn (boot loop). Từ M16d:

- **Không abort, không reboot**: `ble_prov_start` được thử tối đa **3 lần, cách nhau 5 s** (mỗi lần log rõ `ble_prov_start attempt n/3 failed: <tên lỗi> (0x…)`). Vẫn fail → board vào **chế độ an toàn (safe mode)**: đứng yên không provisioning/không telemetry, relay giữ nguyên trạng thái, **nút BOOT vẫn hoạt động** (giữ ≥ 5 s → xóa credentials + reboot thử lại).
- Log tiến trình init chia bước: `step 1/5 ok: auto-stop timer ready` → `step 2/5 ok: nimble_port_init …` → `step 3/5 ok: host callbacks + SMP …` → `step 4/5 ok: provisioning GATT services registered` → `step 5/5 ok: NimBLE host task started`. Crash/fail xảy ra ở đâu nhìn log là biết đúng bước.
- Trước lần gọi BLE đầu tiên, path BLE log diagnostics: free heap / min-ever-free heap, MAC eFuse + MAC STA (nguồn cho boardId tự sinh — M18, mục 4: boardId = hex-8 của 4 byte cuối STA MAC), và trạng thái compile-time `BT_ENABLED`/`BT_NIMBLE_ENABLED`.

Mã lỗi hay gặp trong log:

| Log | Ý nghĩa | Xử lý |
|---|---|---|
| `step 2/5 … + BLE_INIT: controller init failed` | BT controller không khởi tạo được (thường kèm mã `0x101 NO_MEM`, `0x103 INVALID_STATE`) | Chụp log serial gửi lên; kiểm tra image có build đúng target `esp32` |
| `step 2/5 … + BLE_INIT: controller enable failed` | Controller init OK nhưng bật radio thất bại | Hiếm; thường là vấn đề nguồn/nhiễu — thử nguồn USB khác |
| `step 2/5 … + BLE_INIT: nimble host init failed` | Host NimBLE hết bộ nhớ khi cấp buffer HCI/mbuf | Kiểm tra `free heap` dòng diagnostics phía trên |
| `step 4/5 (service registration) failed; rc=0x…` | Lỗi NimBLE host khi đăng ký service/đặt tên GAP | Gửi lại `rc` kèm log |
| `BT_ENABLED is NOT set` / `BT_NIMBLE_ENABLED is NOT set` (diagnostics) | Image build thiếu BLE | `idf.py menuconfig` bật lại BLE (mục 4) rồi build/flash |
| `wifi_conn_apply_credentials failed: ESP_ERR_WIFI_STATE` | Lần PROVISION thứ 2 bị từ chối vì driver Wi-Fi đang giữa attempt retry cũ (bug M20 — bản trước fix; apply cũ `esp_wifi_set_config` trước khi hủy attempt) | Cập nhật firmware bản có fix M20: apply mới hủy attempt trước, đợi driver idle rồi mới nạp credentials |

Bắt serial khi board lỗi:

```bash
. ~/.espressif/tools/activate_idf_v6.0.1.sh
cd firmware/esp32-telemetry
python "$IDF_PATH/tools/idf.py" -p /dev/ttyUSB0 monitor
```

Copy gửi orchestrator: dòng `BLE diagnostics: …`, toàn bộ các dòng `ble_prov: step …` / `BLE_INIT: …`, và dòng `ble_prov_start attempt n/3 failed: …` — bộ này đủ để khoanh vùng bước chết mà không cần đoán.

Thoát safe mode: sửa cấu hình (reflash hoặc menuconfig) → **giữ nút BOOT ≥ 5 s** → board xóa NVS `bleprov` rồi reboot vào BLE/Kconfig mode tương ứng (mục 10.4).

**M20 — re-provision lần 2 fail với `ESP_ERR_WIFI_STATE` (đã fix).** Hiện tượng: lần PROVISION đầu sai mật khẩu → vòng retry nền của wifi_conn tiếp tục `esp_wifi_connect` bằng credentials cũ; app gửi PROVISION lần 2 với giá trị đúng → board vẫn báo `FAILED:ERROR`, log có `wifi:sta is connecting, cannot set config` + `wifi_conn_apply_credentials failed: ESP_ERR_WIFI_STATE (0x3006)`. Nguyên nhân: apply cũ nạp `esp_wifi_set_config` TRƯỚC khi hủy attempt đang treo, trong khi driver từ chối set_config khi station đang connecting. Bản mới đảo thứ tự: `esp_wifi_disconnect` trước → đợi driver idle (poll có trần 2 s) → `esp_wifi_set_config` (lỗi `ESP_ERR_WIFI_STATE` tồn dư được retry tối đa 5 lần, cách nhau 100 ms) → `esp_wifi_connect` lại ngay bằng credentials mới. Sự kiện disconnect do chính apply gây ra bị suppress — không bị ghi là lỗi, không làm tăng backoff (log kèm `suppressed (apply in progress)`), nên classification/backoff của attempt MỚI vẫn hoạt động nguyên vẹn và `FAILED:BAD_AUTH` vẫn báo sớm cho app. Ngoài ra sau khi board báo `FAILED:*` hết 30 s chờ IP, firmware gọi `wifi_conn_suspend_retries()` ngừng vòng retry (bớt spam log, bớt nhiễu coex Wi-Fi/BLE) cho tới khi nhận PROVISION mới — apply là đường resume duy nhất, không cần API resume riêng.

## 11. Truy vấn dữ liệu

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

## 12. Phần cần ESP32 thật

Các kiểm thử tự động (mục 8) và unit test không mô phỏng được phần cứng — những việc dưới đây **bắt buộc có ESP32 thật** để xác minh:

1. **Đọc SHT30/SHT31 thật qua I2C** — CRC-8 (poly 0x31, init 0xFF) và timing thực tế (độ trễ single-shot, clock stretching) chỉ đúng khi gặp cảm biến thật; cần external pull-up 4.7 kΩ nếu module không có sẵn.
2. **Wi-Fi LAN thật** — kết nối router thật, retry/backoff khi AP rớt, xử lý đổi IP/DHCP.
3. **LWT offline** — rút nguồn ESP32 → sau keep-alive, broker publish retained `offline` lên `{prefix}/boards/{boardId}/status`; khi ESP32 kết nối lại sẽ publish retained `online` (kèm descriptor + state relay).
4. **Chuỗi QoS 1 end-to-end** — mất mạng rồi có lại: message được retransmit đến khi broker nhận (at-least-once), outbox không tăng vô hạn (`MQTT_OUTBOX_LIMIT` — đầy thì drop + log).
5. **Chọn đúng chân SDA/SCL** cho loại module cụ thể — một số breakout expose chân khác; đổi qua `SHT3X_I2C_SDA_GPIO`/`SHT3X_I2C_SCL_GPIO` trong menuconfig nếu cần.
6. **BLE provisioning trên board thật** (mục 10) — host harness chỉ test được logic thuần (classify disconnect reason, validate độ dài, JSON Device Info, chuỗi Status); phần RF/BLE stack bắt buộc test trên target: build với `CONFIG_WIFI_SSID=""`, quét `IoTBoard-*` bằng app, pair Just Works khi ghi encrypted, nhận `CONNECTED`/`FAILED:*`, BLE tự tắt 30 s sau connect, lần boot sau dùng NVS, nút BOOT giữ 5 s quay lại provisioning (mục 10.4).

## 13. Cấu trúc thư mục

```
firmware/esp32-telemetry/          Firmware ESP-IDF v6.0.1 (C, CMake, FreeRTOS)
  main/                            app_main (composition root), board_id.c/h (M18: boardId từ MAC), Kconfig.projbuild, idf_component.yml
  components/sht3x/                Driver I2C master (driver/i2c_master.h) + CRC-8 + convert giá trị
  components/wifi_conn/            Wi-Fi station event-driven, retry backoff, classify disconnect reason (M16b)
  components/ble_prov/             BLE provisioning NimBLE: GATT 9 UUID, Just Works + bond, NVS "bleprov" (M16a)
  components/mqtt_app/             esp-mqtt (managed component espressif/mqtt), contract boards v2 (descriptor/telemetry/sensor+relay state/LWT) + QoS 1 + outbox limit
  sdkconfig.defaults               Cấu hình mặc định (không chứa secret)

src/                               Backend TypeScript (ESM strict)
  env.ts                           Đọc + validate biến môi trường bắt buộc (gồm TOPIC_PREFIX)
  telemetry/                       Schema Zod + validate (telemetry v2: {prefix}/boards/+/telemetry)
  boards/                          M14a: descriptor registry (cache boardId→descriptor) + ingest telemetry v2 (map theo descriptor)
  mqtt/                            mqtt-service: một MQTT connection — subscribe + route extraRoutes (descriptor registry + boards ingest)
  influx/                          influx-writer: retry backoff, queue 1000 điểm, drop oldest khi đầy (fields tổng quát Record từ M14a)
  query/latest.ts                  CLI: bản ghi telemetry mới nhất theo roomId (Flux)
  query/history.ts                 CLI: lịch sử 1 giờ theo roomId (Flux)
  main.ts                          Composition root + graceful shutdown (SIGINT/SIGTERM)

docker-compose.yml                 3 service: amqtt, influxdb (2.7.10 local), backend
Dockerfile                         Multi-stage node:24-alpine: build tsc → runtime chỉ dist + deps production
amqtt/broker.yaml                  Cấu hình broker amqtt (cấm anonymous, listener 1883 TCP + 9001 WebSocket)
amqtt/Dockerfile                   amqtt/amqtt:0.12.1 + deps persistence sqlite (SessionDBPlugin)
scripts/amqtt-setup.sh             Sinh password file argon2 lúc container start (user MQTT_USER + user app)
scripts/pairing-code.sh            In khối mã ghép nối cho app mobile (LAN IP, WS, token — chỉ ra stdout)
.env.example                       Mẫu biến môi trường — copy thành .env và điền giá trị thật (InfluxDB local)
```

## 14. App mobile kết nối

App mobile nói **thẳng** contract board-centric `{prefix}/boards/{boardId}/...` — cùng contract firmware v2 publish — qua MQTT over WebSocket 9001 trên broker amqtt. Backend không đứng giữa app và firmware: nó chỉ ingest telemetry v2 vào InfluxDB (mục 8.1); lệnh relay đi thẳng app → broker → firmware.

### 14.1. Điểm kết nối

| Thứ | Giá trị | Ghi chú |
|---|---|---|
| MQTT over WebSocket | `ws://<LAN_IP>:9001` | Listener WebSocket 9001 trong `amqtt/broker.yaml` (`type: ws`) |
| MQTT user | `app` (`MQTT_APP_USER`) | Mật khẩu `MQTT_APP_PASSWORD` trong `.env`; để trống thì `docker compose logs amqtt` in ra bản random |
| MQTT topic prefix | `<TOPIC_PREFIX>`, default `smarthome` | Đặt trong `.env` |
| History (Flux) | `POST http://<LAN_IP>:8086/api/v2/query` | Token **read-only**, không qua backend (mục 11) |
| Influx org / bucket | `INFLUX_ORG` / `INFLUX_BUCKET` | Mặc định `smarthome` / `telemetry` |

### 14.2. Contract topic (firmware ↔ frontend)

`{prefix}` = `TOPIC_PREFIX` (default `smarthome`); `{boardId}` do firmware đặt (`DEVICE_ID`, hoặc tự sinh từ MAC — mục 4).

| Hướng | Topic | Payload | QoS / retained |
|---|---|---|---|
| Descriptor → app | `{prefix}/boards/{boardId}/descriptor` | JSON descriptor (ví dụ dưới) | 1 / **retained** |
| Sensor → app | `{prefix}/boards/{boardId}/sensors/{S}/state` | số thuần (vd `25.74`) | 1 / **retained** |
| Trạng thái → app | `{prefix}/boards/{boardId}/status` | `online` / `offline` (thuần) | 1 / **retained** |
| Relay state → app | `{prefix}/boards/{boardId}/relays/{K}/state` | `ON` / `OFF` (thuần) | 1 / **retained** |
| App → relay | `{prefix}/boards/{boardId}/relays/{K}/set` | `ON` / `OFF` (thuần) | 1 / không retained |

Descriptor firmware v2 publish cho board `"0"` — retained, publish lại **mỗi lần board connect** (kể cả reconnect):

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

Ghi chú:

- Firmware v2 tự publish mọi topic trên: descriptor (kèm `displayName` optional theo Kconfig `BOARD_DISPLAY_NAME` — registry backend nhận optional) + telemetry v2 + `sensors/{S}/state` + `status` (LWT) + `relays/{K}/{set,state}` — xem mục 8.1 cho payload telemetry v2.
- `boardType` là mã loại board cấu hình trong firmware (`BOARD_TYPE`, mục 4) — thêm board mới chỉ cần flash firmware, không cần cấu hình backend.
- `status` giữ payload **plain** `online`/`offline` (KHÔNG bọc JSON) — quyết định M13.
- InfluxDB ghi tags `{boardId, roomId: boardId}` (quy ước 1:1) cho mọi điểm telemetry — app query Lịch sử theo board (mục 11); dữ liệu cũ không backfill.
- Backend KHÔNG tham gia đường relay: lệnh `set` đi thẳng từ app qua broker tới firmware; state retained do firmware publish để app vẽ UI.

### 14.3. In mã ghép nối

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

### 14.4. Token Influx read-only cho app

App query Flux trực tiếp InfluxDB local bằng token **read-only** (không dùng token admin). Trên instance đang chạy đã có user `app-mobile`; tạo token và gán vào `.env` biến `INFLUX_APP_TOKEN` để `pairing-code.sh` in sẵn:

```bash
docker exec -it "$(docker compose ps -q influxdb)" influx auth create \
  --user app-mobile --read-bucket "$INFLUX_BUCKET" \
  --description "mobile app read-only"
```

Copy token in ra vào `.env` (`INFLUX_APP_TOKEN=...`) rồi chạy lại `bash scripts/pairing-code.sh`.

