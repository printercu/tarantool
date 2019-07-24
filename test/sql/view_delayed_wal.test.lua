test_run = require('test_run').new()
engine = test_run:get_cfg('engine')
box.execute('pragma sql_default_engine=\''..engine..'\'')
fiber = require('fiber')

-- View reference counters are incremented before firing
-- on_commit triggers (i.e. before being written into WAL), so
-- it is impossible to drop a space referenced by a created, but
-- *still* not yet committed view.
--
box.execute('CREATE TABLE t1(id INT PRIMARY KEY)')
function create_view() box.execute('CREATE VIEW v1 AS SELECT * FROM t1') end
function drop_index_t1() box.space._index:delete{box.space.T1.id, 0} end
function drop_space_t1() box.space._space:delete{box.space.T1.id} end
box.error.injection.set("ERRINJ_WAL_DELAY", true)
f1 = fiber.create(create_view)
f2 = fiber.create(drop_index_t1)
f3 = fiber.create(drop_space_t1)
box.error.injection.set("ERRINJ_WAL_DELAY", false)
fiber.sleep(0.1)
box.space.T1 ~= nil
box.space.V1 ~= nil

--
-- In the same way, we have to drop all referenced spaces before
-- dropping view, since view reference counter of space to be
-- dropped is checked before firing on_commit trigger.
--
box.execute('CREATE TABLE t2 (id INT PRIMARY KEY)')
box.execute('CREATE VIEW view2 AS SELECT * FROM t2')

function drop_view() box.space._space:delete{box.space.VIEW2.id} end
function drop_index_t2() box.space._index:delete{box.space.T2.id, 0} end
function drop_space_t2() box.space._space:delete{box.space.T2.id} end
box.error.injection.set("ERRINJ_WAL_DELAY", true)
f1 = fiber.create(drop_index_t2)
f2 = fiber.create(drop_space_t2)
f3 = fiber.create(drop_view)
box.error.injection.set("ERRINJ_WAL_DELAY", false)
fiber.sleep(0.1)
box.space._space:get{box.space.T2.id}['name']
box.space.V2

-- Since deletion via Lua doesn't remove entry from
-- SQL data dictionary we have to restart instance to clean up.
--
test_run:cmd('restart server default with cleanup=1')
