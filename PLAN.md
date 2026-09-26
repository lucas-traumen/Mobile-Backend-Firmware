# PLAN.md — kế hoạch hiện tại

> Orchestrator cập nhật file này mỗi workflow. Đây là current plan; quyết định bền vững nằm trong `PROJECT_MEMORY.md`.

## Run hiện tại

- `run_id`: `1677db7c-acaf-49c9-820e-fb7fc4659151` — workflow M15 (MQTT host normalization: doc + handoff frontend), tạo khi dispatch `M15b:coder` 2026-09-17. Phase: done (phần việc repo này).
- Run cũ `038c62c7-4698-4ee9-ae58-9c1296386ef1` (workflow M14) vẫn còn M14c treo chờ user flash firmware v2 — xem mục M14.
- **2026-09-22**: M19:coder DONE + verified (run `014bb9ea` — normalize broker URI scheme-less + đóng residual M18). On-target 2026-09-23 bị chặn sớm hơn bởi bug **M20** (race `wifi_conn_apply_credentials` khi driver đang connecting) — **M20:coder DONE + orchestrator verified 2026-09-23** (run `6d2b0587` — build 0 warning + harness 51/51). Còn: user flash binary cumulative M19+M20 + re-provision → sau CONNECTED orchestrator verify end-to-end + đóng M14c.
- **2026-09-19**: user muốn làm "cơ chế quét mã để cấu hình board" → plan **M16** (firmware BLE provisioning) đã soạn trong mục M16 — **CHỜ USER DUYỆT** trước khi dispatch (chưa có run_id; plugin cấp ở dispatch đầu).
- **2026-09-19 (sau bàn bạc mDNS/broker discovery)**: user chốt mô hình LAN (app + server + board cùng WiFi, demo trên laptop) và duyệt **M17** — server mDNS discovery (avahi), "vậy triển khai bên backend". M17 chạy TRƯỚC M16; QR server trong M16 B+ bỏ khỏi luồng chính (mDNS thay — app tự dò broker). Dispatch `M17:coder` cùng ngày.
- **2026-09-19 chiều**: M17 ĐÓNG (coder + integration PASS — quảng bá mDNS live, 3 container healthy, MQTT round-trip qua IP mDNS). User "ok" duyệt M16 → dispatch cả 3 phần a/b/c cùng ngày, tất cả DONE + orchestrator verified: M16a component ble_prov (build xanh 0 warning, 9 UUID, credentials byte-identical), M16b wiring 3 chế độ boot (BLE link thật 1.074 MB/28% free, fix GCC dead-strip nhánh BLE bằng noinline), M16c README mục 10 + harness 130/130. **Còn: on-target test (user, cần frontend amend broker chars) + chuyển tiếp board 0 (gộp M14c)**.
- **2026-09-21 (log on-target M16/M18 user dán)**: BLE provisioning chạy đúng tới WiFi (ssid UTF-8 "Phòng toàn trai đẹp" OK, IP 192.168.2.17, advertise `IoTBoard-3b6baf6c` = M18 boardId-from-MAC hoạt động, fail-soft M16d đúng: `FAILED:ERROR` → chờ giá trị sửa, không reboot). Lỗi duy nhất: `mqtt_app_init` fail `Error parse uri = 192.168.2.28:1883` — **broker app gửi thiếu scheme `mqtt://`** (esp-mqtt yêu cầu scheme). Máy server đang là 192.168.2.28 (wlp4s0), mosquitto healthy listen 0.0.0.0:1883 → địa chỉ ĐÚNG, chỉ thiếu scheme. Plan **M19** (firmware normalize broker URI) đã soạn — **CHỜ USER DUYỆT**.
- **2026-09-26**: M23 DONE + orchestrator verified (`run_id` `65d543ab-fd30-4f5a-a22a-86ef3f93afac`, session coder `ses_f2666d498ffe6mUpuOiihxzilK`). `idf.py build` do orchestrator chạy lại: exit 0, binary `0x1145d0`, 28% free. Chưa flash board `3b6baf6c`.
- **2026-09-26**: M24 DONE — user đã duyệt triển khai; `M24:coder` DONE theo run `4f9d6c2e-7b19-4d8a-93e1-c5a0b6f4d812`, review APPROVE, orchestrator tự xác minh syntax/harness/compose gate sau hardening.

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

- **2026-09-19 — ngữ cảnh M16**: user muốn làm cơ chế quét QR cấu hình board. Phía app Mobile_Frontend đã code xong luồng QR→BLE onboarding (task `boards-ble-wifi-provisioning`: coder DONE, tester PASS, reviewer APPROVE — đang chờ user acceptance, 13 file chưa commit ở repo đó; cần native rebuild; GATT contract ở `.ai/plans/current-plan.md` frontend). Phần thiếu thuộc repo này: firmware chưa có BLE service nào (`CONFIG_BT_ENABLED is not set`). Kèm phát hiện lệch: `BOARD_TYPE` firmware đang `#define "A"` (`mqtt_app.c:45`) trong khi frontend display convention cần `"IoT_ESP32-S2R3"` (title = boardType, ảnh map theo type slug) — fold vào M16 cùng lần flash. Plan M16 đã soạn — CHỜ DUYỆT.

## Mục tiêu

Luồng đã chốt 2026-09-25 — chi tiết bền vững ở `PROJECT_MEMORY.md`. Đoạn dưới đây thay mô tả bridge/Mosquitto cũ; các mục M1–M21 phía dưới là nhật ký, không phải kiến trúc đang chạy.

- Thu thập: ESP32 (SHT30/SHT31 hoặc fake mode) nói thẳng contract `{prefix}/boards/{boardId}/...` qua **amqtt** TCP 1883. Backend chỉ ingest telemetry v2 → InfluxDB 2.7 local.
- Realtime: app nối **cùng broker** qua WebSocket 9001, cùng contract boards. Không còn bridge, không còn topic `room/`.
- Relay: app publish `{prefix}/boards/{boardId}/relays/{K}/set`; firmware subscribe và trả `.../state`. Backend không nằm trên đường điều khiển.
- History: app query Flux trực tiếp InfluxDB local, token read-only. Tag `boardId`; tag `roomId` ghi bằng chính `boardId`.
- Identity: `boardId` hex-8 từ MAC khi `DEVICE_ID` rỗng. Phòng thuộc app.
- Provisioning: NVS `bleprov` chỉ là check nhanh lúc boot (15 s Wi-Fi + 10 s MQTT). Fail → xóa NVS, reboot, BLE. BOOT ≥ 5 s vẫn là đường xóa tay.
- Từ xa: Tailscale vẫn hoãn.

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
- **M13 ĐÃ DUYỆT 2026-09-14** (user: "ok triển khai" — tất cả theo đề xuất + bổ sung boardType theo ý "nhiều mẫu board cùng loại A"). 7 quyết định chốt xem mục M13. Dispatch `M13:coder` cùng ngày; sau coder xong orchestrator tự verify + integration test (compose, mosquitto_sub, Flux theo tag boardId, dọn retained).
- **M13:coder thất bại 2 lần 2026-09-14 vì lỗi provider** với model cũ `nexusmmo/deepseek-v4-flash` (lần 1 "unknown certificate verification error", lần 2 "Upstream service temporarily unavailable"). Worktree giữ lại code 2 phiên làm được trước khi chết.
- **Orchestrator verify checkpoint 2026-09-15**: `npm run typecheck` exit 0; `npm test` 101/103 (2 fail = known-issue `orchestrator-state.test.ts`, ngoài scope M13). Đã có trong worktree và đạt: `src/bridge/boards-mapper.ts` + test, `src/env.ts` parse `LEGACY_BOARDS` + test, `src/bridge/bridge-service.ts` dual-publish, `src/influx/influx-writer.ts` tag `boardId` + test, `src/main.ts` wiring, `.env.example` `LEGACY_BOARDS`, `scripts/clear-legacy-retained.sh`. **Còn thiếu duy nhất: README.md** (chưa có nội dung boards/LEGACY_BOARDS).
- **Retry M13:coder (attempt 3) 2026-09-15** theo chỉ thị user sau khi cập nhật model agent (coder → `xkiro/z-ai/glm-5.3-flash`, reviewer → `nexusmmo/qwen3.8-max`, tester → `xkiro/z-ai/glm-5.3`): phiên mới kèm checkpoint, việc chính = README.md; đây là lần retry provider cuối — nếu lại lỗi provider thì `blocked`. Sau coder xong: orchestrator verify lại + integration test như kế hoạch M13.
- **M13 HOÀN TẤT 2026-09-15** — coder attempt 3 (model mới glm-5.3-flash) DONE: chỉ sửa README (+52/−2 — mục 6 LEGACY_BOARDS, mục 13.3 Contract board-centric, mục 13.4/13.5 đánh lại số), không đụng file nào khác. Orchestrator verify lại: typecheck exit 0, test 101/103 (2 fail known-issue ngoài scope).
- **Integration test M13 — orchestrator PASS toàn bộ 2026-09-15**: `.env` thêm `LEGACY_BOARDS=0:A` + `docker compose up -d --build` (backend recreate, bridge subscribe thêm filter `smarthome/boards/+/relays/+/set`, không lỗi). Kịch bản: (1) subscribe `smarthome/boards/0/#` thấy `descriptor` retained đúng shape M13 (schemaVersion 1, boardType "A", S1 temperature °C/S2 humidity %, K1–K3, KHÔNG displayName) + `status` + `relays/K1..K3/state` retained tách kênh đúng PASS; (2) publish telemetry giả board "0" (25.38/60.05) → dual-publish ĐÚNG CẢ HAI contract: `smarthome/room/0/sensor/{temperature,humidity}` (cũ) + `smarthome/boards/0/sensors/{S1,S2}/state` (mới, retained) PASS; (3) lệnh `smarthome/boards/0/relays/K1/set` ON → bridge forward `smarthome/0/relay/K1/set` ON PASS (board offline lúc test nên không tác động phần cứng); (4) query Flux filter tag `boardId=="0"` trả point mới 25.38/60.05 với tag boardId+roomId, dữ liệu cũ KHÔNG backfill (bị filter loại đúng) PASS; (5) `scripts/clear-legacy-retained.sh` dọn 8 topic retained của `esp32-01` — trước còn `smarthome/{room/,}esp32-01/status offline`, sau trống PASS.
- **Lưu ý sau M13**: board thật "0" đang OFFLINE lúc test (status offline là LWT đúng) — khi cấp điện lại, telemetry/status tự chảy qua dual-publish không cần làm gì thêm. Điểm telemetry giả 25.38/60.05 đã ghi vào bucket thật (tiền lệ các integration test M5/M11) — app Lịch sử board 0 có thể thấy 1 điểm lạ, bỏ qua được. **Còn treo**: đối chiếu Mobile_Frontend shape status plain `online`/`offline` vs JSON (quyết định M13-7, rủi ro 2026-09-13) — nếu app cần JSON thì sửa một chỗ `mapStatusToBoards`. Thay đổi M13 chưa commit — chờ user quyết định.
- **M14a:coder DONE — orchestrator verified 2026-09-15** (run `038c62c7`): typecheck exit 0; test 157/159 (2 fail known-issue `orchestrator-state.test.ts` ngoài scope; toàn bộ test v1 cũ pass không sửa expectation). File mới: `src/telemetry/schema-v2.ts` + `validate-v2.ts` (+test), `src/boards/descriptor.ts` + `descriptor-registry.ts` + `boards-ingest.ts` (+test). Sửa: `mqtt-service.ts` thêm `extraRoutes` (registry + ingest v2 chạy trên CÙNG connection — subscribe trong 'connect', route trước đường v1, v1 nguyên vẹn), `influx-writer.ts` fields → `Record<string, number>` + `writePointData()`, `main.ts` wiring, README mục 8.1/12/13.3. Bridge KHÔNG đụng.
- **Integration test M14a — orchestrator PASS toàn bộ 2026-09-15** (rebuild + `docker compose up -d --build`): (1) backend subscribe thêm 2 filter `smarthome/boards/+/descriptor` + `smarthome/boards/+/telemetry`, registry tự nạp descriptor board "0" từ retained của bridge PASS; (2) telemetry v2 board "0" → WARN misconfig "bỏ khỏi LEGACY_BOARDS" đúng nội dung + điểm VẪN ingest: map data-driven S1→temperature 26.5, S2→humidity 62.25, tags boardId=0+roomId=0 PASS; (3) cold-start board "5": telemetry trước descriptor → WARN + bỏ point; descriptor (kèm `displayName` optional) nạp OK; telemetry lại với kênh lạ S9 → S9 WARN + bỏ, S1/S2 ghi đúng 21.7/55.5 PASS. Dọn sau test: xóa retained descriptor + Influx delete points board "5" và điểm test board "0" hôm nay (dữ liệu thật các ngày trước nguyên vẹn) — bucket sạch, không board ma.
- **M14b:coder DONE — orchestrator verified 2026-09-15**: `idf.py build` exit 0 (binary 0xdb950, 43% partition free, `-Werror` sạch); typecheck exit 0 + test 157/159 (backend không đụng); `mqtt_app.c` viết lại contract v2 đúng spec — LWT = `{prefix}/boards/{boardId}/status` offline retained, descriptor retained mỗi connect (cJSON, bảng sensor hằng mirror template A + `displayName` khi Kconfig khác rỗng, relays lấy từ `relay_get_all()`), telemetry v2 QoS 1 KHÔNG retained + comment đúng lý do (retained replay sẽ re-ingest điểm cũ), sensor state per kênh retained cùng chu kỳ, subscribe duy nhất `{base}/relays/+/set` parse kênh từ segment (sai → log + bỏ), relay state retained sau mỗi lệnh + khi connect, `round2()` giữ precision 2 chữ số; KHÔNG còn topic v1 nào trong mqtt_app. Kconfig mới `MQTT_TOPIC_PREFIX` (default smarthome) + `BOARD_DISPLAY_NAME` (rỗng) vào sdkconfig.defaults + sdkconfig thật (backup `/tmp/opencode/sdkconfig.backup-m14b`, diff chỉ đúng 2 dòng thêm — credentials byte-identical). README cập nhật contract v2 + thứ tự reflash. API đổi: `mqtt_app_config_t` đổi `device_id`→`board_id`, thêm `topic_prefix`/`display_name`, bỏ `room_id` (chỉ mqtt_app + main.c dùng).
- **Còn lại M14c (chờ user)**: theo thứ tự bắt buộc — (1) app Mobile_Frontend chuyển sang boards contract; (2) user `idf.py flash monitor` board "0"; (3) orchestrator: `.env` `LEGACY_BOARDS=` rỗng + `docker compose up -d`, `bash scripts/clear-legacy-retained.sh 0`, verify end-to-end (descriptor từ firmware, telemetry v2 → Influx, lệnh relay boards, LWT). LƯU Ý: flash xong board 0 mất contract room cũ ngay lập tức (bridge không còn nguồn v1).
- **Tài liệu bàn giao frontend 2026-09-15**: tạo `docs/frontend-setup.md` theo yêu cầu user — thông số kết nối 3 môi trường (web máy Docker `localhost`, Android emulator `10.0.2.2`, điện thoại thật IP LAN), secret qua `scripts/pairing-code.sh`, contract boards đầy đủ, ví dụ query Influx, 2 kịch bản mô phỏng không phần cứng (v1 qua bridge hiện tại + v2 trực tiếp dùng boardId giả `sim1`), bảng xử lý sự cố. Token read-only Influx đã lưu biến `INFLUX_APP_TOKEN` trong `.env` (orchestrator lấy lại từ server 2026-09-15 sau khi file `/tmp/opencode/readonly-token.txt` bị dọn; đã verify query OK). File chưa commit.
- **2026-09-17 — user test web app, app không connect MQTT** (log lặp `Mqtt: connecting to ws://ws://localhost:9001:9001`). Chẩn đoán orchestrator: bug ở **Mobile_Frontend** — user dán nguyên URL `ws://localhost:9001` (theo bảng "MQTT URL" trong `docs/frontend-setup.md`) vào trường "Địa chỉ máy chủ" trong khi app có trường host + port riêng; `settingsSchema.ts` chỉ `trim().min(1)` không normalize → `mqttJsClient.ts:154` ghép `ws://${host}:${port}` ra URL hỏng. 3 dòng log lặp = retry backoff của adapter (tối đa 10 lần rồi `failed`), không phải 3 lỗi riêng. Đề xuất fix = M15 (chờ duyệt).

### M15 (ĐÃ DUYỆT 2026-09-17 — user "ok") — MQTT host normalization (app) + doc bàn giao

Chẩn đoán gốc (2026-09-17): user dán nguyên URL `ws://localhost:9001` (theo bảng trong `docs/frontend-setup.md`) vào trường "Địa chỉ máy chủ" của app trong khi app có trường host + port riêng; `settingsSchema.ts` chỉ `trim().min(1)` không normalize → `mqttJsClient.ts:154` ghép `ws://${host}:${port}` ra URL hỏng. Log lặp = retry backoff của adapter (tối đa 10 lần rồi `failed`).

- **M15b:coder DONE — orchestrator verified 2026-09-17** (run `1677db7c`, session `ses_f54bf19edffefgOCJwbkKy8Mzk`): chỉ sửa `docs/frontend-setup.md` (+13 dòng, 3 chỗ): blockquote sau bảng mục 2.1–2.3 (host/port 2 trường riêng, host KHÔNG kèm `ws://`, cảnh báo dán URL ghép thành `ws://ws://…`, ghi chú forward-looking M15a), 1 dòng bảng xử lý sự cố mục 6, 1 dòng chú thích bảng tham chiếu mục 7. URL trong các bảng giữ nguyên (đúng cho browser). `git status`: 19 file tracked modified y như trước dispatch — không đụng ngoài phạm vi. Reviewer/tester không cần: thay đổi thuần tài liệu.
- **M15a — thực hiện ở repo Mobile_Frontend** (user "ok" phương án khuyến nghị: chạy qua orchestrator của repo đó, không dispatch từ đây). Brief handoff (dán vào session orchestrator Mobile_Frontend):

```
Bug: app MQTT không connect — user dán nguyên URL "ws://localhost:9001" vào trường
"Địa chỉ máy chủ" (host); adapter ghép ws://${host}:${port} → URL hỏng
"ws://ws://localhost:9001:9001" rồi retry tới failed. Nguồn: bảng MQTT URL trong
Mobile_Backend/docs/frontend-setup.md.

Việc (M15a): normalize mqtt.host trong settings schema.

File: app-mobile/src/modules/settings/internal/domain/settingsSchema.ts
      (+ settingsSchema.test.ts)

Hành vi yêu cầu (zod, transform object-level vì port có thể bị override bởi URL):
1. host: trim.
2. Nếu bắt đầu bằng ws:// wss:// http:// https:// (case-insensitive) → parse URL:
   host mới = hostname (IPv6 giữ bracket); URL có port tường minh → port đó override
   trường port; không có → giữ port field.
3. Nếu KHÔNG có scheme nhưng khớp ^[^\s:/]+:\d{1,5}$ (vd "localhost:9001") → tách
   host + port tương tự.
4. Các trường hợp khác giữ nguyên.
5. Rỗng sau xử lý → lỗi validation "MQTT host is required" (không im lặng).

Test thêm: dán "ws://localhost:9001" → host localhost + port 9001;
"ws://192.168.2.34:9001" → host 192.168.2.34 + port 9001;
"wss://broker.example.com" (không port) → host broker.example.com, port giữ nguyên;
"localhost" không đổi; "localhost:9001" → tách đúng; "ws://" → validation error.
Test hiện có (host trần) phải pass không sửa expectation.

Verify: cd app-mobile && npm run typecheck && npm test.
```

- **Workaround tức thì (user, không cần M15a)**: app Settings → "Địa chỉ máy chủ" điền host trần: `localhost` (web trên máy Docker) hoặc `10.0.2.2` (Android emulator) hoặc IP LAN (điện thoại thật); Port `9001`; user/pass giữ nguyên → Save. Doc M15b đã ghi rõ cách này.
- **M15a:coder — REPO Mobile_Frontend** (ngoài project này — cần user duyệt cách chạy): sửa `app-mobile/src/modules/settings/internal/domain/settingsSchema.ts`:
  - Normalize `mqtt.host` bằng zod transform: nếu chứa scheme (`ws://`, `wss://`, `http://`, `https://`) → parse `URL` → lấy hostname (IPv6 giữ bracket `[::1]`).
  - URL có port tường minh → port đó override trường `port` (ý định gần nhất của user thắng); không có → giữ port field.
  - Rỗng sau khi strip → lỗi validation "MQTT host is required".
  - File kèm: `settingsSchema.test.ts` thêm case paste URL có/không port, host trần không đổi, IPv6, `ws://` trống.
  - Tiêu chí: `cd app-mobile && npm run typecheck && npm test` xanh; test hiện có không đổi expectation (đều dùng host trần).
- **M15b:coder — repo này**: `docs/frontend-setup.md` làm rõ app điền host KHÔNG scheme + port riêng (URL đầy đủ trong bảng giữ làm tham chiếu); ghi chú app (sau M15a) chấp nhận dán cả URL.
- **M15a sau khi xong**: user verify lại app connect MQTT (điền host trần hoặc dán cả URL đều phải connect được). Đóng M15 khi xác nhận.

### M16 (ĐÃ DUYỆT 2026-09-19 — user "ok" theo 5 default) — Firmware BLE provisioning: QR→BLE onboarding board mới

Duyệt 2026-09-19: (1) không BLE-cứu khi WiFi đã lưu sai — recovery qua nút BOOT; (2) `BOARD_TYPE` default `"IoT_ESP32-S2R3"` kèm lần flash; (3) NVS plain-text chấp nhận (ghi README); (4) broker = B+ không QR (mDNS M17 thay — BLE vẫn truyền broker xuống board); (5) nút BOOT ≥ 5 s xóa NVS provisioning → reboot BLE mode.

**Mở rộng contract GATT (3 characteristic broker — phần B+ không có trong contract frontend gốc 3a01–3a06, frontend amend BLE task sẽ thêm cho khớp):**

| Mục | UUID | Thuộc tính |
|---|---|---|
| Broker URI | `e5f4a3b2-1c0d-4e2f-9a8b-7c6d5e4f3a07` | WRITE — vd `mqtt://192.168.100.3:1883` |
| MQTT User | `e5f4a3b2-1c0d-4e2f-9a8b-7c6d5e4f3a08` | WRITE |
| MQTT Pass | `e5f4a3b2-1c0d-4e2f-9a8b-7c6d5e4f3a09` | WRITE, encrypted |

Fact bổ sung 2026-09-19 (orchestrator verify): NimBLE có sẵn trong core IDF v6.0.1 kèm examples local làm chuẩn API — `~/.espressif/v6.0.1/esp-idf/examples/bluetooth/nimble/bleprph` (GATT server + advertising + pairing) và `bleprph_wifi_coex` (đúng case WiFi+BLE).

Bối cảnh: app Mobile_Frontend đã code xong luồng `Quét QR board mới → sheet not-found → nút "Cấu hình WiFi qua Bluetooth" → BLE modal (scan theo service UUID, chọn board, form SSID/pass, gửi SSID → pass → PROVISION, chờ status notify)`. GATT contract đã chốt 2 phía (frontend `.ai/plans/current-plan.md` + `modules/devices/README.md`). Phần thiếu: firmware ESP32 chưa có BLE service — repo này implement.

**GATT contract (copy từ frontend — firmware PHẢI khớp từng byte):**

| Mục | UUID | Thuộc tính |
|---|---|---|
| Service | `e5f4a3b2-1c0d-4e2f-9a8b-7c6d5e4f3a01` | — |
| WiFi SSID | `e5f4a3b2-1c0d-4e2f-9a8b-7c6d5e4f3a02` | WRITE, encrypted |
| WiFi Password | `e5f4a3b2-1c0d-4e2f-9a8b-7c6d5e4f3a03` | WRITE, encrypted |
| Command | `e5f4a3b2-1c0d-4e2f-9a8b-7c6d5e4f3a04` | WRITE — nhận ASCII `PROVISION` |
| Device Info | `e5f4a3b2-1c0d-4e2f-9a8b-7c6d5e4f3a05` | READ — JSON `{"schemaVersion":1,"boardId":"0","boardType":"IoT_ESP32-S2R3"}` |
| Status | `e5f4a3b2-1c0d-4e2f-9a8b-7c6d5e4f3a06` | NOTIFY — `IDLE` / `CONNECTING` / `CONNECTED` / `FAILED:BAD_AUTH` / `FAILED:NO_SSID` / `FAILED:TIMEOUT` / `FAILED:ERROR` |

Hành vi firmware theo contract frontend (6 điểm): (1) boot KHÔNG có WiFi đã lưu → advertising service UUID + local name `IoTBoard-{boardId}`; (2) Device Info = JSON y format nhãn QR; (3) SSID/pass encrypted-write kích hoạt pairing Just Works + bond (v1 chấp nhận không-MITM); (4) `PROVISION` → connect WiFi bằng SSID/pass đã ghi, lưu NVS khi thành công; SSID ≤ 32 bytes UTF-8, pass ≤ 63 bytes (rỗng = mạng mở); (5) status notify như bảng — sau `CONNECTED` giữ BLE ~30 s rồi tắt; (6) ghi >MTU dùng write-with-response, stack tự reassemble ATT long write chuẩn (app request MTU 128).

Thiết kế:

1. **Component mới `components/ble_prov`** — stack **NimBLE** (đã verify có trong core ESP-IDF v6.0.1 tại `~/.espressif/v6.0.1/esp-idf/components/bt/host/nimble` — KHÔNG cần managed component):
   - GATT service + 5 characteristic đúng bảng UUID; Device Info build bằng cJSON (chung nguồn `BOARD_TYPE`/`DEVICE_ID` với descriptor mqtt_app).
   - Advertising: service UUID + local name `IoTBoard-{boardId}`.
   - SSID/pass: permission WRITE + WRITE_ENCRYPTED → lần ghi đầu trigger pairing Just Works + bond (IO cap no-input/no-output, bonding on).
   - Validate độ dài (SSID 1..32, pass 0..63); sai → log + status `FAILED:NO_SSID` khi PROVISION thiếu SSID.
   - State machine: nhận `PROVISION` → `CONNECTING` → `CONNECTED` (có IP; ghi NVS namespace riêng vd `bleprov`; timer 30 s rồi deinit BLE giải phóng RAM) / `FAILED:*` (classify WiFi disconnect reason: auth fail → `BAD_AUTH`, join timeout → `TIMEOUT`, khác → `ERROR`); app connect + subscribe → notify `IDLE`.
   - Kết quả connect WiFi báo về cho main qua callback/event, không tự đụng wifi_conn nội bộ qua lại.
2. **`wifi_conn` mở rộng**: API nhận credentials runtime; nguồn theo thứ tự **NVS provisioned → Kconfig (giữ hành vi board hiện có) → không có (chế độ BLE)**. Kconfig SSID rỗng không còn là lỗi boot — chuyển sang BLE mode. Classify reason code cho FAILED:*. Giữ tương thích call site main.c hiện có.
3. **BOARD_TYPE → Kconfig**: bỏ `#define BOARD_TYPE "A"` (`mqtt_app.c:45`), thêm Kconfig `BOARD_TYPE` string default `"IoT_ESP32-S2R3"` — descriptor MQTT và Device Info BLE dùng chung (fix lệch display app: title = boardType, ảnh map theo type slug `iot-esp32-s2r3.png`).
4. **sdkconfig.defaults + sdkconfig thật**: thêm `CONFIG_BT_ENABLED=y`, `CONFIG_BT_NIMBLE_ENABLED=y`, coexist WiFi/BLE; sửa `sdkconfig` theo protocol cũ (backup `/tmp/opencode/sdkconfig.backup-m16`, diff credentials byte-identical sau).
5. **RAM/flash note (rủi ro chính)**: binary hiện ~880 KB / partition 1536 KB; NimBLE thêm ~250–350 KB — dự kiến vẫn fit nhưng đầu line; BLE chỉ chạy khi WiFi chưa connect và tắt 30 s sau `CONNECTED` nên đỉnh heap chỉ ở giai đoạn chuyển tiếp.

| task_key | Việc | File chính | Tiêu chí đạt | Lệnh kiểm tra |
|---|---|---|---|---|
| M16a:coder | Component `ble_prov`: GATT service + 8 char (6 contract gốc + 3 broker mở rộng), advertising `IoTBoard-{boardId}`, Just Works bond, validate, state machine status, NVS save + Kconfig BLE/BOARD_TYPE + sdkconfig | `components/ble_prov/**`, `main/Kconfig.projbuild`, `sdkconfig.defaults`, `sdkconfig`, `components/mqtt_app/mqtt_app.c` (BOARD_TYPE) | `idf.py build` xanh không warning; UUID + hành vi khớp contract từng byte; sdkconfig credentials byte-identical | `. ~/.espressif/tools/activate_idf_v6.0.1.sh && cd firmware/esp32-telemetry && idf.py build` |
|---|---|---|---|---|
| **M16a:coder DONE — orchestrator verified 2026-09-19** (session `ses_f46615137ffeEcAqx7R9WCFgf6`): build exit 0 do chính orchestrator chạy lại + **0 compiler warning** trong stderr log; binary 916.960 B (42% partition free — BLE CHƯA link vì main.c chưa tham chiếu, đúng thiết kế); coder đã force-link test riêng: +208 KB NimBLE/coex → 1.099 KB (29% free) — fit; **credentials sdkconfig byte-identical** backup (orchestrator diff lại PASS); cả 9 UUID đúng dạng byte NimBLE (LSB-first, macro đuôi chung, comment giải thích); mqtt_app.c chỉ đổi BOARD_TYPE→`CONFIG_BOARD_TYPE`; Kconfig chỉ thêm `BOARD_TYPE` default `IoT_ESP32-S2R3`; sdkconfig resolve đúng `BT_NIMBLE_ENABLED` + `NVS_PERSIST` + `BLE_ONLY` + trim role CENTRAL/OBSERVER + `ESP_COEX_SW_COEXIST_ENABLE` (tên mới thay `SW_COEXIST_ENABLE` legacy). Decisions đáng chú ý của coder: length sai → NACK `BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN` (không ACK-and-bỏ); long write NimBLE tự reassemble (đã đọc source xác nhận); mutex thread-safety host task↔main; Device Info snprintf + escape. | | | |
| M16b:coder | Tích hợp: `wifi_conn` credentials runtime (NVS → Kconfig → BLE mode) + classify reason → FAILED:*, `mqtt_app` nhận broker config runtime, main.c wiring BLE lifecycle (init khi không có creds, deinit 30 s sau CONNECTED, nút BOOT ≥ 5 s erase) | `components/wifi_conn/**`, `components/mqtt_app/**`, `main/main.c` | `idf.py build` xanh; board có Kconfig SSID boot bình thường KHÔNG BLE | idem |
|---|---|---|---|---|
| **M16b:coder DONE — orchestrator verified 2026-09-19** (session `ses_f4594c59affegm1B463Y3NusDk`): orchestrator tự build lại exit 0 — binary `0x112e70` (1,074 MB, 28% partition free), **BLE link thật** (nm: 18 symbol `ble_prov_*`/`nimble_port_*`); sdkconfig KHÔNG đụng (git status sạch). mqtt_app KHÔNG đụng (config vốn runtime). Đọc main.c verify wiring: 4a NVS → 4b Kconfig (kconfig_ssid_present) → 4c BLE mode; BOOT task debounce 2 mẫu ổn định + giữ 5 s → `ble_prov_erase` + restart; on_provision chỉ copy+signal (không block host task); đợi IP 30 s slice 250 ms + early-exit BAD_AUTH; vòng provisioning retry vô hạn. wifi_conn: `apply_credentials` (esp_wifi_set_config + reset backoff, không re-init netif) + fail-reason query 3 lớp. **Finding quan trọng coder (đã verify logic)**: GCC constant-fold `CONFIG_WIFI_SSID[0] != '\0'` (literal non-empty) → dead-code-eliminate cả nhánh BLE + NimBLE khỏi binary (map file xác nhận discarded); fix = `kconfig_ssid_present()` đánh dấu `__attribute__((noinline))` → nhánh thành runtime call, 1 binary phục vụ cả 3 chế độ. Mapping reason code → BAD_AUTH/TIMEOUT/ERROR đầy đủ (bảng trong báo cáo coder; caveat 210/211 xếp TIMEOUT dù có thể là security-mismatch — reviewer/harness lưu ý). Ghi chú: `ble_prov_stop` chạy trong esp_timer task, `nimble_port_stop` block ≤2 s làm trễ esp_timer khác một lần sau CONNECTED — chấp nhận. | | | |
| M16c:coder | README mục BLE provisioning (bảng UUID 8 char + hành vi, hạn chế Just Works không-MITM, NVS pass plain-text, phục hồi nút BOOT, sinh QR label qrencode JSON Device Info) + host harness test logic thuần | `README.md`; harness `/tmp/opencode/m16-harness/**` (không vào repo) | README khớp contract; harness PASS (validate độ dài, state machine, JSON Device Info, mapping reason→FAILED:*) | harness script exit 0 + `idf.py build` |
|---|---|---|---|---|
| **M16c:coder DONE — orchestrator verified 2026-09-19** (session `ses_f4574b3ceffe1ICWNsTBR2xVGy`): orchestrator chạy lại harness **130/130 PASS exit 0** (46 wifi_conn + 84 ble_prov, `#include` .c THẬT pattern M9b — classify đủ bảng reason, validation 18 length case, Device Info JSON byte-exact + escape, status 7 chuỗi, PROVISION case-sensitive, thêm flow PROVISION→NVS 5 keys→load/erase round-trip, backoff reset, adv name truncate 29B); build lại "Project build complete" sau Kconfig help text mới; untracked chỉ `components/ble_prov/` (M16a). README mục `## 10. BLE provisioning (M16)` @dòng 310 (10.1–10.7 đủ: 9 UUID, hành vi, 3 chế độ boot + build rút hộp, BOOT recovery, qrencode, 5 hạn chế, luồng end-to-end) + renumber mục 10–13→11–14 + sửa các chỗ khẳng định hành vi cũ (SSID rỗng từ chối start → BLE mode); Kconfig help WIFI_SSID sửa đúng hành vi mới.

**M16 (a+b+c) — firmware DONE toàn bộ 2026-09-19.** Còn lại:
1. **On-target test (user)** — flash board `CONFIG_WIFI_SSID=""` (hoặc erase NVS bleprov) → quét BLE `IoTBoard-0` từ app → pair Just Works → đẩy SSID/pass/broker/MQTT → PROVISION → verify CONNECTED, BLE tắt 30 s, boot sau dùng NVS, BOOT 5 s quay lại provisioning. **Điều kiện**: app frontend phải amend BLE task trước (3 characteristic broker 3a07–3a09 + prefill broker từ Settings) — repo Mobile_Frontend.
2. Board "0" hiện offline trỏ broker chết `192.168.2.28` — lần flash M16 giải quyết luôn: v2 contract (M14b) + boardType `IoT_ESP32-S2R3` + BLE + broker runtime một lần flash. M14c (chuyển tiếp board 0) gộp vào luồng này: sau flash xong thì `.env` `LEGACY_BOARDS=` rỗng + `clear-legacy-retained.sh 0` + verify end-to-end.

Thứ tự: M16a → M16b → M16c tuần tự (một coder). Sau mỗi dispatch orchestrator tự verify `idf.py build` + diff sdkconfig credentials (pattern M9/M14 — không dispatch reviewer/tester riêng cho firmware trừ khi user muốn). Test end-to-end trên phần cứng: user (cần app Android native rebuild + flash board M16).

Lưu ý ranh giới v1 (khớp out-of-scope frontend): DEVICE_ID vẫn qua Kconfig build-time (mỗi board một bản build — quy trình hiện tại; QR label sinh từ descriptor sau khi board lên lần đầu); KHÔNG provisioning khi board ĐÃ có WiFi đã lưu.

**Broker config — quyết định phát sinh 2026-09-19 (câu hỏi user: "làm sao user biết broker")**:
- Phương án A (v1 nguyên bản, đúng contract frontend hiện tại): broker URI + MQTT user/pass nằm trong Kconfig firmware từ lúc build — người build/flash biết broker, người quét QR trên app KHÔNG cần biết. Hạn chế: board mới vẫn phải build/flash từng con với cấu hình nhà mình; đổi broker là reflash.
- Phương án B+ (đề xuất — mô hình "hub + QR" user chốt 2026-09-19: server + board chung WiFi nhà, QR server là ngu gốc địa chỉ):
  - **QR server** (dán trên máy Docker, sinh bằng `qrencode` — đã có trên host): JSON KHÔNG chứa secret `{"schemaVersion":1,"brokerHost":"<IP-LAN>","wsPort":9001,"mqttPort":1883}`. App quét lần đầu → prefill broker host + WS port vào Settings (pass MQTT gõ tay một lần, không in vào QR).
  - **BLE truyền broker xuống board**: thêm 3 characteristic — Broker URI (WRITE, vd `mqtt://192.168.100.3:1883`; esp-mqtt hỗ trợ cả `ws://`; prefill host từ app Settings + port 1883, sửa được), MQTT User (WRITE), MQTT Pass (WRITE, encrypted). App gửi SSID/pass WiFi + broker + MQTT auth trong MỘT phiên provisioning.
  - Firmware: NVS `bleprov` lưu trọn bộ WiFi + broker + MQTT auth; nguồn cấu hình runtime: NVS → Kconfig fallback (`mqtt_app` nhận config runtime giống `wifi_conn` ở M16b). Board hiện có (Kconfig) không đổi hành vi.
  - Recovery khi provision sai (WiFi OK nhưng broker fail — rule v1 BLE không bật lại): giữ nút BOOT (GPIO0) ≥ 5 s khi board đang chạy → xóa NVS provisioning → reboot vào lại BLE mode, không cần cắm USB.
  - **Điều kiện**: IP máy server cố định (DHCP reservation trên router — việc một lần; IP đổi thì chạy lại script in QR mới). mDNS (`smarthome.local`) DEFER: IDF v6 bỏ mdns khỏi core (phải managed component `espressif/mdns`) + app RN cần lib zeroconf — phình hai phía, không đáng cho nhà 1 server + vài board.
  - Frontend (2 việc nhỏ, repo Mobile_Frontend): Settings thêm "Quét mã máy chủ" prefill broker; BLE modal thêm mục Broker prefill từ Settings — fold vào đợt amend BLE task (CHƯA accept/commit bên đó — thời điểm rẻ nhất đổi contract). Firmware hỗ trợ char broker sẵn thì app chưa gửi cũng vô hại.
  - Nếu chọn B+: M16a thêm 3 characteristic broker vào bảng char; M16b mở rộng `mqtt_app` nhận broker config runtime; M16c thêm `scripts/server-qr.sh` (đọc IP LAN host, sinh QR qrencode) + README mục onboarding 2 bước (QR server → QR board).

Câu hỏi mở (trả lời khi duyệt — mặc định theo phương án ghi):
1. WiFi đã lưu nhưng sai (đổi pass router): v1 KHÔNG bật BLE cứu — phục hồi qua cơ chế nút BOOT (câu 5). Thêm fallback "fail N phút → bật BLE" sau này nếu cần. (default: KHÔNG làm)
2. `BOARD_TYPE` default `"IoT_ESP32-S2R3"` kèm lần flash này — fix lệch display app. (default: CÓ)
3. NVS lưu WiFi pass + MQTT pass plain-text (board không bật flash encryption) — hạn chế ghi rõ README. (default: CÓ, chấp nhận)
4. Broker: **A** giữ Kconfig (đúng contract frontend hiện tại, không đụng app) hay **B+** QR server (script sinh QR không-secret, app quét lần đầu prefill Settings) + BLE truyền broker xuống board (3 char mới, NVS → Kconfig fallback)? (default đề xuất: **B+** — mô hình user mô tả 2026-09-19)
5. Recovery tổng quát: giữ nút BOOT ≥ 5 s khi board đang chạy → xóa NVS provisioning (WiFi + broker nếu có) → reboot vào BLE mode — hữu ích cho cả A lẫn B+. (default: CÓ)

**Cập nhật 2026-09-19 (sau M17)**: phần "QR server" của B+ **BỎ khỏi luồng chính** — M17 (mDNS) thay: app tự dò broker trên LAN, không ai in QR IP. Giữ lại từ B+: 3 characteristic broker + MQTT auth qua BLE, NVS `bleprov` (NVS → Kconfig fallback), nút BOOT recovery. BLE modal frontend prefill broker từ Settings của app (nguồn = mDNS discovery).

### M18 (ĐÃ DUYỆT 2026-09-21 — user "hướng này đi") — boardId tự sinh từ MAC (hex-8) khi DEVICE_ID rỗng

Quyết định identity mới (thay quy ước gán tay "0"–"9" của M13, nâng cấp ý hex-8 unique của user 2026-09-21):
- **Nguồn unique duy nhất: MAC eFuse nhà máy** (Espressif đốt, toàn cầu, không đổi). `boardId` = 4 byte cuối MAC dạng hex-8 (vd MAC `5c:01:3b:6b:af:6c` → `"3b6baf6c"`), sinh lúc boot vào RAM — ổn định vì MAC không đổi, không cần NVS.
- `CONFIG_DEVICE_ID` **không rỗng** → giữ hành vi gán tay (back-compat board "0" nếu muốn). **Rỗng (default mới)** → tự sinh từ MAC. Một binary generic flash đại trà, không có bước gán id từng board.
- QR label chuyển sang **in SAU flash**: `scripts/board-qr.sh` đọc MAC (esptool) → qrencode JSON `{"schemaVersion":1,"boardId":"<hex8>","boardType":"IoT_ESP32-S2R3"}`.
- App/backend/bridge **zero thay đổi** (regex `[a-zA-Z0-9_-]+` chấp nhận hex; app hiển thị `Id: <hex8>` + title theo boardType; 3 bên đều "đọc id từ board").
- Kết quả: board của user (`5c:01:3b:6b:af:6c`) sẽ thành `boardId "3b6baf6c"` — chấp nhận mất liên kết history Influx cũ tag `boardId=0` (board 0 nghỉ hưu; dữ liệu cũ vẫn query được, không backfill).
- Sau flash board: `LEGACY_BOARDS=` rỗng (board "0" không còn tồn tại trên wire) + `bash scripts/clear-legacy-retained.sh 0` — gộp đóng M14c.

| task_key | Việc | File chính | Tiêu chí đạt | Lệnh kiểm tra |
|---|---|---|---|---|
| M18:coder | (1) main.c: hàm thuần sinh boardId từ MAC 4 byte cuối (hex-8, lowercase) khi `CONFIG_DEVICE_ID` rỗng — dùng cho CẢ 3 chế độ boot (mqtt_app + ble_prov cùng id); (2) Kconfig `DEVICE_ID` default `""` + help mới (rỗng = auto MAC; Kconfig hiện default "esp32-01"); (3) sdkconfig thật đổi `DEVICE_ID=""` (protocol backup + credentials byte-identical); (4) `scripts/board-qr.sh` mới (đọc MAC qua esptool hoặc tham số, sinh JSON + qrencode, không dùng generator bọc URL); (5) README mục identity/QR cập nhật flow in-sau-flash; (6) harness M16c thêm test hàm format (tách hàm thuần testable được) | `main/main.c`, `main/Kconfig.projbuild`, `sdkconfig`, `scripts/board-qr.sh`, `README.md`, `/tmp/opencode/m18-harness/` hoặc extend m16-harness | `idf.py build` xanh 0 warning; log boot in rõ nguồn id (`boardId=3b6baf6c (from MAC)`); harness PASS; script `bash -n` sạch + chạy demo được với MAC tham số; sdkconfig credentials byte-identical | idf.py build + harness run.sh + bash -n |
|---|---|---|---|---|
| **M18:coder DONE — orchestrator verified 2026-09-21** (session `ses_f3faa3a72ffey9xzRTuYaGQcZb`): build exit 0 (0x113c90, 28% free, 0 warning) + harness **148/148** (46+84+18 — 18 test mới `test_board_id.c`) — cả hai do orchestrator chạy lại; `board-qr.sh` verify: JSON đúng `{"boardId":"3b6baf6c",...}` + QR render, MAC sai exit 1 + hướng dẫn, `--board-id`/`-t` chạy đúng; nm xác nhận `resolve_board_id` (noinline) + `board_id_from_mac` link thật — KHÔNG dead-strip; sdkconfig đã sẵn `DEVICE_ID=""` từ flash trước của user (không cần sửa — backup `/tmp/opencode/sdkconfig.backup-m18` byte-identical). Files: `main/board_id.{c,h}` mới (pure), main.c resolve trước 3 nhánh boot + log `boardId="…" (from MAC)`, Kconfig default `""`, CMake SRCS, README 4 chỗ. **Root cause M16d cũng đóng luôn**: board loop gốc rễ = `DEVICE_ID=""` mà firmware cũ từ chối — không phải lỗi NimBLE (M16d hardening đã đúng + giờ rỗng là hành vi CHÍNH THỨC). |
- **M18 residual — PENDING**: `sdkconfig.defaults` dòng ~45 vẫn `CONFIG_DEVICE_ID="esp32-01"` (ngoài scope coder lần này) — chỉ ảnh hưởng khi xóa `sdkconfig`/clone mới (defaults sẽ ghim id cố định). Fold vào coder dispatch kế tiếp.
- **M17b — căn mDNS theo contract app 2026-09-21 (coder DONE + orchestrator verified, session `ses_f3f58a9b1ffe17kScqQVQK71uN`)**: app dò thấy server FAIL vì lệch contract — app browse `_smarthome._tcp` :9001 + TXT gạch dưới (`influx_port/influx_org/influx_bucket`) trong khi M17 quảng bá `_mqtt._tcp` :1883 + TXT gạch ngang. Frontend đã tự hoàn thành 2 task liên quan (session repo đó: `settings-mdns-discovery` + `ble-provisioning-v2-broker-push` — 3 char broker `3a07–3a09` ĐÃ có mặt phía app!). Fix: `avahi/smarthome.service` đổi type `_smarthome._tcp`, port **9001** (WS — app lấy thẳng làm mqtt.port), TXT đủ 4 keys với giá trị THẬT từ `.env` (org `smarthome`, bucket **`telemetry`** — README frontend ghi bucket `smarthome` là VÍ DỤ SAI, đã tránh); `server-init.sh` bước verify browse `_smarthome._tcp` + tóm tắt; README 4 chỗ. Verify: XML_OK + BASH_OK + NO_STALE_REFS. **User chạy lại `bash scripts/server-init.sh` để cp file mới (sudo) — xong app "Tìm máy chủ trong mạng" sẽ thấy server.**
- **Phát hiện thêm từ README frontend**: task `settings-secrets-qr` phía app muốn `server-init.sh` in QR credentials (`{"schemaVersion":1,"kind":"credentials","mqttUsername":...,"mqttPassword":...,"influxToken":...}` qrencode ANSIUTF8) cho chủ hệ thống quét lần đầu — việc phía server CHƯA làm, để cân nhắc milestone sau (có secret trong QR terminal — cân nhắc bảo mật trước khi làm).

Sau M18:coder — orchestrator verify + user flash board (menuconfig: WiFi `Truong Lung` + broker `mqtt://192.192.100.3:1883`, để `DEVICE_ID` rỗng) → log phải hiện `boardId=3b6baf6c (from MAC)` → orchestrator đóng M14c (LEGACY_BOARDS rỗng + dọn retained board "0") + sinh QR label cho board.

### M16d (2026-09-21) — Fix BLE boot loop trên board thật + harden init

**Bug (log user 2026-09-21)**: flash `CONFIG_WIFI_SSID=""` → BLE mode → `ESP_ERROR_CHECK(ble_prov_start)` (main.c:302) abort → **boot loop vô hạn** (mỗi vòng reboot reset relay — nguy hiểm nếu gắn tải thật). Dòng lỗi gốc bị mất (board rút USB trước khi bắt serial); addr2line + ELF SHA256 `d85373bf0` khớp → mapping dòng 302 chắc chắn.

- **M16d:coder DONE — orchestrator verified 2026-09-21** (session `ses_f40053ca5ffe2UiLcDgdPlKdOw`): build exit 0 (0x113ad0, 28% free, 0 warning) + harness 130/130 PASS (orchestrator chạy lại cả hai). Thay đổi: (1) `ble_prov_start` refactor goto-cleanup 5 bước + log tiến trình từng bước (`step n/5 ok/failed`) — step fail rollback đúng thứ tự ngược (đã đọc source `nimble_port.c` v6.0.1: nhánh fail tự deinit controller); (2) main.c **hết abort**: retry 3 lần backoff 5 s → vẫn fail → SAFE MODE (park, không reboot, relay OFF giữ nguyên, nút BOOT 5 s vẫn hoạt động); toàn bộ đường provisioning fail-soft (wifi/mqtt lỗi → `FAILED:ERROR` cho app + tiếp vòng); latch chống double-init wifi/telemetry client; (3) `log_ble_boot_diagnostics()` trước BLE start: heap free/min-ever + MAC eFuse + MAC STA + compile-time BT config; (4) README 10.8 sự cố BLE.
- **Gốc rễ CHƯA chốt** (không có dòng lỗi thật): coder đối chiếu từng bước + từng config với bleprph example — không sai khác; nghi vấn: controller init/enable fail (chip-rev/nguồn) hoặc esp_nimble_init NO_MEM hoặc service register rc≠0. **Lần flash tới log `step n/5` sẽ phân biệt 100%** — chờ user flash lại.
- **Next user**: `idf.py -p /dev/ttyUSB0 flash monitor` → gửi các dòng: `BLE diagnostics:`, `step n/5`, `attempt n/3 failed`. Mong đợi: BLE lên advertising HOẶC board SAFE MODE im lặng (không boot loop) + log đủ chốt gốc rễ.

### M19 (CHỜ DUYỆT 2026-09-21) — Firmware normalize broker URI scheme-less (bắt lỗi on-target M16)

Chẩn đoán gốc (log user 2026-09-21, 100% khớp code):
- App đẩy qua BLE char `3a07` giá trị `192.168.2.28:1883` — **không có scheme** (contract ghi ví dụ `mqtt://192.168.100.3:1883`; app prefill host+port từ Settings nên compose scheme-less).
- `mqtt_app_init` copy `broker_uri` nguyên vẹn vào `s_broker_uri` → `esp_mqtt_client_init` parse URI fail (`Error parse uri = 192.168.2.28:1883`) → ESP_FAIL → `start_telemetry` fail-soft → `FAILED:ERROR` cho app (đúng thiết kế M16d), NVS KHÔNG lưu gì.
- Địa chỉ+bảng: máy server hiện 192.168.2.28 (cùng subnet board), mosquitto healthy `0.0.0.0:1883` — sai DUY NHẤT là thiếu scheme.
- Nhánh an toàn đã chạy đúng: WiFi retry OK, UTF-8 SSID OK, board tự resume advertising sau disconnect (reason 531) — user re-provision được ngay không cần erase NVS.

Quyết định hướng fix: **normalize tại nơi tiêu thụ (firmware)** thay vì bắt app gửi đúng scheme — một text field/app/user luôn có thể gửi `host:port`; firmware tự bù `mqtt://` khi thiếu (cùng tinh thần M15a normalize host phía app). Che mọi nguồn cấu hình: BLE, NVS, Kconfig.

Thiết kế:
1. Hàm thuần tách file riêng trong component mqtt_app — `broker_uri.c` + `include/broker_uri.h` (pattern `main/board_id.{c,h}` M18; harness compile .c thật trên host mà không phải stub cả mqtt_app.c):
   `mqtt_app_normalize_broker_uri(const char *in, char *out, size_t out_len)` → `esp_err_t`:
   - `in` chứa `://` → copy nguyên vẹn (không đụng scheme đã có).
   - Không chứa `://` → prepend `mqtt://`.
   - Không đủ chỗ (cần strlen+8 > out_len khi prepend, strlen+1 > out_len khi copy) → `ESP_ERR_INVALID_SIZE` — **không cắt cụt** (URI cắt cụt = host sai, âm thầm hại hơn).
   - `in` NULL/rỗng → `ESP_ERR_INVALID_ARG` (khớp check hiện có của mqtt_app_init).
   - Hàm thuần không global state, không log trong hàm (log một dòng ESP_LOGI tại caller khi đã bù scheme — khả năng chẩn đoán serial). KHÔNG lowercase scheme, KHÔNG strip whitespace (ble_prov đã trim field của nó; Kconfig/NVS là việc người đặt).
   - Gọi trong `mqtt_app_init` thay dòng `fmt_bounded(s_broker_uri, ...)` hiện tại; `broker_uri.c` thêm vào SRCS CMakeLists component (format `idf_component_register` hiện có).
   - KHÔNG xử lý scheme viết hoa (edge hiếm — esp-mqtt tự từ chối + fail-soft như hiện tại).
2. Harness mới `/tmp/opencode/m19-harness/` (harness M16/M18 đã bị dọn khỏi /tmp): gcc host + stub `esp_err.h`, compile `broker_uri.c` THẬT. Test: scheme-less → bù; có scheme (`mqtt://`, `ws://`, `mqtts://`) nguyên vẹn; buffer vừa đủ cận → INVALID_SIZE; buffer đủ chính xác → OK; rỗng/NULL → INVALID_ARG; chuỗi dài gần 128; out không bẩn khi fail.
3. Doc: README các chỗ nói broker URI + Kconfig `MQTT_BROKER_URI` help (KHÔNG đổi default) ghi scheme tùy chọn (thiếu = tự bù `mqtt://`).
4. **Fold M18 residual**: `sdkconfig.defaults` `CONFIG_DEVICE_ID="esp32-01"` → `""` (đúng default M18 — chỉ ảnh hưởng clone mới/xóa sdkconfig; `sdkconfig` thật đã là `""` từ flash trước).

| task_key | Việc | File chính | Tiêu chí đạt | Lệnh kiểm tra |
|---|---|---|---|---|
| M19:coder | (1) `broker_uri.{c,h}` + gọi trong `mqtt_app_init` + CMakeLists SRCS; (2) harness m19 test normalize; (3) README + Kconfig help scheme tùy chọn; (4) `sdkconfig.defaults` `CONFIG_DEVICE_ID=""` | `components/mqtt_app/broker_uri.c`, `components/mqtt_app/include/broker_uri.h`, `components/mqtt_app/CMakeLists.txt`, `components/mqtt_app/mqtt_app.c`, `main/Kconfig.projbuild` (help), `sdkconfig.defaults`, `README.md`, harness `/tmp/opencode/` | `idf.py build` xanh 0 warning; harness PASS (case mới + case cận biên); `git diff sdkconfig.defaults` chỉ đúng dòng DEVICE_ID; git status footprint đúng danh sách file | `. ~/.espressif/tools/activate_idf_v6.0.1.sh && cd firmware/esp32-telemetry && idf.py build` + harness run |
|---|---|---|---|---|
| **M19:coder DONE — orchestrator verified 2026-09-22** (run `014bb9ea`, session `ses_f3b0fde00ffeNEqYgPCtAJQvhA`): cả hai lệnh kiểm tra do orchestrator TỰ CHẠY LẠI — `idf.py build` exit 0 (ép touch recompile `broker_uri.c` → **0 warning** trong log) + harness host **15/15 PASS exit 0** (gcc `-Werror`, compile `broker_uri.c` THẬT từ repo + stub `esp_err.h`; biên `BROKER_URI_MAX=128`: scheme-less 120 ký tự → OK vừa khít, 121 → INVALID_SIZE — spec gốc của orchestrator sai số học ở case này, hướng xử lý của coder đúng, chấp nhận). File: `broker_uri.{c,h}` mới (hàm thuần, fail KHÔNG ghi bẩn `out` — test j xác nhận bằng sentinel); `mqtt_app_init` gọi normalize thay `fmt_bounded` + một dòng `ESP_LOGI` khi bù scheme; CMakeLists thêm SRCS; Kconfig help + README 2 chỗ (bảng mục 4 + bullet mục 6) ghi scheme tùy chọn; `sdkconfig.defaults` chỉ đúng 1 dòng `CONFIG_DEVICE_ID=""` — **đóng residual M18**. Footprint git: baseline +1 M (CMakeLists) +2 ?? (`broker_uri.{c,h}`) — đúng danh sách; `sdkconfig` credentials KHÔNG đụng (mtime cũ hơn dispatch). Sự cố phiên: máy reboot ~12:03 2026-09-22 xóa sạch `/tmp` (harness + build log của coder mất) → resume CÙNG session coder để tái tạo harness rồi orchestrator re-verify — kết quả khớp lần đầu. Allegation "actor ngoài sửa CMakeLists 00:12:19" chốt = edit đầu của chính coder đã áp dụng thành công nhưng bị báo lỗi, retry gặp oldString-mismatch → coder hiểu nhầm; 2 process opencode khác chạy cùng lúc đó thuộc repo Mobile_Frontend/Blockchain (cwd khác, không ghi được worktree này); sau 00:16:30 không còn ghi nào vào repo ngoài state plugin. Ghi chú vận hành: (1) `/` đang 98% đầy (4.2G trống) — cân nhắc dọn trước khi build lớn tiếp theo; (2) harness nằm `/tmp` sẽ lại mất khi reboot — nếu muốn lưu vết dài hạn, copy `run.sh` + `test_broker_uri.c` vào repo ở milestone sau. | | | |

Sau M19:coder → orchestrator verify (build + harness + diff) → **user flash + re-provision** (broker nhập lại y `192.168.2.28:1883` cũng chạy) → kỳ vọng: `CONNECTED`, BLE tắt sau 30 s, telemetry v2 lên `smarthome/boards/3b6baf6c/...`, backend ingest (board 3b6baf6c không nằm trong LEGACY_BOARDS → đường v2 sạch) → orchestrator verify Influx + đóng M14c (`.env` `LEGACY_BOARDS=` rỗng + `clear-legacy-retained.sh 0`) + sinh QR label `scripts/board-qr.sh` cho board `3b6baf6c`.

**Workaround tức thì (không cần M19)**: nếu trường broker trong BLE modal app cho gõ text tự do → sửa thành `mqtt://192.168.2.28:1883` rồi PROVISION lại ngay (board đang advertising sẵn). Nếu field bị prefill khóa/giới hạn thì phải flash firmware fix. M19 vẫn đáng làm để schema-less vô hại vĩnh viễn.

### M20 (CHỜ DUYỆT 2026-09-23) — Fix race `wifi_conn_apply_credentials` khi driver đang connecting

Chẩn đoán gốc (log on-target 2026-09-23, user — M19 fix CHƯA được quan sát live vì flow bị chặn sớm hơn ở bước WiFi):

```
E (379698) wifi:sta is connecting, cannot set config
E (379708) wifi_conn: esp_wifi_set_config failed: ESP_ERR_WIFI_STATE
E (379708) main: wifi_conn_apply_credentials failed: ESP_ERR_WIFI_STATE (0x3006)
```

Chuỗi sự kiện trong log: lần PROVISION trước (cùng boot) fail WiFi với reason 15 (`4WAY_HANDSHAKE_TIMEOUT` — sai password) → vòng retry `esp_timer` của wifi_conn chạy nền liên tục với credentials sai; app gửi PROVISION thứ hai với giá trị đã sửa → `wifi_conn_apply_credentials` gọi `esp_wifi_set_config` **TRƯỚC KHI** abort attempt đang treo → driver từ chối vì "sta is connecting" → main báo FAILED:ERROR dù credentials mới đúng. Comment hiện tại (`wifi_conn.c:304-306`) xây trên giả định sai — set_config KHÔNG được phép khi driver đang connecting. Bug tồn tại từ M16b, chưa bị chạm tới vì on-target test trước fail ở bước broker (M19) trước khi ai re-provision giữa vòng retry. Lưu ý: reason 15 trong log cũng xác nhận lần gửi trước sai password (sửa giá trị rồi gửi lại là đúng hướng của user).

Thiết kế:
1. Đảo thứ tự + chờ idle trong `wifi_conn_apply_credentials` (`components/wifi_conn/wifi_conn.c`):
   - Flag `s_apply_in_progress` (critical section, dùng chung `s_lock`).
   - Handler `WIFI_EVENT_STA_DISCONNECTED`: khi đang apply → set signal `s_apply_settled = true`, KHÔNG update fail-state, KHÔNG schedule retry, KHÔNG đếm attempt (abort cục bộ không phải failure của credentials); `retry_timer_cb` cũng check flag trước khi connect (belt-and-braces — esp_timer_stop không hủy callback đã dequeue).
   - Thứ tự mới: set flag + stop timer + reset counters/fail (như hiện tại) → `esp_wifi_disconnect()` (abort in-flight; lỗi return = driver đã idle → skip wait) → nếu disconnect trả OK: poll `s_apply_settled` bounded (slice 10–20 ms, cap 2 s) → `esp_wifi_set_config` (vẫn `ESP_ERR_WIFI_STATE` do race tồn → micro-retry ≤ 5 × 100 ms) → clear flag → `esp_wifi_connect()` (reconnect tường minh — handler đã bị suppress trong lúc apply, còn `wifi_conn_start()` của main là no-op khi đã started).
   - Sau khi connect: classify/fail-state thuộc về attempt mới như thường (main `wait_provisioning_ip` early-exit BAD_AUTH hoạt động nguyên vẹn).
2. (Hygiene — được phép bỏ nếu coder/review thấy rủi ro) `wifi_conn_suspend_retries()` mới: stop timer + disconnect + suppress — main gọi sau khi báo FAILED do WiFi trong BLE loop, ngừng churn retry với password sai trong lúc chờ app gửi lại (giảm spam log + interference coex WiFi/BLE — log user thấy `Coexist: Wi-Fi connect fail` lặp liên tục).
3. Harness `/tmp/opencode/m20-harness/` (pattern M16c; harness 46 test wifi_conn cũ đã bị /tmp wipe — chỉ tái tạo phần liên quan): compile `wifi_conn.c` THẬT với stub esp_wifi/esp_timer/esp_event. Assert: thứ tự gọi disconnect→set_config→connect; retry bị suppress trong apply; fail-state không bị abort-event pollute (apply xong vẫn NONE cho tới attempt mới); counter reset; bounded loop chấm dứt đúng cap; apply khi driver idle (disconnect trả lỗi) → không đợi 2 s; apply khi connected → có wait signal.
4. README mục 10.8 (sự cố BLE): từ M20 re-provision giữa vòng retry an toàn.

| task_key | Việc | File chính | Tiêu chí đạt | Lệnh kiểm tra |
|---|---|---|---|---|
| M20:coder | (1) apply race fix + handler suppress; (2) `wifi_conn_suspend_retries` + main gọi sau FAILED WiFi; (3) harness; (4) README 10.8 | `components/wifi_conn/wifi_conn.c`, `components/wifi_conn/include/wifi_conn.h`, `main/main.c` (chỗ gọi suspend), `README.md`, harness `/tmp/opencode/` | `idf.py build` xanh 0 warning; harness PASS; main.c chỉ đổi tối thiểu chỗ gọi suspend; footprint git đúng danh sách | `. ~/.espressif/tools/activate_idf_v6.0.1.sh && cd firmware/esp32-telemetry && idf.py build` + harness run |
|---|---|---|---|---|
| **M20:coder DONE — orchestrator verified 2026-09-23** (run `6d2b0587`, session `ses_f35ddfea9ffeiJBfjWm2qOxyDd`): cả hai lệnh kiểm tra do orchestrator TỰ CHẠY LẠI — `idf.py build` exit 0 **0 warning** (binary 1.130.592 B, +736 B so M19) + harness host **51/51 PASS exit 0** (gcc `-Werror -O2`, compile `wifi_conn.c` THẬT + 12 stub header; 9 test group: repro bug M20, idle/connected/capped-settle, micro-retry, always-fail, suspend/resume, regression boot thường). Đọc diff verify thiết kế đúng từng chi tiết: guard `s_apply_in_progress` set ngay đầu apply (clear suspended cùng lúc — apply là đường resume duy nhất) → stop timer → `esp_wifi_disconnect()` (lỗi = idle → skip wait) → poll settled 20 ms/cap 2 s → `set_config` micro-retry 5×100 ms → **guard drop TRƯỚC `esp_wifi_connect()`** (disconnect của attempt MỚI classify thật — early-exit BAD_AUTH của main nguyên vẹn); handler suppress cả 2 nhánh (apply/suspended) đều VẪN clear CONNECTED bit, không đụng fail-state/backoff; `retry_timer_cb` belt-and-braces. main.c +4 dòng đúng call site (sau report ở nhánh no-IP — nhánh BAD_AUTH early-break cùng rơi vào đây). 5 quyết định lệch spec của coder đều hợp lý (đọc trong session): connect-error cuối apply = log + ESP_OK (fail-soft 30 s của main giữ contract), harness reset ladder qua apply (process 1 lần), nhánh suppress vẫn set `s_has_ip=false`... mtimes xác nhận chỉ 4 file được phép đổi (00:34–00:36). | | | |

Sau M20:coder → orchestrator verify (build + harness + đọc diff) → **user flash binary cumulative (M19+M20)** → re-provision BLE (giá trị như lần cuối) → kỳ vọng chuỗi đầy đủ: log `broker URI had no scheme — normalized to mqtt://192.168.2.28:1883` → CONNECTED → BLE tắt 30 s → telemetry v2 `smarthome/boards/3b6baf6c/...` → orchestrator verify Influx + đóng M14c + QR label (lệnh `board-qr.sh -m 5c:01:3b:6b:af:6c` đã chạy OK).


### M21 (CHỜ DUYỆT 2026-09-23) — Thay Mosquitto bằng amqtt (Python asyncio MQTT broker)

Bối cảnh: user muốn triển khai **amqtt** (Yakifo/amqtt — fork của hbmqtt, pure Python asyncio, MQTT 3.1.1) thay cho Mosquitto làm broker.

Fact kiểm chứng từ docs chính thức amqtt (2026-09-23):
- MQTT 3.1.1 đầy đủ: QoS 0/1/2, retained, LWT — tương thích mqtt.js v5 backend (default 3.1.1) + esp-mqtt (3.1.1). KHÔNG hỗ trợ MQTT 5.0 (không ai trong stack dùng 5 → không vướng).
- Listeners: nhiều TCP + WebSocket trong cùng config YAML (`type: tcp` / `type: ws`) → thay trực tiếp 1883 (ESP32 + backend) + 9001 (app mobile WS).
- Docker image chính thức `amqtt/amqtt` trên DockerHub; mount config YAML vào `/app/conf/broker.yaml` (theo docs compose example).
- Auth: `FileAuthPlugin` đọc file `username:hash` mỗi dòng; từ v0.12 hash = **argon2** (sha512 deprecated do Python 3.13 bỏ `crypt`). KHÔNG include `AnonymousAuthPlugin` (cấm anonymous — parity `allow_anonymous false` hiện tại).
- Session persistence: `amqtt.contrib.persistence.SessionDBPlugin` — lưu session + retained messages qua restart (parity `persistence true` của mosquitto; chi tiết config đọc source khi làm).
- CLI kèm package: `amqtt_pub`/`amqtt_sub` (dùng được `--url mqtt://user:pass@host`) — thay `mosquitto_pub/sub` trong scripts; flags chi tiết (-C/-W...) coder xác minh lúc làm.
- Hiệu năng pure Python thấp hơn Mosquitto — với 2 board + 1 app + backend (vài msg/5 s) quá đủ.
- mDNS M17 + avahi KHÔNG đụng (quảng bá IP host + port, không phụ thuộc broker).

Việc mosquitto đang đảm nhiệm mà amqtt phải parity đủ (checklist tích hợp):
1. Listener TCP 1883 (ESP32/backend) + WS 9001 (app), auth bắt buộc 2 user (`esp32` + `app`).
2. Retained messages sống sót qua restart broker (SessionDBPlugin) — descriptor board + relay state + sensor state + status đều retained.
3. LWT → board offline khi mất kết nối đột ngột.
4. QoS 1 end-to-end.
5. Healthcheck container (hiện `nc -z`; image python → dùng python socket 1 dòng).
6. Scripts hiện exec `mosquitto_pub/sub` TRONG container mosquitto (`clear-legacy-retained.sh`, verify trong `server-init.sh`) → chuyển sang `amqtt_pub/sub` trong container hoặc mosquitto-clients từ host.
7. Passwd sinh lúc start từ `.env` (`mosquitto-setup.sh`) → script setup mới sinh file argon2 (parity: sinh random + in log khi `MQTT_APP_PASSWORD` trống).

Phân pha (chống đứt dịch vụ stack đang chạy tốt):

| task_key | Việc | File chính | Tiêu chí đạt | Lệnh kiểm tra |
|---|---|---|---|---|
| M21a:coder | PoC amqtt standalone port phụ (21883/29001, KHÔNG đụng compose đang chạy): Dockerfile hoặc image pin tag + `broker.yaml` (2 listener + FileAuthPlugin, không anonymous) + script sinh password file argon2 từ env + test parity bằng mosquitto clients host (retained round-trip, QoS 1, LWT, auth sai bị chối, WS handshake) | `amqtt/**` (mới), `scripts/amqtt-setup.sh` (mới), PoC notes | PoC chạy trên port phụ; 5 checklist đầu pass qua mosquitto_pub/sub host; auth sai bị từ chối; retained sống sót restart container PoC | `docker build` + kịch bản mosquitto_pub/sub + restart container PoC |
| M21b:coder | Swap service trong compose: `mosquitto` → `amqtt` (giữ port 1883/9001), volume riêng (mosquitto-data không tái dùng), healthcheck python, cập nhật `.env.example` + scripts liên quan + README + `docs/frontend-setup.md`; giữ file mosquitto/ trong repo (rollback) | `docker-compose.yml`, `amqtt/**`, `scripts/clear-legacy-retained.sh`, `scripts/server-init.sh`, `.env.example`, `README.md`, `docs/frontend-setup.md` | `docker compose config` OK; 3 container healthy; backend connect + subscribe đủ filter hiện tại; mosquitto_pub/sub từ host round-trip qua 1883 + 9001 (ws) | `docker compose up -d --build` + mosquitto clients host |
| M21c | Integration end-to-end (orchestrator): board 3b6baf6c reconnect tự publish lại descriptor retained; restart broker → retained + session sống; backend Influx ghi tiếp; app WS 9001 handshake; auth sai bị chối; LWT offline khi kill connection | — | checklist trên pass | — |

**M21a:coder DONE — orchestrator verified 2026-09-24** (session `ses_f309cfa0fffe0eFLkWV1815N84`): cả hai lệnh kiểm tra do orchestrator TỰ CHẠY LẠI — round-trip QoS 1 + retained qua TCP 21883 (publish `orchestrator-check` → sub đọc lại đúng, exit 0) + WS handshake 29001 trả `HTTP/1.1 101 Switching Protocols` (curl timeout sau upgrade là hành vi đúng — broker chờ gói MQTT CONNECT). Footprint git đúng: chỉ `?? amqtt/`, `?? scripts/amqtt-passwd.py`, `?? scripts/amqtt-setup.sh` — không modified file cũ. Quyết định coder: dùng image chính thức `amqtt/amqtt:0.12.1` (DockerHub có tag, pushed 2026-09-16) + Dockerfile mỏng cài thêm deps `SessionDBPlugin` (sqlalchemy[asyncio] + aiosqlite + greenlet — image gốc không bundle). Persistence xác nhận: restart container → log `Retained messages restored: 3`. Config 0.12.x (đọc source trong image, không đoán): key plugin snake_case (`password_file`, `clear_on_shutdown`), listener bắt buộc tên `default`, anonymous → `FileAuthPlugin.authenticate` trả None → từ chối. WS bắt buộc header `Sec-WebSocket-Protocol: mqtt` (thiếu → 400). Lưu ý vận hành: broker chỉ graceful-shutdown trên SIGINT, `docker restart` (SIGTERM) = chết đột ngột — M21b cân nhắc signal handling + healthcheck python. Container PoC để chạy cho verify; dọn: `docker compose -f amqtt/docker-compose.poc.yml down -v`.

**M21b:coder DONE — orchestrator verified 2026-09-24** (session `ses_f2e5afd50ffenOxWpQ4IQ2qeOx`): `docker compose config --quiet` OK; `mosquitto/` + `scripts/mosquitto-setup.sh` + PoC `broker-poc.yaml`/`docker-compose.poc.yml` đã xóa; `grep mosquitto` chỉ còn trong PLAN.md (nhật ký). Service `amqtt` port 1883+9001, `stop_signal: SIGINT` (wrapper trap TERM→INT không dùng được: dash `&` để SIGINT=SIG_IGN cho con, Python không cài KeyboardInterrupt — coder đã chứng minh). Healthcheck gửi CONNECT MQTT thật (raw socket spam ERROR log). Orchestrator sửa `.env` `MQTT_URL=mqtt://amqtt:1883`, `docker compose up -d --build`: amqtt healthy, influx healthy, backend log `connected tới mqtt://amqtt:1883` + subscribe đủ filter. Round-trip `amqtt_pub`/`amqtt_sub` trong container đọc lại `m21b-ok`; WS 9001 `HTTP/1.1 101` (cần `Sec-WebSocket-Protocol: mqtt`). Đã `docker rm` container test `poc-lwt-*` và `docker rmi eclipse-mosquitto:2.0.20`. Volume `mosquitto-data` không còn. Volume PoC `amqtt-poc_amqtt-poc-data` còn (dọn tùy chọn).

Câu hỏi mở (trả lời khi duyệt — mặc định theo phương án ghi):
1. **Lý do đổi amqtt?** (a) muốn customize broker logic bằng Python (plugin riêng — auth/ACL/topic logic), (b) học tập/thử nghiệm, (c) chỉ thay thế tính năng tương đương. Với (a): M21a thêm việc dựng 1 plugin mẫu chứng minh đường customize; (b)/(c): làm minimal đúng bảng trên. (default: hỏi lại user — user duyệt "ok" không chọn, triển khai theo **(c) minimal**; muốn (a) thì thêm milestone riêng sau)
2. **Tên service compose**: đổi `mosquitto` → `amqtt` (đúng semantics, phải đổi `MQTT_URL=mqtt://amqtt:1883` + `.env`/docs theo) hay giữ tên `mosquitto` (zero thay đổi `.env`/docs, tên sai sự thật)? (default: đổi tên cho đúng — sửa cùng lúc trong M21b)
3. **Rollback**: **ĐÃ CHỐT 2026-09-24 — user "thay thế toàn bộ mosquitto và xóa sạch"** — M21b XÓA `mosquitto/**` + `scripts/mosquitto-setup.sh` + volume `mosquitto-data`, không giữ rollback trong repo. Lịch sử PLAN.md (milestone cũ nhắc mosquitto) GIỮ NGUYÊN — đó là nhật ký, không phải hướng dẫn vận hành.
4. Version amqtt: pin mới nhất ổn định 0.12.x (Python 3.10–3.14) — nếu image DockerHub không có tag 0.12.x thì tự Dockerfile `python:3.12-slim` + `pip install amqtt==<exact>` + `argon2-cffi`. (default: coder quyết theo khảo sát thực tế DockerHub, ghi rõ trong báo cáo)


### M17 (ĐÃ DUYỆT 2026-09-19 — user "vậy triển khai bên backend") — Server mDNS discovery (avahi)

Bối cảnh: server = laptop demo nối WiFi (DHCP, IP đã đổi ít nhất 1 lần: `192.168.2.34` → `192.168.100.3`); app + board + server cùng LAN. Bài toán: app không phụ thuộc IP gõ tay — server tự quảng bá qua mDNS/DNS-SD, app dò tìm (task frontend riêng, handoff dưới). Là nền cho M16: app biết broker → BLE đẩy xuống board.

Fact máy (orchestrator verify 2026-09-19): `avahi-daemon` active (systemd); `avahi-utils` (avahi-browse…) **CHƯA cài** → script phải xử lý thiếu/thiếu gợi ý cài; `sudo` cần password → **user chạy script tương tác, orchestrator KHÔNG chạy được** (chỉ verify phi-sudo); host nhiều interface (WiFi `192.168.100.3` + docker `172.x` + tailscale `100.x`) — avahi quảng bá hết, app side lọc (ghi rõ handoff).

Thiết kế chốt:
- 1 file avahi service duy nhất: type `_mqtt._tcp`, port `1883`, TXT records `ws-port=9001` + `influx-port=8086` + `prefix=smarthome`. App browse 1 dịch vụ lấy trọn bộ địa chỉ. Avahi tự gắn IP host vào quảng bá — IP đổi tự bám theo.
- `scripts/server-init.sh` idempotent (user chạy, được prompt sudo): check avahi-daemon → check `.env` → `docker compose config --quiet` → `sudo cp` service file vào `/etc/avahi/services/` (avahi tự nhận file mới, không restart) → `docker compose up -d --build` → đợi container healthy → verify quảng bá (`avahi-browse -rt _mqtt._tcp` nếu có utils, thiếu thì WARNING + gợi ý `sudo apt-get install -y avahi-utils`, không fail) → tóm tắt địa chỉ + 3 port + hint firewall (ufw cho 1883/9001/8086 nếu bật) + lưu ý cùng WiFi + laptop không sleep.
- Secrets KHÔNG qua mDNS: MQTT pass + Influx token vẫn nhập tay trong app lần đầu (mDNS chỉ mang địa chỉ).
- Giới hạn ghi README: mDNS chết trên WiFi có AP isolation (quán/văn phòng) → fallback nhập tay (app đã có); ngoài LAN không tìm thấy (remote = Tailscale hoãn M12); laptop sleep = server mất.
- mDNS thay QR server (đã bỏ khỏi M16 B+).

| task_key | Việc | File chính | Tiêu chí đạt | Lệnh kiểm tra |
|---|---|---|---|---|
| M17:coder | Avahi service template + script khởi tạo + README mục "Khởi tạo server" | `avahi/smarthome.service` (mới), `scripts/server-init.sh` (mới), `README.md` | `bash -n` sạch; XML well-formed; `docker compose config --quiet` OK (compose không đổi); `git status` chỉ 3 file; KHÔNG secret trong file | `bash -n scripts/server-init.sh` + `python3 -c "import xml.dom.minidom; xml.dom.minidom.parse('avahi/smarthome.service')"` + `docker compose config --quiet` |

- **M17:coder DONE — orchestrator verified 2026-09-19** (session `ses_f46a757f4ffe4cNaj3WEgAaXpl`): cả 3 lệnh kiểm tra PASS (`bash -n` exit 0; XML well-formed; `docker compose config --quiet` OK); `git status` footprint đúng 3 file (README thêm mục `### Khởi tạo server (mDNS discovery)` +31 dòng trong mục 6 — không đụng mục khác). Script 169 dòng đủ 8 bước, idempotent, không in secret, `detect_lan_ip()` lọc đúng dải docker `172.16/12` + tailscale CGNAT `100.64/10` + IPv6 (coder đã test cô lập trên host 15 địa chỉ → chọn đúng `192.168.100.3`). XML: `_mqtt._tcp` :1883 + TXT `ws-port=9001`, `influx-port=8086`, `prefix=smarthome` — đúng contract handoff frontend.
- **M17 integration — orchestrator PASS toàn bộ 2026-09-19**: user chạy script (bước 1–4 xong: service file vào `/etc/avahi/services/`) nhưng compose up chưa hoàn thành (không container nào) → orchestrator chạy `docker compose up -d --build` (không cần sudo). Kết quả: (1) quảng bá mDNS live — `avahi-browse -rt _mqtt._tcp` thấy `Smart Home Server` hostname `lucas-Dell-G15-5525.local` port 1883 + đủ 3 TXT record, có địa chỉ WiFi `192.168.100.3` (kèm interface phụ docker `172.17.0.1`/loopback `127.0.0.1`/IPv6 — đúng cảnh báo đa IP trong README, app lọc); (2) 3 container mosquitto healthy + influxdb healthy + backend up, log backend đã subscribe đủ đường (boards descriptor/telemetry + bridge + relays), registry nạp lại descriptor board "0" từ retained (LEGACY_BOARDS còn `0:A`); (3) MQTT round-trip qua ĐÚNG IP mDNS quảng bá `192.168.100.3:1883` với auth `.env` (publish retained `hello-mdns` → sub đọc lại đúng → dọn retained) PASS. **M17 phần server: ĐÓNG.**
- **Còn lại sau M17**: (1) task frontend "Tìm máy chủ trong mạng" (brief handoff trong mục này — giao khi user sẵn sàng); (2) app thật trên điện thoại verify discovery từ thiết bị khác (cần task frontend xong); (3) quay lại M16 BLE provisioning (CHỜ DUYỆT — phần QR server của B+ đã bỏ, mDNS thay).

Sau M17:coder — orchestrator verify (lệnh bảng trên + đọc README) → **USER chạy** `bash scripts/server-init.sh` (sudo tương tác) → orchestrator verify quảng bá `avahi-browse -r _mqtt._tcp` (đã cài avahi-utils lúc đó) + `mosquitto_pub` test tới IP resolve → xong M17 phần server. Reviewer/tester không dispatch mặc định (thuần infra tooling + doc, rủi ro thấp) — user muốn thì thêm.

Handoff brief cho Mobile_Frontend (task Settings discovery — dán session orchestrator repo đó):

```
Việc: Settings thêm nút "Tìm máy chủ trong mạng" (mDNS discovery).

Nguồn: Mobile_Backend M17 — server quảng bá avahi service _mqtt._tcp port 1883
+ TXT records: ws-port=9001, influx-port=8086, prefix=smarthome.

File chính: app-mobile/src/modules/settings/ (+ test)

Hành vi:
1. Dep react-native-zeroconf (native rebuild — đã cần cho expo-camera/ble-plx).
2. Nút trong Settings (gần trường host): browse _mqtt._tcp ~5-10 s → parse TXT
   (ws-port, influx-port, prefix) → prefill: mqtt host + port ws-port; influx
   host + port influx-port; prefix (nếu user chưa đặt). Pass MQTT + token
   Influx KHÔNG có trong mDNS — giữ nhập tay.
3. Chọn địa chỉ khi service resolve nhiều IP (laptop có docker 172.x +
   tailscale 100.x): loại loopback/link-local/docker/tailscale (127/8,
   169.254/16, 172.16/12, 100.64/10); còn nhiều → ưu tiên cùng subnet với
   thiết bị, hoặc hiện danh sách chọn; KHÔNG đoán mù IP cuối cùng.
4. Không tìm thấy (AP isolation / web app) → thông báo + giữ nhập tay (fallback
   hiện trạng không đổi). Web Platform.OS gate nút.
5. Test: mock zeroconf — resolve 1 IP duy nhất → prefill đúng; nhiều IP → lọc
   đúng; timeout → thông báo; test hiện có (host nhập tay) pass nguyên.

Verify: cd app-mobile && npm run typecheck && npm test
```


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

### M13 (ĐÃ DUYỆT 2026-09-14) — Contract board-centric theo pha

Thiết kế mục tiêu (chốt hướng theo review 2026-09-14):
- App contract `{prefix}/boards/{boardId}/...`: `descriptor` (retained), `sensors/{S}/state`, `relays/{K}/state`, `relays/{K}/set`, `status`. Phòng là khái niệm thuần app (một phòng nhiều board), mapping local + xuất/nhập cấu hình để backup (phía app).
- **boardId ổn định** — giữ "0", KHÔNG đổi sang "A-0011" chỉ để tên đẹp; tên hiển thị qua `displayName` (optional) trong descriptor. Nếu sau này đổi định dạng ID phải có bảng ánh xạ ID cũ→mới xác thực, không coi reflash là đủ.
- **Mỗi board đúng một nguồn descriptor**: board mới do firmware tự publish; board cũ (firmware v1) do bridge mô tả theo **danh sách khai báo** (env `LEGACY_BOARDS`), bridge KHÔNG tự suy descriptor từ mọi status retained gặp được — retained status không chứng minh cấu hình phần cứng. Dọn retained chỉ nhắm board xác nhận ngừng dùng (không phải board offline).
- **Telemetry đích: schemaVersion 2 theo kênh** — `{"schemaVersion":2,"boardId":"0","values":{"S1":28.5,"S2":71}}`, descriptor khai báo ý nghĩa từng kênh (S1=temperature, S2=humidity, S3=…). Bridge/backend map theo descriptor (data-driven), không nhánh if theo loại cảm biến. Chức năng phi-số (camera, dimmer) vẫn hỗ trợ riêng. → firmware M14.
- **Lịch sử**: Influx giữ field tên ngữ nghĩa (temperature/humidity/…), ánh xạ kênh→field qua descriptor lúc ghi → dữ liệu cũ của board "0" tương thích ngay, KHÔNG backfill; KHÔNG bao giờ mặc định roomId cũ = boardId khi backfill về sau — chỉ chuyển khi có bảng ánh xạ + khoảng thời gian xác thực, dữ liệu không xác định giữ legacy. Tab Lịch sử query theo tag `boardId` (writer M13 thêm tag cạnh roomId).
- **Chuyển contract**: dual-publish room + boards CHỈ trong giai đoạn hỗ trợ app cũ, có mốc ngừng + danh sách retained phải dọn (các topic `{prefix}/room/...` — publish retained rỗng để xóa).
- **Lệnh không hợp lệ**: bản tối thiểu — app coi "không có stat xác nhận trong timeout" là lỗi; error topic riêng để sau (M14+). Firmware hiện đã từ chối id lạ an toàn (log + bỏ, không crash — M9b).
- App chỉ hiển thị kênh được khai báo trong descriptor; board offline hiển thị giá trị cũ (retained) — đúng hiện trạng.

Phân pha (đã duyệt):
- **M13 — backend, firmware không đụng**: bridge translate contract v1 → board-centric cho board trong `LEGACY_BOARDS=0:A` (boardId:boardType; template loại A hardcode S1/S2 + K1–K3 — translation layer có phạm vi rõ); dual-publish; Influx writer thêm tag `boardId`; dọn retained của identity cũ đã xác nhận bỏ (`esp32-01`). Board "0" flash binary URI-sạch (đã build) là chạy với app mới ngay.
- **M14 — firmware + ingest v2 (khi thêm board/cảm biến mới hoặc tiện reflash)**: firmware publish descriptor + `sensors/{S}/state` trực tiếp (telemetry v2 theo kênh); backend ingest đường mới + map field qua descriptor; gỡ `LEGACY_BOARDS` khi board cuối lên v2; cân nhắc error topic cho lệnh.

| task_key | Việc | File chính | Tiêu chí đạt |
|---|---|---|---|
| M13:coder | Bridge mapping board-centric (env `LEGACY_BOARDS=0:A` + template loại A) + dual-publish + unit test; writer thêm tag `boardId`; `.env.example` thêm `LEGACY_BOARDS`; script dọn retained `esp32-01` | `src/bridge/**`, `src/influx/**`, `src/env.ts`, `src/main.ts`, `.env.example`, `scripts/**`, `README.md` | typecheck + test xanh; publish telemetry giả board "0" → `mosquitto_sub` thấy `{prefix}/boards/0/{descriptor,sensors/S1/state,sensors/S2/state,status}` + relays/K*/state đúng shape; lệnh `boards/0/relays/K1/set` ON → `smarthome/0/relay/K1/set`; query Flux theo tag boardId trả point; topic `smarthome/room/esp32-01/...` hết retained |

Quyết định duyệt 2026-09-14 ("ok triển khai" — theo đề xuất, bổ sung boardType):
1. Phân pha: M13 backend trước, M14 firmware v2 sau — CHỐT.
2. Influx field giữ tên ngữ nghĩa (temperature/humidity), ánh xạ kênh→field qua descriptor; writer thêm tag `boardId` (v1: deviceId ≡ boardId, tag = payload.deviceId cho mọi điểm) — CHỐT.
3. `LEGACY_BOARDS=0:A` (một board, loại A); `esp32-01` là identity cũ đã bỏ → dọn retained — CHỐT.
4. Lệnh lỗi = tối thiểu: app timeout chờ stat xác nhận; error topic để M14+ — CHỐT.
5. `displayName`: BỎ ở M13, thêm ở M14 cùng firmware (descriptor M13 không có trường này) — CHỐT.
6. Bổ sung theo ý user "nhiều mẫu board cùng loại A, số id khác": descriptor có `boardType` (vd `"A"`) — cấu hình phần cứng định nghĩa theo LOẠI, boardId theo TỪNG CON. Type KHÔNG vào đường dẫn topic. Env format `boardId:boardType` phân tách phẩy (vd `0:A,5:A`) — CHỐT.
7. Shape status contract boards: payload plain `online`/`offline` retained (giữ nguyên như firmware v1) — cần đối chiếu Mobile_Frontend (rủi ro 2026-09-13); nếu app cần JSON thì sửa một chỗ mapper, không đổi topic.

### M14 (ĐÃ DUYỆT 2026-09-15 — user "ok", 3 câu hỏi mở chốt theo mặc định) — Firmware v2 board-centric + ingest đường mới

Quyết định duyệt 2026-09-15:
1. Tag `roomId` cho điểm Influx v2: **roomId = boardId** — history app cũ liên tục khi quy ước 1:1 còn hiệu lực; bỏ khi app hết query theo roomId.
2. Error topic lệnh relay: **bỏ qua ở M14** — giữ "app timeout chờ stat" (quyết định 4 M13).
3. Thứ tự: **M14a backend → M14b firmware → user flash (M14c)**.

Pha đã duyệt từ M13 (quyết định 1, 2026-09-14). Thiết kế chi tiết:

**Firmware v2 nói chuyện thẳng contract boards** (prefix qua Kconfig mới `MQTT_TOPIC_PREFIX`, default `smarthome`; `DEVICE_ID` hiện có giữ nguyên, giá trị dùng làm boardId theo quy ước 1:1):
- `{prefix}/boards/{boardId}/descriptor` — retained, publish mỗi lần connect. JSON schemaVersion 1 + **`displayName`** (Kconfig `BOARD_DISPLAY_NAME` optional, bỏ field nếu để trống — quyết định 5 M13). Nội dung descriptor = bảng kê phần cứng của chính board trong code firmware (S1=temperature °C, S2=humidity %, K1–K3), mirror template A của bridge.
- `{prefix}/boards/{boardId}/telemetry` — payload v2 `{"schemaVersion":2,"boardId":"0","values":{"S1":28.5,"S2":71}}`, QoS 1, **KHÔNG retained** (đây là đường ingest mới cho backend; không dùng `sensors/{S}/state` để ingest vì retained replay khi backend restart sẽ ghi điểm trùng).
- `{prefix}/boards/{boardId}/sensors/{S}/state` — số thuần, retained (app realtime).
- `{prefix}/boards/{boardId}/status` — plain `online`/`offline` retained; **LWT chuyển sang topic này**.
- Relay: subscribe `{prefix}/boards/{boardId}/relays/+/set` (payload `ON`/`OFF` thuần, parse kênh từ topic) → `relay_set()` → publish `{prefix}/boards/{boardId}/relays/{K}/state` retained sau mỗi lệnh + khi connect.
- **Bỏ hẳn topic v1 trong firmware v2** (LWT cũ `smarthome/{id}/status`, telemetry v1, `relay/state` JSON gộp, subscribe per-channel v1) — board v1 nào cần contract v1 thì giữ firmware cũ; bridge đã lo phần dịch.

**Backend ingest v2** (M14a — chạy trước, deploy được khi chưa có board v2 nào, không regression):
- Schema Zod v2 trong `src/telemetry/`: `schemaVersion: 2`, `boardId` (regex `[a-zA-Z0-9_-]+`), `values: Record<string, number finite>` (bỏ range check vật lý — kênh generic, descriptor không mang khoảng; điểm khác biệt so v1 ghi rõ trong code).
- Module mới (đặt `src/boards/` hoặc tương đương — coder chọn chỗ hợp phong cách): **descriptor registry** — subscribe `{prefix}/boards/+/descriptor` trên connection mqtt-service, validate Zod (schemaVersion 1, boardId, boardType, sensors[{channel,field,unit}], relays[{channel}], displayName optional string), cache boardId→descriptor theo message retained mới nhất.
- Routing theo topic trong mqtt-service: `smarthome/+/telemetry` → đường v1 (giữ nguyên hoàn toàn); `{prefix}/boards/+/telemetry` → đường v2: validate v2 + đối chiếu boardId topic↔payload → lookup descriptor trong registry → **map `values` → field theo `descriptor.sensors` (data-driven, không nhánh if theo loại cảm biến)** → writer.
- Kênh lạ (không có trong descriptor) → WARN + bỏ kênh; hết kênh hợp lệ → bỏ point + WARN; descriptor chưa có (cold-start race) → WARN + bỏ (chu kỳ 5 s kế tiếp tự ổn, không queue).
- **WARN misconfig**: v2 telemetry từ board vẫn nằm trong `LEGACY_BOARDS` → WARN rõ "2 nguồn descriptor cho cùng board — bỏ khỏi LEGACY_BOARDS" (vẫn ingest point).
- Writer tổng quát hóa: `fields` thành `Record<string, number>` (không cứng temperature/humidity); điểm v2 tags `{boardId, roomId: boardId}` (xem câu hỏi mở 1); đường v1 giữ nguyên tags từ payload.
- Bridge **KHÔNG đụng** (dual-publish room + descriptor LEGACY cho board v1 giữ nguyên).

**Chuyển tiếp board "0"** (M14c — SAU khi app Mobile_Frontend đã dùng boards contract):
1. User flash firmware v2 (`idf.py flash monitor`).
2. `.env`: `LEGACY_BOARDS=` (rỗng) → restart backend → bridge ngừng publish descriptor + ngừng forward lệnh cho board 0 (firmware v2 tự subscribe trực tiếp).
3. `bash scripts/clear-legacy-retained.sh 0` — dọn retained v1 của board 0 (LWT status cũ, relay/state JSON cũ, `smarthome/room/0/...`).
4. Orchestrator verify end-to-end: descriptor từ firmware (có displayName nếu đặt), telemetry v2 → Influx point tag boardId + field ngữ nghĩa, lệnh relay boards → state, status online/LWT.
- Lưu ý: contract room cũ **mất board 0** từ lúc reflash (payload v2 không có roomId) — thứ tự bắt buộc: app lên boards contract TRƯỚC, reflash SAU.

**sdkconfig** (pattern M9a): Kconfig mới thêm defaults vào `sdkconfig.defaults`; coder sửa `sdkconfig` thật trực tiếp (backup `/tmp/opencode/` trước, diff credentials sau — sdkconfig gitignored chứa Wi-Fi/MQTT thật).

**Không làm ở M14** (mặc định): error topic cho lệnh relay — giữ cơ chế "app timeout chờ stat" (quyết định 4 M13); cân nhắc lại sau khi app cần.

| task_key | Việc | File chính | Tiêu chí đạt | Lệnh kiểm tra |
|---|---|---|---|---|
| M14a:coder | Backend ingest v2: schema v2 + descriptor registry + routing + writer tổng quát + unit test + README mục ingest | `src/telemetry/**`, `src/mqtt/**`, `src/influx/**`, module registry mới, `src/main.ts`, `README.md` | typecheck + unit test xanh; v1 path không đổi (test cũ pass) | `npm run typecheck && npm test` |
| M14b:coder | Firmware v2: Kconfig (prefix + displayName) + mqtt_app boards contract (descriptor/telemetry v2/sensor state/status LWT/relay per-channel) + sdkconfig + README firmware | `firmware/esp32-telemetry/main/**`, `firmware/esp32-telemetry/components/mqtt_app/**`, `sdkconfig.defaults`, `README.md` | `idf.py build` xanh; descriptor đúng shape M13+displayName; không còn topic v1 trong firmware | `. ~/.espressif/tools/activate_idf_v6.0.1.sh && cd firmware/esp32-telemetry && idf.py build` |
| M14c | Chuyển tiếp board 0 (user flash + orchestrator: `.env`, dọn retained, integration test) | — | checklist M14c pass | — |

Thứ tự: M14a → M14b tuần tự (một coder); M14c chờ user flash khi app sẵn sàng.

Câu hỏi mở (trả lời khi duyệt — mặc định sẽ theo phương án ghi):
1. Tag `roomId` cho điểm Influx v2: **mặc định roomId = boardId** (quy ước 1:1 còn hiệu lực — history app cũ liên tục, schema tag đồng nhất mọi điểm; bỏ khi app hết query theo roomId) — hay bỏ hẳn roomId cho điểm v2 (sạch về mặt mô hình, app cũ mất history board 0 từ lúc reflash)?
2. Error topic lệnh relay: **mặc định bỏ qua ở M14** (timeout chờ stat đủ dùng) — hay làm luôn?
3. Thứ tự M14a (backend) → M14b (firmware) → user flash — OK?

### M22 (ĐÃ DUYỆT 2026-09-25 — user "ok chạy m22 đi") — NVS stale thì về BLE, không retry nền

User 2026-09-25: "sai cơ chế rồi nếu đã lưu nvs nếu check không có thì quay về ble đợi. nvs chỉ check nhanh thôi, ở đây nó bỏ qua."

**Log chứng minh (board `3b6baf6c`, SSID NVS `"Phòng toàn trai đẹp"`):**

- `wifi_conn: disconnected (reason=201)` lặp attempt 1→6, backoff tới 30 s. Reason 201 = `WIFI_REASON_NO_AP_FOUND` — SSID đã lưu không còn trên không khí (đổi mạng / AP tắt), không phải sai mật khẩu.
- Sau 60 s: `Wi-Fi not connected after 60 s; Wi-Fi and MQTT keep retrying in the background` rồi `mqtt_app initialized … broker=192.168.2.28:1883` và `connect() error: Host is unreachable`. Broker trong NVS là IP mạng cũ; laptop hiện tại là `192.168.100.3`.
- Không có dòng `BLE provisioning mode`. Nhánh 4a (`main.c`, `ble_prov_load == ESP_OK`) return sớm, BLE tắt. Nút BOOT ≥ 5 s là đường recovery duy nhất — user không muốn phải bấm.

**Cơ chế đã duyệt M16 (2026-09-19) bị thay:** "không BLE-cứu khi WiFi đã lưu sai — recovery qua nút BOOT". M22 đảo quyết định đó.

**Chốt lại 2026-09-26 (M23, user yêu cầu sửa ba lệch):** reason 202 / `BAD_AUTH` không xóa NVS và không reboot — giữ credential, dừng retry, BOOT ≥ 5 s là đường ra. Reason 200/201 fail ngay rồi xóa + reboot, nhưng latch “ngừng retry” chỉ bật ở nhánh NVS; nhánh Kconfig vẫn backoff. Trước `esp_restart()` vì broker fail phải stop + destroy client MQTT. Chi tiết giao `M23:coder`.

**Hành vi mới (NVS path only; Kconfig path giữ retry nền như cũ):**

1. Boot: `ble_prov_load` như hiện tại. Có đủ 5 key → coi là ứng viên, không phải "đã xong".
2. Check nhanh Wi-Fi: `wifi_conn_start`, chờ `WIFI_CONNECTED_BIT` tối đa **15 s** (không 60 s). Trong cửa sổ đó, reason `201` (`NO_AP_FOUND`) hoặc `200` (`BEACON_TIMEOUT`) → fail nhanh, không backoff 30 s.
3. Có IP: thử MQTT connect tới broker đã lưu, tối đa **10 s**.
4. Một trong hai fail (không thấy AP, hoặc có IP nhưng broker unreachable / connack timeout) → `ble_prov_erase()` (chỉ namespace `bleprov`, không đụng bond NimBLE nếu erase API hiện chỉ xóa 5 key — đúng API đó), log một dòng `stale NVS — erased, BLE provisioning`, `esp_restart()`.
5. Boot sau: NVS trống + `CONFIG_WIFI_SSID` rỗng → nhánh 4c BLE, advertise `IoTBoard-{boardId}`, đứng chờ app. Không vòng xóa-reboot nếu lần BLE chưa từng ghi NVS (erase chỉ chạy khi `ble_prov_load == ESP_OK`).
6. Cả hai check pass → chạy telemetry như 4a hiện tại. BOOT ≥ 5 s giữ nguyên.

**Không làm:** không sửa Avahi, không sửa app, không đổi contract GATT, không đụng nhánh Kconfig (SSID bake vào image vẫn retry nền).

| task_key | Việc | File được sửa | Tiêu chí đạt | Lệnh kiểm tra |
|---|---|---|---|---|
| M22:coder | NVS path: check Wi-Fi 15 s (fail-fast 200/201) + MQTT 10 s; fail → erase `bleprov` + restart vào BLE | `firmware/esp32-telemetry/main/main.c`, `firmware/esp32-telemetry/components/wifi_conn/wifi_conn.c` (+ header nếu cần cờ fail-fast), README mục 10 (một đoạn thay "BLE không bật lại khi credential sai") | build 0 warning mới; không còn "keep retrying in the background" trên nhánh NVS; Kconfig path không đổi; erase chỉ khi load NVS thành công | `. ~/.espressif/tools/activate_idf_v6.0.1.sh && cd firmware/esp32-telemetry && idf.py build` |

Sau coder: orchestrator tự chạy lại `idf.py build`. Reviewer/tester không dispatch mặc định (một nhánh boot, user flash xác nhận). User flash + bỏ BOOT; kỳ vọng log: `stale NVS — erased` rồi reboot, sau đó `BLE provisioning mode` / `IoTBoard-3b6baf6c`. App đẩy SSID + `mqtt://192.168.100.3:1883`.

### M24 (DONE 2026-09-26) — Menu bootstrap/vận hành cho server mới tinh

#### Mục tiêu và phạm vi

Thiết bị server mới có thể bắt đầu từ số 0: sau khi người vận hành đã flash OS, bật SSH, cài Docker Engine + Compose v2, git, avahi-daemon và qrencode, họ clone repo rồi chạy **một entrypoint duy nhất** thay vì nhớ chuỗi lệnh rời rạc. M24 chỉ thêm lớp điều phối host; không đổi contract MQTT, firmware, backend TypeScript hay schema InfluxDB.

**Ranh giới quan trọng:** `scripts/smarthome.sh` nằm trong repo nên không thể chạy trước khi repo được clone. Bootstrap "trước khi có code" là lớp cài đặt của OS, không phải logic trong container:

1. Flash OS + bật SSH và có mạng.
2. Trên host mới, dùng package manager/installer của distro để cài `git`, Docker Engine + Compose v2, `openssl`, `avahi-daemon`, `avahi-utils` và `qrencode` (không chạy Docker để cài chính Docker; không cần repo cho bước này).
3. Dùng `git clone`/SSH key hoặc HTTPS token để lấy repo, rồi `git checkout` tag/commit đã chọn.
4. Từ thư mục repo mới chạy `scripts/smarthome.sh init`; lúc này mới có `.env.example`, Compose file và các script wrapper. `.env` được sinh từ repo, sau đó Docker mới pull/build image rồi start stack.

Git và Docker vì vậy đều được cài trên host nhưng có vai trò khác nhau: Git lấy đúng source/config; Docker chạy build/runtime. Không thể `docker compose pull` hoặc `docker compose build` backend trước khi có `docker-compose.yml`/`Dockerfile` từ repo. Nếu yêu cầu một lệnh duy nhất ngay sau khi flash OS, cần thêm một artifact ngoài repo (cloud-init, image provisioning, Ansible hoặc installer được host độc lập) để cài dependency và clone revision; đó là phạm vi bootstrap riêng, không phải `smarthome.sh` trong M24.

File logic mới duy nhất trong phần triển khai là `scripts/smarthome.sh`; đồng thời phải cập nhật `README.md` để người vận hành biết cách đi từ OS trắng đến stack chạy và cách dùng menu. Script phải gọi lại logic đã có, không chép lại nội dung của:

- `scripts/server-init.sh` — avahi + validate Compose + compose up + đợi service sẵn sàng + verify mDNS;
- `scripts/credentials-qr.sh` — QR credentials cho app;
- `scripts/board-qr.sh` — QR board, có thể truyền `--board-id` hoặc `-m MAC`;
- `scripts/pairing-code.sh` — khối mã ghép nối.

`server-init.sh` vẫn là nguồn sự thật cho phần khởi động stack và health wait. `smarthome.sh` không tự cài package hệ điều hành, không tự ghi `/etc`, không in secret ngoài các lệnh QR vốn đã có chủ đích in secret.

#### Hành vi bắt buộc

1. **Tự tìm repo root và chạy ở đó** để script hoạt động dù người vận hành gọi từ thư mục khác. Dùng Bash strict mode; kiểm tra các script phụ trợ tồn tại trước khi dispatch.
2. **Sinh `.env` an toàn khi init lần đầu:**
   - Nếu `.env` đã tồn tại thì không ghi đè và không sinh lại secret.
   - Nếu thiếu, tạo từ `.env.example` bằng file tạm rồi rename atomic; đặt permission `600`.
   - Dùng `openssl rand -hex` để thay các placeholder secret trong bản mẫu bằng giá trị random: `MQTT_PASSWORD`, `MQTT_APP_PASSWORD`, `INFLUXDB_INIT_PASSWORD` và một token admin dùng đồng nhất cho `INFLUXDB_INIT_ADMIN_TOKEN` + `INFLUX_TOKEN`. Giữ nguyên các giá trị không-secret (`MQTT_USER`, org, bucket, URL service name, prefix).
   - Không ghi secret vào log, argv, file tạm còn sót hoặc output của menu. Không tự sinh `INFLUX_APP_TOKEN` read-only vì token đó cần policy/quyền riêng; QR credentials vẫn xử lý trường này theo script hiện tại.
   - Nếu thiếu `openssl`, `.env.example` hoặc không thể tạo `.env`, dừng với thông báo cài/khắc phục rõ ràng; không gọi `server-init.sh` với cấu hình nửa vời.
3. **Tải/build trước, khởi động sau:** sau khi `.env` có mặt, validate `docker compose config --quiet`, tải image-only services và build các service có Dockerfile ở trạng thái chưa chạy (`docker compose pull --ignore-buildable` + `docker compose build --pull`, hoặc cách tương đương tương thích với Compose v2 đang yêu cầu). Nếu pull/build/verify lỗi thì dừng; chỉ khi bước này thành công mới gọi `bash scripts/server-init.sh`. Lần gọi `server-init.sh` có thể dùng cache đã build; không được chạy stack trước bước preload/verify.
4. **Menu không đối số:**
   ```text
   1) Init server mới
   2) QR credentials
   3) QR board
   4) Khối mã ghép nối
   5) Trạng thái
   0) Thoát
   ```
   Input sai phải báo lỗi và quay lại menu, không thoát đột ngột. EOF/Ctrl-C phải kết thúc sạch.
5. **Gọi trực tiếp có đối số:** hỗ trợ tên lệnh dễ nhớ và alias số tương ứng (`init`/`1`, `credentials-qr`/`2`, `board-qr`/`3`, `pairing-code`/`4`, `status`/`5`, `0`/`exit`). Tham số sau `board-qr` phải được truyền nguyên vẹn tới `board-qr.sh`, ví dụ `bash scripts/smarthome.sh board-qr --board-id 3b6baf6c` hoặc `bash scripts/smarthome.sh 3 -m 5c:01:3b:6b:af:6c`; không dùng `eval`.
6. **QR board trong menu:** vì server nhúng không có ESP-IDF/esptool, option 3 phải hỏi `boardId` hoặc MAC bằng `read` rồi gọi `board-qr.sh --board-id ...` / `board-qr.sh -m ...`; không mặc định cố đọc serial. Chế độ đối số vẫn cho phép truyền mọi option hợp lệ của `board-qr.sh`.
7. **Trạng thái:** chạy từ repo root `docker compose ps` và, nếu có `avahi-browse`, `avahi-browse -rt _smarthome._tcp` có timeout; thiếu tool hoặc stack chưa chạy chỉ in cảnh báo trạng thái, không làm menu crash. Không đọc/in `.env`.
8. **Preflight và lỗi:** kiểm tra tối thiểu Bash, Docker Compose v2 cho các lệnh cần thiết và `openssl` khi phải sinh `.env`; thông báo rõ lệnh cài đặt còn thiếu (`docker`, `docker compose`, `avahi-daemon`, `avahi-utils`, `qrencode`) nhưng không tự chạy `apt`/`sudo apt`. Các script QR được phép tự xử lý trường hợp thiếu `qrencode` theo hành vi hiện có.

#### README bắt buộc phải mô tả toàn bộ vòng đời vận hành

Thêm một mục rõ ràng, có thể làm theo tuần tự, ví dụ `Khởi động server mới từ số 0`, bao gồm:

- tiền điều kiện OS/network/SSH và cài `git`, Docker Engine + Compose v2, `openssl`, `avahi-daemon`, `avahi-utils`, `qrencode` trên host; nhấn mạnh Docker phải được cài trước khi dùng Compose và không cần image/repo để cài Docker;
- clone repo bằng HTTPS hoặc SSH, checkout tag/commit/version cụ thể, rồi chuyển vào thư mục repo;
- lệnh chạy lần đầu `bash scripts/smarthome.sh init`, giải thích `.env` sinh từ `.env.example`, secret random, `.env` không commit;
- menu option 1–5/0 và các command mode tương ứng; ví dụ QR board bằng `--board-id` hoặc `-m MAC` trên thiết bị không có ESP-IDF;
- thứ tự init `config → pull/build → server-init → healthy`, cách kiểm tra `docker compose ps`, `avahi-browse`, và QR/pairing sau khi stack sẵn sàng;
- hành vi reboot (`restart: unless-stopped`), cập nhật version an toàn (pull/checkout rồi init/build lại), xử lý lỗi thường gặp và nguyên tắc không dán secret vào log/chat/commit;
- ranh giới: `smarthome.sh` chỉ chạy sau clone; nếu muốn bootstrap một lệnh từ OS trắng thì cần cloud-init/Ansible/installer ngoài repo, không giả vờ rằng script trong repo làm được việc đó.

#### Không đưa backup/restore vào M24

M24 giữ menu nhỏ và an toàn, **chưa thêm backup/restore volume InfluxDB**. Restore là thao tác phá hủy cần contract riêng (đường dẫn archive, xác nhận hai bước, dừng stack, backup hiện trạng trước khi ghi đè, kiểm tra ownership và verify dữ liệu sau restore). Đề xuất tách thành M25; nếu user muốn có ngay thì chỉnh plan trước khi duyệt/dispatch.

| task_key | Việc | File chính | Tiêu chí đạt | Lệnh kiểm tra |
|---|---|---|---|---|
| M24:coder | Tạo `scripts/smarthome.sh`: menu + command dispatch, sinh `.env` lần đầu với secret random an toàn, preload pull/build không chạy stack, wrapper cho server-init/QR/pairing/status, prompt QR board bằng MAC/id; cập nhật README hướng dẫn bootstrap OS trắng → clone → init → vận hành | `scripts/smarthome.sh` (mới), `README.md` | `bash -n` sạch; không ghi đè `.env` đã có; `.env` mới permission 600 và các secret cần thiết không còn placeholder; không lộ secret trong log/argv; init chỉ gọi `server-init.sh` sau pull/build/config thành công; menu/direct aliases + pass-through board args hoạt động; README có quy trình từ server chưa có repo/Docker đến reboot, menu, QR, status và troubleshooting; không sửa logic các script hiện có | `bash -n scripts/smarthome.sh`; test cô lập với `PATH`/fake command hoặc harness không chạm Docker thật; `docker compose config --quiet`; đọc diff + kiểm tra secret/permission; kiểm tra README đối chiếu checklist; sau coder orchestrator chạy lại toàn bộ kiểm tra |

Thứ tự M24: sau khi user duyệt, dispatch **một** `M24:coder`; coder xong mới cân nhắc reviewer/tester. Vì đây là host shell/orchestration có rủi ro về secret và process execution, reviewer đọc diff là nên có; tester có thể chạy harness kiểm tra menu/init bằng fake commands, không cần khởi động stack thật. Orchestrator phải tự xác minh lại mọi lệnh trước khi ghi M24 DONE.

**M24 hoàn tất (2026-09-26):** `scripts/smarthome.sh` mới + README section bootstrap/vận hành; coder xử lý hardening helper `server-init.sh`, cảnh báo Docker daemon, cleanup SIGTERM và câu README về dependency. Reviewer APPROVE. Tester bị giới hạn quyền shell nên không tự chạy lệnh, nhưng orchestrator đã chạy lại độc lập: `bash -n` PASS, harness fake-PATH **88/88 PASS**, `docker compose config --quiet` PASS, `git diff --check` PASS. Không chạy stack thật/không đụng volume; `.env` thật nguyên vẹn. Các thay đổi source M24 chưa commit theo quy định — chờ user yêu cầu commit nếu cần.

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
