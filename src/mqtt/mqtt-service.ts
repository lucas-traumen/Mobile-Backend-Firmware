import mqtt, { type MqttClient } from "mqtt"
import type { TelemetryPayload } from "../telemetry/schema.js"
import { validateTelemetryMessage } from "../telemetry/validate.js"

export const TELEMETRY_TOPIC_FILTER = "smarthome/+/telemetry"

/** Điểm đến của payload hợp lệ — InfluxWriter thỏa structurally. */
export interface TelemetrySink {
  write(payload: TelemetryPayload): void
}

export interface MqttServiceOptions {
  url: string
  username: string
  password: string
  /** Nơi nhận payload đã validate. */
  sink: TelemetrySink
  topicFilter?: string
  /** mqtt.js tự reconnect với chu kỳ này (ms); mặc định 1000. */
  reconnectPeriodMs?: number
}

/**
 * Đóng gói mqtt.js: subscribe `smarthome/+/telemetry`, route message hợp lệ
 * vào sink (validate trong service), auto-reconnect là hành vi mặc định của
 * mqtt.js (reconnectPeriod > 0) + resubscribe trong event 'connect' (bắn lại
 * sau mỗi lần nối thành công, kể cả reconnect).
 *
 * Không handler nào được phép throw ra ngoài event loop.
 */
export class MqttService {
  private client: MqttClient | null = null
  private readonly options: Required<Pick<MqttServiceOptions, "url" | "username" | "password" | "sink">> &
    Pick<MqttServiceOptions, "topicFilter" | "reconnectPeriodMs">

  constructor(options: MqttServiceOptions) {
    this.options = options
  }

  async start(): Promise<void> {
    if (this.client !== null) return
    const client = mqtt.connect(this.options.url, {
      username: this.options.username,
      password: this.options.password,
      reconnectPeriod: this.options.reconnectPeriodMs ?? 1000,
      connectTimeout: 10_000,
    })
    this.client = client

    client.on("connect", () => {
      console.log(`[mqtt-service] connected tới ${this.options.url}`)
      // 'connect' bắn lại sau mỗi lần reconnect → resubscribe luôn ở đây.
      client.subscribe(this.topicFilter, { qos: 1 }, (err) => {
        if (err !== null) {
          console.error(`[mqtt-service] subscribe "${this.topicFilter}" lỗi: ${err.message}`)
        } else {
          console.log(`[mqtt-service] subscribed "${this.topicFilter}" (qos 1)`)
        }
      })
    })
    client.on("message", (topic, payload) => {
      this.handleMessage(topic, payload)
    })
    client.on("error", (err) => {
      console.error(`[mqtt-service] lỗi client: ${err.message}`)
    })
    client.on("close", () => {
      console.warn("[mqtt-service] kết nối đóng — sẽ tự reconnect")
    })
    client.on("reconnect", () => {
      console.warn("[mqtt-service] đang reconnect...")
    })
  }

  async stop(): Promise<void> {
    const client = this.client
    this.client = null
    if (client === null) return
    await new Promise<void>((resolve) => {
      client.end(false, {}, () => resolve())
    })
    console.log("[mqtt-service] đã ngắt kết nối broker")
  }

  private get topicFilter(): string {
    return this.options.topicFilter ?? TELEMETRY_TOPIC_FILTER
  }

  /** validate → sink.write; mọi lỗi dừng ở đây, không crash process. */
  private handleMessage(topic: string, payload: Buffer): void {
    try {
      const message = validateTelemetryMessage(topic, payload)
      if (message !== null) {
        this.options.sink.write(message.payload)
      }
    } catch (error) {
      console.error(
        `[mqtt-service] xử lý message topic "${topic}" lỗi: ${
          error instanceof Error ? error.message : String(error)
        }`,
      )
    }
  }
}
