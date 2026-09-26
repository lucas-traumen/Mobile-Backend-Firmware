# PROJECT_MEMORY.md — quyết định bền vững

> Chỉ ghi quyết định đúng dài hạn. Trạng thái đang chạy nằm ở `PLAN.md` và `.opencode/state/`.

## Stack

- TypeScript ESM (`"type": "module"`), strict, Node ≥ 24, npm, vitest.

## OpenCode v3 — đã xác minh với opencode 1.18.26 (2026-09-05)

- Agent definitions: `.opencode/agents/*.md` (mode + model + permission + prompt trong frontmatter). `opencode.json` chỉ chứa `default_agent: "orchestrator"` + `subagent_depth: 1`.
  - Lệch so văn bản đặc tả gốc (đề nghị JSON agent block trong opencode.json): chọn markdown-only để một nguồn duy nhất, tránh drift giữa hai chỗ khai báo.
- Model IDs (provider `xkiro`): sol = `xkiro/openai/gpt-5.6-sol`, terra = `xkiro/openai/gpt-5.6-terra`, coder = `xkiro/z-ai/glm-5.3-flash`, fallback opus = `xkiro/anthropic/claude-opus-5`.
- Reasoning effort: quản ở provider config GLOBAL (xkiro: sol/terra/opus mặc định `max`; glm-5.3-flash không override). Agent không set `reasoningEffort` — tránh gán máy móc max. Muốn đổi → sửa provider config global, không sửa repo này.
- Task tool (v1.18.26): args `description/prompt/subagent_type/task_id` (+ `background` experimental, tắt); metadata trả `{parentSessionId, sessionId, model}`; `task_id` chính là child session ID; title session con = `description + " (@agent subagent)"` nên marker `[run_id/task_key]` xuất hiện trong title — plugin dựa vào đó khi recovery; `subagent_depth` mặc định 1 chặn worker spawn lồng.
- Plugin hooks dùng: `tool.execute.before` (mutate `output.args`), `tool.execute.after`, catch-all `event` — payload nằm trong `event.properties` (`info` cho `session.created`, `part` cho `message.part.updated`).
- Plugin import `@opencode-ai/plugin` type-only; npm devDependency phục vụ typecheck local, runtime không cần resolve (opencode load file qua Bun, import type bị erase).
- State: `.opencode/state/<run_id>.json` schema v3; ghi atomic (tmp + rename); lock `.opencode/state/locks/<run_id>.json` theo PID (lock sống = process khác đang chạy run đó → không đụng); single-writer: từ chối dispatch coder thứ hai trên cùng worktree.

## Môi trường dev (xác minh 2026-09-05)

- ESP-IDF v6.0.1 cài qua EIM tại `~/.espressif/v6.0.1/esp-idf`; activate: `. ~/.espressif/tools/activate_idf_v6.0.1.sh` — LƯU Ý `idf.py` không có trên PATH trực tiếp, gọi `python "$IDF_PATH/tools/idf.py" <cmd>`.
- Component cache: `~/.espressif/tools/components`.
- ESP-IDF v6: `esp-mqtt` không còn trong core — firmware MQTT phải dùng managed component `espressif/mqtt` (khai qua `idf_component.yml`). **cJSON cũng vậy** — component `json` built-in đã bỏ ở v6, dùng managed `espressif/cjson` (namespaced `espressif__cjson` trong CMake PRIV_REQUIRES).
- Firmware dùng partition table OTA (`partitions.csv`: nvs/otadata/phy_init/ota_0/ota_1) từ M9a — layout phải giữ ổn định để đường OTA sau này không phải USB-flash lại toàn bộ.
- `sdkconfig` (gitignored, chứa Wi-Fi/MQTT credentials thật) tồn tại trên máy — sửa sdkconfig.defaults KHÔNG tự áp dụng cho sdkconfig hiện có; phải sửa sdkconfig trực tiếp hoặc xóa build lại. Luôn backup trước khi đụng, diff credentials sau khi xong.
- Docker 29.1.3 + Compose v5.5.0; Node ≥ 24; có internet (npm + components.espressif.com OK).

## Quyết định kiến trúc (2026-09-12)

- **Local-first**: toàn bộ stack trong Docker Compose trên một máy LAN. InfluxDB Cloud Serverless (M8) bị đảo ngược — history lưu InfluxDB 2.7.10 OSS local, backup = backup volume. Lý do: dễ bảo hành, một nơi duy nhất.
- **History**: app mobile query Flux trực tiếp InfluxDB local (`POST /api/v2/query`) bằng token read-only — KHÔNG có HTTP API backend. Measurement `sensors`.
- **Truy cập từ xa**: Tailscale (không cloud trung gian, không port forwarding). HOÃN — làm sau khi app chạy được trong LAN. Khi làm: sidecar container hoặc cài trực tiếp máy host.

Các bullet bridge và `deviceId ≡ roomId` của mục này **đã bị thay** bởi mục 2026-09-25. Giữ lại ở đây chỉ để đọc lịch sử M10–M13.

## Quyết định kiến trúc đã chốt (2026-09-25)

Một đường dữ liệu. Firmware và app nói cùng contract. Backend không dịch topic.

```
ESP32  --MQTT TCP 1883-->  amqtt  --WS 9001-->  app (realtime, cùng contract boards)
                              |
                              +--> backend (chỉ ingest telemetry v2) --> InfluxDB 2.7
                                                                          ^
app history: Flux POST /api/v2/query, token read-only -------------------+
```

- **Broker**: amqtt 0.12.1 (MQTT 3.1.1). TCP `1883` cho ESP và backend, WebSocket `9001` cho app. Auth bắt buộc, không anonymous. Mosquitto đã bỏ. Retained + LWT sống qua restart nhờ SessionDBPlugin. `docker stop` phải là SIGINT.
- **Contract duy nhất** `{prefix}/boards/{boardId}/...` với `prefix` mặc định `smarthome`:
  - `descriptor` retained — firmware publish mỗi lần connect
  - `telemetry` QoS 1, **không** retained — schema v2 `{"schemaVersion":2,"boardId","values":{"S1","S2"}}`
  - `sensors/{S}/state` retained — số thuần, cho app realtime
  - `status` retained — plain `online` / `offline` (LWT trên topic này)
  - `relays/{K}/set` payload `ON`/`OFF` → `relays/{K}/state` retained
- **Không bridge, không contract `room/`.** `src/bridge/` và ingest v1 đã xóa. Board firmware cũ không còn được dịch — phải flash v2.
- **Backend** chỉ làm một việc: subscribe descriptor + telemetry, map `values` → field Influx theo descriptor (không nhánh theo loại cảm biến), ghi measurement `sensors`. Mất descriptor → bỏ point, không queue. App điều khiển relay bằng cách publish thẳng topic `set`, không qua backend.
- **Identity**: `boardId` = `CONFIG_DEVICE_ID` nếu khác rỗng; không thì hex-8 lowercase của 4 byte cuối MAC STA (ví dụ `3b6baf6c`). Ổn định vì MAC eFuse không đổi, không lưu NVS. Phòng là khái niệm của app. Influx vẫn ghi tag `roomId = boardId` để query cũ không gãy; tab lịch sử mới lọc tag `boardId`.
- **Descriptor** là nguồn nghĩa của kênh (S1 → `temperature` °C, S2 → `humidity` %). Bảng phần cứng nằm trong firmware (`mqtt_app.c`, template A). Backend không hardcode tên kênh.
- **Discovery**: avahi `_smarthome._tcp` port `9001`, TXT `prefix`, `influx_port`, `influx_org`, `influx_bucket`. Secret không đi qua mDNS.
- **Provisioning** (thứ tự boot):
  1. NVS `bleprov` đủ 5 key — chỉ là ứng viên. Wi-Fi phải có IP trong 15 s. Reason 200/201 (AP không còn trên không khí) thì fail ngay, xóa namespace `bleprov`, reboot vào BLE. Hết 15 s mà chưa có IP và lý do không phải sai mật khẩu thì cũng xóa + reboot. Reason 202 và mọi `BAD_AUTH` thì **không xóa, không reboot**: dừng retry, giữ credential, log yêu cầu giữ BOOT ≥ 5 s. Có IP rồi mà MQTT không connected trong 10 s, hoặc `mqtt_app_init` lỗi: dừng client MQTT (stop + destroy) rồi mới xóa + reboot. Nhánh này không retry nền sau khi đã kết luận stale.
  2. Kconfig SSID khác rỗng — không tự xóa NVS, retry nền kể cả khi reason 200/201. Latch “AP mất thì ngừng retry” chỉ bật trên nhánh NVS.
  3. Không NVS và SSID rỗng — BLE advertising `IoTBoard-{boardId}`, GATT 9 UUID, Just Works. `CONNECTED` thì lưu NVS rồi tắt BLE sau 30 s. Nút BOOT giữ ≥ 5 s vẫn xóa NVS và reboot.
- **Broker URI** thiếu scheme được firmware bù `mqtt://` trước khi đưa vào esp-mqtt.

## Quy ước

- task_key: `<milestone>:<role>` — `M1:coder`, `M1:review`, `M1:test`.
- Reviewer read-only (edit+bash deny); tester chỉ được lệnh test/build qua duyệt (`ask`); coder edit allow nhưng scoped bằng prompt.
- Supervisor ngoài OpenCode (tự khởi động lại tiến trình chết): CHƯA triển khai — plugin chỉ phục hồi khi opencode được chạy lại trong project. Nếu cần auto-restart tuyệt đối, viết supervisor riêng ngoài plugin (plugin trong tiến trình không thể tự dựng lại tiến trình đã chết).
- Không commit `.opencode/state/`, secret, `.env`.
