import { telemetryV2Schema, type TelemetryV2Payload } from "./schema-v2.js"

const MAX_LOGGED_PAYLOAD_LENGTH = 300

export interface ValidatedTelemetryV2 {
  topic: string
  /** boardId lấy từ topic — nguồn tin cậy (firmware v2 publish lên đúng topic riêng của nó). */
  boardId: string
  payload: TelemetryV2Payload
}

function escapeRegExp(value: string): string {
  return value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")
}

/** Pattern topic telemetry v2 `{prefix}/boards/{boardId}/telemetry` — prefix động theo env (M14a). */
export function boardsTelemetryTopicPattern(prefix: string): RegExp {
  return new RegExp(`^${escapeRegExp(prefix)}\\/boards\\/([^/]+)\\/telemetry$`)
}

/** Trả về boardId từ topic `{prefix}/boards/{boardId}/telemetry`, hoặc null nếu topic không khớp. */
export function extractBoardIdFromBoardsTelemetryTopic(
  topic: string,
  prefix: string,
): string | null {
  const match = boardsTelemetryTopicPattern(prefix).exec(topic)
  return match === null ? null : match[1]
}

/**
 * Parse bản tin MQTT telemetry v2:
 * 1. topic phải khớp `{prefix}/boards/{boardId}/telemetry`;
 * 2. payload phải là JSON hợp lệ;
 * 3. payload phải qua telemetryV2Schema (Zod);
 * 4. payload.boardId phải trùng boardId trong segment topic.
 *
 * Không bao giờ throw — sai thì log chi tiết rồi trả null để caller bỏ qua.
 */
export function validateTelemetryV2Message(
  topic: string,
  rawPayload: Buffer | string,
  prefix: string,
): ValidatedTelemetryV2 | null {
  const topicBoardId = extractBoardIdFromBoardsTelemetryTopic(topic, prefix)
  if (topicBoardId === null) {
    console.warn(
      `[validate-v2] topic không khớp mẫu "${prefix}/boards/{boardId}/telemetry": "${topic}" — bỏ qua`,
    )
    return null
  }

  const raw = typeof rawPayload === "string" ? rawPayload : rawPayload.toString("utf8")

  let json: unknown
  try {
    json = JSON.parse(raw)
  } catch (error) {
    console.warn(
      `[validate-v2] JSON hỏng trên topic "${topic}": ${
        error instanceof Error ? error.message : String(error)
      } — payload: ${truncateForLog(raw)}`,
    )
    return null
  }

  const parsed = telemetryV2Schema.safeParse(json)
  if (!parsed.success) {
    console.warn(
      `[validate-v2] payload sai schema v2 trên topic "${topic}": ${JSON.stringify(parsed.error.issues)}` +
        ` — payload: ${truncateForLog(raw)}`,
    )
    return null
  }

  if (parsed.data.boardId !== topicBoardId) {
    console.warn(
      `[validate-v2] boardId lệch topic "${topic}": payload.boardId="${parsed.data.boardId}"` +
        ` — payload: ${truncateForLog(raw)}`,
    )
    return null
  }

  return { topic, boardId: topicBoardId, payload: parsed.data }
}

function truncateForLog(value: string, maxLength = MAX_LOGGED_PAYLOAD_LENGTH): string {
  return value.length <= maxLength ? value : `${value.slice(0, maxLength)}...(truncated)`
}
