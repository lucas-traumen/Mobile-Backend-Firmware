// Composition root: env → InfluxDB WriteApi → InfluxWriter → MqttService.
// Chạy: host `npm run start` (tsx) hoặc container `node dist/src/main.js`.
import { InfluxDB } from "@influxdata/influxdb-client"

import { BridgeService } from "./bridge/bridge-service.js"
import { InfluxWriter } from "./influx/influx-writer.js"
import { MqttService } from "./mqtt/mqtt-service.js"
import { loadBackendEnv, optionalEnv } from "./env.js"

// writeApi của client bị tắt retry nội bộ (maxRetries: 0) và auto-flush
// (flushInterval: 0): việc retry/backoff + queue giới hạn do InfluxWriter
// quản để đúng chính sách trong PLAN.md (backoff ≤ 30s, queue 1000, drop oldest).
const WRITE_OPTIONS = { flushInterval: 0, maxRetries: 0 } as const

// Bridge log mỗi hướng publish ở mức DEBUG; bật bằng LOG_LEVEL=debug (hoặc trace).
function isDebugLogLevel(): boolean {
  const level = optionalEnv("LOG_LEVEL")?.toLowerCase()
  return level === "debug" || level === "trace" || level === "verbose"
}

let shuttingDown = false

function main(): void {
  const env = loadBackendEnv() // thiếu biến nào → throw "Missing required env var: <NAME>"

  const influx = new InfluxDB({ url: env.influxUrl, token: env.influxToken })
  const writer = new InfluxWriter(() =>
    influx.getWriteApi(env.influxOrg, env.influxBucket, "ms", WRITE_OPTIONS),
  )

  const mqttService = new MqttService({
    url: env.mqttUrl,
    username: env.mqttUser,
    password: env.mqttPassword,
    sink: writer,
  })

  // Bridge contract firmware ↔ frontend (MQTT connection riêng, xem src/bridge/).
  const bridgeService = new BridgeService({
    url: env.mqttUrl,
    username: env.mqttUser,
    password: env.mqttPassword,
    topicPrefix: env.topicPrefix,
    debug: isDebugLogLevel(),
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
        await bridgeService.stop()
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
  void bridgeService.start()
}

main()
