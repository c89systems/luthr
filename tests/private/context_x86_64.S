/* Private System V x86_64 feasibility context. Not a public runtime ABI. */
.text
.globl test_context_switch
.hidden test_context_switch
.type test_context_switch,@function
test_context_switch:
  movq %rsp, 0(%rdi)
  movq %rbx, 8(%rdi)
  movq %rbp, 16(%rdi)
  movq %r12, 24(%rdi)
  movq %r13, 32(%rdi)
  movq %r14, 40(%rdi)
  movq %r15, 48(%rdi)
  stmxcsr 56(%rdi)
  fnstcw 60(%rdi)
  ldmxcsr 56(%rsi)
  fldcw 60(%rsi)
  movq 8(%rsi), %rbx
  movq 16(%rsi), %rbp
  movq 24(%rsi), %r12
  movq 32(%rsi), %r13
  movq 40(%rsi), %r14
  movq 48(%rsi), %r15
  movq 0(%rsi), %rsp
  ret
.size test_context_switch,.-test_context_switch

.globl test_context_boot
.hidden test_context_boot
.hidden test_fiber_entry
.type test_context_boot,@function
test_context_boot:
  subq $8, %rsp
  movq %r12, %rdi
  call test_fiber_entry
  ud2
.size test_context_boot,.-test_context_boot
.section .note.GNU-stack,"",@progbits

/* Keep independent sentinels across every migratable pause. */
.text
.globl test_register_pause
.hidden test_register_pause
.type test_register_pause,@function
test_register_pause:
  pushq %rbx
  pushq %rbp
  pushq %r12
  pushq %r13
  pushq %r14
  pushq %r15
  subq $8, %rsp
  movq $101, %rbx
  movq $102, %rbp
  movq $103, %r12
  movq $104, %r13
  movq $105, %r14
  movq $106, %r15
  call test_fiber_pause
  movl $1, %eax
  cmpq $101, %rbx
  jne .Lregister_done
  cmpq $102, %rbp
  jne .Lregister_done
  cmpq $103, %r12
  jne .Lregister_done
  cmpq $104, %r13
  jne .Lregister_done
  cmpq $105, %r14
  jne .Lregister_done
  cmpq $106, %r15
  jne .Lregister_done
  xorl %eax, %eax
.Lregister_done:
  addq $8, %rsp
  popq %r15
  popq %r14
  popq %r13
  popq %r12
  popq %rbp
  popq %rbx
  ret
.size test_register_pause,.-test_register_pause
