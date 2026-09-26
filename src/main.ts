// Composition root: env → InfluxDB WriteApi → InfluxWriter → MqttService
// (+ descriptor registry + ingest telemetry v2 trên cùng connection mqtt-service).
// Chạy: host `npm run start` (tsx) hoặc container `node dist/src/main.js`.
import { InfluxDB } from "@influxdata/influxdb-client"

import { BoardsTelemetryIngest } from "./boards/boards-ingest.js"
import { DescriptorRegistry } from "./boards/descriptor-registry.js"
import { InfluxWriter } from "./influx/influx-writer.js"
import { MqttService } from "./mqtt/mqtt-service.js"
import { loadBackendEnv } from "./env.js"

// writeApi của client bị tắt retry nội bộ (maxRetries: 0) và auto-flush
// (flushInterval: 0): việc retry/backoff + queue giới hạn do InfluxWriter
// quản để đúng chính sách trong PLAN.md (backoff ≤ 30s, queue 1000, drop oldest).
const WRITE_OPTIONS = { flushInterval: 0, maxRetries: 0 } as const

let shuttingDown = false

function main(): void {
  const env = loadBackendEnv() // thiếu biến nào → throw "Missing required env var: <NAME>"

  const influx = new InfluxDB({ url: env.influxUrl, token: env.influxToken })
  const writer = new InfluxWriter(() =>
    influx.getWriteApi(env.influxOrg, env.influxBucket, "ms", WRITE_OPTIONS),
  )

  // Descriptor registry + ingest telemetry v2 chạy trên CÙNG connection của
  // MqttService (extraRoutes — backend chỉ có một MQTT connection). Retained
  // descriptor tự nạp ngay sau subscribe và bắn lại sau mỗi lần resubscribe
  // khi reconnect.
  const descriptorRegistry = new DescriptorRegistry({ prefix: env.topicPrefix })
  const boardsIngest = new BoardsTelemetryIngest({
    prefix: env.topicPrefix,
    registry: descriptorRegistry,
    onPoint: (point) => writer.writePointData(point),
  })

  const mqttService = new MqttService({
    url: env.mqttUrl,
    username: env.mqttUser,
    password: env.mqttPassword,
    extraRoutes: [descriptorRegistry, boardsIngest],
  })

  const shutdown = (signal: string): void => {
    if (shuttingDown) {
      process.exit(1) // tín hiệu lần hai: thoát ngay
    }
    shuttingDown = true
    console.log(`[main] nhận ${signal} — graceful shutdown: đóng writeApi rồi disconnect mqtt...`)
    void (async () => {
      try {
        await writer.close()
        await mqttService.stop()
        console.log("[main] shutdown xong")
        process.exit(0)
      } catch (error) {
        console.error(
          `[main] shutdown lỗi: ${error instanceof Error ? error.message : String(error)}`,
        )
        process.exit(1)
      }
    })()
  }
  process.on("SIGINT", () => shutdown("SIGINT"))
  process.on("SIGTERM", () => shutdown("SIGTERM"))

  void mqttService.start()
}

main()
