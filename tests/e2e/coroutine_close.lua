for _,explicit in ipairs{false,true} do
  for _,failure in ipairs{false,true} do
    local token={}
    local co
    local closed,after={},0
    local function resource(id,close)
      return setmetatable({id=id},{__close=close})
    end
    co=coroutine.create(function()
      local older <close> =resource(1,function(self,err)
        assert(err==nil)
        closed[#closed+1]=self.id
        if failure then error(token,0) end
      end)
      local newer <close> =resource(2,function(self,err)
        assert(err==nil)
        closed[#closed+1]=self.id
        if explicit then
          pcall(coroutine.close,co)
        else
          pcall(coroutine.close)
        end
        after=after+1
      end)
      coroutine.yield(20)
    end)
    local ok,value=coroutine.resume(co)
    assert(ok and value==20 and coroutine.status(co)=='suspended')
    ok,value=coroutine.close(co)
    assert(ok==not failure and value==(failure and token or nil))
    assert(table.concat(closed,',')=='2,1' and after==0)
    assert(coroutine.status(co)=='dead')
    ok,value=coroutine.close(co)
    assert(ok and value==nil and #closed==2)
  end
end
print('coroutine close: recursive explicit/implicit close, cleanup order, errors and repeated close passed')
