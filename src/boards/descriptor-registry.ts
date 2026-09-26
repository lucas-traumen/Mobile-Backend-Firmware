// M14a — Descriptor registry: cache boardId → descriptor từ retained message
// `{prefix}/boards/{boardId}/descriptor`. Firmware v2 publish descriptor mỗi lần
// connect (retained) nên registry nhận ngay sau subscribe; khi MqttService
// resubscribe sau reconnect, broker bắn lại retained → registry tự nạp lại.
//
// Registry chạy trên CÙNG connection của MqttService (route bổ sung — descriptor
// và telemetry v2 dùng chung một MQTT connection). Sai shape /
// boardId lệch topic → WARN + bỏ, không crash.
import { boardDescriptorSchema, type BoardDescriptor } from "./descriptor.js"

const LOG_PREFIX = "[boards-registry]"

const MAX_LOGGED_PAYLOAD_LENGTH = 300

export interface DescriptorRegistryOptions {
  /** Prefix contract boards — từ env TOPIC_PREFIX. */
  prefix: string
}

function escapeRegExp(value: string): string {
  return value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")
}

export class DescriptorRegistry {
  private readonly descriptors = new Map<string, BoardDescriptor>()
  private readonly pattern: RegExp

  /** Filter subscribe descriptor — MqttService subscribe trong event 'connect' (resubscribe khi reconnect). */
  readonly topicFilter: string

  constructor(options: DescriptorRegistryOptions) {
    this.pattern = new RegExp(
      `^${escapeRegExp(options.prefix)}\\/boards\\/([^/]+)\\/descriptor$`,
    )
    this.topicFilter = `${options.prefix}/boards/+/descriptor`
  }

  /** Số board đang có descriptor trong cache (quan sát/tests). */
  get size(): number {
    return this.descriptors.size
  }

  /** Descriptor mới nhất của board, null nếu chưa nhận lần nào (cold-start). */
  lookup(boardId: string): BoardDescriptor | null {
    return this.descriptors.get(boardId) ?? null
  }

  /**
   * Route handler cho MqttService (MqttExtraRoute): trả true nếu topic thuộc
   * contract descriptor — message được xử lý KỂ CẢ khi sai shape (WARN + bỏ).
   * Topic khác → false.
   */
  handleMessage(topic: string, payload: Buffer): boolean {
    const match = this.pattern.exec(topic)
    if (match === null) return false
    this.applyMessage(topic, match[1], payload)
    return true
  }

  /** Áp một message descriptor; topicBoardId lấy từ segment topic (nguồn tin cậy). */
  private applyMessage(topic: string, topicBoardId: string, rawPayload: Buffer): void {
    const raw = rawPayload.toString("utf8")

    let json: unknown
    try {
      json = JSON.parse(raw)
    } catch (error) {
      console.warn(
        `${LOG_PREFIX} JSON hỏng trên topic "${topic}": ${
          error instanceof Error ? error.message : String(error)
        } — payload: ${truncateForLog(raw)} — bỏ descriptor`,
      )
      return
    }

    const parsed = boardDescriptorSchema.safeParse(json)
    if (!parsed.success) {
      console.warn(
        `${LOG_PREFIX} descriptor sai shape trên topic "${topic}": ${JSON.stringify(parsed.error.issues)}` +
          ` — payload: ${truncateForLog(raw)} — bỏ descriptor`,
      )
      return
    }

    if (parsed.data.boardId !== topicBoardId) {
      console.warn(
        `${LOG_PREFIX} descriptor boardId "${parsed.data.boardId}" lệch segment topic "${topicBoardId}"` +
          ` trên topic "${topic}" — bỏ descriptor`,
      )
      return
    }

    // Message mới nhất ghi đè bản cũ — "cache theo message mới nhất".
    this.descriptors.set(topicBoardId, parsed.data)
    console.log(
      `${LOG_PREFIX} đã nạp descriptor board "${topicBoardId}" (type "${parsed.data.boardType}",` +
        ` ${parsed.data.sensors.length} sensor, ${parsed.data.relays.length} relay)`,
    )
  }
}

function truncateForLog(value: string, maxLength = MAX_LOGGED_PAYLOAD_LENGTH): string {
  return value.length <= maxLength ? value : `${value.slice(0, maxLength)}...(truncated)`
}
