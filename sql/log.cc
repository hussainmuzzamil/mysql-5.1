/* Copyright (c) 2000, 2011, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */


/**
  @file

  @brief
  logging of commands

  @todo
    Abort logging when we get an error in reading or writing log files
*/

#include "mysql_priv.h"
#include "sql_repl.h"
#include "rpl_filter.h"
#include "rpl_rli.h"
#include "rpl_mi.h"
#include "my_atomic.h"

#include <my_dir.h>
#include <stdarg.h>
#include <m_ctype.h>				// For test_if_number

#ifdef __NT__
#include "message.h"
#endif

#include <mysql/plugin.h>
#include "mysql_priv.h"
#include "debug_sync.h"

/** The max InnoDB allowed replication binlog filename length: the value should
    be the same as TRX_SYS_MYSQL_RELAY_NAME_LEN. */
#define MAX_INNODB_BINLOG_FILENAME_LEN      250

/* max size of the log message */
#define MAX_LOG_BUFFER_SIZE 1024
#define MAX_USER_HOST_SIZE 512
#define MAX_TIME_SIZE 32
#define MY_OFF_T_UNDEF (~(my_off_t)0UL)

#define FLAGSTR(V,F) ((V)&(F)?#F" ":"")

LOGGER logger;

MYSQL_BIN_LOG mysql_bin_log;
ulong sync_binlog_counter= 0;
ulong binlog_fsync_slow= 0;

static bool test_if_number(const char *str,
			   long *res, bool allow_wildcards);
static int binlog_init(void *p);
static int binlog_close_connection(handlerton *hton, THD *thd);
static int binlog_savepoint_set(handlerton *hton, THD *thd, void *sv);
static int binlog_savepoint_rollback(handlerton *hton, THD *thd, void *sv);
static int binlog_commit(handlerton *hton, THD *thd, bool all, bool async);
static int binlog_rollback(handlerton *hton, THD *thd, bool all);
static int binlog_prepare(handlerton *hton, THD *thd, bool all, bool async);

struct QueryLogEvent {
  const char *query_;
  const size_t query_length_;
};

/** Queries with the correct log position in the event */
QueryLogEvent query_with_log[] =
{
  { "BEGIN", strlen("BEGIN") },
  { "COMMIT", strlen("COMMIT") }
};

/**
  Silence all errors and warnings reported when performing a write
  to a log table.
  Errors and warnings are not reported to the client or SQL exception
  handlers, so that the presence of logging does not interfere and affect
  the logic of an application.
*/
class Silence_log_table_errors : public Internal_error_handler
{
  char m_message[MYSQL_ERRMSG_SIZE];
public:
  Silence_log_table_errors()
  {
    m_message[0]= '\0';
  }

  virtual ~Silence_log_table_errors() {}

  virtual bool handle_error(uint sql_errno, const char *message,
                            MYSQL_ERROR::enum_warning_level level,
                            THD *thd);
  const char *message() const { return m_message; }
};

bool
Silence_log_table_errors::handle_error(uint /* sql_errno */,
                                       const char *message_arg,
                                       MYSQL_ERROR::enum_warning_level /* level */,
                                       THD * /* thd */)
{
  strmake(m_message, message_arg, sizeof(m_message)-1);
  return TRUE;
}


sql_print_message_func sql_print_message_handlers[3] =
{
  sql_print_information,
  sql_print_warning,
  sql_print_error
};


char *make_default_log_name(char *buff,const char* log_ext)
{
  strmake(buff, pidfile_name, FN_REFLEN-5);
  return fn_format(buff, buff, mysql_data_home, log_ext,
                   MYF(MY_UNPACK_FILENAME|MY_REPLACE_EXT));
}

/*
  Helper class to hold a mutex for the duration of the
  block.

  Eliminates the need for explicit unlocking of mutexes on, e.g.,
  error returns.  On passing a null pointer, the sentry will not do
  anything.
 */
class Mutex_sentry
{
public:
  Mutex_sentry(pthread_mutex_t *mutex)
    : m_mutex(mutex)
  {
    if (m_mutex)
      pthread_mutex_lock(mutex);
  }

  ~Mutex_sentry()
  {
    if (m_mutex)
      pthread_mutex_unlock(m_mutex);
#ifndef DBUG_OFF
    m_mutex= 0;
#endif
  }

private:
  pthread_mutex_t *m_mutex;

  // It's not allowed to copy this object in any way
  Mutex_sentry(Mutex_sentry const&);
  void operator=(Mutex_sentry const&);
};

/*
  Helper class to store binary log transaction data.
*/
class binlog_trx_data {
public:
  binlog_trx_data()
    : at_least_one_stmt_committed(0), incident(FALSE), m_pending(0),
    before_stmt_pos(MY_OFF_T_UNDEF)
  {
    trans_log.end_of_file= max_binlog_cache_size;
  }

  ~binlog_trx_data()
  {
    DBUG_ASSERT(pending() == NULL);
    close_cached_file(&trans_log);
  }

  my_off_t position() const {
    return my_b_tell(&trans_log);
  }

  bool empty() const
  {
    return pending() == NULL && my_b_tell(&trans_log) == 0;
  }

  /*
    Truncate the transaction cache to a certain position. This
    includes deleting the pending event.
   */
  void truncate(my_off_t pos)
  {
    DBUG_PRINT("info", ("truncating to position %lu", (ulong) pos));
    DBUG_PRINT("info", ("before_stmt_pos=%lu", (ulong) pos));
    if (pending())
    {
      delete pending();
    }
    set_pending(0);
    reinit_io_cache(&trans_log, WRITE_CACHE, pos, 0, 0);
    trans_log.end_of_file= max_binlog_cache_size;
    if (pos < before_stmt_pos)
      before_stmt_pos= MY_OFF_T_UNDEF;

    /*
      The only valid positions that can be truncated to are at the
      beginning of a statement. We are relying on this fact to be able
      to set the at_least_one_stmt_committed flag correctly. In other word, if
      we are truncating to the beginning of the transaction cache,
      there will be no statements in the cache, otherwhise, we will
      have at least one statement in the transaction cache.
     */
    at_least_one_stmt_committed= (pos > 0);
  }

  /*
    Reset the entire contents of the transaction cache, emptying it
    completely.
   */
  void reset() {
    if (!empty())
      truncate(0);
    before_stmt_pos= MY_OFF_T_UNDEF;
    incident= FALSE;
    trans_log.end_of_file= max_binlog_cache_size;
    DBUG_ASSERT(empty());
  }

  Rows_log_event *pending() const
  {
    return m_pending;
  }

  void set_pending(Rows_log_event *const pending)
  {
    m_pending= pending;
  }

  IO_CACHE trans_log;                         // The transaction cache

  void set_incident(void)
  {
    incident= TRUE;
  }
  
  bool has_incident(void)
  {
    return(incident);
  }

  /**
    Boolean that is true if there is at least one statement in the
    transaction cache.
  */
  bool at_least_one_stmt_committed;
  bool incident;

private:
  /*
    Pending binrows event. This event is the event where the rows are
    currently written.
   */
  Rows_log_event *m_pending;

public:
  /*
    Binlog position before the start of the current statement.
  */
  my_off_t before_stmt_pos;
};

handlerton *binlog_hton;

bool LOGGER::is_log_table_enabled(uint log_table_type)
{
  switch (log_table_type) {
  case QUERY_LOG_SLOW:
    return (table_log_handler != NULL) && opt_slow_log;
  case QUERY_LOG_GENERAL:
    return (table_log_handler != NULL) && opt_log ;
  default:
    DBUG_ASSERT(0);
    return FALSE;                             /* make compiler happy */
  }
}


/* Check if a given table is opened log table */
int check_if_log_table(uint db_len, const char *db, uint table_name_len,
                       const char *table_name, uint check_if_opened)
{
  if (db_len == 5 &&
      !(lower_case_table_names ?
        my_strcasecmp(system_charset_info, db, "mysql") :
        strcmp(db, "mysql")))
  {
    if (table_name_len == 11 && !(lower_case_table_names ?
                                  my_strcasecmp(system_charset_info,
                                                table_name, "general_log") :
                                  strcmp(table_name, "general_log")))
    {
      if (!check_if_opened || logger.is_log_table_enabled(QUERY_LOG_GENERAL))
        return QUERY_LOG_GENERAL;
      return 0;
    }

    if (table_name_len == 8 && !(lower_case_table_names ?
      my_strcasecmp(system_charset_info, table_name, "slow_log") :
      strcmp(table_name, "slow_log")))
    {
      if (!check_if_opened || logger.is_log_table_enabled(QUERY_LOG_SLOW))
        return QUERY_LOG_SLOW;
      return 0;
    }
  }
  return 0;
}


Log_to_csv_event_handler::Log_to_csv_event_handler()
{
}


Log_to_csv_event_handler::~Log_to_csv_event_handler()
{
}


void Log_to_csv_event_handler::cleanup()
{
  logger.is_log_tables_initialized= FALSE;
}

/* log event handlers */

/**
  Log command to the general log table

  Log given command to the general log table.

  @param  event_time        command start timestamp
  @param  user_host         the pointer to the string with user@host info
  @param  user_host_len     length of the user_host string. this is computed
                            once and passed to all general log event handlers
  @param  thread_id         Id of the thread, issued a query
  @param  command_type      the type of the command being logged
  @param  command_type_len  the length of the string above
  @param  sql_text          the very text of the query being executed
  @param  sql_text_len      the length of sql_text string


  @return This function attempts to never call my_error(). This is
  necessary, because general logging happens already after a statement
  status has been sent to the client, so the client can not see the
  error anyway. Besides, the error is not related to the statement
  being executed and is internal, and thus should be handled
  internally (@todo: how?).
  If a write to the table has failed, the function attempts to
  write to a short error message to the file. The failure is also
  indicated in the return value. 

  @retval  FALSE   OK
  @retval  TRUE    error occured
*/

bool Log_to_csv_event_handler::
  log_general(THD *thd, time_t event_time, const char *user_host,
              uint user_host_len, int thread_id,
              const char *command_type, uint command_type_len,
              const char *sql_text, uint sql_text_len,
              CHARSET_INFO *client_cs)
{
  TABLE_LIST table_list;
  TABLE *table;
  bool result= TRUE;
  bool need_close= FALSE;
  bool need_pop= FALSE;
  bool need_rnd_end= FALSE;
  uint field_index;
  Silence_log_table_errors error_handler;
  Open_tables_state open_tables_backup;
  ulonglong save_thd_options;
  bool save_time_zone_used;

  /*
    CSV uses TIME_to_timestamp() internally if table needs to be repaired
    which will set thd->time_zone_used
  */
  save_time_zone_used= thd->time_zone_used;

  save_thd_options= thd->options;
  thd->options&= ~OPTION_BIN_LOG;

  bzero(& table_list, sizeof(TABLE_LIST));
  table_list.alias= table_list.table_name= GENERAL_LOG_NAME.str;
  table_list.table_name_length= GENERAL_LOG_NAME.length;

  table_list.lock_type= TL_WRITE_CONCURRENT_INSERT;

  table_list.db= MYSQL_SCHEMA_NAME.str;
  table_list.db_length= MYSQL_SCHEMA_NAME.length;

  /*
    1) open_performance_schema_table generates an error of the
    table can not be opened or is corrupted.
    2) "INSERT INTO general_log" can generate warning sometimes.

    Suppress these warnings and errors, they can't be dealt with
    properly anyway.

    QQ: this problem needs to be studied in more detail.
    Comment this 2 lines and run "cast.test" to see what's happening.
  */
  thd->push_internal_handler(& error_handler);
  need_pop= TRUE;

  if (!(table= open_performance_schema_table(thd, & table_list,
                                             & open_tables_backup)))
    goto err;

  need_close= TRUE;

  if (table->file->extra(HA_EXTRA_MARK_AS_LOG_TABLE) ||
      table->file->ha_rnd_init(0))
    goto err;

  need_rnd_end= TRUE;

  /* Honor next number columns if present */
  table->next_number_field= table->found_next_number_field;

  /*
    NOTE: we do not call restore_record() here, as all fields are
    filled by the Logger (=> no need to load default ones).
  */

  /*
    We do not set a value for table->field[0], as it will use
    default value (which is CURRENT_TIMESTAMP).
  */

  /* check that all columns exist */
  if (table->s->fields < 6)
    goto err;

  DBUG_ASSERT(table->field[0]->type() == MYSQL_TYPE_TIMESTAMP);

  ((Field_timestamp*) table->field[0])->store_timestamp((my_time_t)
                                                        event_time);

  /* do a write */
  if (table->field[1]->store(user_host, user_host_len, client_cs) ||
      table->field[2]->store((longlong) thread_id, TRUE) ||
      table->field[3]->store((longlong) server_id, TRUE) ||
      table->field[4]->store(command_type, command_type_len, client_cs))
    goto err;

  /*
    A positive return value in store() means truncation.
    Still logging a message in the log in this case.
  */
  table->field[5]->flags|= FIELDFLAG_HEX_ESCAPE;
  if (table->field[5]->store(sql_text, sql_text_len, client_cs) < 0)
    goto err;

  /* mark all fields as not null */
  table->field[1]->set_notnull();
  table->field[2]->set_notnull();
  table->field[3]->set_notnull();
  table->field[4]->set_notnull();
  table->field[5]->set_notnull();

  /* Set any extra columns to their default values */
  for (field_index= 6 ; field_index < table->s->fields ; field_index++)
  {
    table->field[field_index]->set_default();
  }

  /* log table entries are not replicated */
  if (table->file->ha_write_row(table->record[0]))
    goto err;

  result= FALSE;

err:
  if (result && !thd->killed)
    sql_print_error("Failed to write to mysql.general_log: %s",
                    error_handler.message());

  if (need_rnd_end)
  {
    table->file->ha_rnd_end();
    table->file->ha_release_auto_increment();
  }
  if (need_pop)
    thd->pop_internal_handler();
  if (need_close)
    close_performance_schema_table(thd, & open_tables_backup);

  thd->options= save_thd_options;
  thd->time_zone_used= save_time_zone_used;
  return result;
}


/*
  Log a query to the slow log table

  SYNOPSIS
    log_slow()
    thd               THD of the query
    current_time      current timestamp
    query_start_arg   command start timestamp
    user_host         the pointer to the string with user@host info
    user_host_len     length of the user_host string. this is computed once
                      and passed to all general log event handlers
    query_time        Amount of time the query took to execute (in microseconds)
    lock_time         Amount of time the query was locked (in microseconds)
    is_command        The flag, which determines, whether the sql_text is a
                      query or an administrator command (these are treated
                      differently by the old logging routines)
    sql_text          the very text of the query or administrator command
                      processed
    sql_text_len      the length of sql_text string

  DESCRIPTION

   Log a query to the slow log table

  RETURN
    FALSE - OK
    TRUE - error occured
*/

bool Log_to_csv_event_handler::
  log_slow(THD *thd, time_t current_time, time_t query_start_arg,
           const char *user_host, uint user_host_len,
           ulonglong query_utime, ulonglong lock_utime, bool is_command,
           const char *sql_text, uint sql_text_len,
           struct system_status_var *query_start_status)
{
  TABLE_LIST table_list;
  TABLE *table;
  bool result= TRUE;
  bool need_close= FALSE;
  bool need_rnd_end= FALSE;
  Silence_log_table_errors error_handler;
  Open_tables_state open_tables_backup;
  CHARSET_INFO *client_cs= thd->variables.character_set_client;
  bool save_time_zone_used;
  DBUG_ENTER("Log_to_csv_event_handler::log_slow");

  thd->push_internal_handler(& error_handler);
  /*
    CSV uses TIME_to_timestamp() internally if table needs to be repaired
    which will set thd->time_zone_used
  */
  save_time_zone_used= thd->time_zone_used;

  bzero(& table_list, sizeof(TABLE_LIST));
  table_list.alias= table_list.table_name= SLOW_LOG_NAME.str;
  table_list.table_name_length= SLOW_LOG_NAME.length;

  table_list.lock_type= TL_WRITE_CONCURRENT_INSERT;

  table_list.db= MYSQL_SCHEMA_NAME.str;
  table_list.db_length= MYSQL_SCHEMA_NAME.length;

  if (!(table= open_performance_schema_table(thd, & table_list,
                                             & open_tables_backup)))
    goto err;

  need_close= TRUE;

  if (table->file->extra(HA_EXTRA_MARK_AS_LOG_TABLE) ||
      table->file->ha_rnd_init(0))
    goto err;

  need_rnd_end= TRUE;

  /* Honor next number columns if present */
  table->next_number_field= table->found_next_number_field;

  restore_record(table, s->default_values);    // Get empty record

  /* check that all columns exist */
  if (table->s->fields < 11)
    goto err;

  /* store the time and user values */
  DBUG_ASSERT(table->field[0]->type() == MYSQL_TYPE_TIMESTAMP);
  ((Field_timestamp*) table->field[0])->store_timestamp((my_time_t)
                                                        current_time);
  if (table->field[1]->store(user_host, user_host_len, client_cs))
    goto err;

  if (query_start_arg)
  {
    longlong query_time= (longlong) (query_utime/1000000);
    longlong lock_time=  (longlong) (lock_utime/1000000);
    /*
      A TIME field can not hold the full longlong range; query_time or
      lock_time may be truncated without warning here, if greater than
      839 hours (~35 days)
    */
    MYSQL_TIME t;
    t.neg= 0;

    /* fill in query_time field */
    calc_time_from_sec(&t, (long) min(query_time, (longlong) TIME_MAX_VALUE_SECONDS), 0);
    if (table->field[2]->store_time(&t, MYSQL_TIMESTAMP_TIME))
      goto err;
    /* lock_time */
    calc_time_from_sec(&t, (long) min(lock_time, (longlong) TIME_MAX_VALUE_SECONDS), 0);
    if (table->field[3]->store_time(&t, MYSQL_TIMESTAMP_TIME))
      goto err;
    /* rows_sent */
    if (table->field[4]->store((longlong) thd->sent_row_count, TRUE))
      goto err;
    /* rows_examined */
    if (table->field[5]->store((longlong) thd->examined_row_count, TRUE))
      goto err;
  }
  else
  {
    table->field[2]->set_null();
    table->field[3]->set_null();
    table->field[4]->set_null();
    table->field[5]->set_null();
  }
  /* fill database field */
  if (thd->db)
  {
    if (table->field[6]->store(thd->db, thd->db_length, client_cs))
      goto err;
    table->field[6]->set_notnull();
  }

  if (thd->stmt_depends_on_first_successful_insert_id_in_prev_stmt)
  {
    if (table->
        field[7]->store((longlong)
                        thd->first_successful_insert_id_in_prev_stmt_for_binlog,
                        TRUE))
      goto err;
    table->field[7]->set_notnull();
  }

  /*
    Set value if we do an insert on autoincrement column. Note that for
    some engines (those for which get_auto_increment() does not leave a
    table lock until the statement ends), this is just the first value and
    the next ones used may not be contiguous to it.
  */
  if (thd->auto_inc_intervals_in_cur_stmt_for_binlog.nb_elements() > 0)
  {
    if (table->
        field[8]->store((longlong)
          thd->auto_inc_intervals_in_cur_stmt_for_binlog.minimum(), TRUE))
      goto err;
    table->field[8]->set_notnull();
  }

  if (table->field[9]->store((longlong) server_id, TRUE))
    goto err;
  table->field[9]->set_notnull();

  /*
    Column sql_text.
    A positive return value in store() means truncation.
    Still logging a message in the log in this case.
  */
  if (table->field[10]->store(sql_text, sql_text_len, client_cs) < 0)
    goto err;

  /* log table entries are not replicated */
  if (table->file->ha_write_row(table->record[0]))
    goto err;

  result= FALSE;

err:
  thd->pop_internal_handler();

  if (result && !thd->killed)
    sql_print_error("Failed to write to mysql.slow_log: %s",
                    error_handler.message());

  if (need_rnd_end)
  {
    table->file->ha_rnd_end();
    table->file->ha_release_auto_increment();
  }
  if (need_close)
    close_performance_schema_table(thd, & open_tables_backup);
  thd->time_zone_used= save_time_zone_used;
  DBUG_RETURN(result);
}

int Log_to_csv_event_handler::
  activate_log(THD *thd, uint log_table_type)
{
  TABLE_LIST table_list;
  TABLE *table;
  int result;
  Open_tables_state open_tables_backup;

  DBUG_ENTER("Log_to_csv_event_handler::activate_log");

  bzero(& table_list, sizeof(TABLE_LIST));

  if (log_table_type == QUERY_LOG_GENERAL)
  {
    table_list.alias= table_list.table_name= GENERAL_LOG_NAME.str;
    table_list.table_name_length= GENERAL_LOG_NAME.length;
  }
  else
  {
    DBUG_ASSERT(log_table_type == QUERY_LOG_SLOW);
    table_list.alias= table_list.table_name= SLOW_LOG_NAME.str;
    table_list.table_name_length= SLOW_LOG_NAME.length;
  }

  table_list.lock_type= TL_WRITE_CONCURRENT_INSERT;

  table_list.db= MYSQL_SCHEMA_NAME.str;
  table_list.db_length= MYSQL_SCHEMA_NAME.length;

  table= open_performance_schema_table(thd, & table_list,
                                       & open_tables_backup);
  if (table)
  {
    result= 0;
    close_performance_schema_table(thd, & open_tables_backup);
  }
  else
    result= 1;

  DBUG_RETURN(result);
}

bool Log_to_csv_event_handler::
  log_error(enum loglevel level, const char *format, va_list args)
{
  /* No log table is implemented */
  DBUG_ASSERT(0);
  return FALSE;
}

bool Log_to_file_event_handler::
  log_error(enum loglevel level, const char *format,
            va_list args)
{
  return vprint_msg_to_log(level, format, args);
}

void Log_to_file_event_handler::init_pthread_objects()
{
  mysql_log.init_pthread_objects();
  mysql_slow_log.init_pthread_objects();
}


/** Wrapper around MYSQL_LOG::write() for slow log. */

bool Log_to_file_event_handler::
  log_slow(THD *thd, time_t current_time, time_t query_start_arg,
           const char *user_host, uint user_host_len,
           ulonglong query_utime, ulonglong lock_utime, bool is_command,
           const char *sql_text, uint sql_text_len,
           struct system_status_var *query_start_status)
{
  Silence_log_table_errors error_handler;
  thd->push_internal_handler(&error_handler);
  bool retval= mysql_slow_log.write(thd, current_time, query_start_arg,
                                    user_host, user_host_len,
                                    query_utime, lock_utime, is_command,
                                    sql_text, sql_text_len,
                                    query_start_status);
  thd->pop_internal_handler();
  return retval;
}


/**
   Wrapper around MYSQL_LOG::write() for general log. We need it since we
   want all log event handlers to have the same signature.
*/

bool Log_to_file_event_handler::
  log_general(THD *thd, time_t event_time, const char *user_host,
              uint user_host_len, int thread_id,
              const char *command_type, uint command_type_len,
              const char *sql_text, uint sql_text_len,
              CHARSET_INFO *client_cs)
{
  Silence_log_table_errors error_handler;
  thd->push_internal_handler(&error_handler);
  bool retval= mysql_log.write(event_time, user_host, user_host_len,
                               thread_id, command_type, command_type_len,
                               sql_text, sql_text_len);
  thd->pop_internal_handler();
  return retval;
}


bool Log_to_file_event_handler::init()
{
  if (!is_initialized)
  {
    if (opt_slow_log)
      mysql_slow_log.open_slow_log(sys_var_slow_log_path.value);

    if (opt_log)
      mysql_log.open_query_log(sys_var_general_log_path.value);

    is_initialized= TRUE;
  }

  return FALSE;
}


void Log_to_file_event_handler::cleanup()
{
  mysql_log.cleanup();
  mysql_slow_log.cleanup();
}

void Log_to_file_event_handler::flush()
{
  /* reopen log files */
  if (opt_log)
    mysql_log.reopen_file();
  if (opt_slow_log)
    mysql_slow_log.reopen_file();
}

/*
  Log error with all enabled log event handlers

  SYNOPSIS
    error_log_print()

    level             The level of the error significance: NOTE,
                      WARNING or ERROR.
    format            format string for the error message
    args              list of arguments for the format string

  RETURN
    FALSE - OK
    TRUE - error occured
*/

bool LOGGER::error_log_print(enum loglevel level, const char *format,
                             va_list args)
{
  bool error= FALSE;
  Log_event_handler **current_handler;

  /* currently we don't need locking here as there is no error_log table */
  for (current_handler= error_log_handler_list ; *current_handler ;)
    error= (*current_handler++)->log_error(level, format, args) || error;

  return error;
}


void LOGGER::cleanup_base()
{
  DBUG_ASSERT(inited == 1);
  rwlock_destroy(&LOCK_logger);
  if (table_log_handler)
  {
    table_log_handler->cleanup();
    delete table_log_handler;
    table_log_handler= NULL;
  }
  if (file_log_handler)
    file_log_handler->cleanup();
}


void LOGGER::cleanup_end()
{
  DBUG_ASSERT(inited == 1);
  if (file_log_handler)
  {
    delete file_log_handler;
    file_log_handler=NULL;
  }
  inited= 0;
}


/**
  Perform basic log initialization: create file-based log handler and
  init error log.
*/
void LOGGER::init_base()
{
  DBUG_ASSERT(inited == 0);
  inited= 1;

  /*
    Here we create file log handler. We don't do it for the table log handler
    here as it cannot be created so early. The reason is THD initialization,
    which depends on the system variables (parsed later).
  */
  if (!file_log_handler)
    file_log_handler= new Log_to_file_event_handler;

  /* by default we use traditional error log */
  init_error_log(LOG_FILE);

  file_log_handler->init_pthread_objects();
  my_rwlock_init(&LOCK_logger, NULL);
}


void LOGGER::init_log_tables()
{
  if (!table_log_handler)
    table_log_handler= new Log_to_csv_event_handler;

  if (!is_log_tables_initialized &&
      !table_log_handler->init() && !file_log_handler->init())
    is_log_tables_initialized= TRUE;
}


bool LOGGER::flush_logs(THD *thd)
{
  int rc= 0;

  /*
    Now we lock logger, as nobody should be able to use logging routines while
    log tables are closed
  */
  logger.lock_exclusive();

  /* reopen log files */
  file_log_handler->flush();

  /* end of log flush */
  logger.unlock();
  return rc;
}


/*
  Log slow query with all enabled log event handlers

  SYNOPSIS
    slow_log_print()

    thd                 THD of the query being logged
    query               The query being logged
    query_length        The length of the query string
    current_utime       Current time in microseconds (from undefined start)

  RETURN
    FALSE   OK
    TRUE    error occured
*/

bool LOGGER::slow_log_print(THD *thd, const char *query, uint query_length,
                            ulonglong current_utime,
                            struct system_status_var *query_start_status)

{
  bool error= FALSE;
  Log_event_handler **current_handler;
  bool is_command= FALSE;
  char user_host_buff[MAX_USER_HOST_SIZE + 1];
  Security_context *sctx= thd->security_ctx;
  uint user_host_len= 0;
  ulonglong query_utime, lock_utime;

  DBUG_ASSERT(thd->enable_slow_log);
  /*
    Print the message to the buffer if we have slow log enabled
  */

  if (*slow_log_handler_list)
  {
    time_t current_time;

    /* do not log slow queries from replication threads */
    if (thd->slave_thread && !opt_log_slow_slave_statements)
      return 0;

    lock_shared();
    if (!opt_slow_log)
    {
      unlock();
      return 0;
    }

    /* fill in user_host value: the format is "%s[%s] @ %s [%s]" */
    user_host_len= (strxnmov(user_host_buff, MAX_USER_HOST_SIZE,
                             sctx->priv_user ? sctx->priv_user : "", "[",
                             sctx->user ? sctx->user : "", "] @ ",
                             sctx->host ? sctx->host : "", " [",
                             sctx->ip ? sctx->ip : "", "]", NullS) -
                    user_host_buff);

    current_time= my_time_possible_from_micro(current_utime);
    if (thd->start_utime)
    {
      query_utime= (current_utime - thd->start_utime);
      lock_utime=  (thd->utime_after_lock - thd->start_utime);
    }
    else
    {
      query_utime= lock_utime= 0;
    }

    if (!query)
    {
      is_command= TRUE;
      query= command_name[thd->command].str;
      query_length= command_name[thd->command].length;
    }

    for (current_handler= slow_log_handler_list; *current_handler ;)
      error= (*current_handler++)->log_slow(thd, current_time, thd->start_time,
                                            user_host_buff, user_host_len,
                                            query_utime, lock_utime, is_command,
                                            query, query_length,
                                            query_start_status) || error;

    unlock();
  }
  return error;
}

bool LOGGER::general_log_write(THD *thd, enum enum_server_command command,
                               const char *query, uint query_length)
{
  bool error= FALSE;
  Log_event_handler **current_handler= general_log_handler_list;
  char user_host_buff[MAX_USER_HOST_SIZE + 1];
  Security_context *sctx= thd->security_ctx;
  uint user_host_len= 0;
  time_t current_time;

  DBUG_ASSERT(thd);

  lock_shared();
  if (!opt_log)
  {
    unlock();
    return 0;
  }
  user_host_len= strxnmov(user_host_buff, MAX_USER_HOST_SIZE,
                          sctx->priv_user ? sctx->priv_user : "", "[",
                          sctx->user ? sctx->user : "", "] @ ",
                          sctx->host ? sctx->host : "", " [",
                          sctx->ip ? sctx->ip : "", "]", NullS) -
                                                          user_host_buff;

  current_time= my_time(0);
  while (*current_handler)
    error|= (*current_handler++)->
      log_general(thd, current_time, user_host_buff,
                  user_host_len, thd->thread_id,
                  command_name[(uint) command].str,
                  command_name[(uint) command].length,
                  query, query_length,
                  thd->variables.character_set_client) || error;
  unlock();

  return error;
}

bool LOGGER::general_log_print(THD *thd, enum enum_server_command command,
                               const char *format, va_list args)
{
  uint message_buff_len= 0;
  char message_buff[MAX_LOG_BUFFER_SIZE];

  /* prepare message */
  if (format)
    message_buff_len= my_vsnprintf(message_buff, sizeof(message_buff),
                                   format, args);
  else
    message_buff[0]= '\0';

  return general_log_write(thd, command, message_buff, message_buff_len);
}

void LOGGER::init_error_log(uint error_log_printer)
{
  if (error_log_printer & LOG_NONE)
  {
    error_log_handler_list[0]= 0;
    return;
  }

  switch (error_log_printer) {
  case LOG_FILE:
    error_log_handler_list[0]= file_log_handler;
    error_log_handler_list[1]= 0;
    break;
    /* these two are disabled for now */
  case LOG_TABLE:
    DBUG_ASSERT(0);
    break;
  case LOG_TABLE|LOG_FILE:
    DBUG_ASSERT(0);
    break;
  }
}

void LOGGER::init_slow_log(uint slow_log_printer)
{
  if (slow_log_printer & LOG_NONE)
  {
    slow_log_handler_list[0]= 0;
    return;
  }

  switch (slow_log_printer) {
  case LOG_FILE:
    slow_log_handler_list[0]= file_log_handler;
    slow_log_handler_list[1]= 0;
    break;
  case LOG_TABLE:
    slow_log_handler_list[0]= table_log_handler;
    slow_log_handler_list[1]= 0;
    break;
  case LOG_TABLE|LOG_FILE:
    slow_log_handler_list[0]= file_log_handler;
    slow_log_handler_list[1]= table_log_handler;
    slow_log_handler_list[2]= 0;
    break;
  }
}

void LOGGER::init_general_log(uint general_log_printer)
{
  if (general_log_printer & LOG_NONE)
  {
    general_log_handler_list[0]= 0;
    return;
  }

  switch (general_log_printer) {
  case LOG_FILE:
    general_log_handler_list[0]= file_log_handler;
    general_log_handler_list[1]= 0;
    break;
  case LOG_TABLE:
    general_log_handler_list[0]= table_log_handler;
    general_log_handler_list[1]= 0;
    break;
  case LOG_TABLE|LOG_FILE:
    general_log_handler_list[0]= file_log_handler;
    general_log_handler_list[1]= table_log_handler;
    general_log_handler_list[2]= 0;
    break;
  }
}


bool LOGGER::activate_log_handler(THD* thd, uint log_type)
{
  MYSQL_QUERY_LOG *file_log;
  bool res= FALSE;
  lock_exclusive();
  switch (log_type) {
  case QUERY_LOG_SLOW:
    if (!opt_slow_log)
    {
      file_log= file_log_handler->get_mysql_slow_log();

      file_log->open_slow_log(sys_var_slow_log_path.value);
      if (table_log_handler->activate_log(thd, QUERY_LOG_SLOW))
      {
        /* Error printed by open table in activate_log() */
        res= TRUE;
        file_log->close(0);
      }
      else
      {
        init_slow_log(log_output_options);
        opt_slow_log= TRUE;
      }
    }
    break;
  case QUERY_LOG_GENERAL:
    if (!opt_log)
    {
      file_log= file_log_handler->get_mysql_log();

      file_log->open_query_log(sys_var_general_log_path.value);
      if (table_log_handler->activate_log(thd, QUERY_LOG_GENERAL))
      {
        /* Error printed by open table in activate_log() */
        res= TRUE;
        file_log->close(0);
      }
      else
      {
        init_general_log(log_output_options);
        opt_log= TRUE;
      }
    }
    break;
  default:
    DBUG_ASSERT(0);
  }
  unlock();
  return res;
}


void LOGGER::deactivate_log_handler(THD *thd, uint log_type)
{
  my_bool *tmp_opt= 0;
  MYSQL_LOG *file_log;

  switch (log_type) {
  case QUERY_LOG_SLOW:
    tmp_opt= &opt_slow_log;
    file_log= file_log_handler->get_mysql_slow_log();
    break;
  case QUERY_LOG_GENERAL:
    tmp_opt= &opt_log;
    file_log= file_log_handler->get_mysql_log();
    break;
  default:
    MY_ASSERT_UNREACHABLE();
  }

  if (!(*tmp_opt))
    return;

  lock_exclusive();
  file_log->close(0);
  *tmp_opt= FALSE;
  unlock();
}


/* the parameters are unused for the log tables */
bool Log_to_csv_event_handler::init()
{
  return 0;
}

int LOGGER::set_handlers(uint error_log_printer,
                         uint slow_log_printer,
                         uint general_log_printer)
{
  /* error log table is not supported yet */
  DBUG_ASSERT(error_log_printer < LOG_TABLE);

  lock_exclusive();

  if ((slow_log_printer & LOG_TABLE || general_log_printer & LOG_TABLE) &&
      !is_log_tables_initialized)
  {
    slow_log_printer= (slow_log_printer & ~LOG_TABLE) | LOG_FILE;
    general_log_printer= (general_log_printer & ~LOG_TABLE) | LOG_FILE;

    sql_print_error("Failed to initialize log tables. "
                    "Falling back to the old-fashioned logs");
  }

  init_error_log(error_log_printer);
  init_slow_log(slow_log_printer);
  init_general_log(general_log_printer);

  unlock();

  return 0;
}

/** 
    This function checks if a transactional talbe was updated by the
    current statement.

    @param thd The client thread that executed the current statement.
    @return
      @c true if a transactional table was updated, @false otherwise.
*/
static bool stmt_has_updated_trans_table(THD *thd)
{
  Ha_trx_info *ha_info;

  for (ha_info= thd->transaction.stmt.ha_list; ha_info && ha_info->is_started(); ha_info= ha_info->next())
  {
    if (ha_info->is_trx_read_write() && ha_info->ht() != binlog_hton)
      return (TRUE);
  }
  return (FALSE);
}

 /*
  Save position of binary log transaction cache.

  SYNPOSIS
    binlog_trans_log_savepos()

    thd      The thread to take the binlog data from
    pos      Pointer to variable where the position will be stored

  DESCRIPTION

    Save the current position in the binary log transaction cache into
    the variable pointed to by 'pos'
 */

static void
binlog_trans_log_savepos(THD *thd, my_off_t *pos)
{
  DBUG_ENTER("binlog_trans_log_savepos");
  DBUG_ASSERT(pos != NULL);
  if (thd_get_ha_data(thd, binlog_hton) == NULL)
    thd->binlog_setup_trx_data();
  binlog_trx_data *const trx_data=
    (binlog_trx_data*) thd_get_ha_data(thd, binlog_hton);
  DBUG_ASSERT(mysql_bin_log.is_open());
  *pos= trx_data->position();
  DBUG_PRINT("return", ("*pos: %lu", (ulong) *pos));
  DBUG_VOID_RETURN;
}


/*
  Truncate the binary log transaction cache.

  SYNPOSIS
    binlog_trans_log_truncate()

    thd      The thread to take the binlog data from
    pos      Position to truncate to

  DESCRIPTION

    Truncate the binary log to the given position. Will not change
    anything else.

 */
static void
binlog_trans_log_truncate(THD *thd, my_off_t pos)
{
  DBUG_ENTER("binlog_trans_log_truncate");
  DBUG_PRINT("enter", ("pos: %lu", (ulong) pos));

  DBUG_ASSERT(thd_get_ha_data(thd, binlog_hton) != NULL);
  /* Only true if binlog_trans_log_savepos() wasn't called before */
  DBUG_ASSERT(pos != ~(my_off_t) 0);

  binlog_trx_data *const trx_data=
    (binlog_trx_data*) thd_get_ha_data(thd, binlog_hton);
  trx_data->truncate(pos);
  DBUG_VOID_RETURN;
}


/*
  this function is mostly a placeholder.
  conceptually, binlog initialization (now mostly done in MYSQL_BIN_LOG::open)
  should be moved here.
*/

int binlog_init(void *p)
{
  binlog_hton= (handlerton *)p;
  binlog_hton->state=opt_bin_log ? SHOW_OPTION_YES : SHOW_OPTION_NO;
  binlog_hton->db_type=DB_TYPE_BINLOG;
  binlog_hton->savepoint_offset= sizeof(my_off_t);
  binlog_hton->close_connection= binlog_close_connection;
  binlog_hton->savepoint_set= binlog_savepoint_set;
  binlog_hton->savepoint_rollback= binlog_savepoint_rollback;
  binlog_hton->commit= binlog_commit;
  binlog_hton->rollback= binlog_rollback;
  binlog_hton->prepare= binlog_prepare;
  binlog_hton->flags= HTON_NOT_USER_SELECTABLE | HTON_HIDDEN;
  return 0;
}

static int binlog_close_connection(handlerton *hton, THD *thd)
{
  binlog_trx_data *const trx_data=
    (binlog_trx_data*) thd_get_ha_data(thd, binlog_hton);
  DBUG_ASSERT(trx_data->empty());
  thd_set_ha_data(thd, binlog_hton, NULL);
  trx_data->~binlog_trx_data();
  my_free((uchar*)trx_data, MYF(0));
  return 0;
}

/*
  End a transaction.

  SYNOPSIS
    binlog_end_trans()

    thd      The thread whose transaction should be ended
    trx_data Pointer to the transaction data to use
    end_ev   The end event to use, or NULL
    all      True if the entire transaction should be ended, false if
             only the statement transaction should be ended.
    ht       handlerton for transaction used for group commit optimization
    pending  count of transactions in ha_commit_trans including the caller
    log_was_full set to TRUE if binlog was full on write done by this trx

  DESCRIPTION

    End the currently open transaction. The transaction can be either
    a real transaction (if 'all' is true) or a statement transaction
    (if 'all' is false).

    If 'end_ev' is NULL, the transaction is a rollback of only
    transactional tables, so the transaction cache will be truncated
    to either just before the last opened statement transaction (if
    'all' is false), or reset completely (if 'all' is true).
 */
static int
binlog_end_trans(THD *thd, binlog_trx_data *trx_data,
                 Log_event *end_ev, bool all, bool async, handlerton *ht,
                 int32 pending, bool *log_was_full)
{
  DBUG_ENTER("binlog_end_trans");
  int error=0;
  IO_CACHE *trans_log= &trx_data->trans_log;
  DBUG_PRINT("enter", ("transaction: %s  end_ev: 0x%lx",
                       all ? "all" : "stmt", (long) end_ev));
  DBUG_PRINT("info", ("thd->options={ %s%s}",
                      FLAGSTR(thd->options, OPTION_NOT_AUTOCOMMIT),
                      FLAGSTR(thd->options, OPTION_BEGIN)));

  /*
    NULL denotes ROLLBACK with nothing to replicate: i.e., rollback of
    only transactional tables.  If the transaction contain changes to
    any non-transactiona tables, we need write the transaction and log
    a ROLLBACK last.
  */
  if (end_ev != NULL)
  {
    if (thd->binlog_flush_pending_rows_event(TRUE))
      DBUG_RETURN(1);

    DBUG_EXECUTE_IF("error_in_binlog_end_trans", DBUG_RETURN(1); );

    /*
      Doing a commit or a rollback including non-transactional tables,
      i.e., ending a transaction where we might write the transaction
      cache to the binary log.

      We can always end the statement when ending a transaction since
      transactions are not allowed inside stored functions.  If they
      were, we would have to ensure that we're not ending a statement
      inside a stored function.
     */
    error= mysql_bin_log.write(thd, &trx_data->trans_log, end_ev,
                               trx_data->has_incident(), async, ht, pending,
                               log_was_full);
    trx_data->reset();

    statistic_increment(binlog_cache_use, &LOCK_status);
    if (trans_log->disk_writes != 0)
    {
      statistic_increment(binlog_cache_disk_use, &LOCK_status);
      trans_log->disk_writes= 0;
    }
  }
  else
  {
    /*
      If rolling back an entire transaction or a single statement not
      inside a transaction, we reset the transaction cache.

      If rolling back a statement in a transaction, we truncate the
      transaction cache to remove the statement.
     */
    thd->binlog_remove_pending_rows_event(TRUE);
    if (all || !(thd->options & (OPTION_BEGIN | OPTION_NOT_AUTOCOMMIT)))
    {
      if (trx_data->has_incident())
        error= mysql_bin_log.write_incident(thd, TRUE, log_was_full);
      trx_data->reset();
    }
    else                                        // ...statement
      trx_data->truncate(trx_data->before_stmt_pos);
  }

  DBUG_ASSERT(thd->binlog_get_pending_rows_event() == NULL);
  DBUG_RETURN(error);
}

static int binlog_prepare(handlerton *hton, THD *thd, bool all, bool async)
{
  /*
    do nothing.
    just pretend we can do 2pc, so that MySQL won't
    switch to 1pc.
    real work will be done in MYSQL_BIN_LOG::log_xid()
  */
  return 0;
}

/**
  This function is called once after each statement.

  It has the responsibility to flush the transaction cache to the
  binlog file on commits.

  @param hton  The binlog handlerton.
  @param thd   The client thread that executes the transaction.
  @param all   This is @c true if this is a real transaction commit, and
               @false otherwise.

  @see handlerton::commit
*/
static int binlog_commit(handlerton *hton, THD *thd, bool all, bool async)
{
  int error= 0;
  DBUG_ENTER("binlog_commit");
  binlog_trx_data *const trx_data=
    (binlog_trx_data*) thd_get_ha_data(thd, binlog_hton);

  if (trx_data->empty())
  {
    // we're here because trans_log was flushed in MYSQL_BIN_LOG::log_xid()
    trx_data->reset();
    DBUG_RETURN(0);
  }

  /*
    We flush the cache if:

     - we are committing a transaction or;
     - no statement was committed before and just non-transactional
       tables were updated.

    Otherwise, we collect the changes.
  */
  DBUG_PRINT("debug",
             ("all: %d, empty: %s, all.modified_non_trans_table: %s, stmt.modified_non_trans_table: %s",
              all,
              YESNO(trx_data->empty()),
              YESNO(thd->transaction.all.modified_non_trans_table),
              YESNO(thd->transaction.stmt.modified_non_trans_table)));
  if (ending_trans(thd, all) ||
      (trans_has_no_stmt_committed(thd, all) &&
       !stmt_has_updated_trans_table(thd) && stmt_has_updated_non_trans_table(thd)))
  {
    Query_log_event qev(thd, STRING_WITH_LEN("COMMIT"), TRUE, TRUE, 0);
    error= binlog_end_trans(thd, trx_data, &qev, all, FALSE, NULL, 0, NULL);
  }

  trx_data->at_least_one_stmt_committed = my_b_tell(&trx_data->trans_log) > 0;

  if (!all)
    trx_data->before_stmt_pos = MY_OFF_T_UNDEF; // part of the stmt commit
  DBUG_RETURN(error);
}

/**
  This function is called when a transaction involving a transactional
  table is rolled back.

  It has the responsibility to flush the transaction cache to the
  binlog file. However, if the transaction does not involve
  non-transactional tables, nothing needs to be logged.

  @param hton  The binlog handlerton.
  @param thd   The client thread that executes the transaction.
  @param all   This is @c true if this is a real transaction rollback, and
               @false otherwise.

  @see handlerton::rollback
*/
static int binlog_rollback(handlerton *hton, THD *thd, bool all)
{
  DBUG_ENTER("binlog_rollback");
  int error=0;
  binlog_trx_data *const trx_data=
    (binlog_trx_data*) thd_get_ha_data(thd, binlog_hton);

  if (trx_data->empty()) {
    trx_data->reset();
    DBUG_RETURN(0);
  }

  DBUG_PRINT("debug", ("all: %s, all.modified_non_trans_table: %s, stmt.modified_non_trans_table: %s",
                       YESNO(all),
                       YESNO(thd->transaction.all.modified_non_trans_table),
                       YESNO(thd->transaction.stmt.modified_non_trans_table)));
  if (mysql_bin_log.check_write_error(thd))
  {
    /*
      "all == true" means that a "rollback statement" triggered the error and
      this function was called. However, this must not happen as a rollback
      is written directly to the binary log. And in auto-commit mode, a single
      statement that is rolled back has the flag all == false.
    */
    DBUG_ASSERT(!all);
    /*
      We reach this point if either only transactional tables were modified or
      the effect of a statement that did not get into the binlog needs to be
      rolled back. In the latter case, if a statement changed non-transactional
      tables or had the OPTION_KEEP_LOG associated, we write an incident event
      to the binlog in order to stop slaves and notify users that some changes
      on the master did not get into the binlog and slaves will be inconsistent.
      On the other hand, if a statement is transactional, we just safely roll it
      back.
    */
    if ((stmt_has_updated_non_trans_table(thd) ||
        (thd->options & OPTION_KEEP_LOG)) &&
        mysql_bin_log.check_write_error(thd))
      trx_data->set_incident();
    error= binlog_end_trans(thd, trx_data, 0, all, FALSE, NULL, 0, NULL);
  }
  else
  {
   /*
      We flush the cache with a rollback, wrapped in a beging/rollback if:
        . aborting a transaction that modified a non-transactional table or
          the OPTION_KEEP_LOG is activate.
        . aborting a statement that modified both transactional and
          non-transactional tables but which is not in the boundaries of any
          transaction or there was no early change;
    */
    if ((ending_trans(thd, all) &&
        (trans_has_updated_non_trans_table(thd) ||
         (thd->options & OPTION_KEEP_LOG))) ||
        (trans_has_no_stmt_committed(thd, all) &&
         stmt_has_updated_non_trans_table(thd) &&
         thd->current_stmt_binlog_row_based))
    {
      Query_log_event qev(thd, STRING_WITH_LEN("ROLLBACK"), TRUE, TRUE, 0);
      error= binlog_end_trans(thd, trx_data, &qev, all, FALSE, NULL, 0, NULL);
    }
    /*
      Otherwise, we simply truncate the cache as there is no change on
      non-transactional tables as follows.
    */
    else if (ending_trans(thd, all) ||
             (!(thd->options & OPTION_KEEP_LOG) && !stmt_has_updated_non_trans_table(thd)))
      error= binlog_end_trans(thd, trx_data, 0, all, FALSE, NULL, 0, NULL);
  }
  if (!all)
    trx_data->before_stmt_pos = MY_OFF_T_UNDEF; // part of the stmt rollback
  DBUG_RETURN(error);
}

/**
  Cleanup the cache.

  @param thd   The client thread that wants to clean up the cache.
*/
void MYSQL_BIN_LOG::reset_gathered_updates(THD *thd)
{
  binlog_trx_data *const trx_data=
    (binlog_trx_data*) thd_get_ha_data(thd, binlog_hton);

  trx_data->reset();
}

void MYSQL_BIN_LOG::set_write_error(THD *thd)
{
  DBUG_ENTER("MYSQL_BIN_LOG::set_write_error");

  write_error= 1;

  if (check_write_error(thd))
    DBUG_VOID_RETURN;

  if (my_errno == EFBIG)
    my_message(ER_TRANS_CACHE_FULL, ER(ER_TRANS_CACHE_FULL), MYF(MY_WME));
  else
    my_error(ER_ERROR_ON_WRITE, MYF(MY_WME), name, errno);

  DBUG_VOID_RETURN;
}

bool MYSQL_BIN_LOG::check_write_error(THD *thd)
{
  DBUG_ENTER("MYSQL_BIN_LOG::check_write_error");

  bool checked= FALSE;

  if (!thd->is_error())
    DBUG_RETURN(checked);

  switch (thd->main_da.sql_errno())
  {
    case ER_TRANS_CACHE_FULL:
    case ER_ERROR_ON_WRITE:
    case ER_BINLOG_LOGGING_IMPOSSIBLE:
      checked= TRUE;
    break;
  }

  DBUG_RETURN(checked);
}

/**
  @note
  How do we handle this (unlikely but legal) case:
  @verbatim
    [transaction] + [update to non-trans table] + [rollback to savepoint] ?
  @endverbatim
  The problem occurs when a savepoint is before the update to the
  non-transactional table. Then when there's a rollback to the savepoint, if we
  simply truncate the binlog cache, we lose the part of the binlog cache where
  the update is. If we want to not lose it, we need to write the SAVEPOINT
  command and the ROLLBACK TO SAVEPOINT command to the binlog cache. The latter
  is easy: it's just write at the end of the binlog cache, but the former
  should be *inserted* to the place where the user called SAVEPOINT. The
  solution is that when the user calls SAVEPOINT, we write it to the binlog
  cache (so no need to later insert it). As transactions are never intermixed
  in the binary log (i.e. they are serialized), we won't have conflicts with
  savepoint names when using mysqlbinlog or in the slave SQL thread.
  Then when ROLLBACK TO SAVEPOINT is called, if we updated some
  non-transactional table, we don't truncate the binlog cache but instead write
  ROLLBACK TO SAVEPOINT to it; otherwise we truncate the binlog cache (which
  will chop the SAVEPOINT command from the binlog cache, which is good as in
  that case there is no need to have it in the binlog).
*/

static int binlog_savepoint_set(handlerton *hton, THD *thd, void *sv)
{
  DBUG_ENTER("binlog_savepoint_set");

  binlog_trans_log_savepos(thd, (my_off_t*) sv);
  /* Write it to the binary log */

  String log_query;
  if (log_query.append(STRING_WITH_LEN("SAVEPOINT ")) ||
      log_query.append("`") ||
      log_query.append(thd->lex->ident.str, thd->lex->ident.length) ||
      log_query.append("`"))
    DBUG_RETURN(1);
  int errcode= query_error_code(thd, thd->killed == THD::NOT_KILLED);
  Query_log_event qinfo(thd, log_query.c_ptr_safe(), log_query.length(),
                        TRUE, TRUE, errcode);
  DBUG_RETURN(mysql_bin_log.write(&qinfo));
}

static int binlog_savepoint_rollback(handlerton *hton, THD *thd, void *sv)
{
  DBUG_ENTER("binlog_savepoint_rollback");

  /*
    Write ROLLBACK TO SAVEPOINT to the binlog cache if we have updated some
    non-transactional table. Otherwise, truncate the binlog cache starting
    from the SAVEPOINT command.
  */
  if (unlikely(trans_has_updated_non_trans_table(thd) || 
               (thd->options & OPTION_KEEP_LOG)))
  {
    String log_query;
    if (log_query.append(STRING_WITH_LEN("ROLLBACK TO ")) ||
        log_query.append("`") ||
        log_query.append(thd->lex->ident.str, thd->lex->ident.length) ||
        log_query.append("`"))
      DBUG_RETURN(1);
    int errcode= query_error_code(thd, thd->killed == THD::NOT_KILLED);
    Query_log_event qinfo(thd, log_query.c_ptr_safe(), log_query.length(),
                          TRUE, TRUE, errcode);
    DBUG_RETURN(mysql_bin_log.write(&qinfo));
  }
  binlog_trans_log_truncate(thd, *(my_off_t*)sv);
  DBUG_RETURN(0);
}


int check_binlog_magic(IO_CACHE* log, const char** errmsg)
{
  char magic[4];
  DBUG_ASSERT(my_b_tell(log) == 0);

  if (my_b_read(log, (uchar*) magic, sizeof(magic)))
  {
    *errmsg = "I/O error reading the header from the binary log";
    sql_print_error("%s, errno=%d, io cache code=%d", *errmsg, my_errno,
		    log->error);
    return 1;
  }
  if (memcmp(magic, BINLOG_MAGIC, sizeof(magic)))
  {
    *errmsg = "Binlog has bad magic number;  It's not a binary log file that can be used by this version of MySQL";
    return 1;
  }
  return 0;
}


File open_binlog(IO_CACHE *log, const char *log_file_name, const char **errmsg)
{
  File file;
  DBUG_ENTER("open_binlog");

  if ((file = my_open(log_file_name, O_RDONLY | O_BINARY | O_SHARE, 
                      MYF(MY_WME))) < 0)
  {
    sql_print_error("Failed to open log (file '%s', errno %d)",
                    log_file_name, my_errno);
    *errmsg = "Could not open log file";
    goto err;
  }
  if (init_io_cache(log, file, rpl_read_size, READ_CACHE, 0, 0,
                    MYF(MY_WME|MY_DONT_CHECK_FILESIZE)))
  {
    sql_print_error("Failed to create a cache on log (file '%s')",
                    log_file_name);
    *errmsg = "Could not open log file";
    goto err;
  }
  if (check_binlog_magic(log,errmsg))
    goto err;
  DBUG_RETURN(file);

err:
  if (file >= 0)
  {
    my_close(file,MYF(0));
    end_io_cache(log);
  }
  DBUG_RETURN(-1);
}

#ifdef __NT__
static int eventSource = 0;

static void setup_windows_event_source()
{
  HKEY    hRegKey= NULL;
  DWORD   dwError= 0;
  TCHAR   szPath[MAX_PATH];
  DWORD dwTypes;

  if (eventSource)               // Ensure that we are only called once
    return;
  eventSource= 1;

  // Create the event source registry key
  dwError= RegCreateKey(HKEY_LOCAL_MACHINE,
                          "SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\MySQL", 
                          &hRegKey);

  /* Name of the PE module that contains the message resource */
  GetModuleFileName(NULL, szPath, MAX_PATH);

  /* Register EventMessageFile */
  dwError = RegSetValueEx(hRegKey, "EventMessageFile", 0, REG_EXPAND_SZ,
                          (PBYTE) szPath, (DWORD) (strlen(szPath) + 1));

  /* Register supported event types */
  dwTypes= (EVENTLOG_ERROR_TYPE | EVENTLOG_WARNING_TYPE |
            EVENTLOG_INFORMATION_TYPE);
  dwError= RegSetValueEx(hRegKey, "TypesSupported", 0, REG_DWORD,
                         (LPBYTE) &dwTypes, sizeof dwTypes);

  RegCloseKey(hRegKey);
}

#endif /* __NT__ */


/**
  Find a unique filename for 'filename.#'.

  Set '#' to a number as low as possible.

  @return
    nonzero if not possible to get unique filename
*/

static int find_uniq_filename(char *name)
{
  long                  number= 0;
  uint                  i;
  char                  buff[FN_REFLEN];
  struct st_my_dir     *dir_info;
  reg1 struct fileinfo *file_info;
  ulong                 max_found=0;
  size_t		buf_length, length;
  char			*start, *end;
  DBUG_ENTER("find_uniq_filename");

  length= dirname_part(buff, name, &buf_length);
  start=  name + length;
  end=    strend(start);

  *end='.';
  length= (size_t) (end-start+1);

  if ((DBUG_EVALUATE_IF("error_unique_log_filename", 1, 
      !(dir_info = my_dir(buff,MYF(MY_DONT_SORT))))))
  {						// This shouldn't happen
    strmov(end,".1");				// use name+1
    DBUG_RETURN(1);
  }
  file_info= dir_info->dir_entry;
  for (i=dir_info->number_off_files ; i-- ; file_info++)
  {
    if (memcmp(file_info->name, start, length) == 0 &&
	test_if_number(file_info->name+length, &number,0))
    {
      set_if_bigger(max_found,(ulong) number);
    }
  }
  my_dirend(dir_info);

  *end++='.';
  DBUG_RETURN((sprintf(end,"%06ld",max_found+1) < 0));
}


void MYSQL_LOG::init(enum_log_type log_type_arg,
                     enum cache_type io_cache_type_arg)
{
  DBUG_ENTER("MYSQL_LOG::init");
  log_type= log_type_arg;
  io_cache_type= io_cache_type_arg;
  DBUG_PRINT("info",("log_type: %d", log_type));
  DBUG_VOID_RETURN;
}


bool MYSQL_LOG::init_and_set_log_file_name(const char *log_name,
                                           const char *new_name,
                                           enum_log_type log_type_arg,
                                           enum cache_type io_cache_type_arg)
{
  init(log_type_arg, io_cache_type_arg);

  if (new_name && !strmov(log_file_name, new_name))
    return TRUE;
  else if (!new_name && generate_new_name(log_file_name, log_name))
    return TRUE;

  return FALSE;
}


/*
  Open a (new) log file.

  SYNOPSIS
    open()

    log_name            The name of the log to open
    log_type_arg        The type of the log. E.g. LOG_NORMAL
    new_name            The new name for the logfile. This is only needed
                        when the method is used to open the binlog file.
    io_cache_type_arg   The type of the IO_CACHE to use for this log file

  DESCRIPTION
    Open the logfile, init IO_CACHE and write startup messages
    (in case of general and slow query logs).

  RETURN VALUES
    0   ok
    1   error
*/

bool MYSQL_LOG::open(const char *log_name, enum_log_type log_type_arg,
                     const char *new_name, enum cache_type io_cache_type_arg,
                     bool need_mutex)
{
  char buff[FN_REFLEN];
  File file= -1;
  int open_flags= O_CREAT | O_BINARY;
  DBUG_ENTER("MYSQL_LOG::open");
  DBUG_PRINT("enter", ("log_type: %d", (int) log_type_arg));

  write_error= 0;

  if (need_mutex)
    (void) pthread_mutex_lock(&LOCK_log);

  if (!(name= my_strdup(log_name, MYF(MY_WME))))
  {
    name= (char *)log_name; // for the error message
    goto err;
  }

  if (init_and_set_log_file_name(name, new_name,
                                 log_type_arg, io_cache_type_arg))
    goto err;

  if (io_cache_type == SEQ_READ_APPEND)
    open_flags |= O_RDWR | O_APPEND;
  else
    open_flags |= O_WRONLY | (log_type == LOG_BIN ? 0 : O_APPEND);

  db[0]= 0;

  if ((file= my_open(log_file_name, open_flags,
                     MYF(MY_WME | ME_WAITTANG))) < 0 ||
      init_io_cache(&log_file, file, IO_SIZE, io_cache_type,
                    my_tell(file, MYF(MY_WME)), 0,
                    MYF(MY_WME | MY_NABP |
                        ((log_type == LOG_BIN) ? MY_WAIT_IF_FULL : 0))))
    goto err;

  if (log_type == LOG_NORMAL)
  {
    char *end;
    int len=my_snprintf(buff, sizeof(buff), "%s, Version: %s (%s). "
#ifdef EMBEDDED_LIBRARY
                        "embedded library\n",
                        my_progname, server_version, MYSQL_COMPILATION_COMMENT
#elif __NT__
			"started with:\nTCP Port: %d, Named Pipe: %s\n",
                        my_progname, server_version, MYSQL_COMPILATION_COMMENT,
                        mysqld_port, mysqld_unix_port
#else
			"started with:\nTcp port: %d  Unix socket: %s\n",
                        my_progname, server_version, MYSQL_COMPILATION_COMMENT,
                        mysqld_port, mysqld_unix_port
#endif
                       );
    end= strnmov(buff + len, "Time                 Id Command    Argument\n",
                 sizeof(buff) - len);
    if (my_b_write(&log_file, (uchar*) buff, (uint) (end-buff)) ||
	flush_io_cache(&log_file))
      goto err;
  }

  log_state= LOG_OPENED;
  if (need_mutex)
    (void) pthread_mutex_unlock(&LOCK_log);
  DBUG_RETURN(0);

err:
  sql_print_error("Could not use %s for logging (error %d). \
Turning logging off for the whole duration of the MySQL server process. \
To turn it on again: fix the cause, \
shutdown the MySQL server and restart it.", name, errno);
  if (file >= 0)
    my_close(file, MYF(0));
  end_io_cache(&log_file);
  safeFree(name);
  log_state= LOG_CLOSED;
  if (need_mutex)
    (void) pthread_mutex_unlock(&LOCK_log);
  DBUG_RETURN(1);
}

MYSQL_LOG::MYSQL_LOG()
  : name(0), write_error(FALSE), inited(FALSE), log_type(LOG_UNKNOWN),
    log_state(LOG_CLOSED)
{
  /*
    We don't want to initialize LOCK_Log here as such initialization depends on
    safe_mutex (when using safe_mutex) which depends on MY_INIT(), which is
    called only in main(). Doing initialization here would make it happen
    before main().
  */
  bzero((char*) &log_file, sizeof(log_file));
}

void MYSQL_LOG::init_pthread_objects()
{
  DBUG_ASSERT(inited == 0);
  inited= 1;
  (void) pthread_mutex_init(&LOCK_log, MY_MUTEX_INIT_SLOW);
  (void) pthread_mutex_init(&LOCK_group_commit, MY_MUTEX_INIT_FAST);
}

/*
  Close the log file

  SYNOPSIS
    close()
    exiting     Bitmask. For the slow and general logs the only used bit is
                LOG_CLOSE_TO_BE_OPENED. This is used if we intend to call
                open at once after close.

  NOTES
    One can do an open on the object at once after doing a close.
    The internal structures are not freed until cleanup() is called
*/

void MYSQL_LOG::close(uint exiting)
{					// One can't set log_type here!
  DBUG_ENTER("MYSQL_LOG::close");
  DBUG_PRINT("enter",("exiting: %d", (int) exiting));
  if (log_state == LOG_OPENED)
  {
    end_io_cache(&log_file);

    if (my_sync(log_file.file, MYF(MY_WME)) && ! write_error)
    {
      write_error= 1;
      sql_print_error(ER(ER_ERROR_ON_WRITE), name, errno);
    }

    if (my_close(log_file.file, MYF(MY_WME)) && ! write_error)
    {
      write_error= 1;
      sql_print_error(ER(ER_ERROR_ON_WRITE), name, errno);
    }
  }

  log_state= (exiting & LOG_CLOSE_TO_BE_OPENED) ? LOG_TO_BE_OPENED : LOG_CLOSED;
  safeFree(name);
  DBUG_VOID_RETURN;
}

/** This is called only once. */

void MYSQL_LOG::cleanup()
{
  DBUG_ENTER("cleanup");
  if (inited)
  {
    inited= 0;
    (void) pthread_mutex_destroy(&LOCK_log);
    (void) pthread_mutex_destroy(&LOCK_group_commit);
    close(0);
  }
  DBUG_VOID_RETURN;
}


int MYSQL_LOG::generate_new_name(char *new_name, const char *log_name)
{
  fn_format(new_name, log_name, mysql_data_home, "", 4);
  if (log_type == LOG_BIN)
  {
    if (!fn_ext(log_name)[0])
    {
      if (find_uniq_filename(new_name))
      {
        my_printf_error(ER_NO_UNIQUE_LOGFILE, ER(ER_NO_UNIQUE_LOGFILE),
                        MYF(ME_FATALERROR), log_name);
	sql_print_error(ER(ER_NO_UNIQUE_LOGFILE), log_name);
	return 1;
      }
    }
  }
  return 0;
}


/*
  Reopen the log file

  SYNOPSIS
    reopen_file()

  DESCRIPTION
    Reopen the log file. The method is used during FLUSH LOGS
    and locks LOCK_log mutex
*/


void MYSQL_QUERY_LOG::reopen_file()
{
  char *save_name;

  DBUG_ENTER("MYSQL_LOG::reopen_file");
  if (!is_open())
  {
    DBUG_PRINT("info",("log is closed"));
    DBUG_VOID_RETURN;
  }

  pthread_mutex_lock(&LOCK_log);

  save_name= name;
  name= 0;				// Don't free name
  close(LOG_CLOSE_TO_BE_OPENED);

  /*
     Note that at this point, log_state != LOG_CLOSED (important for is_open()).
  */

  open(save_name, log_type, 0, io_cache_type, false);
  my_free(save_name, MYF(0));

  pthread_mutex_unlock(&LOCK_log);

  DBUG_VOID_RETURN;
}


/*
  Write a command to traditional general log file

  SYNOPSIS
    write()

    event_time        command start timestamp
    user_host         the pointer to the string with user@host info
    user_host_len     length of the user_host string. this is computed once
                      and passed to all general log  event handlers
    thread_id         Id of the thread, issued a query
    command_type      the type of the command being logged
    command_type_len  the length of the string above
    sql_text          the very text of the query being executed
    sql_text_len      the length of sql_text string

  DESCRIPTION

   Log given command to to normal (not rotable) log file

  RETURN
    FASE - OK
    TRUE - error occured
*/

bool MYSQL_QUERY_LOG::write(time_t event_time, const char *user_host,
                            uint user_host_len, int thread_id,
                            const char *command_type, uint command_type_len,
                            const char *sql_text, uint sql_text_len)
{
  char buff[32];
  uint length= 0;
  char local_time_buff[MAX_TIME_SIZE];
  struct tm start;
  uint time_buff_len= 0;

  (void) pthread_mutex_lock(&LOCK_log);

  /* Test if someone closed between the is_open test and lock */
  if (is_open())
  {
    /* for testing output of timestamp and thread id */
    DBUG_EXECUTE_IF("reset_log_last_time", last_time= 0;);

    /* Note that my_b_write() assumes it knows the length for this */
      if (event_time != last_time)
      {
        last_time= event_time;

        localtime_r(&event_time, &start);

        time_buff_len= my_snprintf(local_time_buff, MAX_TIME_SIZE,
                                   "%02d%02d%02d %2d:%02d:%02d\t",
                                   start.tm_year % 100, start.tm_mon + 1,
                                   start.tm_mday, start.tm_hour,
                                   start.tm_min, start.tm_sec);

        if (my_b_write(&log_file, (uchar*) local_time_buff, time_buff_len))
          goto err;
      }
      else
        if (my_b_write(&log_file, (uchar*) "\t\t" ,2) < 0)
          goto err;

      /* command_type, thread_id */
      length= my_snprintf(buff, 32, "%5ld ", (long) thread_id);

    if (my_b_write(&log_file, (uchar*) buff, length))
      goto err;

    if (my_b_write(&log_file, (uchar*) command_type, command_type_len))
      goto err;

    if (my_b_write(&log_file, (uchar*) "\t", 1))
      goto err;

    /* sql_text */
    if (my_b_write(&log_file, (uchar*) sql_text, sql_text_len))
      goto err;

    if (my_b_write(&log_file, (uchar*) "\n", 1) ||
        flush_io_cache(&log_file))
      goto err;
  }

  (void) pthread_mutex_unlock(&LOCK_log);
  return FALSE;
err:

  if (!write_error)
  {
    write_error= 1;
    sql_print_error(ER(ER_ERROR_ON_WRITE), name, errno);
  }
  (void) pthread_mutex_unlock(&LOCK_log);
  return TRUE;
}


/*
  Log a query to the traditional slow log file

  SYNOPSIS
    write()

    thd               THD of the query
    current_time      current timestamp
    query_start_arg   command start timestamp
    user_host         the pointer to the string with user@host info
    user_host_len     length of the user_host string. this is computed once
                      and passed to all general log event handlers
    query_utime       Amount of time the query took to execute (in microseconds)
    lock_utime        Amount of time the query was locked (in microseconds)
    is_command        The flag, which determines, whether the sql_text is a
                      query or an administrator command.
    sql_text          the very text of the query or administrator command
                      processed
    sql_text_len      the length of sql_text string

  DESCRIPTION

   Log a query to the slow log file.

  RETURN
    FALSE - OK
    TRUE - error occured
*/

bool MYSQL_QUERY_LOG::write(THD *thd, time_t current_time,
                            time_t query_start_arg, const char *user_host,
                            uint user_host_len, ulonglong query_utime,
                            ulonglong lock_utime, bool is_command,
                            const char *sql_text, uint sql_text_len,
                            struct system_status_var *query_start)
{
  bool error= 0;
  char buff[80], start_time_buff[80], end_time_buff[80], read_time_buff[80];
  char query_time_buff[22+7], lock_time_buff[22+7];
  uint buff_len=0;
  DBUG_ENTER("MYSQL_QUERY_LOG::write");

  if (!is_open())
    DBUG_RETURN(0);

  if (!(specialflag & SPECIAL_SHORT_LOG_FORMAT))
  {
    /* Explicitly done before LOCK_log is locked */
    if (current_time != last_time)
    {
      struct tm start;
      localtime_r(&current_time, &start);

      buff_len= my_snprintf(buff, sizeof buff,
                            "# Time: %02d%02d%02d %2d:%02d:%02d\n",
                            start.tm_year % 100, start.tm_mon + 1,
                            start.tm_mday, start.tm_hour,
                            start.tm_min, start.tm_sec);
    }
  }
  /* For slow query log */
  sprintf(query_time_buff, "%.6f", ulonglong2double(query_utime)/1000000.0);
  sprintf(lock_time_buff,  "%.6f", ulonglong2double(lock_utime)/1000000.0);
  if (opt_log_slow_extra && query_start_arg && query_start)
  {
    struct tm tm_tmp;

    current_time=time(NULL);
    localtime_r(&current_time,&tm_tmp);
    sprintf(end_time_buff,"%2d:%02d:%02d",
            tm_tmp.tm_hour, tm_tmp.tm_min, tm_tmp.tm_sec);

    localtime_r(&query_start_arg,&tm_tmp);
    sprintf(start_time_buff,"%2d:%02d:%02d",
            tm_tmp.tm_hour, tm_tmp.tm_min, tm_tmp.tm_sec);
    sprintf(read_time_buff,"%.6f",
           thd->status_var.read_seconds - query_start->read_seconds);
  }
  else
  {
    end_time_buff[0] = '\0';
    start_time_buff[0] = '\0';
    read_time_buff[0] = '\0';
  }

  (void) pthread_mutex_lock(&LOCK_log);

  if (is_open())
  {						// Safety agains reopen
    int tmp_errno= 0;
    char *end= buff;

    if (!(specialflag & SPECIAL_SHORT_LOG_FORMAT))
    {
      if (current_time != last_time)
      {
        last_time= current_time;

        /* Note that my_b_write() assumes it knows the length for this */
        if (my_b_write(&log_file, (uchar*) buff, buff_len))
          tmp_errno= errno;
      }
      const uchar uh[]= "# User@Host: ";
      if (my_b_write(&log_file, uh, sizeof(uh) - 1))
        tmp_errno= errno;
      if (my_b_write(&log_file, (uchar*) user_host, user_host_len))
        tmp_errno= errno;
      if (my_b_write(&log_file, (uchar*) "\n", 1))
        tmp_errno= errno;
    }

    if (!query_start)
    {
      if (my_b_printf(&log_file,
                      "# Query_time: %s  Lock_time: %s"
                      " Rows_sent: %lu  Rows_examined: %lu\n",
                      query_time_buff, lock_time_buff,
                      (ulong) thd->sent_row_count,
                      (ulong) thd->examined_row_count) == (uint) -1)
        tmp_errno= errno;
    }
    else
    {
      if (my_b_printf(&log_file,
                      "# Query_time: %s  Lock_time: %s"
                      " Rows_sent: %lu  Rows_examined: %lu"
                      " Thread_id: %lu Errno: %lu Killed: %lu"
                      " Bytes_received: %lu Bytes_sent: %lu"
                      " Read_first: %lu Read_last: %lu Read_key: %lu"
                      " Read_next: %lu Read_prev: %lu"
                      " Read_rnd: %lu Read_rnd_next: %lu"
                      " Sort_merge_passes: %lu Sort_range_count: %lu"
                      " Sort_rows: %lu Sort_scan_count: %lu"
                      " Created_tmp_disk_tables: %lu"
                      " Created_tmp_tables: %lu"
                      " Start: %s End: %s"
                      " Reads: %lu Read_time: %s\n",
                      query_time_buff, lock_time_buff,
                      (ulong) thd->sent_row_count,
                      (ulong) thd->examined_row_count,
                      (ulong) thd->thread_id,
                      (ulong) (thd->is_error() ? thd->main_da.sql_errno() : 0),
                      (ulong) thd->killed,
                      (ulong) (thd->status_var.bytes_received -
                          query_start->bytes_received),
                      (ulong) (thd->status_var.bytes_sent -
                          query_start->bytes_sent),
                      (ulong) (thd->status_var.ha_read_first_count -
                          query_start->ha_read_first_count),
                      (ulong) (thd->status_var.ha_read_last_count -
                          query_start->ha_read_last_count),
                      (ulong) (thd->status_var.ha_read_key_count -
                          query_start->ha_read_key_count),
                      (ulong) (thd->status_var.ha_read_next_count -
                          query_start->ha_read_next_count),
                      (ulong) (thd->status_var.ha_read_prev_count -
                          query_start->ha_read_prev_count),
                      (ulong) (thd->status_var.ha_read_rnd_count -
                          query_start->ha_read_rnd_count),
                      (ulong) (thd->status_var.ha_read_rnd_next_count -
                          query_start->ha_read_rnd_next_count),
                      (ulong) (thd->status_var.filesort_merge_passes -
                          query_start->filesort_merge_passes),
                      (ulong) (thd->status_var.filesort_range_count -
                          query_start->filesort_range_count),
                      (ulong) (thd->status_var.filesort_rows -
                          query_start->filesort_rows),
                      (ulong) (thd->status_var.filesort_scan_count -
                          query_start->filesort_scan_count),
                      (ulong) (thd->status_var.created_tmp_disk_tables -
                          query_start->created_tmp_disk_tables),
                      (ulong) (thd->status_var.created_tmp_tables -
                          query_start->created_tmp_tables),
                      start_time_buff, end_time_buff,
                      (ulong) (thd->status_var.read_requests -
                          query_start->read_requests),
                      read_time_buff) == (uint) -1)
        tmp_errno=errno;
    }

    if (thd->db && strcmp(thd->db, db))
    {						// Database changed
      if (my_b_printf(&log_file,"use %s;\n",thd->db) == (uint) -1)
        tmp_errno= errno;
      strmov(db,thd->db);
    }
    if (thd->stmt_depends_on_first_successful_insert_id_in_prev_stmt)
    {
      end=strmov(end, ",last_insert_id=");
      end=longlong10_to_str((longlong)
                            thd->first_successful_insert_id_in_prev_stmt_for_binlog,
                            end, -10);
    }
    // Save value if we do an insert.
    if (thd->auto_inc_intervals_in_cur_stmt_for_binlog.nb_elements() > 0)
    {
      if (!(specialflag & SPECIAL_SHORT_LOG_FORMAT))
      {
        end=strmov(end,",insert_id=");
        end=longlong10_to_str((longlong)
                              thd->auto_inc_intervals_in_cur_stmt_for_binlog.minimum(),
                              end, -10);
      }
    }

    /*
      The timestamp used to only be set when the query had checked the
      start time. Now the slow log always logs the query start time.
      This ensures logs can be used to replicate queries accurately.
    */
    end= strmov(end, ",timestamp=");
    end= int10_to_str((long) query_start_arg, end, 10);

    if (end != buff)
    {
      *end++=';';
      *end='\n';
      if (my_b_write(&log_file, (uchar*) "SET ", 4) ||
          my_b_write(&log_file, (uchar*) buff + 1, (uint) (end-buff)))
        tmp_errno= errno;
    }
    if (is_command)
    {
      end= strxmov(buff, "# administrator command: ", NullS);
      buff_len= (ulong) (end - buff);
      my_b_write(&log_file, (uchar*) buff, buff_len);
    }
    if (my_b_write(&log_file, (uchar*) sql_text, sql_text_len) ||
        my_b_write(&log_file, (uchar*) ";\n",2) ||
        flush_io_cache(&log_file))
      tmp_errno= errno;
    if (tmp_errno)
    {
      error= 1;
      if (! write_error)
      {
        write_error= 1;
        sql_print_error(ER(ER_ERROR_ON_WRITE), name, error);
      }
    }
  }
  (void) pthread_mutex_unlock(&LOCK_log);
  DBUG_RETURN(error);
}


/**
  @todo
  The following should be using fn_format();  We just need to
  first change fn_format() to cut the file name if it's too long.
*/
const char *MYSQL_LOG::generate_name(const char *log_name,
                                      const char *suffix,
                                      bool strip_ext, char *buff)
{
  if (!log_name || !log_name[0])
  {
    strmake(buff, pidfile_name, FN_REFLEN - strlen(suffix) - 1);
    return (const char *)
      fn_format(buff, buff, "", suffix, MYF(MY_REPLACE_EXT|MY_REPLACE_DIR));
  }
  // get rid of extension if the log is binary to avoid problems
  if (strip_ext)
  {
    char *p= fn_ext(log_name);
    uint length= (uint) (p - log_name);
    strmake(buff, log_name, min(length, FN_REFLEN-1));
    return (const char*)buff;
  }
  return log_name;
}



MYSQL_BIN_LOG::MYSQL_BIN_LOG()
  :group_commit_allowed(TRUE),
   current_ticket(1),
   next_ticket(1),
   bytes_written(0), stop_new_xids(FALSE), prepared_xids(0), file_id(1), open_count(1),
   need_start_event(TRUE),
   active_mi(NULL),
   is_relay_log(0),
   description_event_for_exec(0), description_event_for_queue(0)
{
  /*
    We don't want to initialize locks here as such initialization depends on
    safe_mutex (when using safe_mutex) which depends on MY_INIT(), which is
    called only in main(). Doing initialization here would make it happen
    before main().
  */
  index_file_name[0] = 0;
  bzero((char*) &index_file, sizeof(index_file));
  bzero((char*) &purge_index_file, sizeof(purge_index_file));
}

/* this is called only once */

void MYSQL_BIN_LOG::cleanup()
{
  DBUG_ENTER("cleanup");
  if (inited)
  {
    inited= 0;
    close(LOG_CLOSE_INDEX|LOG_CLOSE_STOP_EVENT);
    delete description_event_for_queue;
    delete description_event_for_exec;
    (void) pthread_mutex_destroy(&LOCK_log);
    (void) pthread_mutex_destroy(&LOCK_group_commit);
    (void) pthread_mutex_destroy(&LOCK_index);
    (void) pthread_cond_destroy(&update_cond);
    for (int i=0; i < NUM_BINLOG_COMMIT_COND; i++)
    {
      (void) pthread_cond_destroy(&binlog_commit_cond_array[i]);
    }
    (void) pthread_cond_destroy(&binlog_cond);
  }
  (void) pthread_cond_destroy(&COND_stop_xids);
  DBUG_VOID_RETURN;
}


/* Init binlog-specific vars */
void MYSQL_BIN_LOG::init(bool no_auto_events_arg, ulong max_size_arg)
{
  DBUG_ENTER("MYSQL_BIN_LOG::init");
  no_auto_events= no_auto_events_arg;
  max_size= max_size_arg;
  DBUG_PRINT("info",("max_size: %lu", max_size));
  DBUG_VOID_RETURN;
}


void MYSQL_BIN_LOG::init_pthread_objects()
{
  DBUG_ASSERT(inited == 0);
  inited= 1;
  (void) pthread_mutex_init(&LOCK_log, MY_MUTEX_INIT_SLOW);
  (void) pthread_mutex_init(&LOCK_index, MY_MUTEX_INIT_SLOW);
  (void) pthread_mutex_init(&LOCK_group_commit, MY_MUTEX_INIT_FAST);
  (void) pthread_cond_init(&update_cond, 0);
  for (int i=0; i < NUM_BINLOG_COMMIT_COND; i++)
  {
    (void) pthread_cond_init(&binlog_commit_cond_array[i], 0);
  }
  (void) pthread_cond_init(&binlog_cond, 0);
  (void) pthread_cond_init(&COND_stop_xids, 0);
}


bool MYSQL_BIN_LOG::open_index_file(const char *index_file_name_arg,
                                    const char *log_name, bool need_mutex)
{
  File index_file_nr= -1;
  DBUG_ASSERT(!my_b_inited(&index_file));

  /*
    First open of this class instance
    Create an index file that will hold all file names uses for logging.
    Add new entries to the end of it.
  */
  myf opt= MY_UNPACK_FILENAME;
  if (!index_file_name_arg)
  {
    index_file_name_arg= log_name;    // Use same basename for index file
    opt= MY_UNPACK_FILENAME | MY_REPLACE_EXT;
  }
  fn_format(index_file_name, index_file_name_arg, mysql_data_home,
            ".index", opt);
  if ((index_file_nr= my_open(index_file_name,
                              O_RDWR | O_CREAT | O_BINARY ,
                              MYF(MY_WME))) < 0 ||
       my_sync(index_file_nr, MYF(MY_WME)) ||
       init_io_cache(&index_file, index_file_nr,
                     IO_SIZE, WRITE_CACHE,
                     my_seek(index_file_nr,0L,MY_SEEK_END,MYF(0)),
			0, MYF(MY_WME | MY_WAIT_IF_FULL)) ||
      DBUG_EVALUATE_IF("fault_injection_openning_index", 1, 0))
  {
    /*
      TODO: all operations creating/deleting the index file or a log, should
      call my_sync_dir() or my_sync_dir_by_file() to be durable.
      TODO: file creation should be done with my_create() not my_open().
    */
    if (index_file_nr >= 0)
      my_close(index_file_nr,MYF(0));
    return TRUE;
  }

#ifdef HAVE_REPLICATION
  /*
    Sync the index by purging any binary log file that is not registered.
    In other words, either purge binary log files that were removed from
    the index but not purged from the file system due to a crash or purge
    any binary log file that was created but not register in the index
    due to a crash.
  */

  if (set_purge_index_file_name(index_file_name_arg) ||
      open_purge_index_file(FALSE) ||
      purge_index_entry(NULL, NULL, need_mutex) ||
      close_purge_index_file() ||
      DBUG_EVALUATE_IF("fault_injection_recovering_index", 1, 0))
  {
    sql_print_error("MYSQL_BIN_LOG::open_index_file failed to sync the index "
                    "file.");
    return TRUE;
  }
#endif

  return FALSE;
}


int MYSQL_BIN_LOG::close_index_file()
{
  if (my_b_inited(&index_file))
  {
    end_io_cache(&index_file);
    my_close(index_file.file, MYF(0));
  }
  return 0;
}


/**
  Open a (new) binlog file.

  - Open the log file and the index file. Register the new
  file name in it
  - When calling this when the file is in use, you must have a locks
  on LOCK_log and LOCK_index.

  @retval
    0	ok
  @retval
    1	error
*/

bool MYSQL_BIN_LOG::open(const char *log_name,
                         enum_log_type log_type_arg,
                         const char *new_name,
                         enum cache_type io_cache_type_arg,
                         bool no_auto_events_arg,
                         ulong max_size_arg,
                         bool null_created_arg,
                         bool need_mutex)
{
  File file= -1;

  DBUG_ENTER("MYSQL_BIN_LOG::open");
  DBUG_PRINT("enter",("log_type: %d",(int) log_type_arg));

  if (need_mutex)
    (void) pthread_mutex_lock(&LOCK_log);

  if (init_and_set_log_file_name(log_name, new_name, log_type_arg,
                                 io_cache_type_arg))
  {
    sql_print_error("MSYQL_BIN_LOG::open failed to generate new file name.");
    DBUG_RETURN(1);
  }

#ifdef HAVE_REPLICATION
  if (open_purge_index_file(TRUE) ||
      register_create_index_entry(log_file_name) ||
      sync_purge_index_file() ||
      DBUG_EVALUATE_IF("fault_injection_registering_index", 1, 0))
  {
    /**
        TODO: although this was introduced to appease valgrind
              when injecting emulated faults using fault_injection_registering_index
              it may be good to consider what actually happens when
              open_purge_index_file succeeds but register or sync fails.

              Perhaps we might need the code below in MYSQL_LOG_BIN::cleanup
              for "real life" purposes as well? 
     */
    DBUG_EXECUTE_IF("fault_injection_registering_index", {
      if (my_b_inited(&purge_index_file))
      {
        end_io_cache(&purge_index_file);
        my_close(purge_index_file.file, MYF(0));
      }
    });

    sql_print_error("MSYQL_BIN_LOG::open failed to sync the index file.");
    DBUG_RETURN(1);
  }
  DBUG_EXECUTE_IF("crash_create_non_critical_before_update_index", DBUG_SUICIDE(););
#endif

  write_error= 0;

  /* open the main log file */
  if (MYSQL_LOG::open(log_name, log_type_arg, new_name,
                      io_cache_type_arg, false))
  {
#ifdef HAVE_REPLICATION
    close_purge_index_file();
#endif
    DBUG_RETURN(1);                            /* all warnings issued */
  }

  init(no_auto_events_arg, max_size_arg);

  open_count++;

  DBUG_ASSERT(log_type == LOG_BIN);

  {
    bool write_file_name_to_index_file=0;

    if (!my_b_filelength(&log_file))
    {
      /*
	The binary log file was empty (probably newly created)
	This is the normal case and happens when the user doesn't specify
	an extension for the binary log files.
	In this case we write a standard header to it.
      */
      if (my_b_safe_write(&log_file, (uchar*) BINLOG_MAGIC,
			  BIN_LOG_HEADER_SIZE))
        goto err;
      bytes_written+= BIN_LOG_HEADER_SIZE;
      write_file_name_to_index_file= 1;
    }

    if (need_start_event && !no_auto_events)
    {
      /*
        In 4.x we set need_start_event=0 here, but in 5.0 we want a Start event
        even if this is not the very first binlog.
      */
      Format_description_log_event s(BINLOG_VERSION);
      /*
        don't set LOG_EVENT_BINLOG_IN_USE_F for SEQ_READ_APPEND io_cache
        as we won't be able to reset it later
      */
      if (io_cache_type == WRITE_CACHE)
        s.flags|= LOG_EVENT_BINLOG_IN_USE_F;
      if (!s.is_valid())
        goto err;
      s.dont_set_created= null_created_arg;
      if (s.write(&log_file))
        goto err;
      bytes_written+= s.data_written;
    }
    if (description_event_for_queue &&
        description_event_for_queue->binlog_version>=4)
    {
      /*
        This is a relay log written to by the I/O slave thread.
        Write the event so that others can later know the format of this relay
        log.
        Note that this event is very close to the original event from the
        master (it has binlog version of the master, event types of the
        master), so this is suitable to parse the next relay log's event. It
        has been produced by
        Format_description_log_event::Format_description_log_event(char* buf,).
        Why don't we want to write the description_event_for_queue if this
        event is for format<4 (3.23 or 4.x): this is because in that case, the
        description_event_for_queue describes the data received from the
        master, but not the data written to the relay log (*conversion*),
        which is in format 4 (slave's).
      */
      /*
        Set 'created' to 0, so that in next relay logs this event does not
        trigger cleaning actions on the slave in
        Format_description_log_event::apply_event_impl().
      */
      description_event_for_queue->created= 0;
      /* Don't set log_pos in event header */
      description_event_for_queue->set_artificial_event();

      if (description_event_for_queue->write(&log_file))
        goto err;
      bytes_written+= description_event_for_queue->data_written;
    }

    if (rpl_transaction_enabled)
    {
      /*
         Make sure that filename is not longer than the limit inside
         InnoDB's transaction header.
       */
      if (strlen(log_file_name) >= MAX_INNODB_BINLOG_FILENAME_LEN)
      {
        sql_print_error("Too long binlog filename(%s) for InnoDB: %d bytes",
                        log_file_name, MAX_INNODB_BINLOG_FILENAME_LEN);
        goto err;
      }

      /*
        Need a special event in each relay-log file to make sure that each
        file always has the correct master-log information itself. Always
        write a Rotate_log_event with server_id as MASTER_INFO_SERVER_ID
        at the beginning of the relay-log with the corresponding master-log
        information.
       */
      Master_info *mi = get_master_info();
      if (mi != NULL && strlen(mi->master_log_name) > 0)
      {
        Rotate_log_event mi_event(mi->master_log_name,
                                  strlen(mi->master_log_name),
                                  mi->master_log_pos, 0);
        mi_event.set_server_id(MASTER_INFO_SERVER_ID);
        if (mi_event.write(&log_file))
        {
          sql_print_error("Could not write MASTER Rotate_log_event");
          goto err;
        }
        bytes_written += mi_event.data_written;
      }
    }

    if (flush_io_cache(&log_file) ||
        my_sync(log_file.file, MYF(MY_WME)))
      goto err;

    if (!is_relay_log)
    {
      /* 
        If binlog_last_valid_pos is not set here, read_log_event in 
        mysql_binlog_send() may hit EOF on the first read itself (since
        binlog_last_valid_pos is initialized to 0) which causes to reopen
        binlog again unnecessarily resulting in slowing down of reading
        binlogs.
      */
      set_binlog_last_valid_pos(my_b_tell(&log_file));
    }
    if (write_file_name_to_index_file)
    {
#ifdef HAVE_REPLICATION
      DBUG_EXECUTE_IF("crash_create_critical_before_update_index", DBUG_SUICIDE(););
#endif

      DBUG_ASSERT(my_b_inited(&index_file) != 0);
      reinit_io_cache(&index_file, WRITE_CACHE,
                      my_b_filelength(&index_file), 0, 0);
      /*
        As this is a new log file, we write the file name to the index
        file. As every time we write to the index file, we sync it.
      */
      if (DBUG_EVALUATE_IF("fault_injection_updating_index", 1, 0) ||
          my_b_write(&index_file, (uchar*) log_file_name,
                     strlen(log_file_name)) ||
          my_b_write(&index_file, (uchar*) "\n", 1) ||
          flush_io_cache(&index_file) ||
          my_sync(index_file.file, MYF(MY_WME)))
        goto err;

#ifdef HAVE_REPLICATION
      DBUG_EXECUTE_IF("crash_create_after_update_index", DBUG_SUICIDE(););
#endif
    }
  }
  log_state= LOG_OPENED;

#ifdef HAVE_REPLICATION
  close_purge_index_file();
#endif

  if (need_mutex)
    (void) pthread_mutex_unlock(&LOCK_log);
  DBUG_RETURN(0);

err:
#ifdef HAVE_REPLICATION
  if (is_inited_purge_index_file())
    purge_index_entry(NULL, NULL, false);
  close_purge_index_file();
#endif
  sql_print_error("Could not use %s for logging (error %d). \
Turning logging off for the whole duration of the MySQL server process. \
To turn it on again: fix the cause, \
shutdown the MySQL server and restart it.", name, errno);
  if (file >= 0)
    my_close(file,MYF(0));
  end_io_cache(&log_file);
  close_index_file();
  safeFree(name);
  log_state= LOG_CLOSED;
  if (need_mutex)
    (void) pthread_mutex_unlock(&LOCK_log);
  DBUG_RETURN(1);
}


int MYSQL_BIN_LOG::get_current_log(LOG_INFO* linfo)
{
  pthread_mutex_lock(&LOCK_log);
  int ret = raw_get_current_log(linfo);
  pthread_mutex_unlock(&LOCK_log);
  return ret;
}

int MYSQL_BIN_LOG::raw_get_current_log(LOG_INFO* linfo)
{
  strmake(linfo->log_file_name, log_file_name, sizeof(linfo->log_file_name)-1);
  linfo->pos = my_b_tell(&log_file);
  return 0;
}

/**
  Move all data up in a file in an filename index file.

    We do the copy outside of the IO_CACHE as the cache buffers would just
    make things slower and more complicated.
    In most cases the copy loop should only do one read.

  @param index_file			File to move
  @param offset			Move everything from here to beginning

  @note
    File will be truncated to be 'offset' shorter or filled up with newlines

  @retval
    0	ok
*/

#ifdef HAVE_REPLICATION

static bool copy_up_file_and_fill(IO_CACHE *index_file, my_off_t offset)
{
  int bytes_read;
  my_off_t init_offset= offset;
  File file= index_file->file;
  uchar io_buf[IO_SIZE*2];
  DBUG_ENTER("copy_up_file_and_fill");

  for (;; offset+= bytes_read)
  {
    (void) my_seek(file, offset, MY_SEEK_SET, MYF(0));
    if ((bytes_read= (int) my_read(file, io_buf, sizeof(io_buf), MYF(MY_WME)))
	< 0)
      goto err;
    if (!bytes_read)
      break;					// end of file
    (void) my_seek(file, offset-init_offset, MY_SEEK_SET, MYF(0));
    if (my_write(file, io_buf, bytes_read, MYF(MY_WME | MY_NABP)))
      goto err;
  }
  /* The following will either truncate the file or fill the end with \n' */
  if (my_chsize(file, offset - init_offset, '\n', MYF(MY_WME)) ||
      my_sync(file, MYF(MY_WME)))
    goto err;

  /* Reset data in old index cache */
  reinit_io_cache(index_file, READ_CACHE, (my_off_t) 0, 0, 1);
  DBUG_RETURN(0);

err:
  DBUG_RETURN(1);
}

#endif /* HAVE_REPLICATION */

/**
  Find the position in the log-index-file for the given log name.

  @param linfo		Store here the found log file name and position to
                       the NEXT log file name in the index file.
  @param log_name	Filename to find in the index file.
                       Is a null pointer if we want to read the first entry
  @param need_lock	Set this to 1 if the parent doesn't already have a
                       lock on LOCK_index

  @note
    On systems without the truncate function the file will end with one or
    more empty lines.  These will be ignored when reading the file.

  @retval
    0			ok
  @retval
    LOG_INFO_EOF	        End of log-index-file found
  @retval
    LOG_INFO_IO		Got IO error while reading file
*/

int MYSQL_BIN_LOG::find_log_pos(LOG_INFO *linfo, const char *log_name,
			    bool need_lock)
{
  int error= 0;
  char *fname= linfo->log_file_name;
  uint log_name_len= log_name ? (uint) strlen(log_name) : 0;
  DBUG_ENTER("find_log_pos");
  DBUG_PRINT("enter",("log_name: %s", log_name ? log_name : "NULL"));

  /*
    Mutex needed because we need to make sure the file pointer does not
    move from under our feet
  */
  if (need_lock)
    pthread_mutex_lock(&LOCK_index);
  safe_mutex_assert_owner(&LOCK_index);

  /* As the file is flushed, we can't get an error here */
  (void) reinit_io_cache(&index_file, READ_CACHE, (my_off_t) 0, 0, 0);

  for (;;)
  {
    uint length;
    my_off_t offset= my_b_tell(&index_file);
    /* If we get 0 or 1 characters, this is the end of the file */

    if ((length= my_b_gets(&index_file, fname, FN_REFLEN)) <= 1)
    {
      /* Did not find the given entry; Return not found or error */
      error= !index_file.error ? LOG_INFO_EOF : LOG_INFO_IO;
      break;
    }

    // if the log entry matches, null string matching anything
    if (!log_name ||
	(log_name_len == length-1 && fname[log_name_len] == '\n' &&
	 !memcmp(fname, log_name, log_name_len)))
    {
      DBUG_PRINT("info",("Found log file entry"));
      fname[length-1]=0;			// remove last \n
      linfo->index_file_start_offset= offset;
      linfo->index_file_offset = my_b_tell(&index_file);
      break;
    }
  }

  if (need_lock)
    pthread_mutex_unlock(&LOCK_index);
  DBUG_RETURN(error);
}


/**
  Find the position in the log-index-file for the given log name.

  @param
    linfo		Store here the next log file name and position to
			the file name after that.
  @param
    need_lock		Set this to 1 if the parent doesn't already have a
			lock on LOCK_index

  @note
    - Before calling this function, one has to call find_log_pos()
    to set up 'linfo'
    - Mutex needed because we need to make sure the file pointer does not move
    from under our feet

  @retval
    0			ok
  @retval
    LOG_INFO_EOF	        End of log-index-file found
  @retval
    LOG_INFO_IO		Got IO error while reading file
*/

int MYSQL_BIN_LOG::find_next_log(LOG_INFO* linfo, bool need_lock)
{
  int error= 0;
  uint length;
  char *fname= linfo->log_file_name;

  if (need_lock)
    pthread_mutex_lock(&LOCK_index);
  safe_mutex_assert_owner(&LOCK_index);

  /* As the file is flushed, we can't get an error here */
  (void) reinit_io_cache(&index_file, READ_CACHE, linfo->index_file_offset, 0,
			 0);

  linfo->index_file_start_offset= linfo->index_file_offset;
  if ((length=my_b_gets(&index_file, fname, FN_REFLEN)) <= 1)
  {
    error = !index_file.error ? LOG_INFO_EOF : LOG_INFO_IO;
    goto err;
  }
  fname[length-1]=0;				// kill \n
  linfo->index_file_offset = my_b_tell(&index_file);

err:
  if (need_lock)
    pthread_mutex_unlock(&LOCK_index);
  return error;
}


/**
  Delete all logs refered to in the index file.
  Start writing to a new log file.

  The new index file will only contain this file.

  @param thd		Thread
  @param need_lock      Whether locks must be obtained

  @note
    If not called from slave thread, write start event to new log

  @retval
    0	ok
  @retval
    1   error
*/

bool MYSQL_BIN_LOG::reset_logs(THD* thd, bool need_lock)
{
  LOG_INFO linfo;
  bool error=0;
  const char* save_name;
  DBUG_ENTER("reset_logs");

  ha_reset_logs(thd);

  /*
    The following mutex is needed to ensure that no threads call
    'delete thd' as we would then risk missing a 'rollback' from this
    thread. If the transaction involved MyISAM tables, it should go
    into binlog even on rollback.
  */
  pthread_mutex_lock(&LOCK_thread_count);

  if (need_lock)
  {
    /*
      We need to get both locks to be sure that no one is trying to
      write to the index log file.
    */
    pthread_mutex_lock(&LOCK_log);
    pthread_mutex_lock(&LOCK_index);
  }
  safe_mutex_assert_owner(&LOCK_log);
  safe_mutex_assert_owner(&LOCK_index);

  /* Save variables so that we can reopen the log */
  save_name=name;
  name=0;					// Protect against free
  close(LOG_CLOSE_TO_BE_OPENED);

  /*
    First delete all old log files and then update the index file.
    As we first delete the log files and do not use sort of logging,
    a crash may lead to an inconsistent state where the index has
    references to non-existent files.

    We need to invert the steps and use the purge_index_file methods
    in order to make the operation safe.
  */
  if (find_log_pos(&linfo, NullS, 0))
  {
    error=1;
    goto err;
  }

  for (;;)
  {
    if ((error= my_delete_allow_opened(linfo.log_file_name, MYF(0))) != 0)
    {
      if (my_errno == ENOENT) 
      {
        push_warning_printf(current_thd, MYSQL_ERROR::WARN_LEVEL_WARN,
                            ER_LOG_PURGE_NO_FILE, ER(ER_LOG_PURGE_NO_FILE),
                            linfo.log_file_name);
        sql_print_information("Failed to delete file '%s'",
                              linfo.log_file_name);
        my_errno= 0;
        error= 0;
      }
      else
      {
        push_warning_printf(current_thd, MYSQL_ERROR::WARN_LEVEL_WARN,
                            ER_BINLOG_PURGE_FATAL_ERR,
                            "a problem with deleting %s; "
                            "consider examining correspondence "
                            "of your binlog index file "
                            "to the actual binlog files",
                            linfo.log_file_name);
        error= 1;
        goto err;
      }
    }
    if (find_next_log(&linfo, 0))
      break;
  }

  /* Start logging with a new file */
  close(LOG_CLOSE_INDEX | LOG_CLOSE_TO_BE_OPENED);
  if ((error= my_delete_allow_opened(index_file_name, MYF(0))))	// Reset (open will update)
  {
    if (my_errno == ENOENT) 
    {
      push_warning_printf(current_thd, MYSQL_ERROR::WARN_LEVEL_WARN,
                          ER_LOG_PURGE_NO_FILE, ER(ER_LOG_PURGE_NO_FILE),
                          index_file_name);
      sql_print_information("Failed to delete file '%s'",
                            index_file_name);
      my_errno= 0;
      error= 0;
    }
    else
    {
      push_warning_printf(current_thd, MYSQL_ERROR::WARN_LEVEL_WARN,
                          ER_BINLOG_PURGE_FATAL_ERR,
                          "a problem with deleting %s; "
                          "consider examining correspondence "
                          "of your binlog index file "
                          "to the actual binlog files",
                          index_file_name);
      error= 1;
      goto err;
    }
  }
  if (!thd->slave_thread)
    need_start_event=1;
  if (!open_index_file(index_file_name, 0, FALSE))
    if ((error= open(save_name, log_type, 0, io_cache_type, no_auto_events, max_size, 0, FALSE)))
      goto err;
  my_free((uchar*) save_name, MYF(0));

err:
  VOID(pthread_mutex_unlock(&LOCK_thread_count));
  if (need_lock)
  {
    pthread_mutex_unlock(&LOCK_index);
    pthread_mutex_unlock(&LOCK_log);
  }
  DBUG_RETURN(error);
}


/**
  Delete relay log files prior to rli->group_relay_log_name
  (i.e. all logs which are not involved in a non-finished group
  (transaction)), remove them from the index file and start on next
  relay log.

  IMPLEMENTATION
  - Protects index file with LOCK_index
  - Delete relevant relay log files
  - Copy all file names after these ones to the front of the index file
  - If the OS has truncate, truncate the file, else fill it with \n'
  - Read the next file name from the index file and store in rli->linfo

  @param rli	       Relay log information
  @param included     If false, all relay logs that are strictly before
                      rli->group_relay_log_name are deleted ; if true, the
                      latter is deleted too (i.e. all relay logs
                      read by the SQL slave thread are deleted).

  @note
    - This is only called from the slave-execute thread when it has read
    all commands from a relay log and want to switch to a new relay log.
    - When this happens, we can be in an active transaction as
    a transaction can span over two relay logs
    (although it is always written as a single block to the master's binary
    log, hence cannot span over two master's binary logs).

  @retval
    0			ok
  @retval
    LOG_INFO_EOF	        End of log-index-file found
  @retval
    LOG_INFO_SEEK	Could not allocate IO cache
  @retval
    LOG_INFO_IO		Got IO error while reading file
*/

#ifdef HAVE_REPLICATION

int MYSQL_BIN_LOG::purge_first_log(Relay_log_info* rli, bool included)
{
  int error;
  char *to_purge_if_included= NULL;
  DBUG_ENTER("purge_first_log");

  DBUG_ASSERT(is_open());
  DBUG_ASSERT(rli->slave_running == 1);
  DBUG_ASSERT(!strcmp(rli->linfo.log_file_name,rli->event_relay_log_name));

  pthread_mutex_lock(&LOCK_index);
  to_purge_if_included= my_strdup(rli->group_relay_log_name, MYF(0));

  /*
    Read the next log file name from the index file and pass it back to
    the caller.
  */
  if((error=find_log_pos(&rli->linfo, rli->event_relay_log_name, 0)) || 
     (error=find_next_log(&rli->linfo, 0)))
  {
    char buff[22];
    sql_print_error("next log error: %d  offset: %s  log: %s included: %d",
                    error,
                    llstr(rli->linfo.index_file_offset,buff),
                    rli->event_relay_log_name,
                    included);
    goto err;
  }

  /*
    Reset rli's coordinates to the current log.
  */
  rli->event_relay_log_pos= BIN_LOG_HEADER_SIZE;
  strmake(rli->event_relay_log_name,rli->linfo.log_file_name,
	  sizeof(rli->event_relay_log_name)-1);

  /*
    If we removed the rli->group_relay_log_name file,
    we must update the rli->group* coordinates, otherwise do not touch it as the
    group's execution is not finished (e.g. COMMIT not executed)
  */
  if (included)
  {
    rli->group_relay_log_pos = BIN_LOG_HEADER_SIZE;
    strmake(rli->group_relay_log_name,rli->linfo.log_file_name,
            sizeof(rli->group_relay_log_name)-1);
    rli->notify_group_relay_log_name_update();
  }

  /* Store where we are in the new file for the execution thread */
  flush_relay_log_info(rli);

  DBUG_EXECUTE_IF("crash_before_purge_logs", DBUG_SUICIDE(););

  pthread_mutex_lock(&rli->log_space_lock);
  rli->relay_log.purge_logs(to_purge_if_included, included,
                            0, 0, &rli->log_space_total);
  pthread_mutex_unlock(&rli->log_space_lock);

  /*
    Ok to broadcast after the critical region as there is no risk of
    the mutex being destroyed by this thread later - this helps save
    context switches
  */
  pthread_cond_broadcast(&rli->log_space_cond);

  /*
   * Need to update the log pos because purge logs has been called 
   * after fetching initially the log pos at the begining of the method.
   */
  if((error=find_log_pos(&rli->linfo, rli->event_relay_log_name, 0)))
  {
    char buff[22];
    sql_print_error("next log error: %d  offset: %s  log: %s included: %d",
                    error,
                    llstr(rli->linfo.index_file_offset,buff),
                    rli->group_relay_log_name,
                    included);
    goto err;
  }

  /* If included was passed, rli->linfo should be the first entry. */
  DBUG_ASSERT(!included || rli->linfo.index_file_start_offset == 0);

err:
  my_free(to_purge_if_included, MYF(0));
  pthread_mutex_unlock(&LOCK_index);
  DBUG_RETURN(error);
}

/**
  Update log index_file.
*/

int MYSQL_BIN_LOG::update_log_index(LOG_INFO* log_info, bool need_update_threads)
{
  if (copy_up_file_and_fill(&index_file, log_info->index_file_start_offset))
    return LOG_INFO_IO;

  // now update offsets in index file for running threads
  if (need_update_threads)
    adjust_linfo_offsets(log_info->index_file_start_offset);
  return 0;
}

/**
  Remove all logs before the given log from disk and from the index file.

  @param to_log	      Delete all log file name before this file.
  @param included            If true, to_log is deleted too.
  @param need_mutex
  @param need_update_threads If we want to update the log coordinates of
                             all threads. False for relay logs, true otherwise.
  @param freed_log_space     If not null, decrement this variable of
                             the amount of log space freed

  @note
    If any of the logs before the deleted one is in use,
    only purge logs up to this one.

  @retval
    0			ok
  @retval
    LOG_INFO_EOF		to_log not found
    LOG_INFO_EMFILE             too many files opened
    LOG_INFO_FATAL              if any other than ENOENT error from
                                my_stat() or my_delete()
*/

int MYSQL_BIN_LOG::purge_logs(const char *to_log, 
                          bool included,
                          bool need_mutex, 
                          bool need_update_threads, 
                          ulonglong *decrease_log_space)
{
  int error= 0;
  bool exit_loop= 0;
  LOG_INFO log_info;
  THD *thd= current_thd;
  DBUG_ENTER("purge_logs");
  DBUG_PRINT("info",("to_log= %s",to_log));

  if (need_mutex)
    pthread_mutex_lock(&LOCK_index);
  if ((error=find_log_pos(&log_info, to_log, 0 /*no mutex*/))) 
  {
    sql_print_error("MYSQL_BIN_LOG::purge_logs was called with file %s not "
                    "listed in the index.", to_log);
    goto err;
  }

  if ((error= open_purge_index_file(TRUE)))
  {
    sql_print_error("MYSQL_BIN_LOG::purge_logs failed to sync the index file.");
    goto err;
  }

  /*
    File name exists in index file; delete until we find this file
    or a file that is used.
  */
  if ((error=find_log_pos(&log_info, NullS, 0 /*no mutex*/)))
    goto err;
  while ((strcmp(to_log,log_info.log_file_name) || (exit_loop=included)) &&
         !is_active(log_info.log_file_name) &&
         !log_in_use(log_info.log_file_name))
  {
    if ((error= register_purge_index_entry(log_info.log_file_name)))
    {
      sql_print_error("MYSQL_BIN_LOG::purge_logs failed to copy %s to register file.",
                      log_info.log_file_name);
      goto err;
    }

    if (find_next_log(&log_info, 0) || exit_loop)
      break;
  }

  DBUG_EXECUTE_IF("crash_purge_before_update_index", DBUG_SUICIDE(););

  if ((error= sync_purge_index_file()))
  {
    sql_print_error("MSYQL_BIN_LOG::purge_logs failed to flush register file.");
    goto err;
  }

  /* We know how many files to delete. Update index file. */
  if ((error=update_log_index(&log_info, need_update_threads)))
  {
    sql_print_error("MSYQL_BIN_LOG::purge_logs failed to update the index file");
    goto err;
  }

  DBUG_EXECUTE_IF("crash_purge_critical_after_update_index", DBUG_SUICIDE(););

err:
  /* Read each entry from purge_index_file and delete the file. */
  if (is_inited_purge_index_file() &&
      (error= purge_index_entry(thd, decrease_log_space, FALSE)))
    sql_print_error("MSYQL_BIN_LOG::purge_logs failed to process registered files"
                    " that would be purged.");
  close_purge_index_file();

  DBUG_EXECUTE_IF("crash_purge_non_critical_after_update_index", DBUG_SUICIDE(););

  if (need_mutex)
    pthread_mutex_unlock(&LOCK_index);
  DBUG_RETURN(error);
}

int MYSQL_BIN_LOG::set_purge_index_file_name(const char *base_file_name)
{
  int error= 0;
  DBUG_ENTER("MYSQL_BIN_LOG::set_purge_index_file_name");
  if (fn_format(purge_index_file_name, base_file_name, mysql_data_home,
                ".~rec~", MYF(MY_UNPACK_FILENAME | MY_SAFE_PATH |
                              MY_REPLACE_EXT)) == NULL)
  {
    error= 1;
    sql_print_error("MYSQL_BIN_LOG::set_purge_index_file_name failed to set "
                      "file name.");
  }
  DBUG_RETURN(error);
}

int MYSQL_BIN_LOG::open_purge_index_file(bool destroy)
{
  int error= 0;
  File file= -1;

  DBUG_ENTER("MYSQL_BIN_LOG::open_purge_index_file");

  if (destroy)
    close_purge_index_file();

  if (!my_b_inited(&purge_index_file))
  {
    if ((file= my_open(purge_index_file_name, O_RDWR | O_CREAT | O_BINARY,
                       MYF(MY_WME | ME_WAITTANG))) < 0  ||
        init_io_cache(&purge_index_file, file, IO_SIZE,
                      (destroy ? WRITE_CACHE : READ_CACHE),
                      0, 0, MYF(MY_WME | MY_NABP | MY_WAIT_IF_FULL)))
    {
      error= 1;
      sql_print_error("MYSQL_BIN_LOG::open_purge_index_file failed to open register "
                      " file.");
    }
  }
  DBUG_RETURN(error);
}

int MYSQL_BIN_LOG::close_purge_index_file()
{
  int error= 0;

  DBUG_ENTER("MYSQL_BIN_LOG::close_purge_index_file");

  if (my_b_inited(&purge_index_file))
  {
    end_io_cache(&purge_index_file);
    error= my_close(purge_index_file.file, MYF(0));
  }
  my_delete(purge_index_file_name, MYF(0));
  bzero((char*) &purge_index_file, sizeof(purge_index_file));

  DBUG_RETURN(error);
}

bool MYSQL_BIN_LOG::is_inited_purge_index_file()
{
  DBUG_ENTER("MYSQL_BIN_LOG::is_inited_purge_index_file");
  DBUG_RETURN (my_b_inited(&purge_index_file));
}

int MYSQL_BIN_LOG::sync_purge_index_file()
{
  int error= 0;
  DBUG_ENTER("MYSQL_BIN_LOG::sync_purge_index_file");

  if ((error= flush_io_cache(&purge_index_file)) ||
      (error= my_sync(purge_index_file.file, MYF(MY_WME))))
    DBUG_RETURN(error);

  DBUG_RETURN(error);
}

int MYSQL_BIN_LOG::register_purge_index_entry(const char *entry)
{
  int error= 0;
  DBUG_ENTER("MYSQL_BIN_LOG::register_purge_index_entry");

  if ((error=my_b_write(&purge_index_file, (const uchar*)entry, strlen(entry))) ||
      (error=my_b_write(&purge_index_file, (const uchar*)"\n", 1)))
    DBUG_RETURN (error);

  DBUG_RETURN(error);
}

int MYSQL_BIN_LOG::register_create_index_entry(const char *entry)
{
  DBUG_ENTER("MYSQL_BIN_LOG::register_create_index_entry");
  DBUG_RETURN(register_purge_index_entry(entry));
}

int MYSQL_BIN_LOG::purge_index_entry(THD *thd, ulonglong *decrease_log_space,
                                     bool need_mutex)
{
  MY_STAT s;
  int error= 0;
  LOG_INFO log_info;
  LOG_INFO check_log_info;

  DBUG_ENTER("MYSQL_BIN_LOG:purge_index_entry");

  DBUG_ASSERT(my_b_inited(&purge_index_file));

  if ((error=reinit_io_cache(&purge_index_file, READ_CACHE, 0, 0, 0)))
  {
    sql_print_error("MSYQL_BIN_LOG::purge_index_entry failed to reinit register file "
                    "for read");
    goto err;
  }

  for (;;)
  {
    uint length;

    if ((length=my_b_gets(&purge_index_file, log_info.log_file_name,
                          FN_REFLEN)) <= 1)
    {
      if (purge_index_file.error)
      {
        error= purge_index_file.error;
        sql_print_error("MSYQL_BIN_LOG::purge_index_entry error %d reading from "
                        "register file.", error);
        goto err;
      }

      /* Reached EOF */
      break;
    }

    /* Get rid of the trailing '\n' */
    log_info.log_file_name[length-1]= 0;

    if (!my_stat(log_info.log_file_name, &s, MYF(0)))
    {
      if (my_errno == ENOENT) 
      {
        /*
          It's not fatal if we can't stat a log file that does not exist;
          If we could not stat, we won't delete.
        */
        if (thd)
        {
          push_warning_printf(thd, MYSQL_ERROR::WARN_LEVEL_WARN,
                              ER_LOG_PURGE_NO_FILE, ER(ER_LOG_PURGE_NO_FILE),
                              log_info.log_file_name);
        }
        sql_print_information("Failed to execute my_stat on file '%s'",
			      log_info.log_file_name);
        my_errno= 0;
      }
      else
      {
        /*
          Other than ENOENT are fatal
        */
        if (thd)
        {
          push_warning_printf(thd, MYSQL_ERROR::WARN_LEVEL_WARN,
                              ER_BINLOG_PURGE_FATAL_ERR,
                              "a problem with getting info on being purged %s; "
                              "consider examining correspondence "
                              "of your binlog index file "
                              "to the actual binlog files",
                              log_info.log_file_name);
        }
        else
        {
          sql_print_information("Failed to delete log file '%s'; "
                                "consider examining correspondence "
                                "of your binlog index file "
                                "to the actual binlog files",
                                log_info.log_file_name);
        }
        error= LOG_INFO_FATAL;
        goto err;
      }
    }
    else
    {
      if ((error= find_log_pos(&check_log_info, log_info.log_file_name, need_mutex)))
      {
        if (error != LOG_INFO_EOF)
        {
          if (thd)
          {
            push_warning_printf(thd, MYSQL_ERROR::WARN_LEVEL_WARN,
                                ER_BINLOG_PURGE_FATAL_ERR,
                                "a problem with deleting %s and "
                                "reading the binlog index file",
                                log_info.log_file_name);
          }
          else
          {
            sql_print_information("Failed to delete file '%s' and "
                                  "read the binlog index file",
                                  log_info.log_file_name);
          }
          goto err;
        }
           
        error= 0;
        if (!need_mutex)
        {
          /*
            This is to avoid triggering an error in NDB.
          */
          ha_binlog_index_purge_file(current_thd, log_info.log_file_name);
        }

        DBUG_PRINT("info",("purging %s",log_info.log_file_name));
        if (!my_delete(log_info.log_file_name, MYF(0)))
        {
          if (decrease_log_space)
            *decrease_log_space-= s.st_size;
        }
        else
        {
          if (my_errno == ENOENT)
          {
            if (thd)
            {
              push_warning_printf(thd, MYSQL_ERROR::WARN_LEVEL_WARN,
                                  ER_LOG_PURGE_NO_FILE, ER(ER_LOG_PURGE_NO_FILE),
                                  log_info.log_file_name);
            }
            sql_print_information("Failed to delete file '%s'",
                                  log_info.log_file_name);
            my_errno= 0;
          }
          else
          {
            if (thd)
            {
              push_warning_printf(thd, MYSQL_ERROR::WARN_LEVEL_WARN,
                                  ER_BINLOG_PURGE_FATAL_ERR,
                                  "a problem with deleting %s; "
                                  "consider examining correspondence "
                                  "of your binlog index file "
                                  "to the actual binlog files",
                                  log_info.log_file_name);
            }
            else
            {
              sql_print_information("Failed to delete file '%s'; "
                                    "consider examining correspondence "
                                    "of your binlog index file "
                                    "to the actual binlog files",
                                    log_info.log_file_name);
            }
            if (my_errno == EMFILE)
            {
              DBUG_PRINT("info",
                         ("my_errno: %d, set ret = LOG_INFO_EMFILE", my_errno));
              error= LOG_INFO_EMFILE;
              goto err;
            }
            error= LOG_INFO_FATAL;
            goto err;
          }
        }
      }
    }
  }

err:
  DBUG_RETURN(error);
}

/**
  Remove all logs before the given file date from disk and from the
  index file.

  @param thd		Thread pointer
  @param purge_time	Delete all log files before given date.

  @note
    If any of the logs before the deleted one is in use,
    only purge logs up to this one.

  @retval
    0				ok
  @retval
    LOG_INFO_PURGE_NO_ROTATE	Binary file that can't be rotated
    LOG_INFO_FATAL              if any other than ENOENT error from
                                my_stat() or my_delete()
*/

int MYSQL_BIN_LOG::purge_logs_before_date(time_t purge_time)
{
  int error;
  char to_log[FN_REFLEN];
  LOG_INFO log_info;
  MY_STAT stat_area;
  THD *thd= current_thd;
  
  DBUG_ENTER("purge_logs_before_date");

  pthread_mutex_lock(&LOCK_index);
  to_log[0]= 0;

  if ((error=find_log_pos(&log_info, NullS, 0 /*no mutex*/)))
    goto err;

  while (strcmp(log_file_name, log_info.log_file_name) &&
	 !is_active(log_info.log_file_name) &&
         !log_in_use(log_info.log_file_name))
  {
    if (!my_stat(log_info.log_file_name, &stat_area, MYF(0)))
    {
      if (my_errno == ENOENT) 
      {
        /*
          It's not fatal if we can't stat a log file that does not exist.
        */
        my_errno= 0;
      }
      else
      {
        /*
          Other than ENOENT are fatal
        */
        if (thd)
        {
          push_warning_printf(thd, MYSQL_ERROR::WARN_LEVEL_WARN,
                              ER_BINLOG_PURGE_FATAL_ERR,
                              "a problem with getting info on being purged %s; "
                              "consider examining correspondence "
                              "of your binlog index file "
                              "to the actual binlog files",
                              log_info.log_file_name);
        }
        else
        {
          sql_print_information("Failed to delete log file '%s'",
                                log_info.log_file_name);
        }
        error= LOG_INFO_FATAL;
        goto err;
      }
    }
    else
    {
      if (stat_area.st_mtime < purge_time) 
        strmake(to_log, 
                log_info.log_file_name, 
                sizeof(log_info.log_file_name) - 1);
      else
        break;
    }
    if (find_next_log(&log_info, 0))
      break;
  }

  error= (to_log[0] ? purge_logs(to_log, 1, 0, 1, (ulonglong *) 0) : 0);

err:
  pthread_mutex_unlock(&LOCK_index);
  DBUG_RETURN(error);
}
#endif /* HAVE_REPLICATION */


/**
  Create a new log file name.

  @param buf		buf of at least FN_REFLEN where new name is stored

  @note
    If file name will be longer then FN_REFLEN it will be truncated
*/

void MYSQL_BIN_LOG::make_log_name(char* buf, const char* log_ident)
{
  uint dir_len = dirname_length(log_file_name); 
  if (dir_len >= FN_REFLEN)
    dir_len=FN_REFLEN-1;
  strnmov(buf, log_file_name, dir_len);
  strmake(buf+dir_len, log_ident, FN_REFLEN - dir_len -1);
}


/**
  Check if we are writing/reading to the given log file.
*/

bool MYSQL_BIN_LOG::is_active(const char *log_file_name_arg)
{
  return !strcmp(log_file_name, log_file_name_arg);
}


/*
  Wrappers around new_file_impl to avoid using argument
  to control locking. The argument 1) less readable 2) breaks
  incapsulation 3) allows external access to the class without
  a lock (which is not possible with private new_file_without_locking
  method).

  @retval
    nonzero - error
*/

int MYSQL_BIN_LOG::new_file()
{
  return new_file_impl(1);
}

/*
  @retval
    nonzero - error
 */
int MYSQL_BIN_LOG::new_file_without_locking()
{
  return new_file_impl(0);
}


/**
  Start writing to a new log file or reopen the old file.

  @param need_lock		Set to 1 if caller has not locked LOCK_log

  @retval
    nonzero - error

  @note
    The new file name is stored last in the index file
*/

int MYSQL_BIN_LOG::new_file_impl(bool need_lock)
{
  int error= 0, close_on_error= FALSE;
  char new_name[FN_REFLEN], *new_name_ptr, *old_name, *file_to_open;

  DBUG_ENTER("MYSQL_BIN_LOG::new_file_impl");
  if (!is_open())
  {
    DBUG_PRINT("info",("log is closed"));
    DBUG_RETURN(error);
  }

  if (need_lock)
    pthread_mutex_lock(&LOCK_log);
  pthread_mutex_lock(&LOCK_index);

  safe_mutex_assert_owner(&LOCK_log);
  safe_mutex_assert_owner(&LOCK_index);

  /* As LOCK_log can be released below, stop_new_xids is used to prevent
     1) increments of prepared_xids
     2) new_file_impl from running concurrently
  */
  if (stop_new_xids)
  {
    DBUG_ASSERT(!stop_new_xids);
    sql_print_error("new_file_impl called concurrently");

    pthread_mutex_unlock(&LOCK_index);

    while (stop_new_xids)
      pthread_cond_wait(&COND_stop_xids, &LOCK_log);
      
    pthread_mutex_lock(&LOCK_index);
  }
  stop_new_xids= TRUE;

  /*
    if binlog is used as tc log, be sure all xids are "unlogged",
    so that on recover we only need to scan one - latest - binlog file
    for prepared xids. As this is expected to be a rare event,
    simple wait strategy is enough.

    If this slept while holding LOCK_log then the transactions it is waiting
    for to finish might get stuck if they were sleeping in flush_and_sync.
    When waking in flush_and_sync they would not be able to lock LOCK_log.
    The workaround is to set stop_new_xids before unlocking LOCK_log while
    waiting.
  */
  if (prepared_xids)
  {
    tc_log_page_waits++;
    pthread_mutex_lock(&LOCK_prep_xids);

    pthread_mutex_unlock(&LOCK_index);
    pthread_mutex_unlock(&LOCK_log);

    while (prepared_xids) {
      DBUG_PRINT("info", ("prepared_xids=%lu", prepared_xids));
      pthread_cond_wait(&COND_prep_xids, &LOCK_prep_xids);
    }
    pthread_mutex_unlock(&LOCK_prep_xids);
    pthread_mutex_lock(&LOCK_log);
    pthread_mutex_lock(&LOCK_index);

    DBUG_ASSERT(!prepared_xids);
  }

  /* Reuse old name if not binlog and not update log */
  new_name_ptr= name;

  /*
    If user hasn't specified an extension, generate a new log name
    We have to do this here and not in open as we want to store the
    new file name in the current binary log file.
  */
  if ((error= generate_new_name(new_name, name)))
    goto end;
  new_name_ptr=new_name;

  if (log_type == LOG_BIN)
  {
    if (!no_auto_events)
    {
      /*
        We log the whole file name for log file as the user may decide
        to change base names at some point.
      */
      Rotate_log_event r(new_name+dirname_length(new_name),
                         0, LOG_EVENT_OFFSET, is_relay_log ? Rotate_log_event::RELAY_LOG : 0);
      if(DBUG_EVALUATE_IF("fault_injection_new_file_rotate_event", (error=close_on_error=TRUE), FALSE) ||
         (error= r.write(&log_file)))
      {
        DBUG_EXECUTE_IF("fault_injection_new_file_rotate_event", errno=2;);
        close_on_error= TRUE;
        my_printf_error(ER_ERROR_ON_WRITE, ER(ER_CANT_OPEN_FILE), MYF(ME_FATALERROR), name, errno);
        goto end;
      }
      bytes_written += r.data_written;
    }
    /*
      Update needs to be signalled even if there is no rotate event
      log rotation should give the waiting thread a signal to
      discover EOF and move on to the next log.
    */
    signal_update();
  }
  old_name=name;
  name=0;				// Don't free name
  close(LOG_CLOSE_TO_BE_OPENED | LOG_CLOSE_INDEX);

  /*
     Note that at this point, log_state != LOG_CLOSED (important for is_open()).
  */

  /*
     new_file() is only used for rotation (in FLUSH LOGS or because size >
     max_binlog_size or max_relay_log_size).
     If this is a binary log, the Format_description_log_event at the beginning of
     the new file should have created=0 (to distinguish with the
     Format_description_log_event written at server startup, which should
     trigger temp tables deletion on slaves.
  */

  /* reopen index binlog file, BUG#34582 */
  file_to_open= index_file_name;
  error= open_index_file(index_file_name, 0, FALSE);
  if (!error)
  {
    /* reopen the binary log file. */
    file_to_open= new_name_ptr;
    error= open(old_name, log_type, new_name_ptr, io_cache_type,
                no_auto_events, max_size, 1, FALSE);
  }

  /* handle reopening errors */
  if (error)
  {
    my_printf_error(ER_CANT_OPEN_FILE, ER(ER_CANT_OPEN_FILE), 
                    MYF(ME_FATALERROR), file_to_open, error);
    close_on_error= TRUE;
  }

  my_free(old_name,MYF(0));

end:

  stop_new_xids= FALSE;
  pthread_cond_broadcast(&COND_stop_xids);

  if (error && close_on_error /* rotate or reopen failed */)
  {
    /* 
      Close whatever was left opened.

      We are keeping the behavior as it exists today, ie,
      we disable logging and move on (see: BUG#51014).

      TODO: as part of WL#1790 consider other approaches:
       - kill mysql (safety);
       - try multiple locations for opening a log file;
       - switch server to protected/readonly mode
       - ...
    */
    close(LOG_CLOSE_INDEX);
    sql_print_error("Could not open %s for logging (error %d). "
                     "Turning logging off for the whole duration "
                     "of the MySQL server process. To turn it on "
                     "again: fix the cause, shutdown the MySQL "
                     "server and restart it.", 
                     new_name_ptr, errno);
  }

  if (need_lock)
    pthread_mutex_unlock(&LOCK_log);
  pthread_mutex_unlock(&LOCK_index);

  DBUG_RETURN(error);
}


bool MYSQL_BIN_LOG::append(Log_event* ev)
{
  bool error = 0;
  USER_STATS *us= current_thd ? thd_get_user_stats(current_thd) : NULL;

  pthread_mutex_lock(&LOCK_log);
  DBUG_ENTER("MYSQL_BIN_LOG::append");

  DBUG_ASSERT(log_file.type == SEQ_READ_APPEND);
  /*
    Log_event::write() is smart enough to use my_b_write() or
    my_b_append() depending on the kind of cache we have.
  */
  if (ev->write(&log_file))
  {
    error=1;
    goto err;
  }
  bytes_written+= ev->data_written;
  binlog_bytes_written += ev->data_written;
  if (us)
  {
    us->binlog_bytes_written += ev->data_written;
  }

  DBUG_PRINT("info",("max_size: %lu",max_size));

  /* A call to ::new_file_impl is in progress when stop_new_xids==TRUE.
     I don't think this needs to wait to force a rotate.
  */
  if (!stop_new_xids && (uint) my_b_append_tell(&log_file) > max_size)
    error= new_file_without_locking();

err:
  pthread_mutex_unlock(&LOCK_log);
  signal_update();				// Safe as we don't call close
  DBUG_RETURN(error);
}


bool MYSQL_BIN_LOG::appendv(bool* newfile, const char* buf, uint len,...)
{
  bool error= 0;
  USER_STATS *us= current_thd ? thd_get_user_stats(current_thd) : NULL;
  DBUG_ENTER("MYSQL_BIN_LOG::appendv");
  va_list(args);
  va_start(args,len);

  DBUG_ASSERT(log_file.type == SEQ_READ_APPEND);
  *newfile= FALSE;

  safe_mutex_assert_owner(&LOCK_log);
  do
  {
    if (my_b_append(&log_file,(uchar*) buf,len))
    {
      error= 1;
      goto err;
    }
    bytes_written += len;
    binlog_bytes_written += len;
    if (us)
    {
      us->binlog_bytes_written += len;
    }

  } while ((buf=va_arg(args,const char*)) && (len=va_arg(args,uint)));
  DBUG_PRINT("info",("max_size: %lu",max_size));

  /* A call to ::new_file_impl is in progress when stop_new_xids==TRUE
     I don't think this needs to wait to force a rotate.
  */
  if (!stop_new_xids && (uint) my_b_append_tell(&log_file) > max_size)
  {
    error= new_file_without_locking();
    *newfile= TRUE;
  }

err:
  if (!error)
    signal_update();
  DBUG_RETURN(error);
}

void MYSQL_BIN_LOG::disable_group_commit(THD *thd, const char* msg)
{
  /* 
     Disable group commit forever because a bug has been hit. I prefer
     to not crash a production server via assert.
  */
  group_commit_allowed= FALSE;

  sql_print_error("Group commit disabled because a bug has been found. "
                  "Ticket values: current(%lu), next(%lu), thd(%lu). %s",
                  (unsigned long) current_ticket,
                  (unsigned long) next_ticket,
                  (unsigned long) thd->ticket,
                  msg);
}

/**
 * Remember order in which XID events are written to the binlog.
 * @return 0 if this transaction is to be ordered and transaction log commit
 * record will match binlog XID event order.
 */
int MYSQL_BIN_LOG::order_for_group_commit(THD *thd, handlerton *ht)
{
  DBUG_ASSERT(!thd->ticket);
  DBUG_EXECUTE_IF("group_commit_already_set", thd->ticket= 1; );

  if (thd->ticket)
  {
    disable_group_commit(thd, "ticket already set");
    return 1;
  }

  if (!group_commit_allowed ||
      !force_binlog_order ||
      !ht ||
      !ht->is_ordered_commit(ht, thd))
    return 1;

  pthread_mutex_lock(&LOCK_group_commit);
  thd->ticket = next_ticket++;
  pthread_mutex_unlock(&LOCK_group_commit);

  DBUG_EXECUTE_IF("group_commit_rollover", {
    current_ticket= 0xFFFFFFFFFFFFFFFFULL;
    next_ticket= current_ticket + 1;
    thd->ticket= current_ticket; });

  if ((thd->ticket + 1) == 0)
  {
    disable_group_commit(thd, "ticket rolled over");
    return 1;
  }

  return 0;
}

/**
 * Increment current ticket and let other waiting threads
 * know that it may be their turn in the queue
 */
void MYSQL_BIN_LOG::increment_group_commit_ticket(THD* thd)
{
  int slot;

  if (!thd->ticket)
  {
    /*
       The caller of this might not know whether an error prevented
       it from being called during commit processing.
    */
    return;
  }

  DBUG_EXECUTE_IF("group_commit_increment_bad_state", --thd->ticket; );

  /*
    Wakeup the threads sleeping on the NEXT slot
  */
  slot = (1 + thd->ticket) % NUM_BINLOG_COMMIT_COND;

  pthread_mutex_lock(&LOCK_group_commit);

  if (thd->ticket != current_ticket)
    disable_group_commit(thd, "ticket != current on increment");

  ++current_ticket;
  pthread_cond_broadcast(&binlog_commit_cond_array[slot]);
  pthread_mutex_unlock(&LOCK_group_commit);
  thd->ticket = 0;
}

/**
 * Will not return until it is the current thread's turn.
 *
 * Ensures that handlerton::commit_fast is called in the same order as
 * XID events are written to the binlog.
 */
void MYSQL_BIN_LOG::wait_for_group_commit_order(THD *thd)
{
  ulonglong initial_ticket;
  timespec cond_wake_time;
  my_bool first_loop= TRUE;
  my_bool first_err= FALSE;
  my_bool first_log= FALSE;
  my_fast_timer_t wait_start;
  double wait_secs;

  DBUG_EXECUTE_IF("group_commit_wait_bad_state", thd->ticket= 0; );

  if (!thd->ticket)
  {
    disable_group_commit(thd, "ticket not set before wait_for_group_commit_order");
    return;
  }

  thd_proc_info(thd, "wait for group commit order");

  DEBUG_SYNC(thd, "on_group_commit_dequeue");

  set_timespec(cond_wake_time, 1);
  my_get_fast_timer(&wait_start);

  pthread_mutex_lock(&LOCK_group_commit);
  initial_ticket = current_ticket;

  DBUG_EXECUTE_IF("group_commit_long_wait", goto wait_loop; );
  DBUG_EXECUTE_IF("group_commit_really_long_wait", goto wait_loop; );

  /*
    thd->ticket is initialized to zero when a new thd is created and is set
    for transactions in which commit will be ordered for real group commit.
    current_ticket starts at 1 and only increases.
  */
  while (group_commit_allowed &&
         thd->ticket > current_ticket)
  {
    int slot, err;

#ifndef DBUG_OFF
wait_loop:
#endif

    if (!first_loop)
    {
      set_timespec(cond_wake_time, 1);
    }
    {
      first_loop= FALSE;
    }

    slot = thd->ticket % NUM_BINLOG_COMMIT_COND;
    err = pthread_cond_timedwait(&binlog_commit_cond_array[slot],
                                 &LOCK_group_commit, &cond_wake_time);

    DBUG_EXECUTE_IF("group_commit_long_wait", {
        wait_secs= 2;
        goto long_wait;} );
    DBUG_EXECUTE_IF("group_commit_really_long_wait", {
        wait_secs= group_commit_hang_disable_secs+1;
        goto really_long_wait;} );

    if (err == ETIMEDOUT)
    {
      /*
        Cannot assume ETIMEDOUT implies this waited for cond_wake_time
      */
      wait_secs = my_fast_timer_diff_now(&wait_start, NULL);

      if (wait_secs > group_commit_hang_disable_secs)
      {
#ifndef DBUG_OFF
really_long_wait:
#endif
        /*
          Will exit the loop because group_commit_allowed is set to FALSE
         */
        disable_group_commit(thd, "waited too long for ticket");
        ++binlog_fsync_reallylongwait;
      }
      else if (!first_err && wait_secs > 0.9)
      {
        /* 
          wait_secs might be just less than 1 second 
        */
#ifndef DBUG_OFF
long_wait:
#endif
        first_err= TRUE;
        ++binlog_fsync_longwait;
      }

      if (!first_log && wait_secs > group_commit_hang_log_secs)
      {
        first_log= TRUE;

        sql_print_error("Group commit: %ld start waiting for ticket %lu to reach %lu for "
                        "%lu microseconds, initial ticket was %lu",
                        (unsigned long) thd->variables.pseudo_thread_id,
                        (unsigned long) current_ticket,
                        (unsigned long) thd->ticket,
                        (unsigned long) (1000000.0 * wait_secs),
                        (unsigned long) initial_ticket);
      }

    }
  }
  pthread_mutex_unlock(&LOCK_group_commit);

  wait_secs = my_fast_timer_diff_now(&wait_start, NULL);
  binlog_fsync_ticketwait_secs += wait_secs;
  ++binlog_fsync_ticketwaits;

  if (wait_secs > group_commit_hang_log_secs)
  {
    sql_print_error("Group commit: %ld done waiting for ticket to reach %lu for "
                    "%lu microseconds, initial ticket was %lu",
                    (unsigned long) thd->variables.pseudo_thread_id,
                    (unsigned long) thd->ticket,
                    (unsigned long) (1000000.0 * wait_secs),
                    (unsigned long) initial_ticket);
  }
}

bool MYSQL_BIN_LOG::flush_and_sync(THD *thd, bool async, handlerton *ht, int32 pending)
{
  int err=0;
  my_fast_timer_t fsync_start;
  double fsync_time;
  ulong sync_period= sync_binlog_period;
  bool group_commit_on= FALSE;
  int32 min_size= (int32) group_commit_min_size;

  safe_mutex_assert_owner(&LOCK_log);

  if (flush_io_cache(&log_file))
    return 1;

  thd_proc_info(thd, "flush and sync binlog");

  if (!async && (++sync_binlog_counter >= sync_period && sync_period))
  {
    /*
      Cache because it is referenced twice and could change in between.
    */
    ulong timeout_usecs = group_commit_timeout_usecs;

    /*
      Waiting counts the number of threads trying to share one binlog fsync.
      Threads don't wait and immediately call fsync when enough are waiting.
      Without the check on this, all can wait for the group commit timeout
      and that makes commit slow.
    */
    static int32 waiting= 0;
    bool enough_pending, not_too_many_waiting;

    DBUG_EXECUTE_IF("error_in_flush_and_sync_before", return 1; );

    /*
      Remember the order in which XID events are written. This must be called
      before LOCK_log is released.
    */
    if (!order_for_group_commit(thd, ht))
    {
      group_commit_on= TRUE;
      DBUG_ASSERT(thd->ticket);
    }
    else
    {
      ++binlog_fsync_notry;
    }

    DBUG_EXECUTE_IF("crash_before_group_commit", abort(););
    DBUG_EXECUTE_IF("error_in_flush_and_sync_after", return 1; );

    enough_pending= pending >= min_size;
    binlog_fsync_enough_pending += enough_pending;

    not_too_many_waiting= enough_pending && (waiting < (pending / 2));
    binlog_fsync_not_too_many_waiting += not_too_many_waiting;

    if (group_commit_on && enough_pending && not_too_many_waiting)
    {
      my_fast_timer_t wait_start;
      ulonglong my_fsync_count = binlog_fsync_count;
      timespec cond_wake_time;

      DBUG_EXECUTE_IF("binlog_fsync_signal",
                      {  pthread_mutex_unlock(&LOCK_log);
                         DEBUG_SYNC(thd, "wait_for_binlog_fsync_signal");
                         pthread_mutex_lock(&LOCK_log);
                      });
      DEBUG_SYNC(thd, "before_binlog_sync");

      ++binlog_fsync_wait;

      set_timespec_nsec(cond_wake_time, timeout_usecs * 1000);

      thd_proc_info(thd, "flush and sync binlog : wait for gc");
      my_get_fast_timer(&wait_start);
      ++waiting;
      err = pthread_cond_timedwait(&binlog_cond, &LOCK_log, &cond_wake_time);

      double wait_secs = my_fast_timer_diff_now(&wait_start, NULL);
      binlog_fsync_syncwait_secs += wait_secs;
      ++binlog_fsync_syncwaits;

      if (err && err != EINTR && err != ETIMEDOUT)
      {
        sql_print_warning("Group commit: got error %d from "
                          "pthread_cond_timedwait\n", err);
      }
      err = 0;

      /*
        Only do a sync if no one else has synced
      */
      if (my_fsync_count == binlog_fsync_count)
      {
        int fd = log_file.file;
        sync_binlog_counter= 0;
        thd_proc_info(thd, "flush and sync binlog : fsync");
        my_get_fast_timer(&fsync_start);
        err= my_sync(fd, MYF(MY_WME));
        fsync_time = my_fast_timer_diff_now(&fsync_start, NULL);
        pthread_cond_broadcast(&binlog_cond);
        ++binlog_fsync_groupsync;
        ++binlog_fsync_count;
        binlog_fsync_total_secs += fsync_time;
        if ((fsync_time * 1000000) >= binlog_fsync_slow_usecs)
          ++binlog_fsync_slow;
      }
      else
      {
        /*
          Group commit was done
        */
        ++binlog_fsync_grouped;
      }
    }
    else
    {
      int fd = log_file.file;
      waiting= 0;
      sync_binlog_counter= 0;
      thd_proc_info(thd, "flush and sync binlog : fsync");
      my_get_fast_timer(&fsync_start);
      err= my_sync(fd, MYF(MY_WME));
      fsync_time= my_fast_timer_diff_now(&fsync_start, NULL);
      if (force_binlog_order)
      {
        /* Another thread might be waiting */
        pthread_cond_broadcast(&binlog_cond);
      }
      ++binlog_fsync_nowait;
      ++binlog_fsync_count;
      binlog_fsync_total_secs += fsync_time;
      if ((fsync_time * 1000000) >= binlog_fsync_slow_usecs)
        ++binlog_fsync_slow;
    }
  }

  return err;
}

void MYSQL_BIN_LOG::start_union_events(THD *thd, query_id_t query_id_param)
{
  DBUG_ASSERT(!thd->binlog_evt_union.do_union);
  thd->binlog_evt_union.do_union= TRUE;
  thd->binlog_evt_union.unioned_events= FALSE;
  thd->binlog_evt_union.unioned_events_trans= FALSE;
  thd->binlog_evt_union.first_query_id= query_id_param;
}

void MYSQL_BIN_LOG::stop_union_events(THD *thd)
{
  DBUG_ASSERT(thd->binlog_evt_union.do_union);
  thd->binlog_evt_union.do_union= FALSE;
}

bool MYSQL_BIN_LOG::is_query_in_union(THD *thd, query_id_t query_id_param)
{
  return (thd->binlog_evt_union.do_union && 
          query_id_param >= thd->binlog_evt_union.first_query_id);
}

/**
  This function checks if a transaction, either a multi-statement
  or a single statement transaction is about to commit or not.

  @param thd The client thread that executed the current statement.
  @param all Committing a transaction (i.e. TRUE) or a statement
             (i.e. FALSE).
  @return
    @c true if committing a transaction, otherwise @c false.
*/
bool ending_trans(const THD* thd, const bool all)
{
  return (all || (!all && !(thd->options & 
                  (OPTION_BEGIN | OPTION_NOT_AUTOCOMMIT))));
}

/**
  This function checks if a non-transactional table was updated by
  the current transaction.

  @param thd The client thread that executed the current statement.
  @return
    @c true if a non-transactional table was updated, @c false
    otherwise.
*/
bool trans_has_updated_non_trans_table(const THD* thd)
{
  return (thd->transaction.all.modified_non_trans_table ||
          thd->transaction.stmt.modified_non_trans_table);
}

/**
  This function checks if any statement was committed and cached.

  @param thd The client thread that executed the current statement.
  @param all Committing a transaction (i.e. TRUE) or a statement
             (i.e. FALSE).
  @return
    @c true if at a statement was committed and cached, @c false
    otherwise.
*/
bool trans_has_no_stmt_committed(const THD* thd, bool all)
{
  binlog_trx_data *const trx_data=
    (binlog_trx_data*) thd_get_ha_data(thd, binlog_hton);

  return (!all && !trx_data->at_least_one_stmt_committed);
}

/**
  This function checks if a non-transactional table was updated by the
  current statement.

  @param thd The client thread that executed the current statement.
  @return
    @c true if a non-transactional table was updated, @c false otherwise.
*/
bool stmt_has_updated_non_trans_table(const THD* thd)
{
  return (thd->transaction.stmt.modified_non_trans_table);
}

/*
  These functions are placed in this file since they need access to
  binlog_hton, which has internal linkage.
*/

int THD::binlog_setup_trx_data()
{
  DBUG_ENTER("THD::binlog_setup_trx_data");
  binlog_trx_data *trx_data=
    (binlog_trx_data*) thd_get_ha_data(this, binlog_hton);

  if (trx_data)
    DBUG_RETURN(0);                             // Already set up

  trx_data= (binlog_trx_data*) my_malloc(sizeof(binlog_trx_data), MYF(MY_ZEROFILL));
  if (!trx_data ||
      open_cached_file(&trx_data->trans_log, mysql_tmpdir,
                       LOG_PREFIX, binlog_cache_size, MYF(MY_WME)))
  {
    my_free((uchar*)trx_data, MYF(MY_ALLOW_ZERO_PTR));
    DBUG_RETURN(1);                      // Didn't manage to set it up
  }
  thd_set_ha_data(this, binlog_hton, trx_data);

  trx_data= new (thd_get_ha_data(this, binlog_hton)) binlog_trx_data;

  DBUG_RETURN(0);
}

/*
  Function to start a statement and optionally a transaction for the
  binary log.

  SYNOPSIS
    binlog_start_trans_and_stmt()

  DESCRIPTION

    This function does three things:
    - Start a transaction if not in autocommit mode or if a BEGIN
      statement has been seen.

    - Start a statement transaction to allow us to truncate the binary
      log.

    - Save the currrent binlog position so that we can roll back the
      statement by truncating the transaction log.

      We only update the saved position if the old one was undefined,
      the reason is that there are some cases (e.g., for CREATE-SELECT)
      where the position is saved twice (e.g., both in
      select_create::prepare() and THD::binlog_write_table_map()) , but
      we should use the first. This means that calls to this function
      can be used to start the statement before the first table map
      event, to include some extra events.
 */

void
THD::binlog_start_trans_and_stmt()
{
  binlog_trx_data *trx_data= (binlog_trx_data*) thd_get_ha_data(this, binlog_hton);
  DBUG_ENTER("binlog_start_trans_and_stmt");
  DBUG_PRINT("enter", ("trx_data: 0x%lx  trx_data->before_stmt_pos: %lu",
                       (long) trx_data,
                       (trx_data ? (ulong) trx_data->before_stmt_pos :
                        (ulong) 0)));

  if (trx_data == NULL ||
      trx_data->before_stmt_pos == MY_OFF_T_UNDEF)
  {
    this->binlog_set_stmt_begin();
    if (options & (OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
      trans_register_ha(this, TRUE, binlog_hton);
    trans_register_ha(this, FALSE, binlog_hton);
    /*
      Mark statement transaction as read/write. We never start
      a binary log transaction and keep it read-only,
      therefore it's best to mark the transaction read/write just
      at the same time we start it.
      Not necessary to mark the normal transaction read/write
      since the statement-level flag will be propagated automatically
      inside ha_commit_trans.
    */
    ha_data[binlog_hton->slot].ha_info[0].set_trx_read_write();
  }
  DBUG_VOID_RETURN;
}

void THD::binlog_set_stmt_begin() {
  binlog_trx_data *trx_data=
    (binlog_trx_data*) thd_get_ha_data(this, binlog_hton);

  /*
    The call to binlog_trans_log_savepos() might create the trx_data
    structure, if it didn't exist before, so we save the position
    into an auto variable and then write it into the transaction
    data for the binary log (i.e., trx_data).
  */
  my_off_t pos= 0;
  binlog_trans_log_savepos(this, &pos);
  trx_data= (binlog_trx_data*) thd_get_ha_data(this, binlog_hton);
  trx_data->before_stmt_pos= pos;
}


/*
  Write a table map to the binary log.
 */

int THD::binlog_write_table_map(TABLE *table, bool is_trans)
{
  int error;
  DBUG_ENTER("THD::binlog_write_table_map");
  DBUG_PRINT("enter", ("table: 0x%lx  (%s: #%lu)",
                       (long) table, table->s->table_name.str,
                       table->s->table_map_id));

  /* Pre-conditions */
  DBUG_ASSERT(current_stmt_binlog_row_based && mysql_bin_log.is_open());
  DBUG_ASSERT(table->s->table_map_id != ULONG_MAX);

  Table_map_log_event
    the_event(this, table, table->s->table_map_id, is_trans);

  if (is_trans && binlog_table_maps == 0)
    binlog_start_trans_and_stmt();

  if ((error= mysql_bin_log.write(&the_event)))
    DBUG_RETURN(error);

  binlog_table_maps++;
  DBUG_RETURN(0);
}

Rows_log_event*
THD::binlog_get_pending_rows_event() const
{
  binlog_trx_data *const trx_data=
    (binlog_trx_data*) thd_get_ha_data(this, binlog_hton);
  /*
    This is less than ideal, but here's the story: If there is no
    trx_data, prepare_pending_rows_event() has never been called
    (since the trx_data is set up there). In that case, we just return
    NULL.
   */
  return trx_data ? trx_data->pending() : NULL;
}

void
THD::binlog_set_pending_rows_event(Rows_log_event* ev)
{
  if (thd_get_ha_data(this, binlog_hton) == NULL)
    binlog_setup_trx_data();

  binlog_trx_data *const trx_data=
    (binlog_trx_data*) thd_get_ha_data(this, binlog_hton);

  DBUG_ASSERT(trx_data);
  trx_data->set_pending(ev);
}


/**
  Remove the pending rows event, discarding any outstanding rows.

  If there is no pending rows event available, this is effectively a
  no-op.
 */
int
MYSQL_BIN_LOG::remove_pending_rows_event(THD *thd)
{
  DBUG_ENTER("MYSQL_BIN_LOG::remove_pending_rows_event");

  binlog_trx_data *const trx_data=
    (binlog_trx_data*) thd_get_ha_data(thd, binlog_hton);

  DBUG_ASSERT(trx_data);

  if (Rows_log_event* pending= trx_data->pending())
  {
    delete pending;
    trx_data->set_pending(NULL);
  }

  DBUG_RETURN(0);
}

/*
  Moves the last bunch of rows from the pending Rows event to the binlog
  (either cached binlog if transaction, or disk binlog). Sets a new pending
  event.
*/
int
MYSQL_BIN_LOG::flush_and_set_pending_rows_event(THD *thd,
                                                Rows_log_event* event)
{
  DBUG_ENTER("MYSQL_BIN_LOG::flush_and_set_pending_rows_event(event)");
  DBUG_ASSERT(mysql_bin_log.is_open());
  DBUG_PRINT("enter", ("event: 0x%lx", (long) event));

  int error= 0;

  binlog_trx_data *const trx_data=
    (binlog_trx_data*) thd_get_ha_data(thd, binlog_hton);

  DBUG_ASSERT(trx_data);

  DBUG_PRINT("info", ("trx_data->pending(): 0x%lx", (long) trx_data->pending()));

  if (Rows_log_event* pending= trx_data->pending())
  {
    IO_CACHE *file= &log_file;

    /*
      Decide if we should write to the log file directly or to the
      transaction log.
    */
    if (pending->get_cache_stmt() || my_b_tell(&trx_data->trans_log)) {
      file= &trx_data->trans_log;
    }

    /*
      If we are not writing to the log file directly, we could avoid
      locking the log.
    */
    pthread_mutex_lock(&LOCK_log);

    /*
      Write pending event to log file or transaction cache
    */
    if (pending->write(file))
    {
      pthread_mutex_unlock(&LOCK_log);
      set_write_error(thd);
      DBUG_RETURN(1);
    }

    if (file == &log_file)
    {
      USER_STATS *us= current_thd ? thd_get_user_stats(current_thd) : NULL;
      if (us)
      {
        us->binlog_bytes_written += pending->data_written;
      }
      binlog_bytes_written += pending->data_written;
    }

    delete pending;

    if (file == &log_file)
    {
      error= flush_and_sync(thd, FALSE, NULL, 0);
      if (!error)
      {
        signal_update();
        error= rotate_and_purge(thd, RP_LOCK_LOG_IS_ALREADY_LOCKED, true);
      }
    }

    pthread_mutex_unlock(&LOCK_log);
  }

  thd->binlog_set_pending_rows_event(event);

  DBUG_RETURN(error);
}

/**
  Write an event to the binary log.
*/

bool MYSQL_BIN_LOG::write(Log_event *event_info)
{
  THD *thd= event_info->thd;
  bool error= 1;
  IO_CACHE *file= NULL;
  USER_STATS *us= thd ? thd_get_user_stats(thd) : NULL;
  my_atomic_bigint written= 0;

  DBUG_ENTER("MYSQL_BIN_LOG::write(Log_event *)");

  if (thd->binlog_evt_union.do_union)
  {
    /*
      In Stored function; Remember that function call caused an update.
      We will log the function call to the binary log on function exit
    */
    thd->binlog_evt_union.unioned_events= TRUE;
    thd->binlog_evt_union.unioned_events_trans |= event_info->cache_stmt;
    DBUG_RETURN(0);
  }

  /*
    Flush the pending rows event to the transaction cache or to the
    log file.  Since this function potentially aquire the LOCK_log
    mutex, we do this before aquiring the LOCK_log mutex in this
    function.

    We only end the statement if we are in a top-level statement.  If
    we are inside a stored function, we do not end the statement since
    this will close all tables on the slave.
  */
  bool const end_stmt=
    thd->prelocked_mode && thd->lex->requires_prelocking();
  if (thd->binlog_flush_pending_rows_event(end_stmt))
    DBUG_RETURN(error);

  /*
     In most cases this is only called if 'is_open()' is true; in fact this is
     mostly called if is_open() *was* true a few instructions before, but it
     could have changed since.
  */
  if (likely(is_open()))
  {
    file= &log_file;
#ifdef HAVE_REPLICATION
    /*
      In the future we need to add to the following if tests like
      "do the involved tables match (to be implemented)
      binlog_[wild_]{do|ignore}_table?" (WL#1049)"
    */
    const char *local_db= event_info->get_db();
    if ((thd && !(thd->options & OPTION_BIN_LOG)) ||
	(thd->lex->sql_command != SQLCOM_ROLLBACK_TO_SAVEPOINT &&
         thd->lex->sql_command != SQLCOM_SAVEPOINT &&
         !binlog_filter->db_ok(local_db)))
    {
      DBUG_RETURN(0);
    }
#endif /* HAVE_REPLICATION */

#if defined(USING_TRANSACTIONS) 
    /*
      Should we write to the binlog cache or to the binlog on disk?

      Write to the binlog cache if:
      1 - a transactional engine/table is updated (stmt_has_updated_trans_table == TRUE);
      2 - or the event asks for it (cache_stmt == TRUE);
      3 - or the cache is already not empty (meaning we're in a transaction;
      note that the present event could be about a non-transactional table, but
      still we need to write to the binlog cache in that case to handle updates
      to mixed trans/non-trans table types).
      
      Write to the binlog on disk if only a non-transactional engine is
      updated and:
      1 - the binlog cache is empty or;
      2 - --binlog-direct-non-transactional-updates is set and we are about to
      use the statement format. When using the row format (cache_stmt == TRUE).
    */
    if (opt_using_transactions && thd)
    {
      if (thd->binlog_setup_trx_data())
        goto err;

      binlog_trx_data *const trx_data=
        (binlog_trx_data*) thd_get_ha_data(thd, binlog_hton);
      IO_CACHE *trans_log= &trx_data->trans_log;
      my_off_t trans_log_pos= my_b_tell(trans_log);
      if (event_info->get_cache_stmt() || stmt_has_updated_trans_table(thd) ||
          (!thd->variables.binlog_direct_non_trans_update &&
            trans_log_pos != 0))
      {
        DBUG_PRINT("info", ("Using trans_log: cache: %d, trans_log_pos: %lu",
                            event_info->get_cache_stmt(),
                            (ulong) trans_log_pos));
        thd->binlog_start_trans_and_stmt();
        file= trans_log;
      }
    }
#endif /* USING_TRANSACTIONS */
    DBUG_PRINT("info",("event type: %d",event_info->get_type_code()));

    /*
     *  only aquire lock if we are not writing to trans_log
     */
    if (file == &log_file)
    {
      pthread_mutex_lock(&LOCK_log);
    }

    /*
      No check for auto events flag here - this write method should
      never be called if auto-events are enabled
    */

    /*
      1. Write first log events which describe the 'run environment'
      of the SQL command
    */

    /*
      If row-based binlogging, Insert_id, Rand and other kind of "setting
      context" events are not needed.
    */
    if (thd)
    {
      if (!thd->current_stmt_binlog_row_based)
      {
        if (thd->stmt_depends_on_first_successful_insert_id_in_prev_stmt)
        {
          Intvar_log_event e(thd,(uchar) LAST_INSERT_ID_EVENT,
                             thd->first_successful_insert_id_in_prev_stmt_for_binlog);
          if (e.write(file))
            goto err;
          written += e.data_written;
        }
        if (thd->auto_inc_intervals_in_cur_stmt_for_binlog.nb_elements() > 0)
        {
          DBUG_PRINT("info",("number of auto_inc intervals: %u",
                             thd->auto_inc_intervals_in_cur_stmt_for_binlog.
                             nb_elements()));
          Intvar_log_event e(thd, (uchar) INSERT_ID_EVENT,
                             thd->auto_inc_intervals_in_cur_stmt_for_binlog.
                             minimum());
          if (e.write(file))
            goto err;
          written += e.data_written;
        }
        if (thd->rand_used)
        {
          Rand_log_event e(thd,thd->rand_saved_seed1,thd->rand_saved_seed2);
          if (e.write(file))
            goto err;
          written += e.data_written;
        }
        if (thd->user_var_events.elements)
        {
          for (uint i= 0; i < thd->user_var_events.elements; i++)
          {
            BINLOG_USER_VAR_EVENT *user_var_event;
            get_dynamic(&thd->user_var_events,(uchar*) &user_var_event, i);
            User_var_log_event e(thd, user_var_event->user_var_event->name.str,
                                 user_var_event->user_var_event->name.length,
                                 user_var_event->value,
                                 user_var_event->length,
                                 user_var_event->type,
                                 user_var_event->charset_number);
            if (e.write(file))
              goto err;
            written += e.data_written;
          }
        }
      }
    }

    /*
       Write the SQL command
     */

    if (event_info->write(file) || 
        DBUG_EVALUATE_IF("injecting_fault_writing", 1, 0))
      goto err;
    written += event_info->data_written;

    if (file == &log_file) // we are writing to the real log (disk)
    {
      if (flush_and_sync(thd, FALSE, NULL, 0))
	goto err;
      signal_update();
      if ((error= rotate_and_purge(thd, RP_LOCK_LOG_IS_ALREADY_LOCKED, true)))
        goto err;
      if (us)
      {
        us->binlog_bytes_written += written;
      }
      binlog_bytes_written += written;
    }
    error=0;

err:
    if (error)
      set_write_error(thd);
  }

  // only unlock if we initially locked
  if (file == &log_file)
  {
    pthread_mutex_unlock(&LOCK_log);
  }
  DBUG_RETURN(error);
}


int error_log_print(enum loglevel level, const char *format,
                    va_list args)
{
  return logger.error_log_print(level, format, args);
}


bool slow_log_print(THD *thd, const char *query, uint query_length,
                    ulonglong current_utime,
                    struct system_status_var *query_start_status)
{
  return logger.slow_log_print(thd, query, query_length, current_utime,
                               query_start_status);
}


bool LOGGER::log_command(THD *thd, enum enum_server_command command)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  Security_context *sctx= thd->security_ctx;
#endif
  /*
    Log command if we have at least one log event handler enabled and want
    to log this king of commands
  */
  if (*general_log_handler_list && (what_to_log & (1L << (uint) command)))
  {
    if ((thd->options & OPTION_LOG_OFF)
#ifndef NO_EMBEDDED_ACCESS_CHECKS
         && (sctx->master_access & SUPER_ACL)
#endif
       )
    {
      /* No logging */
      return FALSE;
    }

    return TRUE;
  }

  return FALSE;
}


bool general_log_print(THD *thd, enum enum_server_command command,
                       const char *format, ...)
{
  va_list args;
  uint error= 0;

  /* Print the message to the buffer if we want to log this king of commands */
  if (! logger.log_command(thd, command))
    return FALSE;

  va_start(args, format);
  error= logger.general_log_print(thd, command, format, args);
  va_end(args);

  return error;
}

bool general_log_write(THD *thd, enum enum_server_command command,
                       const char *query, uint query_length)
{
  /* Write the message to the log if we want to log this king of commands */
  if (logger.log_command(thd, command))
    return logger.general_log_write(thd, command, query, query_length);

  return FALSE;
}

/**
  @note
    If rotation fails, for instance the server was unable 
    to create a new log file, we still try to write an 
    incident event to the current log.

  @retval
    nonzero - error 
*/
int MYSQL_BIN_LOG::rotate_and_purge(THD *thd, uint flags, bool log_maybe_full)
{
  int error= 0;
  DBUG_ENTER("MYSQL_BIN_LOG::rotate_and_purge");
  DBUG_EXECUTE_IF("enable_group_commit", {
    group_commit_allowed= TRUE;
    current_ticket=1;
    next_ticket=1; });
#ifdef HAVE_REPLICATION
  bool check_purge= false;
#endif
  if (!(flags & RP_LOCK_LOG_IS_ALREADY_LOCKED))
  {
    if (log_maybe_full)
    {
      pthread_mutex_lock(&LOCK_log);
    }
    else
    {
      int err= pthread_mutex_trylock(&LOCK_log);
      if (err)
      {
        /* When this connection wrote to the binlog it was not full, so it is
           OK to not wait here on LOCK_log to check again whether the log is full.
        */
        DBUG_RETURN(error);
      }
    }
  }

  /* A call to ::new_file_impl is in progress when stop_new_xids==TRUE */
  while (stop_new_xids)
    pthread_cond_wait(&COND_stop_xids, &LOCK_log);

  if ((flags & RP_FORCE_ROTATE) ||
      (my_b_tell(&log_file) >= (my_off_t) max_size))
  {
    if ((error= new_file_without_locking()))
      /** 
         Be conservative... There are possible lost events (eg, 
         failing to log the Execute_load_query_log_event
         on a LOAD DATA while using a non-transactional
         table)!

         We give it a shot and try to write an incident event anyway
         to the current log. 
      */
      if (!write_incident(current_thd, FALSE, NULL))
        flush_and_sync(thd, FALSE, NULL, 0);

#ifdef HAVE_REPLICATION
    check_purge= true;
#endif
  }
  if (!(flags & RP_LOCK_LOG_IS_ALREADY_LOCKED))
    pthread_mutex_unlock(&LOCK_log);
#ifdef HAVE_REPLICATION
  /*
    NOTE: Run purge_logs wo/ holding LOCK_log
          as it otherwise will deadlock in ndbcluster_binlog_index_purge_file
  */
  if (!error && check_purge && expire_logs_days)
  {
    time_t purge_time= my_time(0) - expire_logs_days*24*60*60;
    if (purge_time >= 0)
      purge_logs_before_date(purge_time);
  }
#endif
  DBUG_RETURN(error);
}

uint MYSQL_BIN_LOG::next_file_id()
{
  uint res;
  pthread_mutex_lock(&LOCK_log);
  res = file_id++;
  pthread_mutex_unlock(&LOCK_log);
  return res;
}


/*
  Write the contents of a cache to the binary log.

  SYNOPSIS
    write_cache()
    cache    Cache to write to the binary log
    lock_log True if the LOCK_log mutex should be aquired, false otherwise
    sync_log True if the log should be flushed and sync:ed

  DESCRIPTION
    Write the contents of the cache to the binary log. The cache will
    be reset as a READ_CACHE to be able to read the contents from it.
 */

int MYSQL_BIN_LOG::write_cache(IO_CACHE *cache, bool lock_log)
{
  Mutex_sentry sentry(lock_log ? &LOCK_log : NULL);

  if (reinit_io_cache(cache, READ_CACHE, 0, 0, 0))
    return ER_ERROR_ON_WRITE;
  uint length= my_b_bytes_in_cache(cache), group, carry, hdr_offs;
  long val;
  uchar header[LOG_EVENT_HEADER_LEN];

  /*
    The events in the buffer have incorrect end_log_pos data
    (relative to beginning of group rather than absolute),
    so we'll recalculate them in situ so the binlog is always
    correct, even in the middle of a group. This is possible
    because we now know the start position of the group (the
    offset of this cache in the log, if you will); all we need
    to do is to find all event-headers, and add the position of
    the group to the end_log_pos of each event.  This is pretty
    straight forward, except that we read the cache in segments,
    so an event-header might end up on the cache-border and get
    split.
  */

  group= (uint)my_b_tell(&log_file);
  hdr_offs= carry= 0;

  DBUG_ASSERT(!cache->error);

  do
  {

    /*
      if we only got a partial header in the last iteration,
      get the other half now and process a full header.
    */
    if (unlikely(carry > 0))
    {
      DBUG_ASSERT(carry < LOG_EVENT_HEADER_LEN);

      /* assemble both halves */
      memcpy(&header[carry], (char *)cache->read_pos, LOG_EVENT_HEADER_LEN - carry);

      /* fix end_log_pos */
      val= uint4korr(&header[LOG_POS_OFFSET]) + group;
      int4store(&header[LOG_POS_OFFSET], val);

      /* write the first half of the split header */
      if (my_b_write(&log_file, header, carry))
        return ER_ERROR_ON_WRITE;

      /*
        copy fixed second half of header to cache so the correct
        version will be written later.
      */
      memcpy((char *)cache->read_pos, &header[carry], LOG_EVENT_HEADER_LEN - carry);

      /* next event header at ... */
      hdr_offs = uint4korr(&header[EVENT_LEN_OFFSET]) - carry;

      carry= 0;
    }

    /* if there is anything to write, process it. */

    if (likely(length > 0))
    {
      /*
        process all event-headers in this (partial) cache.
        if next header is beyond current read-buffer,
        we'll get it later (though not necessarily in the
        very next iteration, just "eventually").
      */

      while (hdr_offs < length)
      {
        /*
          partial header only? save what we can get, process once
          we get the rest.
        */

        if (hdr_offs + LOG_EVENT_HEADER_LEN > length)
        {
          carry= length - hdr_offs;
          memcpy(header, (char *)cache->read_pos + hdr_offs, carry);
          length= hdr_offs;
        }
        else
        {
          /* we've got a full event-header, and it came in one piece */

          uchar *log_pos= (uchar *)cache->read_pos + hdr_offs + LOG_POS_OFFSET;

          /* fix end_log_pos */
          val= uint4korr(log_pos) + group;
          int4store(log_pos, val);

          /* next event header at ... */
          log_pos= (uchar *)cache->read_pos + hdr_offs + EVENT_LEN_OFFSET;
          hdr_offs += uint4korr(log_pos);

        }
      }

      /*
        Adjust hdr_offs. Note that it may still point beyond the segment
        read in the next iteration; if the current event is very long,
        it may take a couple of read-iterations (and subsequent adjustments
        of hdr_offs) for it to point into the then-current segment.
        If we have a split header (!carry), hdr_offs will be set at the
        beginning of the next iteration, overwriting the value we set here:
      */
      hdr_offs -= length;
    }

    /* Write data to the binary log file */
    DBUG_EXECUTE_IF("corrupt_binlog_tail", length/= 2;);
    if (my_b_write(&log_file, cache->read_pos, length))
      return ER_ERROR_ON_WRITE;
    DBUG_EXECUTE_IF("corrupt_binlog_tail",
                    {
                      flush_io_cache(&log_file);
                      abort();
                    });
    cache->read_pos=cache->read_end;		// Mark buffer used up
  } while ((length= my_b_fill(cache)));

  DBUG_ASSERT(carry == 0);

  /* Check for error from my_b_fill -- see http://bugs.mysql.com/60173
   */
  return cache->error ? ER_ERROR_ON_WRITE : 0;  // All OK
}

/*
  Helper function to get the error code of the query to be binlogged.
 */
int query_error_code(THD *thd, bool not_killed)
{
  int error;
  
  if (not_killed || (thd->killed == THD::KILL_BAD_DATA))
  {
    error= thd->is_error() ? thd->main_da.sql_errno() : 0;

    /* thd->main_da.sql_errno() might be ER_SERVER_SHUTDOWN or
       ER_QUERY_INTERRUPTED, So here we need to make sure that error
       is not set to these errors when specified not_killed by the
       caller.
    */
    if (error == ER_SERVER_SHUTDOWN || error == ER_QUERY_INTERRUPTED)
      error= 0;
  }
  else
  {
    /* killed status for DELAYED INSERT thread should never be used */
    DBUG_ASSERT(!(thd->system_thread & SYSTEM_THREAD_DELAYED_INSERT));
    error= thd->killed_errno();
  }

  return error;
}

bool MYSQL_BIN_LOG::write_incident(THD *thd, bool lock, bool *log_was_full)
{
  uint error= 0;
  USER_STATS *us= thd ? thd_get_user_stats(thd) : NULL;
  DBUG_ENTER("MYSQL_BIN_LOG::write_incident");

  if (!is_open())
    DBUG_RETURN(error);

  LEX_STRING const write_error_msg=
    { C_STRING_WITH_LEN("error writing to the binary log") };
  Incident incident= INCIDENT_LOST_EVENTS;
  Incident_log_event ev(thd, incident, write_error_msg);
  if (lock) {
    pthread_mutex_lock(&LOCK_log);
  }
  error= ev.write(&log_file);
  if (log_was_full)
  {
    *log_was_full = my_b_tell(&log_file) >= (my_off_t) max_size;
  }

  if (us)
  {
    us->binlog_bytes_written += ev.data_written;
  }
  binlog_bytes_written += ev.data_written;

  if (lock)
  {
    if (!error && !(error= flush_and_sync(thd, FALSE, NULL, 0)))
    {
      signal_update();
      error= rotate_and_purge(thd, RP_LOCK_LOG_IS_ALREADY_LOCKED, true);
    }
    pthread_mutex_unlock(&LOCK_log);
  }
  DBUG_RETURN(error);
}

/**
  Write a cached log entry to the binary log.
  - To support transaction over replication, we wrap the transaction
  with BEGIN/COMMIT or BEGIN/ROLLBACK in the binary log.
  We want to write a BEGIN/ROLLBACK block when a non-transactional table
  was updated in a transaction which was rolled back. This is to ensure
  that the same updates are run on the slave.

  @param thd
  @param cache		The cache to copy to the binlog
  @param commit_event   The commit event to print after writing the
                        contents of the cache.
  @param incident       Defines if an incident event should be created to
                        notify that some non-transactional changes did
                        not get into the binlog.
  @param ht             handlerton to be used for group commit optimization
  @param pending        Number of transactions in ha_commit_trans including
                        the caller

  @note
    We only come here if there is something in the cache.
  @note
    The thing in the cache is always a complete transaction.
  @note
    'cache' needs to be reinitialized after this functions returns.
*/

bool MYSQL_BIN_LOG::write(THD *thd, IO_CACHE *cache, Log_event *commit_event,
                          bool incident, bool async, handlerton *ht, int32 pending,
                          bool *log_was_full)
{
  USER_STATS *us= thd ? thd_get_user_stats(thd) : NULL;
  DBUG_ENTER("MYSQL_BIN_LOG::write(THD *, IO_CACHE *, Log_event *)");
  VOID(pthread_mutex_lock(&LOCK_log));

  if (is_open() &&
      my_b_tell(cache) > 0 &&
      commit_event && commit_event->get_type_code() == XID_EVENT)
  {
    /* If this will call ::new_file_impl, wait until that call won't wait.
    */
    while (stop_new_xids)
      pthread_cond_wait(&COND_stop_xids, &LOCK_log);
  }

  /* NULL would represent nothing to replicate after ROLLBACK */
  DBUG_ASSERT(commit_event != NULL);

  DBUG_ASSERT(is_open());
  if (likely(is_open()))                       // Should always be true
  {
    /*
      We only bother to write to the binary log if there is anything
      to write.
     */
    if (my_b_tell(cache) > 0)
    {
      /*
        Log "BEGIN" at the beginning of every transaction.  Here, a
        transaction is either a BEGIN..COMMIT block or a single
        statement in autocommit mode.
      */
      Query_log_event qinfo(thd, STRING_WITH_LEN("BEGIN"), TRUE, TRUE, 0);

      /*
        Now this Query_log_event has artificial log_pos 0. It must be
        adjusted to reflect the real position in the log. Not doing it
        would confuse the slave: it would prevent this one from
        knowing where he is in the master's binlog, which would result
        in wrong positions being shown to the user, MASTER_POS_WAIT
        undue waiting etc.
      */
      if (qinfo.write(&log_file))
        goto err;
      if (us)
      {
        us->binlog_bytes_written += qinfo.data_written;
      }
      binlog_bytes_written += qinfo.data_written;

      DBUG_EXECUTE_IF("crash_before_writing_xid",
                      {
                        if ((write_error= write_cache(cache, false)))
                          DBUG_PRINT("info", ("error writing binlog cache: %d",
                                               write_error));
                        flush_and_sync(thd, FALSE, ht, pending);
                        DBUG_PRINT("info", ("crashing before writing xid"));
                        DBUG_SUICIDE();
                      });

      if ((write_error= write_cache(cache, false)))
        goto err;
      if (us)
      {
        us->binlog_bytes_written += my_b_tell(cache);
      }
      binlog_bytes_written += my_b_tell(cache);

      if (commit_event && commit_event->write(&log_file))
        goto err;
      if (commit_event)
      {
        binlog_bytes_written += qinfo.data_written;
        if (us)
          us->binlog_bytes_written += qinfo.data_written;
      }

      if (incident && write_incident(thd, FALSE, NULL))
        goto err;

      if (flush_and_sync(thd, async, ht, pending))
        goto err;
      DBUG_EXECUTE_IF("half_binlogged_transaction", DBUG_SUICIDE(););
      if (cache->error)				// Error on read
      {
        sql_print_error(ER(ER_ERROR_ON_READ), cache->file_name, errno);
        write_error=1;				// Don't give more errors
        goto err;
      }
      signal_update();
    }

    /*
      if commit_event is Xid_log_event, increase the number of
      prepared_xids (it's decreasd in ::unlog()). Binlog cannot be rotated
      if there're prepared xids in it - see the comment in new_file() for
      an explanation.
      If the commit_event is not Xid_log_event (then it's a Query_log_event)
      rotate binlog, if necessary.
    */
    if (commit_event && commit_event->get_type_code() == XID_EVENT)
    {
      DBUG_ASSERT(!stop_new_xids);
      pthread_mutex_lock(&LOCK_prep_xids);
      prepared_xids++;
      pthread_mutex_unlock(&LOCK_prep_xids);

      if (log_was_full)
      {
        *log_was_full = my_b_tell(&log_file) >= (my_off_t) max_size;
      }
    }
    else
      if (rotate_and_purge(thd, RP_LOCK_LOG_IS_ALREADY_LOCKED, true))
        goto err;
  }
  VOID(pthread_mutex_unlock(&LOCK_log));

  DBUG_RETURN(0);

err:
  if (!write_error)
  {
    write_error= 1;
    sql_print_error(ER(ER_ERROR_ON_WRITE), name, errno);
  }
  VOID(pthread_mutex_unlock(&LOCK_log));
  DBUG_RETURN(1);
}


/**
  Wait until we get a signal that the binary log has been updated.

  @param thd		Thread variable
  @param new_msg        New message to display for connection status

  @note
    One must have a lock on LOCK_log before calling this function.
    This lock will be released before return! That's required by
    THD::enter_cond() (see NOTES in sql_class.h).
*/

void MYSQL_BIN_LOG::wait_for_update(THD* thd, const char* new_msg)
{
  const char *old_msg;
  DBUG_ENTER("wait_for_update");

  old_msg= thd->enter_cond(&update_cond, &LOCK_log, new_msg);
  pthread_cond_wait(&update_cond, &LOCK_log);
  thd->exit_cond(old_msg);
  DBUG_VOID_RETURN;
}


/**
  Close the log file.

  @param exiting     Bitmask for one or more of the following bits:
          - LOG_CLOSE_INDEX : if we should close the index file
          - LOG_CLOSE_TO_BE_OPENED : if we intend to call open
                                     at once after close.
          - LOG_CLOSE_STOP_EVENT : write a 'stop' event to the log

  @note
    One can do an open on the object at once after doing a close.
    The internal structures are not freed until cleanup() is called
*/

void MYSQL_BIN_LOG::close(uint exiting)
{					// One can't set log_type here!
  DBUG_ENTER("MYSQL_BIN_LOG::close");
  DBUG_PRINT("enter",("exiting: %d", (int) exiting));
  if (log_state == LOG_OPENED)
  {
#ifdef HAVE_REPLICATION
    if (log_type == LOG_BIN && !no_auto_events &&
	(exiting & LOG_CLOSE_STOP_EVENT))
    {
      Stop_log_event s;
      s.write(&log_file);
      bytes_written+= s.data_written;
      signal_update();
    }
#endif /* HAVE_REPLICATION */

    /* don't pwrite in a file opened with O_APPEND - it doesn't work */
    if (log_file.type == WRITE_CACHE && log_type == LOG_BIN)
    {
      my_off_t offset= BIN_LOG_HEADER_SIZE + FLAGS_OFFSET;
      my_off_t org_position= my_tell(log_file.file, MYF(0));
      uchar flags= 0;            // clearing LOG_EVENT_BINLOG_IN_USE_F
      my_pwrite(log_file.file, &flags, 1, offset, MYF(0));
      /*
        Restore position so that anything we have in the IO_cache is written
        to the correct position.
        We need the seek here, as my_pwrite() is not guaranteed to keep the
        original position on system that doesn't support pwrite().
      */
      my_seek(log_file.file, org_position, MY_SEEK_SET, MYF(0));
    }

    /* this will cleanup IO_CACHE, sync and close the file */
    MYSQL_LOG::close(exiting);
  }

  /*
    The following test is needed even if is_open() is not set, as we may have
    called a not complete close earlier and the index file is still open.
  */

  if ((exiting & LOG_CLOSE_INDEX) && my_b_inited(&index_file))
  {
    end_io_cache(&index_file);
    if (my_close(index_file.file, MYF(0)) < 0 && ! write_error)
    {
      write_error= 1;
      sql_print_error(ER(ER_ERROR_ON_WRITE), index_file_name, errno);
    }
  }
  log_state= (exiting & LOG_CLOSE_TO_BE_OPENED) ? LOG_TO_BE_OPENED : LOG_CLOSED;
  safeFree(name);
  DBUG_VOID_RETURN;
}


void MYSQL_BIN_LOG::set_max_size(ulong max_size_arg)
{
  /*
    We need to take locks, otherwise this may happen:
    new_file() is called, calls open(old_max_size), then before open() starts,
    set_max_size() sets max_size to max_size_arg, then open() starts and
    uses the old_max_size argument, so max_size_arg has been overwritten and
    it's like if the SET command was never run.
  */
  DBUG_ENTER("MYSQL_BIN_LOG::set_max_size");
  pthread_mutex_lock(&LOCK_log);
  if (is_open())
    max_size= max_size_arg;
  pthread_mutex_unlock(&LOCK_log);
  DBUG_VOID_RETURN;
}

bool MYSQL_BIN_LOG::extract_master_info(Log_event* ev,
                                        char *master_log_name,
                                        my_off_t *master_log_pos)
{
  bool extracted = false;
  DBUG_ENTER("MYSQL_LOG::extract_master_info");

  /*
    Some events always have correct master log information, like:
    Xid_event, Master_info_log_event, Rotate_log_event, etc
    Some Query events have correct master log information (each query
    event only has one query inside), like BEGIN/COMMIT.
   */
  switch (ev->get_type_code())
  {
  case QUERY_EVENT:
  {
    Query_log_event *query = (Query_log_event *)ev;

    /*
      Check whether the query event has the correct master log information:
      if so, accept it.
     */
    for (size_t idx = 0; idx < sizeof(query_with_log)/sizeof(QueryLogEvent);
         ++idx)
      if ((query->q_len == query_with_log[idx].query_length_ &&
           strncmp(query_with_log[idx].query_, query->query,
                   query->q_len) == 0))
    {
      *master_log_pos = query->log_pos;
      extracted = true;
    }
    break;
  }
  case ROTATE_EVENT:
    /*
      Because I/O thread can add rotate events for slave side purpose,
      like exceed file size limit, and those events would not carry the
      correct master side information, we skip them. Those events only
      have slave side information.
     */
    if (ev->server_id != ::server_id)
    {
      Rotate_log_event *rotate = (Rotate_log_event *)ev;

      /*
        Rotate_log_event from the master always indicates the correct info.
       */
      strcpy(master_log_name, rotate->new_log_ident);
      master_log_name[rotate->ident_len] = '\0';
      *master_log_pos = rotate->pos;
      extracted = true;
    }
    break;
  case XID_EVENT:
    /*
      XID_EVENT is the same as COMMIT in relay-log. It carries the
      correct master log information.
     */
    *master_log_pos = ev->log_pos;
    extracted = true;
    break;
  case FORMAT_DESCRIPTION_EVENT:
    /*
      Format event would not cause re-transmit the same event from the master,
      Assume its position in relay-log is correct.
     */
    extracted = true;
    break;
  default:
    extracted = false;
    break;
  }
  DBUG_RETURN(extracted);
}

/*
  Determine whether we find the relay-log corresponding to the master-log info.
 */
bool MYSQL_BIN_LOG::find_master_pos_inlog(const char *relay_log_name,
                                          ulonglong relay_log_pos,
                                          const char *master_log_name,
                                          ulonglong master_log_pos,
                                          char *last_master_log_name,
                                          ulonglong *last_master_log_pos,
                                          bool *relay_file_error,
                                          my_off_t *last_valid_offset,
                                          my_off_t *relay_file_size,
                                          const char **errmsg)
{
  IO_CACHE log_file;
  DBUG_ENTER("MYSQL_LOG::find_master_pos_inlog");
  file_id = open_binlog(&log_file, relay_log_name, errmsg);

  /* Note that File may be -1 and file_id is uint. */
  if ((File)file_id < 0)
  {
    file_id = 0; // make is_valid() return false
    *relay_file_error = true;
    DBUG_RETURN(false);
  }

  Format_description_log_event *desc_event =
    new Format_description_log_event(3);

  for (;;)
  {
    Log_event* ev = Log_event::read_log_event(&log_file, NULL, desc_event, NULL);
    if (!ev)
    {
      break;
    }

    /*
       If the relay-log event has been executed by slave sql thread, then we
       can assume that the event is safe., strlen(master_log_name)
       update_master_info() might shrink the last relay-log to make sure that
       we would not re-append events.  Then, we would re-transmit all events
       from the master.  If the last relay-log gets shrinked, we need to make
       sure that re-appended relay-log is the same as the one before the
       shrink.  Otherwise, the sql thread will get confused.

       TODO(wei): we still need to handle the situation that the last relay
       log is corrupted and the shrink point is before the execution point.
     */
    my_off_t offset = my_b_tell(&log_file);

    if (extract_master_info(ev, last_master_log_name, last_master_log_pos))
    {
      *last_valid_offset = offset;
    }
    else if (offset == relay_log_pos)
    {
      strmake(last_master_log_name, master_log_name, strlen(master_log_name));
      *last_master_log_pos = master_log_pos;
      *last_valid_offset = offset;
    }

    if (ev->get_type_code() == FORMAT_DESCRIPTION_EVENT)
    {
      delete desc_event;
      desc_event = (Format_description_log_event *)ev;

      /*
         If we have the correct last executed relay-log information, we can
         seek to the position after getting the correct format event.
       */
      if (relay_log_pos != RPL_BAD_POS && offset < relay_log_pos)
      {
        my_b_seek(&log_file, relay_log_pos);
        strmake(last_master_log_name, master_log_name, strlen(master_log_name));
        *last_master_log_pos = master_log_pos;
        *last_valid_offset   = relay_log_pos;
      }
    }
    else
    {
      delete ev;
    }
  }
  *relay_file_error = log_file.error;
  if (relay_file_size)
    *relay_file_size = my_b_tell(&log_file);

  my_close(file_id, MYF(MY_WME));
  end_io_cache(&log_file);
  delete desc_event;

  DBUG_RETURN(!(*relay_file_error));
}

int MYSQL_BIN_LOG::update_master_info(THD *thd,
                                      const char *relay_log_name,
                                      ulonglong relay_log_pos,
                                      const char *master_log_name,
                                      ulonglong master_log_pos,
                                      bool *need_check_master_log,
                                      bool *found_relay_info)
{
  int error = 0;
  LOG_INFO linfo;
  char last_relay_log_name[FN_REFLEN];
  char last_master_log_name[FN_REFLEN];

  Master_info* mi = get_master_info();
  const char *errmsg = NULL;

  my_off_t last_valid_off = 0, last_master_log_pos;

  bool found_relay_file = false;
  bool relay_file_error = false;
  my_off_t relay_file_size = 0;

  /* Whether the specified relay_log_name, relay_log_pos are available */
  bool relay_log_info_avail;

  char buff1[22], buff2[22];

  DBUG_ENTER("MYSQL_LOG::update_master_info");

  *found_relay_info      = false;
  *need_check_master_log = false;
  relay_log_info_avail   = (strcmp(relay_log_name, "") != 0 &&
                            relay_log_pos != RPL_BAD_POS);
  strmake(last_master_log_name, "", FN_REFLEN);

  if (find_log_pos(&linfo, NullS, true))
  {
    /*
        This should be fine because we are going to retrieve all master-logs
        from scratch.
     */
    ha_reset_slave(thd);
    sql_print_information("update_master_info: relay-log file not found,"
                          " will reset replication from scratch");

    DBUG_RETURN(0);
  }
  else
  {
    /*
       Find the last replication relay-log filename in relay-log.info and
       find the specified <relay_log_name> is in relay-log.info.
     */
    for (;;)
    {
      /* find the last log file from index_log_file */
      strmake(last_relay_log_name, linfo.log_file_name, FN_REFLEN);
      last_relay_log_name[FN_REFLEN - 1] = '\0';

      /* check whether we found the specified relay-log file */
      if (relay_log_info_avail && !found_relay_file &&
          strcmp(last_relay_log_name, relay_log_name) == 0)
      {
        found_relay_file = true;
      }
      if (find_next_log(&linfo, true))
        break;
    }
  }

  if (relay_log_info_avail && !found_relay_file)
  {
    /*
       This might be totally wrong because we could not find the last executed
       relay-log based on InnoDB information.  It might be normal that there
       is a relay-log switch which cause the old log purged.  We need to check
       whether master-log information match relay-log.info.
     */
    *need_check_master_log = true;
  }

  if (relay_log_info_avail &&
      strcmp(relay_log_name, last_relay_log_name) == 0)
  {
    if (!find_master_pos_inlog(relay_log_name, relay_log_pos,
                               master_log_name, master_log_pos,
                               last_master_log_name, &last_master_log_pos,
                               &relay_file_error, &last_valid_off,
                               &relay_file_size, &errmsg))
    {
      /* We could not read the relay-log file correctly. */
      sql_print_information("update_master_info: open relay-log(%s) error %s",
                            relay_log_name, errmsg);
      DBUG_RETURN(1);
    }
  }
  else
  {
    if (!find_master_pos_inlog(last_relay_log_name, (ulonglong) -1,
                               NULL, (ulonglong) -1,
                               last_master_log_name, &last_master_log_pos,
                               &relay_file_error, &last_valid_off,
                               &relay_file_size, &errmsg))
    {
      /* We could not read the relay-log file correctly. */
      sql_print_information("update_master_info: open relay-log(%s) error %s",
                            last_relay_log_name, errmsg);
      DBUG_RETURN(1);
    }
  }

  /*
     If we do not find master-log reading information from all valid events,
     we assume that the information in file master.log is correct based on
     the protocol: we always write to file master.info before writing events
     into relay-log.
   */
  if (strlen(last_master_log_name) > 0)
  {
    DBUG_PRINT("info",("found master log_file_name: '%s'  position: %s",
                       last_master_log_name,
                       llstr(last_master_log_pos, buff1)));

    /* truncate the log to the last valid event */
    if (relay_file_error || last_valid_off != relay_file_size)
    {
      File trunc_file_id = my_open(last_relay_log_name, O_WRONLY, MYF(MY_WME));
      if (trunc_file_id < 0)
      {
        sql_print_error("update_master_info: open file '%s' for "
                        "truncation failed; error: %d",
                        last_relay_log_name, errno);
        DBUG_RETURN(1);
      }

      /*
         We might truncate the last file less than relay-log.info indicates the
         file should be.  This is fine because we going to create a new
         relay-log file after this one and the sql thread will be directed to
         the new file to read relay events.
       */
      off_t new_len = (off_t)last_valid_off;
      if (ftruncate(trunc_file_id, new_len))
      {
        sql_print_error("update_master_info: truncate file(%s) from %s to %s; "
                        "error: %d\n", last_relay_log_name,
                        llstr(relay_file_size, buff1),
                        llstr(new_len, buff2), errno);
        my_close(trunc_file_id, MYF(MY_WME));

        DBUG_RETURN(1);
      }
      my_close(trunc_file_id, MYF(MY_WME));

      sql_print_information("update_master_info: truncated file(%s) from %s to %s",
                            last_relay_log_name, llstr(relay_file_size, buff1),
                            llstr(new_len, buff2));
    }

    /* update offset for SQL thread (master log name and offset) */

    if (strcmp(mi->master_log_name, last_master_log_name) != 0 ||
        mi->master_log_pos != last_master_log_pos)
    {
      sql_print_information("update_master_info: adjust master offset:\n"
                            "\tOld: file:'%s', position:%s\n"
                            "\tNew: file:'%s', position:%s",
                            mi->master_log_name,
                            llstr(mi->master_log_pos, buff1),
                            last_master_log_name,
                            llstr(last_master_log_pos, buff2));
      strcpy(mi->master_log_name, last_master_log_name);
      mi->master_log_pos = last_master_log_pos;
    }

    /*
       We must write the file to disk here even there are no changes because
       MYSQL_BIN_LOG::open() might create a new relay-log file.
     */
    reinit_io_cache(&mi->file, WRITE_CACHE, 0L, 0, 1);
    if ((error=test(flush_master_info(mi, FALSE, FALSE))))
    {
      sql_print_error("update_master_info: failed to flush master info file");
    }
    else
    {
      error = my_sync(mi->file.file, MYF(MY_WME));
    }
  }
  else if (relay_log_info_avail)
  {
    sql_print_warning("update_master_info: cannot find master information "
                      "from the last relay-log: assume master.info is correct");
  }

  *found_relay_info = relay_log_info_avail;
  DBUG_RETURN(error);
}


/**
  Check if a string is a valid number.

  @param str			String to test
  @param res			Store value here
  @param allow_wildcards	Set to 1 if we should ignore '%' and '_'

  @note
    For the moment the allow_wildcards argument is not used
    Should be move to some other file.

  @retval
    1	String is a number
  @retval
    0	Error
*/

static bool test_if_number(register const char *str,
			   long *res, bool allow_wildcards)
{
  reg2 int flag;
  const char *start;
  DBUG_ENTER("test_if_number");

  flag=0; start=str;
  while (*str++ == ' ') ;
  if (*--str == '-' || *str == '+')
    str++;
  while (my_isdigit(files_charset_info,*str) ||
	 (allow_wildcards && (*str == wild_many || *str == wild_one)))
  {
    flag=1;
    str++;
  }
  if (*str == '.')
  {
    for (str++ ;
	 my_isdigit(files_charset_info,*str) ||
	   (allow_wildcards && (*str == wild_many || *str == wild_one)) ;
	 str++, flag=1) ;
  }
  if (*str != 0 || flag == 0)
    DBUG_RETURN(0);
  if (res)
    *res=atol(start);
  DBUG_RETURN(1);			/* Number ok */
} /* test_if_number */


void sql_perror(const char *message)
{
#ifdef HAVE_STRERROR
  sql_print_error("%s: %s",message, strerror(errno));
#else
  perror(message);
#endif
}


/*
  Change the file associated with two output streams. Used to
  redirect stdout and stderr to a file. The streams are reopened
  only for appending (writing at end of file).
*/
extern "C" my_bool reopen_fstreams(const char *filename,
                                   FILE *outstream, FILE *errstream)
{
  if (outstream && !my_freopen(filename, "a", outstream))
    return TRUE;

  if (errstream && !my_freopen(filename, "a", errstream))
      return TRUE;

  /* The error stream must be unbuffered. */
  if (errstream)
    setbuf(errstream, NULL);

  return FALSE;
}


/*
  Unfortunately, there seems to be no good way
  to restore the original streams upon failure.
*/
static bool redirect_std_streams(const char *file)
{
  if (reopen_fstreams(file, stdout, stderr))
    return TRUE;

  setbuf(stderr, NULL);
  return FALSE;
}


bool flush_error_log()
{
  bool result= 0;
  if (opt_error_log)
  {
    VOID(pthread_mutex_lock(&LOCK_error_log));
    if (redirect_std_streams(log_error_file))
      result= 1;
    VOID(pthread_mutex_unlock(&LOCK_error_log));
  }
  return result;
}

void MYSQL_BIN_LOG::signal_update()
{
  /*
    We shouldn't set binlog_last_valid_pos for relay log which results in
    invalid value in the global variable binlog_last_valid_pos
  */
  if (!is_relay_log)
  {
      set_binlog_last_valid_pos(my_b_tell(&log_file));
  }

  DBUG_ENTER("MYSQL_BIN_LOG::signal_update");
  pthread_cond_broadcast(&update_cond);
  DBUG_VOID_RETURN;
}

#ifdef __NT__
static void print_buffer_to_nt_eventlog(enum loglevel level, char *buff,
                                        size_t length, size_t buffLen)
{
  HANDLE event;
  char   *buffptr= buff;
  DBUG_ENTER("print_buffer_to_nt_eventlog");

  /* Add ending CR/LF's to string, overwrite last chars if necessary */
  strmov(buffptr+min(length, buffLen-5), "\r\n\r\n");

  setup_windows_event_source();
  if ((event= RegisterEventSource(NULL,"MySQL")))
  {
    switch (level) {
      case ERROR_LEVEL:
        ReportEvent(event, EVENTLOG_ERROR_TYPE, 0, MSG_DEFAULT, NULL, 1, 0,
                    (LPCSTR*)&buffptr, NULL);
        break;
      case WARNING_LEVEL:
        ReportEvent(event, EVENTLOG_WARNING_TYPE, 0, MSG_DEFAULT, NULL, 1, 0,
                    (LPCSTR*) &buffptr, NULL);
        break;
      case INFORMATION_LEVEL:
        ReportEvent(event, EVENTLOG_INFORMATION_TYPE, 0, MSG_DEFAULT, NULL, 1,
                    0, (LPCSTR*) &buffptr, NULL);
        break;
    }
    DeregisterEventSource(event);
  }

  DBUG_VOID_RETURN;
}
#endif /* __NT__ */


#ifndef EMBEDDED_LIBRARY
static void print_buffer_to_file(enum loglevel level, const char *buffer,
                                 size_t length)
{
  time_t skr;
  struct tm tm_tmp;
  struct tm *start;
  DBUG_ENTER("print_buffer_to_file");
  DBUG_PRINT("enter",("buffer: %s", buffer));

  VOID(pthread_mutex_lock(&LOCK_error_log));

  skr= my_time(0);
  localtime_r(&skr, &tm_tmp);
  start=&tm_tmp;

  fprintf(stderr, "%02d%02d%02d %2d:%02d:%02d [%s] %.*s\n",
          start->tm_year % 100,
          start->tm_mon+1,
          start->tm_mday,
          start->tm_hour,
          start->tm_min,
          start->tm_sec,
          (level == ERROR_LEVEL ? "ERROR" : level == WARNING_LEVEL ?
           "Warning" : "Note"),
          (int) length, buffer);

  fflush(stderr);

  VOID(pthread_mutex_unlock(&LOCK_error_log));
  DBUG_VOID_RETURN;
}

/**
  Prints a printf style message to the error log and, under NT, to the
  Windows event log.

  This function prints the message into a buffer and then sends that buffer
  to other functions to write that message to other logging sources.

  @param level          The level of the msg significance
  @param format         Printf style format of message
  @param args           va_list list of arguments for the message

  @returns
    The function always returns 0. The return value is present in the
    signature to be compatible with other logging routines, which could
    return an error (e.g. logging to the log tables)
*/
int vprint_msg_to_log(enum loglevel level, const char *format, va_list args)
{
  char   buff[1024];
  size_t length;
  DBUG_ENTER("vprint_msg_to_log");

  length= my_vsnprintf(buff, sizeof(buff), format, args);
  print_buffer_to_file(level, buff, length);

#ifdef __NT__
  print_buffer_to_nt_eventlog(level, buff, length, sizeof(buff));
#endif

  DBUG_RETURN(0);
}
#endif /* EMBEDDED_LIBRARY */


void sql_print_error(const char *format, ...) 
{
  va_list args;
  DBUG_ENTER("sql_print_error");

  va_start(args, format);
  error_log_print(ERROR_LEVEL, format, args);
  va_end(args);

  DBUG_VOID_RETURN;
}


void sql_print_warning(const char *format, ...) 
{
  va_list args;
  DBUG_ENTER("sql_print_warning");

  va_start(args, format);
  error_log_print(WARNING_LEVEL, format, args);
  va_end(args);

  DBUG_VOID_RETURN;
}


void sql_print_information(const char *format, ...) 
{
  va_list args;
  DBUG_ENTER("sql_print_information");

  va_start(args, format);
  error_log_print(INFORMATION_LEVEL, format, args);
  va_end(args);

  DBUG_VOID_RETURN;
}


/********* transaction coordinator log for 2pc - mmap() based solution *******/

/*
  the log consists of a file, mmapped to a memory.
  file is divided on pages of tc_log_page_size size.
  (usable size of the first page is smaller because of log header)
  there's PAGE control structure for each page
  each page (or rather PAGE control structure) can be in one of three
  states - active, syncing, pool.
  there could be only one page in active or syncing states,
  but many in pool - pool is fifo queue.
  usual lifecycle of a page is pool->active->syncing->pool
  "active" page - is a page where new xid's are logged.
  the page stays active as long as syncing slot is taken.
  "syncing" page is being synced to disk. no new xid can be added to it.
  when the sync is done the page is moved to a pool and an active page
  becomes "syncing".

  the result of such an architecture is a natural "commit grouping" -
  If commits are coming faster than the system can sync, they do not
  stall. Instead, all commit that came since the last sync are
  logged to the same page, and they all are synced with the next -
  one - sync. Thus, thought individual commits are delayed, throughput
  is not decreasing.

  when a xid is added to an active page, the thread of this xid waits
  for a page's condition until the page is synced. when syncing slot
  becomes vacant one of these waiters is awaken to take care of syncing.
  it syncs the page and signals all waiters that the page is synced.
  PAGE::waiters is used to count these waiters, and a page may never
  become active again until waiters==0 (that is all waiters from the
  previous sync have noticed the sync was completed)

  note, that the page becomes "dirty" and has to be synced only when a
  new xid is added into it. Removing a xid from a page does not make it
  dirty - we don't sync removals to disk.
*/

ulong tc_log_page_waits= 0;

#ifdef HAVE_MMAP

#define TC_LOG_HEADER_SIZE (sizeof(tc_log_magic)+1)

static const char tc_log_magic[]={(char) 254, 0x23, 0x05, 0x74};

ulong opt_tc_log_size= TC_LOG_MIN_SIZE;
ulong tc_log_max_pages_used=0, tc_log_page_size=0, tc_log_cur_pages_used=0;

int TC_LOG_MMAP::open(const char *opt_name)
{
  uint i;
  bool crashed=FALSE;
  PAGE *pg;

  DBUG_ASSERT(total_ha_2pc > 1);
  DBUG_ASSERT(opt_name && opt_name[0]);

  tc_log_page_size= my_getpagesize();
  DBUG_ASSERT(TC_LOG_PAGE_SIZE % tc_log_page_size == 0);

  fn_format(logname,opt_name,mysql_data_home,"",MY_UNPACK_FILENAME);
  if ((fd= my_open(logname, O_RDWR, MYF(0))) < 0)
  {
    if (my_errno != ENOENT)
      goto err;
    if (using_heuristic_recover())
      return 1;
    if ((fd= my_create(logname, CREATE_MODE, O_RDWR, MYF(MY_WME))) < 0)
      goto err;
    inited=1;
    file_length= opt_tc_log_size;
    if (my_chsize(fd, file_length, 0, MYF(MY_WME)))
      goto err;
  }
  else
  {
    inited= 1;
    crashed= TRUE;
    sql_print_information("Recovering after a crash using %s", opt_name);
    if (tc_heuristic_recover)
    {
      sql_print_error("Cannot perform automatic crash recovery when "
                      "--tc-heuristic-recover is used");
      goto err;
    }
    file_length= my_seek(fd, 0L, MY_SEEK_END, MYF(MY_WME+MY_FAE));
    if (file_length == MY_FILEPOS_ERROR || file_length % tc_log_page_size)
      goto err;
  }

  data= (uchar *)my_mmap(0, (size_t)file_length, PROT_READ|PROT_WRITE,
                        MAP_NOSYNC|MAP_SHARED, fd, 0);
  if (data == MAP_FAILED)
  {
    my_errno=errno;
    goto err;
  }
  inited=2;

  npages=(uint)file_length/tc_log_page_size;
  DBUG_ASSERT(npages >= 3);             // to guarantee non-empty pool
  if (!(pages=(PAGE *)my_malloc(npages*sizeof(PAGE), MYF(MY_WME|MY_ZEROFILL))))
    goto err;
  inited=3;
  for (pg=pages, i=0; i < npages; i++, pg++)
  {
    pg->next=pg+1;
    pg->waiters=0;
    pg->state=POOL;
    pthread_mutex_init(&pg->lock, MY_MUTEX_INIT_FAST);
    pthread_cond_init (&pg->cond, 0);
    pg->start=(my_xid *)(data + i*tc_log_page_size);
    pg->end=(my_xid *)(pg->start + tc_log_page_size);
    pg->size=pg->free=tc_log_page_size/sizeof(my_xid);
  }
  pages[0].size=pages[0].free=
                (tc_log_page_size-TC_LOG_HEADER_SIZE)/sizeof(my_xid);
  pages[0].start=pages[0].end-pages[0].size;
  pages[npages-1].next=0;
  inited=4;

  if (crashed && recover())
      goto err;

  memcpy(data, tc_log_magic, sizeof(tc_log_magic));
  data[sizeof(tc_log_magic)]= (uchar)total_ha_2pc;
  my_msync(fd, data, tc_log_page_size, MS_SYNC);
  inited=5;

  pthread_mutex_init(&LOCK_sync,    MY_MUTEX_INIT_FAST);
  pthread_mutex_init(&LOCK_active,  MY_MUTEX_INIT_FAST);
  pthread_mutex_init(&LOCK_pool,    MY_MUTEX_INIT_FAST);
  pthread_cond_init(&COND_active, 0);
  pthread_cond_init(&COND_pool, 0);

  inited=6;

  syncing= 0;
  active=pages;
  pool=pages+1;
  pool_last=pages+npages-1;

  return 0;

err:
  close();
  return 1;
}

/**
  there is no active page, let's got one from the pool.

  Two strategies here:
    -# take the first from the pool
    -# if there're waiters - take the one with the most free space.

  @todo
    TODO page merging. try to allocate adjacent page first,
    so that they can be flushed both in one sync
*/

void TC_LOG_MMAP::get_active_from_pool()
{
  PAGE **p, **best_p=0;
  int best_free;

  if (syncing)
    pthread_mutex_lock(&LOCK_pool);

  do
  {
    best_p= p= &pool;
    if ((*p)->waiters == 0) // can the first page be used ?
      break;                // yes - take it.

    best_free=0;            // no - trying second strategy
    for (p=&(*p)->next; *p; p=&(*p)->next)
    {
      if ((*p)->waiters == 0 && (*p)->free > best_free)
      {
        best_free=(*p)->free;
        best_p=p;
      }
    }
  }
  while ((*best_p == 0 || best_free == 0) && overflow());

  active=*best_p;
  if (active->free == active->size) // we've chosen an empty page
  {
    tc_log_cur_pages_used++;
    set_if_bigger(tc_log_max_pages_used, tc_log_cur_pages_used);
  }

  if ((*best_p)->next)              // unlink the page from the pool
    *best_p=(*best_p)->next;
  else
    pool_last=*best_p;

  if (syncing)
    pthread_mutex_unlock(&LOCK_pool);
}

/**
  @todo
  perhaps, increase log size ?
*/
int TC_LOG_MMAP::overflow()
{
  /*
    simple overflow handling - just wait
    TODO perhaps, increase log size ?
    let's check the behaviour of tc_log_page_waits first
  */
  tc_log_page_waits++;
  pthread_cond_wait(&COND_pool, &LOCK_pool);
  return 1; // always return 1
}

/**
  Record that transaction XID is committed on the persistent storage.

    This function is called in the middle of two-phase commit:
    First all resources prepare the transaction, then tc_log->log() is called,
    then all resources commit the transaction, then tc_log->unlog() is called.

    All access to active page is serialized but it's not a problem, as
    we're assuming that fsync() will be a main bottleneck.
    That is, parallelizing writes to log pages we'll decrease number of
    threads waiting for a page, but then all these threads will be waiting
    for a fsync() anyway

   If tc_log == MYSQL_LOG then tc_log writes transaction to binlog and
   records XID in a special Xid_log_event.
   If tc_log = TC_LOG_MMAP then xid is written in a special memory-mapped
   log.

  @retval
    0  - error
  @retval
    \# - otherwise, "cookie", a number that will be passed as an argument
    to unlog() call. tc_log can define it any way it wants,
    and use for whatever purposes. TC_LOG_MMAP sets it
    to the position in memory where xid was logged to.
*/

int TC_LOG_MMAP::log_xid(THD *thd, my_xid xid, bool async, handlerton *ht, int32 pending,
                         bool *full)
{
  int err;
  PAGE *p;
  ulong cookie;

  pthread_mutex_lock(&LOCK_active);

  /*
    if active page is full - just wait...
    frankly speaking, active->free here accessed outside of mutex
    protection, but it's safe, because it only means we may miss an
    unlog() for the active page, and we're not waiting for it here -
    unlog() does not signal COND_active.
  */
  while (unlikely(active && active->free == 0))
    pthread_cond_wait(&COND_active, &LOCK_active);

  /* no active page ? take one from the pool */
  if (active == 0)
    get_active_from_pool();

  p=active;
  pthread_mutex_lock(&p->lock);

  /* searching for an empty slot */
  while (*p->ptr)
  {
    p->ptr++;
    DBUG_ASSERT(p->ptr < p->end);               // because p->free > 0
  }

  /* found! store xid there and mark the page dirty */
  cookie= (ulong)((uchar *)p->ptr - data);      // can never be zero
  *p->ptr++= xid;
  p->free--;
  p->state= DIRTY;

  /* to sync or not to sync - this is the question */
  pthread_mutex_unlock(&LOCK_active);
  pthread_mutex_lock(&LOCK_sync);
  pthread_mutex_unlock(&p->lock);

  if (syncing)
  {                                          // somebody's syncing. let's wait
    p->waiters++;
    /*
      note - it must be while (), not do ... while () here
      as p->state may be not DIRTY when we come here
    */
    while (p->state == DIRTY && syncing)
      pthread_cond_wait(&p->cond, &LOCK_sync);
    p->waiters--;
    err= p->state == ERROR;
    if (p->state != DIRTY)                   // page was synced
    {
      if (p->waiters == 0)
        pthread_cond_signal(&COND_pool);     // in case somebody's waiting
      pthread_mutex_unlock(&LOCK_sync);
      goto done;                             // we're done
    }
  }                                          // page was not synced! do it now
  DBUG_ASSERT(active == p && syncing == 0);
  pthread_mutex_lock(&LOCK_active);
  syncing=p;                                 // place is vacant - take it
  active=0;                                  // page is not active anymore
  pthread_cond_broadcast(&COND_active);      // in case somebody's waiting
  pthread_mutex_unlock(&LOCK_active);
  pthread_mutex_unlock(&LOCK_sync);
  err= sync();

done:
  return err ? 0 : cookie;
}

int TC_LOG_MMAP::sync()
{
  int err;

  DBUG_ASSERT(syncing != active);

  /*
    sit down and relax - this can take a while...
    note - no locks are held at this point
  */
  err= my_msync(fd, syncing->start, 1, MS_SYNC);

  /* page is synced. let's move it to the pool */
  pthread_mutex_lock(&LOCK_pool);
  pool_last->next=syncing;
  pool_last=syncing;
  syncing->next=0;
  syncing->state= err ? ERROR : POOL;
  pthread_cond_broadcast(&syncing->cond);    // signal "sync done"
  pthread_cond_signal(&COND_pool);           // in case somebody's waiting
  pthread_mutex_unlock(&LOCK_pool);

  /* marking 'syncing' slot free */
  pthread_mutex_lock(&LOCK_sync);
  syncing=0;
  pthread_cond_signal(&active->cond);        // wake up a new syncer
  pthread_mutex_unlock(&LOCK_sync);
  return err;
}

/**
  erase xid from the page, update page free space counters/pointers.
  cookie points directly to the memory where xid was logged.
*/

int TC_LOG_MMAP::unlog(THD *thd, ulong cookie, my_xid xid, bool log_was_full)
{
  PAGE *p=pages+(cookie/tc_log_page_size);
  my_xid *x=(my_xid *)(data+cookie);

  DBUG_ASSERT(*x == xid);
  DBUG_ASSERT(x >= p->start && x < p->end);
  *x=0;

  pthread_mutex_lock(&p->lock);
  p->free++;
  DBUG_ASSERT(p->free <= p->size);
  set_if_smaller(p->ptr, x);
  if (p->free == p->size)               // the page is completely empty
    statistic_decrement(tc_log_cur_pages_used, &LOCK_status);
  if (p->waiters == 0)                 // the page is in pool and ready to rock
    pthread_cond_signal(&COND_pool);   // ping ... for overflow()
  pthread_mutex_unlock(&p->lock);
  return 0;
}

void TC_LOG_MMAP::close()
{
  uint i;
  switch (inited) {
  case 6:
    pthread_mutex_destroy(&LOCK_sync);
    pthread_mutex_destroy(&LOCK_active);
    pthread_mutex_destroy(&LOCK_pool);
    pthread_cond_destroy(&COND_pool);
  case 5:
    data[0]='A'; // garble the first (signature) byte, in case my_delete fails
  case 4:
    for (i=0; i < npages; i++)
    {
      if (pages[i].ptr == 0)
        break;
      pthread_mutex_destroy(&pages[i].lock);
      pthread_cond_destroy(&pages[i].cond);
    }
  case 3:
    my_free((uchar*)pages, MYF(0));
  case 2:
    my_munmap((char*)data, (size_t)file_length);
  case 1:
    my_close(fd, MYF(0));
  }
  if (inited>=5) // cannot do in the switch because of Windows
    my_delete(logname, MYF(MY_WME));
  inited=0;
}

int TC_LOG_MMAP::recover()
{
  HASH xids;
  PAGE *p=pages, *end_p=pages+npages;

  if (memcmp(data, tc_log_magic, sizeof(tc_log_magic)))
  {
    sql_print_error("Bad magic header in tc log");
    goto err1;
  }

  /*
    the first byte after magic signature is set to current
    number of storage engines on startup
  */
  if (data[sizeof(tc_log_magic)] != total_ha_2pc)
  {
    sql_print_error("Recovery failed! You must enable "
                    "exactly %d storage engines that support "
                    "two-phase commit protocol",
                    data[sizeof(tc_log_magic)]);
    goto err1;
  }

  if (hash_init(&xids, &my_charset_bin, tc_log_page_size/3, 0,
                sizeof(my_xid), 0, 0, MYF(0)))
    goto err1;

  for ( ; p < end_p ; p++)
  {
    for (my_xid *x=p->start; x < p->end; x++)
      if (*x && my_hash_insert(&xids, (uchar *)x))
        goto err2; // OOM
  }

  if (ha_recover(&xids))
    goto err2;

  hash_free(&xids);
  bzero(data, (size_t)file_length);
  return 0;

err2:
  hash_free(&xids);
err1:
  sql_print_error("Crash recovery failed. Either correct the problem "
                  "(if it's, for example, out of memory error) and restart, "
                  "or delete tc log and start mysqld with "
                  "--tc-heuristic-recover={commit|rollback}");
  return 1;
}
#endif

TC_LOG *tc_log;
TC_LOG_DUMMY tc_log_dummy;
TC_LOG_MMAP  tc_log_mmap;

/**
  Perform heuristic recovery, if --tc-heuristic-recover was used.

  @note
    no matter whether heuristic recovery was successful or not
    mysqld must exit. So, return value is the same in both cases.

  @retval
    0	no heuristic recovery was requested
  @retval
    1   heuristic recovery was performed
*/

int TC_LOG::using_heuristic_recover()
{
  if (!tc_heuristic_recover)
    return 0;

  sql_print_information("Heuristic crash recovery mode");
  if (ha_recover(0))
    sql_print_error("Heuristic crash recovery failed");
  sql_print_information("Please restart mysqld without --tc-heuristic-recover");
  return 1;
}

/****** transaction coordinator log for 2pc - binlog() based solution ******/
#define TC_LOG_BINLOG MYSQL_BIN_LOG

/**
  @todo
  keep in-memory list of prepared transactions
  (add to list in log(), remove on unlog())
  and copy it to the new binlog if rotated
  but let's check the behaviour of tc_log_page_waits first!
*/

int TC_LOG_BINLOG::open(const char *opt_name)
{
  LOG_INFO log_info;
  int      error= 1;

  DBUG_ASSERT(total_ha_2pc > 1);
  DBUG_ASSERT(opt_name && opt_name[0]);

  pthread_mutex_init(&LOCK_prep_xids, MY_MUTEX_INIT_FAST);
  pthread_cond_init (&COND_prep_xids, 0);

  if (!my_b_inited(&index_file))
  {
    /* There was a failure to open the index file, can't open the binlog */
    cleanup();
    return 1;
  }

  if (using_heuristic_recover())
  {
    /* generate a new binlog to mask a corrupted one */
    open(opt_name, LOG_BIN, 0, WRITE_CACHE, 0, max_binlog_size, 0, TRUE);
    cleanup();
    return 1;
  }

  if ((error= find_log_pos(&log_info, NullS, 1)))
  {
    if (error != LOG_INFO_EOF)
      sql_print_error("find_log_pos() failed (error: %d)", error);
    else
      error= 0;
    goto err;
  }

  {
    const char *errmsg;
    IO_CACHE    log;
    File        file;
    Log_event  *ev=0;
    Format_description_log_event fdle(BINLOG_VERSION);
    char        log_name[FN_REFLEN];
    my_off_t    valid_pos= 0;
    my_off_t    binlog_size;
    MY_STAT     s;

    if (! fdle.is_valid())
      goto err;

    do
    {
      strmake(log_name, log_info.log_file_name, sizeof(log_name)-1);
    } while (!(error= find_next_log(&log_info, 1)));

    if (error !=  LOG_INFO_EOF)
    {
      sql_print_error("find_log_pos() failed (error: %d)", error);
      goto err;
    }

    if ((file= open_binlog(&log, log_name, &errmsg)) < 0)
    {
      sql_print_error("%s", errmsg);
      error= 1;
      goto err;
    }

    if (!my_stat(log_name, &s, MYF(0)))
    {
      sql_print_error("my_stat failed on %s with errno %d",
                      log_name, my_errno);
      error= 1;
      goto err;
    }
    binlog_size= s.st_size;

    if ((ev= Log_event::read_log_event(&log, 0, &fdle, NULL)) &&
        ev->get_type_code() == FORMAT_DESCRIPTION_EVENT &&
        ev->flags & LOG_EVENT_BINLOG_IN_USE_F)
    {
      sql_print_information("Recovering after a crash using %s", opt_name);
      valid_pos= my_b_tell(&log);
      error= recover(&log, (Format_description_log_event *)ev, &valid_pos);
    }
    else
      error=0;

    delete ev;
    end_io_cache(&log);
    my_close(file, MYF(MY_WME));

    if (error)
      goto err;

    /* Trim the crashed binlog file to last valid transaction
       or event (non-transaction) base on valid_pos. */
    if (valid_pos > 0)
    {
      if ((file= my_open(log_name, O_RDWR | O_BINARY, MYF(MY_WME))) < 0)
      {
        sql_print_error("Failed to open the crashed binlog file "
                        "when master server is recovering it.");
        return -1;
      }

      /* Change binlog file size to valid_pos */
      if (valid_pos < binlog_size)
      {
        if (my_chsize(file, valid_pos, 0, MYF(MY_WME)))
        {
          sql_print_error("Failed to trim the crashed binlog file "
                          "when master server is recovering it.");
          my_close(file, MYF(MY_WME));
          return -1;
        }
        else
        {
          char buff1[22];
          char buff2[22];
          char buff3[22];
          sql_print_information("Crashed binlog file %s size is %s, "
                                "but recovered up to %s. Binlog trimmed "
                                "to %s bytes.",
                                log_name,
                                llstr(binlog_size, buff1),
                                llstr(valid_pos, buff2),
                                llstr(valid_pos, buff3));
        }
      }

      /* Clear LOG_EVENT_BINLOG_IN_USE_F */
      my_off_t offset= BIN_LOG_HEADER_SIZE + FLAGS_OFFSET;
      uchar flags= 0;
      if (my_pwrite(file, &flags, 1, offset, MYF(0)) != 1)
      {
        sql_print_error("Failed to clear LOG_EVENT_BINLOG_IN_USE_F "
                        "for the crashed binlog file when master "
                        "server is recovering it.");
        my_close(file, MYF(MY_WME));
        return -1;
      }

      my_close(file, MYF(MY_WME));
    } //end if
  }

err:
  return error;
}

/** This is called on shutdown, after ha_panic. */
void TC_LOG_BINLOG::close()
{
  DBUG_ASSERT(prepared_xids==0);
  pthread_mutex_destroy(&LOCK_prep_xids);
  pthread_cond_destroy (&COND_prep_xids);
}

/**
  @todo
  group commit

  @retval
    0    error
  @retval
    1    success
*/
int TC_LOG_BINLOG::log_xid(THD *thd, my_xid xid, bool async, handlerton *ht, int32 pending,
                           bool *full)
{
  DBUG_ENTER("TC_LOG_BINLOG::log");
  Xid_log_event xle(thd, xid);
  binlog_trx_data *trx_data=
    (binlog_trx_data*) thd_get_ha_data(thd, binlog_hton);
  /*
    We always commit the entire transaction when writing an XID. Also
    note that the return value is inverted.
   */
  DBUG_RETURN(!binlog_end_trans(thd, trx_data, &xle, TRUE, async, ht, pending, full));
}

int TC_LOG_BINLOG::unlog(THD *thd, ulong cookie, my_xid xid, bool log_was_full)
{
  DBUG_ENTER("TC_LOG_BINLOG::unlog");
  pthread_mutex_lock(&LOCK_prep_xids);
  DBUG_ASSERT(prepared_xids > 0);
  if (--prepared_xids == 0) {
    DBUG_PRINT("info", ("prepared_xids=%lu", prepared_xids));
    pthread_cond_broadcast(&COND_prep_xids);
  }
  pthread_mutex_unlock(&LOCK_prep_xids);
  DBUG_RETURN(rotate_and_purge(thd, 0, log_was_full));     // as ::write() did not rotate
}

int TC_LOG_BINLOG::recover(IO_CACHE *log, Format_description_log_event *fdle,
                           my_off_t *valid_pos)
{
  Log_event  *ev;
  HASH xids;
  MEM_ROOT mem_root;
  /*
    The flag is used for handling the case that a transaction
    is partially written to the binlog.
  */
  bool in_transaction= FALSE;
 
  if (! fdle->is_valid() ||
      hash_init(&xids, &my_charset_bin, TC_LOG_PAGE_SIZE/3, 0,
                sizeof(my_xid), 0, 0, MYF(0)))
    goto err1;

  init_alloc_root(&mem_root, TC_LOG_PAGE_SIZE, TC_LOG_PAGE_SIZE);

  fdle->flags&= ~LOG_EVENT_BINLOG_IN_USE_F; // abort on the first error

  while ((ev= Log_event::read_log_event(log,0,fdle,NULL)) && ev->is_valid())
  {
    if (ev->get_type_code() == QUERY_EVENT &&
        !strcmp(((Query_log_event*)ev)->query, "BEGIN"))
      in_transaction= TRUE;
    
    if (ev->get_type_code() == QUERY_EVENT &&
        !strcmp(((Query_log_event*)ev)->query, "COMMIT"))
    {
      DBUG_ASSERT(in_transaction == TRUE);
      in_transaction= FALSE;
    }
    else if (ev->get_type_code() == XID_EVENT)
    {
      /* MEMCACHED_RESOLVE: currently binlog from memcached,
         might not have MySQL transaction marks, so quote this assert
         out first. Will reinstate later.
         DBUG_ASSERT(in_transaction == TRUE);
      */
      in_transaction= FALSE;
      Xid_log_event *xev=(Xid_log_event *)ev;
      uchar *x= (uchar *) memdup_root(&mem_root, (uchar*) &xev->xid,
                                      sizeof(xev->xid));
      if (!x || my_hash_insert(&xids, x))
        goto err2;
    }

    /*
      Recorded valid position for the crashed binlog file
      which did not contain incorrect events. The following
      positions increase the variable valid_pos:
      
      1 -
        ...
        <---> HERE IS VALID <--->
        BEGIN
        ...
        COMMIT
        ...

      2 -
        ...
        <---> HERE IS VALID <--->
        DDL/UTILITY
        ...

      In other words, the following positions do not increase
      the variable valid_pos:

      1 -
        BEGIN
        <---> HERE IS VALID <--->
        ...
    */
    if (!log->error && !in_transaction)
      *valid_pos= my_b_tell(log);

    delete ev;
  }

  if (ha_recover(&xids))
    goto err2;

  free_root(&mem_root, MYF(0));
  hash_free(&xids);
  return 0;

err2:
  free_root(&mem_root, MYF(0));
  hash_free(&xids);
err1:
  sql_print_error("Crash recovery failed. Either correct the problem "
                  "(if it's, for example, out of memory error) and restart, "
                  "or delete (or rename) binary log and start mysqld with "
                  "--tc-heuristic-recover={commit|rollback}");
  return 1;
}


#ifdef INNODB_COMPATIBILITY_HOOKS
/**
  Checks binlog status.
  @return true if the binlog is open.
*/
extern "C"
my_bool mysql_bin_log_is_open(void)
{
  return mysql_bin_log.is_open();
}
/**
  Get the file name of the MySQL binlog.
  @return the name of the binlog file
*/
extern "C"
const char* mysql_bin_log_file_name(void)
{
  return mysql_bin_log.get_log_fname();
}
/**
  Get the current position of the MySQL binlog.
  @return byte offset from the beginning of the binlog
*/
extern "C"
ulonglong mysql_bin_log_file_pos(void)
{
  return (ulonglong) mysql_bin_log.get_log_file()->pos_in_file;
}

/** Get the file name of the MySQL relaylog from active_mi
 * @return the name of the relaylog file from active_mi
*/
extern "C"
const char* active_relay_log_file_name(void)
{
  return active_mi->rli.event_relay_log_name;
}

/**
  Get the current position of the MySQL relaylog from active_mi
  @return byte offset from the beginning of the relaylog from active_mi
*/
extern "C"
ulonglong active_relay_log_file_pos(void)
{
  return (ulonglong) active_mi->rli.future_event_relay_log_pos;
}

/** Get the file name of the MySQL binlog from active_mi
 * @return the name of the MySQL binlog from active_mi
*/
extern "C"
const char* active_bin_log_file_name(void)
{
  return active_mi->rli.group_master_log_name;
}

/**
  Get the current position of the MySQL relaylog from active_mi.
  @return byte offset from the beginning of the relaylog from active_mi
*/
extern "C"
ulonglong active_bin_log_file_pos(void)
{
  return (ulonglong) active_mi->rli.future_group_master_log_pos;
}

#endif /* INNODB_COMPATIBILITY_HOOKS */


struct st_mysql_storage_engine binlog_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

mysql_declare_plugin(binlog)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &binlog_storage_engine,
  "binlog",
  "MySQL AB",
  "This is a pseudo storage engine to represent the binlog in a transaction",
  PLUGIN_LICENSE_GPL,
  binlog_init, /* Plugin Init */
  NULL, /* Plugin Deinit */
  0x0100 /* 1.0 */,
  NULL,                       /* status variables                */
  NULL,                       /* system variables                */
  NULL                        /* config options                  */
}
mysql_declare_plugin_end;
