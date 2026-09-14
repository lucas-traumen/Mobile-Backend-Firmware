import { telemetrySchema, type TelemetryPayload } from "./schema.js"

const TOPIC_PATTERN = /^smarthome\/([^/]+)\/telemetry$/

const MAX_LOGGED_PAYLOAD_LENGTH = 300

export interface ValidatedTelemetry {
  topic: string
  /** deviceId lấy từ topic — nguồn tin cậy (firmware publish lên đúng topic riêng của nó). */
  deviceId: string
  payload: TelemetryPayload
}

/** Trả về deviceId từ topic `smarthome/{deviceId}/telemetry`, hoặc null nếu topic không khớp. */
export function extractDeviceIdFromTopic(topic: string): string | null {
  const match = TOPIC_PATTERN.exec(topic)
  return match === null ? null : match[1]
}

/**
 * Parse bản tin MQTT telemetry:
 * 1. topic phải khớp `smarthome/{deviceId}/telemetry`;
 * 2. payload phải là JSON hợp lệ;
 * 3. payload phải qua telemetrySchema (Zod);
 * 4. payload.deviceId phải trùng deviceId trong topic.
 *
 * Không bao giờ throw — sai thì log chi tiết rồi trả null để caller bỏ qua.
 */
export function validateTelemetryMessage(
  topic: string,
  rawPayload: Buffer | string,
): ValidatedTelemetry | null {
  const topicDeviceId = extractDeviceIdFromTopic(topic)
  if (topicDeviceId === null) {
    console.warn(
      `[validate] topic không khớp mẫu "smarthome/{deviceId}/telemetry": "${topic}" — bỏ qua`,
    )
    return null
  }

  const raw = typeof rawPayload === "string" ? rawPayload : rawPayload.toString("utf8")

  let json: unknown
  try {
    json = JSON.parse(raw)
  } catch (error) {
    console.warn(
      `[validate] JSON hỏng trên topic "${topic}": ${
        error instanceof Error ? error.message : String(error)
      } — payload: ${truncateForLog(raw)}`,
    )
    return null
  }

  const parsed = telemetrySchema.safeParse(json)
  if (!parsed.success) {
    console.warn(
      `[validate] payload sai schema trên topic "${topic}": ${JSON.stringify(parsed.error.issues)}` +
        ` — payload: ${truncateForLog(raw)}`,
    )
    return null
  }

  if (parsed.data.deviceId !== topicDeviceId) {
    console.warn(
      `[validate] deviceId lệch topic "${topic}": payload.deviceId="${parsed.data.deviceId}"` +
        ` — payload: ${truncateForLog(raw)}`,
    )
    return null
  }

  return { topic, deviceId: topicDeviceId, payload: parsed.data }
}

function truncateForLog(value: string, maxLength = MAX_LOGGED_PAYLOAD_LENGTH): string {
  return value.length <= maxLength ? value : `${value.slice(0, maxLength)}...(truncated)`
}
