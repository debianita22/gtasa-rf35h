#!/bin/sh
# test_runner.sh -- rf35h-gtasa (the service and launcher script) with fakes
#
#   tests/test_runner.sh [BUILD_DIR]
#
# A fake gtasa and a fake systemctl stand in for the real ones. With BUSYBOX
# pointing at a busybox binary, the script runs under its ash and applets,
# as on Lakka.
set -u

cd "$(dirname "$0")/.."
B=${1:-tests/build/native}
W="$PWD/$B/runner-work"
RUNNER="$PWD/packaging/lakka/gtasa/scripts/rf35h-gtasa"
rm -rf "$W"
mkdir -p "$W/bin"
fail=0
check() {
  if eval "$1"; then :; else
    echo "  FAIL: $2"
    fail=1
  fi
}

SH=sh
if [ -n "${BUSYBOX:-}" ]; then
  for a in sh awk sed grep date du cut sort basename mv rm mkdir cat printf tr head tail sleep touch readlink; do
    ln -sf "$BUSYBOX" "$W/bin/$a"
  done
  SH="$W/bin/sh"
  echo "(busybox $("$BUSYBOX" | head -1 | cut -d' ' -f2))"
fi

# fake gtasa: --write-config, --import, --check, or a game run (GAME_RC, GAME_SECS)
cat > "$W/bin/gtasa" <<'EOF'
#!/bin/sh
dir=.
while [ $# -gt 0 ]; do
  case "$1" in
    -d) dir=$2; shift ;;
    --write-config) [ -f "$dir/gtasa_nx.cfg" ] || printf '# defaults\nfps_cap_30 1\nrender_scale 100\nperf_mode 2\nswap_ab 0\n' > "$dir/gtasa_nx.cfg"; exit 0 ;;
    --import) echo "import" >> "$dir/fake.calls"
              echo "import ${GTASA_LOCK_FD:-none} $(readlink "/proc/$$/fd/${GTASA_LOCK_FD:-9}" 2>/dev/null)" >> "$dir/fake.lock"
              exit "${IMPORT_RC:-0}" ;;
    --check) echo "check" >> "$dir/fake.calls"
             echo "check $(readlink "/proc/$$/fd/9" 2>/dev/null)" >> "$dir/fake.lock"
             [ "${CHECK_RC:-0}" = 0 ] && echo "OK: game files usable" && exit 0
             echo "FAIL: 3 GameNative entry points missing"; exit 1 ;;
  esac
  shift
done
echo "run" >> "$dir/fake.calls"
sleep "${GAME_SECS:-0}"
[ -n "${FAKE_UPTIME_AFTER:-}" ] && echo "$FAKE_UPTIME_AFTER" > "$UPTIME_FILE"
[ -n "${GAME_ERROR:-}" ] && echo "$GAME_ERROR" > "$dir/last-error.txt"
exit "${GAME_RC:-0}"
EOF
cat > "$W/bin/systemctl" <<EOF
#!/bin/sh
echo "\$*" >> "$W/systemctl.calls"
EOF
chmod +x "$W/bin/gtasa" "$W/bin/systemctl"
export GTASA_DIR="$W/game" GTASA_BIN="$W/bin/gtasa" RA_LOGS="$W/logs" SYSTEMCTL="$W/bin/systemctl"
export PATH="$W/bin:$PATH"
mkdir -p "$GTASA_DIR"
LRTL="$RA_LOGS/GTA San Andreas/GTA San Andreas.lrtl"
run() { "$SH" "$RUNNER" "$@"; }

echo "-- start: options into gtasa_nx.cfg, then the service"
echo "old" > "$GTASA_DIR/last-error.txt"
run start fps=60 scale=75 perf=1 swap_ab=1 show_fps=1 stats=1
check '[ $? -eq 0 ]' "start failed"
cfg="$GTASA_DIR/gtasa_nx.cfg"
check 'grep -qx "fps_cap_30 0" "$cfg"' "fps=60 -> fps_cap_30 0"
check 'grep -qx "render_scale 75" "$cfg"' "scale=75"
check 'grep -qx "perf_mode 1" "$cfg"' "perf=1"
check 'grep -qx "swap_ab 1" "$cfg"' "swap_ab=1"
check 'grep -qx "show_fps 1" "$cfg" && grep -qx "stats 1" "$cfg"' "show_fps and stats added"
check '[ $(grep -c "^render_scale" "$cfg") -eq 1 ]' "no duplicate keys"
check 'grep -qx "# defaults" "$cfg"' "the commented defaults were kept"
check '[ ! -e "$GTASA_DIR/last-error.txt" ]' "last-error.txt cleared before a start"
check 'grep -qx "start --no-block rf35h-gtasa.service" "$W/systemctl.calls"' "service started"

echo "-- start: a bad option is refused before anything changes"
rm -f "$W/systemctl.calls"
cp "$cfg" "$W/cfg.before"
run start scale=30 2>/dev/null
check '[ $? -ne 0 ]' "scale=30 accepted"
run start "perf=2;reboot" 2>/dev/null
check '[ $? -ne 0 ]' "perf=2;reboot accepted"
run start colour=red 2>/dev/null
check '[ $? -ne 0 ]' "unknown option accepted"
run start fps=30 swap_ab=0 scale=30 2>/dev/null
check '[ $? -ne 0 ]' "a bad option after good ones accepted"
check '[ ! -e "$W/systemctl.calls" ]' "service started despite a bad option"
check 'cmp -s "$cfg" "$W/cfg.before"' "config changed by a refused start"

echo "-- run: play time into RetroArch's runtime log"
GAME_SECS=2 run run
check '[ $? -eq 0 ]' "run failed"
check '[ -f "$LRTL" ]' "no runtime log"
check 'grep -q "\"runtime\": \"0:00:0[23]\"" "$LRTL"' "runtime $(grep runtime "$LRTL" 2>/dev/null)"
check 'grep -q "\"play_count\": \"1\"" "$LRTL"' "first play_count is 1"
check 'grep -Eq "\"last_played\": \"[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\"" "$LRTL"' "last_played format"
# RetroArch rewrites the file when it unloads the launcher core: its count stays
printf '{\n  "version": "1.0",\n  "runtime": "1:59:58",\n  "last_played": "2026-01-01 10:00:00",\n  "play_count": "5",\n  "state_slot": "0"\n}\n' > "$LRTL"
GAME_SECS=3 run run
check 'grep -q "\"runtime\": \"2:00:0[12]\"" "$LRTL"' "runtime added across the hour: $(grep runtime "$LRTL")"
check 'grep -q "\"play_count\": \"5\"" "$LRTL"' "RetroArch's play_count kept"
check 'grep -q "\"version\": \"1.0\"" "$LRTL" && grep -q "\"state_slot\": \"0\"" "$LRTL"' "all fields present"

echo "-- run: the time comes from the uptime, which NTP cannot move"
printf '1000.50 1.00\n' > "$W/uptime"
UPTIME_FILE="$W/uptime" FAKE_UPTIME_AFTER="1061.70 1.00" run run
check 'grep -q "\"runtime\": \"2:01:0[23]\"" "$LRTL"' "61 s of uptime not added: $(grep runtime "$LRTL")"

echo "-- run: RetroArch's runtime log directory from retroarch.cfg"
mkdir -p "$W/home"
printf 'playlist_directory = "~/pl"\nruntime_log_directory = "default"\n' > "$W/retroarch.cfg"
printf '10.00 1.00\n' > "$W/uptime"
(unset RA_LOGS; HOME="$W/home" RA_CFG="$W/retroarch.cfg" UPTIME_FILE="$W/uptime" FAKE_UPTIME_AFTER="20.00 1.00" run run)
check '[ -f "$W/home/pl/logs/GTA San Andreas/GTA San Andreas.lrtl" ]' "not under playlist_directory/logs: $(find "$W/home" -name '*.lrtl')"
printf 'playlist_directory = "/nonexistent"\nruntime_log_directory = "%s/rtl"\n' "$W" > "$W/retroarch.cfg"
printf '10.00 1.00\n' > "$W/uptime"
(unset RA_LOGS; RA_CFG="$W/retroarch.cfg" UPTIME_FILE="$W/uptime" FAKE_UPTIME_AFTER="20.00 1.00" run run)
check '[ -f "$W/rtl/GTA San Andreas/GTA San Andreas.lrtl" ]' "runtime_log_directory ignored"

echo "-- run: a failing game leaves a reason for the launcher"
GAME_RC=2 run run
check '[ $? -eq 2 ]' "exit status not passed on"
check 'grep -q "codice 2" "$GTASA_DIR/last-error.txt"' "generic reason written"
GAME_RC=2 GAME_ERROR="Mancano i dati del gioco" run run
check 'grep -qx "Mancano i dati del gioco" "$GTASA_DIR/last-error.txt"' "the game's own reason kept"
rm -f "$GTASA_DIR/last-error.txt"

echo "-- run: systemd's SIGTERM to the whole unit still gets the time recorded"
before=$(grep runtime "$LRTL")
GAME_SECS=5 setsid "$SH" "$RUNNER" run &
pid=$!
sleep 1
kill -TERM "-$pid" 2>/dev/null  # like KillMode=control-group: every process
wait $pid
check '[ "$(grep runtime "$LRTL")" != "$before" ]' "runtime not recorded after SIGTERM ($before)"
check 'grep -q "codice 143" "$GTASA_DIR/last-error.txt"' "the killed game's status: $(cat "$GTASA_DIR/last-error.txt" 2>/dev/null)"
rm -f "$GTASA_DIR/last-error.txt"

echo "-- import: extraction, then validation"
rm -f "$GTASA_DIR/fake.calls" "$GTASA_DIR/fake.lock"
run import
check '[ $? -eq 0 ]' "import failed"
check '[ "$(cat "$GTASA_DIR/fake.calls" | tr "\n" " ")" = "import check " ]' "calls: $(cat "$GTASA_DIR/fake.calls" | tr '\n' ' ')"
check 'grep -q "^import 9 .*/\.import\.lock$" "$GTASA_DIR/fake.lock"' "the importer did not get the lock descriptor: $(head -1 "$GTASA_DIR/fake.lock")"
check 'grep -q "^check .*/\.import\.lock$" "$GTASA_DIR/fake.lock"' "the lock descriptor is not open during the check: $(tail -1 "$GTASA_DIR/fake.lock")"
rm -f "$GTASA_DIR/fake.calls"
CHECK_RC=1 run import
check '[ $? -ne 0 ]' "an invalid install reported success"
check 'grep -q "^fail 100 .*GameNative" "$GTASA_DIR/import-status.txt"' "status: $(cat "$GTASA_DIR/import-status.txt")"
check 'grep -q "GameNative" "$GTASA_DIR/last-error.txt"' "reason in last-error.txt"
rm -f "$GTASA_DIR/fake.calls" "$GTASA_DIR/import-status.txt"
IMPORT_RC=2 run import
check '[ $? -eq 2 ] && [ "$(cat "$GTASA_DIR/fake.calls")" = "import" ]' "a failed extraction is not validated"

echo "-- slim: texture database variants the game never opened"
T="$GTASA_DIR/texdb/gta3"
mkdir -p "$T"
for fmt in etc dxt pvr unc; do for ext in dat tmb toc; do echo "$fmt" > "$T/gta3.$fmt.$ext"; done; done
printf '[   12.000  100] data: texture database texdb/gta3/gta3.etc.dat\n[   12.100  100] data: texture database texdb/mobile/mobile.unc.dat\n' > "$GTASA_DIR/gtasa.log"
out=$(run slim)
check 'echo "$out" | grep -q "in use: etc unc"' "formats in use: $out"
check 'echo "$out" | grep -q "unused: .*gta3.dxt.dat" && echo "$out" | grep -q "unused: .*gta3.pvr.toc"' "unused variants listed"
check '[ -f "$T/gta3.dxt.dat" ]' "slim without --yes deleted files"
run slim --yes > /dev/null
check '[ ! -e "$T/gta3.dxt.dat" ] && [ ! -e "$T/gta3.pvr.tmb" ]' "unused variants not deleted"
check '[ -f "$T/gta3.etc.dat" ] && [ -f "$T/gta3.etc.toc" ] && [ -f "$T/gta3.unc.tmb" ]' "used variants deleted"
rm -f "$GTASA_DIR/gtasa.log"
run slim 2>/dev/null
check '[ $? -ne 0 ]' "slim without a log must refuse"

[ $fail = 0 ] && echo "runner: ok" || { echo "runner: FAILED"; exit 1; }
