// M14a — Ingest đường telemetry v2: topic `{prefix}/boards/{boardId}/telemetry`
// (không retained), payload `{"schemaVersion":2,"boardId":"0","values":{"S1":28.5}}`.
//
// Map `values` → fields Influx DATA-DRIVEN theo `descriptor.sensors`
// (channel → field): không có nhánh if theo loại cảm biến — thêm kênh/cảm biến
// mới chỉ cần descriptor mới, logic map không đổi. Kênh trong `values` mà
// descriptor không khai báo → WARN + bỏ kênh; sau lọc hết kênh hợp lệ → bỏ point.
// Descriptor chưa có (cold-start race — board v2 mới connect, telemetry đến trước
// descriptor) → WARN + bỏ point, KHÔNG queue: chu kỳ telemetry 5 s kế tiếp tự ổn.
//
// Chạy trên CÙNG connection MqttService (route bổ sung) — descriptor và
// telemetry v2 dùng chung một MQTT connection của backend.
import type { TelemetryPointData } from "../influx/influx-writer.js"
import { validateTelemetryV2Message } from "../telemetry/validate-v2.js"
import type { BoardDescriptor } from "./descriptor.js"
import type { DescriptorRegistry } from "./descriptor-registry.js"

const LOG_PREFIX = "[boards-ingest]"

export interface BoardsTelemetryIngestOptions {
  /** Prefix contract boards — từ env TOPIC_PREFIX. */
  prefix: string
  /** Nguồn descriptor cho map kênh → field. */
  registry: DescriptorRegistry
  /** Điểm đã map — main dẫn vào InfluxWriter.writePointData. */
  onPoint: (point: TelemetryPointData) => void
}

/**
 * Map `values` theo descriptor: kênh đã khai báo → field (kênh sau ghi đè nếu
 * descriptor khai báo trùng kênh), kênh lạ bị lọc ra `unknownChannels`.
 */
export function mapValuesToFields(
  descriptor: BoardDescriptor,
  values: Record<string, number>,
): { fields: Record<string, number>; unknownChannels: string[] } {
  const declared = new Map<string, string>()
  for (const sensor of descriptor.sensors) {
    declared.set(sensor.channel, sensor.field)
  }

  const fields: Record<string, number> = {}
  const unknownChannels: string[] = []
  for (const [channel, value] of Object.entries(values)) {
    const field = declared.get(channel)
    if (field === undefined) {
      unknownChannels.push(channel)
      continue
    }
    fields[field] = value
  }
  return { fields, unknownChannels }
}

export class BoardsTelemetryIngest {
  private readonly prefix: string
  private readonly registry: DescriptorRegistry
  private readonly onPoint: (point: TelemetryPointData) => void
  private readonly pattern: RegExp

  /** Filter subscribe telemetry v2 — MqttService subscribe trong event 'connect'. */
  readonly topicFilter: string

  constructor(options: BoardsTelemetryIngestOptions) {
    this.prefix = options.prefix
    this.registry = options.registry
    this.onPoint = options.onPoint
    this.pattern = new RegExp(
      `^${escapeRegExp(options.prefix)}\\/boards\\/([^/]+)\\/telemetry$`,
    )
    this.topicFilter = `${options.prefix}/boards/+/telemetry`
  }

  /**
   * Route handler cho MqttService (MqttExtraRoute): trả true nếu topic thuộc
   * telemetry v2 — message đã được xử lý (kể cả bị bỏ sau WARN). Topic khác → false.
   */
  handleMessage(topic: string, payload: Buffer): boolean {
    const match = this.pattern.exec(topic)
    if (match === null) return false
    this.ingest(topic, payload, new Date())
    return true
  }

  /** receivedAt là tham số để test deterministic; production dùng thời điểm nhận. */
  ingest(topic: string, rawPayload: Buffer | string, receivedAt: Date): void {
    const validated = validateTelemetryV2Message(topic, rawPayload, this.prefix)
    if (validated === null) return

    const boardId = validated.payload.boardId // validate đã đảm bảo trùng segment topic

    const descriptor = this.registry.lookup(boardId)
    if (descriptor === null) {
      console.warn(
        `${LOG_PREFIX} board "${boardId}" chưa có descriptor (cold-start hoặc board chưa publish)` +
          ` — bỏ point này, chu kỳ telemetry kế tiếp tự ổn khi descriptor đã đến`,
      )
      return
    }

    const { fields, unknownChannels } = mapValuesToFields(descriptor, validated.payload.values)
    for (const channel of unknownChannels) {
      console.warn(
        `${LOG_PREFIX} board "${boardId}": kênh "${channel}" không có trong descriptor — bỏ kênh`,
      )
    }
    if (Object.keys(fields).length === 0) {
      console.warn(
        `${LOG_PREFIX} board "${boardId}": không còn kênh hợp lệ sau khi lọc theo descriptor — bỏ point`,
      )
      return
    }

    this.onPoint({
      measurement: "sensors",
      // Quyết định M14: roomId = boardId (quy ước 1:1) — history app cũ liên tục,
      // schema tag đồng nhất mọi điểm; bỏ khi app hết query theo roomId.
      tags: { roomId: boardId, boardId },
      fields,
      timestamp: receivedAt,
    })
  }
}

function escapeRegExp(value: string): string {
  return value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")
}
