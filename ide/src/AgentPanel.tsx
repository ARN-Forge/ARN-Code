import { useEffect, useRef, useState } from 'react';
import { invoke } from '@tauri-apps/api/core';
import { listen } from '@tauri-apps/api/event';
import './AgentPanel.css';

type Event = { type: string; session: string; requestId?: string; text?: string; id?: string; path?: string; operation?: string; before?: string; after?: string; summary?: string };
type Message = { role: string; content: string };
interface Props {
  projectRoot?: string;
  dirtyPaths: string[];
  busy: boolean;
  onBusyChange: (busy: boolean) => void;
  onFilesChanged: () => Promise<void>;
}
export function AgentPanel({ projectRoot, dirtyPaths, busy, onBusyChange, onFilesChanged }: Props) {
  const [collapsed, setCollapsed] = useState(false);
  const [messages, setMessages] = useState<Message[]>([]);
  const [input, setInput] = useState('');
  const [provider, setProvider] = useState('gemini');
  const [apiKey, setApiKey] = useState('');
  const [models, setModels] = useState<string[]>([]);
  const [model, setModel] = useState('');
  const [connected, setConnected] = useState(false);
  const [checking, setChecking] = useState(false);
  const [ready, setReady] = useState(false);
  const [error, setError] = useState('');
  const [proposal, setProposal] = useState<Event | null>(null);
  const [answering, setAnswering] = useState(false);
  const session = useRef('');
  const mounted = useRef(true);
  const refresh = useRef(onFilesChanged);
  refresh.current = onFilesChanged;
  useEffect(() => {
    mounted.current = true;
    let disposed = false;
    const unlisten = listen<Event>('agent-event', ({ payload: event }) => {
      if (disposed || event.session !== session.current) return;
      if (event.type === 'stream') {
        setMessages(previous => {
          const next = [...previous];
          const last = next[next.length - 1];
          if (last?.role === 'assistant') next[next.length - 1] = { ...last, content: last.content + (event.text || '') };
          return next;
        });
      } else if (event.type === 'confirmation_required') {
        setAnswering(false); setProposal(event);
      } else if (event.type === 'confirmation_resolved') {
        setProposal(null);
      } else if (event.type === 'files_changed') {
        void refresh.current();
      } else if (event.type === 'disconnected') {
        setConnected(false); setReady(false); setProposal(null);
        setError('ARN disconnected. Reopen the project to restart it.');
      } else if (['complete', 'cancelled', 'error'].includes(event.type)) {
        setProposal(null);
      }
    });
    void unlisten.then(async () => {
      if (disposed || !projectRoot) return;
      try {
        const status = await invoke<{session: string; connected: boolean}>('get_agent_status');
        if (!disposed) { session.current = status.session; setConnected(status.connected); setReady(true); }
      } catch (e) { if (!disposed) setError(String(e)); }
    });
    return () => { disposed = true; mounted.current = false; void unlisten.then(stop => stop()); };
  }, [projectRoot]);
  const verify = async () => {
    setChecking(true); setConnected(false); setModels([]); setModel(''); setError('');
    try {
      const result = await invoke<{models: string[]}>('configure_agent', { provider, apiKey });
      if (mounted.current) { setModels(result.models); setModel(result.models[0] || ''); setApiKey(''); }
    } catch (e) { if (mounted.current) setError(String(e)); }
    finally { if (mounted.current) setChecking(false); }
  };
  const select = async () => {
    setChecking(true); setError('');
    try { await invoke('select_agent_model', {model}); if (mounted.current) setConnected(true); }
    catch (e) { if (mounted.current) setError(String(e)); }
    finally { if (mounted.current) setChecking(false); }
  };
  const send = async (e: React.FormEvent) => {
    e.preventDefault();
    if (!input.trim() || busy || dirtyPaths.length || !connected) return;
    const message = input.trim(); setInput(''); setError(''); onBusyChange(true);
    setMessages(previous => [...previous, {role:'user', content:message}, {role:'assistant', content:''}]);
    try { await invoke('send_agent_message', {message, dirtyPaths}); }
    catch (e) { if (mounted.current) setError(String(e)); }
    finally {
      if (mounted.current) {
        setProposal(null);
        await refresh.current();
        onBusyChange(false);
      }
    }
  };
  const answer = async (approved: boolean) => {
    if (!proposal?.id || answering) return;
    setAnswering(true);
    try {
      await invoke('confirm_agent_change', {id:proposal.id, approved:approved && dirtyPaths.length === 0});
      setProposal(null);
    } catch (e) { setError(String(e)); setAnswering(false); }
  };
  const cancel = async () => {
    try { await invoke('cancel_agent'); setProposal(null); }
    catch (e) { setError(String(e)); }
  };
  const reset = async () => {
    try { await invoke('reset_agent_session'); setMessages([]); setError(''); }
    catch (e) { setError(String(e)); }
  };
  if (collapsed && !proposal) return <div className="agent-panel-collapsed"><button onClick={() => setCollapsed(false)}>⟨ Agent {busy ? '…' : ''}</button></div>;
  return <aside className="agent-panel" style={{width:420, minWidth:300, resize:'horizontal', overflow:'auto'}}>
    <div className="agent-header">
      <h2 className="agent-title">ARN Agent</h2>
      <span>{checking ? 'Verifying provider…' : busy ? 'Working…' : connected ? 'Ready' : 'Not connected'}</span>
      <button disabled={busy || checking} onClick={reset}>New chat</button>
      <button onClick={() => setCollapsed(true)}>⟩</button>
    </div>
    {!projectRoot && <p>Open a project folder first.</p>}
    <div className="agent-config">
      <label>Provider<select disabled={!ready || busy || checking} value={provider} onChange={e => { setProvider(e.target.value); setModels([]); setModel(''); setConnected(false); }}>
        <option value="gemini">Google Gemini</option><option value="deepseek">DeepSeek</option>
      </select></label>
      <label>API key<input type="password" autoComplete="off" value={apiKey} disabled={busy || checking} onChange={e => { setApiKey(e.target.value); setConnected(false); setModels([]); setModel(''); }} /></label>
      <button disabled={!ready || busy || checking || !apiKey} onClick={verify}>Verify access and load models</button>
      <label>Model<select disabled={busy || checking || !models.length} value={model} onChange={e => {setModel(e.target.value); setConnected(false);}}>
        {!models.length && <option value="">Load models from provider</option>}
        {models.map(name => <option key={name} value={name}>{name}</option>)}
      </select></label>
      <button disabled={busy || checking || !model || connected} onClick={select}>Use selected model</button>
      <p className="agent-note">API key is kept in memory. Every file change requires approval.</p>
    </div>
    {error && <p role="alert" style={{color:'#ff9c9c', padding:12, whiteSpace:'pre-wrap'}}>{error}</p>}
    <div className="agent-messages">
      {messages.map((message, i) => <div key={i} className={`agent-message agent-message-${message.role}`}>
        <div className="agent-message-role">{message.role}</div><div className="agent-message-content" style={{whiteSpace:'pre-wrap'}}>{message.content}</div>
      </div>)}
    </div>
    {proposal && <section aria-label="File change approval" style={{padding:12, border:'1px solid #d19a66', overflow:'auto', maxHeight:'55vh'}}>
      <strong>{proposal.operation}: {proposal.path}</strong><p>{proposal.summary}</p>
      <p>Automatically rejected after 120 seconds. Review the full before/after content:</p>
      <details open><summary>Before {proposal.operation === 'delete_file' ? '(will be deleted)' : ''}</summary><pre style={{whiteSpace:'pre-wrap'}}>{proposal.before || '(empty / absent)'}</pre></details>
      <details open><summary>After</summary><pre style={{whiteSpace:'pre-wrap'}}>{proposal.after || '(empty / deleted)'}</pre></details>
      <button disabled={answering || !!dirtyPaths.length} onClick={() => answer(true)}>Approve change</button>
      <button disabled={answering} onClick={() => answer(false)}>Reject</button>
    </section>}
    <form className="agent-composer" onSubmit={send}>
      {!!dirtyPaths.length && <p>Save or close unsaved tabs before running the agent.</p>}
      {busy && <p>The editor is read-only until this request finishes.</p>}
      <textarea className="agent-input" value={input} onChange={e => setInput(e.target.value)} disabled={busy || !connected} placeholder="Ask ARN to inspect or change this project…" />
      <button className="agent-send-btn" type="submit" disabled={busy || !connected || !!dirtyPaths.length || !input.trim()}>Send</button>
      {busy && <button type="button" onClick={cancel}>Cancel request</button>}
    </form>
  </aside>;
}
