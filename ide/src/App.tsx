import { useState, useEffect, useRef } from 'react';
import { invoke } from '@tauri-apps/api/core';
import { open } from '@tauri-apps/plugin-dialog';
import Editor from '@monaco-editor/react';
import { AgentPanel } from './AgentPanel';

interface FileTreeNode {
  name: string;
  path: string;
  is_dir: boolean;
  children?: FileTreeNode[];
}

interface FileContent {
  path: string;
  content: string;
}

interface EditorTab {
  path: string;
  name: string;
  content: string;
  isDirty: boolean;
}

export default function App() {
  const [projectRoot, setProjectRoot] = useState<string | null>(null);
  const [fileTree, setFileTree] = useState<FileTreeNode | null>(null);
  const [tabs, setTabs] = useState<EditorTab[]>([]);
  const [activeTab, setActiveTab] = useState<string | null>(null);
  const [expandedDirs, setExpandedDirs] = useState<Set<string>>(new Set());

  const [agentBusy, setAgentBusy] = useState(false);
  const [projectError, setProjectError] = useState('');
  const [switching, setSwitching] = useState(false);
  const [projectVersion, setProjectVersion] = useState(0);
  const tabsRef = useRef(tabs);
  tabsRef.current = tabs;
  const projectRef = useRef(projectRoot);
  projectRef.current = projectRoot;
  const refreshAgentFiles = async () => {
    const root = projectRef.current;
    if (!root) return;
    await loadFileTree(root);
    const updates = await Promise.all(tabsRef.current.map(async tab => {
      if (tab.isDirty) return tab;
      try {
        const file = await invoke<FileContent>('read_file', {path:tab.path});
        return {...tab, content:file.content};
      } catch { return {...tab, isDirty:true}; } // Preserve deleted/unreadable buffers for recovery.
    }));
    if (projectRef.current !== root) return;
    setTabs(current => current.map(tab => tab.isDirty ? tab : (updates.find(t => t.path === tab.path) || tab)));
  };

  // Initialize
  useEffect(() => {
    invoke('get_project_root').then((root) => {
      if (root) {
        setProjectRoot(root as string);
        loadFileTree(root as string);
      }
    });
  }, []);

  const selectProjectFolder = async () => {
    if (tabs.some(tab => tab.isDirty) && !confirm('Discard unsaved tabs and change project?')) return;
    const selected = await open({
      directory: true,
      multiple: false,
      title: 'Select Project Folder',
    });

    if (selected) {
      const path = Array.isArray(selected) ? selected[0] : selected;
      setSwitching(true); setProjectError('');
      try {
        const canonical = await invoke<string>('set_project_root', { path });
        setProjectRoot(canonical);
      } catch (error) {
        setProjectError(String(error));
        const root = await invoke<string | null>('get_project_root');
        setProjectRoot(root);
      }
      setProjectVersion(v => v + 1);
      await loadFileTree(path);
      setTabs([]); setActiveTab(null); setAgentBusy(false); setSwitching(false);
    }
  };

  const loadFileTree = async (_root: string) => {
    try {
      const tree = (await invoke('list_files')) as FileTreeNode;
      setFileTree(tree);
      if (tree) setExpandedDirs(previous => new Set([...previous, tree.path]));
    } catch (error) {
      console.error('Failed to load file tree:', error);
    }
  };

  const openFile = async (filePath: string) => {
    try {
      const fileContent = (await invoke('read_file', {
        path: filePath,
      })) as FileContent;

      const fileName = fileContent.path.split(/[/\\]/).pop() || 'unknown';

      // Check if tab already open
      const existingTab = tabs.find((t) => t.path === filePath);
      if (existingTab) {
        setActiveTab(filePath);
        return;
      }

      const newTab: EditorTab = {
        path: filePath,
        name: fileName,
        content: fileContent.content,
        isDirty: false,
      };

      setTabs([...tabs, newTab]);
      setActiveTab(filePath);
    } catch (error) {
      console.error('Failed to open file:', error);
    }
  };

  const closeTab = (path: string) => {
    const tab = tabs.find((t) => t.path === path);
    if (tab?.isDirty) {
      if (!confirm(`Close unsaved file "${tab.name}"?`)) {
        return;
      }
    }

    const newTabs = tabs.filter((t) => t.path !== path);
    setTabs(newTabs);

    if (activeTab === path) {
      setActiveTab(newTabs.length > 0 ? newTabs[0].path : null);
    }
  };

  const saveFile = async (path: string) => {
    const tab = tabs.find((t) => t.path === path);
    if (!tab) return;

    try {
      await invoke('write_file', {
        path,
        content: tab.content,
      });

      setTabs(
        tabs.map((t) =>
          t.path === path ? { ...t, isDirty: false } : t
        )
      );
    } catch (error) {
      console.error('Failed to save file:', error);
    }
  };

  const updateTabContent = (path: string, content: string) => {
    if (agentBusy || switching) return;
    setTabs(
      tabs.map((t) =>
        t.path === path && t.content !== content ? { ...t, content, isDirty: true } : t
      )
    );
  };

  const toggleDir = (dirPath: string) => {
    setExpandedDirs((prev) => {
      const next = new Set(prev);
      if (next.has(dirPath)) {
        next.delete(dirPath);
      } else {
        next.add(dirPath);
      }
      return next;
    });
  };

  const renderFileTree = (node: FileTreeNode, depth: number): React.ReactNode => {
    if (!node.is_dir) {
      // File
      return (
        <div
          key={node.path}
          style={{
            paddingLeft: `${depth * 16}px`,
            cursor: 'pointer',
            padding: '4px 8px',
            userSelect: 'none',
            fontSize: '13px',
          }}
          onClick={() => openFile(node.path)}
          onDoubleClick={() => {}}
        >
          📄 {node.name}
        </div>
      );
    }

    // Directory
    const isExpanded = expandedDirs.has(node.path);
    return (
      <div key={node.path}>
        <div
          style={{
            paddingLeft: `${depth * 16}px`,
            cursor: 'pointer',
            padding: '4px 8px',
            userSelect: 'none',
            fontSize: '13px',
            fontWeight: '500',
          }}
          onClick={() => toggleDir(node.path)}
        >
          {isExpanded ? '📂' : '📁'} {node.name}
        </div>
        {isExpanded &&
          node.children?.map((child) =>
            renderFileTree(child, depth + 1)
          )}
      </div>
    );
  };

  const activeTabData = tabs.find((t) => t.path === activeTab);

  return (
    <div style={{ display: 'flex', height: '100vh', backgroundColor: '#1e1e1e', color: '#e0e0e0', overflow: 'hidden' }}>
      {/* Sidebar */}
      <div style={{ width: '250px', borderRight: '1px solid #333', display: 'flex', flexDirection: 'column' }}>
        <div style={{ padding: '12px', borderBottom: '1px solid #333' }}>
          <button
            onClick={selectProjectFolder}
            disabled={switching}
            style={{
              width: '100%',
              padding: '8px',
              backgroundColor: '#007acc',
              color: 'white',
              border: 'none',
              borderRadius: '4px',
              cursor: 'pointer',
              fontSize: '12px',
            }}
          >
            Open Folder
          </button>
        </div>

        {projectError && <p role="alert" style={{color:"#ff9c9c", padding:8}}>{projectError}</p>}
        {projectRoot && (
          <div style={{ padding: '8px', overflowY: 'auto', flex: 1, fontSize: '12px' }}>
            {fileTree && renderFileTree(fileTree, 0)}
          </div>
        )}

        {!projectRoot && (
          <div style={{ padding: '16px', textAlign: 'center', color: '#888', fontSize: '12px' }}>
            No folder open
          </div>
        )}
      </div>

      {/* Main editor area */}
      <div style={{ flex: 1, display: 'flex', flexDirection: 'column' }}>
        {/* Tab bar */}
        {tabs.length > 0 && (
          <div
            style={{
              display: 'flex',
              borderBottom: '1px solid #333',
              backgroundColor: '#252526',
              height: '35px',
            }}
          >
            {tabs.map((tab) => (
              <div
                key={tab.path}
                style={{
                  display: 'flex',
                  alignItems: 'center',
                  padding: '0 12px',
                  borderRight: '1px solid #333',
                  cursor: 'pointer',
                  backgroundColor: activeTab === tab.path ? '#1e1e1e' : '#2d2d30',
                  fontSize: '12px',
                  userSelect: 'none',
                }}
                onClick={() => setActiveTab(tab.path)}
              >
                <span style={{ marginRight: '8px' }}>{tab.name}</span>
                {tab.isDirty && <span style={{ marginRight: '8px', color: '#ff7f50' }}>●</span>}
                <button
                  onClick={(e) => {
                    e.stopPropagation();
                    closeTab(tab.path);
                  }}
                  style={{
                    background: 'none',
                    border: 'none',
                    color: '#888',
                    cursor: 'pointer',
                    fontSize: '10px',
                    padding: '0 4px',
                  }}
                >
                  ✕
                </button>
              </div>
            ))}
          </div>
        )}

        {/* Editor */}
        {activeTabData ? (
          <div style={{ flex: 1, display: 'flex', flexDirection: 'column' }}>
            <div style={{ display: 'flex', padding: '8px', borderBottom: '1px solid #333', gap: '8px' }}>
              <button
                onClick={() => saveFile(activeTab!)}
                disabled={!activeTabData.isDirty || agentBusy || switching}
                style={{
                  padding: '4px 12px',
                  backgroundColor: activeTabData.isDirty ? '#007acc' : '#333',
                  color: 'white',
                  border: 'none',
                  borderRadius: '3px',
                  cursor: activeTabData.isDirty ? 'pointer' : 'not-allowed',
                  fontSize: '11px',
                }}
              >
                Save
              </button>
            </div>
            <Editor
              height="100%"
              path={activeTabData.path}
              defaultLanguage="text"
              value={activeTabData.content}
              onChange={(value) => updateTabContent(activeTab!, value || '')}
              theme="vs-dark"
              options={{
                readOnly: agentBusy || switching,
                minimap: { enabled: false },
                fontSize: 12,
                tabSize: 2,
              }}
            />
          </div>
        ) : (
          <div
            style={{
              flex: 1,
              display: 'flex',
              alignItems: 'center',
              justifyContent: 'center',
              color: '#888',
            }}
          >
            {tabs.length === 0 ? 'Open a file to start editing' : 'Select a tab'}
          </div>
        )}
      </div>

      {/* Agent Panel */}
      {!switching && <AgentPanel key={projectVersion} projectRoot={projectRoot || undefined}
        dirtyPaths={tabs.filter(t => t.isDirty).map(t => t.path)} busy={agentBusy}
        onBusyChange={setAgentBusy} onFilesChanged={refreshAgentFiles} />}
    </div>
  );
}
