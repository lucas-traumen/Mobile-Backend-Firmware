// M10 — Bridge service: MQTT client riêng (client-id `backend-bridge`) subscribe
// 4 nhóm topic (telemetry/status/relay-state firmware + relay command frontend),
// route qua topic-mapper rồi re-publish. Tách connection khỏi MqttService để luồng
// telemetry→Influx (src/mqtt/mqtt-service.ts) không bị ảnh hưởng.
//
// Bất biến: không handler nào được throw ra event loop; mqtt.js tự reconnect
// (reconnectPeriod > 0) và resubscribe trong event 'connect' (bắn lại mỗi lần nối).
import mqtt, { type MqttClient } from "mqtt"

import {
  FIRMWARE_RELAY_STATE_FILTER,
  FIRMWARE_STATUS_FILTER,
  FIRMWARE_TELEMETRY_FILTER,
  classifyBridgeTopic,
  mapFrontendRelayCommand,
  mapRelayStateToFrontend,
  mapStatusToFrontend,
  mapTelemetryToFrontend,
  type BridgeLogger,
  type BridgePublish,
} from "./topic-mapper.js"

export const BRIDGE_CLIENT_ID = "backend-bridge"

const LOG_PREFIX = "[bridge]"

export interface BridgeServiceOptions {
  url: string
  username: string
  password: string
  /** Prefix topic contract frontend; default `smarthome`. */
  topicPrefix: string
  /** mqtt.js tự reconnect với chu kỳ này (ms); mặc định 1000. */
  reconnectPeriodMs?: number
  /** Bật log DEBUG (mỗi hướng publish / message bỏ qua). Mặc định tắt. */
  debug?: boolean
  logger?: BridgeLogger
}

const consoleLogger: BridgeLogger = {
  debug: (message) => console.debug(`${LOG_PREFIX} ${message}`),
  info: (message) => console.log(`${LOG_PREFIX} ${message}`),
  warn: (message) => console.warn(`${LOG_PREFIX} ${message}`),
  error: (message) => console.error(`${LOG_PREFIX} ${message}`),
}

export class BridgeService {
  private client: MqttClient | null = null
  private readonly options: BridgeServiceOptions & { debug: boolean; logger: BridgeLogger }

  constructor(options: BridgeServiceOptions) {
    const debug = options.debug ?? false
    const base = options.logger ?? consoleLogger
    // DEBUG (mỗi hướng publish + message bỏ qua) chỉ bật khi LOG_LEVEL=debug;
    // tắt thì nuốt log debug nhưng vẫn giữ info/warn/error.
    const logger: BridgeLogger = debug ? base : { ...base, debug: () => {} }
    this.options = { ...options, debug, logger }
  }

  async start(): Promise<void> {
    if (this.client !== null) return
    const client = mqtt.connect(this.options.url, {
      clientId: BRIDGE_CLIENT_ID,
      username: this.options.username,
      password: this.options.password,
      reconnectPeriod: this.options.reconnectPeriodMs ?? 1000,
      connectTimeout: 10_000,
    })
    this.client = client

    client.on("connect", () => {
      this.options.logger.info(`connected tới ${this.options.url}`)
      this.subscribeFirmwareTopics(client)
      this.subscribeFrontendCommand(client)
    })
    client.on("message", (topic, payload) => {
      this.handleMessage(topic, payload)
    })
    client.on("error", (err) => {
      this.options.logger.error(`lỗi client: ${err.message}`)
    })
    client.on("close", () => {
      this.options.logger.warn("kết nối đóng — sẽ tự reconnect")
    })
    client.on("reconnect", () => {
      this.options.logger.warn("đang reconnect...")
    })
  }

  async stop(): Promise<void> {
    const client = this.client
    this.client = null
    if (client === null) return
    await new Promise<void>((resolve) => {
      client.end(false, {}, () => resolve())
    })
    this.options.logger.info("đã ngắt kết nối broker")
  }

  /** Subscribe theo từng filter để log rõ filter nào lỗi (không gộp mảng). */
  private subscribeFirmwareTopics(client: MqttClient): void {
    const filters = [FIRMWARE_TELEMETRY_FILTER, FIRMWARE_STATUS_FILTER, FIRMWARE_RELAY_STATE_FILTER]
    for (const filter of filters) {
      client.subscribe(filter, { qos: 1 }, (err) => {
        if (err !== null) {
          this.options.logger.error(`subscribe "${filter}" lỗi: ${err.message}`)
        } else {
          this.options.logger.info(`subscribed "${filter}" (qos 1)`)
        }
      })
    }
  }

  private subscribeFrontendCommand(client: MqttClient): void {
    const filter = `${this.options.topicPrefix}/room/+/cmnd/relay/+`
    client.subscribe(filter, { qos: 1 }, (err) => {
      if (err !== null) {
        this.options.logger.error(`subscribe "${filter}" lỗi: ${err.message}`)
      } else {
        this.options.logger.info(`subscribed "${filter}" (qos 1)`)
      }
    })
  }

  /** Route message theo nhóm topic; mọi lỗi dừng ở đây, không crash process. */
  private handleMessage(topic: string, payload: Buffer): void {
    try {
      const kind = classifyBridgeTopic(topic, this.options.topicPrefix)
      if (kind === null) {
        this.options.logger.debug(`bỏ topic lạ "${topic}"`)
        return
      }

      const prefix = this.options.topicPrefix
      const publishes: BridgePublish[] =
        kind === "telemetry"
          ? mapTelemetryToFrontend(topic, payload, prefix, this.options.logger)
          : kind === "status"
            ? mapStatusToFrontend(topic, payload, prefix, this.options.logger)
            : kind === "relayState"
              ? mapRelayStateToFrontend(topic, payload, prefix, this.options.logger)
              : mapFrontendRelayCommand(topic, payload, prefix, this.options.logger)

      for (const publish of publishes) {
        this.publish(publish)
      }
    } catch (error) {
      this.options.logger.error(
        `xử lý message topic "${topic}" lỗi: ${error instanceof Error ? error.message : String(error)}`,
      )
    }
  }

  private publish(publish: BridgePublish): void {
    const client = this.client
    if (client === null) {
      this.options.logger.warn(`bỏ publish "${publish.topic}": chưa kết nối`)
      return
    }
    if (this.options.debug) {
      this.options.logger.debug(
        `publish "${publish.topic}" <- ${publish.payload} (qos ${publish.qos}, retain ${publish.retain})`,
      )
    }
    client.publish(publish.topic, publish.payload, { qos: publish.qos, retain: publish.retain }, (err) => {
      if (err != null) {
        this.options.logger.error(`publish "${publish.topic}" lỗi: ${err.message}`)
      }
    })
  }
}
