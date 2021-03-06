/* PANDABEGINCOMMENT
 * 
 * Authors:
 *  Tim Leek               tleek@ll.mit.edu
 *  Ryan Whelan            rwhelan@ll.mit.edu
 *  Joshua Hodosh          josh.hodosh@ll.mit.edu
 *  Michael Zhivich        mzhivich@ll.mit.edu
 *  Brendan Dolan-Gavitt   brendandg@gatech.edu
 * 
 * This work is licensed under the terms of the GNU GPL, version 2. 
 * See the COPYING file in the top-level directory. 
 * 
PANDAENDCOMMENT */
// This needs to be defined before anything is included in order to get
// the PRIx64 macro
#define __STDC_FORMAT_MACROS

extern "C" {

#include "config.h"
#include "qemu-common.h"

#include "panda_common.h"
#include "panda_plugin.h"
#include "panda_plugin_plugin.h"

}

#include <map>
#include <set>

#include "../taint2/taint2.h"
#include "../taint2/taint2_ext.h"

// These need to be extern "C" so that the ABI is compatible with
// QEMU/PANDA, which is written in C
extern "C" {

bool init_plugin(void *);
void uninit_plugin(void *);
void taint_change(void);

}

std::map<target_ulong, std::set<target_ulong> > tainted_instrs;

void taint_change() {
    extern CPUState *cpu_single_env;
    CPUState *env = cpu_single_env;
    target_ulong asid = panda_current_asid(env);

    tainted_instrs[asid].insert(panda_current_pc(env));
}

bool init_plugin(void *self) {
    panda_require("taint2");

    PPP_REG_CB("taint2", on_taint_change, taint_change);

    assert(init_taint2_api());
    taint2_track_taint_state();

    return true;
}

void uninit_plugin(void *self) {
    printf("Tainted instructions: \n");
    for (auto &&asid_kv : tainted_instrs) {
        target_ulong asid = asid_kv.first;
        printf(TARGET_FMT_lx ": \n", asid);
        for (auto pc : asid_kv.second) {
            printf(TARGET_FMT_lx ", ", pc);
        }
        printf("\n");
    }
}
