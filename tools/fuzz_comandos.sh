#!/bin/bash
# Prueba de robustez de los comandos del host: le escribe comandos validos,
# invalidos y bytes al azar por la entrada estandar mientras corre con dos
# nodos simulados, y comprueba que termine bien con 'q' sin fallas.
#
#   tools/fuzz_comandos.sh              # una corrida
#   tools/fuzz_comandos.sh 20           # 20 corridas con semillas distintas
#   VALGRIND=1 tools/fuzz_comandos.sh   # el host corre bajo valgrind
#
# Hay que compilar antes (make y make fake). Para ASan: make asan compila
# build-asan/bin/host; se usa con HOST=build-asan/bin/host.

set -u
cd "$(dirname "$0")/.." || exit 1

RUNS=${1:-1}
HOST=${HOST:-build/bin/host}
FAKE=${FAKE:-build/bin/fake_node}
WORK=$(mktemp -d)
FAIL=0
PIDS=()

cleanup() {
    for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null; done
    rm -rf "$WORK"
}
trap cleanup EXIT

for bin in "$HOST" "$FAKE"; do
    [ -x "$bin" ] || { echo "Falta $bin: corra make y make fake" >&2; exit 1; }
done

# Comandos escritos a mano: validos, invalidos y en los limites
fixed_input() {
    printf 'h\n?\n'
    printf 'a\n\n'                                   # cancelado
    printf 'a    \n'
    printf 'a 9,P,100,50,40,3\n'                    # valida
    printf 'a 9,P,100,50,40,3\n'                    # id repetido
    printf 'a 10,S,50,100,60,1\n'
    printf 'a 11,S,50,100,60,1\n'                   # segunda esporadica
    printf 'a abc\n'
    printf 'a ,,,,,\n'
    printf 'a 1,2,3\n'
    printf 'a 12,X,100,50,40,3\n'                   # tipo malo
    printf 'a 13,P,0,50,40,3\n'                     # W = 0
    printf 'a 14,P,100,0,0,3\n'                     # T = 0
    printf 'a 15,P,100,50,60,3\n'                   # D > T
    printf 'a 16,P,99999999999999999999,50,40,3\n'  # numero enorme
    printf 'a -1,P,100,50,40,3\n'                   # negativo
    printf 'a 17,P,100,50,40,3,extra\n'
    printf 'a 18,P,1000000,50,40,3\n'               # no cabe
    printf 'a %0200d\n' 7                           # linea muy larga
    printf 'a %s\n' "$(head -c 300 /dev/zero | tr '\0' 'x')"
    printf 'k\n\n'
    printf 'k 0\n'
    printf 'k 99\n'
    printf 'k abc\n'
    printf 'k -1\n'
    printf 'k 1.5\n'
    printf 'k 99999999999999999999\n'
    printf 's\n\n'
    printf 's 0\n'
    printf 's 99\n'
    printf 's abc\n'
    printf 's 1\n'
    printf 's 2\n'
    printf 'zzz\n   \n\n\n'                          # teclas desconocidas
    printf 'a\b\b\b\n'                               # borrar sin texto
    printf 'a\x7f\x7f1,P,100,50,40,3\n'              # borrar con 0x7f
    printf '\x1b[A\x1b[B\x1b[C\x1b[D\n'              # flechas
    printf '\x00\xff\xfe\x80\n'                      # bytes de control y altos
    printf 'a\r\n'
    printf 'k\r2\r\n'
}

# Bytes al azar sin q ni Q, para que no termine antes de tiempo
random_input() {
    head -c "$1" /dev/urandom | tr -d 'qQ'
}

start_node() {
    local sn=$1 log="$WORK/node$2.log"
    "$FAKE" "$sn" > "$log" 2>&1 &
    PIDS+=($!)
    for _ in $(seq 1 50); do
        grep -q ' en /' "$log" 2>/dev/null && break
        sleep 0.1
    done
    sed -n 's/.* en \(\/.*\)$/\1/p' "$log" | head -1
}

for run in $(seq 1 "$RUNS"); do
    PORT1=$(start_node 61 1)
    PORT2=$(start_node 86 2)
    if [ -z "$PORT1" ] || [ -z "$PORT2" ]; then
        echo "corrida $run: no se pudo iniciar los nodos simulados" >&2
        FAIL=1
        break
    fi
    cat > "$WORK/fuzz.conf" <<EOF
algorithm = EDF
heuristic = worst-fit
tick_ms = 10
ports = $PORT1, $PORT2
tick_report_every = 10
missed_reports_for_failure = 3
tasks_file = tasks/ejemplo.csv
EOF
    { fixed_input; random_input 4000; printf '\n'; } > "$WORK/input.bin"

    CMD=("$HOST" "$WORK/fuzz.conf")
    if [ "${VALGRIND:-0}" = 1 ]; then
        CMD=(valgrind -q --leak-check=full --error-exitcode=99 "${CMD[@]}")
    fi
    # Al final se baja el nodo 2 y se espera a que el host lo detecte (reubicacion)
    # antes de pedir la terminacion con q
    { cat "$WORK/input.bin"; printf 'k 2\n'; sleep 2; printf 'q\n'; } |
        timeout 120 "${CMD[@]}" > "$WORK/out.txt" 2> "$WORK/err.txt"
    CODE=$?
    [ -n "${KEEP:-}" ] && cp "$WORK/out.txt" /tmp/fz_out.txt && cp "$WORK/err.txt" /tmp/fz_err.txt

    PROBLEM=""
    [ "$CODE" -ne 0 ] && PROBLEM="codigo de salida $CODE"
    grep -qi "segmentation\|core dumped\|AddressSanitizer\|==[0-9][0-9]*==" "$WORK/err.txt" "$WORK/out.txt" \
        && PROBLEM="$PROBLEM; errores de memoria o segfault"
    grep -q '===== Report' "$WORK/out.txt" || PROBLEM="$PROBLEM; no hay reporte final"
    ls /dev/shm 2>/dev/null | grep -qi 'host\|rt_' && PROBLEM="$PROBLEM; quedo memoria compartida"
    pgrep -x host > /dev/null && PROBLEM="$PROBLEM; quedaron procesos host"

    if [ -n "$PROBLEM" ]; then
        FAIL=1
        echo "corrida $run: FALLA ($PROBLEM)"
        cp "$WORK/input.bin" "fuzz_falla_$run.bin"
        tail -20 "$WORK/err.txt"
        echo "entrada guardada en fuzz_falla_$run.bin"
    else
        echo "corrida $run: ok"
    fi
    for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null; done
    PIDS=()
done

if [ "$FAIL" -eq 0 ]; then
    echo "OK: $RUNS corrida(s) sin fallas"
fi
exit "$FAIL"
