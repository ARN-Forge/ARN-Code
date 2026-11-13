// Monaco Editor is bundled with the application (monaco-editor, pinned in
// package.json) instead of being loaded from a CDN: the editor runs in the same
// WebView as the Tauri IPC bridge, so it must not be code fetched at run time.
// @monaco-editor/react is told to use this instance, which stops its loader
// from requesting its default CDN copy.
import * as monaco from 'monaco-editor';
import EditorWorker from 'monaco-editor/editor/editor.worker.js?worker';
import { loader } from '@monaco-editor/react';

// The IDE opens every file as plain text, which needs only the core editor
// worker. Language services with their own workers (TypeScript, JSON, CSS,
// HTML) are not bundled; asking for one fails loudly instead of silently.
self.MonacoEnvironment = {
  getWorker(_workerId: string, label: string) {
    if (label === 'editorWorkerService') return new EditorWorker();
    throw new Error(`Monaco worker '${label}' is not bundled with ARN IDE`);
  },
};

loader.config({ monaco });

export { monaco };
