import mqtt, { type MqttClient } from "mqtt"

/**
 * Route descriptor registry + boards ingest chạy trên CÙNG connection (M14a —
 * không tạo connection MQTT thứ hai). handleMessage trả true nếu message thuộc
 * route (đã xử lý, kể cả bị bỏ sau WARN); false → rơi xuống các route sau.
 */
export interface MqttExtraRoute {
  topicFilter: string
  handleMessage(topic: string, payload: Buffer): boolean
}

export interface MqttServiceOptions {
  url: string
  username: string
  password: string
  /** mqtt.js tự reconnect với chu kỳ này (ms); mặc định 1000. */
  reconnectPeriodMs?: number
  /** Route descriptor + telemetry v2; mỗi filter được subscribe trong event 'connect' (resubscribe khi reconnect). */
  extraRoutes?: MqttExtraRoute[]
}

/**
 * Đóng gói mqtt.js: một MQTT connection, subscribe filter của từng extraRoutes
 * (descriptor registry + telemetry v2), route message về route tương ứng.
 * Auto-reconnect là hành vi mặc định của mqtt.js (reconnectPeriod > 0) +
 * resubscribe trong event 'connect' (bắn lại sau mỗi lần nối thành công, kể cả
 * reconnect — retained descriptor của extraRoutes cũng được bắn lại lúc này).
 *
 * Không handler nào được phép throw ra ngoài event loop.
 */
export class MqttService {
  private client: MqttClient | null = null
  private readonly options: Required<Pick<MqttServiceOptions, "url" | "username" | "password">> &
    Pick<MqttServiceOptions, "reconnectPeriodMs" | "extraRoutes">

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
      for (const route of this.extraRoutes) {
        client.subscribe(route.topicFilter, { qos: 1 }, (err) => {
          if (err !== null) {
            console.error(`[mqtt-service] subscribe "${route.topicFilter}" lỗi: ${err.message}`)
          } else {
            console.log(`[mqtt-service] subscribed "${route.topicFilter}" (qos 1)`)
          }
        })
      }
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

  private get extraRoutes(): readonly MqttExtraRoute[] {
    return this.options.extraRoutes ?? []
  }

  /** Mỗi route tự chốt topic mình phụ trách; không route nào nhận → log + bỏ. */
  private handleMessage(topic: string, payload: Buffer): void {
    try {
      for (const route of this.extraRoutes) {
        if (route.handleMessage(topic, payload)) return
      }
      console.warn(`[mqtt-service] không route nào nhận topic "${topic}" — bỏ`)
    } catch (error) {
      console.error(
        `[mqtt-service] xử lý message topic "${topic}" lỗi: ${
          error instanceof Error ? error.message : String(error)
        }`,
      )
    }
  }
}
