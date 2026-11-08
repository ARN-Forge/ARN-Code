#!/usr/bin/env bash
# PTY tests for ARN Code's POSIX terminal UI. tmux is the terminal emulator:
# ARN runs on a real pseudo-terminal inside a shell, keys arrive as the bytes
# a terminal sends, and tmux's screen and scrollback are inspected. The shell
# records `stty -g` before and after each run.
#
# Streaming, tool notices, permission prompts, cancellation and errors come
# from tests/acp_fake_server.mjs, started through a fake `kiro-cli`
# (ARN_KIRO_BIN), so no network or account is needed.
#
#   tests/posix_pty_test.sh path/to/arn path/to/terminal_posix_test path/to/acp_fake_server.mjs
#
# Needs bash, tmux (3.x), node, pgrep and a UTF-8 locale.

set -u
arn=$(readlink -f "$1")
probe=$(readlink -f "$2")
fake_server=$(readlink -f "$3")
export LANG=${PTY_TEST_LOCALE:-C.UTF-8} LC_ALL=${PTY_TEST_LOCALE:-C.UTF-8}
socket="arn_pty_$$"
state=$(mktemp -d)
passes=0
failures=0

# A fake Kiro CLI: authenticated, one model, and `acp` runs the fake server.
mkdir -p "$state/home"
cat > "$state/kiro-cli" <<EOF
#!/bin/sh
case "\$1" in
    whoami) echo "pty-test@example.invalid" ;;
    chat) echo '["fake-model-a"]' ;;
    acp) exec node "$fake_server" ;;
    *) exit 1 ;;
esac
EOF
chmod +x "$state/kiro-cli"

pass() { passes=$((passes + 1)); echo "PASS $1"; }
fail() { failures=$((failures + 1)); echo "FAIL $1"; screen | sed "s/^/    | /"; }
expect() { # expect "description" command [args...]
    local name=$1
    shift
    if "$@"; then pass "$name"; else fail "$name"; fi
}

t() { tmux -L "$socket" -f /dev/null "$@"; }
screen() { t capture-pane -p -t pty; }           # visible screen
history() { t capture-pane -p -J -S - -t pty; }  # scrollback + screen, wrapped lines joined
row() { screen | sed -n "${1}p" | sed 's/[[:space:]]*$//'; }
occurrences() { grep -cF -- "$1" || true; }      # matching lines on stdin
equals() { [[ "$1" == "$2" ]]; }
alternate_on() { equals "$(t display -p -t pty '#{alternate_on}')" 1; }
alternate_off() { equals "$(t display -p -t pty '#{alternate_on}')" 0; }

wait_screen() { # wait_screen token [tenths of a second, default 100]
    local n; for ((n = 0; n < ${2:-100}; n++)); do
        screen | grep -qF -- "$1" && return 0
        sleep 0.1
    done
    return 1
}
wait_row() { # wait_row row prefix [tenths]
    local n; for ((n = 0; n < ${3:-100}; n++)); do
        [[ "$(row "$1")" == "$2"* ]] && return 0
        sleep 0.1
    done
    return 1
}
wait_exit() {
    local n; for ((n = 0; n < 150; n++)); do
        history | grep -q '^EXIT=' && return 0
        sleep 0.1
    done
    return 1
}
exit_code() { history | sed -n 's/^EXIT=\([0-9]*\).*/\1/p' | tail -1; }
stty_restored() { [[ -s "$state/stty_before" ]] && cmp -s "$state/stty_before" "$state/stty_after"; }

# A shell in a fresh tmux server of the given size, running one program.
start() { # start width height command...
    local width=$1 height=$2
    shift 2
    t kill-server 2>/dev/null
    rm -f "$state"/stty_*
    t new-session -d -s pty -x "$width" -y "$height" "env PS1='$ ' bash --norc --noprofile"
    t set-option -g history-limit 20000
    sleep 0.3
    # The marker is computed so the typed command line does not contain it.
    t send-keys -t pty "stty -g > $state/stty_before; echo before-arn-\$((40+2)); HOME='$state/home' ARN_KIRO_BIN='$state/kiro-cli' $*; echo EXIT=\$?; stty -g > $state/stty_after" Enter
}
start_arn() { start "$1" "$2" "'$arn'"; wait_row $(($2 - 2)) "arn ›"; }
resize() { t resize-window -t pty -x "$1" -y "$2"; }
pane_height() { t display -p -t pty '#{pane_height}'; }
# Idle at the prompt. While a request runs, ARN's cancellation monitor owns
# input and drops keys other than Esc/Ctrl+C, so typing must wait for this.
idle() {
    local hint
    local n; for ((n = 0; n < 150; n++)); do
        hint=$(row $(($(pane_height) - 1)))
        [[ "$hint" == "Type /help for commands"* || "$hint" == "Input reset complete."* ]] && return 0
        sleep 0.1
    done
    return 1
}
type_line() { idle; t send-keys -t pty -l "$1"; t send-keys -t pty Enter; }
# Newest live arn (the container's init may leave exited ones as zombies).
arn_pid() { ps -C arn -o pid=,stat= | awk '$2 !~ /Z/ {print $1}' | tail -1; }

# Layout for an alternate-screen frame of the given height (1-based rows).
prompt_row() { echo $(($1 - 2)); }
hint_row() { echo $(($1 - 1)); }
prompt_once_at_row() { # prompt_once_at_row height
    equals "$(screen | grep -nE '^arn ›( |$)' | cut -d: -f1)" "$(prompt_row "$1")"
}
header_once() { equals "$(screen | occurrences 'ARN AGENT CODE')" 1; }
transcript_rows() { screen | sed -n "11,$(($1 - 4))p"; } # width >= 80: nine header rows

# After exit: the conversation printed once, no frame in scrollback.
once_in_history() { equals "$(history | occurrences "$1")" 1; }
count_in_history() { equals "$(history | occurrences "$1")" "$2"; }
absent_from_history() { ! history | grep -qF -- "$1"; }
no_frames_in_history() {
    local text
    text=$(history)
    ! grep -qF 'ARN AGENT CODE' <<<"$text" &&
        ! grep -qF 'Type /help for commands' <<<"$text" &&
        ! grep -qF 'Scrolled up' <<<"$text" &&
        ! grep -qF '──────────' <<<"$text" &&
        ! grep -qE '^arn ›( |$)' <<<"$text" &&
        ! grep -qF 'Arnie is working' <<<"$text"
}
shell_history_kept() { history | head -n 3 | grep -qF before-arn-42; }
clean_exit() { # clean_exit name expected-code
    expect "$1: exits" wait_exit
    expect "$1: exit code $2" equals "$(exit_code)" "$2"
    expect "$1: alternate screen left" alternate_off
    expect "$1: terminal settings restored" stty_restored
    expect "$1: no frame in scrollback" no_frames_in_history
    expect "$1: shell history kept above" shell_history_kept
}
acp_ready() {
    type_line "/acp kiro"
    wait_screen "ACP agent ready: Kiro via kiro-cli"
}

# --- 1. Streaming, tools, confirmations, cancellation, Unicode, /exit ------
echo "== ACP session at 100x30"
start_arn 100 30
expect "startup: alternate screen" alternate_on
expect "startup: header once" header_once
expect "startup: prompt once, third row from the bottom" prompt_once_at_row 30
expect "startup: hint row" wait_row 29 "Type /help for commands"
expect "startup: shell history hidden" bash -c "! tmux -L $socket capture-pane -p -t pty | grep -qF before-arn-42"
expect "acp: backend ready" acp_ready

type_line "__FAKE_ACP_CMD__ stream-and-finish"
expect "stream: reply finished" wait_screen "Done."
expect "stream: text streamed once" equals "$(screen | occurrences 'hello world')" 1
# The ACP layer reports the tool call and its update; each is a notice.
expect "stream: tool notice shown" bash -c "(( $(tmux -L $socket capture-pane -p -t pty | grep -cF "• read_file") >= 1 ))"
expect "stream: prompt once" prompt_once_at_row 30

type_line "__FAKE_ACP_CMD__ stream-unicode"
expect "unicode: reply finished" wait_screen " end."
unicode_line=$'Unicode: Привіт ї є ґ, 你好世界, 🙂👍🎉, e\xcc\x81 end.' # e + U+0301
expect "unicode: mixed scripts intact" wait_screen "$unicode_line"

type_line "__FAKE_ACP_CMD__ permission-then-finish"
expect "confirm: question in the status row" wait_row 29 "ACP tool: delete important file [y/N]"
expect "confirm: prompt empty" equals "$(row 28)" "arn ›"
t send-keys -t pty y
expect "confirm: choice in the prompt row" wait_row 28 "arn › y"
t send-keys -t pty Enter
expect "confirm: allowed" wait_screen "choice=allow"
expect "confirm: answer recorded" wait_screen "ACP tool: delete important file [y/N] y"
expect "confirm: reply on its own row" bash -c "tmux -L $socket capture-pane -p -t pty | grep -qE '^ session=sess_[0-9]+;choice=allow$'"
expect "confirm: status row restored" wait_row 29 "Type /help for commands"

type_line "__FAKE_ACP_CMD__ permission-then-finish"
expect "decline: question shown" wait_row 29 "ACP tool: delete important file [y/N]"
t send-keys -t pty n Enter
expect "decline: rejected" wait_screen "choice=reject"

type_line "__FAKE_ACP_CMD__ wait-for-cancel"
expect "cancel: request running" wait_screen "waiting"
expect "cancel: working status" wait_row 29 "Arnie is working"
t send-keys -t pty Escape
expect "cancel: Esc cancels" wait_screen "Request cancelled."
type_line "__FAKE_ACP_CMD__ wait-for-cancel"
expect "cancel: second request running" wait_row 29 "Arnie is working"
sleep 0.3
t send-keys -t pty C-c
for ((i = 0; i < 100; i++)); do (($(screen | occurrences 'Request cancelled.') >= 2)) && break; sleep 0.1; done
expect "cancel: Ctrl+C cancels" equals "$(screen | occurrences 'Request cancelled.')" 2
expect "cancel: ARN keeps running" alternate_on
# Known, not part of the renderer: the terminal turns Ctrl+C into SIGINT for
# the whole foreground process group, and the ACP backend runs in ARN's
# group, so it stops too. Start it again for the rest of this session.
expect "acp: backend restarted after Ctrl+C" acp_ready

# Scrolling while output arrives: the rows being read stay put.
type_line "__FAKE_ACP_CMD__ stream-lines"
expect "scroll: streaming" wait_screen "line 12 "
t send-keys -t pty PPage
expect "scroll: PgUp while streaming" wait_row 29 "Scrolled up"
before=$(transcript_rows 30)
sleep 0.7
after=$(transcript_rows 30)
expect "scroll: view pinned while output arrives" equals "$before" "$after"
expect "scroll: still streaming below" bash -c "! tmux -L $socket capture-pane -p -t pty | grep -qF 'line 60 '"
t send-keys -t pty PPage
sleep 0.3
t send-keys -t pty NPage
sleep 0.3
t send-keys -t pty End
expect "scroll: End returns to the newest output" wait_screen "line 60 "
expect "scroll: reply finished" wait_row 29 "Type /help for commands"
expect "scroll: newest rows at the bottom" bash -c "tmux -L $socket capture-pane -p -t pty | sed -n 26p | grep -qF 'Done.'"
t send-keys -t pty PPage
expect "scroll: PgUp at the prompt" wait_row 29 "Scrolled up"
t send-keys -t pty End
expect "scroll: End at the prompt" wait_row 29 "Type /help for commands"

# Resizing while output arrives, and again at the prompt.
type_line "__FAKE_ACP_CMD__ stream-lines"
wait_screen "line 3 " >/dev/null
for ((i = 0; i < 24; i++)); do
    case $((i % 4)) in
        0) resize 70 24 ;; 1) resize 120 40 ;; 2) resize 50 20 ;; 3) resize 100 30 ;;
    esac
    sleep 0.12
done
expect "resize: reply finished" wait_row 29 "Type /help for commands" 150
expect "resize: header once" header_once
expect "resize: prompt once" prompt_once_at_row 30
expect "resize: no line twice on screen" equals "$(screen | grep -E '^ line [0-9]+ ' | sort | uniq -d | wc -l)" 0
resize 60 20
sleep 0.5
expect "resize at the prompt: layout follows" prompt_once_at_row 20
expect "resize at the prompt: header once" header_once
resize 100 30
sleep 0.5

type_line "/exit"
clean_exit "exit" 0
expect "transcript: user message once" once_in_history "arn › __FAKE_ACP_CMD__ stream-and-finish"
expect "transcript: streamed text once" once_in_history "hello world"
expect "transcript: tool notice printed" bash -c "tmux -L $socket capture-pane -p -J -S - -t pty | grep -qxF ' • read_file'"
expect "transcript: Unicode intact" once_in_history "$unicode_line"
expect "transcript: allowed once" once_in_history "choice=allow"
expect "transcript: rejected once" once_in_history "choice=reject"
expect "transcript: answers recorded" count_in_history "ACP tool: delete important file [y/N]" 2
expect "transcript: both cancellations" count_in_history "Request cancelled." 2
expect "transcript: each streamed line once" count_in_history " line 37 Привіт 你好 🙂" 2
expect "transcript: /exit last" bash -c "tmux -L $socket capture-pane -p -J -S - -t pty | grep -v '^\$' | grep -B1 '^EXIT=' | head -1 | grep -qF 'arn › /exit'"

# --- 2. Secure API key input -------------------------------------------------
echo "== Secure key input"
start_arn 100 30
type_line "/key-gemini"
expect "key: secure prompt" wait_row 29 "Enter submits · Esc or Ctrl+C cancels · key is kept in memory only"
t send-keys -t pty -l "FAKE_SECRET_123"
expect "key: input masked" wait_row 28 "arn › ***************"
expect "key: secret never drawn" bash -c "! tmux -L $socket capture-pane -p -t pty | grep -qF FAKE_SECRET"
t send-keys -t pty Escape
expect "key: entry cancelled" wait_screen "API key entry cancelled"
expect "key: reset required" wait_row 29 "Input reset required"
t send-keys -t pty Escape
expect "key: reset done" wait_row 29 "Input reset complete."
t send-keys -t pty -l "/key-gemini FAKE_INLINE_456"
sleep 0.3
expect "inline key: masked while typed" bash -c "! tmux -L $socket capture-pane -p -t pty | grep -qF FAKE_INLINE"
t send-keys -t pty Enter
expect "inline key: rejected" wait_screen "Inline API key entry is disabled"
t send-keys -t pty Escape
expect "inline key: reset done" wait_row 29 "Input reset complete."
type_line "/exit"
clean_exit "key session" 0
expect "key: secret not in transcript or scrollback" absent_from_history "FAKE_SECRET"
expect "key: inline secret not in transcript or scrollback" absent_from_history "FAKE_INLINE"
expect "key: warnings printed once" once_in_history "Inline API key entry is disabled"

# --- 3. Ctrl+C at the prompt, /clear, the entry cap, Ctrl+D -------------------
echo "== Prompt keys, /clear and the entry cap"
start_arn 100 30
type_line "/help"
expect "help: shown" wait_screen "Exit ARN"
type_line "/clear"
sleep 0.3
t send-keys -t pty -l "draft-to-drop"
expect "Ctrl+C: draft shown" wait_row 28 "arn › draft-to-drop"
t send-keys -t pty C-c
expect "Ctrl+C: draft cleared" wait_row 28 "arn ›"
expect "Ctrl+C: ARN keeps running" alternate_on
for ((i = 0; i < 8; i++)); do type_line "/help"; sleep 0.15; done
expect "cap: still drawing" prompt_once_at_row 30
type_line "/exit"
clean_exit "cap session" 0
expect "/clear: earlier entries not printed" absent_from_history "arn › /clear"
helps=$(history | occurrences "arn › /help")
expect "cap: oldest entries dropped ($helps of 8 /help kept)" bash -c "(( $helps < 8 && $helps >= 4 ))"
# The last /help output, then the /exit that Ctrl+D sends.
expect "cap: newest entries kept" bash -c "tmux -L $socket capture-pane -p -J -S - -t pty | grep -v '^\$' | grep -B2 '^EXIT=' | head -2 | tr '\n' '|' | grep -qF 'Exit ARN| arn › /exit|'"

# --- 4. Signals ------------------------------------------------------------------
for signal in TERM HUP; do
    echo "== SIG$signal at the prompt"
    start_arn 100 30
    type_line "/status"
    wait_screen "Provider:" >/dev/null
    kill -s "$signal" "$(arn_pid)"
    clean_exit "SIG$signal idle" 0
    expect "SIG$signal idle: transcript once" once_in_history "arn › /status"
done

echo "== SIGTERM while streaming"
start_arn 100 30
acp_ready >/dev/null
type_line "__FAKE_ACP_CMD__ stream-lines"
wait_screen "line 5 " >/dev/null
pid=$(arn_pid)
backend=$(pgrep -P "$pid" | head -1)
expect "SIGTERM streaming: ACP backend running" bash -c "[[ -n '$backend' ]] && kill -0 '$backend'"
kill -s TERM "$pid"
clean_exit "SIGTERM streaming" 0
expect "SIGTERM streaming: partial reply printed once" once_in_history " line 1 Привіт 你好 🙂"
expect "SIGTERM streaming: request cancelled" once_in_history "Request cancelled."
expect "SIGTERM streaming: reply stopped" absent_from_history " line 60 "
expect "SIGTERM streaming: ACP backend stopped" bash -c "! ps -o stat= -p '$backend' | grep -qv Z"

echo "== Error from the backend"
start_arn 100 30
acp_ready >/dev/null
type_line "__FAKE_ACP_CMD__ crash"
expect "error: reported" wait_screen "Error:"
expect "error: prompt usable" wait_row 29 "Type /help for commands"
type_line "/exit"
clean_exit "error session" 0
expect "error: printed once" once_in_history "Error:"

# --- 5. An exception unwinds through the renderer --------------------------------
echo "== Exception"
start 100 30 "'$probe' --probe-throw"
expect "exception: probe drawn" wait_screen "probe-entry-2!"
expect "exception: on the alternate screen" alternate_on
clean_exit "exception" 1
expect "exception: transcript once" once_in_history "probe-entry-1!"
expect "exception: error reported after the transcript" bash -c "tmux -L $socket capture-pane -p -J -S - -t pty | grep -A3 'probe-entry-2!' | grep -qF 'probe caught: probe failure'"

t kill-server 2>/dev/null
rm -rf "$state"
echo "$passes passed, $failures failed"
((failures == 0))
