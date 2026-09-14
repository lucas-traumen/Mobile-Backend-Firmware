# PLAN.md — kế hoạch hiện tại

> Orchestrator cập nhật file này mỗi workflow. Đây là current plan; quyết định bền vững nằm trong `PROJECT_MEMORY.md`.

## Run hiện tại

- `run_id`: chưa có — plugin cấp ở lần dispatch Task đầu tiên (marker `[run_id/task_key]` trong description).

## Trạng thái

- M1–M4:coder **DONE**.
- M5:review **FAIL** — 2 BLOCKER cần sửa trước khi test tích hợp:
  1. Env var mismatch: `.env.example` dùng `INFLUXDB_INIT_TOKEN/ORG/BUCKET` nhưng `src/env.ts` đọc `INFLUX_TOKEN/ORG/BUCKET` → backend container crash khi start. Fix: thêm 3 biến `INFLUX_TOKEN/ORG/BUCKET` vào `.env.example` + `docker-compose.yml` (map từ `INFLUXDB_INIT_*`), giữ cả hai bộ vì image influxdb bắt buộc prefix `INFLUXDB_INIT_`.
  2. Flux injection: `src/query/latest.ts` và `src/query/history.ts` ghép `${deviceId}` trực tiếp vào query string. Fix: validate `deviceId` bằng regex `^[a-zA-Z0-9_-]+$` trước khi ghép (parameterized query InfluxDB client không hỗ trợ tag value binding trong Flux template đơn giản).
- M5:test **PARTIAL** — unit test + firmware build PASS; integration test SKIP do thiếu `.env` và tester bị deny quyền `docker compose up`. Sau khi coder fix blocker, orchestrator sẽ tạo `.env` từ `.env.example` và tự chạy integration test (hoặc mở permission cho tester).
- Đang dispatch **M6:coder** để fix 2 blocker review.
- M6:coder **DONE** — orchestrator đã verify 2026-09-07: `npm run typecheck` exit 0; fix env + regex xác nhận trong code. `npm test`: 41/43 pass; **2 test fail nằm ở `src/orchestrator-state.test.ts` (plugin orchestration — ngoài scope M6)**, file plugin `.opencode/plugins/orchestrator-state.ts` có mtime sau M6 (chưa rõ ai sửa) → tách theo dõi riêng, không chặn telemetry.
- **Integration test 2026-09-07 — PASS toàn bộ kịch bản M5 còn SKIP** (orchestrator tự chạy): tạo `.env` từ `.env.example` (secrets random, git check-ignore OK) → `docker compose up -d --build`: 3 container healthy, backend connect + subscribe. Kịch bản: (B) compose healthy PASS; (C) payload hợp lệ → InfluxDB có point đúng giá trị (query CLI thấy 25.38/60.05) PASS; (D) payload lỗi (T=999, JSON hỏng) → log validate + bỏ qua, backend không crash PASS; (E) stop mosquitto → backend sống, start lại → publish mới ghi tiếp PASS; (F) stop influxdb 12s → writer retry backoff, start lại → queue xả hết 10/10 point PASS. Lưu ý: query CLI trên host cần `INFLUX_URL=http://localhost:8086` + source `.env` (service name `influxdb` chỉ resolve trong mạng compose).
- Việc phần cứng còn lại (user): flash lại firmware với `CONFIG_MQTT_BROKER_URI="mqtt://192.168.2.34:1883"` (IP LAN máy Docker, cùng subnet ESP 192.168.2.x) + `CONFIG_MQTT_USER`/`CONFIG_MQTT_PASSWORD` khớp `.env` (giá trị trong `.env`, không commit) qua `idf.py menuconfig` rồi `idf.py flash monitor`. Hiện firmware đang trỏ 192.168.1.100 sai subnet + user/pass rỗng → MQTT connect timeout (log 2026-09-07).
- **M8:coder DONE — orchestrator verified 2026-09-07**: `npm run typecheck` exit 0; `npm test` 41/43 pass (2 fail còn lại là `src/orchestrator-state.test.ts` — plugin orchestration ngoài scope, đúng như dự kiến); `docker compose config` OK, compose còn 2 service (mosquitto + backend); query CLI đã dùng SQL với **bind parameter** (`$deviceId` + `params`) — không ghép string, giữ regex validation như defense-in-depth; dep mới `@influxdata/influxdb3-client@^2.4.0`; `.env.example` bỏ `INFLUXDB_INIT_*`, `INFLUX_URL` trỏ cloud. Còn lại: user điền token/org/bucket cloud vào `.env` → orchestrator chạy `docker compose up -d --build` + test end-to-end. Lưu ý: `.env` cũ (local) sẽ khiến backend fail start với compose mới — phải cập nhật trước.
- **M7 đã duyệt (bản đơn giản: random mỗi 5 s, default ON) — M7:coder DONE, orchestrator đã verify 2026-09-07**: `idf.py build` exit 0 (build lại độc lập, `--flash-size 4MB` trong lệnh flash, hết warning 2MB); `CONFIG_SENSOR_FAKE_MODE=y` + `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y` trong `sdkconfig`; `main.c` bọc `#if CONFIG_SENSOR_FAKE_MODE` giữ nguyên nhánh I2C thật, log `FAKE mode: T=... RH=...` mỗi chu kỳ, publish MQTT chung cả hai nhánh. Lưu ý: coder đã backup/merge `sdkconfig` (gitignored) giữ Wi-Fi credentials khỏi repo.

## Mục tiêu

Luồng Smart Home hoàn chỉnh phục vụ **app mobile (Mobile_Frontend)**:

- Thu thập: **ESP32 + SHT30/SHT31 (I2C) → MQTT (Mosquitto) → backend Node.js/TypeScript (container) → InfluxDB 2 local** (revert M8 cloud — user chốt local-only 2026-09-12 cho dễ bảo hành; backup = backup volume).
- Điều khiển relay: ESP32 subscribe lệnh MQTT (M9), app điều khiển qua bridge (M10).
- App realtime: kết nối mosquitto qua **WebSocket 9001**, contract `<prefix>/room/{roomId}/...` — backend làm **bridge** dịch từ contract firmware `smarthome/...` (M10). Firmware KHÔNG đổi contract.
- History: app query **trực tiếp InfluxDB local** bằng Flux API v2 (phương án A — frontend thiết kế sẵn theo hướng này; bỏ HTTP API backend). Token read-only Influx nằm trong app, chỉ hiệu lực trong LAN.
- Truy cập từ xa (ngoài nhà): **Tailscale** — ĐÃ HOÃN, làm sau khi app chạy được trong LAN (mốc tham chiếu M12 tương lai, chưa lên kế hoạch).

Quy ước identity (chốt 2026-09-12): `deviceId` (0–9) ≡ `roomId` — mỗi phòng một ESP, quan hệ 1:1. Relay: `K1↔1`, `K2↔2`, `K3↔3`; slot 4–10 bridge bỏ qua cho đến khi có ESP nhiều relay hơn.

## Quyết định kỹ thuật (từ khảo sát 2026-09-05)

### Chung

- Ghim phiên bản: `eclipse-mosquitto:2.0.20`, `influxdb:2.7.10`, node image `node:24-alpine`, npm `mqtt@^5`, `@influxdata/influxdb-client@^1.35`, **`zod@^3`** (validation schema), dev `tsx` (chạy TS trên host) + `@types/mqtt@^2`.
- Secret: chỉ qua `.env` (đã gitignore) và `menuconfig`/`sdkconfig` firmware (không commit `sdkconfig` đầy đủ secret); log không in mật khẩu/token.
- Kiến trúc Modular Monolith: module theo trách nhiệm, giữ style ESM strict hiện có.

### Firmware `firmware/esp32-telemetry/` — ESP-IDF v6.0.1 native (C, CMake, FreeRTOS)

- Cảm biến **SHT30/SHT31 qua I2C** — dùng driver I2C master mới `driver/i2c_master.h` (legacy `driver/i2c.h` đã EOL ở v6, sẽ bị remove ở v7).
- esp-mqtt từ IDF v6 không còn trong core → managed component `espressif/mqtt ^1.0.0` qua `main/idf_component.yml` (đã có sẵn trong cache EIM local, network cũng truy cập được component registry).
- Components tách theo trách nhiệm:
  - `components/sht3x` — I2C master bus + device (địa chỉ 0x44/0x45 từ Kconfig); đọc single-shot high-repeatability (cmd 0x2400, no clock stretching), đọc 6 byte, kiểm tra **CRC-8 (poly 0x31, init 0xFF)** cho temperature và humidity; convert `T = -45 + 175·raw/65535`, `RH = 100·raw/65535`; validate khoảng vật lý (−40…125 °C, 0…100 %RH) và finite; lỗi CRC/timeout/nack → trả error code, **không bao giờ trả NaN**.
  - `components/wifi_conn` — Station mode, event-driven (`esp_event`), retry với backoff (giới hạn số lần rồi chờ sự kiện hệ thống), không chờ vô hạn.
  - `components/mqtt_app` — esp-mqtt client: LWT `smarthome/{deviceId}/status` = `offline` (retain); khi `MQTT_EVENT_CONNECTED` publish `online` retained; telemetry `smarthome/{deviceId}/telemetry` QoS 1 retain=false, chỉ publish khi đã connected; cấu hình `outbox.limit` để chặn RAM tăng vô hạn khi mất kết nối; publish fail/outbox đầy → drop + log.
  - `main/` — `Kconfig.projbuild` (I2C SDA/SCL GPIO, địa chỉ SHT3x, Wi-Fi SSID/pass ẩn, broker URI, MQTT user/pass ẩn, deviceId, roomId, chu kỳ đo mặc định 5000 ms), `main.c` composition + task đọc sensor bằng `vTaskDelayUntil()`.
- Payload JSON đúng schema `schemaVersion/deviceId/roomId/temperature/humidity`; timestamp lấy thời điểm backend nhận bản tin.

### Backend `src/` — tách module MQTT / validation / InfluxDB, chạy được cả trên host (tsx) và trong container

- `src/env.ts` — đọc và validate biến môi trường bắt buộc.
- `src/telemetry/schema.ts` — schema Zod (`zod@^3`) cho payload telemetry: `schemaVersion` literal 1, `deviceId` string, `roomId` string min 1, `temperature` number finite −40…125, `humidity` number finite 0…100. Export `telemetrySchema` và inferred type `TelemetryPayload`.
- `src/telemetry/validate.ts` — parse topic `smarthome/+/telemetry` + JSON, gọi `telemetrySchema.safeParse()`, đối chiếu `deviceId` với segment topic; sai → log chi tiết lỗi Zod + bỏ qua, không crash.
- `src/mqtt/mqtt-service.ts` — mqtt.js v5 (auto-reconnect sẵn), subscribe `smarthome/+/telemetry`, route message hợp lệ vào InfluxDB writer.
- `src/influx/influx-writer.ts` — writeApi InfluxDB 2; khi Influx lỗi → retry với backoff; hàng đợi giới hạn (1000 điểm; đầy → drop oldest + log WARN — chính sách xử lý đầy được ghi rõ trong code/README); measurement `environment`, tags `device_id`/`room_id`, fields `temperature`/`humidity` (số), timestamp = `Date.now()` UTC lúc nhận.
- `src/main.ts` — composition root + graceful shutdown (SIGINT: đóng writeApi); host: `npm run start` qua tsx; container: node dist.
- `src/query/` — script CLI: đọc bản ghi mới nhất và lịch sử 1 giờ theo deviceId (chạy trên host, nối vào influxdb của compose).
- `Dockerfile` multi-stage: stage build (`npm ci` + `tsc` emit `dist/` qua tsconfig build riêng) → stage runtime `node:24-alpine` chỉ chứa `dist` + dependencies production; chạy `node dist/src/main.js`; không COPY `.env` (env inject lúc runtime).

### Infra — tất cả server-side trong Docker Compose

- `docker-compose.yml`, 3 service:
  - `mosquitto` — image `eclipse-mosquitto:2.0.20`, auth bằng password file sinh từ `.env` qua script setup (cấm anonymous); volume `mosquitto-data` (config + passwd) — passwd sinh ở entrypoint/init script để secret không nằm sẵn trong repo.
  - `influxdb` — `influxdb:2.7.10`, init `DOCKER_INFLUXDB_INIT_MODE=setup` (org/bucket/admin token từ `.env`), volume `influxdb-data`; healthcheck `influx ping`.
  - `backend` — build từ Dockerfile, `env_file: .env`, `restart: unless-stopped`, `depends_on` (mosquitto + influxdb có healthcheck).
- Mạng nội compose: backend nối broker/influx bằng service name (`mqtt://mosquitto:1883`, `http://influxdb:8086`) — `.env.example` ghi giá trị cho compose; README ghi giá trị `localhost` tương ứng khi chạy backend trực tiếp trên host.
- ESP32 vẫn nối broker bằng **IP LAN của máy chạy Docker** (map port 1883 ra host).
- `mosquitto/config/mosquitto.conf` + `scripts/mosquitto-setup.sh`.
- `.env.example` đầy đủ biến (broker URL, MQTT user/pass, Influx URL/token/org/bucket, deviceId/roomId mẫu).

## Việc

| task_key | Agent | Việc | File chính | Tiêu chí đạt | Lệnh kiểm tra |
|---|---|---|---|---|---|
| M1:coder | coder | Infra: compose (mosquitto + influx + backend service) + mosquitto auth + Dockerfile backend + `.env.example` | `docker-compose.yml`, `Dockerfile`, `mosquitto/**`, `scripts/**`, `.env.example` | `docker compose config` hợp lệ; Dockerfile build được (stage build tsc xanh); mosquitto chặn anonymous; influx setup org/bucket/token | `docker compose config` + `docker compose build backend` |
| M2:coder | coder | Backend: env + validation + mqtt-service + influx-writer + main + unit test + query CLI | `src/**`, `package.json`, `tsconfig.build.json` | typecheck + unit test xanh | `npm run typecheck && npm test` |
| M3:coder | coder | Firmware ESP-IDF hoàn chỉnh (sht3x I2C + wifi_conn + mqtt_app + Kconfig + sdkconfig.defaults) | `firmware/esp32-telemetry/**` | build xanh cho target esp32 | `. ~/.espressif/tools/activate_idf_v6.0.1.sh && cd firmware/esp32-telemetry && idf.py build` |
| M4:coder | coder | README (đấu dây SHT30/SHT31, menuconfig, build/flash/monitor, chạy compose, simulate, truy vấn) + đồng bộ script | `README.md`, `scripts/**` | README đủ mục bàn giao yêu cầu; lệnh mô phỏng chạy được | đối chiếu checklist yêu cầu |
| M5:review | reviewer | Review toàn bộ M1–M4 (standards + spec) | — | Không finding blocker | — |
| M5:test | tester | Verification tích hợp + firmware build | — | Xem mục Kiểm thử | xem dưới |

Thứ tự: M1→M2→M3→M4 tuần tự (một coder ghi worktree tại một thời điểm); sau M4 dispatch M5:review và M5:test song song.

### M7 (ĐÃ DUYỆT, phiên bản đơn giản hóa theo yêu cầu) — demo mode firmware: dữ liệu giả khi chưa có SHT3x

Mục đích: chạy end-to-end **không cần cảm biến** — ESP32 tự sinh nhiệt độ/độ ẩm giả mỗi chu kỳ `SENSOR_PERIOD_MS` (5 s), publish MQTT đúng schema. Backend/InfluxDB không đổi. Khi sensor thật hoạt động sẽ tắt cờ (thay đổi task sau).

| task_key | Agent | Việc | File chính | Tiêu chí đạt | Lệnh kiểm tra |
|---|---|---|---|---|---|
| M7:coder | coder | Thêm Kconfig `SENSOR_FAKE_MODE` (bool, **default y** — đang giai đoạn demo không sensor); khi bật: `main.c` bỏ qua `sht3x_read()`, sinh `T = 24.0…26.0` và `RH = 55.0…65.0` bằng `rand()` + `esp_random()` seed, luôn finite/đúng khoảng schema; log dòng `FAKE` rõ ràng mỗi chu kỳ; cờ tắt → đường đọc I2C thật nguyên vẹn | `firmware/esp32-telemetry/main/Kconfig.projbuild`, `main/main.c` | `idf.py build` xanh; khi bật: log `telemetry sent: T=... RH=...` mỗi 5 s, giá trị trong khoảng cho phép | `. ~/.espressif/tools/activate_idf_v6.0.1.sh && cd firmware/esp32-telemetry && idf.py build` |

Thiết kế chốt (đơn giản hóa):
- Chỉ sửa `main/Kconfig.projbuild` + `main/main.c` — **không đụng** `components/sht3x/**` (driver thật giữ nguyên sạch).
- Random: `float` 2 chữ số thập phân, không cần sine/clamp vì khoảng hẹp đã hợp lệ.
- Kèm sửa flash size 4MB trong `sdkconfig.defaults` (đã duyệt).

### M8 (ĐÃ DUYỆT 2026-09-07) — InfluxDB Cloud Serverless thay InfluxDB local

Mục đích: giảm tải lưu trữ trên máy — backend ghi trực tiếp InfluxDB Cloud Serverless (user đã có account, region us-east-1). Không đổi firmware, không đổi MQTT, không đổi schema.

| task_key | Agent | Việc | File chính | Tiêu chí đạt | Lệnh kiểm tra |
|---|---|---|---|---|---|
| M8:coder | coder | (1) Bỏ service `influxdb` khỏi `docker-compose.yml` + bỏ depends_on influx của backend; (2) `src/query/latest.ts` + `src/query/history.ts` viết lại từ Flux client sang SQL qua `@influxdata/influxdb3-client` (thêm dep); (3) `.env.example` cập nhật phần Influx Cloud (URL, token, org, bucket — bỏ INFLUXDB_INIT_*); (4) README cập nhật mục Docker Compose + Truy vấn | `docker-compose.yml`, `src/query/latest.ts`, `src/query/history.ts`, `.env.example`, `README.md`, `package.json` | typecheck + unit test xanh; backend container vẫn build được; query CLI dùng SQL | `npm run typecheck && npm test` + `docker compose config --quiet` |

Thiết kế chốt M8:
- `src/influx/influx-writer.ts` + `src/mqtt/**` + schema/validation **không đổi** — chỉ cấu hình qua `.env` (`INFLUX_URL` trỏ cloud).
- Lưu ý giữ nguyên logic queue/retry/drop-oldest của writer; unit test writer phải vẫn pass (mock giữ nguyên interface).
- Query SQL Serverless: `SELECT time, device_id, room_id, temperature, humidity FROM environment WHERE device_id = '<validated>' ORDER BY time DESC LIMIT 1` (latest) / `WHERE time > now() - interval '1 hour'` (history) — vẫn validate deviceId/bucket bằng regex hiện có trước khi ghép.
- `.env.example`: giữ biến `INFLUXDB_INIT_*` chỉ dạng comment (đã bỏ chạy local influx), hoặc bỏ hẳn kèm chú thích — coder tự quyết định gọn nhất, ghi rõ trong báo cáo.
- Orchestrator chịu trách nhiệm: tạo `.env` giá trị cloud thật (user tự điền token) + chạy test end-to-end sau khi coder xong.

### M9 (ĐÃ DUYỆT 2026-09-07 — M9:coder BLOCKED sau 2 dispatch rỗng; 2026-09-12 user duyệt retry lần 3 với prompt bẻ nhỏ, vẫn rỗng thì đổi model coder) — Điều khiển 3 relay từ ESP32

Mapping phần cứng user chốt 2026-09-07 (DevKit 38 pin, ESP32 classic):
- G32 → K1, G33 → K2, G25 → K3.
- Cả 3 chân đều xuất được (output-capable), không vướng chân input-only như G35 cũ, không trùng I2C SDA/SCL (21/22). Ghi chú: G32/G33 thuộc nhóm RTC, mức boot mặc định cần kiểm chứng relay không nhảy khi reset.

Quyết định user đã chốt:
- Relay **Active High** (HIGH = ON, LOW = OFF); boot mặc định **OFF hết** (init GPIO LOW sớm, khuyến nghị pull-down ngoài nếu relay nhạy lúc reset).
- App chưa xong → điều khiển bằng **CLI `mosquitto_pub`** gửi lệnh MQTT, không đụng backend/InfluxDB.
- Mỗi phòng một ESP (deviceId rút gọn `0`–`9` do app chọn); tên relay thân thiện do **người dùng tự đặt ở tầng app** — firmware chỉ hiểu kênh vật lý K1/K2/K3 (không hardcode tên phòng trong firmware để khỏi reflash khi đổi tên).

Thiết kế đề xuất (hướng chi tiết — dễ mở rộng, chờ duyệt):
- Component `relay` riêng, dạng bảng generic: mảng `{id, gpio, active_high, state}` — hiện 3 dòng K1/K2/K3, thêm relay mới chỉ thêm dòng + Kconfig, không đổi protocol. Init tất cả OFF (LOW vì Active High) sớm khi boot; API `relay_set(id, on/off)` + `relay_get_all()`.
- Topic lệnh chuẩn duy nhất (mở rộng được): `smarthome/{deviceId}/relay/set` — payload JSON có version:
  `{"schemaVersion":1,"relay":"K1","state":"ON"}` (`relay`: `K1`/`K2`/`K3`, sau này mở rộng `K4`…`Kn` hoặc alias thân thiện; `state`: `ON`/`OFF`; các field tương lai như `durationMs`/`seq`/`source` thêm optional, firmware lạ thì bỏ qua). Sai schema/kênh → log + bỏ, không crash.
- Giữ tương thích CLI đơn giản: vẫn nhận topic từng kênh `smarthome/{deviceId}/relay/K1/set` … payload `ON`/`OFF` thuần (map nội bộ về cùng hàm xử lý JSON) để test nhanh bằng `mosquitto_pub`.
- Topic state: `smarthome/{deviceId}/relay/state` retained JSON `{"schemaVersion":1,"K1":"OFF","K2":"OFF","K3":"ON"}` publish sau mỗi lệnh + khi MQTT reconnect (app đọc một topic là đủ vẽ UI nhiều phòng).
- `mqtt_app` mở rộng subscribe cả 2 dạng topic trên (deviceId từ Kconfig, string — dùng `"3"` thay vì `esp32-01` là hợp lệ, khớp regex backend). Telemetry giữ nguyên.
- Kconfig thêm `RELAY_K1_GPIO` (default 32), `RELAY_K2_GPIO` (default 33), `RELAY_K3_GPIO` (default 25) + `RELAY_ACTIVE_HIGH` (default y); giữ nguyên telemetry.
- README thêm mục đấu dây relay Active High + ví dụ CLI cả 2 dạng + bảng mapping tên thân thiện (app) ↔ (deviceId, Kx) + ghi chú mở rộng (thêm K4, duration, alias).
- Kiểm tra: `idf.py build` xanh; test CLI từng kênh ON/OFF cả dạng JSON lẫn thuần + state retained; reboot vẫn OFF; telemetry không ảnh hưởng.
- **Bổ sung duyệt 2026-09-12 (OTA)**: kèm đổi partition table sang OTA (`ota_0`/`ota_1` + giữ NVS/data, file `partitions.csv`) — CHỈ mở đường, không implement logic OTA (tải/verify/rollback/HTTP endpoint là milestone riêng sau này, khi làm phải có xác thực/chữ ký image). Lý do: đổi partition layout sau này vẫn phải USB-flash lại toàn bộ ESP.
- **Retry 3 (duyệt 2026-09-12) — bẻ nhỏ thành 2 dispatch**: (1) component `relay` + Kconfig + partition table; (2) tích hợp `mqtt_app` subscribe + README. Phần 1 xong mới phần 2.
- **M9a:coder DONE — orchestrator verified 2026-09-12**: `idf.py build` exit 0, không warning; partition table OTA xác nhận (nvs 16K + otadata 8K + phy 4K + ota_0/ota_1 1536K mỗi ô, app dùng 44%); component relay chỉ phụ thuộc `esp_driver_gpio` (nm check: chỉ GPIO + log); semantics kiểm chứng bằng host harness (init OFF, set_level trước gpio_config chống glitch, id lạ → ESP_ERR_INVALID_ARG, nhánh active-low đúng). Lưu ý: `sdkconfig` (gitignored, chứa Wi-Fi credentials) đã sửa 3 dòng partition — backup tại `/tmp/opencode/sdkconfig.backup-m9a`, 5 dòng credential byte-identical với backup. `main.c` thêm tối thiểu `relay_init()` đầu app_main. Ghi chú cho M9b: relay_set không có lock — mọi lệnh phải cùng MQTT handler task; G32/G33 chân RTC cần pull-down ngoài nếu relay nhảy lúc reset (kiểm chứng khi flash thật).
- **Còn lại M9b (chưa dispatch)**: mqtt_app subscribe `smarthome/{deviceId}/relay/set` JSON + per-channel `.../relay/K1/set` payload ON/OFF thuần → map `relay_set`; publish retained `smarthome/{deviceId}/relay/state` sau mỗi lệnh + khi reconnect; README mục relay.
- **M9b:coder DONE — orchestrator verified 2026-09-12**: `idf.py build` exit 0 không warning (app 0xdc560, 43% free); backend không đụng (typecheck 0, test 41/43 known-issue). mqtt_app: subscribe đúng 2 topic (JSON + per-channel `+/set`, comment phân tích wildcard), xử lý trong MQTT event task (không task mới — relay_set không cần lock), state retained build từ `relay_get_all()` (generic), JSON sai/id lạ/state lạ → log + bỏ không crash. Host harness 28/28 PASS tại `/tmp/opencode/m9b-harness/` (dùng chính mqtt_app.c + relay.c, không vào repo). cJSON: IDF v6 bỏ component `json` built-in → managed component `espressif/cjson ^1.7.19` qua `mqtt_app/idf_component.yml` (namespaced `espressif__cjson` trong PRIV_REQUIRES). README có mục 9 relay đủ đấu dây + 3 ví dụ CLI. sdkconfig credentials byte-identical backup M9a.
- **M11 HOÀN TẤT (coder + integration test) 2026-09-12.** Stack local đang chạy: mosquitto + influxdb 2.7.10 + backend, measurement `sensors` tag `roomId`. Token read-only cho app đã tạo (user `app-mobile`) — giá trị trong `/tmp/opencode/readonly-token.txt`, cần user lưu lại chỗ an toàn (không commit).
- **M9 ĐÓNG 2026-09-13**: user test relay qua mosquitto_pub thành công (K1/K2/K3 ON/OFF) — state retained đúng `{"schemaVersion":1,"K1":"ON","K2":"ON","K3":"OFF"}`, status online. Chuỗi lệnh relay end-to-end hoạt động thật trên phần cứng.
- **Setup kết nối app mobile 2026-09-13**: orchestrator verify hạ tầng — 3 container up, WS 9001 handshake 101 OK, bridge còn retained `smarthome/room/0/...` (board thật) + `phong-khach` (test cũ). Token read-only `app-mobile` còn hiệu lực (query lại qua `influx auth list` trong container; file `/tmp/opencode/readonly-token.txt` đã bị dọn). Cấu hình app: host `10.0.2.2`, port 9001, prefix `smarthome`, user `app` + `MQTT_APP_PASSWORD` trong `.env`; Influx `http://10.0.2.2:8086`, org `smarthome`, bucket `telemetry`.
- **Rủi ro contract phát hiện 2026-09-13 (CHƯA xác minh)**: hướng dẫn phía Mobile_Frontend mô phỏng status bằng JSON `{"status":"online"}` trên `<prefix>/room/{id}/status`, nhưng bridge M10 (`mapStatusToFrontend`) publish plain `online`/`offline`. Nếu app chỉ parse JSON → board thật lẫn board giả đều không hiện online. Xác minh: publish trực tiếp 2 dạng lên `smarthome/room/<id>/status` (retained) rồi xem app nhận dạng nào. Nếu mismatch → đề xuất M12 sửa bridge publish JSON + unit test — chờ user duyệt.

### M11 (ĐÃ DUYỆT 2026-09-12 — coder DONE cùng ngày, xem Trạng thái) — Revert InfluxDB về local + đổi schema cho khớp frontend

Mục đích: trở lại InfluxDB 2.7.10 local trong compose (đảo ngược M8), đồng thời đổi measurement/tag để app mobile query Flux trực tiếp không cần adapter. Chạy TRƯỚC M9/M10 vì là nền cho test tích hợp.

| task_key | Agent | Việc | File chính | Tiêu chí đạt | Lệnh kiểm tra |
|---|---|---|---|---|---|
| M11:coder | coder | (1) Thêm lại service `influxdb:2.7.10` + volume + healthcheck + `depends_on` của backend; (2) `src/query/latest.ts` + `src/query/history.ts` viết lại từ SQL (influxdb3-client) về Flux (influxdb-client, pattern M1–M6); (3) `src/influx/influx-writer.ts`: measurement `environment` → `sensors`, tag `device_id` → `roomId` (tag `room_id` cũ bỏ); (4) `.env.example` khôi phục `INFLUXDB_INIT_*` + `INFLUX_URL=http://influxdb:8086`; (5) README mục compose/backup volume; (6) bỏ dep `@influxdata/influxdb3-client` | `docker-compose.yml`, `src/query/**`, `src/influx/influx-writer.ts`, `src/influx/*.test.ts` (nếu mock theo measurement), `.env.example`, `README.md`, `package.json` | typecheck + unit test xanh; `docker compose config` OK; query CLI chạy được với Flux; writer ghi measurement `sensors` tag `roomId` | `npm run typecheck && npm test` + `docker compose config --quiet` |

Thiết kế chốt M11:
- Writer: giữ nguyên logic queue/retry/drop-oldest — chỉ đổi tên measurement/tag nơi khai báo point.
- Token read-only cho app (flux query only, không write) do orchestrator tạo sau khi coder xong (qua `influx user create`/`influx auth create`), ghi README, không commit.
- Bridge M10 publish realtime không qua Influx — việc đổi measurement/tag không ảnh hưởng contract MQTT.

- **M11:coder DONE — orchestrator verified 2026-09-12**: typecheck exit 0; test 41/43 (2 fail known-issue `orchestrator-state.test.ts` ngoài scope); `docker compose config` OK (3 service: mosquitto + influxdb 2.7.10 + backend, healthcheck + depends_on); writer ghi measurement `sensors` tag `roomId`; query CLI về Flux (influxdb-client, giữ regex guard — test injection exit 1 đúng); dep influxdb3-client đã gỡ; `.env.example` khôi phục `INFLUXDB_INIT_*`; README có mục Backup volume.
- Việc còn lại M11 (orchestrator): cập nhật `.env` thật từ `.env.example` mới (biến `INFLUXDB_INIT_*` + đồng bộ `INFLUX_*` — hiện `.env` vẫn là cấu hình cloud cũ, container influxdb sẽ setup fail nếu chưa đổi) → `docker compose up -d --build` → test end-to-end (3 container healthy, publish payload giả, query Flux thấy point) → tạo token read-only cho app mobile, ghi README, không commit.
- **Integration test M11 — orchestrator PASS toàn bộ 2026-09-12**: `.env` mới (giữ MQTT credentials cũ `esp32` để không reflash ESP32; secrets Influx random; backup cloud cũ tại `/tmp/opencode/env.backup-cloud-m11`); xóa volume influxdb cũ + `docker compose up -d --build` → 3 container healthy, backend connect + subscribe. Kịch bản: (1) payload hợp lệ 2 phòng → Influx có point `sensors` tag `roomId` đúng giá trị (25.38/27.12) PASS; (2) payload lỗi T=999 → validate bắt + bỏ, không crash PASS; (3) query CLI host (Flux, `INFLUX_URL=http://localhost:8086`) latest + history 1h trả đúng PASS; (4) stop influxdb 30s + publish 6 point → writer retry backoff, start lại → queue xả hết 6/6 point PASS; (5) user `app-mobile` + token read-only (`read:orgs/.../buckets`) tạo xong — app query `POST /api/v2/query` Flux CSV qua token này PASS (26.5 phong-khach), write bằng token này bị **403 chặn đúng** PASS. Token read-only lưu `/tmp/opencode/readonly-token.txt` (tạm, ngoài repo).

### M10 (ĐÃ DUYỆT 2026-09-12, dispatch 2026-09-13) — Mosquitto WebSocket + bridge contract frontend

Mục đích: app mobile kết nối realtime qua WS 9001 với contract `<prefix>/room/{roomId}/...`; backend bridge dịch 2 chiều giữa contract firmware `smarthome/...` và contract frontend. Chạy sau M11.

| task_key | Agent | Việc | File chính | Tiêu chí đạt | Lệnh kiểm tra |
|---|---|---|---|---|---|
| M10:coder | coder | (1) `mosquitto.conf` thêm listener 9001 `protocol websockets` (cùng user/pass auth như 1883, cấm anonymous); compose expose 9001; (2) module `src/bridge/`: subscribe `smarthome/+/telemetry` → re-publish tách field `<prefix>/room/{roomId}/sensor/temperature` + `/humidity` (số, retained); subscribe `smarthome/+/status` → re-publish `<prefix>/room/{roomId}/status`; subscribe `<prefix>/room/+/cmnd/relay/+` (payload ON/OFF thuần) → map `K{n}` → publish `smarthome/{deviceId}/relay/K{n}/set`; subscribe `smarthome/+/relay/state` (retained JSON) → tách `<prefix>/room/{roomId}/stat/relay/{n}` retained; (3) env mới `TOPIC_PREFIX` (default `smarthome`); (4) unit test bridge mapping | `mosquitto/config/mosquitto.conf`, `docker-compose.yml`, `src/bridge/**`, `src/env.ts`, `.env.example`, `README.md` | typecheck + unit test xanh; integration thủ công: publish telemetry giả → `mosquitto_sub` thấy topic `<prefix>/...` đúng shape; publish cmnd relay → topic `smarthome/.../relay/K1/set` xuất hiện | `npm run typecheck && npm test` + `docker compose config --quiet` |

Thiết kế chốt M10:
- Bridge là module độc lập trong cùng process backend (Modular Monolith) — không service riêng, không thêm container.
- Bridge dùng chung MQTT connection hoặc connection riêng tùy thiết kế coder — yêu cầu: mất kết nối không crash process, reconnect tự động.
- Câu hỏi mở: frontend dùng prefix cụ thể gì cho `<prefix>`? Default `smarthome` — chờ xác nhận từ phía Mobile_Frontend.

Thứ tự thực hiện: **M11 → M9 → M10** (M11 nền cho test; M9 firmware độc lập; M10 bridge cần cả hai bên đã chốt). M9 và M10 không đụng file nhau nhưng quy ước một coder tại một thời điểm.

Lưu ý: M1 cần `docker compose build backend` → M2 phải xong source backend trước khi build image được. Giải pháp: M1 tạo Dockerfile + compose nhưng tiêu chí build image kiểm sau khi M2 xong (M4/tester verify lại). Coder M1 không được đụng `src/`.

## Kiểm thử

Tự động (không cần phần cứng):
- `npm run typecheck`, `npm test` — unit: validation (đúng/sai schema, deviceId lệch topic, NaN, JSON hỏng, ngoài khoảng SHT3x), queue/retry/drop-oldest của writer (mock writeApi).
- `docker compose up -d --build` → backend container connect mosquitto + influxdb (healthcheck xanh) → publish payload giả bằng `mosquitto_pub` (chạy trong container mosquitto, hoặc `docker exec`) → query Influx (CLI query) thấy point mới nhất; payload lỗi → log + bỏ, backend không crash.
- Phục hồi: `docker compose stop mosquitto` → backend container sống, tự reconnect khi start lại; `docker compose stop influxdb` → queue không vượt giới hạn, drop oldest đúng chính sách, ghi tiếp khi start lại.
- Firmware: `idf.py build` target esp32 trên ESP-IDF v6.0.1 local.

Cần ESP32 thật (ghi rõ trong README phần "cần phần cứng"):
- Đọc SHT30/SHT31 thật qua I2C (CRC/timing thực tế), Wi-Fi thật qua LAN, LWT `offline` thật khi rút nguồn, chuỗi QoS 1 end-to-end, chọn đúng chân SDA/SCL cho loại module.

## Câu hỏi mở (trả lời khi duyệt — mặc định sẽ theo phương án ghi)

1. Target chip: `esp32` (classic) — OK? (default: esp32; SHT3x nối GPIO21=SDA, GPIO22=SCL, cấu hình được qua Kconfig)
2. Truy vấn latest/1h làm script CLI chạy trên host (không HTTP API) — đúng ý "chưa thêm giao diện"? (default: CLI)
3. Backend container production-style (build dist, không volume-mount source) — hay bạn muốn dev-style (mount source + tsx watch)? (default: production-style)
4. ~~Validation backend viết tay~~ → **đã chốt dùng Zod** theo yêu cầu "an toàn trước, tối ưu sau".

Đã chốt theo ý bạn: Docker cho toàn bộ server-side (mosquitto + influxdb + backend); cảm biến SHT30/SHT31 thay DHT11; docker pull image ở giai đoạn build/test được phép.

## Lịch sử

- 2026-09-05: Bootstrap repo — TS stub, OpenCode v3 orchestration (agents + state plugin + docs).
- 2026-09-05: Workflow luồng telemetry — khảo sát môi trường, plan v1 (DHT11).
- 2026-09-05: Plan v2 — đổi DHT11 → SHT30/SHT31 (I2C), thêm backend Docker container + Dockerfile multi-stage, compose 3 service.
- 2026-09-07: M5 review/test + M6 fix env + Flux injection; integration test compose PASS toàn bộ.
- 2026-09-07: M7 fake-mode firmware DONE; M8 InfluxDB Cloud Serverless DONE.
- 2026-09-12: Tích hợp Mobile_Frontend — user chốt: local-only (revert M8 → M11), backend bridge realtime (M10), app query Flux trực tiếp Influx local (phương án A, không HTTP API backend), identity deviceId≡roomId, K↔1..3. Tailscale cho truy cập từ xa: HOÃN, làm sau khi app chạy được trong LAN. M9 retry lần 3 (prompt bẻ nhỏ, fallback đổi model coder).
