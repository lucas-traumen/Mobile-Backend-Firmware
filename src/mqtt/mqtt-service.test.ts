import { EventEmitter } from "node:events"
import { beforeEach, describe, expect, it, vi } from "vitest"
import mqtt, { type MqttClient } from "mqtt"

import { MqttService, type MqttExtraRoute } from "./mqtt-service.js"

vi.mock("mqtt", () => ({
  default: { connect: vi.fn() },
}))

const connectMock = vi.mocked(mqtt.connect)

class FakeMqttClient extends EventEmitter {
  subscribe = vi.fn(
    (topic: string, _opts: { qos: number }, cb?: (err: Error | null) => void): unknown => {
      cb?.(null)
      return this
    },
  )
  end = vi.fn((_force: boolean, _opts: object, cb?: () => void): unknown => {
    cb?.()
    return this
  })
}

describe("MqttService", () => {
  let client: FakeMqttClient
  let warnSpy: ReturnType<typeof vi.spyOn>
  let errorSpy: ReturnType<typeof vi.spyOn>

  beforeEach(() => {
    vi.clearAllMocks()
    client = new FakeMqttClient()
    connectMock.mockReturnValue(client as unknown as MqttClient)
    warnSpy = vi.spyOn(console, "warn").mockImplementation(() => {})
    errorSpy = vi.spyOn(console, "error").mockImplementation(() => {})
  })

  function makeService(routes: MqttExtraRoute[] = []): MqttService {
    return new MqttService({
      url: "mqtt://broker.test:1883",
      username: "user1",
      password: "pass1",
      extraRoutes: routes,
    })
  }

  it("connect đúng url + credentials + tự reconnect (reconnectPeriod > 0)", async () => {
    const service = makeService()
    await service.start()

    expect(connectMock).toHaveBeenCalledWith(
      "mqtt://broker.test:1883",
      expect.objectContaining({
        username: "user1",
        password: "pass1",
        reconnectPeriod: expect.any(Number),
      }),
    )
    const options = connectMock.mock.calls[0][1]
    expect(options?.reconnectPeriod).toBeGreaterThan(0)
  })

  it("connect → subscribe filter của từng extraRoute (qos 1)", async () => {
    const service = makeService([
      { topicFilter: "smarthome/boards/+/descriptor", handleMessage: () => true },
      { topicFilter: "smarthome/boards/+/telemetry", handleMessage: () => true },
    ])
    await service.start()
    client.emit("connect")

    const subscribedFilters = client.subscribe.mock.calls.map((call) => call[0])
    expect(subscribedFilters).toEqual([
      "smarthome/boards/+/descriptor",
      "smarthome/boards/+/telemetry",
    ])
    expect(client.subscribe).toHaveBeenCalledWith(
      "smarthome/boards/+/descriptor",
      expect.objectContaining({ qos: 1 }),
      expect.any(Function),
    )
  })

  it("subscribe lỗi của một filter → log + không chặn filter còn lại", async () => {
    client.subscribe = vi.fn(
      (topic: string, _opts: { qos: number }, cb?: (err: Error | null) => void): unknown => {
        cb?.(topic.includes("telemetry") ? new Error("no permission") : null)
        return this
      },
    )
    const service = makeService([
      { topicFilter: "smarthome/boards/+/descriptor", handleMessage: () => true },
      { topicFilter: "smarthome/boards/+/telemetry", handleMessage: () => true },
    ])
    await service.start()
    client.emit("connect")

    expect(errorSpy).toHaveBeenCalledWith(expect.stringContaining("no permission"))
  })

  it("message thuộc route → route nhận đủ (topic, payload), route sau không được hỏi", async () => {
    const first = vi.fn(() => false)
    const second = vi.fn(() => true)
    const service = makeService([
      { topicFilter: "smarthome/boards/+/descriptor", handleMessage: first },
      { topicFilter: "smarthome/boards/+/telemetry", handleMessage: second },
    ])
    await service.start()
    client.emit("connect")

    const payload = Buffer.from(JSON.stringify({ schemaVersion: 2, boardId: "0", values: { S1: 1 } }))
    client.emit("message", "smarthome/boards/0/telemetry", payload)

    // route đầu tự trả false (topic không thuộc), route thứ hai nhận và chốt.
    expect(first).toHaveBeenCalledWith("smarthome/boards/0/telemetry", payload)
    expect(second).toHaveBeenCalledWith("smarthome/boards/0/telemetry", payload)
    expect(second).toHaveBeenCalledTimes(1)
  })

  it("topic lạ (không route nào nhận) → log + bỏ, không crash", async () => {
    const service = makeService([
      { topicFilter: "smarthome/boards/+/telemetry", handleMessage: () => false },
    ])
    await service.start()
    client.emit("connect")

    expect(() => client.emit("message", "smarthome/esp32-01/status", Buffer.from("online"))).not.toThrow()
    expect(warnSpy).toHaveBeenCalledWith(expect.stringContaining("không route nào nhận"))
  })

  it("route throw → bắt lại, không crash event loop", async () => {
    const service = makeService([
      {
        topicFilter: "smarthome/boards/+/telemetry",
        handleMessage: () => {
          throw new Error("route exploded")
        },
      },
    ])
    await service.start()
    client.emit("connect")

    expect(() =>
      client.emit("message", "smarthome/boards/0/telemetry", Buffer.from("{}")),
    ).not.toThrow()
    expect(errorSpy).toHaveBeenCalledWith(expect.stringContaining("route exploded"))
  })

  it("event 'error' → chỉ log, không crash", async () => {
    const service = makeService()
    await service.start()

    expect(() => client.emit("error", new Error("socket hang up"))).not.toThrow()
    expect(errorSpy).toHaveBeenCalledWith(expect.stringContaining("socket hang up"))
  })

  it("reconnect: 'connect' bắn lại → resubscribe extraRoutes (retained descriptor được bắn lại)", async () => {
    const service = makeService([
      { topicFilter: "smarthome/boards/+/descriptor", handleMessage: () => true },
      { topicFilter: "smarthome/boards/+/telemetry", handleMessage: () => true },
    ])
    await service.start()

    client.emit("connect") // lần nối đầu
    client.emit("close") // mất kết nối
    client.emit("connect") // reconnect thành công

    expect(client.subscribe).toHaveBeenCalledTimes(4) // 2 lần connect × 2 filter
    expect(client.subscribe).toHaveBeenLastCalledWith(
      "smarthome/boards/+/telemetry",
      expect.objectContaining({ qos: 1 }),
      expect.any(Function),
    )
  })

  it("start hai lần → không connect thêm client thứ hai", async () => {
    const service = makeService()
    await service.start()
    await service.start()
    expect(connectMock).toHaveBeenCalledTimes(1)
  })

  it("stop: end client, lần sau start nối lại client mới", async () => {
    const service = makeService()
    await service.start()
    await service.stop()
    expect(client.end).toHaveBeenCalledWith(false, {}, expect.any(Function))

    const second = new FakeMqttClient()
    connectMock.mockReturnValueOnce(second as unknown as MqttClient)
    await service.start()
    expect(connectMock).toHaveBeenCalledTimes(2)
  })
})
