import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises"
import { tmpdir } from "node:os"
import path from "node:path"
import { afterAll, beforeEach, describe, expect, it } from "vitest"
import { OrchestratorState } from "../.opencode/plugins/orchestrator-state.js"

type PluginHooks = Awaited<ReturnType<typeof OrchestratorState>>

const RUN_ID = "11111111-2222-3333-4444-555555555555"
const MARKER = `[${RUN_ID}/M1:coder]`

const worktrees: string[] = []
let worktree: string
const sessions: Array<{ id: string; parentID?: string; directory?: string; title?: string }> = []

async function loadPlugin(): Promise<PluginHooks> {
  const client = {
    session: { list: async () => ({ data: sessions }) },
    app: { log: async () => undefined },
  }
  return OrchestratorState({
    project: { id: "proj_test" },
    worktree,
    directory: worktree,
    client,
  } as unknown as Parameters<typeof OrchestratorState>[0])
}

async function readState(): Promise<any> {
  return JSON.parse(await readFile(path.join(worktree, ".opencode", "state", `${RUN_ID}.json`), "utf8"))
}

function taskCall(description: string, taskId?: string) {
  const input = {
    tool: "task",
    sessionID: "ses_MAIN",
    callID: `call_${Math.random().toString(36).slice(2, 8)}`,
  }
  const output = {
    args: {
      subagent_type: "coder",
      prompt: "do the work",
      description,
      ...(taskId ? { task_id: taskId } : {}),
    },
  }
  return { input, output }
}

async function deadenRunLock(): Promise<void> {
  const lock = path.join(worktree, ".opencode", "state", "locks", `${RUN_ID}.json`)
  await writeFile(lock, JSON.stringify({ pid: 999_999_999, at: "2026-09-05T00:00:00.000Z" }))
}

beforeEach(async () => {
  worktree = await mkdtemp(path.join(tmpdir(), "oc-state-"))
  worktrees.push(worktree)
  sessions.length = 0
})

afterAll(async () => {
  for (const dir of worktrees) await rm(dir, { recursive: true, force: true })
})

describe("orchestrator-state plugin", () => {
  it("records dispatch, captures the child session, and stores the result", async () => {
    const hooks = await loadPlugin()
    const call = taskCall(`Scaffold server ${MARKER}`)
    await hooks["tool.execute.before"]!(call.input, call.output)

    const dispatched = await readState()
    expect(dispatched.schema_version).toBe(3)
    expect(dispatched.main_session_id).toBe("ses_MAIN")
    expect(dispatched.phase).toBe("implementing")
    expect(dispatched.tasks["M1:coder"]).toMatchObject({
      status: "dispatching",
      attempt: 1,
      call_id: call.input.callID,
      session_id: null,
    })

    await hooks.event!({
      event: {
        type: "session.created",
        properties: {
          info: {
            id: "ses_CHILD",
            parentID: "ses_MAIN",
            directory: worktree,
            title: `Scaffold server ${MARKER} (@coder subagent)`,
          },
        },
      },
    } as never)

    const running = await readState()
    expect(running.tasks["M1:coder"]).toMatchObject({ status: "running", session_id: "ses_CHILD" })

    await hooks["tool.execute.after"]!(
      { ...call.input, args: call.output.args },
      { title: "Scaffold server", output: "<task>done</task>", metadata: { parentSessionId: "ses_MAIN", sessionId: "ses_CHILD" } },
    )

    const completed = await readState()
    expect(completed.tasks["M1:coder"]).toMatchObject({ status: "completed", session_id: "ses_CHILD" })
    expect(completed.tasks["M1:coder"].result.status).toBe("completed")
  })

  it("injects a marker when the description has none", async () => {
    const hooks = await loadPlugin()
    const call = taskCall("No marker here")
    await hooks["tool.execute.before"]!(call.input, call.output)
    expect(String(call.output.args.description)).toMatch(/\[[0-9a-f-]{36}\/coder:t1\]$/)
  })

  it("blocks duplicate dispatch, allows retry after completion, and resumes via task_id", async () => {
    const hooks = await loadPlugin()
    const first = taskCall(`Work ${MARKER}`)
    await hooks["tool.execute.before"]!(first.input, first.output)
    await hooks["tool.execute.after"]!(
      { ...first.input, args: first.output.args },
      { title: "Work", output: "ok", metadata: { parentSessionId: "ses_MAIN", sessionId: "ses_CHILD" } },
    )

    // Completed → a fresh dispatch of the same key is a retry, not a duplicate.
    const retry = taskCall(`Retry ${MARKER}`)
    await hooks["tool.execute.before"]!(retry.input, retry.output)
    expect((await readState()).tasks["M1:coder"].attempt).toBe(2)

    // Dispatching → a second parallel dispatch is blocked.
    const duplicate = taskCall(`Duplicate ${MARKER}`)
    await expect(hooks["tool.execute.before"]!(duplicate.input, duplicate.output)).rejects.toThrow(
      /dispatch trùng bị chặn/,
    )

    // Resuming the recorded session is always allowed.
    const resume = taskCall(`Resume ${MARKER}`, "ses_CHILD")
    await hooks["tool.execute.before"]!(resume.input, resume.output)
    expect((await readState()).tasks["M1:coder"]).toMatchObject({ status: "running", session_id: "ses_CHILD" })
  })

  it("enforces a single coder per worktree across task keys", async () => {
    const hooks = await loadPlugin()
    const first = taskCall(`A ${MARKER}`)
    await hooks["tool.execute.before"]!(first.input, first.output)

    const second = taskCall(`B [${RUN_ID}/M2:coder]`)
    await expect(hooks["tool.execute.before"]!(second.input, second.output)).rejects.toThrow(/single-writer/)

    const state = await readState()
    expect(state.phase).toBe("blocked")
    expect(state.next_action).toMatch(/chỉ một coder mỗi worktree/)
  })

  it("recovers a dead run's child session on restart and marks it interrupted", async () => {
    const hooks = await loadPlugin()
    const call = taskCall(`Work ${MARKER}`)
    await hooks["tool.execute.before"]!(call.input, call.output)
    await deadenRunLock()

    sessions.push({
      id: "ses_CHILD",
      parentID: "ses_MAIN",
      directory: worktree,
      title: `Work ${MARKER} (@coder subagent)`,
    })
    await loadPlugin()

    const state = await readState()
    expect(state.tasks["M1:coder"]).toMatchObject({ status: "interrupted", session_id: "ses_CHILD" })
    expect(state.phase).toBe("interrupted")
    expect(state.next_action).toMatch(/Xác minh M1:coder/)
  })

  it("never guesses when multiple child sessions match — marks the run blocked", async () => {
    const hooks = await loadPlugin()
    const call = taskCall(`Work ${MARKER}`)
    await hooks["tool.execute.before"]!(call.input, call.output)
    await deadenRunLock()

    sessions.push(
      {
        id: "ses_CHILD_A",
        parentID: "ses_MAIN",
        directory: worktree,
        title: `Work ${MARKER} (@coder subagent)`,
      },
      {
        id: "ses_CHILD_B",
        parentID: "ses_MAIN",
        directory: worktree,
        title: `Work ${MARKER} (@coder subagent)`,
      },
    )
    await loadPlugin()

    const state = await readState()
    expect(state.tasks["M1:coder"]).toMatchObject({ status: "unknown", session_id: null })
    expect(state.phase).toBe("blocked")
    expect(state.next_action).toMatch(/không đoán/)
  })
})
