import { useEffect, useRef, useState } from 'react';
import { invoke } from '@tauri-apps/api/core';
import { listen } from '@tauri-apps/api/event';
import './AgentPanel.css';

type Event = { type: string; session: string; requestId?: string; text?: string; message?: string; id?: string; path?: string; operation?: string; before?: string; after?: string; summary?: string };
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
  const [showConfig, setShowConfig] = useState(false);
  const [activity, setActivity] = useState('Waiting for provider response');
  const [elapsed, setElapsed] = useState(0);
  const [cancelling, setCancelling] = useState(false);
  const startedAt = useRef(0);
  const session = useRef('');
  const mounted = useRef(true);
  const refresh = useRef(onFilesChanged);
  refresh.current = onFilesChanged;
  useEffect(() => {
    if (!busy && !checking) return;
    const update = () => setElapsed(Math.floor((Date.now() - startedAt.current) / 1000));
    update();
    const timer = window.setInterval(update, 1000);
    return () => window.clearInterval(timer);
  }, [busy, checking]);
  useEffect(() => {
    mounted.current = true;
    let disposed = false;
    const unlisten = listen<Event>('agent-event', ({ payload: event }) => {
      if (disposed || event.session !== session.current) return;
      if (event.type === 'stream') {
        setActivity('Receiving response');
        setMessages(previous => {
          const next = [...previous];
          const last = next[next.length - 1];
          if (last?.role === 'assistant') next[next.length - 1] = { ...last, content: last.content + (event.text || '') };
          return next;
        });
      } else if (event.type === 'progress') {
        setActivity(event.message || 'Working');
      } else if (event.type === 'confirmation_required') {
        setActivity('Waiting for your approval');
        setAnswering(false); setProposal(event);
      } else if (event.type === 'confirmation_resolved') {
        setActivity('Continuing after file review');
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
    startedAt.current = Date.now(); setElapsed(0);
    setChecking(true); setConnected(false); setModels([]); setModel(''); setError('');
    try {
      const result = await invoke<{models: string[]}>('configure_agent', { provider, apiKey, session:session.current });
      if (mounted.current) { setModels(result.models); setModel(result.models[0] || ''); setApiKey(''); }
    } catch (e) { if (mounted.current) setError(String(e)); }
    finally { if (mounted.current) setChecking(false); }
  };
  const select = async () => {
    startedAt.current = Date.now(); setElapsed(0);
    setChecking(true); setError('');
    try { await invoke('select_agent_model', {model, session:session.current}); if (mounted.current) { setConnected(true); setShowConfig(false); } }
    catch (e) { if (mounted.current) setError(String(e)); }
    finally { if (mounted.current) setChecking(false); }
  };
  const send = async (e: React.FormEvent) => {
    e.preventDefault();
    if (!input.trim() || busy || dirtyPaths.length || !connected) return;
    const message = input.trim(); setInput(''); setError(''); onBusyChange(true);
    startedAt.current = Date.now(); setElapsed(0); setCancelling(false);
    setActivity('Waiting for provider response');
    setMessages(previous => [...previous, {role:'user', content:message}, {role:'assistant', content:''}]);
    try {
      await invoke('send_agent_message', {message, dirtyPaths, session:session.current});
      if (mounted.current) setMessages(previous => previous.map((item, index) =>
        index === previous.length - 1 && !item.content ? {...item, content:'Request finished without a text response.'} : item));
    }
    catch (e) {
      if (mounted.current) {
        setError(String(e));
        setMessages(previous => previous.filter((item, index) => index !== previous.length - 1 || !!item.content));
      }
    }
    finally {
      if (mounted.current) {
        setProposal(null);
        try { await refresh.current(); }
        finally { onBusyChange(false); setCancelling(false); }
      }
    }
  };
  const answer = async (approved: boolean) => {
    if (!proposal?.id || answering) return;
    setAnswering(true);
    try {
      await invoke('confirm_agent_change', {id:proposal.id, session:session.current, approved:approved && dirtyPaths.length === 0});
      setProposal(null);
    } catch (e) { setError(String(e)); setAnswering(false); }
  };
  const cancel = async () => {
    setCancelling(true);
    try { await invoke('cancel_agent', {session:session.current}); setProposal(null); }
    catch (e) { setError(String(e)); setCancelling(false); }
  };
  const reset = async () => {
    try { await invoke('reset_agent_session', {session:session.current}); setMessages([]); setError(''); }
    catch (e) { setError(String(e)); }
  };
  if (collapsed && !proposal) return <div className="agent-panel-collapsed"><button onClick={() => setCollapsed(false)}>Agent {busy ? '...' : ''}</button></div>;
  return <aside className="agent-panel" style={{width:420, minWidth:300, resize:'horizontal', overflow:'auto'}}>
    <div className="agent-header">
      <h2 className="agent-title">ARN Agent</h2>
      <span className="agent-status-text">{checking ? 'Connecting...' : busy ? 'Working...' : connected ? 'Ready' : 'Not connected'}</span>
      <button disabled={busy || checking} onClick={reset}>New chat</button>
      <button onClick={() => setCollapsed(true)}>Hide</button>
    </div>
    {!projectRoot && <p>Open a project folder first.</p>}
    {connected && <div className="agent-connection">
      <span>{provider === 'gemini' ? 'Google Gemini' : 'DeepSeek'} · {model}</span>
      <button disabled={busy || checking} onClick={() => setShowConfig(value => !value)}>{showConfig ? 'Close settings' : 'Settings'}</button>
    </div>}
    {(!connected || showConfig) && <div className="agent-config">
      <h3>Connect AI</h3>
      <label>Provider<select disabled={!ready || busy || checking} value={provider} onChange={e => { setProvider(e.target.value); setModels([]); setModel(''); setConnected(false); }}>
        <option value="gemini">Google Gemini</option><option value="deepseek">DeepSeek</option>
      </select></label>
      <label>API key<input type="password" autoComplete="off" value={apiKey} disabled={busy || checking} onChange={e => { setApiKey(e.target.value); setConnected(false); setModels([]); setModel(''); }} /></label>
      <button className="agent-btn-primary" disabled={!ready || busy || checking || !apiKey} onClick={verify}>{checking ? `Checking access... ${elapsed}s` : 'Verify access and load models'}</button>
      <label>Model<select disabled={busy || checking || !models.length} value={model} onChange={e => {setModel(e.target.value); setConnected(false);}}>
        {!models.length && <option value="">Load models from provider</option>}
        {models.map(name => <option key={name} value={name}>{name}</option>)}
      </select></label>
      <button className="agent-btn-primary" disabled={busy || checking || !model || connected} onClick={select}>Use selected model</button>
      <p className="agent-note">API key is kept in memory. Every file change requires approval.</p>
    </div>}
    {error && <p role="alert" style={{color:'#ff9c9c', padding:12, whiteSpace:'pre-wrap'}}>{error}</p>}
    <div className="agent-messages">
      {messages.map((message, i) => !message.content && message.role === 'assistant' ? null : <div key={i} className={`agent-message agent-message-${message.role}`}>
        <div className="agent-message-role">{message.role}</div><div className="agent-message-content" style={{whiteSpace:'pre-wrap'}}>{message.content}</div>
      </div>)}
    </div>
    {busy && <div className="agent-progress" role="status" aria-live="polite">
      <div className="agent-progress-title"><span>{cancelling ? 'Cancelling request...' : activity}</span><span className="agent-progress-time" aria-hidden="true">{elapsed}s</span></div>
      {elapsed >= 30 && !proposal && !cancelling && <p>This request is taking longer. You can cancel it below; the current stage is shown above.</p>}
    </div>}
    {proposal && <section aria-label="File change approval" style={{padding:12, border:'1px solid #d19a66', overflow:'auto', maxHeight:'55vh', flexShrink:0}}>
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
      <textarea className="agent-input" value={input} onChange={e => setInput(e.target.value)} disabled={busy || !connected} placeholder="Ask ARN to inspect or change this project..." />
      <button className="agent-send-btn" type="submit" disabled={busy || !connected || !!dirtyPaths.length || !input.trim()}>Send</button>
      {busy && <button className="agent-cancel-btn" type="button" disabled={cancelling} onClick={cancel}>{cancelling ? 'Cancelling...' : 'Cancel request'}</button>}
    </form>
  </aside>;
}
