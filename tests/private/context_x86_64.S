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
  call luaCT_pause
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
.section .note.GNU-stack,"",@progbits
