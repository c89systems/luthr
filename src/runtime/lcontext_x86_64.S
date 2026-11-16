/* Private System V x86_64 runtime context. Not a public runtime ABI. */
.text
.globl luaCT_switch
.hidden luaCT_switch
.type luaCT_switch,@function
luaCT_switch:
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
.size luaCT_switch,.-luaCT_switch

.globl luaCT_boot
.hidden luaCT_boot
.hidden luaCT_entry
.type luaCT_boot,@function
luaCT_boot:
  subq $8, %rsp
  movq %r12, %rdi
  call luaCT_entry
  ud2
.size luaCT_boot,.-luaCT_boot
.section .note.GNU-stack,"",@progbits

