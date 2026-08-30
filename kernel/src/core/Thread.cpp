#include "core/Thread.hpp"

extern "C" __attribute__((naked)) void oz::switch_context(void** old_rsp, void* new_rsp) {
    __asm__ volatile(
        // Save current thread's callee-saved registers
        "pushq %rbx\n"
        "pushq %rbp\n"
        "pushq %r12\n"
        "pushq %r13\n"
        "pushq %r14\n"
        "pushq %r15\n"
        
        // Save current rsp to *old_rsp
        "movq %rsp, (%rdi)\n"
        
        // Load new rsp from new_rsp
        "movq %rsi, %rsp\n"
        
        // Restore next thread's callee-saved registers
        "popq %r15\n"
        "popq %r14\n"
        "popq %r13\n"
        "popq %r12\n"
        "popq %rbp\n"
        "popq %rbx\n"
        
        // Return to next thread's execution point
        "ret\n"
    );
}

extern "C" __attribute__((naked)) void oz::thread_stub() {
    __asm__ volatile(
        "sti\n"             // Enable interrupts for the new thread
        "movq %rbx, %rdi\n" // Move arg to first argument register
        "call *%r12\n"      // Call entry function
        // When the thread entry function returns, terminate the thread
        "movq %r14, %rdi\n" // Pass scheduler pointer as argument
        "call *%r13\n"
        "1: hlt\n"
        "jmp 1b\n"
    );
}
