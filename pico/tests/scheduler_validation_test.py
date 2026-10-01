#!/usr/bin/env python3
"""Compare actual old/new proc_tick bodies over all scheduling state classes."""
import pathlib
import re
import subprocess
root = pathlib.Path(__file__).resolve().parents[2]
build = root / 'pico/build/scheduler_validation'
build.mkdir(parents=True, exist_ok=True)

def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

old = subprocess.check_output(['git', 'show', '04b19ce:snes/kernel/proc.c'], cwd=root, text=True)
new = (root / 'snes/kernel/proc.c').read_text()
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PROC_MAX 3
#define ST_FREE 0
#define ST_LIVE 1
struct state {
    struct { uint16_t s; uint8_t state; } procs[3];
    uint8_t p_filing_prev, p_lock, sched_on;
    uint16_t sched_ticks, sched_limit;
    uint16_t PROC_CUR_ID, PROC_SWITCHED, PROC_OLD_S, PROC_NEXT_S, PROC_NEXT_ID, PROC_WANT;
    uint16_t SNOS_EXEC_RUN, SNOS_EXEC_FLAG;
};
static struct state st;
'''
fields = ['procs', 'p_filing_prev', 'p_lock', 'sched_on', 'sched_ticks', 'sched_limit',
          'PROC_CUR_ID', 'PROC_SWITCHED', 'PROC_OLD_S', 'PROC_NEXT_S', 'PROC_NEXT_ID',
          'PROC_WANT', 'SNOS_EXEC_RUN', 'SNOS_EXEC_FLAG']
for label, source in [('old', old), ('new', new)]:
    body = function(source, 'static int runnable(') + '\n' + function(source, 'void proc_tick(')
    for name in fields:
        body = re.sub(r'\b' + name + r'\b', 'st.' + name, body)
    body = body.replace('runnable(', label + '_runnable(').replace('proc_tick(', label + '_tick(')
    code += body + '\n'
code += r'''
int main(void) {
    unsigned cases = 0;
    for (unsigned states = 0; states < 27; ++states)
    for (unsigned cur = 0; cur < 3; ++cur)
    for (unsigned flags = 0; flags < 128; ++flags)
    for (unsigned tick = 0; tick < 4; ++tick) {
        memset(&st, 0, sizeof st);
        unsigned n = states;
        for (unsigned i = 0; i < 3; ++i) {
            st.procs[i].state = n % 3; n /= 3; st.procs[i].s = 0x1000 + i * 0x600;
        }
        st.PROC_CUR_ID = cur;
        st.p_lock = flags & 1;
        st.SNOS_EXEC_RUN = (flags >> 1) & 1;
        st.SNOS_EXEC_FLAG = (flags >> 2) & 1;
        st.PROC_SWITCHED = (flags >> 3) & 1;
        st.sched_on = (flags >> 4) & 1;
        st.PROC_WANT = (flags >> 5) & 1;
        st.p_filing_prev = flags & 64 ? 2 : 0;
        st.PROC_OLD_S = 0x1234; st.PROC_NEXT_ID = 2; st.PROC_NEXT_S = 0x1600;
        st.sched_limit = 2;
        st.sched_ticks = tick == 3 ? 65535 : tick;
        struct state initial = st;
        old_tick(); struct state expected = st;
        st = initial; new_tick(); assert(!memcmp(&st, &expected, sizeof st)); ++cases;
    }
    printf("scheduler: %u old/new decisions and state updates agree\n", cases);
}
'''
(build / 'check.c').write_text(code)
exe = build / 'check'
subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                str(build / 'check.c'), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
