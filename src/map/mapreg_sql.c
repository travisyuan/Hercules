// Copyright (c) Hercules Dev Team, licensed under GNU GPL.
// See the LICENSE file
// Portions Copyright (c) Athena Dev Teams

#include "../common/cbasetypes.h"
#include "../common/db.h"
#include "../common/ers.h"
#include "../common/malloc.h"
#include "../common/showmsg.h"
#include "../common/sql.h"
#include "../common/strlib.h"
#include "../common/timer.h"
#include "map.h" // map->mysql_handle
#include "script.h"
#include "mapreg.h"
#include <stdlib.h>
#include <string.h>

struct mapreg_interface mapreg_s;

#define MAPREG_AUTOSAVE_INTERVAL (300*1000)

/* TODO may be worth not to save/store arrays with the SCRIPT_ARRAY_LEN_IDX index */

/// Looks up the value of an integer variable using its uid.
int mapreg_readreg(int64 uid) {
	struct mapreg_save *m = i64db_get(mapreg->db, uid);
	return m?m->u.i:0;
}

/// Looks up the value of a string variable using its uid.
char* mapreg_readregstr(int64 uid) {
	struct mapreg_save *m = i64db_get(mapreg->str_db, uid);
	return m?m->u.str:NULL;
}

/// Modifies the value of an integer variable.
bool mapreg_setreg(int64 uid, int val) {
	struct mapreg_save *m;
	int num = (int32)(uid & 0xFFFFFFFF);
	int i   = (int32)((uid >> 32) & 0xFFFFFFFF);
	const char* name = script->get_str(num);

	if( val != 0 ) {
		if( (m = i64db_get(mapreg->db,uid)) ) {
			m->u.i = val;
			if(name[1] != '@' && i != SCRIPT_ARRAY_LEN_IDX) {// $@ isn't saved; index SCRIPT_ARRAY_LEN_IDX also isn't saved;
				m->save = true;
				mapreg->i_dirty = true;
			}
		} else {
			m = ers_alloc(mapreg->ers, struct mapreg_save);

			m->u.i = val;
			m->uid = uid;
			m->save = false;

			if(name[1] != '@' && i != SCRIPT_ARRAY_LEN_IDX) {// $@ isn't saved; index SCRIPT_ARRAY_LEN_IDX also isn't saved;
				// write new variable to database
				char tmp_str[32*2+1];
				SQL->EscapeStringLen(map->mysql_handle, tmp_str, name, strnlen(name, 32));
				if( SQL_ERROR == SQL->Query(map->mysql_handle, "INSERT INTO `%s`(`varname`,`index`,`value`) VALUES ('%s','%d','%d')", mapreg->table, tmp_str, i, val) )
					Sql_ShowDebug(map->mysql_handle);
			}
			i64db_put(mapreg->db, uid, m);
		}
	} else { // val == 0
		if( (m = i64db_get(mapreg->db,uid)) ) {
			ers_free(mapreg->ers, m);
		}
		i64db_remove(mapreg->db,uid);

		if( name[1] != '@' && i != SCRIPT_ARRAY_LEN_IDX) {// $@ isn't saved; index SCRIPT_ARRAY_LEN_IDX also isn't saved;
			if( SQL_ERROR == SQL->Query(map->mysql_handle, "DELETE FROM `%s` WHERE `varname`='%s' AND `index`='%d'", mapreg->table, name, i) )
				Sql_ShowDebug(map->mysql_handle);
		}
	}

	return true;
}

/// Modifies the value of a string variable.
bool mapreg_setregstr(int64 uid, const char* str) {
	struct mapreg_save *m;
	int num = (int)(uid & 0xFFFFFFFF);
	int i   = (int32)((uid >> 32) & 0xFFFFFFFF);
	const char* name = script->get_str(num);
	
	if( str == NULL || *str == 0 ) {
		if(name[1] != '@' && i != SCRIPT_ARRAY_LEN_IDX) {// $@ isn't saved; index SCRIPT_ARRAY_LEN_IDX also isn't saved;
			if( SQL_ERROR == SQL->Query(map->mysql_handle, "DELETE FROM `%s` WHERE `varname`='%s' AND `index`='%d'", mapreg->table, name, i) )
				Sql_ShowDebug(map->mysql_handle);
		}
		if( (m = i64db_get(mapreg->str_db,uid)) ) {
			if( m->u.str != NULL )
				aFree(m->u.str);
			ers_free(mapreg->ers, m);
		}
		i64db_remove(mapreg->str_db,uid);
	} else {
		if( (m = i64db_get(mapreg->str_db,uid)) ) {
			if( m->u.str != NULL )
				aFree(m->u.str);
			m->u.str = aStrdup(str);
			if(name[1] != '@' && i != SCRIPT_ARRAY_LEN_IDX) {// $@ isn't saved; index SCRIPT_ARRAY_LEN_IDX also isn't saved;
				mapreg->str_dirty = true;
				m->save = true;
			}
		} else {
			m = ers_alloc(mapreg->ers, struct mapreg_save);

			m->uid = uid;
			m->u.str = aStrdup(str);
			m->save = false;
			
			if(name[1] != '@' && i != SCRIPT_ARRAY_LEN_IDX) {// $@ isn't saved; index SCRIPT_ARRAY_LEN_IDX also isn't saved;
				//put returned null, so we must insert.
				char tmp_str[32*2+1];
				char tmp_str2[255*2+1];
				SQL->EscapeStringLen(map->mysql_handle, tmp_str, name, strnlen(name, 32));
				SQL->EscapeStringLen(map->mysql_handle, tmp_str2, str, strnlen(str, 255));
				if( SQL_ERROR == SQL->Query(map->mysql_handle, "INSERT INTO `%s`(`varname`,`index`,`value`) VALUES ('%s','%d','%s')", mapreg->table, tmp_str, i, tmp_str2) )
					Sql_ShowDebug(map->mysql_handle);
			}
			i64db_put(mapreg->str_db, uid, m);
		}
	}

	return true;
}

/// Loads permanent variables from database
void script_load_mapreg(void) {
	/*
	        0        1       2
	   +-------------------------+
	   | varname | index | value |
	   +-------------------------+
	                                */
	SqlStmt* stmt = SQL->StmtMalloc(map->mysql_handle);
	char varname[32+1];
	int index;
	char value[255+1];
	uint32 length;

	if ( SQL_ERROR == SQL->StmtPrepare(stmt, "SELECT `varname`, `index`, `value` FROM `%s`", mapreg->table)
	  || SQL_ERROR == SQL->StmtExecute(stmt)
	  ) {
		SqlStmt_ShowDebug(stmt);
		SQL->StmtFree(stmt);
		return;
	}

	SQL->StmtBindColumn(stmt, 0, SQLDT_STRING, &varname[0], sizeof(varname), &length, NULL);
	SQL->StmtBindColumn(stmt, 1, SQLDT_INT, &index, 0, NULL, NULL);
	SQL->StmtBindColumn(stmt, 2, SQLDT_STRING, &value[0], sizeof(value), NULL, NULL);
	
	while ( SQL_SUCCESS == SQL->StmtNextRow(stmt) ) {
		struct mapreg_save *m = NULL;
		int s = script->add_str(varname);
		int i = index;
		DBMap *mdb = mapreg->db;

		if( varname[length-1] == '$' ) {
			mdb = mapreg->str_db;
		}
		
		if( i64db_exists(mdb, reference_uid(s,i)) ) {
			ShowWarning("load_mapreg: duplicate! '%s' (%d) => '%s' skipping...\n",varname,index,value);
			continue;
		}
		
		m = ers_alloc(mapreg->ers, struct mapreg_save);
		m->uid = reference_uid(s,i);
		m->save = false;
		
		if( varname[length-1] == '$' ) {
			m->u.str = aStrdup(value);
		} else {
			m->u.i = atoi(value);
		}
		
		i64db_put(mdb, m->uid, m);
		
		if( i > 0 ) {
			if( (m = i64db_get(mdb, reference_uid(s,SCRIPT_ARRAY_LEN_IDX))) ) {
				if( varname[length-1] == '$' ) {
					int len;
					
					if( (len = atoi(m->u.str)) < i ) {
						char v[11];
						
						aFree(m->u.str);
						snprintf(v,11,"%d",i+1);
						m->u.str = aStrdup(v);
					}
				} else {
					if( m->u.i < i )
						m->u.i = i+1;
				}
			} else {
				m = ers_alloc(mapreg->ers, struct mapreg_save);
				m->uid = reference_uid(s,SCRIPT_ARRAY_LEN_IDX);
				m->save = false;
				
				if( varname[length-1] == '$' ) {
					char v[11];
					
					snprintf(v,11,"%d",i+1);
					m->u.str = aStrdup(v);
				} else {
					m->u.i = i+1;
				}
				i64db_put(mdb, m->uid, m);
			}
		}
	}
	
	SQL->StmtFree(stmt);

	mapreg->i_dirty = false;
	mapreg->str_dirty = false;
}

/// Saves permanent variables to database
void script_save_mapreg(void) {
	DBIterator* iter;
	struct mapreg_save *m = NULL;

	if( mapreg->i_dirty ) {
		iter = db_iterator(mapreg->db);
		for( m = dbi_first(iter); dbi_exists(iter); m = dbi_next(iter) ) {
			if( m->save ) {
				int num = (int32)(m->uid & 0xFFFFFFFF);
				int i   = (int32)((m->uid >> 32) & 0xFFFFFFFF);
				const char* name = script->get_str(num);

				if( SQL_ERROR == SQL->Query(map->mysql_handle, "UPDATE `%s` SET `value`='%d' WHERE `varname`='%s' AND `index`='%d' LIMIT 1", mapreg->table, m->u.i, name, i) )
					Sql_ShowDebug(map->mysql_handle);
				m->save = false;
			}
		}
		dbi_destroy(iter);
		mapreg->i_dirty = false;
	}

	if( mapreg->str_dirty ) {
		iter = db_iterator(mapreg->str_db);
		for( m = dbi_first(iter); dbi_exists(iter); m = dbi_next(iter) ) {
			if( m->save ) {
				int num = (int32)(m->uid & 0xFFFFFFFF);
				int i   = (int32)((m->uid >> 32) & 0xFFFFFFFF);
				const char* name = script->get_str(num);
				char tmp_str2[2*255+1];

				SQL->EscapeStringLen(map->mysql_handle, tmp_str2, m->u.str, safestrnlen(m->u.str, 255));
				if( SQL_ERROR == SQL->Query(map->mysql_handle, "UPDATE `%s` SET `value`='%s' WHERE `varname`='%s' AND `index`='%d' LIMIT 1", mapreg->table, tmp_str2, name, i) )
					Sql_ShowDebug(map->mysql_handle);
				m->save = false;
			}
		}
		dbi_destroy(iter);
		mapreg->str_dirty = false;
	}
}

int script_autosave_mapreg(int tid, unsigned int tick, int id, intptr_t data) {
	mapreg->save();
	return 0;
}


void mapreg_reload(void) {
	DBIterator* iter;
	struct mapreg_save *m = NULL;

	mapreg->save();

	iter = db_iterator(mapreg->db);
	for( m = dbi_first(iter); dbi_exists(iter); m = dbi_next(iter) ) {
		ers_free(mapreg->ers, m);
	}
	dbi_destroy(iter);
	
	iter = db_iterator(mapreg->str_db);
	for( m = dbi_first(iter); dbi_exists(iter); m = dbi_next(iter) ) {
		if( m->u.str != NULL ) {
			aFree(m->u.str);
		}
		ers_free(mapreg->ers, m);
	}
	dbi_destroy(iter);
	
	db_clear(mapreg->db);
	db_clear(mapreg->str_db);

	mapreg->load();
}

void mapreg_final(void) {
	DBIterator* iter;
	struct mapreg_save *m = NULL;
	
	mapreg->save();

	iter = db_iterator(mapreg->db);
	for( m = dbi_first(iter); dbi_exists(iter); m = dbi_next(iter) ) {
		ers_free(mapreg->ers, m);
	}
	dbi_destroy(iter);
	
	iter = db_iterator(mapreg->str_db);
	for( m = dbi_first(iter); dbi_exists(iter); m = dbi_next(iter) ) {
		if( m->u.str != NULL ) {
			aFree(m->u.str);
		}
		ers_free(mapreg->ers, m);
	}
	dbi_destroy(iter);
		
	db_destroy(mapreg->db);
	db_destroy(mapreg->str_db);
	
	ers_destroy(mapreg->ers);
}

void mapreg_init(void) {
	mapreg->db = i64db_alloc(DB_OPT_BASE);
	mapreg->str_db = i64db_alloc(DB_OPT_BASE);
	mapreg->ers = ers_new(sizeof(struct mapreg_save), "mapreg_sql.c::mapreg_ers", ERS_OPT_NONE);

	mapreg->load();

	timer->add_func_list(mapreg->save_timer, "mapreg_script_autosave_mapreg");
	timer->add_interval(timer->gettick() + MAPREG_AUTOSAVE_INTERVAL, mapreg->save_timer, 0, 0, MAPREG_AUTOSAVE_INTERVAL);
}

bool mapreg_config_read(const char* w1, const char* w2) {
	if(!strcmpi(w1, "mapreg_db"))
		safestrncpy(mapreg->table, w2, sizeof(mapreg->table));
	else
		return false;

	return true;
}
void mapreg_defaults(void) {
	mapreg = &mapreg_s;
	
	/* */
	mapreg->db = NULL;
	mapreg->str_db = NULL;
	mapreg->ers = NULL;
	
	safestrncpy(mapreg->table, "mapreg", sizeof(mapreg->table));
	mapreg->i_dirty = false;
	mapreg->str_dirty = false;
	
	/* */
	mapreg->init = mapreg_init;
	mapreg->final = mapreg_final;
	
	/* */
	mapreg->readreg = mapreg_readreg;
	mapreg->readregstr = mapreg_readregstr;
	mapreg->setreg = mapreg_setreg;
	mapreg->setregstr = mapreg_setregstr;
	mapreg->load = script_load_mapreg;
	mapreg->save = script_save_mapreg;
	mapreg->save_timer = script_autosave_mapreg;
	mapreg->reload = mapreg_reload;
	mapreg->config_read = mapreg_config_read;

}
