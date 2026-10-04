// Fake ACP server used by acp_client_test and kiro_adapter_test.
//
// Reads line-delimited JSON-RPC messages from stdin and writes responses to
// stdout. Behaviour is driven by the JSON-RPC "method" field and a
// `__FAKE_ACP_CMD__ <command>` token embedded in prompt text:
//
//   stream-and-finish       → streams agent text, a tool call, and turn_end
//   permission-then-finish  → asks session/request_permission, awaits reply,
//                             then emits text + turn_end
//   crash                   → exits with code 2 mid-prompt
//   malformed-session       → returns an object missing sessionId for session/new
//   plain                   → returns an immediate stopReason

import { writeSync, closeSync } from 'node:fs';

let buffer = '';
process.stdin.on('data', (chunk) => {
  buffer += chunk.toString('utf8');
  let nl;
  while ((nl = buffer.indexOf('\n')) >= 0) {
    const line = buffer.slice(0, nl).replace(/\r$/, '');
    buffer = buffer.slice(nl + 1);
    if (line.trim()) handle(line);
  }
});
process.stdin.on('end', () => { if (buffer.trim()) handle(buffer); });

function send(obj) {
  writeSync(1, JSON.stringify(obj) + '\n');
}

function delay(ms) { return new Promise((res) => setTimeout(res, ms)); }

let sessionCounter = 0;
let cancelTurn;
let permissionCounter = 0;
const permissions = new Map();

function permission(sessionId, opaque = false, subject = false) {
  ++permissionCounter;
  const id = permissionCounter % 2 ? 'agent-permission-' + permissionCounter : permissionCounter;
  const options = opaque ? [
    { kind: 'allow_always', name: 'Allow everything', optionId: 'always/' + permissionCounter },
    { kind: 'reject_once', name: 'Allow-looking display name', optionId: 'deny/' + permissionCounter + ':opaque' },
    { kind: 'allow_once', name: 'Reject-looking display name', optionId: 'yes/' + permissionCounter + ':opaque' },
    { kind: 'reject_always', name: 'Reject everything', optionId: 'never/' + permissionCounter }
  ] : [
    { kind: 'allow_once', name: 'Allow', optionId: 'allow' },
    { kind: 'reject_once', name: 'Reject', optionId: 'reject' }
  ];
  const toolCall = { toolCallId: 'same-looking-tool', title: 'delete', kind: 'delete', status: 'pending' };
  const params = { sessionId, description: 'delete important file', options,
    ...(subject ? { title: 'Permission', subject: { type: 'tool_call', toolCall } } : { toolCall }) };
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => {
      permissions.delete(JSON.stringify(id));
      reject(new Error('Permission request was not answered: ' + id));
    }, 3000);
    permissions.set(JSON.stringify(id), { options, resolve, reject, timeout });
    send({ jsonrpc: '2.0', id, method: 'session/request_permission', params });
  });
}

function permissionResponse(msg) {
  const pending = permissions.get(JSON.stringify(msg.id));
  if (!pending) return;
  permissions.delete(JSON.stringify(msg.id));
  clearTimeout(pending.timeout);
  const outcome = msg.result?.outcome;
  if (outcome?.outcome === 'selected' &&
      pending.options.some(o => o.optionId === outcome.optionId)) {
    pending.resolve(outcome.optionId);
  } else if (outcome?.outcome === 'cancelled' && !('optionId' in outcome)) {
    pending.resolve('cancelled');
  } else pending.reject(new Error('Invalid permission response or non-exact optionId'));
}

function handle(line) {
  let msg;
  try { msg = JSON.parse(line); } catch { return; }
  if (Array.isArray(msg)) {
    for (const item of msg) handleLine(item);
  } else {
    handleLine(msg);
  }
}

async function handleLine(msg) {
  if (!msg || typeof msg !== 'object') return;
  if ('id' in msg && !('method' in msg)) {
    permissionResponse(msg);
  } else if ('id' in msg && 'method' in msg) {
    try {
      const result = await dispatch(msg.method, msg.params || {}, msg.id);
      if (result !== undefined) send({ jsonrpc: '2.0', id: msg.id, result });
    } catch (e) {
      send({ jsonrpc: '2.0', id: msg.id,
        error: { code: e?.code ?? -32603, message: e?.message ?? String(e) } });
    }
  } else if ('method' in msg) {
    dispatch(msg.method, msg.params || {}, null).catch(() => {});
  }
}

async function dispatch(method, params, id) {
  switch (method) {
    case 'session/cancel':
      if (cancelTurn) { cancelTurn(); cancelTurn = undefined; }
      return undefined;
    case 'initialize':
      return {
        protocolVersion: params.protocolVersion,
        capabilities: {
          session: { prompt: { image: {}, audio: {}, embeddedContext: {} } },
          authMethods: []
        },
        agentInfo: {
          name: 'fake-acp',
          title: 'Fake ACP Server',
          version: '1.0.0'
        }
      };
    case 'session/new':
      if (params.__test_malformed) return { notAValid: 'response' };
      sessionCounter += 1;
      return {
        sessionId: 'sess_' + sessionCounter,
        models: [{ modelId: 'fake-model-a' }, { modelId: 'fake-model-b' }],
        currentModelId: 'fake-model-a',
      };
    case 'session/set_model':
      return { ok: true };
    case 'session/set_mode':
      return { ok: true };
    case 'session/prompt': {
      const text = (params.prompt || []).map(b => b.text || '').join('\n');
      const cmd = (text.match(/__FAKE_ACP_CMD__\s+(\S+)/) || [])[1] || 'plain';
      return handlePromptCommand(cmd, params.sessionId);
    }
    default:
      throw Object.assign(new Error('Method not implemented: ' + method), { code: -32601 });
  }
}

function notification(method, params) {
  send({ jsonrpc: '2.0', method, params });
}

async function handlePromptCommand(cmd, sessionId) {
  switch (cmd) {
    case 'wait-for-cancel':
      await new Promise((resolve) => {
        cancelTurn = resolve;
        notification('session/update', {
          sessionId, update: { sessionUpdate: 'agent_message_chunk',
            content: { type: 'text', text: 'waiting' } } });
      });
      return { stopReason: 'cancelled' };
    case 'stream-and-finish': {
      notification('session/update', {
        sessionId, update: { sessionUpdate: 'agent_message_chunk',
          content: { type: 'text', text: 'hello ' } } });
      await delay(5);
      notification('session/update', {
        sessionId, update: { sessionUpdate: 'agent_message_chunk',
          content: { type: 'text', text: 'world' } } });
      notification('session/update', {
        sessionId, update: { sessionUpdate: 'tool_call',
          toolCall: { toolCallId: 'tc1', title: 'read_file', kind: 'read', status: 'in_progress' } } });
      await delay(5);
      notification('session/update', {
        sessionId, update: { sessionUpdate: 'tool_call_update',
          toolCallUpdate: { toolCallId: 'tc1', status: 'completed' } } });
      notification('session/update', {
        sessionId, update: { sessionUpdate: 'turn_end' } });
      return { stopReason: 'end_turn' };
    }
    case 'permission-then-finish':
    case 'permission-opaque':
    case 'permission-two':
    case 'permission-overlap':
    case 'permission-pending':
    case 'permission-exit':
    case 'permission-eof':
    case 'permission-wrong-session':
    case 'permission-subject': {
      if (cmd === 'permission-exit') setTimeout(() => process.exit(2), 80);
      if (cmd === 'permission-eof') setTimeout(() => closeSync(1), 80);
      const ask = () => permission(
        cmd === 'permission-wrong-session' ? 'wrong-session' : sessionId,
        cmd !== 'permission-then-finish', cmd === 'permission-subject');
      const choices = cmd === 'permission-overlap' ? await Promise.all([ask(), ask()])
        : [await ask()];
      if (cmd === 'permission-two') choices.push(await ask());
      notification('session/update', {
        sessionId, update: { sessionUpdate: 'agent_message_chunk',
          content: { type: 'text', text: 'session=' + sessionId + ';choice=' + choices.join(',') } } });
      notification('session/update', { sessionId, update: { sessionUpdate: 'turn_end' } });
      return { stopReason: choices.includes('cancelled') ? 'cancelled' : 'end_turn' };
    }
    case 'crash':
      await delay(5);
      process.exit(2);
      return { stopReason: 'end_turn' };
    case 'malformed-session':
      return { notAValid: 'response' };
    case 'plain':
    default:
      return { stopReason: 'end_turn' };
  }
}
