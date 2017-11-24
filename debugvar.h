#ifndef DEBUGVAR_H
#define DEBUGVAR_H

#include <lua.h>
#include <lauxlib.h>
#include <stdint.h>
#include <string.h>

/*
	TYPE            frame (16bit)      index (32bit)      size
	-----------------------------------------------------------
	VAR_FRAME_LOCAL stack frame        index              1
	VAR_FRAME_FUNC  stack frame        -                  1
	VAR_INDEX       0/1 (*)            index              ?
	VAR_INDEX_OBJ   0/1 (*)            size of table      ?
	VAR_UPVALUE     -                  index              ?
	VAR_GLOBAL      -                  -                  1
	VAR_REGISTRY    -                  -                  1
	VAR_MAINTHREAD  -                  -                  1
	VAR_METATABLE   0/1 (**)           - (lua type)       ?/1
	VAR_USERVALUE   -                  -                  ?

	* : 0 indexed value, 1 next key
	** : 0 metatable of object ; 1 metatable of lua type
*/

#define VAR_FRAME_LOCAL 0	// stack(frame, index)
#define VAR_FRAME_FUNC 1 // stack(frame).func
#define VAR_INDEX  2	// table[const key]
#define VAR_INDEX_OBJ 3	// table[object key]
#define VAR_UPVALUE 4	// func[index]
#define VAR_GLOBAL 5	// _G
#define VAR_REGISTRY 6	// REGISTRY
#define VAR_MAINTHREAD 7
#define VAR_METATABLE 8	// table.metatable
#define VAR_USERVALUE 9	// userdata.uservalue

/* host 的变量在 debuger 都是以 value 的形式来表示，每个 value 描述了如何定位 host 中的一个变量
* 以上定位类型是否必要和完备呢？
* FUNCLIST, 局部变量、全局变量、REGISTRY、Table[x]、Table.metatable、UpValue、userdata.uservalue、MAINTHREAD
*
* 如果变量是非引用类型可以直接 copy 到 debuger 中来，否则的话才需要用 value 来层层定位
*
* 需要一个变量更具体的信息，需要将 value 解压到原始 state 然后再分析
*
* value 的 uservalue 怎么使用的?
*/
struct value {
	uint8_t type;
	uint16_t frame;
	int index;	
};

// return record number of value 
// 表明定位到具体的值需要多少个 value
static int
sizeof_value(struct value *v) {
	switch (v->type) {
	case VAR_FRAME_LOCAL:
	case VAR_FRAME_FUNC:
	case VAR_GLOBAL:
	case VAR_REGISTRY:
	case VAR_MAINTHREAD:
		return 1;
	case VAR_INDEX_OBJ:
		return 1 + v->index + sizeof_value(v+1+v->index);	// ??
	case VAR_METATABLE:
		if (v->frame == 1) {
			return 1;
		}
		// go through
	case VAR_INDEX:
	case VAR_UPVALUE:
	case VAR_USERVALUE:
		return 1 + sizeof_value(v+1);
	}
	return 0;
}

// copy a value from -> to, return the lua type of copied or LUA_TNONE
// 进行一个非引用 object 的 copy
// +1
static int
copy_value(lua_State *from, lua_State *to) {
	int t = lua_type(from, -1);
	switch(t) {
	case LUA_TNIL:
		lua_pushnil(to);
		break;
	case LUA_TBOOLEAN:
		lua_pushboolean(to, lua_toboolean(from,-1));
		break;
	case LUA_TNUMBER:
		if (lua_isinteger(from, -1)) {
			lua_pushinteger(to, lua_tointeger(from, -1));
		} else {
			lua_pushnumber(to, lua_tonumber(from, -1));
		}
		break;
	case LUA_TSTRING: {
		size_t sz;
		const char *str = lua_tolstring(from, -1, &sz);
		lua_pushlstring(to, str, sz);
		break;
		}
	case LUA_TLIGHTUSERDATA:
		lua_pushlightuserdata(to, lua_touserdata(from, -1));
		break;
	default:
		return LUA_TNONE;
	}
	return t;
}

// L top : uservalue
// return 类型，将 value 具体的值拷贝到 cL 去
static int
eval_value_(lua_State *L, lua_State *cL, struct value *v) {
	if (lua_checkstack(cL, 3) == 0)
		return luaL_error(L, "stack overflow");

	switch (v->type) {
	case VAR_FRAME_LOCAL: {
		lua_Debug ar;
		if (lua_getstack(cL, v->frame, &ar) == 0)
			break;
		const char * name = lua_getlocal(cL, &ar, v->index);
		if (name) {
			return lua_type(cL, -1);
		}
		break;
	}
	case VAR_FRAME_FUNC: {
		lua_Debug ar;
		if (lua_getstack(cL, v->frame, &ar) == 0)
			break;
		if (lua_getinfo(cL, "f", &ar) == 0)
			break;
		return LUA_TFUNCTION;
	}
	case VAR_INDEX:
	case VAR_INDEX_OBJ: {
		int t = eval_value_(L, cL, v+1);
		if (t == LUA_TNONE)
			break;
		if (t != LUA_TTABLE) {
			// only table can be index
			lua_pop(cL, 1);
			break;
		}
		if (v->type == VAR_INDEX) {
			if (v->index == 0) {
				lua_pushnil(L);
			} else {
				lua_rawgeti(L, -1, v->index);
			}
			if (copy_value(L, cL) == LUA_TNONE) {
				lua_pop(L, 1);
				lua_pop(cL, 1);
				break;
			}
			lua_pop(L, 1);	// pop key
		} else {
			if (eval_value_(L, cL, v+1+v->index) == LUA_TNONE) {
				lua_pop(cL, 1);	// pop table
				break;
			}
		}
		if (v->frame == 0) {
			// index key
			lua_rawget(cL, -2);
			lua_replace(cL, -2);
			return lua_type(cL, -1);
		} else {
			// next key
			if (lua_next(cL, -2) == 0) {
				lua_pop(cL, 1);	// pop table
				break;
			}
			lua_pop(cL, 1);	// pop value
			lua_replace(cL, -2);
			return lua_type(cL, -1);
		}
	}
	case VAR_UPVALUE: {
		int t = eval_value_(L, cL, v+1);
		if (t == LUA_TNONE)
			break;
		if (t != LUA_TFUNCTION) {
			// only function has upvalue
			lua_pop(cL, 1);
			break;
		}
		if (lua_getupvalue(cL, -1, v->index)) {
			lua_replace(cL, -2);	// remove function
			return lua_type(cL, -1);
		} else {
			lua_pop(L, 1);
			break;
		}
	}
	case VAR_GLOBAL:
		return lua_rawgeti(cL, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
	case VAR_REGISTRY:
		lua_pushvalue(cL, LUA_REGISTRYINDEX);
		return LUA_TTABLE;
	case VAR_MAINTHREAD:
		return lua_rawgeti(cL, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
	case VAR_METATABLE:
		if (v->frame == 1) {
			switch(v->index) {
			case LUA_TNIL:
				lua_pushnil(cL);
				break;
			case LUA_TBOOLEAN:
				lua_pushboolean(cL, 0);
				break;
			case LUA_TNUMBER:
				lua_pushinteger(cL, 0);
				break;
			case LUA_TSTRING:
				lua_pushstring(cL, "");
				break;
			case LUA_TLIGHTUSERDATA:
				lua_pushlightuserdata(cL, NULL);
				break;
			case LUA_TTHREAD:
				lua_rawgeti(cL, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
				break;
			default:
				return LUA_TNONE;
			}
		} else {
			int t = eval_value_(L, cL, v+1);
			if (t == LUA_TNONE)
				break;
			if (t != LUA_TTABLE && t != LUA_TUSERDATA) {
				lua_pop(cL, 1);
				break;
			}
		}
		if (lua_getmetatable(cL, -1)) {
			lua_replace(cL, -2);
			return LUA_TTABLE;
		} else {
			lua_pop(cL, 1);
			break;
		}
	case VAR_USERVALUE: {
		int t = eval_value_(L, cL, v+1);
		if (t == LUA_TNONE)
			break;
		if (t != LUA_TUSERDATA) {
			lua_pop(cL, 1);
			break;
		}
		return lua_getuservalue(cL, -1);
	}
	}
	return LUA_TNONE;
}

// extract L top into cL, return the lua type or LUA_TNONE(failed)
// extract 是可以解压到最底层，返回最终变量的类型
// 将一个 value 解压到它来源的 stack，这样可以方便遍历它
static int
eval_value(lua_State *L, lua_State *cL) {
	if (lua_checkstack(cL, 1) == 0)
		return luaL_error(L, "stack overflow");
	int t = copy_value(L, cL);	// 非引用的可以直接 copy
	if (t != LUA_TNONE) {
		return t;
	}
	t = lua_type(L, -1);
	if (t == LUA_TUSERDATA) {	// 引用类型只能通过层层定位到最终值
		struct value *v = lua_touserdata(L, -1);
		lua_getuservalue(L, -1);	// 获得 userdata 关联的 uservalue
		t = eval_value_(L, cL, v);
		lua_pop(L, 1);	// pop uservalue
		return t;
	}
	return LUA_TNONE;
}

// 从 cL 获得一个值写入 L, cL 通常是 host state，而 L 是 debuger 的
// 一般是传入一个定位，将定位到的最终值写入
// in value_addr, out value
static void
get_value(lua_State *L, lua_State *cL) {
	if (eval_value(L, cL) == LUA_TNONE) {	// 先从 L 弄一个值过去缓存? a trick
		lua_pop(L, 1);
		lua_pushnil(L);
		// failed
		return;
	}
	lua_pop(L, 1);	// 已经 extract 过去了，本地无需再保存
	if (copy_value(cL, L) == LUA_TNONE) {	// copy 回来
		lua_pushfstring(L, "[%s: %p]", 
			lua_typename(cL, lua_type(cL, -1)),
			lua_topointer(cL, -1)
			);
	}
	lua_pop(cL,1);	// host 弹出之前缓存的
}


static const char *
get_type(lua_State *L, lua_State *cL) {
	int t = eval_value(L, cL);
	lua_pop(cL,1);
	return lua_typename(L, t);
}

/* 从 cL 获得一个 local 变量相关信息，将其存入一个 userdata 中，并压入 L */
static const char *
get_frame_local(lua_State *L, lua_State *cL, int frame, int index) {
	lua_Debug ar;
	if (lua_getstack(cL, frame, &ar) == 0) {
		return NULL;
	}
	if (lua_checkstack(cL, 1) == 0) {
		luaL_error(L, "stack overflow");
	}
	const char * name = lua_getlocal(cL, &ar, index);
	if (name == NULL)
		return NULL;
	if (copy_value(cL, L) != LUA_TNONE) {
		lua_pop(cL, 1);
		return name;
	}
	lua_pop(cL, 1);
	struct value *v = lua_newuserdata(L, sizeof(struct value));
	v->type = VAR_FRAME_LOCAL;
	v->frame = frame;
	v->index = index;
	return name;
}

static int
get_frame_func(lua_State *L, lua_State *cL, int frame) {
	lua_Debug ar;
	if (lua_getstack(cL, frame, &ar) == 0) {
		return 0;
	}
	if (lua_checkstack(cL, 1) == 0) {
		luaL_error(L, "stack overflow");
	}
	if (lua_getinfo(cL, "f", &ar) == 0) {
		return 0;
	}
	lua_pop(cL, 1);

	struct value *v = lua_newuserdata(L, sizeof(struct value));
	v->type = VAR_FRAME_FUNC;
	v->frame = frame;
	v->index = 0;
	return 1;
}

/* 将 index 所指的value 的 uservalue 拷贝到 top 上的 v 的 uservalue*/
// 不改变栈大小
// value(top)
static void
copy_table(lua_State *L, int index) {
	if (lua_getuservalue(L, index) == LUA_TTABLE) { // value, uservalue
		int n = lua_rawlen(L, -1);
		lua_createtable(L, n, 0);	// value, uservalue, table
		// v, ut, []
		int i;
		for (i=1;i<=n;i++) {
			lua_rawgeti(L, -2, i);	// value, uservalue, table, uservalue[i]
			lua_rawseti(L, -2, i);	// value, uservalue, table
		}
		lua_setuservalue(L, -3);	// value, uservalue
	}
	lua_pop(L, 1); // value
}

// table(v) 的 uservalue 用来定位其下item的，它是 key 的集合
// table key
static void
new_index(lua_State *L, int type) {
	struct value *t = lua_touserdata(L, -2);
	int sz = sizeof_value(t);
	struct value *v = lua_newuserdata(L, sizeof(struct value) * (sz + 1));
	v->type = VAR_INDEX;
	v->frame = type;
	memcpy(v+1,t,sz * sizeof(struct value));	// value 放了 table的定位，因为只有找到table才能找到它

	// 然后看在 table 中怎么找到自己
	// t k v
	copy_table(L, -3);	// copy uservalue from t to v
	if (lua_isnil(L, -2)) {
		// key is nil
		v->index = 0;
	} else {
		if (lua_getuservalue(L, -3) != LUA_TTABLE) {
			lua_pop(L, 1);	// t, k, v
			lua_createtable(L, 1, 0);	// t, k, v, []
		}
		// t k v []
		int n = lua_rawlen(L, -1);
		lua_pushvalue(L, -3);
		// t k v [] k
		lua_rawseti(L, -2, n+1);
		// t k v [... k]
		lua_setuservalue(L, -2);	// v.uservalue = [...k]
		v->index = n+1;	// v 的 k 在uservalue 的位置
		// 由此看出对于 key 是基本对象的 table[key]，它的定位方式是:
		// v.uservalue 保存了一个 key 的 array, 这个 array 和 table 关联的
		// 通过 v.index 可以访问到这个 key
		// t k v
	}
}

// 不改变栈大小
static int
append_table(lua_State *L, int index) {
	if (lua_getuservalue(L, index) != LUA_TTABLE) {
		lua_pop(L, 1);
		return 0;
	}
	// ..., v , [uv]
	if (lua_getuservalue(L, -2) != LUA_TTABLE) {
		lua_pop(L, 2);
		// ..., v
		copy_table(L, index);
		return 0;
	}
	// ..., v, [from_uv], [to_uv]
	int offset = lua_rawlen(L, -1);
	int i;
	for (i=1;;i++) {
		if (lua_rawgeti(L, -2, i) == LUA_TNIL) {	// from[i]
			// ..., v, [], [] , nil
			break;
		}
		lua_rawseti(L, -2, i + offset);	// ..., v, [], []
	}
	lua_pop(L, 3);
	return offset;
}

// table key(top)
// 1. 定位到 table
// 2. 定位到 key
// 3. 从 table 中根据 key 定位到 value
static void
new_index_object(lua_State *L, int type) {
	struct value *t = lua_touserdata(L, -2);
	int ts = sizeof_value(t);
	struct value *k = lua_touserdata(L, -1);
	int ks = sizeof_value(k);

	struct value *v = lua_newuserdata(L, sizeof(struct value) * (ts + ks + 1));
	v->type = VAR_INDEX_OBJ;
	v->frame = type;
	v->index = ts;	// 多少个步骤才定位到所在的table

	// step1
	memcpy(v+1,t,ts * sizeof(struct value));

	// t k v step3.1
	copy_table(L, -3);	// copy uservalue from t to v, table 的查找表

	// step2
	memcpy(v+1+ts,k,ks * sizeof(struct value));

	// step3.2
	int offset = append_table(L, -2);	// move uservalue from k to v, key 的查找表
	if (offset) {
		int i;
		v = v+1+ts;	// k start
		for (i=0;i<ks;i++,v++) {
			if (v->type == VAR_INDEX && v->index != 0) {
				v->index += offset;	// k 在表中的位置
			}
		}
	}
}

// L: table key(top)
static int
table_key(lua_State *L, lua_State *cL) {
	if (lua_checkstack(cL, 3) == 0) {
		return luaL_error(L, "stack overflow");
	}
	lua_insert(L, -2);	// L : key table (top)
	int t = eval_value(L, cL);	// table 放到对面去
	if (t != LUA_TTABLE) {
		lua_pop(cL, 1);	// pop table
		lua_pop(L, 2);	// pop k/t
		return 0;
	}
	lua_insert(L, -2);	// L : table key (top)
	if (eval_value(L, cL) == LUA_TNONE) {	// key 也放到对面
		lua_pop(cL, 1);	// pop table
		lua_pop(L, 2);	// pop k/t
		return 0;
	}
	return 1;
}

// L: table, key(top)	cL:table value (top)
static void
combine_tk(lua_State *L, lua_State *cL, int type) {
	if (copy_value(cL, L) != LUA_TNONE) { // 非引用类型，直接将 value 考到 L L: t, k, v
		lua_pop(cL, 2);		// cL: L: t, k, v
		// L : t, k, v
		lua_replace(L, -3);	// L: v, k
		lua_pop(L, 1); // v
		return;
	}
	lua_pop(cL, 2);	// pop t v from cL
	// L : t, k
	if (lua_type(L, -1) == LUA_TUSERDATA) {
		// key is object
		new_index_object(L, type);
	} else {
		new_index(L, type);
	}
	// L : t, k, v
	lua_replace(L, -3);	// v,k
	lua_pop(L, 1); // v
}

// table, key(top)
static int
get_index(lua_State *L, lua_State *cL) {
	if (table_key(L, cL) == 0)
		return 0;
	lua_rawget(cL, -2);	// cL : table value(top)
	combine_tk(L, cL, 0);
	return 1;
}

// table last_key
static int
next_key(lua_State *L, lua_State *cL) {
	if (table_key(L, cL) == 0)
		return 0;
	if (lua_next(cL, -2) == 0) {
		lua_pop(cL, 1);	// remove table
		return 0;
	}
	lua_pop(cL, 1);	// remove value
	combine_tk(L, cL, 1);
	return 1;
}

// stack: function (value)
static const char *
get_upvalue(lua_State *L, lua_State *cL, int index) {
	if (lua_type(L, -1) != LUA_TUSERDATA) {	// func 是以 value 的方式表示
		lua_pop(L, 1);
		return NULL;
	}
	int t = eval_value(L, cL);	// 将 function 拷贝到 cL
	if (t == LUA_TNONE) {	// copy 失败
		lua_pop(L, 1);	// remove function object
		return NULL;
	}
	if (t != LUA_TFUNCTION) {	// copy 的不是 function
		lua_pop(L, 1);	// remove function object
		lua_pop(cL, 1);	// remove none function
		return NULL;
	}
	const char *name = lua_getupvalue(cL, -1, index);	// 在原始 state 获得upvalue相关信息
	if (name == NULL) {
		lua_pop(L, 1);	// remove function object
		lua_pop(cL, 1);	// remove function
		return NULL;
	}
	if (copy_value(cL, L) != LUA_TNONE) {	// 将 upvalue 拷回
		lua_replace(L, -2);	// remove function object
		lua_pop(cL, 1);
		return name;
	}
	// 运行到这里表示 upvalue 是复杂类型，只能一层一层取
	lua_pop(cL, 2);	// remove func / upvalue
	struct value *f = lua_touserdata(L, -1);
	int sz = sizeof_value(f);
	struct value *v = lua_newuserdata(L, sizeof(struct value) * (1+sz));
	v->type = VAR_UPVALUE;
	v->frame = 0;
	v->index = index;
	memcpy(v+1, f, sizeof(struct value) * sz);	// 函数也要带上 f, v
	copy_table(L, -2);	// 将vf的uservalue 拷贝到新的 v上
	lua_replace(L, -2);	// remove function object
	return name;
}

static struct value *
get_registry(lua_State *L, int type) {
	switch (type) {
	case VAR_GLOBAL:
	case VAR_REGISTRY:
	case VAR_MAINTHREAD:
		break;
	default:
		return NULL;
	}
	struct value * v = lua_newuserdata(L, sizeof(struct value));
	v->frame = 0;
	v->index = 0;
	v->type = type;
	return v;
}

// value
static struct value *
get_metatable(lua_State *L, lua_State *cL) {
	if (lua_checkstack(cL, 2)==0)
		luaL_error(L, "stack overflow");
	int t = eval_value(L, cL);	// cL: value
	if (t == LUA_TNONE) {
		lua_pop(L, 1);
		return NULL;
	}
	if (lua_getmetatable(cL,-1) == 0) {	// cL:value, mt
		lua_pop(L, 1);
		lua_pop(cL, 1);
		return NULL;
	}
	lua_pop(cL, 2);
	if (t == LUA_TTABLE || t == LUA_TUSERDATA) {
		struct value *t = lua_touserdata(L, -1);
		int sz = sizeof_value(t);
		struct value *v = lua_newuserdata(L, sizeof(struct value) * (sz + 1));
		v->type = VAR_METATABLE;
		v->frame = 0;	// table 或者 userdata
		v->index = 0;
		memcpy(v+1,t,sz * sizeof(struct value));
		// t v
		copy_table(L, -2);
		lua_replace(L, -2);
		return v;
	} else {
		lua_pop(L, 1);
		struct value *v = lua_newuserdata(L, sizeof(struct value));
		v->type = VAR_METATABLE;
		v->frame = 1;
		v->index = t;
		return v;
	}
}

static int
get_uservalue(lua_State *L, lua_State *cL) {
	if (lua_checkstack(cL, 2)==0)
		return luaL_error(L, "stack overflow");
	int t = eval_value(L, cL);
	if (t == LUA_TNONE) {
		lua_pop(L, 1);
		return 0;
	}
	if (t != LUA_TUSERDATA) {
		lua_pop(cL, 1);
		lua_pop(L, 1);
		return 0;
	}
	lua_getuservalue(cL, -1);
	if (copy_value(cL, L) != LUA_TNONE) {
		lua_pop(cL, 2);	// pop userdata / uservalue
		lua_replace(L, -2);
		return 1;
	}
	// L : value
	// cL : value uservalue
	struct value *u = lua_touserdata(L, -1);
	int sz = sizeof_value(u);
	struct value *v = lua_newuserdata(L, sizeof(struct value) * (sz + 1));
	v->type = VAR_USERVALUE;
	v->frame = 0;
	v->index = 0;
	memcpy(v+1,u,sz * sizeof(struct value));
	// u v
	copy_table(L, -2);
	lua_replace(L, -2);
	return 1;
}

static void
show_detail_(lua_State *L, luaL_Buffer *b, struct value *v, int top) {
	luaL_checkstack(L, 3, NULL);
	switch(v->type) {
	case VAR_FRAME_LOCAL:
		lua_pushfstring(L, "(L %d %d)",v->frame,v->index);
		luaL_addvalue(b);
		break;
	case VAR_FRAME_FUNC:
		lua_pushfstring(L, "(f %d)",v->frame);
		luaL_addvalue(b);
		break;
	case VAR_UPVALUE:
		show_detail_(L, b, v+1, top);
		lua_pushfstring(L, ".u[%d]",v->index);
		luaL_addvalue(b);
		break;
	case VAR_METATABLE:
		if (v->frame == 1) {
			lua_pushfstring(L, "(%s mt)", lua_typename(L, v->index));
			luaL_addvalue(b);
		} else {
			luaL_addchar(b, '(');
			show_detail_(L, b, v+1, top);
			luaL_addstring(b, " mt)");
		}
		break;
	case VAR_USERVALUE:
		luaL_addchar(b, '(');
		show_detail_(L, b, v+1, top);
		luaL_addstring(b, " uv)");
		break;
	case VAR_GLOBAL:
		luaL_addstring(b, "_G");
		break;
	case VAR_REGISTRY:
		luaL_addstring(b, "_REGISTRY");
		break;
	case VAR_MAINTHREAD:
		luaL_addstring(b, "_MAINTHREAD");
		break;
	case VAR_INDEX:
		if (v->frame == 0) {
			show_detail_(L, b, v+1, top);
			luaL_addchar(b, '.');
			lua_rawgeti(L, top, v->index);
			size_t sz;
			const char *str = luaL_tolstring(L, -1, &sz);
			luaL_addlstring(b, str, sz);
			lua_pop(L, 1);
		} else {
			luaL_addstring(b, "(next ");
			show_detail_(L, b, v+1, top);
			luaL_addchar(b, ' ');
			lua_rawgeti(L, top, v->index);
			size_t sz;
			const char *str = luaL_tolstring(L, -1, &sz);
			luaL_addlstring(b, str, sz);
			lua_pop(L, 1);
			luaL_addchar(b, ')');
		}
		break;
	case VAR_INDEX_OBJ:
		if (v->frame == 0) {
			show_detail_(L, b, v+1, top);
			luaL_addchar(b, '[');
			show_detail_(L, b, v+1+v->index, top);
			luaL_addchar(b, ']');
		} else {
			luaL_addstring(b, "(next ");
			show_detail_(L, b, v+1, top);
			luaL_addchar(b, ' ');
			show_detail_(L, b, v+1+v->index, top);
			luaL_addchar(b, ')');
		}
		break;
	}
}

static int
show_detail(lua_State *L) {
	if (lua_type(L, -1) == LUA_TUSERDATA) {
		struct value *v = lua_touserdata(L, -1);
		lua_getuservalue(L, -1);
		int top = lua_gettop(L);
		luaL_Buffer b;
		luaL_buffinit(L, &b);
		show_detail_(L, &b, v, top);
		luaL_pushresult(&b);		
	} else {
		lua_pushfstring(L, "[%s:%s]", lua_typename(L, lua_type(L, -1)), luaL_tolstring(L, -1, NULL));
	}
	return 1;
}

#endif
