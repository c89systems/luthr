/* Shared instruction definitions for register-only and capture-aware dispatch.
** Included with VM_CAPTURE and handler labels selected by lvm.c. See lua.h. */
      vmcase(OP_MOVE) {
        execute_OP_MOVE(i);
        vmbreak;
      }
      vmcase(OP_LOADI) {
        StkId ra = RA(i);
        lua_Integer b = GETARG_sBx(i);
        setivalue(s2v(ra), b);
        vmbreak;
      }
      vmcase(OP_LOADF) {
        StkId ra = RA(i);
        int b = GETARG_sBx(i);
        setfltvalue(s2v(ra), cast_num(b));
        vmbreak;
      }
      vmcase(OP_LOADK) {
        StkId ra = RA(i);
        TValue *rb = k + GETARG_Bx(i);
        setobj2s(L, ra, rb);
        vmbreak;
      }
      vmcase(OP_LOADKX) {
        StkId ra = RA(i);
        TValue *rb;
        rb = k + GETARG_Ax(*pc); pc++;
        setobj2s(L, ra, rb);
        vmbreak;
      }
      vmcase(OP_LOADFALSE) {
        StkId ra = RA(i);
        setbfvalue(s2v(ra));
        vmbreak;
      }
      vmcase(OP_LFALSESKIP) {
        StkId ra = RA(i);
        setbfvalue(s2v(ra));
        pc++;  /* skip next instruction */
        vmbreak;
      }
      vmcase(OP_LOADTRUE) {
        StkId ra = RA(i);
        setbtvalue(s2v(ra));
        vmbreak;
      }
      vmcase(OP_LOADNIL) {
        StkId ra = RA(i);
        int b = GETARG_B(i);
        do {
          setnilvalue(s2v(ra++));
        } while (b--);
        vmbreak;
      }
      vmcase(OP_GETUPVAL) {
        execute_OP_GETUPVAL(i);
        vmbreak;
      }
      vmcase(OP_SETUPVAL) {
        StkId ra = readRA(i);
        UpVal *uv = luaF_capturecell(cl, GETARG_B(i));
        if (luaF_localupval(L, uv))
          luaF_writeprivate(L, uv, s2v(ra));
        else if (!luaF_trystoreinteger(uv, s2v(ra))) {
          savestate(L, ci);
          luaF_writecapture(L, uv, s2v(ra));
          updatebase(ci);
        }
        vmbreak;
      }
      vmcase(OP_GETTABUP) {
        StkId ra = readRA(i);
        TValue receiver;
        TValue *key = KC(i);  /* a short string */
        unsigned upvalue = GETARG_B(i);
        luaF_loadupval(L, luaF_capturecell(cl, upvalue), &receiver);
        vmtableget(&receiver, key, luaH_trygetshortstr, tsvalue(key), upvalue + 1u);
        vmbreak;
      }
      vmcase(OP_GETTABLE) {
        StkId ra = readRA(i);
        TValue *rb = vRB(i);
        TValue *rc = vRC(i);
        vmtableget(rb, rc, luaH_tryget, rc, 0);
        vmbreak;
      }
      vmcase(OP_GETI) {
        StkId ra = readRA(i);
        TValue *rb = vRB(i);
        TValue key;
        setivalue(&key, GETARG_C(i));
        vmtableget(rb, &key, luaH_trygetint, ivalue(&key), 0);
        vmbreak;
      }
      vmcase(OP_GETFIELD) {
        StkId ra = readRA(i);
        TValue *rb = vRB(i);
        TValue *rc = KC(i);
        vmtableget(rb, rc, luaH_trygetshortstr, tsvalue(rc), 0);
        vmbreak;
      }
      vmcase(OP_SETTABUP) {
        TValue receiver;
        TValue *rb = KB(i);
        TValue *rc = RKC(i);
        unsigned upvalue = GETARG_A(i);
        luaF_loadupval(L, luaF_capturecell(cl, upvalue), &receiver);
        vmtableset(&receiver, rb, rc, upvalue + 1u, luaH_tryset);
        vmbreak;
      }
      vmcase(OP_SETTABLE) {
        StkId ra = readRA(i);
        TValue *rb = vRB(i);  /* key (table is in 'ra') */
        TValue *rc = RKC(i);  /* value */
        vmtableset(s2v(ra), rb, rc, 0, luaH_tryset);
        vmbreak;
      }
      vmcase(OP_SETI) {
        StkId ra = readRA(i);
        TValue key;
        int b = GETARG_B(i);
        TValue *rc = RKC(i);
        setivalue(&key, b);
        vmtableset(s2v(ra), &key, rc, 0, luaH_tryset);
        vmbreak;
      }
      vmcase(OP_SETFIELD) {
        StkId ra = readRA(i);
        TValue *rb = KB(i);
        TValue *rc = RKC(i);
        vmtableset(s2v(ra), rb, rc, 0, luaH_trysetfield);
        vmbreak;
      }
      vmcase(OP_NEWTABLE) {
        Instruction extra = *pc++;
        readRA(i);  /* materialize a captured destination before allocation */
        savepc(ci);
        createtable(L, ci, ip, i, extra);
        updatebase(ci);
        updatetrap(ci);
        vmbreak;
      }
      vmcase(OP_SELF) {
        StkId ra = RA(i);
        TValue *rb = vRB(i);
        TValue *rc = KC(i);
        readrange(ra, 2);
        setobj2s(L, ra + 1, rb);
        vmtableget(rb, rc, luaH_trygetshortstr, tsvalue(rc), 0);
        vmbreak;
      }
      vmcase(OP_ADDI) {
        op_arithI(L, l_addi, luai_numadd);
        vmbreak;
      }
      vmcase(OP_ADDK) {
        op_arithK(L, l_addi, luai_numadd);
        vmbreak;
      }
      vmcase(OP_SUBK) {
        op_arithK(L, l_subi, luai_numsub);
        vmbreak;
      }
      vmcase(OP_MULK) {
        op_arithK(L, l_muli, luai_nummul);
        vmbreak;
      }
      vmcase(OP_MODK) {
        savestate(L, ci);  /* in case of division by 0 */
        op_arithK(L, luaV_mod, luaV_modf);
        vmbreak;
      }
      vmcase(OP_POWK) {
        op_arithfK(L, luai_numpow);
        vmbreak;
      }
      vmcase(OP_DIVK) {
        op_arithfK(L, luai_numdiv);
        vmbreak;
      }
      vmcase(OP_IDIVK) {
        savestate(L, ci);  /* in case of division by 0 */
        op_arithK(L, luaV_idiv, luai_numidiv);
        vmbreak;
      }
      vmcase(OP_BANDK) {
        op_bitwiseK(L, l_band);
        vmbreak;
      }
      vmcase(OP_BORK) {
        op_bitwiseK(L, l_bor);
        vmbreak;
      }
      vmcase(OP_BXORK) {
        op_bitwiseK(L, l_bxor);
        vmbreak;
      }
      vmcase(OP_SHLI) {
        StkId ra = RA(i);
        TValue *rb = vRB(i);
        int ic = GETARG_sC(i);
        lua_Integer ib;
        if (tointegerns(rb, &ib)) {
          pc++; setivalue(s2v(ra), luaV_shiftl(ic, ib));
        }
        vmbreak;
      }
      vmcase(OP_SHRI) {
        StkId ra = RA(i);
        TValue *rb = vRB(i);
        int ic = GETARG_sC(i);
        lua_Integer ib;
        if (tointegerns(rb, &ib)) {
          pc++; setivalue(s2v(ra), luaV_shiftl(ib, -ic));
        }
        vmbreak;
      }
      vmcase(OP_ADD) {
        op_arith(L, l_addi, luai_numadd);
        vmbreak;
      }
      vmcase(OP_SUB) {
        op_arith(L, l_subi, luai_numsub);
        vmbreak;
      }
      vmcase(OP_MUL) {
        op_arith(L, l_muli, luai_nummul);
        vmbreak;
      }
      vmcase(OP_MOD) {
        savestate(L, ci);  /* in case of division by 0 */
        op_arith(L, luaV_mod, luaV_modf);
        vmbreak;
      }
      vmcase(OP_POW) {
        op_arithf(L, luai_numpow);
        vmbreak;
      }
      vmcase(OP_DIV) {  /* float division (always with floats) */
        op_arithf(L, luai_numdiv);
        vmbreak;
      }
      vmcase(OP_IDIV) {  /* floor division */
        savestate(L, ci);  /* in case of division by 0 */
        op_arith(L, luaV_idiv, luai_numidiv);
        vmbreak;
      }
      vmcase(OP_BAND) {
        op_bitwise(L, l_band);
        vmbreak;
      }
      vmcase(OP_BOR) {
        op_bitwise(L, l_bor);
        vmbreak;
      }
      vmcase(OP_BXOR) {
        op_bitwise(L, l_bxor);
        vmbreak;
      }
      vmcase(OP_SHL) {
        op_bitwise(L, luaV_shiftl);
        vmbreak;
      }
      vmcase(OP_SHR) {
        op_bitwise(L, luaV_shiftr);
        vmbreak;
      }
      vmcase(OP_MMBIN) {
        StkId ra = readRA(i);
        Instruction pi = *(pc - 2);  /* original arithmetic */
        TValue *rb = vRB(i);
        TMS tm = (TMS)GETARG_C(i);
        StkId result = readreg(GETARG_A(pi));
        lua_assert(OP_ADD <= GET_OPCODE(pi) && GET_OPCODE(pi) <= OP_SHR);
        Protect(luaT_trybinTM(L, s2v(ra), rb, result, tm));
        vmbreak;
      }
      vmcase(OP_MMBINI) {
        StkId ra = readRA(i);
        Instruction pi = *(pc - 2);  /* original arithmetic */
        int imm = GETARG_sB(i);
        TMS tm = (TMS)GETARG_C(i);
        int flip = GETARG_k(i);
        StkId result = readreg(GETARG_A(pi));
        Protect(luaT_trybiniTM(L, s2v(ra), imm, flip, result, tm));
        vmbreak;
      }
      vmcase(OP_MMBINK) {
        StkId ra = readRA(i);
        Instruction pi = *(pc - 2);  /* original arithmetic */
        TValue *imm = KB(i);
        TMS tm = (TMS)GETARG_C(i);
        int flip = GETARG_k(i);
        StkId result = readreg(GETARG_A(pi));
        Protect(luaT_trybinassocTM(L, s2v(ra), imm, flip, result, tm));
        vmbreak;
      }
      vmcase(OP_UNM) {
        StkId ra = readRA(i);
        TValue *rb = vRB(i);
        lua_Number nb;
        if (ttisinteger(rb)) {
          lua_Integer ib = ivalue(rb);
          setivalue(s2v(ra), intop(-, 0, ib));
        }
        else if (tonumberns(rb, nb)) {
          setfltvalue(s2v(ra), luai_numunm(L, nb));
        }
        else
          Protect(luaT_trybinTM(L, rb, rb, ra, TM_UNM));
        vmbreak;
      }
      vmcase(OP_BNOT) {
        StkId ra = readRA(i);
        TValue *rb = vRB(i);
        lua_Integer ib;
        if (tointegerns(rb, &ib)) {
          setivalue(s2v(ra), intop(^, ~l_castS2U(0), ib));
        }
        else
          Protect(luaT_trybinTM(L, rb, rb, ra, TM_BNOT));
        vmbreak;
      }
      vmcase(OP_NOT) {
        StkId ra = RA(i);
        TValue *rb = vRB(i);
        if (l_isfalse(rb))
          setbtvalue(s2v(ra));
        else
          setbfvalue(s2v(ra));
        vmbreak;
      }
      vmcase(OP_LEN) {
        StkId ra = readRA(i);
        Protect(luaV_objlen(L, ra, vRB(i)));
        vmbreak;
      }
      vmcase(OP_CONCAT) {
        StkId ra = RA(i);
        int n = GETARG_B(i);  /* number of elements to concatenate */
        readrange(ra, n);
        L->top.p = ra + n;  /* mark the end of concat operands */
        ProtectNT(luaV_concat(L, n));
        checkGC(L, L->top.p); /* 'luaV_concat' ensures correct top */
        vmbreak;
      }
      vmcase(OP_CLOSE) {
        StkId ra = RA(i);
        lua_assert(!GETARG_B(i));  /* 'close must be alive */
        Protect(luaF_close(L, ra, LUA_OK, 1));
        vmbreak;
      }
      vmcase(OP_TBC) {
        StkId ra = readRA(i);
        /* create new to-be-closed upvalue */
        halfProtect(luaF_newtbcupval(L, ra));
        vmbreak;
      }
      vmcase(OP_JMP) {
        dojump(ci, i, 0);
        vmbreak;
      }
      vmcase(OP_EQ) {
        StkId ra = readRA(i);
        int cond;
        TValue *rb = vRB(i);
        Protect(cond = luaV_equalobj(L, s2v(ra), rb));
        docondjump();
        vmbreak;
      }
      vmcase(OP_LT) {
        op_order(L, l_lti, LTnum, lessthanothers);
        vmbreak;
      }
      vmcase(OP_LE) {
        op_order(L, l_lei, LEnum, lessequalothers);
        vmbreak;
      }
      vmcase(OP_EQK) {
        StkId ra = readRA(i);
        TValue *rb = KB(i);
        /* basic types do not use '__eq'; we can use raw equality */
        int cond = luaV_rawequalobj(s2v(ra), rb);
        docondjump();
        vmbreak;
      }
      vmcase(OP_EQI) {
        StkId ra = readRA(i);
        int cond;
        int im = GETARG_sB(i);
        if (ttisinteger(s2v(ra)))
          cond = (ivalue(s2v(ra)) == im);
        else if (ttisfloat(s2v(ra)))
          cond = luai_numeq(fltvalue(s2v(ra)), cast_num(im));
        else
          cond = 0;  /* other types cannot be equal to a number */
        docondjump();
        vmbreak;
      }
      vmcase(OP_LTI) {
        op_orderI(L, l_lti, luai_numlt, 0, TM_LT);
        vmbreak;
      }
      vmcase(OP_LEI) {
        op_orderI(L, l_lei, luai_numle, 0, TM_LE);
        vmbreak;
      }
      vmcase(OP_GTI) {
        op_orderI(L, l_gti, luai_numgt, 1, TM_LT);
        vmbreak;
      }
      vmcase(OP_GEI) {
        op_orderI(L, l_gei, luai_numge, 1, TM_LE);
        vmbreak;
      }
      vmcase(OP_TEST) {
        StkId ra = readRA(i);
        int cond = !l_isfalse(s2v(ra));
        docondjump();
        vmbreak;
      }
      vmcase(OP_TESTSET) {
        StkId ra = RA(i);
        TValue *rb = vRB(i);
        if (l_isfalse(rb) == GETARG_k(i))
          pc++;
        else {
          setobj2s(L, ra, rb);
          if (haslocalcaptures(ci)) {
            savepc(ci);
            luaF_storelocals(L, ra, 1);
            updatebase(ci);
          }
          donextjump(ci);
        }
        vmbreak;
      }
      vmcase(OP_CALL) {
        StkId ra = RA(i);
        CallInfo *newci;
        int b = GETARG_B(i);
        int nresults = GETARG_C(i) - 1;
        if (b == 0)  /* previous instruction set top */
          b = cast_int(L->top.p - ra);
        readrange(ra, b);
        if (checktag(s2v(ra), LUA_VBCF)) {
          if (luaD_tryleaf(L, ra, b - 1, nresults) < 0) {
            L->top.p = ra + b;
            savepc(ci);
            if (luaD_callleaffallback(L, ra, nresults))
              vmsuspend;
          }
          else if (l_unlikely(luaTK_pendingstop(L) != NULL ||
                              (L->policy != NULL && luaCP_exhausted(L)))) {
            savepc(ci);
            luaTK_checkstop(L);
            luaCP_checklimit(L);
          }
          updatetrap(ci);
          vmbreak;
        }
        L->top.p = ra + b;
        savepc(ci);  /* in case of errors */
#if !VM_CAPTURE && !defined(HARDSTACKTESTS)
        /* Enter a Lua closure directly when its arguments and frame fit. */
        if (ttisLclosure(s2v(ra))) {
          LClosure *closure = clLvalue(s2v(ra));
          const Proto *callee = closure->p;
          int fsize = callee->maxstacksize;
          if (ci->next != NULL && b > callee->numparams &&
              L->stack_last.p - L->top.p > fsize) {
            ci = luaD_initframe(L, ci->next, ra, GETARG_C(i), ra + 1 + fsize);
            luaD_initluaframe(ci, callee->code);
            cl = closure;
            p = callee;
            goto startclosure;
          }
        }
#endif
        if (checktag(s2v(ra), LUA_VLCF)) {
          luaD_precallC(L, ra, GETARG_C(i), fvalue(s2v(ra)), 0);
          updatetrap(ci);
          vmbreak;
        }
        if ((newci = luaD_precall(L, ra, nresults)) == NULL)
          updatetrap(ci);  /* C call; nothing else to be done */
        else {  /* Lua call: run function in this same C frame */
          ci = newci;
          goto startfunc;
        }
        vmbreak;
      }
      vmcase(OP_TAILCALL) {
        StkId ra = RA(i);
        int b = GETARG_B(i);  /* number of arguments + 1 (function) */
        int n;  /* number of results when calling a C function */
        int nparams1 = GETARG_C(i);
        /* delta is virtual 'func' - real 'func' (vararg functions) */
        int delta = (nparams1) ? ci->u.l.nextraargs + nparams1 : 0;
        if (b != 0)
          L->top.p = ra + b;
        else  /* previous instruction set top */
          b = cast_int(L->top.p - ra);
        readrange(ra, b);
        savepc(ci);  /* several calls here can raise errors */
        if (TESTARG_k(i)) {
          luaF_closeupval(L, base);  /* close upvalues from current call */
          lua_assert(L->tbclist.p < base);  /* no pending tbc variables */
          lua_assert(base == ci->func.p + 1);
        }
        if ((n = luaD_pretailcall(L, ci, ra, b, delta)) < 0)  /* Lua function? */
          goto startfunc;  /* execute the callee */
        else {  /* C function? */
          ci->func.p -= delta;  /* restore 'func' (if vararg) */
          luaD_poscall(L, ci, n);  /* finish caller */
          updatetrap(ci);  /* 'luaD_poscall' can change hooks */
          goto ret;  /* caller returns after the tail call */
        }
      }
      vmcase(OP_RETURN) {
        StkId ra = RA(i);
        int n = GETARG_B(i) - 1;  /* number of results */
        int nparams1 = GETARG_C(i);
        if (n < 0)  /* not fixed? */
          n = cast_int(L->top.p - ra);  /* get what is available */
        readrange(ra, n);
        savepc(ci);
        if (TESTARG_k(i)) {  /* may there be open upvalues? */
          ci->u2.nres = n;  /* save number of returns */
          if (L->top.p < ci->top.p)
            L->top.p = ci->top.p;
          luaF_close(L, base, CLOSEKTOP, 1);
          updatetrap(ci);
          updatestack(ci);
        }
        if (nparams1)  /* vararg function? */
          ci->func.p -= ci->u.l.nextraargs + nparams1;
        L->top.p = ra + n;  /* set call for 'luaD_poscall' */
        luaD_poscall(L, ci, n);
        updatetrap(ci);  /* 'luaD_poscall' can change hooks */
        goto ret;
      }
      vmcase(OP_RETURN0) {
        if (l_unlikely(gethookmask(L))) {
          StkId ra = RA(i);
          L->top.p = ra;
          savepc(ci);
          luaD_poscall(L, ci, 0);  /* no hurry... */
          trap = 1;
        }
        else {  /* do the 'poscall' here */
          int nres = get_nresults(ci->callstatus);
          L->ci = ci->previous;  /* back to caller */
          L->top.p = base - 1;
          for (; l_unlikely(nres > 0); nres--)
            setnilvalue(s2v(L->top.p++));  /* all results are nil */
          if (isLua(L->ci) && L->ci->u.l.captures != 0)
            luaF_storelocals(L, base - 1, cast_int(L->top.p - (base - 1)));
        }
        goto ret;
      }
      vmcase(OP_RETURN1) {
        readRA(i);
        if (l_unlikely(gethookmask(L))) {
          StkId ra = RA(i);
          L->top.p = ra + 1;
          savepc(ci);
          luaD_poscall(L, ci, 1);  /* no hurry... */
          trap = 1;
        }
        else {  /* do the 'poscall' here */
          int nres = get_nresults(ci->callstatus);
          L->ci = ci->previous;  /* back to caller */
          if (nres == 0)
            L->top.p = base - 1;  /* asked for no results */
          else {
            StkId ra = RA(i);
            setobjs2s(L, base - 1, ra);  /* at least this result */
            L->top.p = base;
            for (; l_unlikely(nres > 1); nres--)
              setnilvalue(s2v(L->top.p++));  /* complete missing results */
            if (isLua(L->ci) && L->ci->u.l.captures != 0)
              luaF_storelocals(L, base - 1, cast_int(L->top.p - (base - 1)));
          }
        }
       ret:  /* return from a Lua function */
        if (ci->callstatus & CIST_FRESH)
          vmreturn;  /* end this frame */
        else {
          ci = ci->previous;
          goto returning;  /* continue running caller in this frame */
        }
      }
      vmcase(OP_FORLOOP) {
        StkId ra = RA(i);
        readrange(ra, 3);
        if (ttisinteger(s2v(ra + 1))) {  /* integer loop? */
          lua_Unsigned count = l_castS2U(ivalue(s2v(ra)));
          if (count > 0) {  /* still more iterations? */
            lua_Integer step = ivalue(s2v(ra + 1));
            lua_Integer idx = ivalue(s2v(ra + 2));  /* control variable */
            chgivalue(s2v(ra), l_castU2S(count - 1));  /* update counter */
            idx = intop(+, idx, step);  /* add step to index */
            chgivalue(s2v(ra + 2), idx);  /* update control variable */
            if (haslocalcaptures(ci)) {
              savepc(ci);
              luaF_storelocals(L, ra, 1);
              updatebase(ci);
              luaF_storelocals(L, RA(i) + 2, 1);
              updatebase(ci);
            }
            pc -= GETARG_Bx(i);  /* jump back */
          }
        }
        else if (floatforloop(L, ra)) {  /* float loop */
          pc -= GETARG_Bx(i);  /* jump back */
          if (haslocalcaptures(ci)) {
            savepc(ci);
            luaF_storelocals(L, ra + 2, 1);
            updatebase(ci);
          }
        }
        updatetrap(ci);  /* allows a signal to break the loop */
        vmbreak;
      }
      vmcase(OP_FORPREP) {
        StkId ra = RA(i);
        readrange(ra, 3);
        savestate(L, ci);  /* in case of errors */
        if (forprep(L, ra))
          pc += GETARG_Bx(i) + 1;  /* skip the loop */
        else if (haslocalcaptures(ci)) {
          savepc(ci);
          luaF_storelocals(L, ra, 3);
          updatebase(ci);
        }
        vmbreak;
      }
      vmcase(OP_TFORPREP) {
       /* before: 'ra' has the iterator function, 'ra + 1' has the state,
          'ra + 2' has the initial value for the control variable, and
          'ra + 3' has the closing variable. This opcode then swaps the
          control and the closing variables and marks the closing variable
          as to-be-closed.
       */
       StkId ra = RA(i);
       TValue temp;  /* to swap control and closing variables */
       readrange(ra + 2, 2);
       setobj(L, &temp, s2v(ra + 3));
       setobjs2s(L, ra + 3, ra + 2);
       setobj2s(L, ra + 2, &temp);
       if (haslocalcaptures(ci)) {
         savepc(ci);
         luaF_storelocals(L, ra + 2, 2);
         updatebase(ci);
         ra = RA(i);
       }
        /* create to-be-closed upvalue (if closing var. is not nil) */
        halfProtect(luaF_newtbcupval(L, ra + 2));
        pc += GETARG_Bx(i);  /* go to end of the loop */
        ip = pc; i = *(pc++);  /* fetch next instruction */
        lua_assert(GET_OPCODE(i) == OP_TFORCALL && ra == RA(i));
        goto l_tforcall;
      }
      vmcase(OP_TFORCALL) {
       l_tforcall: {
        /* 'ra' has the iterator function, 'ra + 1' has the state,
           'ra + 2' has the closing variable, and 'ra + 3' has the control
           variable. The call will use the stack starting at 'ra + 3',
           so that it preserves the first three values, and the first
           return will be the new value for the control variable.
        */
        StkId ra = RA(i);
        readrange(ra, 3 + (GETARG_C(i) > 3 ? GETARG_C(i) : 3));
        setobjs2s(L, ra + 5, ra + 3);  /* copy the control variable */
        setobjs2s(L, ra + 4, ra + 1);  /* copy state */
        setobjs2s(L, ra + 3, ra);  /* copy function */
        L->top.p = ra + 3 + 3;
        ProtectNT(luaD_call(L, ra + 3, GETARG_C(i)));  /* do the call */
        updatestack(ci);  /* stack may have changed */
        ip = pc; i = *(pc++);  /* go to next instruction */
        lua_assert(GET_OPCODE(i) == OP_TFORLOOP && ra == RA(i));
        goto l_tforloop;
      }}
      vmcase(OP_TFORLOOP) {
       l_tforloop: {
        StkId ra = RA(i);
        readrange(ra + 3, 1);
        if (!ttisnil(s2v(ra + 3)))  /* continue loop? */
          pc -= GETARG_Bx(i);  /* jump back */
        vmbreak;
      }}
      vmcase(OP_SETLIST) {
        StkId ra = RA(i);
        unsigned n = cast_uint(GETARG_vB(i));
        unsigned last = cast_uint(GETARG_vC(i));
        if (n == 0)
          n = cast_uint(L->top.p - ra) - 1;  /* get up to the top */
        else
          L->top.p = ci->top.p;  /* correct top in case of emergency GC */
        readrange(ra, cast_int(n + 1));
        last += n;
        if (TESTARG_k(i)) {
          last += cast_uint(GETARG_Ax(*pc)) * (MAXARG_vC + 1);
          pc++;
        }
        ProtectNT(luaH_setlist(L, hvalue(s2v(ra)), ra, n, last));
        updatebase(ci);
        vmbreak;
      }
      vmcase(OP_CLOSURE) {
        StkId ra = readRA(i);
        Proto *p = cl->p->p[GETARG_Bx(i)];
        LClosure *cached = cachedclosure(L, p);
        if (cached != NULL) {
          setclLvalue2s(L, ra, cached);
        }
        else {
          halfProtect(pushclosure(L, p, cl, base, ra));
          checkGC(L, ra + 1);
        }
        vmbreak;
      }
      vmcase(OP_VARARG) {
        StkId ra = RA(i);
        int n = GETARG_C(i) - 1;  /* required results (-1 means all) */
        int vatab = GETARG_k(i) ? GETARG_B(i) : -1;
        readrange(ra, n < 0 ? cast_int(ci->top.p - ra) : n);
        if (vatab >= 0) readreg(vatab);
        Protect(luaT_getvarargs(L, ci, ra, n, vatab));
        vmbreak;
      }
      vmcase(OP_GETVARG) {
        StkId ra = RA(i);
        TValue *rc = vRC(i);
        luaT_getvararg(ci, ra, rc);
        vmbreak;
      }
      vmcase(OP_ERRNNIL) {
        TValue *ra = vRA(i);
        if (!ttisnil(ra))
          halfProtect(luaG_errnnil(L, cl, GETARG_Bx(i)));
        vmbreak;
      }
      vmcase(OP_VARARGPREP) {
        ProtectNT(luaT_adjustvarargs(L, ci, cl->p));
        if (l_unlikely(trap)) {  /* previous "Protect" updated trap */
          luaD_hookcall(L, ci);
          L->oldpc = 1;  /* next opcode will be seen as a "new" line */
        }
        updatebase(ci);  /* function has new base after adjustment */
        vmbreak;
      }
      vmcase(OP_EXTRAARG) {
        lua_assert(0);
        vmbreak;
      }
