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

/*
 * PANDA taint analysis plugin
 * Ryan Whelan, Tim Leek, Sam Coe, Nathan VanBenschoten
 */

// This needs to be defined before anything is included in order to get
// the PRIx64 macro
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#ifdef NDEBUG
#undef NDEBUG
#endif

#include "label_set.h"

extern "C" {

#include <sys/time.h>

#include "qemu-common.h"
#include "cpu-all.h"
#include "panda_plugin.h"
#include "panda_plugin_plugin.h"
#include "panda_common.h"
#include "panda/network.h"
#include "rr_log.h"
#include "pandalog.h"
#include "cpu.h"

extern int loglevel;

// For the C API to taint accessible from other plugins
void taint2_enable_taint(void);
int taint2_enabled(void);
void taint2_label_ram(uint64_t pa, uint32_t l) ;
uint32_t taint2_query_ram(uint64_t pa);
void taint2_delete_ram(uint64_t pa);
uint32_t taint2_query_reg(int reg_num, int offset);

uint32_t taint2_query_llvm(int reg_num, int offset);

void taint2_labelset_spit(LabelSetP ls);

void taint2_labelset_ram_iter(uint64_t pa, int (*app)(uint32_t el, void *stuff1), void *stuff2);
void taint2_labelset_reg_iter(int reg_num, int offset, int (*app)(uint32_t el, void *stuff1), void *stuff2);
void taint2_labelset_llvm_iter(int reg_num, int offset, int (*app)(uint32_t el, void *stuff1), void *stuff2);
void taint2_labelset_iter(LabelSetP ls,  int (*app)(uint32_t el, void *stuff1), void *stuff2) ;

uint32_t *taint2_labels_applied(void);
uint32_t taint2_num_labels_applied(void);

void taint2_track_taint_state(void);

}

#include <llvm/PassManager.h>
#include <llvm/PassRegistry.h>
#include <llvm/Analysis/Verifier.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

#include "tcg-llvm.h"
#include "panda_memlog.h"

#include "shad_dir_32.h"
#include "shad_dir_64.h"
#include "llvm_taint_lib.h"
#include "fast_shad.h"
#include "taint_ops.h"
#include "taint2.h"

#include "pirate_mark_lava_struct.h"

// These need to be extern "C" so that the ABI is compatible with
// QEMU/PANDA, which is written in C
extern "C" {


bool init_plugin(void *);
void uninit_plugin(void *);
int after_block_translate(CPUState *env, TranslationBlock *tb);
bool before_block_exec_invalidate_opt(CPUState *env, TranslationBlock *tb);
int before_block_exec(CPUState *env, TranslationBlock *tb);
int after_block_exec(CPUState *env, TranslationBlock *tb,
    TranslationBlock *next_tb);
//int cb_cpu_restore_state(CPUState *env, TranslationBlock *tb);
int guest_hypercall_callback(CPUState *env);

int phys_mem_write_callback(CPUState *env, target_ulong pc, target_ulong addr,
                       target_ulong size, void *buf);
int phys_mem_read_callback(CPUState *env, target_ulong pc, target_ulong addr,
        target_ulong size, void *buf);

void taint_state_changed(void);
PPP_PROT_REG_CB(on_taint_change);
PPP_CB_BOILERPLATE(on_taint_change);

bool track_taint_state = false;

}

Shad *shadow = NULL; // Global shadow memory

// Pointer passed in init_plugin()
void *plugin_ptr = NULL;

// Our pass manager to derive taint ops
llvm::FunctionPassManager *FPM = NULL;

// Taint function pass.
llvm::PandaTaintFunctionPass *PTFP = NULL;

// For now, taint becomes enabled when a label operation first occurs, and
// becomes disabled when a query operation subsequently occurs
bool taintEnabled = false;

// Lets us know right when taint was disabled
bool taintJustDisabled = false;

// Taint memlog
static taint2_memlog taint_memlog;

// Configuration
bool tainted_pointer = true;
static TaintGranularity granularity;
static TaintLabelMode mode;
bool optimize_llvm = true;
extern bool inline_taint;


/*
 * These memory callbacks are only for whole-system mode.  User-mode memory
 * accesses are captured by IR instrumentation.
 */
int phys_mem_write_callback(CPUState *env, target_ulong pc, target_ulong addr,
                       target_ulong size, void *buf) {
    /*if (size == 4) {
        printf("pmem: " TARGET_FMT_lx "\n", addr);
    }*/
    taint_memlog_push(&taint_memlog, addr);
    return 0;
}

int phys_mem_read_callback(CPUState *env, target_ulong pc, target_ulong addr,
        target_ulong size, void *buf){
    /*if (size == 4) {
        printf("pmem: " TARGET_FMT_lx "\n", addr);
    }*/
    taint_memlog_push(&taint_memlog, addr);
    return 0;
}

void verify(void) {
    llvm::Module *mod = tcg_llvm_ctx->getModule();
    std::string err;
    if(verifyModule(*mod, llvm::AbortProcessAction, &err)){
        printf("%s\n", err.c_str());
    }
}

void __taint2_enable_taint(void) {
    if(taintEnabled) {return;}
    printf ("taint2: __taint_enable_taint\n");
    taintEnabled = true;
    panda_cb pcb;

    pcb.after_block_translate = after_block_translate;
    panda_register_callback(plugin_ptr, PANDA_CB_AFTER_BLOCK_TRANSLATE, pcb);
    pcb.before_block_exec_invalidate_opt = before_block_exec_invalidate_opt;
    panda_register_callback(plugin_ptr, PANDA_CB_BEFORE_BLOCK_EXEC_INVALIDATE_OPT, pcb);
    pcb.before_block_exec = before_block_exec;
    panda_register_callback(plugin_ptr, PANDA_CB_BEFORE_BLOCK_EXEC, pcb);
    pcb.after_block_exec = after_block_exec;
    panda_register_callback(plugin_ptr, PANDA_CB_AFTER_BLOCK_EXEC, pcb);
    pcb.phys_mem_read = phys_mem_read_callback;
    panda_register_callback(plugin_ptr, PANDA_CB_PHYS_MEM_READ, pcb);
    pcb.phys_mem_write = phys_mem_write_callback;
    panda_register_callback(plugin_ptr, PANDA_CB_PHYS_MEM_WRITE, pcb);
/*
    pcb.cb_cpu_restore_state = cb_cpu_restore_state;
    panda_register_callback(plugin_ptr, PANDA_CB_CPU_RESTORE_STATE, pcb);
    // for hd and network taint
    pcb.replay_hd_transfer = cb_replay_hd_transfer_taint;
    panda_register_callback(plugin_ptr, PANDA_CB_REPLAY_HD_TRANSFER, pcb);
    pcb.replay_net_transfer = cb_replay_net_transfer_taint;
    panda_register_callback(plugin_ptr, PANDA_CB_REPLAY_NET_TRANSFER, pcb);
    pcb.replay_before_cpu_physical_mem_rw_ram = cb_replay_cpu_physical_mem_rw_ram;
    panda_register_callback(plugin_ptr, PANDA_CB_REPLAY_BEFORE_CPU_PHYSICAL_MEM_RW_RAM, pcb);
*/
    panda_enable_precise_pc(); //before_block_exec requires precise_pc for panda_current_asid

    if (!execute_llvm){
        panda_enable_llvm();
    }
    panda_enable_llvm_helpers();

    /*
     * Taint processor initialization
     */

    shadow = tp_init(TAINT_BYTE_LABEL, TAINT_GRANULARITY_BYTE);
    if (shadow == NULL){
        printf("Error initializing shadow memory...\n");
        exit(1);
    }

    // Initialize memlog.
    memset(&taint_memlog, 0, sizeof(taint_memlog));

    llvm::Module *mod = tcg_llvm_ctx->getModule();
    FPM = tcg_llvm_ctx->getFunctionPassManager();

    // Add the taint analysis pass to our taint pass manager
    PTFP = new llvm::PandaTaintFunctionPass(shadow, &taint_memlog);
    FPM->add(PTFP);

    if (optimize_llvm) {
        printf("taint2: Adding default optimizations (-O1).\n");
        llvm::PassManagerBuilder Builder;
        Builder.OptLevel = 1;
        Builder.SizeLevel = 0;
        Builder.populateFunctionPassManager(*FPM);
    }

    FPM->doInitialization();

    // Populate module with helper function taint ops
    for (auto i = mod->begin(); i != mod->end(); i++){
        if (!i->isDeclaration()) PTFP->runOnFunction(*i);
    }

    printf("taint2: Done processing helper functions for taint.\n");

    std::string err;
    if(verifyModule(*mod, llvm::AbortProcessAction, &err)){
        printf("%s\n", err.c_str());
        exit(1);
    }

    tcg_llvm_write_module(tcg_llvm_ctx, "/tmp/llvm-mod.bc");

    printf("taint2: Done verifying module. Running...\n");
}

// Derive taint ops
int after_block_translate(CPUState *env, TranslationBlock *tb){

    if (taintEnabled){
        assert(tb->llvm_function);
        // taintfp will make sure it never runs twice.
        //FPM->run(*(tb->llvm_function));
        //tb->llvm_function->dump();
    }

    return 0;
}

// Execute taint ops
int after_block_exec(CPUState *env, TranslationBlock *tb,
        TranslationBlock *next_tb){

    if (taintJustDisabled){
        taintJustDisabled = false;
        execute_llvm = 0;
        generate_llvm = 0;
        panda_do_flush_tb();
        panda_disable_memcb();
	//	mytimer_start(ttimer);
        return 0;
    }

    return 0;
}

__attribute__((unused)) static void print_labels(uint32_t el, void *stuff) {
    printf("%d ", el);
}

__attribute__((unused)) static void record_bit(uint32_t el, void *array) {
    *(uint64_t *)array |= 1 << el;
}

#ifdef TARGET_ARM
// R0 is command (label or query)
// R1 is buf_start
// R2 is length
// R3 is offset (not currently implemented)
void arm_hypercall_callback(CPUState *env){
    //target_ulong buf_start = env->regs[1];
    //target_ulong buf_len = env->regs[2];

    if (env->regs[0] == 7 || env->regs[0] == 8){ //Taint label
        if (!taintEnabled){
            printf("Taint plugin: Label operation detected @ %lu\n", rr_get_guest_instr_count());
            printf("Enabling taint processing\n");
            __taint2_enable_taint();
        }

        // FIXME: do labeling here.
    }

    else if (env->regs[0] == 9){ //Query taint on label
        if (taintEnabled){
            printf("Taint plugin: Query operation detected @ %lu\n", rr_get_guest_instr_count());
            //Addr a = make_maddr(buf_start);
            //bufplot(env, shadow, &a, (int)buf_len);
        }
        //printf("Disabling taint processing\n");
        //taintEnabled = false;
        //taintJustDisabled = true;
        //printf("Label occurrences on HD: %d\n", shad_dir_occ_64(shadow->hd));
    }
}
#endif //TARGET_ARM


int label_spit(uint32_t el, void *stuff) {
    printf ("%d ", el);
    return 0;
}


#define MAX_EL_ARR_IND 1000000
uint32_t el_arr_ind = 0;

// used by hypercall to pack pandalog array with query result
int collect_query_labels_pandalog(uint32_t el, void *stuff) {
    uint32_t *label = (uint32_t *) stuff;
    assert (el_arr_ind < MAX_EL_ARR_IND);
    label[el_arr_ind++] = el;
    return 0;
}



static uint32_t ii = 0;


std::set < LabelSetP > ls_returned;

#ifdef TARGET_I386
// Support all features of label and query program
void i386_hypercall_callback(CPUState *env){
    //printf("taint2: Hypercall! B " TARGET_FMT_lx " C " TARGET_FMT_lx " D " TARGET_FMT_lx "\n",
    //        env->regs[R_EBX], env->regs[R_ECX], env->regs[R_EDX]);

    // Label op.
    // EBX contains addr of that data
    // ECX contains size of data
    // EDX contains the label; ~0UL for autoenc.
    if (env->regs[R_EAX] == 7 || env->regs[R_EAX] == 8){
        target_ulong addr = panda_virt_to_phys(env, env->regs[R_EBX]);
        target_ulong size = env->regs[R_ECX];
        target_ulong label = env->regs[R_EDX];
        if (!taintEnabled){
            printf("taint2: Label operation detected @ %lu\n",
                    rr_get_guest_instr_count());
            printf("taint2: Labeling " TARGET_FMT_lx " to " TARGET_FMT_lx
                    " with label " TARGET_FMT_lx ".\n", addr, addr + size, label);
            __taint2_enable_taint();
        }

        LabelSetP ls = NULL;
        if (label != (target_ulong)~0UL) {
            ls = label_set_singleton(label);
        } // otherwise autoinc.
        qemu_log_mask(CPU_LOG_TAINT_OPS, "label: %lx[%lx+%lx] <- %lx (%lx)\n",
                (uint64_t)shadow->ram, (uint64_t)addr, (uint64_t)size, (uint64_t)label,
                (uint64_t)ls);
        for (unsigned i = 0; i < size; i++) {
            //printf("label %u\n", i);
            shadow->ram->set(addr + i,
                    label_set_singleton(i));
        }
    }

    // Query op.
    // EBX contains addr.
    if (env->regs[R_EAX] == 9) {        
        if (taintEnabled && (taint2_num_labels_applied() > 0)){
            // phys addr
            target_ulong addr = panda_virt_to_phys(env, env->regs[R_EBX]);
            if ((int)addr == -1) {
                printf ("taint2: query of unmapped addr?:  vaddr=0x%x paddr=0x%x\n",
                        (uint32_t) EBX, (uint32_t) addr);
            }
            else {
                printf ("taint enabled AND we have applied some labels AND addr for this query isnt garbage\n");
                LabelSetP ls = tp_query_ram(shadow, addr);
                if (ls) {
                    if (pandalog) {
                        ii ++;
                        printf ("pandalogging taint query\n");
                        // we only want to actually write a particular set contents to pandalog once
                        if (ls_returned.count(ls) == 0) {
                            // this ls hasn't yet been written to pandalog
                            // write out mapping from ls pointer to labelset contents
                            // as its own separate log entry
                            ls_returned.insert(ls);
                            Panda__TaintQueryUniqueLabelSet *tquls = (Panda__TaintQueryUniqueLabelSet *) malloc (sizeof (Panda__TaintQueryUniqueLabelSet));
                            *tquls = PANDA__TAINT_QUERY_UNIQUE_LABEL_SET__INIT;
                            tquls->ptr = (uint64_t) ls;
                            tquls->n_label = ls_card(ls);
                            tquls->label = (uint32_t *) malloc (sizeof(uint32_t) * tquls->n_label);
                            el_arr_ind = 0;
                            tp_ls_iter(ls, collect_query_labels_pandalog, (void *) tquls->label);
                            Panda__LogEntry ple = PANDA__LOG_ENTRY__INIT;
                            ple.taint_query_unique_label_set = tquls;
                            pandalog_write_entry(&ple);
                            free (tquls->label);
                            free (tquls);
                        }
                        // safe to refer to the set by the pointer in this next message
                        Panda__TaintQuery *tq = (Panda__TaintQuery *) malloc(sizeof(Panda__TaintQuery));
                        *tq = PANDA__TAINT_QUERY__INIT;
                        tq->asid = panda_current_asid(env);
                        tq->ptr = (uint64_t) ls;
                        Panda__LogEntry ple = PANDA__LOG_ENTRY__INIT;
                        ple.taint_query = tq;
                        pandalog_write_entry(&ple);
                        free(tq);
                        if (ii == 100){
                            //                            panda_end_replay();
                        }
                    }
                    else {
                        printf("taint2: Query operation detected @ %lu. vaddr=0x%x paddr=0x%x\n",
                               rr_get_guest_instr_count(), (uint32_t) EBX, (uint32_t) addr);
                        
                        uint32_t num_labels = taint2_query_ram(addr);
                        printf("taint2: Queried %lx[%lx]\n", (uint64_t)shadow->ram,
                               (uint64_t)addr);
                        printf("taint2: %u labels.\n", num_labels);
                        qemu_log_mask(CPU_LOG_TAINT_OPS, "query: %lx[%lx]\n",
                                      (uint64_t)shadow->ram, (uint64_t)addr);
                        if (num_labels > 0) {
                            printf ("labels: ");
                            tp_ls_ram_iter(shadow, addr, label_spit, NULL);
                            printf ("\n");
                        }
                    }
                }
            }
        }
    }
    
    // LAVA Query op.
    // EBX contains addr.
    if (env->regs[R_EAX] == 11) {
        target_ulong addr = panda_virt_to_phys(env, env->regs[R_EBX]);
        PirateMarkLavaInfo pmli = {};
        char filenameStr[500];
        char astNodeStr[500];
        if (taintEnabled){
            printf("taint2: Query operation detected @ %lu.\n",
                    rr_get_guest_instr_count());
            //uint64_t array;
            //label_set_iter(FastShad::query(shadow->ram, addr), record_bit, &array);
            
            // TODO some way to do error checking/handling?
            panda_virtual_memory_rw(env, env->regs[R_ESI],
                (uint8_t*)&pmli, sizeof(PirateMarkLavaInfo), false);
            panda_virtual_memory_rw(env, pmli.filenamePtr,
                (uint8_t*)&filenameStr, 500, false);
            pmli.filenamePtr = (uint64_t)filenameStr;
            panda_virtual_memory_rw(env, pmli.astNodePtr,
                (uint8_t*)&astNodeStr, 500, false);
            pmli.astNodePtr = (uint64_t)astNodeStr;
            printf("LAVA Query Info:\n");
            printf("File name: %s\n", (char*)pmli.filenamePtr);
            printf("Line number: %lu\n", pmli.lineNum);
            printf("AST node: %s\n\n", (char*)pmli.astNodePtr);
            
            printf("taint2: %u labels.\n", taint2_query_ram(addr));
            printf("taint2: Queried %lx[%lx]\n", (uint64_t)shadow->ram,
                    (uint64_t)addr);
            qemu_log_mask(CPU_LOG_TAINT_OPS, "query: %lx[%lx]\n",
                    (uint64_t)shadow->ram, (uint64_t)addr);
            //label_set_iter(FastShad::query(shadow->ram, addr),
                    //print_labels, NULL);
            //printf("taint2: Stopping replay.\n");
            //rr_do_end_replay(0);
        }
    }
}
#endif // TARGET_I386

int guest_hypercall_callback(CPUState *env){
#ifdef TARGET_I386
    i386_hypercall_callback(env);
#endif

#ifdef TARGET_ARM
    arm_hypercall_callback(env);
#endif

    return 1;
}

// Called whenever the taint state changes.
void taint_state_changed() {
    PPP_RUN_CB(on_taint_change);
}

bool __taint2_enabled() {
    return taintEnabled;
}

// label this phys addr in memory with this label
void __taint2_label_ram(uint64_t pa, uint32_t l) {
    tp_label_ram(shadow, pa, l);
}

// if phys addr pa is untainted, return 0.
// else returns label set cardinality
uint32_t __taint2_query_ram(uint64_t pa) {
    LabelSetP ls = tp_query_ram(shadow, pa);
    return ls_card(ls);
}


uint32_t __taint2_query_reg(int reg_num, int offset) {
    LabelSetP ls = tp_query_reg(shadow, reg_num, offset);
    return ls_card(ls);
}

uint32_t __taint2_query_llvm(int reg_num, int offset) {
    LabelSetP ls = tp_query_llvm(shadow, reg_num, offset);
    return ls_card(ls);
}


uint32_t *__taint2_labels_applied(void) {
    return tp_labels_applied();
}

uint32_t __taint2_num_labels_applied(void) {
    return tp_num_labels_applied();
}




void __taint2_delete_ram(uint64_t pa) {
    tp_delete_ram(shadow, pa);
}

void __taint2_labelset_spit(LabelSetP ls) {
    std::set<uint32_t> rendered(label_set_render_set(ls));
    for (uint32_t l : rendered) {
        printf("%u ", l);
    }
    printf("\n");
}


void __taint2_labelset_iter(LabelSetP ls,  int (*app)(uint32_t el, void *stuff1), void *stuff2) {
    tp_ls_iter(ls, app, stuff2);
}



void __taint2_labelset_ram_iter(uint64_t pa, int (*app)(uint32_t el, void *stuff1), void *stuff2) {
    tp_ls_ram_iter(shadow, pa, app, stuff2);
}


void __taint2_labelset_reg_iter(int reg_num, int offset, int (*app)(uint32_t el, void *stuff1), void *stuff2) {
    tp_ls_reg_iter(shadow, reg_num, offset, app, stuff2);
}


void __taint2_labelset_llvm_iter(int reg_num, int offset, int (*app)(uint32_t el, void *stuff1), void *stuff2) {
    tp_ls_llvm_iter(shadow, reg_num, offset, app, stuff2);
}

void __taint2_track_taint_state(void) {
    track_taint_state = true;
}



////////////////////////////////////////////////////////////////////////////////////
// C API versions


void taint2_enable_taint(void) {
  __taint2_enable_taint();
}

int taint2_enabled(void) {
  return __taint2_enabled();
}

void taint2_label_ram(uint64_t pa, uint32_t l) {
    __taint2_label_ram(pa, l);
}

uint32_t taint2_query_ram(uint64_t pa) {
  return __taint2_query_ram(pa);
}

void taint2_delete_ram(uint64_t pa) {
  __taint2_delete_ram(pa);
}

uint32_t taint2_query_reg(int reg_num, int offset) {
  return __taint2_query_reg(reg_num, offset);
}

uint32_t taint2_query_llvm(int reg_num, int offset) {
  return __taint2_query_llvm(reg_num, offset);
}

void taint2_labelset_spit(LabelSetP ls) {
    return __taint2_labelset_spit(ls);
}

void taint2_labelset_iter(LabelSetP ls,  int (*app)(uint32_t el, void *stuff1), void *stuff2) {
    __taint2_labelset_iter(ls, app, stuff2);
}


void taint2_labelset_ram_iter(uint64_t pa, int (*app)(uint32_t el, void *stuff1), void *stuff2) {
    __taint2_labelset_ram_iter(pa, app, stuff2);
}


void taint2_labelset_reg_iter(int reg_num, int offset, int (*app)(uint32_t el, void *stuff1), void *stuff2) {
    __taint2_labelset_reg_iter(reg_num, offset, app, stuff2);
}


void taint2_labelset_llvm_iter(int reg_num, int offset, int (*app)(uint32_t el, void *stuff1), void *stuff2) {
    __taint2_labelset_llvm_iter(reg_num, offset, app, stuff2);
}

uint32_t *taint2_labels_applied(void) {
    return __taint2_labels_applied();
}


uint32_t taint2_num_labels_applied(void) {
    return __taint2_num_labels_applied();
}

void taint2_track_taint_state(void) {
    __taint2_track_taint_state();
}


////////////////////////////////////////////////////////////////////////////////////
int before_block_exec(CPUState *env, TranslationBlock *tb) {
    return 0;
}
bool before_block_exec_invalidate_opt(CPUState *env, TranslationBlock *tb) {
    //if (!taintEnabled) __taint_enable_taint();

#ifdef TAINTDEBUG
    //printf("%s\n", tcg_llvm_get_func_name(tb));
#endif

    if (taintEnabled) {
        if (!tb->llvm_tc_ptr) {
            return true;
        } else {
            //tb->llvm_function->dump();
            return false;
        }
    }
    return false;
}

bool init_plugin(void *self) {
    printf("Initializing taint plugin\n");
    plugin_ptr = self;
    panda_cb pcb;
    panda_enable_memcb();
    panda_disable_tb_chaining();
    pcb.guest_hypercall = guest_hypercall_callback;
    panda_register_callback(self, PANDA_CB_GUEST_HYPERCALL, pcb);
    pcb.before_block_exec_invalidate_opt = before_block_exec_invalidate_opt;
    panda_register_callback(self, PANDA_CB_BEFORE_BLOCK_EXEC_INVALIDATE_OPT, pcb);
    /*
    pcb.replay_handle_packet = handle_packet;
    panda_register_callback(plugin_ptr, PANDA_CB_REPLAY_HANDLE_PACKET, pcb);
    */

    panda_arg_list *args = panda_get_args("taint2");
    tainted_pointer = !panda_parse_bool(args, "no_tp");
    inline_taint = panda_parse_bool(args, "inline");
    if (inline_taint) {
        printf("taint2: Inlining taint ops by default.\n");
    } else {
        printf("taint2: Instructed not to inline taint ops.\n");
    }
    if (panda_parse_bool(args, "binary")) mode = TAINT_BINARY_LABEL;
    if (panda_parse_bool(args, "word")) granularity = TAINT_GRANULARITY_WORD;
    optimize_llvm = panda_parse_bool(args, "opt");

    return true;
}



void uninit_plugin(void *self) {

    printf ("uninit taint plugin\n");

    if (shadow) tp_free(shadow);

    panda_disable_llvm();
    panda_disable_memcb();
    panda_enable_tb_chaining();

}
