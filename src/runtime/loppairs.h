/* Adjacent register instructions with predecoded successors. See lua.h. */
VM_PAIR(OP_MOVE, OP_MOVE)
VM_PAIR(OP_MOVE, OP_CALL)
VM_PAIR(OP_GETUPVAL, OP_MOVE)
VM_PAIR(OP_GETUPVAL, OP_GETTABLE)
VM_PAIR(OP_GETUPVAL, OP_SETTABLE)
