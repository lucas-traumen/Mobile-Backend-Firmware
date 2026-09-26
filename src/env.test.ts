import { describe, expect, it } from "vitest"

import { BOARD_ID_PATTERN, DEFAULT_TOPIC_PREFIX } from "./env.js"

describe("env — hằng số contract boards", () => {
  it("DEFAULT_TOPIC_PREFIX = smarthome", () => {
    expect(DEFAULT_TOPIC_PREFIX).toBe("smarthome")
  })

  it("BOARD_ID_PATTERN: boardId hợp lệ cho segment topic — chữ/số/gạch ngang/gạch dưới", () => {
    expect(BOARD_ID_PATTERN.test("0")).toBe(true)
    expect(BOARD_ID_PATTERN.test("board-01_x")).toBe(true)
    expect(BOARD_ID_PATTERN.test("3b6baf6c")).toBe(true)
    expect(BOARD_ID_PATTERN.test("bad id")).toBe(false)
    expect(BOARD_ID_PATTERN.test("../etc")).toBe(false)
    expect(BOARD_ID_PATTERN.test("")).toBe(false)
  })
})
