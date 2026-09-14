/**
 * OpenCode v3 — orchestrator state plugin.
 *
 * Tracks run_id / task_key / session IDs for orchestrator workflows and writes
 * checkpoint state to .opencode/state/<run_id>.json (schema_version 3).
 *
 * Contract (see AGENTS.md — "OpenCode v3"):
 * - The orchestrator embeds "[<run_id>/<task_key>]" in the Task tool `description`.
 *   The marker ends up in the child session title, which is what recovery matches on.
 * - tool.execute.before (task): records dispatching + call_id + attempt; injects the
 *   marker when missing; blocks duplicate dispatch and a second coder on the worktree.
 * - session.created / message.part.updated: captures the child session ID as soon as
 *   metadata is available (Task tool metadata carries `sessionId`), never waiting
 *   for the worker to finish.
 * - tool.execute.after (task): records the result.
 * - On load: reconciles state files against known sessions to recover missing IDs.
 *   Anything ambiguous becomes "blocked" — this plugin never guesses IDs.
 *
 * The state file is a pointer to OpenCode session data, not a replacement for
 * PLAN.md / PROJECT_MEMORY.md and not a backup: copying the JSON to another
 * machine does not carry the conversations.
 */
import { randomUUID } from "node:crypto"
import { mkdir, readdir, readFile, rename, writeFile } from "node:fs/promises"
import path from "node:path"
import type { Hooks, Plugin } from "@opencode-ai/plugin"

type Json = Record<string, unknown>

interface HistoryEntry {
  at: string
  type: string
  [key: string]: unknown
}

type TaskStatus =
  | "pending"
  | "dispatching"
  | "running"
  | "completed"
  | "error"
  | "interrupted"
  | "unknown"
  | "blocked"
  | "cancelled"

type RunPhase =
  | "pending"
  | "planning"
  | "implementing"
  | "reviewing"
  | "testing"
  | "done"
  | "blocked"
  | "interrupted"
  | "cancelled"

interface TaskState {
  session_id: string | null
  call_id: string | null
  status: TaskStatus
  attempt: number
  history: HistoryEntry[]
  agent?: string
  description?: string
  result?: { status: string; summary?: string; error?: string } | null
}

interface RunState {
  schema_version: number
  run_id: string
  project_id: string
  worktree: string
  main_session_id: string | null
  phase: RunPhase
  next_action: string | null
  updated_at: string | null
  tasks: Record<string, TaskState>
}

interface SessionInfo {
  id: string
  parentID?: string
  title?: string
  directory?: string
}

const SCHEMA_VERSION = 3
const TASK_TOOL = "task"
const UUID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\.json$/i
const MARKER_RE = /\[([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})\/([A-Za-z0-9._:-]{1,64})\]/i
const ACTIVE_STATUSES = new Set(["dispatching", "running"])
const WRITER_AGENTS = new Set(["coder"])
const PHASE_BY_AGENT: Record<string, RunPhase> = {
  coder: "implementing",
  reviewer: "reviewing",
  tester: "testing",
}

type BeforeHook = NonNullable<Hooks["tool.execute.before"]>
type AfterHook = NonNullable<Hooks["tool.execute.after"]>
type EventHook = NonNullable<Hooks["event"]>

const now = () => new Date().toISOString()

function truncate(text: unknown, max = 2000): string {
  const value = typeof text === "string" ? text.trim() : ""
  return value.length <= max ? value : `${value.slice(0, max)}…`
}

function parseMarker(text: unknown): { run_id: string; task_key: string } | null {
  const match = MARKER_RE.exec(typeof text === "string" ? text : "")
  if (!match) return null
  return { run_id: match[1].toLowerCase(), task_key: match[2] }
}

function agentFromTitle(title: string): string | undefined {
  const match = /\(@([A-Za-z0-9_-]+) subagent\)/.exec(title)
  return match?.[1]
}

function pidAlive(pid: unknown): boolean {
  if (typeof pid !== "number" || !Number.isInteger(pid) || pid <= 0) return false
  try {
    process.kill(pid, 0)
    return true
  } catch (error) {
    // EPERM means the process exists but is owned by someone else.
    return (error as NodeJS.ErrnoException).code === "EPERM"
  }
}

function withinWorktree(candidate: string, worktree: string): boolean {
  try {
    const resolved = path.resolve(candidate)
    const root = path.resolve(worktree)
    return resolved === root || resolved.startsWith(`${root}${path.sep}`)
  } catch {
    return false
  }
}

export const OrchestratorState: Plugin = async ({ project, worktree, client }) => {
  const stateDir = path.join(worktree, ".opencode", "state")
  const lockDir = path.join(stateDir, "locks")
  const projectId = (project as { id?: string } | undefined)?.id ?? ""

  const log = async (message: string, extra?: Json): Promise<void> => {
    try {
      await (client as { app?: { log?: (input: unknown) => Promise<unknown> } }).app?.log?.({
        body: { service: "orchestrator-state", level: "info", message, extra },
      })
    } catch {
      /* logging is best-effort */
    }
  }

  // callID -> run/task. Only lives as long as this process; events rebuild it.
  const calls = new Map<string, { run_id: string; task_key: string }>()

  // Serialize state mutations per run so read-modify-write stays safe in-process.
  const queues = new Map<string, Promise<unknown>>()
  const serialize = <T>(runId: string, fn: () => Promise<T>): Promise<T> => {
    const tail = queues.get(runId) ?? Promise.resolve()
    const next = tail.then(fn, fn)
    queues.set(runId, next.then(() => undefined, () => undefined))
    return next
  }

  const stateFile = (runId: string) => path.join(stateDir, `${runId}.json`)
  const lockFile = (runId: string) => path.join(lockDir, `${runId}.json`)

  async function listRuns(): Promise<string[]> {
    try {
      const entries = await readdir(stateDir)
      return entries.filter((entry) => UUID_RE.test(entry)).map((entry) => entry.slice(0, -5))
    } catch {
      return []
    }
  }

  async function readRun(runId: string): Promise<RunState | null> {
    try {
      const raw = JSON.parse(await readFile(stateFile(runId), "utf8")) as RunState
      return raw && raw.schema_version === SCHEMA_VERSION ? raw : null
    } catch {
      return null
    }
  }

  async function writeRun(run: RunState): Promise<void> {
    // One writer per file: temp + atomic rename, per-process temp names.
    run.updated_at = now()
    await mkdir(stateDir, { recursive: true })
    const tmp = `${stateFile(run.run_id)}.${process.pid}.tmp`
    await writeFile(tmp, `${JSON.stringify(run, null, 2)}\n`)
    await rename(tmp, stateFile(run.run_id))
  }

  function newRun(runId: string, mainSessionId: string): RunState {
    return {
      schema_version: SCHEMA_VERSION,
      run_id: runId,
      project_id: projectId,
      worktree,
      main_session_id: mainSessionId,
      phase: "pending",
      next_action: null,
      updated_at: now(),
      tasks: {},
    }
  }

  function newTask(agent: string, description: string): TaskState {
    return { session_id: null, call_id: null, status: "pending", attempt: 0, history: [], agent, description }
  }

  async function runLockedByLiveProcess(runId: string): Promise<boolean> {
    try {
      const raw = JSON.parse(await readFile(lockFile(runId), "utf8")) as { pid?: unknown }
      return pidAlive(raw?.pid)
    } catch {
      return false
    }
  }

  async function touchRunLock(runId: string): Promise<void> {
    try {
      await mkdir(lockDir, { recursive: true })
      await writeFile(lockFile(runId), `${JSON.stringify({ pid: process.pid, at: now() })}\n`)
    } catch {
      /* best effort */
    }
  }

  async function findRunByMainSession(sessionId: string): Promise<RunState | null> {
    let fallback: RunState | null = null
    for (const runId of await listRuns()) {
      const run = await readRun(runId)
      if (!run || run.main_session_id !== sessionId) continue
      if (run.phase !== "done" && run.phase !== "blocked" && run.phase !== "cancelled") return run
      fallback = fallback ?? run
    }
    return fallback
  }

  // Single-writer rule: only one coder may be active on this worktree at a time.
  async function findActiveWriter(currentRunId: string, currentTaskKey: string): Promise<string | null> {
    for (const runId of await listRuns()) {
      const live = runId === currentRunId || (await runLockedByLiveProcess(runId))
      if (!live) continue
      const run = await readRun(runId)
      if (!run) continue
      for (const [key, task] of Object.entries(run.tasks)) {
        if (runId === currentRunId && key === currentTaskKey) continue
        if (task.agent && WRITER_AGENTS.has(task.agent) && ACTIVE_STATUSES.has(task.status)) {
          return `${runId}/${key}`
        }
      }
    }
    return null
  }

  async function fetchSessions(): Promise<SessionInfo[]> {
    try {
      const response = (await (client as { session?: { list?: () => Promise<unknown> } }).session?.list?.()) as
        | { data?: unknown }
        | SessionInfo[]
        | undefined
      const data = Array.isArray(response) ? response : response?.data
      return Array.isArray(data) ? (data as SessionInfo[]) : []
    } catch {
      return []
    }
  }

  const onTaskBefore: BeforeHook = async (input, output) => {
    if (input.tool !== TASK_TOOL) return
    const args = (output?.args ?? {}) as { description?: unknown; subagent_type?: unknown; task_id?: unknown }
    const agent = typeof args.subagent_type === "string" ? args.subagent_type : "unknown"
    let description = typeof args.description === "string" ? args.description : ""

    // The orchestrator is supposed to embed [<run_id>/<task_key>]. If missing,
    // bind this dispatch to the run owned by this main session (creating one if
    // needed) and inject the marker so the child session title carries it.
    let marker = parseMarker(description)
    if (!marker) {
      const run = await findRunByMainSession(input.sessionID)
      const runId = run?.run_id ?? randomUUID()
      const seq = Object.keys(run?.tasks ?? {}).length + 1
      marker = { run_id: runId, task_key: `${agent}:t${seq}` }
      description = `${description} [${runId}/${marker.task_key}]`.trim()
      if (output?.args) output.args.description = description
    }
    const runId = marker.run_id
    const taskKey = marker.task_key
    calls.set(input.callID, { run_id: runId, task_key: taskKey })

    const resuming = typeof args.task_id === "string" && args.task_id.length > 0
    const taskId = typeof args.task_id === "string" ? args.task_id : null
    let blockReason: string | null = null

    try {
      await serialize(runId, async () => {
        let run = await readRun(runId)
        if (run && run.worktree !== worktree) {
          blockReason = `run ${runId} thuộc worktree ${run.worktree}; không dispatch từ ${worktree}`
          return
        }
        if (!run) run = newRun(runId, input.sessionID)
        if (!run.main_session_id) run.main_session_id = input.sessionID

        const task = run.tasks[taskKey] ?? newTask(agent, description)
        // Duplicate-dispatch guard: an active task may only be re-dispatched by
        // resuming its own session (task_id), never by spawning a second worker.
        const priorStatus = task.status
        if (ACTIVE_STATUSES.has(priorStatus) && !(resuming && taskId === task.session_id)) {
          task.status = "blocked"
          task.history.push({ at: now(), type: "blocked_dispatch", call_id: input.callID, reason: `task đang ${priorStatus}` })
          run.tasks[taskKey] = task
          run.phase = "blocked"
          run.next_action = `Task ${taskKey} đang ${priorStatus} (session ${task.session_id ?? "?"}); chờ hoặc resume bằng task_id đúng`
          await writeRun(run)
          blockReason = `dispatch trùng bị chặn: task ${taskKey} đang ${priorStatus} — resume bằng task_id=${task.session_id ?? "?"} hoặc dùng task_key mới`
          return
        }
        if (WRITER_AGENTS.has(agent)) {
          const writer = await findActiveWriter(runId, taskKey)
          if (writer) {
            run.phase = "blocked"
            run.next_action = `Đã có coder đang ghi (${writer}); chỉ một coder mỗi worktree`
            await writeRun(run)
            blockReason = `single-writer: ${writer} đang active; không dispatch coder mới`
            return
          }
        }

        task.agent = agent
        task.description = description
        task.attempt += 1
        task.call_id = input.callID
        task.status = resuming ? "running" : "dispatching"
        task.history.push({ at: now(), type: "dispatch", call_id: input.callID, attempt: task.attempt, agent, resumed: resuming })
        if (resuming && taskId) {
          // The Task tool continues exactly this child session (task_id == child session ID).
          if (task.session_id && task.session_id !== taskId) {
            task.history.push({ at: now(), type: "session_replaced", old_session_id: task.session_id, new_session_id: taskId, reason: "task_id khác state" })
          } else if (!task.session_id) {
            task.history.push({ at: now(), type: "session", session_id: taskId })
          }
          task.session_id = taskId
        }
        run.tasks[taskKey] = task
        if (PHASE_BY_AGENT[agent]) run.phase = PHASE_BY_AGENT[agent]
        run.next_action = `Chờ ${taskKey} (${agent})`
        await writeRun(run)
      })
    } catch (error) {
      if (blockReason) throw new Error(`[orchestrator-state] ${blockReason}`)
      await log(`dispatch tracking failed for ${taskKey}: ${String(error)}`)
      return
    }
    if (blockReason) throw new Error(`[orchestrator-state] ${blockReason}`)
    await touchRunLock(runId)
  }

  async function attachChildSession(parentID: string, childID: string, title: string): Promise<void> {
    for (const runId of await listRuns()) {
      const run = await readRun(runId)
      if (!run || run.main_session_id !== parentID) continue
      // Child title = description + " (@agent subagent)" — the marker is inside.
      let targetKey: string | null = null
      const marker = parseMarker(title)
      if (marker && marker.run_id === runId) targetKey = marker.task_key
      if (!targetKey) {
        const pending = Object.entries(run.tasks).filter(([, task]) => task.status === "dispatching" && !task.session_id)
        if (pending.length === 1) targetKey = pending[0][0]
      }
      if (!targetKey) continue
      const key = targetKey
      await serialize(runId, async () => {
        const fresh = await readRun(runId)
        if (!fresh) return
        const task = fresh.tasks[key] ?? newTask(agentFromTitle(title) ?? "unknown", title)
        if (task.session_id === childID) return
        task.history.push(
          task.session_id
            ? { at: now(), type: "session_replaced", old_session_id: task.session_id, new_session_id: childID, reason: "session.created" }
            : { at: now(), type: "session", session_id: childID },
        )
        task.session_id = childID
        if (task.status === "dispatching") task.status = "running"
        fresh.tasks[key] = task
        await writeRun(fresh)
      })
      return
    }
  }

  async function onTaskPart(part: {
    callID?: string
    state?: { status?: string; input?: { description?: unknown } | undefined; metadata?: { sessionId?: string; session_id?: string } | undefined; error?: unknown }
    metadata?: { sessionId?: string; session_id?: string } | undefined
  }): Promise<void> {
    const callID = part.callID
    if (!callID) return
    const state = part.state ?? {}
    const metadata = state.metadata ?? part.metadata ?? {}
    const childID = metadata.sessionId ?? metadata.session_id
    const marker = calls.get(callID) ?? parseMarker(state.input?.description)
    if (!marker) return
    calls.set(callID, marker)
    const runId = marker.run_id
    const taskKey = marker.task_key

    await serialize(runId, async () => {
      const run = await readRun(runId)
      if (!run) return
      const task = run.tasks[taskKey]
      if (!task) return
      if (childID && task.session_id !== childID) {
        task.history.push(
          task.session_id
            ? { at: now(), type: "session_replaced", old_session_id: task.session_id, new_session_id: childID, reason: "tool part metadata" }
            : { at: now(), type: "session", session_id: childID },
        )
        task.session_id = childID
      }
      if (childID && task.status === "dispatching") task.status = "running"
      if (state.status === "error") {
        task.status = "error"
        task.result = { status: "error", error: truncate(state.error) }
        task.history.push({ at: now(), type: "error", call_id: callID, error: truncate(state.error, 500) })
        run.next_action = `Task ${taskKey} lỗi — orchestrator kiểm tra; tối đa 3 vòng sửa rồi blocked`
      }
      await writeRun(run)
    })
  }

  const onEvent: EventHook = async (input) => {
    try {
      const event = (input?.event ?? {}) as { type?: string; properties?: { info?: SessionInfo; part?: unknown } }
      const properties = event.properties ?? {}
      if (event.type === "session.created") {
        const session = properties.info
        if (!session?.id || !session.parentID) return
        if (session.directory && !withinWorktree(session.directory, worktree)) return
        await attachChildSession(session.parentID, session.id, String(session.title ?? ""))
      } else if (event.type === "message.part.updated") {
        const part = properties.part as
          | { type?: string; tool?: string; callID?: string; state?: unknown; metadata?: unknown }
          | undefined
        if (!part || part.type !== "tool" || part.tool !== TASK_TOOL) return
        await onTaskPart(part as Parameters<typeof onTaskPart>[0])
      }
    } catch (error) {
      await log(`event handling failed: ${String(error)}`)
    }
  }

  const onTaskAfter: AfterHook = async (input, output) => {
    if (input.tool !== TASK_TOOL) return
    const marker = calls.get(input.callID)
    if (!marker) return
    const metadata = (output?.metadata ?? {}) as { sessionId?: string; session_id?: string }
    const childID = metadata.sessionId ?? metadata.session_id
    const runId = marker.run_id
    const taskKey = marker.task_key

    await serialize(runId, async () => {
      const run = await readRun(runId)
      if (!run) return
      const task = run.tasks[taskKey]
      if (!task) return
      if (childID && task.session_id !== childID) {
        task.history.push(
          task.session_id
            ? { at: now(), type: "session_replaced", old_session_id: task.session_id, new_session_id: childID, reason: "tool result" }
            : { at: now(), type: "session", session_id: childID },
        )
        task.session_id = childID
      }
      task.call_id = input.callID
      // Failures arrive via message.part.updated (tool part state "error"); never downgrade them.
      if (task.status !== "error") {
        task.status = "completed"
        task.result = { status: "completed", summary: truncate(output?.output) }
      }
      task.history.push({ at: now(), type: "result", status: task.status, call_id: input.callID })
      run.next_action = `Orchestrator xác minh kết quả ${taskKey} (chạy lại lệnh kiểm tra) trước khi chốt done`
      await writeRun(run)
    })
  }

  // Startup recovery: cross-check state files against sessions to restore missing
  // IDs. Unique match -> recovered; ambiguous -> blocked (never guessed).
  async function reconcile(): Promise<void> {
    const runIds = await listRuns()
    if (runIds.length === 0) return
    const sessions = await fetchSessions()
    const local = sessions.filter((session) => !session.directory || withinWorktree(session.directory, worktree))

    for (const runId of runIds) {
      // A live lock means another opencode process owns this workflow — leave it alone.
      if (await runLockedByLiveProcess(runId)) continue
      await serialize(runId, async () => {
        const run = await readRun(runId)
        if (!run) return
        let dirty = false
        const interrupted: string[] = []

        if (!run.main_session_id) {
          const parents = [
            ...new Set(
              local
                .filter((session) => session.parentID && String(session.title ?? "").toLowerCase().includes(runId))
                .map((session) => session.parentID as string),
            ),
          ]
          if (parents.length === 1) {
            run.main_session_id = parents[0]
            dirty = true
          } else if (parents.length > 1) {
            run.phase = "blocked"
            run.next_action = "main_session_id không xác định duy nhất — chọn tay qua `opencode session list --format json`"
            dirty = true
          }
        }

        if (run.main_session_id) {
          const children = local.filter((session) => session.parentID === run.main_session_id)
          for (const [key, task] of Object.entries(run.tasks)) {
            if (task.session_id || task.attempt === 0) continue
            const matches = children.filter((session) => {
              const title = String(session.title ?? "")
              return title.includes(`/${key}]`) || title.includes(key)
            })
            if (matches.length === 1) {
              task.session_id = String(matches[0].id)
              if (ACTIVE_STATUSES.has(task.status)) {
                task.status = "interrupted"
                interrupted.push(key)
              }
              task.history.push({ at: now(), type: "recovered_session", session_id: task.session_id })
              dirty = true
            } else if (matches.length > 1) {
              task.status = "unknown"
              task.history.push({ at: now(), type: "ambiguous_session", candidates: matches.map((session) => String(session.id)) })
              run.phase = "blocked"
              run.next_action = `Task ${key} có nhiều session ứng viên — đối soát tay, không đoán`
              dirty = true
            }
          }
        }

        for (const [key, task] of Object.entries(run.tasks)) {
          if (ACTIVE_STATUSES.has(task.status)) {
            task.status = "interrupted"
            task.history.push({ at: now(), type: "interrupted", reason: "opencode restart" })
            interrupted.push(key)
            dirty = true
          }
        }

        if (interrupted.length > 0) {
          run.phase = "interrupted"
          run.next_action = `Xác minh ${interrupted.join(", ")} bằng git diff + session messages trước khi dispatch lại; không thả coder mới khi writer cũ chưa xác nhận dừng`
          dirty = true
        }
        if (dirty) await writeRun(run)
      })
    }
  }

  try {
    setTimeout(() => reconcile().catch((e) => log(String(e))), 3000)
  } catch (error) {
    await log(`reconciliation failed: ${String(error)}`)
  }
  await log(`orchestrator-state loaded (worktree=${worktree})`)

  return {
    event: onEvent,
    "tool.execute.before": onTaskBefore,
    "tool.execute.after": onTaskAfter,
  }
}
