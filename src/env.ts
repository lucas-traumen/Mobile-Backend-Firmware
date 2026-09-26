// Đọc + validate biến môi trường bắt buộc.
// Không dùng dotenv: container inject env qua docker compose (env_file),
// khi chạy trên host thì export env hoặc chạy qua tsx với env sẵn có.

export interface BackendEnv {
  mqttUrl: string
  mqttUser: string
  mqttPassword: string
  influxUrl: string
  influxToken: string
  influxOrg: string
  influxBucket: string
  /** Prefix topic contract boards (`<prefix>/boards/{boardId}/...`); default `smarthome`. */
  topicPrefix: string
}

/** Prefix contract boards khi TOPIC_PREFIX không đặt hoặc để trống. */
export const DEFAULT_TOPIC_PREFIX = "smarthome"

/** boardId hợp lệ: chữ/số/gạch ngang/gạch dưới (an toàn cho segment topic MQTT). */
export const BOARD_ID_PATTERN = /^[a-zA-Z0-9_-]+$/

export interface InfluxEnv {
  influxUrl: string
  influxToken: string
  influxOrg: string
  influxBucket: string
}

export function requireEnv(name: string): string {
  const value = process.env[name]
  if (value === undefined || value === "") {
    throw new Error(`Missing required env var: ${name}`)
  }
  return value
}

/** Biến optional: undefined hoặc chuỗi rỗng đều trả undefined để caller dùng default. */
export function optionalEnv(name: string): string | undefined {
  const value = process.env[name]
  return value === undefined || value === "" ? undefined : value
}

/** Env cho luồng telemetry đầy đủ (main.ts). */
export function loadBackendEnv(): BackendEnv {
  return {
    mqttUrl: requireEnv("MQTT_URL"),
    mqttUser: requireEnv("MQTT_USER"),
    mqttPassword: requireEnv("MQTT_PASSWORD"),
    influxUrl: requireEnv("INFLUX_URL"),
    influxToken: requireEnv("INFLUX_TOKEN"),
    influxOrg: requireEnv("INFLUX_ORG"),
    influxBucket: requireEnv("INFLUX_BUCKET"),
    topicPrefix: optionalEnv("TOPIC_PREFIX") ?? DEFAULT_TOPIC_PREFIX,
  }
}

/** Env chỉ phục vụ truy vấn InfluxDB (src/query/*.ts). */
export function loadInfluxEnv(): InfluxEnv {
  return {
    influxUrl: requireEnv("INFLUX_URL"),
    influxToken: requireEnv("INFLUX_TOKEN"),
    influxOrg: requireEnv("INFLUX_ORG"),
    influxBucket: requireEnv("INFLUX_BUCKET"),
  }
}
