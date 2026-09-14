// CLI: lịch sử telemetry 1 giờ gần nhất của một roomId.
// Chạy trên host: npx tsx src/query/history.ts <roomId>
// (cần INFLUX_URL, INFLUX_TOKEN, INFLUX_ORG, INFLUX_BUCKET — xem .env.example)
//
// M11: InfluxDB 2.7 local trong compose hỗ trợ Flux, nên đường đọc dùng lại
// @influxdata/influxdb-client (query API v2). Schema mới: measurement `sensors`,
// tag `roomId`, fields `temperature`/`humidity`. Mỗi phòng một ESP nên
// deviceId ≡ roomId (xem PROJECT_MEMORY.md) — CLI nhận roomId.
import { InfluxDB } from "@influxdata/influxdb-client"

import { loadInfluxEnv } from "../env.js"

const roomId = process.argv[2]
if (roomId === undefined || roomId === "") {
  console.error("Usage: npx tsx src/query/history.ts <roomId>")
  process.exit(1)
}

// roomId/bucket ghép thẳng vào chuỗi Flux (client v2 không bind param cho tag
// value) nên vẫn giữ regex chặn ký tự đặc biệt — fail-fast cho CLI dev tool
// (defense-in-depth).
const ROOM_ID_REGEX = /^[a-zA-Z0-9_-]+$/
if (!ROOM_ID_REGEX.test(roomId)) {
  console.error(`Invalid roomId: must match ${ROOM_ID_REGEX.source}`)
  process.exit(1)
}

const env = loadInfluxEnv()

const BUCKET_REGEX = /^[a-zA-Z0-9_.-]+$/
if (!BUCKET_REGEX.test(env.influxBucket)) {
  console.error(`Invalid influxBucket: must match ${BUCKET_REGEX.source}`)
  process.exit(1)
}

const queryApi = new InfluxDB({ url: env.influxUrl, token: env.influxToken }).getQueryApi(env.influxOrg)

const flux = `
from(bucket: "${env.influxBucket}")
  |> range(start: -1h)
  |> filter(fn: (r) => r._measurement == "sensors" and r.roomId == "${roomId}")
  |> filter(fn: (r) => r._field == "temperature" or r._field == "humidity")
  |> sort(columns: ["_time"], desc: true)
`

interface TelemetryRow {
  time: string
  roomId: string
  field: string
  value: unknown
}

console.log(`Lịch sử 1 giờ của room "${roomId}":`)
try {
  const rows = await queryApi.collectRows<TelemetryRow>(flux, (values, tableMeta) => {
    const row = tableMeta.toObject(values)
    return {
      time: toUtcIso(row._time),
      roomId: String(row.roomId ?? ""),
      field: String(row._field ?? ""),
      value: row._value,
    }
  })
  for (const row of rows) {
    if (row.value !== null && row.value !== undefined) {
      console.log(`  ${row.time}  ${row.roomId}  ${row.field}=${row.value}`)
    }
  }
  console.log("Xong.")
} catch (error) {
  console.error(`Truy vấn lỗi: ${error instanceof Error ? error.message : String(error)}`)
  process.exitCode = 1
}

function toUtcIso(value: unknown): string {
  const date = new Date(typeof value === "number" ? value : String(value))
  return Number.isNaN(date.getTime()) ? String(value) : date.toISOString()
}
