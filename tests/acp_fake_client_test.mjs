// Round-trip test: drives the fake ACP server with a single initialize request.
import { spawn } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const server = path.join(here, 'acp_fake_server.mjs');

const child = spawn('node', [server], { stdio: ['pipe', 'pipe', 'inherit'] });
child.stdout.setEncoding('utf8');
let buf = '';
child.stdout.on('data', (chunk) => {
  buf += chunk;
  const nl = buf.indexOf('\n');
  if (nl >= 0) {
    const line = buf.slice(0, nl);
    console.log('SERVER:', line);
    const obj = JSON.parse(line);
    if (!obj.result || !obj.result.capabilities) {
      console.error('Unexpected response shape');
      process.exit(2);
    }
    if (!obj.result.agentInfo || obj.result.agentInfo.name !== 'fake-acp') {
      console.error('Bad agentInfo');
      process.exit(2);
    }
    console.log('ok: initialize round-trip');
    child.kill();
    process.exit(0);
  }
});
child.stdin.write(JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'initialize',
  params: { protocolVersion: 2, capabilities: {}, clientInfo: { name: 't', title: 't', version: '0' } } }) + '\n');
setTimeout(() => { console.error('timeout'); child.kill(); process.exit(3); }, 5000);
