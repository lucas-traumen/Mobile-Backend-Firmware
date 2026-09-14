// M10 — map thuần (pure function) giữa contract firmware `smarthome/...` và
// contract frontend `<prefix>/room/{roomId}/...`. Không I/O, không giữ state —
// unit test trực tiếp, bridge-service chỉ lo vòng đời MQTT.
//
// Quy ước (xem PLAN.md M10):
// - deviceId ≡ roomId (1:1). Telemetry mang roomId trong payload → dùng payload;
//   status/relay-state không mang roomId → chuyển thẳng deviceId thành roomId.
// - Relay firmware hiện có K1..K3 ⇒ slot frontend 1..3; slot 4..10 bỏ qua + log DEBUG.
import { telemetrySchema } from "../telemetry/schema.js"

/** Prefix topic cố định phía firmware (firmware không đổi contract theo env). */
export const FIRMWARE_TOPIC_PREFIX = "smarthome"

export const FIRMWARE_TELEMETRY_FILTER = `${FIRMWARE_TOPIC_PREFIX}/+/telemetry`
export const FIRMWARE_STATUS_FILTER = `${FIRMWARE_TOPIC_PREFIX}/+/status`
export const FIRMWARE_RELAY_STATE_FILTER = `${FIRMWARE_TOPIC_PREFIX}/+/relay/state`

/** Số kênh relay firmware đang hỗ trợ (K1..K3) — slot ngoài khoảng này bridge bỏ qua. */
export const FIRMWARE_RELAY_CHANNELS = 3

/** Một bản tin bridge sẽ publish lại (QoS/retain quyết định bởi chiều map). */
export interface BridgePublish {
  topic: string
  payload: string
  qos: 0 | 1 | 2
  retain: boolean
}

/** Logger tối thiểu bridge dùng; bridge-service truyền bản console, test truyền spy. */
export interface BridgeLogger {
  debug(message: string): void
  info(message: string): void
  warn(message: string): void
  error(message: string): void
}

export type BridgeTopicKind = "telemetry" | "status" | "relayState" | "relayCommand"

const noopLogger: BridgeLogger = {
  debug: () => {},
  info: () => {},
  warn: () => {},
  error: () => {},
}

function escapeRegExp(value: string): string {
  return value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")
}

// Prefix firmware là hằng nên escape một lần; prefix frontend động nên build mỗi call.
const FIRMWARE_PREFIX_PATTERN = escapeRegExp(FIRMWARE_TOPIC_PREFIX)
const TELEMETRY_TOPIC = new RegExp(`^${FIRMWARE_PREFIX_PATTERN}\\/([^/]+)\\/telemetry$`)
const STATUS_TOPIC = new RegExp(`^${FIRMWARE_PREFIX_PATTERN}\\/([^/]+)\\/status$`)
const RELAY_STATE_TOPIC = new RegExp(`^${FIRMWARE_PREFIX_PATTERN}\\/([^/]+)\\/relay\\/state$`)
const RELAY_KEY = /^K([0-9]+)$/

function frontendRelayCommandPattern(prefix: string): RegExp {
  return new RegExp(`^${escapeRegExp(prefix)}\\/room\\/([^/]+)\\/cmnd\\/relay\\/([^/]+)$`)
}

function decodePayload(rawPayload: Buffer | string): string {
  return typeof rawPayload === "string" ? rawPayload : rawPayload.toString("utf8")
}

function message(topic: string, payload: string, retain: boolean): BridgePublish {
  return { topic, payload, qos: 1, retain }
}

/** Nhận diện message thuộc nhóm nào (để bridge-service route đúng mapper, tránh log nhiễu). */
export function classifyBridgeTopic(topic: string, prefix: string): BridgeTopicKind | null {
  if (TELEMETRY_TOPIC.test(topic)) return "telemetry"
  if (STATUS_TOPIC.test(topic)) return "status"
  if (RELAY_STATE_TOPIC.test(topic)) return "relayState"
  if (frontendRelayCommandPattern(prefix).test(topic)) return "relayCommand"
  return null
}

/**
 * Firmware `smarthome/{deviceId}/telemetry` (JSON) → tách thành 2 topic số cho app,
 * retained để app mở lên là có giá trị mới nhất ngay.
 */
export function mapTelemetryToFrontend(
  topic: string,
  rawPayload: Buffer | string,
  prefix: string,
  logger: BridgeLogger = noopLogger,
): BridgePublish[] {
  const match = TELEMETRY_TOPIC.exec(topic)
  if (match === null) {
    logger.debug(`bỏ telemetry: topic không khớp "smarthome/{deviceId}/telemetry": "${topic}"`)
    return []
  }

  let json: unknown
  try {
    json = JSON.parse(decodePayload(rawPayload))
  } catch {
    logger.debug(`bỏ telemetry topic "${topic}": payload không phải JSON hợp lệ`)
    return []
  }

  const parsed = telemetrySchema.safeParse(json)
  if (!parsed.success) {
    logger.debug(`bỏ telemetry topic "${topic}": payload sai shape`)
    return []
  }

  const { roomId, temperature, humidity } = parsed.data
  return [
    message(`${prefix}/room/${roomId}/sensor/temperature`, String(temperature), true),
    message(`${prefix}/room/${roomId}/sensor/humidity`, String(humidity), true),
  ]
}

/**
 * Firmware `smarthome/{deviceId}/status` (`online`/`offline`) → app.
 * Payload không mang roomId ⇒ roomId = deviceId (quy ước 1:1).
 */
export function mapStatusToFrontend(
  topic: string,
  rawPayload: Buffer | string,
  prefix: string,
  logger: BridgeLogger = noopLogger,
): BridgePublish[] {
  const match = STATUS_TOPIC.exec(topic)
  if (match === null) {
    logger.debug(`bỏ status: topic không khớp "smarthome/{deviceId}/status": "${topic}"`)
    return []
  }

  const value = decodePayload(rawPayload).trim()
  if (value !== "online" && value !== "offline") {
    logger.debug(`bỏ status topic "${topic}": payload "${value}" không phải online/offline`)
    return []
  }

  const roomId = match[1]
  return [message(`${prefix}/room/${roomId}/status`, value, true)]
}

/**
 * Firmware `smarthome/{deviceId}/relay/state` (retained JSON) → tách `<prefix>/room/{roomId}/stat/relay/{n}`
 * cho từng kênh K1..K3 có trong payload. Kênh lạ / state lạ → bỏ riêng kênh đó + log DEBUG.
 */
export function mapRelayStateToFrontend(
  topic: string,
  rawPayload: Buffer | string,
  prefix: string,
  logger: BridgeLogger = noopLogger,
): BridgePublish[] {
  const match = RELAY_STATE_TOPIC.exec(topic)
  if (match === null) {
    logger.debug(`bỏ relay state: topic không khớp "smarthome/{deviceId}/relay/state": "${topic}"`)
    return []
  }

  let json: unknown
  try {
    json = JSON.parse(decodePayload(rawPayload))
  } catch {
    logger.debug(`bỏ relay state topic "${topic}": payload không phải JSON hợp lệ`)
    return []
  }

  if (typeof json !== "object" || json === null) {
    logger.debug(`bỏ relay state topic "${topic}": payload không phải object`)
    return []
  }

  const record = json as Record<string, unknown>
  if (record.schemaVersion !== 1) {
    logger.debug(`bỏ relay state topic "${topic}": schemaVersion không phải 1`)
    return []
  }

  const deviceId = match[1]
  const channels: Array<{ slot: number; state: string }> = []
  for (const [key, value] of Object.entries(record)) {
    const keyMatch = RELAY_KEY.exec(key)
    if (keyMatch === null) continue
    const slot = Number(keyMatch[1])
    if (!Number.isInteger(slot) || slot < 1 || slot > FIRMWARE_RELAY_CHANNELS) {
      logger.debug(`bỏ kênh "${key}" topic "${topic}": slot ${slot} ngoài 1..${FIRMWARE_RELAY_CHANNELS}`)
      continue
    }
    if (value !== "ON" && value !== "OFF") {
      logger.debug(`bỏ kênh "${key}" topic "${topic}": state "${String(value)}" không phải ON/OFF`)
      continue
    }
    channels.push({ slot, state: value })
  }

  channels.sort((a, b) => a.slot - b.slot)
  return channels.map((channel) =>
    message(`${prefix}/room/${deviceId}/stat/relay/${channel.slot}`, channel.state, true),
  )
}

/**
 * Frontend `<prefix>/room/{roomId}/cmnd/relay/{slot}` (payload `ON`/`OFF` thuần) → firmware
 * `smarthome/{deviceId}/relay/K{n}/set` (payload thuần, không retained). roomId = deviceId.
 * Slot 4..10 (và slot >10) → bỏ qua + log DEBUG (firmware chưa có kênh tương ứng).
 */
export function mapFrontendRelayCommand(
  topic: string,
  rawPayload: Buffer | string,
  prefix: string,
  logger: BridgeLogger = noopLogger,
): BridgePublish[] {
  const match = frontendRelayCommandPattern(prefix).exec(topic)
  if (match === null) {
    logger.debug(`bỏ relay command: topic không khớp "{prefix}/room/{roomId}/cmnd/relay/{slot}": "${topic}"`)
    return []
  }

  const roomId = match[1]
  const slotRaw = match[2]
  const slot = Number(slotRaw)
  if (!Number.isInteger(slot) || slot < 1 || slot > FIRMWARE_RELAY_CHANNELS) {
    logger.debug(`bỏ relay command topic "${topic}": slot "${slotRaw}" không thuộc 1..${FIRMWARE_RELAY_CHANNELS}`)
    return []
  }

  const value = decodePayload(rawPayload).trim()
  if (value !== "ON" && value !== "OFF") {
    logger.debug(`bỏ relay command topic "${topic}": payload "${value}" không phải ON/OFF`)
    return []
  }

  // deviceId ≡ roomId (1:1) — chuyển thẳng roomId thành deviceId.
  return [message(`${FIRMWARE_TOPIC_PREFIX}/${roomId}/relay/K${slot}/set`, value, false)]
}
