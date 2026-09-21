"""Local JSONL integration tests; the fixture links the production server and tools."""
import json, os, pathlib, queue, subprocess, sys, tempfile, threading

class Server:
    def __init__(self, binary, root):
        self.p = subprocess.Popen([binary], cwd=root, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8')
        self.events = queue.Queue()
        def read():
            for line in self.p.stdout:
                self.events.put(json.loads(line))
        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()
        assert self.next()['protocol'] == 2
    def next(self):
        return self.events.get(timeout=5)
    def send(self, **cmd):
        self.p.stdin.write(json.dumps(cmd) + '\n'); self.p.stdin.flush()
    def until(self, kind):
        for _ in range(30):
            event = self.next()
            if event['type'] == kind: return event
        raise AssertionError(kind)
    def close(self):
        if not self.p.stdin.closed: self.p.stdin.close()
        assert self.p.wait(timeout=5) == 0
        assert not self.p.stderr.read()
    def configure(self):
        self.send(type='configure', id='cfg', provider='gemini', apiKey='test-only-placeholder')
        assert self.next()['models'] == ['fixture-model']
        self.send(type='select_model', id='model', model='fixture-model')
        assert self.next()['type'] == 'configured'
    def write(self, request, content='approved content'):
        self.send(type='prompt', id=request, text=json.dumps({'name':'write_file','arguments':{'path':'test.txt','content':content}}))
        progress = self.next()
        assert progress['type'] == 'progress' and progress['requestId'] == request
        assert progress['message'] == 'Waiting for provider response (attempt 1)'
        return self.until('confirmation_required')

def main(binary):
    with tempfile.TemporaryDirectory(prefix='arn-protocol-') as root:
        root = pathlib.Path(root)
        s = Server(binary, root)
        s.p.stdin.write('{broken-json-secret\n'); s.p.stdin.flush()
        assert s.next() == {'type':'error','message':'Invalid JSON command.'}
        s.configure()
        proposal = s.write('write-1')
        assert proposal['before'] == '' and proposal['after'] == 'approved content'
        assert not (root/'test.txt').exists()
        s.send(type='configure', id='concurrent-config', provider='deepseek', apiKey='unused')
        assert s.next()['requestId'] == 'concurrent-config'
        s.send(type='clear_session', id='concurrent-clear')
        assert s.next()['type'] == 'error'
        s.send(type='prompt', id='concurrent-prompt', text='wait')
        assert s.next()['requestId'] == 'concurrent-prompt'
        s.send(type='confirmation_response', id='wrong-id', approved=True)
        s.send(type='confirmation_response', id=proposal['id'], approved=False)
        s.until('complete'); assert not (root/'test.txt').exists()
        second = s.write('write-2'); assert second['id'] != proposal['id']
        s.send(type='confirmation_response', id=second['id'], approved=True)
        s.until('files_changed'); s.until('complete')
        assert (root/'test.txt').read_text() == 'approved content'
        third = s.write('write-3', 'must not be written')
        s.send(type='cancel', id='write-3'); s.until('cancelled')
        assert (root/'test.txt').read_text() == 'approved content'
        s.send(type='confirmation_response', id=third['id'], approved=True)
        s.send(type='prompt', id='wait', text='wait'); s.until('stream')
        s.send(type='cancel', id='wait'); s.until('cancelled')
        s.write('eof-confirmation', 'must not be written on EOF'); s.close()
        assert (root/'test.txt').read_text() == 'approved content'
        s = Server(binary, root)
        # EOF must not discard a valid final command without a newline.
        s.p.stdin.write(json.dumps({'type':'list_models','id':'final'})); s.p.stdin.flush(); s.p.stdin.close()
        assert s.next()['requestId'] == 'final'
        s.close()
        if os.name == 'nt':
            # Junctions do not require the symbolic-link privilege on Windows.
            with tempfile.TemporaryDirectory(prefix='arn-outside-') as outside:
                outside = pathlib.Path(outside)
                (outside/'secret.txt').write_text('outside-original')
                junction = root/'junction'
                subprocess.run(['cmd', '/c', 'mklink', '/J', str(junction), str(outside)], check=True, capture_output=True)
                try:
                    s = Server(binary, root); s.configure()
                    for name, path in [('read_file','junction/secret.txt'), ('write_file','junction/secret.txt'), ('write_file','junction/new.txt'), ('delete_file','junction/secret.txt')]:
                        s.send(type='prompt', id=name+path, text=json.dumps({'name':name, 'arguments':{'path':path,'content':'must not escape'}}))
                        while True:
                            event = s.next()
                            assert event['type'] != 'confirmation_required', event
                            if event['type'] == 'complete': break
                        assert (outside/'secret.txt').read_text() == 'outside-original'
                        assert not (outside/'new.txt').exists()
                    s.close()
                finally:
                    junction.rmdir() # Remove the test-created link itself, never the target tree.
            print('Windows junction escape tests passed')
    print('JSONL protocol, concurrency, approval, cancellation and EOF tests passed')

if __name__ == '__main__': main(str(pathlib.Path(sys.argv[1]).resolve()))
