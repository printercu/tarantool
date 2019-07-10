env = require('test_run')
test_run = env.new()
engine = test_run:get_cfg('engine')

decimal = require('decimal')
ffi = require('ffi')

_ = box.schema.space.create('test', {engine=engine})
_ = box.space.test:create_index('pk', {parts={1,'decimal'}})

test_run:cmd('setopt delimiter ";"')
for i = 0,16 do
    box.space.test:insert{decimal.new((i-8)/4)}
end;
test_run:cmd('setopt delimiter ""');

box.space.test:select{}

-- check invalid values
box.space.test:insert{1.23}
box.space.test:insert{'str'}
box.space.test:insert{ffi.new('uint64_t', 0)}
-- check duplicates
box.space.test:insert{decimal.new(0)}

box.space.test.index.pk:drop()

_ = box.space.test:create_index('pk', {parts={1, 'number'}})

test_run:cmd('setopt delimiter ";"')
for i = 0, 32 do
    local val = (i - 16) / 8
    if i % 2 == 1 then val = decimal.new(val) end
    box.space.test:insert{val}
end;
test_run:cmd('setopt delimiter ""');

box.space.test:select{}

-- check duplicates
box.space.test:insert{-2}
box.space.test:insert{decimal.new(-2)}
box.space.test:insert{decimal.new(-1.875)}
box.space.test:insert{-1.875}

box.space.test.index.pk:drop()

_ = box.space.test:create_index('pk')
test_run:cmd('setopt delimiter ";"')
for i = 1,10 do
    box.space.test:insert{i, decimal.new(i/10)}
end;
test_run:cmd('setopt delimiter ""');

-- a bigger test with a secondary index this time.
box.space.test:insert{11, 'str'}
box.space.test:insert{12, 0.63}
box.space.test:insert{13, 0.57}
box.space.test:insert{14, 0.33}
box.space.test:insert{16, 0.71}

_ = box.space.test:create_index('sk', {parts={2, 'scalar'}})
box.space.test.index.sk:select{}

box.space.test:drop()
