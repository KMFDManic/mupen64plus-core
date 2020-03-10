/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - r4300_core.c                                            *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2014 Bobby Smiles                                       *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include "r4300_core.h"
#include "cached_interp.h"
#if defined(COUNT_INSTR)
#include "instr_counters.h"
#endif
#include "new_dynarec/new_dynarec.h"
#include "pure_interp.h"
#include "recomp.h"

#include "api/callbacks.h"
#include "api/debugger.h"
#include "api/m64p_types.h"
#ifdef DBG
#include "debugger/dbg_debugger.h"
#endif
#include "main/main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void init_r4300(struct r4300_core* r4300, struct memory* mem, struct mi_controller* mi, struct rdram* rdram, const struct interrupt_handler* interrupt_handlers,
    unsigned int emumode, unsigned int count_per_op, int no_compiled_jump, int randomize_interrupt)
{
    struct new_dynarec_hot_state* new_dynarec_hot_state =
#ifdef NEW_DYNAREC
        &r4300->new_dynarec_hot_state;
#else
        NULL;
#endif

    r4300->emumode = emumode;
    init_cp0(&r4300->cp0, count_per_op, new_dynarec_hot_state, interrupt_handlers);
    init_cp1(&r4300->cp1, new_dynarec_hot_state);

#ifndef NEW_DYNAREC
    r4300->recomp.no_compiled_jump = no_compiled_jump;
#endif

    r4300->mem = mem;
    r4300->mi = mi;
    r4300->rdram = rdram;
    r4300->randomize_interrupt = randomize_interrupt;
    srand((unsigned int) time(NULL));
}

void poweron_r4300(struct r4300_core* r4300)
{
    /* clear registers */
    memset(r4300_regs(r4300), 0, 32*sizeof(int64_t));
    *r4300_mult_hi(r4300) = 0;
    *r4300_mult_lo(r4300) = 0;
    r4300->llbit = 0;

    *r4300_pc_struct(r4300) = NULL;
    r4300->delay_slot = 0;
    r4300->skip_jump = 0;
    r4300->reset_hard_job = 0;


    /* recomp init */
#ifndef NEW_DYNAREC
    r4300->recomp.delay_slot_compiled = 0;
    r4300->recomp.fast_memory = 1;
    r4300->recomp.local_rs = 0;
    r4300->recomp.dyna_interp = 0;
    r4300->recomp.jumps_table = NULL;
    r4300->recomp.jumps_number = 0;
    r4300->recomp.max_jumps_number = 0;
    r4300->recomp.jump_start8 = 0;
    r4300->recomp.jump_start32 = 0;
#if defined(__x86_64__)
    r4300->recomp.riprel_table = NULL;
    r4300->recomp.riprel_number = 0;
    r4300->recomp.max_riprel_number = 0;
#endif

#if defined(__x86_64__)
    r4300->recomp.save_rsp = 0;
    r4300->recomp.save_rip = 0;
#else
    r4300->recomp.save_ebp = 0;
    r4300->recomp.save_ebx = 0;
    r4300->recomp.save_esi = 0;
    r4300->recomp.save_edi = 0;
    r4300->recomp.save_esp = 0;
    r4300->recomp.save_eip = 0;
#endif

    r4300->recomp.branch_taken = 0;
#endif /* !NEW_DYNAREC */

    /* setup CP0 registers */
    poweron_cp0(&r4300->cp0);

    /* setup CP1 registers */
    poweron_cp1(&r4300->cp1);
}

LONG WINAPI ExceptionHandler(struct _EXCEPTION_POINTERS *ExceptionInfo)
{
	if (ExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
	{
        uintptr_t fault_addr = ExceptionInfo->ExceptionRecord->ExceptionInformation[1];

        if ((fault_addr < (uintptr_t)g_dev.r4300.mem->base)
            || (fault_addr >= (uintptr_t)g_dev.r4300.mem->base + 0x20000000))
            return EXCEPTION_EXECUTE_HANDLER;

		// TODO: fix non full mem base
		uint32_t offset = (uint32_t)(fault_addr - (uintptr_t)g_dev.r4300.mem->base);
		offset &= ~0xfff; // page aligned

		invalidate_r4300_cached_code(&g_dev.r4300, (0x80000000 + offset), 0x1000);
		invalidate_r4300_cached_code(&g_dev.r4300, (0xa0000000 + offset), 0x1000);

        /*TODO: handle r/w to framebuffer according to exception flags

                read exception:  Notify GFX that a protected page is currently being read.
                                 GFX should update the page with framebuffer content before continuing execution.

                write exception: Notify GFX that a protected page has been written.
                                 Continue execution.
                                 GFX should update the framebuffer content with data from dirty pages on next DoRspCycles*/
                                 
#define READ_FLAG 0
#define WRITE_FLAG 1
        int rw = ExceptionInfo->ExceptionRecord->ExceptionInformation[0];
        // TODO: FBInfo

		return EXCEPTION_CONTINUE_EXECUTION;
	}
	else
		return EXCEPTION_EXECUTE_HANDLER;
}

void run_r4300(struct r4300_core* r4300)
{
    *r4300_stop(r4300) = 0;
    g_rom_pause = 0;
	PVOID handler = AddVectoredExceptionHandler(1, ExceptionHandler);

    /* clear instruction counters */
#if defined(COUNT_INSTR)
    memset(instr_count, 0, 131*sizeof(instr_count[0]));
#endif

    if (r4300->emumode == EMUMODE_PURE_INTERPRETER)
    {
        freopen("interpreter.txt", "w+", stdout);
        DebugMessage(M64MSG_INFO, "Starting R4300 emulator: Pure Interpreter");
        r4300->emumode = EMUMODE_PURE_INTERPRETER;
        run_pure_interpreter(r4300);
    }
#if defined(DYNAREC)
    else if (r4300->emumode >= 2)
    {
        DebugMessage(M64MSG_INFO, "Starting R4300 emulator: Dynamic Recompiler");
        r4300->emumode = EMUMODE_DYNAREC;
        init_blocks(&r4300->cached_interp);
#ifdef NEW_DYNAREC
        new_dynarec_init();
        new_dyna_start();
        new_dynarec_cleanup();
#else
#if defined(__x86_64__)
        freopen("dynarec_x64.txt", "w+", stdout);
#else
        freopen("dynarec_x86.txt", "w+", stdout);
#endif
        r4300->cached_interp.fin_block = dynarec_fin_block;
        r4300->cached_interp.not_compiled = dynarec_notcompiled;
        r4300->cached_interp.not_compiled2 = dynarec_notcompiled2;
        r4300->cached_interp.init_block = dynarec_init_block;
        r4300->cached_interp.free_block = dynarec_free_block;
        r4300->cached_interp.recompile_block = dynarec_recompile_block;


        dyna_start(dynarec_setup_code);
        (*r4300_pc_struct(r4300))++;
#if defined(PROFILE_R4300)
        profile_write_end_of_code_blocks(r4300);
#endif
#endif
        free_blocks(&r4300->cached_interp);
    }
#endif
    else /* if (r4300->emumode == EMUMODE_INTERPRETER) */
    {
        freopen("cached_interpreter.txt", "w+", stdout);
        DebugMessage(M64MSG_INFO, "Starting R4300 emulator: Cached Interpreter");
        r4300->emumode = EMUMODE_INTERPRETER;
        r4300->cached_interp.fin_block = cached_interp_FIN_BLOCK;
        r4300->cached_interp.not_compiled = cached_interp_NOTCOMPILED;
        r4300->cached_interp.not_compiled2 = cached_interp_NOTCOMPILED2;
        r4300->cached_interp.init_block = cached_interp_init_block;
        r4300->cached_interp.free_block = cached_interp_free_block;
        r4300->cached_interp.recompile_block = cached_interp_recompile_block;

        init_blocks(&r4300->cached_interp);
        cached_interpreter_jump_to(r4300, UINT32_C(0xa4000040));

        /* Prevent segfault on failed cached_interpreter_jump_to */
        if (!r4300->cached_interp.actual->block) {
            return;
        }

        r4300->cp0.last_addr = *r4300_pc(r4300);

        run_cached_interpreter(r4300);

        free_blocks(&r4300->cached_interp);
    }

	RemoveVectoredExceptionHandler(handler);
    DebugMessage(M64MSG_INFO, "R4300 emulator finished.");

    /* print instruction counts */
#if defined(COUNT_INSTR)
    if (r4300->emumode == EMUMODE_DYNAREC)
        instr_counters_print();
#endif
}

int64_t* r4300_regs(struct r4300_core* r4300)
{
#ifndef NEW_DYNAREC
    return r4300->regs;
#else
    return r4300->new_dynarec_hot_state.regs;
#endif
}

int64_t* r4300_mult_hi(struct r4300_core* r4300)
{
#ifndef NEW_DYNAREC
    return &r4300->hi;
#else
    return &r4300->new_dynarec_hot_state.hi;
#endif
}

int64_t* r4300_mult_lo(struct r4300_core* r4300)
{
#ifndef NEW_DYNAREC
    return &r4300->lo;
#else
    return &r4300->new_dynarec_hot_state.lo;
#endif
}

unsigned int* r4300_llbit(struct r4300_core* r4300)
{
    return &r4300->llbit;
}

uint32_t* r4300_pc(struct r4300_core* r4300)
{
#ifdef NEW_DYNAREC
	if (r4300->emumode == EMUMODE_DYNAREC)
		return (uint32_t*)&r4300->new_dynarec_hot_state.pcaddr;
	else
#endif
	if (r4300->emumode != EMUMODE_PURE_INTERPRETER)
        r4300->pcaddr = get_instruction_addr(r4300 , *r4300_pc_struct(r4300));

    return &r4300->pcaddr;
}

struct precomp_instr** r4300_pc_struct(struct r4300_core* r4300)
{
#ifndef NEW_DYNAREC
    return &r4300->pc;
#else
    return &r4300->new_dynarec_hot_state.pc;
#endif
}

int* r4300_stop(struct r4300_core* r4300)
{
#ifndef NEW_DYNAREC
    return &r4300->stop;
#else
    return &r4300->new_dynarec_hot_state.stop;
#endif
}

unsigned int get_r4300_emumode(struct r4300_core* r4300)
{
    return r4300->emumode;
}

uint32_t *fast_mem_access(struct r4300_core* r4300, uint32_t address)
{
    /* This code is performance critical, specially on pure interpreter mode.
     * Removing error checking saves some time, but the emulator may crash. */

    if ((address & UINT32_C(0xc0000000)) != UINT32_C(0x80000000)) {
        address = virtual_to_physical_address(r4300, address, 2);
        if (address == 0) // TLB exception
            return NULL;
    }

    address &= UINT32_C(0x1ffffffc);

    return mem_base_u32(r4300->mem->base, address);
}

/* Read aligned word from memory.
 * address may not be word-aligned for byte or hword accesses.
 * Alignment is taken care of when calling mem handler.
 */
int r4300_read_aligned_word(struct r4300_core* r4300, uint32_t address, uint32_t* value)
{
    if ((address & UINT32_C(0xc0000000)) != UINT32_C(0x80000000)) {
        address = virtual_to_physical_address(r4300, address, 0);
        if (address == 0) {
            return 0;
        }
    }

    address &= UINT32_C(0x1ffffffc);

    mem_read32(mem_get_handler(r4300->mem, address), address & ~UINT32_C(3), value);

    return 1;
}

/* Read aligned dword from memory */
int r4300_read_aligned_dword(struct r4300_core* r4300, uint32_t address, uint64_t* value)
{
    uint32_t w[2];

    /* XXX: unaligned dword accesses should trigger a address error,
     * but inaccurate timing of the core can lead to unaligned address on reset
     * so just emit a warning and keep going */
    if ((address & 0x7) != 0) {
        DebugMessage(M64MSG_WARNING, "Unaligned dword read %08x", address);
    }

    if ((address & UINT32_C(0xc0000000)) != UINT32_C(0x80000000)) {
        address = virtual_to_physical_address(r4300, address, 0);
        if (address == 0) {
            return 0;
        }
    }

    address &= UINT32_C(0x1ffffffc);

    const struct mem_handler* handler = mem_get_handler(r4300->mem, address);
    mem_read32(handler, address + 0, &w[0]);
    mem_read32(handler, address + 4, &w[1]);

    *value = ((uint64_t)w[0] << 32) | w[1];

    return 1;
}

/* Write aligned word to memory.
 * address may not be word-aligned for byte or hword accesses.
 * Alignment is taken care of when calling mem handler.
 */
int r4300_write_aligned_word(struct r4300_core* r4300, uint32_t address, uint32_t value, uint32_t mask)
{
    if ((address & UINT32_C(0xc0000000)) != UINT32_C(0x80000000)) {

        address = virtual_to_physical_address(r4300, address, 1);
        if (address == 0) {
            return 0;
        }
    }

    address &= UINT32_C(0x1ffffffc);

    mem_write32(mem_get_handler(r4300->mem, address), address & ~UINT32_C(3), value, mask);

    return 1;
}

/* Write aligned dword to memory */
int r4300_write_aligned_dword(struct r4300_core* r4300, uint32_t address, uint64_t value, uint64_t mask)
{
    /* XXX: unaligned dword accesses should trigger a address error,
     * but inaccurate timing of the core can lead to unaligned address on reset
     * so just emit a warning and keep going */
    if ((address & 0x7) != 0) {
        DebugMessage(M64MSG_WARNING, "Unaligned dword write %08x", address);
    }

    if ((address & UINT32_C(0xc0000000)) != UINT32_C(0x80000000)) {

        address = virtual_to_physical_address(r4300, address, 1);
        if (address == 0) {
            return 0;
        }
    }

    address &= UINT32_C(0x1ffffffc);

    const struct mem_handler* handler = mem_get_handler(r4300->mem, address);
    mem_write32(handler, address + 0, value >> 32,      mask >> 32);
    mem_write32(handler, address + 4, (uint32_t) value, (uint32_t) mask      );

    return 1;
}

void invalidate_r4300_cached_code(struct r4300_core* r4300, uint32_t address, size_t size)
{
    if (r4300->emumode != EMUMODE_PURE_INTERPRETER)
    {
#ifdef NEW_DYNAREC
        if (r4300->emumode == EMUMODE_DYNAREC)
        {
            invalidate_cached_code_new_dynarec(r4300, address, size);
        }
        else
#endif
        {
            invalidate_cached_code_hacktarux(r4300, address, size);
        }
    }
}


void generic_jump_to(struct r4300_core* r4300, uint32_t address)
{
    switch(r4300->emumode)
    {
    case EMUMODE_PURE_INTERPRETER:
        *r4300_pc(r4300) = address;
        break;

    case EMUMODE_INTERPRETER:
        cached_interpreter_jump_to(r4300, address);
        break;

#ifndef NO_ASM
    case EMUMODE_DYNAREC:
#ifdef NEW_DYNAREC
        r4300->new_dynarec_hot_state.pcaddr = address;
        r4300->new_dynarec_hot_state.pending_exception = 1;
#else
        dynarec_jump_to(r4300, address);
#endif
        break;
#endif

    default:
        /* should not happen */
        break;
    }
}


/* XXX: not really a good interface but it gets the job done... */
void savestates_load_set_pc(struct r4300_core* r4300, uint32_t pc)
{
    generic_jump_to(r4300, pc);
    invalidate_r4300_cached_code(r4300, 0, 0);
}

static const char cop0name[32][32] = {
  "CP0_INDEX_REG",
  "CP0_RANDOM_REG",
  "CP0_ENTRYLO0_REG",
  "CP0_ENTRYLO1_REG",
  "CP0_CONTEXT_REG",
  "CP0_PAGEMASK_REG",
  "CP0_WIRED_REG",
  "7",
  "CP0_BADVADDR_REG",
  "CP0_COUNT_REG",
  "CP0_ENTRYHI_REG",
  "CP0_COMPARE_REG",
  "CP0_STATUS_REG",
  "CP0_CAUSE_REG",
  "CP0_EPC_REG",
  "CP0_PREVID_REG",
  "CP0_CONFIG_REG",
  "CP0_LLADDR_REG",
  "CP0_WATCHLO_REG",
  "CP0_WATCHHI_REG",
  "CP0_XCONTEXT_REG",
  "21",
  "22",
  "23",
  "24",
  "25",
  "26",
  "27",
  "CP0_TAGLO_REG",
  "CP0_TAGHI_REG",
  "CP0_ERROREPC_REG",
  "31"
};

static int r64_checksum(int64_t* regs)
{
    int i;
    int sum = 0;
    for (i = 0; i < 64; i++)
        sum ^= ((u_int*)regs)[i];
    return sum;
}

static int r32_checksum(int* regs)
{
    int i;
    int sum = 0;
    for (i = 0; i < 32; i++)
        sum ^= regs[i];
    return sum;
}

static int rdram_checksum(int extramem)
{
    int i;
    int sum = 0;
    int size = (extramem) ? 2097152 : 1048576;
    for (i = 0; i < size; i++) {
        unsigned int temp = sum;
        sum <<= 1;
        sum |= (~temp) >> 31;
        sum ^= ((u_int*)g_dev.rdram.dram)[i];
    }
    return sum;
}

void print_state(struct r4300_core* r4300, int force)
{
    static int allow_print = 0;
    struct cp0* cp0 = &r4300->cp0;
    struct cp1* cp1 = &r4300->cp1;
    int pcaddr = 0;

    if (r4300->cached_interp.actual != NULL && (*r4300_pc_struct(r4300) != NULL || (r4300->emumode == EMUMODE_DYNAREC)))
    {
        pcaddr = *r4300_pc(r4300);
        cp0_update_count(r4300);
    }

    // Last good
    /*if ((pcaddr == 0x8031b0ac) && (r4300_cp0_regs(cp0)[CP0_COUNT_REG] == 0x0ff69ca0))
        allow_print = 1;*/

    // First bad
    /*if ((pcaddr == 0x8019fd0c) && (r4300_cp0_regs(cp0)[CP0_COUNT_REG] == 0x0ff8a802))
        allow_print = 0;*/

    if (!(force || allow_print)) return;
    
    int i;
    fprintf(stdout, "ds: %d\n", r4300->delay_slot);
    fprintf(stdout, "pcaddr: 0x%08x\n", pcaddr);
    fprintf(stdout, "fcr0: 0x%08x\n", *r4300_cp1_fcr0(cp1));
    fprintf(stdout, "fcr31: 0x%08x\n", *r4300_cp1_fcr31(cp1));
    fprintf(stdout, "hi: 0x%llx\n", *r4300_mult_hi(r4300));
    fprintf(stdout, "lo: 0x%llx\n", *r4300_mult_lo(r4300));
    fprintf(stdout, "regs: 0x%08x\n", r64_checksum(r4300_regs(r4300)));
    fprintf(stdout, "cop0: 0x%08x\n", r32_checksum((int*)r4300_cp0_regs(cp0)));
    fprintf(stdout, "cop1_simple: 0x%08x\n", r64_checksum((int64_t*)*r4300_cp1_regs_simple(cp1)));
    fprintf(stdout, "cop1_double: 0x%08x\n", r64_checksum((int64_t*)*r4300_cp1_regs_double(cp1)));
    //fprintf(stdout,"rdram: 0x%08x\n", rdram_checksum(0));
    fprintf(stdout, "\n");
    
    //if(0)
    {
        for (i = 0; i < 32; i++)
            fprintf(stdout, "regs[%d]:0x%016llx\n", i, r4300_regs(r4300)[i]);
    
        fprintf(stdout, "\n");
    
        for (i = 0; i < 32; i++)
            fprintf(stdout, "cop0[%s]:0x%08x\n", cop0name[i], r4300_cp0_regs(cp0)[i]);
        
        fprintf(stdout, "\n");
        
        for (i = 0; i < 32; i++)
            fprintf(stdout, "cop1_simple[%d]:%lf\n", i, *r4300_cp1_regs_simple(cp1)[i]);
        
        fprintf(stdout, "\n");
        
        for (i = 0; i < 32; i++)
            fprintf(stdout, "cop1_double[%d]:%lf\n", i, *r4300_cp1_regs_double(cp1)[i]);
        
        fprintf(stdout, "\n");
    }
    fflush(stdout);
}