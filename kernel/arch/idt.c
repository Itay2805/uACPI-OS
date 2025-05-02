#include "idt.h"

#include <stdbool.h>
#include <debug/debug.h>
#include <mem/phys.h>
#include <mem/virt.h>
#include <sync/spinlock.h>
#include <thread/pcpu.h>
#include <time/tsc.h>

#include "apic.h"
#include "lib/defs.h"
#include "debug/log.h"
#include "intrin.h"
#include "regs.h"
#include "thread/intr.h"
#include "thread/scheduler.h"

#define IDT_TYPE_TASK           0x5
#define IDT_TYPE_INTERRUPT_16   0x6
#define IDT_TYPE_TRAP_16        0x7
#define IDT_TYPE_INTERRUPT_32   0xE
#define IDT_TYPE_TRAP_32        0xF

typedef struct idt_entry {
    uint64_t handler_low : 16;
    uint64_t selector : 16;
    uint64_t ist : 3;
    uint64_t _zero1 : 5;
    uint64_t gate_type : 4;
    uint64_t _zero2 : 1;
    uint64_t ring : 2;
    uint64_t present : 1;
    uint64_t handler_high : 48;
    uint64_t _zero3 : 32;
} PACKED idt_entry_t;

typedef struct idt {
    uint16_t limit;
    idt_entry_t* base;
} PACKED idt_t;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Exception handling - has a bunch of code to save registers so we can debug more easily
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct exception_context {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t int_num;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    rflags_t rflags;
    uint64_t rsp;
    uint64_t ss;
} exception_context_t;

typedef union page_fault_error {
    struct {
        uint32_t present : 1;
        uint32_t write : 1;
        uint32_t user : 1;
        uint32_t reserved_write : 1;
        uint32_t instruction_fetch : 1;
        uint32_t protection_key : 1;
        uint32_t shadow_stack : 1;
        uint32_t sgx : 1;
    };
    uint32_t packed;
} PACKED page_fault_error_t;

typedef union selector_error_code {
    struct {
        uint32_t e : 1;
        uint32_t tbl : 2;
        uint32_t index : 13;
    };
    uint32_t packed;
} PACKED selector_error_code_t;

/**
 * Forward declare the exception handler
 */
static void common_exception_handler(exception_context_t* ctx);

#define EXCEPTION_STUB(num) \
    __attribute__((naked)) \
    static void exception_handler_##num() { \
        __asm__( \
            "pushq $0\n" \
            "pushq $" #num "\n" \
            "jmp common_exception_stub"); \
    }

#define EXCEPTION_ERROR_STUB(num) \
    __attribute__((naked)) \
    static void exception_handler_##num() { \
        __asm__( \
            "pushq $" #num "\n" \
            "jmp common_exception_stub"); \
    }

EXCEPTION_STUB(0x00);
EXCEPTION_STUB(0x01);
EXCEPTION_STUB(0x02);
EXCEPTION_STUB(0x03);
EXCEPTION_STUB(0x04);
EXCEPTION_STUB(0x05);
EXCEPTION_STUB(0x06);
EXCEPTION_STUB(0x07);
EXCEPTION_ERROR_STUB(0x08);
EXCEPTION_STUB(0x09);
EXCEPTION_ERROR_STUB(0x0A);
EXCEPTION_ERROR_STUB(0x0B);
EXCEPTION_ERROR_STUB(0x0C);
EXCEPTION_ERROR_STUB(0x0D);
EXCEPTION_ERROR_STUB(0x0E);
EXCEPTION_STUB(0x0F);
EXCEPTION_STUB(0x10);
EXCEPTION_ERROR_STUB(0x11);
EXCEPTION_STUB(0x12);
EXCEPTION_STUB(0x13);
EXCEPTION_STUB(0x14);
EXCEPTION_ERROR_STUB(0x15);
EXCEPTION_STUB(0x16);
EXCEPTION_STUB(0x17);
EXCEPTION_STUB(0x18);
EXCEPTION_STUB(0x19);
EXCEPTION_STUB(0x1A);
EXCEPTION_STUB(0x1B);
EXCEPTION_STUB(0x1C);
EXCEPTION_ERROR_STUB(0x1D);
EXCEPTION_ERROR_STUB(0x1E);
EXCEPTION_STUB(0x1F);

__asm__ (
    ".global common_exception_stub\n"
    "common_exception_stub:\n"
    ".cfi_startproc simple\n"
    ".cfi_signal_frame\n"
    ".cfi_def_cfa %rsp, 0\n"
    ".cfi_offset %rip, 16\n"
    ".cfi_offset %rsp, 40\n"
    "cld\n"
    "pushq %rax\n"
    ".cfi_adjust_cfa_offset 8\n"
    ".cfi_rel_offset %rax, 0\n"
    "pushq %rbx\n"
    ".cfi_adjust_cfa_offset 8\n"
    ".cfi_rel_offset %rbx, 0\n"
    "pushq %rcx\n"
    ".cfi_adjust_cfa_offset 8\n"
    ".cfi_rel_offset %rcx, 0\n"
    "pushq %rdx\n"
    ".cfi_adjust_cfa_offset 8\n"
    ".cfi_rel_offset %rdx, 0\n"
    "pushq %rsi\n"
    ".cfi_adjust_cfa_offset 8\n"
    ".cfi_rel_offset %rsi, 0\n"
    "pushq %rdi\n"
    ".cfi_adjust_cfa_offset 8\n"
    ".cfi_rel_offset %rdi, 0\n"
    "pushq %rbp\n"
    ".cfi_adjust_cfa_offset 8\n"
    ".cfi_rel_offset %rbp, 0\n"
    "pushq %r8\n"
    ".cfi_adjust_cfa_offset 8\n"
    ".cfi_rel_offset %r8, 0\n"
    "pushq %r9\n"
    ".cfi_adjust_cfa_offset 8\n"
    ".cfi_rel_offset %r9, 0\n"
    "pushq %r10\n"
    ".cfi_adjust_cfa_offset 8\n"
    ".cfi_rel_offset %r10, 0\n"
    "pushq %r11\n"
    ".cfi_adjust_cfa_offset 8\n"
    ".cfi_rel_offset %r11, 0\n"
    "pushq %r12\n"
    ".cfi_adjust_cfa_offset 8\n"
    ".cfi_rel_offset %r12, 0\n"
    "pushq %r13\n"
    ".cfi_adjust_cfa_offset 8\n"
    ".cfi_rel_offset %r13, 0\n"
    "pushq %r14\n"
    ".cfi_adjust_cfa_offset 8\n"
    ".cfi_rel_offset %r14, 0\n"
    "pushq %r15\n"
    ".cfi_adjust_cfa_offset 8\n"
    ".cfi_rel_offset %r15, 0\n"
    "movq %rsp, %rdi\n"
    "call common_exception_handler\n"
    "popq %r15\n"
    ".cfi_adjust_cfa_offset -8\n"
    ".cfi_restore %r15\n"
    "popq %r14\n"
    ".cfi_adjust_cfa_offset -8\n"
    ".cfi_restore %r14\n"
    "popq %r13\n"
    ".cfi_adjust_cfa_offset -8\n"
    ".cfi_restore %r13\n"
    "popq %r12\n"
    ".cfi_adjust_cfa_offset -8\n"
    ".cfi_restore %r12\n"
    "popq %r11\n"
    ".cfi_adjust_cfa_offset -8\n"
    ".cfi_restore %r11\n"
    "popq %r10\n"
    ".cfi_adjust_cfa_offset -8\n"
    ".cfi_restore %r10\n"
    "popq %r9\n"
    ".cfi_adjust_cfa_offset -8\n"
    ".cfi_restore %r9\n"
    "popq %r8\n"
    ".cfi_adjust_cfa_offset -8\n"
    ".cfi_restore %r8\n"
    "popq %rbp\n"
    ".cfi_adjust_cfa_offset -8\n"
    ".cfi_restore %rbp\n"
    "popq %rdi\n"
    ".cfi_adjust_cfa_offset -8\n"
    ".cfi_restore %rdi\n"
    "popq %rsi\n"
    ".cfi_adjust_cfa_offset -8\n"
    ".cfi_restore %rsi\n"
    "popq %rdx\n"
    ".cfi_adjust_cfa_offset -8\n"
    ".cfi_restore %rdx\n"
    "popq %rcx\n"
    ".cfi_adjust_cfa_offset -8\n"
    ".cfi_restore %rcx\n"
    "popq %rbx\n"
    ".cfi_adjust_cfa_offset -8\n"
    ".cfi_restore %rbx\n"
    "popq %rax\n"
    ".cfi_adjust_cfa_offset -8\n"
    ".cfi_restore %rax\n"
    "addq $16, %rsp\n"
    ".cfi_adjust_cfa_offset -16\n"
    "iretq\n"
    ".cfi_endproc\n"
);

/**
 * Pretty print exception names
 */
static const char* m_exception_names[] = {
    "#DE - Division Error",
    "#DB - Debug",
    "Non-maskable Interrupt",
    "#BP - Breakpoint",
    "#OF - Overflow",
    "#BR - Bound Range Exceeded",
    "#UD - Invalid Opcode",
    "#NM - Device Not Available",
    "#DF - Double Fault",
    "Coprocessor Segment Overrun",
    "#TS - Invalid TSS",
    "#NP - Segment Not Present",
    "#SS - Stack-Segment Fault",
    "#GP - General Protection Fault",
    "#PF - Page Fault",
    "Reserved",
    "#MF - x87 Floating-Point Exception",
    "#AC - Alignment Check",
    "#MC - Machine Check",
    "#XM/#XF - SIMD Floating-Point Exception",
    "#VE - Virtualization Exception",
    "#CP - Control Protection Exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "#HV - Hypervisor Injection Exception",
    "#VC - VMM Communication Exception",
    "#SX - Security Exception",
    "Reserved",
};
STATIC_ASSERT(ARRAY_LENGTH(m_exception_names) == 32);

static spinlock_t m_exception_lock = INIT_SPINLOCK();

/**
 * The default exception handler, simply panics...
 */
static void default_exception_handler(exception_context_t* ctx) {
    spinlock_lock(&m_exception_lock);

    // reset the spinlock so we can print
    ERROR("");
    ERROR("****************************************************");
    ERROR("Exception occurred: %s (%ld)", m_exception_names[ctx->int_num], ctx->error_code);
    ERROR("****************************************************");
    ERROR("");

    page_fault_error_t page_fault_code = {};
    if (ctx->int_num == 0x0E) {
        page_fault_code = (page_fault_error_t) { .packed = ctx->error_code };
        if (page_fault_code.reserved_write) {
            ERROR("one or more page directory entries contain reserved bits which are set to 1");
        } else if (page_fault_code.instruction_fetch) {
            ERROR("tried to run non-executable code");
        } else {
            const char* rw = page_fault_code.write ? "write to" : "read from";
            if (!page_fault_code.present) {
                ERROR("%s non-present page", rw);
            } else {
                ERROR("page-protection violation when %s page", rw);
            }
        }
        ERROR("");
    } else if (ctx->int_num == 0x0D && ctx->error_code != 0) {
        selector_error_code_t selector = (selector_error_code_t) { .packed = ctx->error_code };
        static const char* table[] = {
            "GDT",
            "IDT",
            "LDT",
            "IDT"
        };
        ERROR("Accessing %s[%d]", table[selector.tbl], selector.index);
        ERROR("");
    }

    // check if we have threading_old already
    if (__rdmsr(MSR_IA32_FS_BASE) != 0) {
        // thread_t* thread = scheduler_get_current_thread();
        // if (thread != NULL) {
        //     ERROR("%p", thread);
        //     ERROR("Thread: `%.*s`", sizeof(thread->name), thread->name);
        // } else {
        //     ERROR("Thread: <none>");
        // }
    }
    ERROR("CPU: #%d", get_cpu_id());
    ERROR("");

    // registers
    ERROR("RAX=%016lx RBX=%016lx RCX=%016lx RDX=%016lx", ctx->rax, ctx->rbx, ctx->rcx, ctx->rdx);
    ERROR("RSI=%016lx RDI=%016lx RBP=%016lx RSP=%016lx", ctx->rsi, ctx->rdi, ctx->rbp, ctx->rsp);
    ERROR("R8 =%016lx R9 =%016lx R10=%016lx R11=%016lx", ctx->r8 , ctx->r9 , ctx->r10, ctx->r11);
    ERROR("R12=%016lx R13=%016lx R14=%016lx R15=%016lx", ctx->r12, ctx->r13, ctx->r14, ctx->r15);
    ERROR("RIP=%016lx RFL=%lx", ctx->rip, ctx->rflags.packed);
    ERROR("FS =%016lx GS =%016lx", __rdmsr(MSR_IA32_FS_BASE), 0ul); // TODO: GS BASE
    ERROR("CR0=%08lx CR2=%016lx CR3=%016lx CR4=%08lx", __readcr0(), __readcr2(), __readcr3(), __readcr4());

    ERROR("");

    // print the opcode for nicer debugging
    char buffer[256] = { 0 };
    debug_format_symbol(ctx->rip, buffer, sizeof(buffer));
    ERROR("Code: %s", buffer);
    // debug_disasm_at((void*)ctx->rip, 5);
    ERROR("");

    // stack trace
    ERROR("Stack trace:");
    size_t* base_ptr = (size_t*)ctx->rbp;

    int depth = 0;
    uintptr_t last_ret = 0;

    // if you want to print the assembly of a specific stack trace entry set this (start from 1)
    int to_print = 0;
    while (true) {
        if (!virt_is_mapped((uintptr_t)base_ptr)) {
            ERROR("\t%p is unmapped!", base_ptr);
            break;
        }

        size_t old_bp = base_ptr[0];
        size_t ret_addr = base_ptr[1];
        if (ret_addr == 0) {
            break;
        }

        if (last_ret == ret_addr) {
            depth++;
        } else {
            if (depth > 1) {
                ERROR("\t  ... repeating %d times", depth - 1);
            }

            last_ret = ret_addr;
            depth = 1;

            debug_format_symbol(ret_addr, buffer, sizeof(buffer));
            ERROR("\t> %s (0x%lx)", buffer, ret_addr);

            to_print--;
            if (to_print == 0) {
                // debug_disasm_at((void*)ret_addr, 5);
            }
        }

        if (old_bp == 0) {
            break;
        } else if (old_bp <= (size_t)base_ptr) {
            ERROR("\tGoes back to %lx", old_bp);
            break;
        }
        base_ptr = (size_t*)old_bp;
    }

    ERROR("");

    // stop
    ERROR("Halting :(");
    spinlock_unlock(&m_exception_lock);

    asm("cli");
    asm("hlt");
}

__attribute__((used))
static void common_exception_handler(exception_context_t* ctx) {
    // special case for page
    if (ctx->int_num == EXCEPT_IA32_PAGE_FAULT) {
        if (virt_handle_page_fault(__readcr2())) {
            return;
        }
    }

    // no one handled it, panic
    default_exception_handler(ctx);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// IRQs
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define IRQ_STUB(num) \
    __attribute__((interrupt)) \
    static void interrupt_handler_##num(interrupt_frame_t* frame) { \
        irq_dispatch(num); \
        lapic_eoi(); \
    }

IRQ_STUB(0x21)
IRQ_STUB(0x22)
IRQ_STUB(0x23)
IRQ_STUB(0x24)
IRQ_STUB(0x25)
IRQ_STUB(0x26)
IRQ_STUB(0x27)
IRQ_STUB(0x28)
IRQ_STUB(0x29)
IRQ_STUB(0x2a)
IRQ_STUB(0x2b)
IRQ_STUB(0x2c)
IRQ_STUB(0x2d)
IRQ_STUB(0x2e)
IRQ_STUB(0x2f)
IRQ_STUB(0x30)
IRQ_STUB(0x31)
IRQ_STUB(0x32)
IRQ_STUB(0x33)
IRQ_STUB(0x34)
IRQ_STUB(0x35)
IRQ_STUB(0x36)
IRQ_STUB(0x37)
IRQ_STUB(0x38)
IRQ_STUB(0x39)
IRQ_STUB(0x3a)
IRQ_STUB(0x3b)
IRQ_STUB(0x3c)
IRQ_STUB(0x3d)
IRQ_STUB(0x3e)
IRQ_STUB(0x3f)
IRQ_STUB(0x40)
IRQ_STUB(0x41)
IRQ_STUB(0x42)
IRQ_STUB(0x43)
IRQ_STUB(0x44)
IRQ_STUB(0x45)
IRQ_STUB(0x46)
IRQ_STUB(0x47)
IRQ_STUB(0x48)
IRQ_STUB(0x49)
IRQ_STUB(0x4a)
IRQ_STUB(0x4b)
IRQ_STUB(0x4c)
IRQ_STUB(0x4d)
IRQ_STUB(0x4e)
IRQ_STUB(0x4f)
IRQ_STUB(0x50)
IRQ_STUB(0x51)
IRQ_STUB(0x52)
IRQ_STUB(0x53)
IRQ_STUB(0x54)
IRQ_STUB(0x55)
IRQ_STUB(0x56)
IRQ_STUB(0x57)
IRQ_STUB(0x58)
IRQ_STUB(0x59)
IRQ_STUB(0x5a)
IRQ_STUB(0x5b)
IRQ_STUB(0x5c)
IRQ_STUB(0x5d)
IRQ_STUB(0x5e)
IRQ_STUB(0x5f)
IRQ_STUB(0x60)
IRQ_STUB(0x61)
IRQ_STUB(0x62)
IRQ_STUB(0x63)
IRQ_STUB(0x64)
IRQ_STUB(0x65)
IRQ_STUB(0x66)
IRQ_STUB(0x67)
IRQ_STUB(0x68)
IRQ_STUB(0x69)
IRQ_STUB(0x6a)
IRQ_STUB(0x6b)
IRQ_STUB(0x6c)
IRQ_STUB(0x6d)
IRQ_STUB(0x6e)
IRQ_STUB(0x6f)
IRQ_STUB(0x70)
IRQ_STUB(0x71)
IRQ_STUB(0x72)
IRQ_STUB(0x73)
IRQ_STUB(0x74)
IRQ_STUB(0x75)
IRQ_STUB(0x76)
IRQ_STUB(0x77)
IRQ_STUB(0x78)
IRQ_STUB(0x79)
IRQ_STUB(0x7a)
IRQ_STUB(0x7b)
IRQ_STUB(0x7c)
IRQ_STUB(0x7d)
IRQ_STUB(0x7e)
IRQ_STUB(0x7f)
IRQ_STUB(0x80)
IRQ_STUB(0x81)
IRQ_STUB(0x82)
IRQ_STUB(0x83)
IRQ_STUB(0x84)
IRQ_STUB(0x85)
IRQ_STUB(0x86)
IRQ_STUB(0x87)
IRQ_STUB(0x88)
IRQ_STUB(0x89)
IRQ_STUB(0x8a)
IRQ_STUB(0x8b)
IRQ_STUB(0x8c)
IRQ_STUB(0x8d)
IRQ_STUB(0x8e)
IRQ_STUB(0x8f)
IRQ_STUB(0x90)
IRQ_STUB(0x91)
IRQ_STUB(0x92)
IRQ_STUB(0x93)
IRQ_STUB(0x94)
IRQ_STUB(0x95)
IRQ_STUB(0x96)
IRQ_STUB(0x97)
IRQ_STUB(0x98)
IRQ_STUB(0x99)
IRQ_STUB(0x9a)
IRQ_STUB(0x9b)
IRQ_STUB(0x9c)
IRQ_STUB(0x9d)
IRQ_STUB(0x9e)
IRQ_STUB(0x9f)
IRQ_STUB(0xa0)
IRQ_STUB(0xa1)
IRQ_STUB(0xa2)
IRQ_STUB(0xa3)
IRQ_STUB(0xa4)
IRQ_STUB(0xa5)
IRQ_STUB(0xa6)
IRQ_STUB(0xa7)
IRQ_STUB(0xa8)
IRQ_STUB(0xa9)
IRQ_STUB(0xaa)
IRQ_STUB(0xab)
IRQ_STUB(0xac)
IRQ_STUB(0xad)
IRQ_STUB(0xae)
IRQ_STUB(0xaf)
IRQ_STUB(0xb0)
IRQ_STUB(0xb1)
IRQ_STUB(0xb2)
IRQ_STUB(0xb3)
IRQ_STUB(0xb4)
IRQ_STUB(0xb5)
IRQ_STUB(0xb6)
IRQ_STUB(0xb7)
IRQ_STUB(0xb8)
IRQ_STUB(0xb9)
IRQ_STUB(0xba)
IRQ_STUB(0xbb)
IRQ_STUB(0xbc)
IRQ_STUB(0xbd)
IRQ_STUB(0xbe)
IRQ_STUB(0xbf)
IRQ_STUB(0xc0)
IRQ_STUB(0xc1)
IRQ_STUB(0xc2)
IRQ_STUB(0xc3)
IRQ_STUB(0xc4)
IRQ_STUB(0xc5)
IRQ_STUB(0xc6)
IRQ_STUB(0xc7)
IRQ_STUB(0xc8)
IRQ_STUB(0xc9)
IRQ_STUB(0xca)
IRQ_STUB(0xcb)
IRQ_STUB(0xcc)
IRQ_STUB(0xcd)
IRQ_STUB(0xce)
IRQ_STUB(0xcf)
IRQ_STUB(0xd0)
IRQ_STUB(0xd1)
IRQ_STUB(0xd2)
IRQ_STUB(0xd3)
IRQ_STUB(0xd4)
IRQ_STUB(0xd5)
IRQ_STUB(0xd6)
IRQ_STUB(0xd7)
IRQ_STUB(0xd8)
IRQ_STUB(0xd9)
IRQ_STUB(0xda)
IRQ_STUB(0xdb)
IRQ_STUB(0xdc)
IRQ_STUB(0xdd)
IRQ_STUB(0xde)
IRQ_STUB(0xdf)
IRQ_STUB(0xe0)
IRQ_STUB(0xe1)
IRQ_STUB(0xe2)
IRQ_STUB(0xe3)
IRQ_STUB(0xe4)
IRQ_STUB(0xe5)
IRQ_STUB(0xe6)
IRQ_STUB(0xe7)
IRQ_STUB(0xe8)
IRQ_STUB(0xe9)
IRQ_STUB(0xea)
IRQ_STUB(0xeb)
IRQ_STUB(0xec)
IRQ_STUB(0xed)
IRQ_STUB(0xee)
IRQ_STUB(0xef)
IRQ_STUB(0xf0)
IRQ_STUB(0xf1)
IRQ_STUB(0xf2)
IRQ_STUB(0xf3)
IRQ_STUB(0xf4)
IRQ_STUB(0xf5)
IRQ_STUB(0xf6)
IRQ_STUB(0xf7)
IRQ_STUB(0xf8)
IRQ_STUB(0xf9)
IRQ_STUB(0xfa)
IRQ_STUB(0xfb)
IRQ_STUB(0xfc)
IRQ_STUB(0xfd)
IRQ_STUB(0xfe)
IRQ_STUB(0xff)

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// IDT setup
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * All interrupt handler entries
 */
static idt_entry_t m_idt_entries[256];

/**
 * The idt
 */
static idt_t m_idt = {
    .limit = sizeof(m_idt_entries) - 1,
    .base = m_idt_entries
};

/**
 * Set a single idt entry
 */
static void set_idt_entry(int vector, void* func, int ist, bool cli) {
    m_idt_entries[vector].handler_low = (uint16_t) ((uintptr_t)func & 0xFFFF);
    m_idt_entries[vector].handler_high = (uint64_t) ((uintptr_t)func >> 16);
    m_idt_entries[vector].gate_type = cli ? IDT_TYPE_INTERRUPT_32 : IDT_TYPE_TRAP_32;
    m_idt_entries[vector].selector = 8;
    m_idt_entries[vector].present = 1;
    m_idt_entries[vector].ring = 0;
    m_idt_entries[vector].ist = ist;
}

__attribute__((interrupt))
static void timer_interrupt_handler(interrupt_frame_t* frame) {
    lapic_eoi();
    scheduler_preempt();
}

void init_idt() {
    //
    // IST usage:
    //  - 1: page fault
    //  - 2: nmi
    //  - 3: double fault
    //
    set_idt_entry(EXCEPT_IA32_DIVIDE_ERROR, exception_handler_0x00, 0, true);
    set_idt_entry(EXCEPT_IA32_DEBUG, exception_handler_0x01, 0, true);
    set_idt_entry(EXCEPT_IA32_NMI, exception_handler_0x02, 2, true);
    set_idt_entry(EXCEPT_IA32_BREAKPOINT, exception_handler_0x03, 5, true);
    set_idt_entry(EXCEPT_IA32_OVERFLOW, exception_handler_0x04, 0, true);
    set_idt_entry(EXCEPT_IA32_BOUND, exception_handler_0x05, 0, true);
    set_idt_entry(EXCEPT_IA32_INVALID_OPCODE, exception_handler_0x06, 0, true);
    set_idt_entry(0x07, exception_handler_0x07, 0, true);
    set_idt_entry(EXCEPT_IA32_DOUBLE_FAULT, exception_handler_0x08, 3, true);
    set_idt_entry(0x09, exception_handler_0x09, 0, true);
    set_idt_entry(EXCEPT_IA32_INVALID_TSS, exception_handler_0x0A, 0, true);
    set_idt_entry(EXCEPT_IA32_SEG_NOT_PRESENT, exception_handler_0x0B, 0, true);
    set_idt_entry(EXCEPT_IA32_STACK_FAULT, exception_handler_0x0C, 0, true);
    set_idt_entry(EXCEPT_IA32_GP_FAULT, exception_handler_0x0D, 0, true);
    set_idt_entry(EXCEPT_IA32_PAGE_FAULT, exception_handler_0x0E, 1, true);
    set_idt_entry(0x0F, exception_handler_0x0F, 0, true);
    set_idt_entry(EXCEPT_IA32_FP_ERROR, exception_handler_0x10, 0, true);
    set_idt_entry(EXCEPT_IA32_ALIGNMENT_CHECK, exception_handler_0x11, 0, true);
    set_idt_entry(EXCEPT_IA32_MACHINE_CHECK, exception_handler_0x12, 0, true);
    set_idt_entry(EXCEPT_IA32_SIMD, exception_handler_0x13, 0, true);
    set_idt_entry(0x14, exception_handler_0x14, 0, true);
    set_idt_entry(0x15, exception_handler_0x15, 0, true);
    set_idt_entry(0x16, exception_handler_0x16, 0, true);
    set_idt_entry(0x17, exception_handler_0x17, 0, true);
    set_idt_entry(0x18, exception_handler_0x18, 0, true);
    set_idt_entry(0x19, exception_handler_0x19, 0, true);
    set_idt_entry(0x1A, exception_handler_0x1A, 0, true);
    set_idt_entry(0x1B, exception_handler_0x1B, 0, true);
    set_idt_entry(0x1C, exception_handler_0x1C, 0, true);
    set_idt_entry(0x1D, exception_handler_0x1D, 0, true);
    set_idt_entry(0x1E, exception_handler_0x1E, 0, true);
    set_idt_entry(0x1F, exception_handler_0x1F, 0, true);
    set_idt_entry(0x20, timer_interrupt_handler, 0, true);
    set_idt_entry(0x21, interrupt_handler_0x21, 0, true);
    set_idt_entry(0x22, interrupt_handler_0x22, 0, true);
    set_idt_entry(0x23, interrupt_handler_0x23, 0, true);
    set_idt_entry(0x24, interrupt_handler_0x24, 0, true);
    set_idt_entry(0x25, interrupt_handler_0x25, 0, true);
    set_idt_entry(0x26, interrupt_handler_0x26, 0, true);
    set_idt_entry(0x27, interrupt_handler_0x27, 0, true);
    set_idt_entry(0x28, interrupt_handler_0x28, 0, true);
    set_idt_entry(0x29, interrupt_handler_0x29, 0, true);
    set_idt_entry(0x2a, interrupt_handler_0x2a, 0, true);
    set_idt_entry(0x2b, interrupt_handler_0x2b, 0, true);
    set_idt_entry(0x2c, interrupt_handler_0x2c, 0, true);
    set_idt_entry(0x2d, interrupt_handler_0x2d, 0, true);
    set_idt_entry(0x2e, interrupt_handler_0x2e, 0, true);
    set_idt_entry(0x2f, interrupt_handler_0x2f, 0, true);
    set_idt_entry(0x30, interrupt_handler_0x30, 0, true);
    set_idt_entry(0x31, interrupt_handler_0x31, 0, true);
    set_idt_entry(0x32, interrupt_handler_0x32, 0, true);
    set_idt_entry(0x33, interrupt_handler_0x33, 0, true);
    set_idt_entry(0x34, interrupt_handler_0x34, 0, true);
    set_idt_entry(0x35, interrupt_handler_0x35, 0, true);
    set_idt_entry(0x36, interrupt_handler_0x36, 0, true);
    set_idt_entry(0x37, interrupt_handler_0x37, 0, true);
    set_idt_entry(0x38, interrupt_handler_0x38, 0, true);
    set_idt_entry(0x39, interrupt_handler_0x39, 0, true);
    set_idt_entry(0x3a, interrupt_handler_0x3a, 0, true);
    set_idt_entry(0x3b, interrupt_handler_0x3b, 0, true);
    set_idt_entry(0x3c, interrupt_handler_0x3c, 0, true);
    set_idt_entry(0x3d, interrupt_handler_0x3d, 0, true);
    set_idt_entry(0x3e, interrupt_handler_0x3e, 0, true);
    set_idt_entry(0x3f, interrupt_handler_0x3f, 0, true);
    set_idt_entry(0x40, interrupt_handler_0x40, 0, true);
    set_idt_entry(0x41, interrupt_handler_0x41, 0, true);
    set_idt_entry(0x42, interrupt_handler_0x42, 0, true);
    set_idt_entry(0x43, interrupt_handler_0x43, 0, true);
    set_idt_entry(0x44, interrupt_handler_0x44, 0, true);
    set_idt_entry(0x45, interrupt_handler_0x45, 0, true);
    set_idt_entry(0x46, interrupt_handler_0x46, 0, true);
    set_idt_entry(0x47, interrupt_handler_0x47, 0, true);
    set_idt_entry(0x48, interrupt_handler_0x48, 0, true);
    set_idt_entry(0x49, interrupt_handler_0x49, 0, true);
    set_idt_entry(0x4a, interrupt_handler_0x4a, 0, true);
    set_idt_entry(0x4b, interrupt_handler_0x4b, 0, true);
    set_idt_entry(0x4c, interrupt_handler_0x4c, 0, true);
    set_idt_entry(0x4d, interrupt_handler_0x4d, 0, true);
    set_idt_entry(0x4e, interrupt_handler_0x4e, 0, true);
    set_idt_entry(0x4f, interrupt_handler_0x4f, 0, true);
    set_idt_entry(0x50, interrupt_handler_0x50, 0, true);
    set_idt_entry(0x51, interrupt_handler_0x51, 0, true);
    set_idt_entry(0x52, interrupt_handler_0x52, 0, true);
    set_idt_entry(0x53, interrupt_handler_0x53, 0, true);
    set_idt_entry(0x54, interrupt_handler_0x54, 0, true);
    set_idt_entry(0x55, interrupt_handler_0x55, 0, true);
    set_idt_entry(0x56, interrupt_handler_0x56, 0, true);
    set_idt_entry(0x57, interrupt_handler_0x57, 0, true);
    set_idt_entry(0x58, interrupt_handler_0x58, 0, true);
    set_idt_entry(0x59, interrupt_handler_0x59, 0, true);
    set_idt_entry(0x5a, interrupt_handler_0x5a, 0, true);
    set_idt_entry(0x5b, interrupt_handler_0x5b, 0, true);
    set_idt_entry(0x5c, interrupt_handler_0x5c, 0, true);
    set_idt_entry(0x5d, interrupt_handler_0x5d, 0, true);
    set_idt_entry(0x5e, interrupt_handler_0x5e, 0, true);
    set_idt_entry(0x5f, interrupt_handler_0x5f, 0, true);
    set_idt_entry(0x60, interrupt_handler_0x60, 0, true);
    set_idt_entry(0x61, interrupt_handler_0x61, 0, true);
    set_idt_entry(0x62, interrupt_handler_0x62, 0, true);
    set_idt_entry(0x63, interrupt_handler_0x63, 0, true);
    set_idt_entry(0x64, interrupt_handler_0x64, 0, true);
    set_idt_entry(0x65, interrupt_handler_0x65, 0, true);
    set_idt_entry(0x66, interrupt_handler_0x66, 0, true);
    set_idt_entry(0x67, interrupt_handler_0x67, 0, true);
    set_idt_entry(0x68, interrupt_handler_0x68, 0, true);
    set_idt_entry(0x69, interrupt_handler_0x69, 0, true);
    set_idt_entry(0x6a, interrupt_handler_0x6a, 0, true);
    set_idt_entry(0x6b, interrupt_handler_0x6b, 0, true);
    set_idt_entry(0x6c, interrupt_handler_0x6c, 0, true);
    set_idt_entry(0x6d, interrupt_handler_0x6d, 0, true);
    set_idt_entry(0x6e, interrupt_handler_0x6e, 0, true);
    set_idt_entry(0x6f, interrupt_handler_0x6f, 0, true);
    set_idt_entry(0x70, interrupt_handler_0x70, 0, true);
    set_idt_entry(0x71, interrupt_handler_0x71, 0, true);
    set_idt_entry(0x72, interrupt_handler_0x72, 0, true);
    set_idt_entry(0x73, interrupt_handler_0x73, 0, true);
    set_idt_entry(0x74, interrupt_handler_0x74, 0, true);
    set_idt_entry(0x75, interrupt_handler_0x75, 0, true);
    set_idt_entry(0x76, interrupt_handler_0x76, 0, true);
    set_idt_entry(0x77, interrupt_handler_0x77, 0, true);
    set_idt_entry(0x78, interrupt_handler_0x78, 0, true);
    set_idt_entry(0x79, interrupt_handler_0x79, 0, true);
    set_idt_entry(0x7a, interrupt_handler_0x7a, 0, true);
    set_idt_entry(0x7b, interrupt_handler_0x7b, 0, true);
    set_idt_entry(0x7c, interrupt_handler_0x7c, 0, true);
    set_idt_entry(0x7d, interrupt_handler_0x7d, 0, true);
    set_idt_entry(0x7e, interrupt_handler_0x7e, 0, true);
    set_idt_entry(0x7f, interrupt_handler_0x7f, 0, true);
    set_idt_entry(0x80, interrupt_handler_0x80, 0, true);
    set_idt_entry(0x81, interrupt_handler_0x81, 0, true);
    set_idt_entry(0x82, interrupt_handler_0x82, 0, true);
    set_idt_entry(0x83, interrupt_handler_0x83, 0, true);
    set_idt_entry(0x84, interrupt_handler_0x84, 0, true);
    set_idt_entry(0x85, interrupt_handler_0x85, 0, true);
    set_idt_entry(0x86, interrupt_handler_0x86, 0, true);
    set_idt_entry(0x87, interrupt_handler_0x87, 0, true);
    set_idt_entry(0x88, interrupt_handler_0x88, 0, true);
    set_idt_entry(0x89, interrupt_handler_0x89, 0, true);
    set_idt_entry(0x8a, interrupt_handler_0x8a, 0, true);
    set_idt_entry(0x8b, interrupt_handler_0x8b, 0, true);
    set_idt_entry(0x8c, interrupt_handler_0x8c, 0, true);
    set_idt_entry(0x8d, interrupt_handler_0x8d, 0, true);
    set_idt_entry(0x8e, interrupt_handler_0x8e, 0, true);
    set_idt_entry(0x8f, interrupt_handler_0x8f, 0, true);
    set_idt_entry(0x90, interrupt_handler_0x90, 0, true);
    set_idt_entry(0x91, interrupt_handler_0x91, 0, true);
    set_idt_entry(0x92, interrupt_handler_0x92, 0, true);
    set_idt_entry(0x93, interrupt_handler_0x93, 0, true);
    set_idt_entry(0x94, interrupt_handler_0x94, 0, true);
    set_idt_entry(0x95, interrupt_handler_0x95, 0, true);
    set_idt_entry(0x96, interrupt_handler_0x96, 0, true);
    set_idt_entry(0x97, interrupt_handler_0x97, 0, true);
    set_idt_entry(0x98, interrupt_handler_0x98, 0, true);
    set_idt_entry(0x99, interrupt_handler_0x99, 0, true);
    set_idt_entry(0x9a, interrupt_handler_0x9a, 0, true);
    set_idt_entry(0x9b, interrupt_handler_0x9b, 0, true);
    set_idt_entry(0x9c, interrupt_handler_0x9c, 0, true);
    set_idt_entry(0x9d, interrupt_handler_0x9d, 0, true);
    set_idt_entry(0x9e, interrupt_handler_0x9e, 0, true);
    set_idt_entry(0x9f, interrupt_handler_0x9f, 0, true);
    set_idt_entry(0xa0, interrupt_handler_0xa0, 0, true);
    set_idt_entry(0xa1, interrupt_handler_0xa1, 0, true);
    set_idt_entry(0xa2, interrupt_handler_0xa2, 0, true);
    set_idt_entry(0xa3, interrupt_handler_0xa3, 0, true);
    set_idt_entry(0xa4, interrupt_handler_0xa4, 0, true);
    set_idt_entry(0xa5, interrupt_handler_0xa5, 0, true);
    set_idt_entry(0xa6, interrupt_handler_0xa6, 0, true);
    set_idt_entry(0xa7, interrupt_handler_0xa7, 0, true);
    set_idt_entry(0xa8, interrupt_handler_0xa8, 0, true);
    set_idt_entry(0xa9, interrupt_handler_0xa9, 0, true);
    set_idt_entry(0xaa, interrupt_handler_0xaa, 0, true);
    set_idt_entry(0xab, interrupt_handler_0xab, 0, true);
    set_idt_entry(0xac, interrupt_handler_0xac, 0, true);
    set_idt_entry(0xad, interrupt_handler_0xad, 0, true);
    set_idt_entry(0xae, interrupt_handler_0xae, 0, true);
    set_idt_entry(0xaf, interrupt_handler_0xaf, 0, true);
    set_idt_entry(0xb0, interrupt_handler_0xb0, 0, true);
    set_idt_entry(0xb1, interrupt_handler_0xb1, 0, true);
    set_idt_entry(0xb2, interrupt_handler_0xb2, 0, true);
    set_idt_entry(0xb3, interrupt_handler_0xb3, 0, true);
    set_idt_entry(0xb4, interrupt_handler_0xb4, 0, true);
    set_idt_entry(0xb5, interrupt_handler_0xb5, 0, true);
    set_idt_entry(0xb6, interrupt_handler_0xb6, 0, true);
    set_idt_entry(0xb7, interrupt_handler_0xb7, 0, true);
    set_idt_entry(0xb8, interrupt_handler_0xb8, 0, true);
    set_idt_entry(0xb9, interrupt_handler_0xb9, 0, true);
    set_idt_entry(0xba, interrupt_handler_0xba, 0, true);
    set_idt_entry(0xbb, interrupt_handler_0xbb, 0, true);
    set_idt_entry(0xbc, interrupt_handler_0xbc, 0, true);
    set_idt_entry(0xbd, interrupt_handler_0xbd, 0, true);
    set_idt_entry(0xbe, interrupt_handler_0xbe, 0, true);
    set_idt_entry(0xbf, interrupt_handler_0xbf, 0, true);
    set_idt_entry(0xc0, interrupt_handler_0xc0, 0, true);
    set_idt_entry(0xc1, interrupt_handler_0xc1, 0, true);
    set_idt_entry(0xc2, interrupt_handler_0xc2, 0, true);
    set_idt_entry(0xc3, interrupt_handler_0xc3, 0, true);
    set_idt_entry(0xc4, interrupt_handler_0xc4, 0, true);
    set_idt_entry(0xc5, interrupt_handler_0xc5, 0, true);
    set_idt_entry(0xc6, interrupt_handler_0xc6, 0, true);
    set_idt_entry(0xc7, interrupt_handler_0xc7, 0, true);
    set_idt_entry(0xc8, interrupt_handler_0xc8, 0, true);
    set_idt_entry(0xc9, interrupt_handler_0xc9, 0, true);
    set_idt_entry(0xca, interrupt_handler_0xca, 0, true);
    set_idt_entry(0xcb, interrupt_handler_0xcb, 0, true);
    set_idt_entry(0xcc, interrupt_handler_0xcc, 0, true);
    set_idt_entry(0xcd, interrupt_handler_0xcd, 0, true);
    set_idt_entry(0xce, interrupt_handler_0xce, 0, true);
    set_idt_entry(0xcf, interrupt_handler_0xcf, 0, true);
    set_idt_entry(0xd0, interrupt_handler_0xd0, 0, true);
    set_idt_entry(0xd1, interrupt_handler_0xd1, 0, true);
    set_idt_entry(0xd2, interrupt_handler_0xd2, 0, true);
    set_idt_entry(0xd3, interrupt_handler_0xd3, 0, true);
    set_idt_entry(0xd4, interrupt_handler_0xd4, 0, true);
    set_idt_entry(0xd5, interrupt_handler_0xd5, 0, true);
    set_idt_entry(0xd6, interrupt_handler_0xd6, 0, true);
    set_idt_entry(0xd7, interrupt_handler_0xd7, 0, true);
    set_idt_entry(0xd8, interrupt_handler_0xd8, 0, true);
    set_idt_entry(0xd9, interrupt_handler_0xd9, 0, true);
    set_idt_entry(0xda, interrupt_handler_0xda, 0, true);
    set_idt_entry(0xdb, interrupt_handler_0xdb, 0, true);
    set_idt_entry(0xdc, interrupt_handler_0xdc, 0, true);
    set_idt_entry(0xdd, interrupt_handler_0xdd, 0, true);
    set_idt_entry(0xde, interrupt_handler_0xde, 0, true);
    set_idt_entry(0xdf, interrupt_handler_0xdf, 0, true);
    set_idt_entry(0xe0, interrupt_handler_0xe0, 0, true);
    set_idt_entry(0xe1, interrupt_handler_0xe1, 0, true);
    set_idt_entry(0xe2, interrupt_handler_0xe2, 0, true);
    set_idt_entry(0xe3, interrupt_handler_0xe3, 0, true);
    set_idt_entry(0xe4, interrupt_handler_0xe4, 0, true);
    set_idt_entry(0xe5, interrupt_handler_0xe5, 0, true);
    set_idt_entry(0xe6, interrupt_handler_0xe6, 0, true);
    set_idt_entry(0xe7, interrupt_handler_0xe7, 0, true);
    set_idt_entry(0xe8, interrupt_handler_0xe8, 0, true);
    set_idt_entry(0xe9, interrupt_handler_0xe9, 0, true);
    set_idt_entry(0xea, interrupt_handler_0xea, 0, true);
    set_idt_entry(0xeb, interrupt_handler_0xeb, 0, true);
    set_idt_entry(0xec, interrupt_handler_0xec, 0, true);
    set_idt_entry(0xed, interrupt_handler_0xed, 0, true);
    set_idt_entry(0xee, interrupt_handler_0xee, 0, true);
    set_idt_entry(0xef, interrupt_handler_0xef, 0, true);
    set_idt_entry(0xf0, interrupt_handler_0xf0, 0, true);
    set_idt_entry(0xf1, interrupt_handler_0xf1, 0, true);
    set_idt_entry(0xf2, interrupt_handler_0xf2, 0, true);
    set_idt_entry(0xf3, interrupt_handler_0xf3, 0, true);
    set_idt_entry(0xf4, interrupt_handler_0xf4, 0, true);
    set_idt_entry(0xf5, interrupt_handler_0xf5, 0, true);
    set_idt_entry(0xf6, interrupt_handler_0xf6, 0, true);
    set_idt_entry(0xf7, interrupt_handler_0xf7, 0, true);
    set_idt_entry(0xf8, interrupt_handler_0xf8, 0, true);
    set_idt_entry(0xf9, interrupt_handler_0xf9, 0, true);
    set_idt_entry(0xfa, interrupt_handler_0xfa, 0, true);
    set_idt_entry(0xfb, interrupt_handler_0xfb, 0, true);
    set_idt_entry(0xfc, interrupt_handler_0xfc, 0, true);
    set_idt_entry(0xfd, interrupt_handler_0xfd, 0, true);
    set_idt_entry(0xfe, interrupt_handler_0xfe, 0, true);
    set_idt_entry(0xff, interrupt_handler_0xff, 0, true);

    asm volatile ("lidt %0" : : "m" (m_idt));
}

