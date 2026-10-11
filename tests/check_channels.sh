#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=/tmp/recdvb-cs-left-channel-test
# Owned disposable executable only; 1 MiB budget, cleaned on every exit.
trap 'rm -rf -- "$test_dir"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
if [ -e "$test_dir" ]; then
    du -sk "$test_dir"
    rm -rf -- "$test_dir"
fi
mkdir "$test_dir"
ulimit -f 1024

# Compile the production lookup/frequency functions without Linux DVB I/O.
# Source goes through stdin; there is no temporary source tree or build cache.
python3 - "$repo_dir" <<'PY' | ${CC:-cc} -Wall -Wextra -Werror -Wno-sign-compare -x c -I "$repo_dir" -o "$test_dir/check_channels" -
from pathlib import Path
import sys

source = (Path(sys.argv[1]) / "recpt1core.c").read_text()
start = source.index("static unsigned int GetFrequency_S(int ch)")
end = source.index("\nint\nset_frequency(", start)
print('''
#include <assert.h>
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "recpt1.h"
#include "ch_conv_table_dvb.h"
typedef int boolean;
#define ISDB_T_NODE_LIMIT 24
#define ISDB_T_SLOT_LIMIT 8
char name_buff[16];
ISDB_T_FREQ_CONV_TABLE isdb_t_conv_set = { .parm_freq = name_buff };
''')
print(source[start:end])
print(r'''
static void check(char *name, unsigned int frequency, unsigned int tsid)
{
    ISDB_T_FREQ_CONV_TABLE *entry = searchrecoff(name);
    assert(entry != NULL);
    assert(entry->type == CHTYPE_SATELLITE);
    assert(GetFrequency_S(entry->set_freq) == frequency);
    assert(entry->tsid == tsid);
}

int main(void)
{
    char name[16];
    char id[16];
    unsigned int right_ids[] = {
        0x6020, 0x7040, 0x7060, 0x6080, 0x60a0, 0x70c0,
        0x70e0, 0x7100, 0x7120, 0x7140, 0x7160, 0x7180
    };
    for (int node = 1; node <= 25; node += 2) {
        unsigned int frequency = node == 25 ? 2726000 :
            2766000 + 40000 * ((node - 1) / 2);
        snprintf(name, sizeof(name), "CS%d", node);
        check(name, frequency, 0);
        /* Existing explicit TSID path must agree with the physical name. */
        snprintf(id, sizeof(id), "0x%X", 0xc000 + (node << 4));
        check(id, frequency, 0xc000 + (node << 4));
        int visible = 0;
        for (int i = 0; helpChList[i] != NULL; i++) {
            char *match = strstr(helpChList[i], name);
            if (match != NULL && !isdigit((unsigned char)match[strlen(name)]))
                visible = 1;
        }
        assert(visible);
    }
    for (int i = 0; i < 12; i++) {
        snprintf(name, sizeof(name), "CS%d", (i + 1) * 2);
        check(name, 1613000 + 40000 * i, right_ids[i]);
    }
    check("BS07_0", 1164560, 0xb070);
    check("BS17_2", 1356360, 0xb112);
    check("BS14_0", 2471820, 0xb0e0);
    check("101", 1318000, 0x40f1);
    /* Former CS4K allocation: RF MHz minus the 9505 MHz LNB LO. */
    check("0xC090", (12431 - 9505) * 1000, 0xc090);
    check("0xC0B0", (12471 - 9505) * 1000, 0xc0b0);
    check("0xC130", (12631 - 9505) * 1000, 0xc130);
    check("0xC150", (12671 - 9505) * 1000, 0xc150);
    check("0xC170", (12711 - 9505) * 1000, 0xc170);
    assert(searchrecoff("CS0") == NULL);
    assert(searchrecoff("CS26") == NULL);
    assert(searchrecoff("884") == NULL);
    assert(searchrecoff("885") == NULL);
    assert(GetFrequency_T(63) == 473142857);
    puts("PASS: 13 CS left frequencies, TSID lookup, help, 12 CS right frequencies, BS, former CS4K allocation and invalid channels");
    return 0;
}
''')
PY
"$test_dir/check_channels"
rm -rf -- "$test_dir"
test ! -e "$test_dir"
echo "Cleaned $test_dir"
