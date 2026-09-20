'use strict';
const el = id => document.getElementById(id);
const scenarios = {
  create: {prompt:'Create a simple Python calculator.',answer:"I'll create calculator.py with a check for division by zero.",code:'+ def divide(a, b):\n+     if b == 0:\n+         raise ValueError("Cannot divide by zero")\n+     return a / b',approval:'Create calculator.py?',done:'✓ Preview: calculator.py created. No local files were changed.'},
  explain: {prompt:'Explain what this function does.',answer:'This returns the titles of completed tasks. It leaves the original list unchanged.',code:'def completed_titles(tasks):\n    return [task["title"] for task in tasks\n            if task["done"]]',approval:null,done:'Read-only preview. No file changes to approve.'},
  fix: {prompt:'Fix average() when the list is empty.',answer:'An empty list causes division by zero. I propose returning None when there are no values.',code:'  def average(values):\n+     if not values:\n+         return None\n      return sum(values) / len(values)',approval:'Edit stats.py?',done:'✓ Preview: stats.py updated. No local files were changed.'}
};
let current = 'create';
function showScenario(name) {
  current = name;
  const s = scenarios[name];
  el('demo-prompt').textContent = s.prompt;
  el('answer').textContent = s.answer;
  el('demo-code').textContent = s.code;
  el('approval').hidden = !s.approval;
  el('approval-label').textContent = s.approval || '';
  el('result').textContent = s.approval ? '' : s.done;
  document.querySelectorAll('[data-scenario]').forEach(b => {const active = b.dataset.scenario === name; b.classList.toggle('active', active); b.setAttribute('aria-pressed', String(active));});
}
document.querySelectorAll('[data-scenario]').forEach(b => b.addEventListener('click', () => showScenario(b.dataset.scenario)));
el('approve').addEventListener('click', () => {el('approval').hidden = true; el('result').textContent = scenarios[current].done; el('replay').focus();});
el('decline').addEventListener('click', () => {el('approval').hidden = true; el('result').textContent = 'Change declined. Your files stay as they are.'; el('replay').focus();});
el('replay').addEventListener('click', () => showScenario(current));
const unix = 'curl -fsSL https://raw.githubusercontent.com/arnecto/arn/main/scripts/install.sh | sh';
const platforms = {
  windows: {shell:'PowerShell · x64',command:'irm https://raw.githubusercontent.com/arnecto/arn/main/scripts/install.ps1 | iex',script:'install.ps1',note:'Open a new terminal after installation, then run arn from your project folder.'},
  linux: {shell:'Shell · x64',command:unix,script:'install.sh',note:'Installs to ~/.local/bin. Add that directory to PATH if needed, then run arn from your project folder.'},
  macos: {shell:'Shell · Apple Silicon & Intel',command:'brew install openssl@3\n' + unix,script:'install.sh',note:'Requires Homebrew and OpenSSL 3. Installs to ~/.local/bin; add it to PATH if needed, then run arn.'}
};
let selection = 'windows';
function selectPlatform(name) {
  selection = name;
  const p = platforms[name];
  el('shell-name').textContent = p.shell;
  el('install-command').textContent = p.command;
  el('install-note').textContent = p.note;
  el('script-link').href = 'https://github.com/arnecto/arn/blob/main/scripts/' + p.script;
  el('copy-status').textContent = '';
  document.querySelectorAll('[data-platform]').forEach(b => {const active = b.dataset.platform === name; b.classList.toggle('active', active); b.setAttribute('aria-pressed', String(active));});
}
document.querySelectorAll('[data-platform]').forEach(b => b.addEventListener('click', () => selectPlatform(b.dataset.platform)));
el('copy').addEventListener('click', async () => {
  const name = selection;
  try {await navigator.clipboard.writeText(platforms[name].command); if(selection === name) el('copy-status').textContent = 'Copied. Paste into your terminal when ready.';}
  catch {if(selection !== name) return; const range = document.createRange(); range.selectNodeContents(el('install-command')); const selected = window.getSelection(); selected.removeAllRanges(); selected.addRange(range); el('copy-status').textContent = 'Copy unavailable. Command selected — use Ctrl+C or ⌘C.';}
});
const os = navigator.platform || '';
if (/Mac/i.test(os)) selectPlatform('macos'); else if (/Linux/i.test(os) && !/Android/i.test(navigator.userAgent)) selectPlatform('linux');
const reduced = window.matchMedia('(prefers-reduced-motion: reduce)');
const mascot = el('mascot');
const crab = mascot.querySelector('svg');
mascot.addEventListener('pointermove', event => {if(reduced.matches) return; const bounds = mascot.getBoundingClientRect(); crab.style.translate = ((event.clientX - bounds.left - bounds.width / 2) / 18) + 'px ' + ((event.clientY - bounds.top - bounds.height / 2) / 18) + 'px';});
mascot.addEventListener('pointerleave', () => {crab.style.translate = '';});
let greeting = 0;
mascot.addEventListener('click', () => {const lines = ['Hey, builder. What are we making?', 'My claws are ready. Your call.', 'Small binary. Big plans.']; el('mascot-note').textContent = lines[greeting++ % lines.length];});
