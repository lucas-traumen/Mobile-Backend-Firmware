import { Point, type WriteApi } from "@influxdata/influxdb-client"
import type { TelemetryPayload } from "../telemetry/schema.js"

// Chính sách queue (quyết định trong PLAN.md):
// - Hàng đợi tối đa MAX_QUEUE_SIZE điểm; đầy → drop điểm cũ nhất + WARN.
// - Ghi lỗi → retry với backoff nhân đôi, bắt đầu RETRY_INITIAL_DELAY_MS, trần MAX_RETRY_DELAY_MS.
// - Ghi thành công trở lại (write mới hoặc retry) → xả toàn bộ queue.
// - close(): xả queue best-effort (một lượt, không backoff) rồi dispose writeApi.
export const MAX_QUEUE_SIZE = 1000
export const RETRY_INITIAL_DELAY_MS = 500
export const MAX_RETRY_DELAY_MS = 30_000

/**
 * Phần WriteApi mà writer cần — để test có thể mock structurally
 * (WriteApi thật của @influxdata/influxdb-client thỏa interface này).
 */
export type WritableWriteApi = Pick<WriteApi, "writePoint" | "flush" | "dispose">

/** Dạng dữ liệu trung gian trước khi dựng Point của client. */
export interface TelemetryPointData {
  measurement: "sensors"
  // M11: frontend query Flux trực tiếp InfluxDB local, mong đợi measurement
  // `sensors` + tag `roomId` (mỗi phòng một ESP, deviceId ≡ roomId).
  tags: { roomId: string }
  fields: { temperature: number; humidity: number }
  timestamp: Date
}

export function toInfluxPoint(data: TelemetryPointData): Point {
  return new Point(data.measurement)
    .tag("roomId", data.tags.roomId)
    .floatField("temperature", data.fields.temperature)
    .floatField("humidity", data.fields.humidity)
    .timestamp(data.timestamp)
}

export class InfluxWriter {
  private writeApi: WritableWriteApi
  private queue: TelemetryPointData[] = []
  private retryDelayMs = RETRY_INITIAL_DELAY_MS
  private retryTimer: NodeJS.Timeout | null = null
  private drainPromise: Promise<void> | null = null
  private pumping = false
  private closed = false
  private failedSinceLastSuccess = false

  // Inject qua factory để test thay writeApi giả; production truyền
  // () => influxDB.getWriteApi(org, bucket, "ms", { flushInterval: 0, maxRetries: 0 }).
  constructor(createWriteApi: () => WritableWriteApi) {
    this.writeApi = createWriteApi()
  }

  /** Số điểm đang chờ ghi lại trong queue (quan sát/tests). */
  get queuedCount(): number {
    return this.queue.length
  }

  /** Delay backoff sẽ dùng cho lần retry kế tiếp (quan sát/tests). */
  get nextRetryDelayMs(): number {
    return this.retryDelayMs
  }

  /**
   * Nhận một payload đã validate và xếp vào hàng ghi.
   * Không bao giờ throw; timestamp = thời điểm nhận (UTC, Date.now()).
   */
  write(payload: TelemetryPayload, receivedAt: Date = new Date()): void {
    if (this.closed) {
      console.warn("[influx-writer] write() sau khi close() — bỏ qua")
      return
    }
    this.enqueue({
      measurement: "sensors",
      tags: { roomId: payload.roomId },
      fields: { temperature: payload.temperature, humidity: payload.humidity },
      timestamp: receivedAt,
    })
    void this.pump()
  }

  /** Graceful shutdown: xả queue best-effort rồi dispose writeApi. */
  async close(): Promise<void> {
    this.closed = true
    this.clearRetryTimer()
    // Nếu pump đang chạy: chờ lượt đó xong (thành công hay thất bại) trước khi thử lần cuối.
    if (this.drainPromise !== null) {
      try {
        await this.drainPromise
      } catch {
        // Lỗi của lượt pump đã được log ở pump; close sẽ thử lại bên dưới.
      }
    }
    try {
      await this.drainQueue()
    } catch (error) {
      console.error(
        `[influx-writer] close: còn ${this.queue.length} điểm chưa ghi được, bỏ qua: ${describeError(error)}`,
      )
    }
    const unsent = this.writeApi.dispose()
    console.log(`[influx-writer] closed (queue còn lại: ${this.queue.length}, writeApi chưa ghi: ${unsent})`)
  }

  private enqueue(point: TelemetryPointData): void {
    while (this.queue.length >= MAX_QUEUE_SIZE) {
      const dropped = this.queue.shift()
      console.warn(
        `[influx-writer] queue full, dropped oldest point (roomId=${dropped?.tags.roomId ?? "?"})`,
      )
    }
    this.queue.push(point)
  }

  /** Kick một lượt xả queue; chạy đơn luồng nhờ cờ pumping. */
  private async pump(): Promise<void> {
    if (this.pumping || this.closed) return
    this.pumping = true
    try {
      await this.drainQueue()
      // Thành công: reset backoff cho đợt lỗi sau (nếu có).
      this.retryDelayMs = RETRY_INITIAL_DELAY_MS
      this.clearRetryTimer()
      if (this.failedSinceLastSuccess) {
        this.failedSinceLastSuccess = false
        console.log(`[influx-writer] ghi lại được vào InfluxDB, queue đã xả hết`)
      }
    } catch (error) {
      this.failedSinceLastSuccess = true
      const delay = this.nextRetryDelayMs
      console.error(`[influx-writer] ghi lỗi, retry sau ${delay}ms: ${describeError(error)}`)
      this.scheduleRetry()
    } finally {
      this.pumping = false
    }
  }

  /**
   * Xả queue theo thứ tự vào lúc gọi: lần lượt writePoint + flush từng điểm.
   * Điểm chỉ bị lấy ra khỏi queue khi flush thành công — flush thất bại thì
   * client đã discard line khỏi buffer nội bộ (maxRetries: 0), nên re-queue
   * không gây ghi trùng.
   */
  private drainQueue(): Promise<void> {
    if (this.drainPromise === null) {
      this.drainPromise = this.drainLoop().finally(() => {
        this.drainPromise = null
      })
    }
    return this.drainPromise
  }

  private async drainLoop(): Promise<void> {
    while (this.queue.length > 0) {
      const data = this.queue[0]
      this.writeApi.writePoint(toInfluxPoint(data))
      await this.writeApi.flush()
      this.queue.shift()
    }
  }

  private scheduleRetry(): void {
    this.clearRetryTimer()
    const delay = Math.min(this.retryDelayMs, MAX_RETRY_DELAY_MS)
    this.retryDelayMs = Math.min(this.retryDelayMs * 2, MAX_RETRY_DELAY_MS)
    this.retryTimer = setTimeout(() => {
      this.retryTimer = null
      void this.pump()
    }, delay)
  }

  private clearRetryTimer(): void {
    if (this.retryTimer !== null) {
      clearTimeout(this.retryTimer)
      this.retryTimer = null
    }
  }
}

function describeError(error: unknown): string {
  return error instanceof Error ? error.message : String(error)
}
