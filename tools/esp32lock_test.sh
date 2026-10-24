#!/usr/bin/env bash
# esp32lock 功能测试：覆盖排队、互斥、崩溃恢复与超时抢占。
set -u
export ESP32_LOCK_DIR=/tmp/esp32lock_test
rm -rf "$ESP32_LOCK_DIR"
L="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/esp32lock"
pass=0; fail=0
ok()  { echo "  ✓ $1"; pass=$((pass+1)); }
bad() { echo "  ✗ $1"; fail=$((fail+1)); }

echo "=== T1 空闲 status ==="
out=$($L status)
[[ "$out" == *free* ]] && ok "status=free" || bad "status: $out"

echo "=== T2 exec 基本用法 ==="
$L exec -- echo locked-ok >/dev/null 2>&1 && ok "exec 运行并释放" || bad "exec 失败"

echo "=== T3 并发排队：A 持锁 6s，B 排队 ==="
$L exec -- sleep 6 &
A_PID=$!
sleep 1
t0=$(date +%s)
out=$($L exec 30 -- echo B-got-board 2>&1)
t1=$(date +%s)
wait $A_PID 2>/dev/null
el=$((t1 - t0))
[[ "$out" == *B-got-board* ]] && ok "B 拿到锁执行成功" || bad "B 输出: $out"
(( el >= 4 && el <= 15 )) && ok "B 等了 ${el}s（符合预期 ≥4s）" || bad "B 等待时长异常: ${el}s"
[[ "$out" == *"board acquired after"* ]] && ok "打印了等待提示" || true
out=$($L status)
[[ "$out" == *free* ]] && ok "A/B 结束后 free" || bad "status: $out"

echo "=== T4 begin / end / 双 begin 互斥 ==="
out=$($L begin t4-session)
hpid=$(echo "$out" | grep -o 'holder=[0-9]*' | head -1 | cut -d= -f2)
[[ -n "$hpid" ]] && ok "begin 成功 holder=$hpid" || bad "begin: $out"
out=$($L status)
[[ "$out" == *BUSY* && "$out" == *t4-session* ]] && ok "status 显示 BUSY + label" || bad "status: $out"
$L begin second >/dev/null 2>&1 && bad "第二个 begin 不应成功" || ok "第二个 begin 被拒"
out=$($L exec 3 -- echo should-not-run 2>&1)
[[ "$out" == *"still busy"* ]] && ok "exec 排队超时(3s)明确报忙" || bad "exec 超时输出: $out"
out=$($L end $hpid)
[[ "$out" == *released* ]] && ok "end 释放" || bad "end: $out"
# 不变量：end 后锁必须真正释放，任何子进程都不得继承锁 fd
$L exec 3 -- echo post-end-ok >/dev/null 2>&1 && ok "end 后立刻能 exec（锁真释放了）" || bad "end 后锁仍被占"
out=$($L status)
[[ "$out" == *free* ]] && ok "end 后 free" || bad "status: $out"

echo "=== T5 崩溃安全：kill -9 持锁进程 ==="
$L exec 60 -- sleep 60 &
EPID=$!
for i in $(seq 30); do $L status 2>/dev/null | grep -q BUSY && break; sleep 0.2; done
out=$($L status)
[[ "$out" == *BUSY* ]] && ok "exec 期间 BUSY" || bad "status: $out"
kill -9 $EPID 2>/dev/null
wait $EPID 2>/dev/null
sleep 0.5
out=$($L status)
[[ "$out" == *free* ]] && ok "持锁进程被杀后立即 free（flock 自动释放）" || bad "status: $out"

echo "=== T6 超时抢占（MAX_HOLD）==="
out=$($L begin stale-session)
hpid=$(echo "$out" | grep -o 'holder=[0-9]*' | head -1 | cut -d= -f2)
for i in $(seq 30); do [[ -f "$ESP32_LOCK_DIR/board.meta" ]] && break; sleep 0.2; done
# 伪造: epoch 改成 2000s 前，模拟超时未释放的会话
epoch=$(( $(date +%s) - 2000 ))
line=$(head -1 "$ESP32_LOCK_DIR/board.meta")
printf '%s\n' "${line%|*}|$epoch" > "$ESP32_LOCK_DIR/board.meta"
out=$($L exec 20 -- echo stolen-and-ran 2>&1)
[[ "$out" == *STEALING* && "$out" == *stolen-and-ran* ]] && ok "等待方抢走超时会话并执行" || bad "抢占输出: $out"
kill -9 "$hpid" 2>/dev/null  # holder 已被 kill -9，保险清理
$L status >/dev/null

echo "=== T7 begin 的 holder 随会话 end 退出 ==="
out=$($L begin t7)
hpid=$(echo "$out" | grep -o 'holder=[0-9]*' | head -1 | cut -d= -f2)
kill -0 "$hpid" 2>/dev/null && ok "holder 进程存活" || bad "holder 未存活"
$L end "$hpid" >/dev/null
sleep 0.5
kill -0 "$hpid" 2>/dev/null && bad "holder 未退出" || ok "end 后 holder 退出"

echo
echo "===== 结果: $pass passed, $fail failed ====="
exit $fail
