/*
 * Copyright (c) 2002-2010 BalaBit IT Ltd, Budapest, Hungary
 * Copyright (c) 1998-2010 Balázs Scheidler
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */

#include "afsql.h"

#if ENABLE_SQL

#include "logqueue.h"
#include "template/templates.h"
#include "messages.h"
#include "misc.h"
#include "stats.h"
#include "apphook.h"
#include "timeutils.h"
#include "mainloop-worker.h"

#include <dbi/dbi.h>
#include <string.h>
#include <openssl/md5.h>

/* field flags */
enum
{
  AFSQL_FF_DEFAULT = 0x0001,
};

/* destination driver flags */
enum
{
  AFSQL_DDF_EXPLICIT_COMMITS = 0x0001,
  AFSQL_DDF_DONT_CREATE_TABLES = 0x0002,
};

typedef struct _AFSqlField
{
  guint32 flags;
  gchar *name;
  gchar *type;
  LogTemplate *value;
} AFSqlField;

/**
 * AFSqlDestDriver:
 *
 * This structure encapsulates an SQL destination driver. SQL insert
 * statements are generated from a separate thread because of the blocking
 * nature of the DBI API. It is ensured that while the thread is running,
 * the reference count to the driver structure is increased, thus the db
 * thread can read any of the fields in this structure. To do anything more
 * than simple reading out a value, some kind of locking mechanism shall be
 * used.
 **/
typedef struct _AFSqlDestDriver
{
  LogDestDriver super;
  /* read by the db thread */
  gchar *type;
  gchar *host;
  gchar *port;
  gchar *user;
  gchar *password;
  gchar *database;
  gchar *encoding;
  GList *columns;
  GList *values;
  GList *indexes;
  LogTemplate *table;
  gint fields_len;
  AFSqlField *fields;
  gchar *null_value;
  gint time_reopen;
  gint num_retries;
  gint flush_lines;
  gint flush_lines_queued;
  gint flags;
  gboolean ignore_tns_config;
  GList *session_statements;

  LogTemplateOptions template_options;

  StatsCounterItem *dropped_messages;
  StatsCounterItem *stored_messages;

  /* shared by the main/db thread */
  GMutex *db_thread_mutex;
  GCond *db_thread_wakeup_cond;
  gboolean db_thread_terminate;
  gboolean db_thread_suspended;
  GTimeVal db_thread_suspend_target;
  LogQueue *queue;
  /* used exclusively by the db thread */
  gint32 seq_num;
  dbi_conn dbi_ctx;
  GHashTable *validated_tables;
  guint32 failed_message_counter;
  gboolean enable_indexes;
  WorkerOptions worker_options;
  gboolean transaction_active;
} AFSqlDestDriver;

static gboolean dbi_initialized = FALSE;
static dbi_inst dbi_instance;
static const char *s_oracle = "oracle";
static const char *s_freetds = "freetds";

#define MAX_FAILED_ATTEMPTS 3

void
afsql_dd_set_type(LogDriver *s, const gchar *type)
{
  AFSqlDestDriver *self = (AFSqlDestDriver *) s;

  g_free(self->type);
  if (strcmp(type, "mssql") == 0)
    type = s_freetds;
  self->type = g_strdup(type);
}

void
afsql_dd_set_host(LogDriver *s, const gchar *host)
{
  AFSqlDestDriver *self = (AFSqlDestDriver *) s;

  g_free(self->host);
  self->host = g_strdup(host);
}

gboolean afsql_dd_check_port(const gchar *port)
{
  /* only digits (->numbers) are allowed */
  int len = strlen(port);
  for (int i = 0; i < len; ++i)
    if (port[i] < '0' || port[i] > '9')
      return FALSE;
  return TRUE;
}

void
afsql_dd_set_port(LogDriver *s, const gchar *port)
{
  AFSqlDestDriver *self = (AFSqlDestDriver *) s;

  g_free(self->port);
  self->port = g_strdup(port);
}

void
afsql_dd_set_user(LogDriver *s, const gchar *user)
{
  AFSqlDestDriver *self = (AFSqlDestDriver *) s;

  g_free(self->user);
  self->user = g_strdup(user);
}

void
afsql_dd_set_password(LogDriver *s, const gchar *password)
{
  AFSqlDestDriver *self = (AFSqlDestDriver *) s;

  g_free(self->password);
  self->password = g_strdup(password);
}

void
afsql_dd_set_database(LogDriver *s, const gchar *database)
{
  AFSqlDestDriver *self = (AFSqlDestDriver *) s;

  g_free(self->database);
  self->database = g_strdup(database);
}

void
afsql_dd_set_table(LogDriver *s, const gchar *table)
{
  AFSqlDestDriver *self = (AFSqlDestDriver *) s;

  log_template_compile(self->table, table, NULL);
}

void
afsql_dd_set_columns(LogDriver *s, GList *columns)
{
  AFSqlDestDriver *self = (AFSqlDestDriver *) s;

  string_list_free(self->columns);
  self->columns = columns;
}

void
afsql_dd_set_indexes(LogDriver *s, GList *indexes)
{
  AFSqlDestDriver *self = (AFSqlDestDriver *) s;

  string_list_free(self->indexes);
  if (indexes)
    {
      self->enable_indexes = TRUE;
      self->indexes = indexes;
    }
  else
    {
      self->enable_indexes = FALSE;
      self->indexes = g_list_append(NULL,"");
    }
}

void
afsql_dd_set_values(LogDriver *s, GList *values)
{
  AFSqlDestDriver *self = (AFSqlDestDriver *) s;

  string_list_free(self->values);
  self->values = values;
}

void
afsql_dd_set_null_value(LogDriver *s, const gchar *null)
{
  AFSqlDestDriver *self = (AFSqlDestDriver *) s;

  if (self->null_value)
    g_free(self->null_value);
  self->null_value = g_strdup(null);
}

void
afsql_dd_set_retries(LogDriver *s, gint num_retries)
{
  AFSqlDestDriver *self = (AFSqlDestDriver *) s;

  if (num_retries < 1)
    {
      self->num_retries = 1;
    }
  else
    {
      self->num_retries = num_retries;
    }
}

void
afsql_dd_set_ignore_tns_config(LogDriver *s, const gboolean ignore_tns_config)
{
  AFSqlDestDriver *self = (AFSqlDestDriver *) s;

  self->ignore_tns_config = ignore_tns_config;
}

void
afsql_dd_set_frac_digits(LogDriver *s, gint frac_digits)
{
  AFSqlDestDriver *self = (AFSqlDestDriver *) s;

  self->template_options.frac_digits = frac_digits;
}

void
afsql_dd_set_send_time_zone(LogDriver *s, const gchar *send_time_zone)
{
  AFSqlDestDriver *self = (AFSqlDestDriver *) s;

  self->template_options.time_zone[LTZ_SEND] = g_strdup(send_time_zone);
}

void
afsql_dd_set_local_time_zone(LogDriver *s, const gchar *local_time_zone)
{
  AFSqlDestDriver *self = (AFSqlDestDriver *) s;

  self->template_options.time_zone[LTZ_LOCAL] = g_strdup(local_time_zone);
}

void
afsql_dd_set_flush_lines(LogDriver *s, gint flush_lines)
{
  AFSqlDestDriver *self = (AFSqlDestDriver *) s;

  self->flush_lines = flush_lines;
}

void
afsql_dd_set_session_statements(LogDriver *s, GList *session_statements)
{
  AFSqlDestDriver *self = (AFSqlDestDriver *) s;

  self->session_statements = session_statements;
}

void
afsql_dd_set_flags(LogDriver *s, gint flags)
{
  AFSqlDestDriver *self = (AFSqlDestDriver *) s;

  self->flags = flags;
}

/**
 * afsql_dd_run_query:
 *
 * Run an SQL query on the connected database.
 *
 * NOTE: This function can only be called from the database thread.
 **/
static gboolean
afsql_dd_run_query(AFSqlDestDriver *self, const gchar *query, gboolean silent, dbi_result *result)
{
  dbi_result db_res;

  msg_debug("Running SQL query",
            evt_tag_str("query", query),
            NULL);

  db_res = dbi_conn_query(self->dbi_ctx, query);
  if (!db_res)
    {
      const gchar *dbi_error;

      if (!silent)
        {
          dbi_conn_error(self->dbi_ctx, &dbi_error);
          msg_error("Error running SQL query",
                    evt_tag_str("type", self->type),
                    evt_tag_str("host", self->host),
                    evt_tag_str("port", self->port),
                    evt_tag_str("user", self->user),
                    evt_tag_str("database", self->database),
                    evt_tag_str("error", dbi_error),
                    evt_tag_str("query", query),
                    NULL);
        }
      return FALSE;
    }
  if (result)
    *result = db_res;
  else
    dbi_result_free(db_res);
  return TRUE;
}

static gboolean
afsql_dd_check_sql_identifier(gchar *token, gboolean sanitize)
{
  gint i;

  for (i = 0; token[i]; i++)
    {
      if (!((token[i] == '.') || (token[i] == '_') || (i && token[i] >= '0' && token[i] <= '9') || (g_ascii_tolower(token[i]) >= 'a' && g_ascii_tolower(token[i]) <= 'z')))
        {
          if (sanitize)
            token[i] = '_';
          else
            return FALSE;
        }
    }
  return TRUE;
}

/**
 * afsql_dd_handle_transaction_error:
 *
 * Handle errors inside during a SQL transaction (e.g. INSERT or COMMIT failures).
 *
 * NOTE: This function can only be called from the database thread.
 **/
static void
afsql_dd_handle_transaction_error(AFSqlDestDriver *self)
{
  log_queue_rewind_backlog_all(self->queue);
  self->flush_lines_queued = 0;
}

/**
 * afsql_dd_commit_transaction:
 *
 * Commit SQL transaction.
 *
 * NOTE: This function can only be called from the database thread.
 **/
static gboolean
afsql_dd_commit_transaction(AFSqlDestDriver *self)
{
  gboolean success;

  if (!self->transaction_active)
    return TRUE;

  success = afsql_dd_run_query(self, "COMMIT", FALSE, NULL);
  if (success)
    {
      log_queue_ack_backlog(self->queue, self->flush_lines_queued);
      self->flush_lines_queued = 0;
      self->transaction_active = FALSE;
    }
  else
    {
      msg_error("SQL transaction commit failed, rewinding backlog and starting again",
                NULL);
      afsql_dd_handle_transaction_error(self);
    }
  return success;
}

/**
 * afsql_dd_begin_transaction:
 *
 * Begin SQL transaction.
 *
 * NOTE: This function can only be called from the database thread.
 **/
static gboolean
afsql_dd_begin_transaction(AFSqlDestDriver *self)
{
  gboolean success = TRUE;
  const char *s_begin = "BEGIN";
  if (!strcmp(self->type, s_freetds))
    {
      /* the mssql requires this command */
      s_begin = "BEGIN TRANSACTION";
    }

  if (strcmp(self->type, s_oracle) != 0)
    {
      /* oracle db has no BEGIN TRANSACTION command, it implicitly starts one, after every commit. */
      success = afsql_dd_run_query(self, s_begin, FALSE, NULL);
    }
  self->transaction_active = TRUE;
  return success;
}

static gboolean
afsql_dd_rollback_transaction(AFSqlDestDriver *self)
{
  if (!self->transaction_active)
    return TRUE;

  self->transaction_active = FALSE;

  return afsql_dd_run_query(self, "ROLLBACK", FALSE, NULL);
}

static gboolean
afsql_dd_begin_new_transaction(AFSqlDestDriver *self)
{
  if (self->transaction_active)
    {
      if (!afsql_dd_commit_transaction(self))
        {
          afsql_dd_rollback_transaction(self);
          return FALSE;
        }
    }

  return afsql_dd_begin_transaction(self);
}

/**
 * afsql_dd_create_index:
 *
 * This function creates an index for the column specified and returns
 * TRUE to indicate success.
 *
 * NOTE: This function can only be called from the database thread.
 **/
static gboolean
afsql_dd_create_index(AFSqlDestDriver *self, gchar *table, gchar *column)
{
  GString *query_string;
  gboolean success = TRUE;

  query_string = g_string_sized_new(64);

  if (strcmp(self->type, s_oracle) == 0)
    {
      /* NOTE: oracle index indentifier length is max 30 characters
       * so we use the first 30 characters of the table_column md5 hash */
      if ((strlen(table) + strlen(column)) > 25)
        {

#if ENABLE_SSL
          guchar hash[MD5_DIGEST_LENGTH];
          gchar hash_str[31];
          gchar *cat = g_strjoin("_", table, column, NULL);

          MD5((guchar *)cat, strlen(cat), hash);

          g_free(cat);

          format_hex_string(hash, sizeof(hash), hash_str, sizeof(hash_str));
          hash_str[0] = 'i';
          g_string_printf(query_string, "CREATE INDEX %s ON %s (%s)",
              hash_str, table, column);
#else
          msg_warning("The name of the index would be too long for Oracle to handle and OpenSSL was not detected which would be used to generate a shorter name. Please enable SSL support in order to use this combination.",
                      evt_tag_str("table", table),
                      evt_tag_str("column", column),
                      NULL);
#endif
        }
      else
        g_string_printf(query_string, "CREATE INDEX %s_%s_idx ON %s (%s)",
            table, column, table, column);
    }
  else
    g_string_printf(query_string, "CREATE INDEX %s_%s_idx ON %s (%s)",
                    table, column, table, column);
  if (!afsql_dd_run_query(self, query_string->str, FALSE, NULL))
    {
      msg_error("Error adding missing index",
                evt_tag_str("table", table),
                evt_tag_str("column", column),
                NULL);
      success = FALSE;
    }
  g_string_free(query_string, TRUE);
  return success;
}

/**
 * afsql_dd_validate_table:
 *
 * Check if the given table exists in the database. If it doesn't
 * create it, if it does, check if all the required fields are
 * present and create them if they don't.
 *
 * NOTE: This function can only be called from the database thread.
 **/
static gboolean
afsql_dd_validate_table(AFSqlDestDriver *self, gchar *table)
{
  GString *query_string;
  dbi_result db_res = NULL;
  gboolean success = FALSE;
  gboolean new_transaction_started = FALSE;
  gint i;

  if (self->flags & AFSQL_DDF_DONT_CREATE_TABLES)
    return TRUE;

  afsql_dd_check_sql_identifier(table, TRUE);

  if (g_hash_table_lookup(self->validated_tables, table))
    return TRUE;

  // when table not found, we should request a new txn (and close/commit the existing one)

  if (!afsql_dd_begin_new_transaction(self))
    {
      msg_error("Starting new transaction for querying(SELECT) table has failed",
                evt_tag_str("table", table),
                NULL);
      return FALSE;
    }

  query_string = g_string_sized_new(32);
  g_string_printf(query_string, "SELECT * FROM %s WHERE 0=1", table);
  if (afsql_dd_run_query(self, query_string->str, TRUE, &db_res))
    {
      /* table exists, check structure */
      success = TRUE;
      for (i = 0; success && (i < self->fields_len); i++)
        {
          if (dbi_result_get_field_idx(db_res, self->fields[i].name) == 0)
            {
              GList *l;
              if (!new_transaction_started)
                {
                  if (!afsql_dd_begin_new_transaction(self))
                    {
                      msg_error("Starting new transaction for modifying(ALTER) table has failed",
                                evt_tag_str("table", table),
                                NULL);
                      success = FALSE;
                      break;
                    }
                  new_transaction_started = TRUE;
                }
              /* field does not exist, add this column */
              g_string_printf(query_string, "ALTER TABLE %s ADD %s %s", table, self->fields[i].name, self->fields[i].type);
              if (!afsql_dd_run_query(self, query_string->str, FALSE, NULL))
                {
                  msg_error("Error adding missing column, giving up",
                            evt_tag_str("table", table),
                            evt_tag_str("column", self->fields[i].name),
                            NULL);
                  success = FALSE;
                  break;
                }
              if (self->enable_indexes)
                {
                  for (l = self->indexes; l; l = l->next)
                    {
                      if (strcmp((gchar *) l->data, self->fields[i].name) == 0)
                        {
                          /* this is an indexed column, create index */
                          afsql_dd_create_index(self, table, self->fields[i].name);
                        }
                    }
                }
            }
        }
      if (db_res)
        dbi_result_free(db_res);
    }
  else
    {
      /* table does not exist, create it */
      if (!afsql_dd_begin_new_transaction(self))
        {
          msg_error("Starting new transaction for table creation has failed",
                    evt_tag_str("table", table),
                    NULL);
          success = FALSE;
        }

      new_transaction_started = TRUE;

      g_string_printf(query_string, "CREATE TABLE %s (", table);
      for (i = 0; i < self->fields_len; i++)
        {
          g_string_append_printf(query_string, "%s %s", self->fields[i].name, self->fields[i].type);
          if (i != self->fields_len - 1)
            g_string_append(query_string, ", ");
        }
      g_string_append(query_string, ")");
      if (afsql_dd_run_query(self, query_string->str, FALSE, NULL))
        {
          GList *l;

          success = TRUE;
          if (self->enable_indexes)
            {
              for (l = self->indexes; l; l = l->next)
                {
                  afsql_dd_create_index(self, table, (gchar *) l->data);
                }
            }
        }
      else
        {
          msg_error("Error creating table, giving up",
                    evt_tag_str("table", table),
                    NULL);
          success = FALSE;
        }
    }
  if (success)
    {
      /* we have successfully created/altered the destination table, record this information */
      g_hash_table_insert(self->validated_tables, g_strdup(table), GUINT_TO_POINTER(TRUE));
    }
  g_string_free(query_string, TRUE);
  return success;
}

/**
 * afsql_dd_suspend:
 * timeout: in milliseconds
 *
 * This function is assumed to be called from the database thread
 * only!
 **/
static void
afsql_dd_suspend(AFSqlDestDriver *self)
{
  self->db_thread_suspended = TRUE;
  g_get_current_time(&self->db_thread_suspend_target);
  g_time_val_add(&self->db_thread_suspend_target, self->time_reopen * 1000 * 1000); /* the timeout expects microseconds */
}

static void
afsql_dd_disconnect(AFSqlDestDriver *self)
{
  dbi_conn_close(self->dbi_ctx);
  self->dbi_ctx = NULL;
  g_hash_table_remove_all(self->validated_tables);
}

static gboolean
afsql_dd_ensure_initialized_connection(AFSqlDestDriver *self)
{
  if (self->dbi_ctx)
    return TRUE;

  self->dbi_ctx = dbi_conn_new_r(self->type, dbi_instance);

  if (!self->dbi_ctx)
    {
      msg_error("No such DBI driver",
                evt_tag_str("type", self->type),
                NULL);
      return FALSE;
    }

  dbi_conn_set_option(self->dbi_ctx, "host", self->host);

  if (strcmp(self->type, "mysql"))
    dbi_conn_set_option(self->dbi_ctx, "port", self->port);
  else
    dbi_conn_set_option_numeric(self->dbi_ctx, "port", atoi(self->port));

  dbi_conn_set_option(self->dbi_ctx, "username", self->user);
  dbi_conn_set_option(self->dbi_ctx, "password", self->password);
  dbi_conn_set_option(self->dbi_ctx, "dbname", self->database);
  dbi_conn_set_option(self->dbi_ctx, "encoding", self->encoding);
  dbi_conn_set_option(self->dbi_ctx, "auto-commit", self->flags & AFSQL_DDF_EXPLICIT_COMMITS ? "false" : "true");

  /* database specific hacks */
  dbi_conn_set_option(self->dbi_ctx, "sqlite_dbdir", "");
  dbi_conn_set_option(self->dbi_ctx, "sqlite3_dbdir", "");

  if (dbi_conn_connect(self->dbi_ctx) < 0)
    {
      const gchar *dbi_error;

      dbi_conn_error(self->dbi_ctx, &dbi_error);

      msg_error("Error establishing SQL connection",
                evt_tag_str("type", self->type),
                evt_tag_str("host", self->host),
                evt_tag_str("port", self->port),
                evt_tag_str("username", self->user),
                evt_tag_str("database", self->database),
                evt_tag_str("error", dbi_error),
                NULL);

      return FALSE;
    }

  if (self->session_statements != NULL)
    {
      GList *l;

      for (l = self->session_statements; l; l = l->next)
        {
          if (!afsql_dd_run_query(self, (gchar *) l->data, FALSE, NULL))
            {
              msg_error("Error executing SQL connection statement",
                        evt_tag_str("statement", (gchar *) l->data),
                        NULL);

              return FALSE;
            }
        }
    }

  return TRUE;
}

static GString *
afsql_dd_ensure_accessible_database_table(AFSqlDestDriver *self, LogMessage *msg)
{
  GString *table = g_string_sized_new(32);

  log_template_format(self->table, msg, &self->template_options, LTZ_LOCAL, 0, NULL, table);

  if (!afsql_dd_validate_table(self, table->str))
    {
      /* If validate table is FALSE then close the connection and wait time_reopen time (next call) */
      msg_error("Error checking table, disconnecting from database, trying again shortly",
                evt_tag_int("time_reopen", self->time_reopen),
                NULL);
      g_string_free(table, TRUE);
      return NULL;
    }

  return table;
}

static GString *
afsql_dd_build_insert_command(AFSqlDestDriver *self, LogMessage *msg, GString *table)
{
  GString *insert_command = g_string_sized_new(256);
  GString *value = g_string_sized_new(512);
  gint i, j;

  g_string_printf(insert_command, "INSERT INTO %s (", table->str);

  for (i = 0; i < self->fields_len; i++)
    {
      if ((self->fields[i].flags & AFSQL_FF_DEFAULT) == 0 && self->fields[i].value != NULL)
        {
           g_string_append(insert_command, self->fields[i].name);

           j = i + 1;
           while (j < self->fields_len && (self->fields[j].flags & AFSQL_FF_DEFAULT) == AFSQL_FF_DEFAULT)
             j++;

           if (j < self->fields_len)
             g_string_append(insert_command, ", ");
        }
    }

  g_string_append(insert_command, ") VALUES (");

  for (i = 0; i < self->fields_len; i++)
    {
      gchar *quoted;

      if ((self->fields[i].flags & AFSQL_FF_DEFAULT) == 0 && self->fields[i].value != NULL)
        {
          log_template_format(self->fields[i].value, msg, &self->template_options, LTZ_SEND, self->seq_num, NULL, value);
          if (self->null_value && strcmp(self->null_value, value->str) == 0)
            {
              g_string_append(insert_command, "NULL");
            }
          else
            {
              dbi_conn_quote_string_copy(self->dbi_ctx, value->str, &quoted);
              if (quoted)
                {
                  g_string_append(insert_command, quoted);
                  free(quoted);
                }
              else
                {
                 g_string_append(insert_command, "''");
                }
            }

          j = i + 1;
          while (j < self->fields_len && (self->fields[j].flags & AFSQL_FF_DEFAULT) == AFSQL_FF_DEFAULT)
            j++;
          if (j < self->fields_len)
            g_string_append(insert_command, ", ");
        }
    }

  g_string_append(insert_command, ")");

  g_string_free(value, TRUE);

  return insert_command;
}

static inline gboolean
afsql_dd_is_transaction_handling_enabled(const AFSqlDestDriver *self)
{
  return self->flush_lines_queued != -1;
}

static inline gboolean
afsql_dd_should_begin_new_transaction(const AFSqlDestDriver *self)
{
  return self->flush_lines_queued == 0;
}

static inline gboolean
afsql_dd_should_commit_transaction(const AFSqlDestDriver *self)
{
  return afsql_dd_is_transaction_handling_enabled(self) && self->flush_lines_queued == self->flush_lines;
}

static inline void
afsql_dd_rollback_msg(AFSqlDestDriver *self, LogMessage *msg, LogPathOptions *path_options)
{
  if (self->flags & AFSQL_DDF_EXPLICIT_COMMITS)
    log_queue_rewind_backlog(self->queue, 1);
  else
    log_queue_push_head(self->queue, msg, path_options);
}

static inline gboolean
afsql_dd_handle_insert_row_error_depending_on_connection_availability(AFSqlDestDriver *self,
                                                                      LogMessage *msg,
                                                                      LogPathOptions *path_options)
{
  const gchar *dbi_error, *error_message;

  if (dbi_conn_ping(self->dbi_ctx) == 1)
    {
      afsql_dd_rollback_msg(self, msg, path_options);
      return TRUE;
    }

  if (afsql_dd_is_transaction_handling_enabled(self))
    {
      error_message = "SQL connection lost in the middle of a transaction,"
                      " rewinding backlog and starting again";
      afsql_dd_handle_transaction_error(self);
    }
  else
    {
      error_message = "Error, no SQL connection after failed query attempt";
      afsql_dd_rollback_msg(self, msg, path_options);
    }

  dbi_conn_error(self->dbi_ctx, &dbi_error);
  msg_error(error_message,
            evt_tag_str("type", self->type),
            evt_tag_str("host", self->host),
            evt_tag_str("port", self->port),
            evt_tag_str("username", self->user),
            evt_tag_str("database", self->database),
            evt_tag_str("error", dbi_error),
            NULL);

  return FALSE;
}

/**
 * afsql_dd_insert_db:
 *
 * This function is running in the database thread
 *
 * Returns: FALSE to indicate that the connection should be closed and
 * this destination suspended for time_reopen() time.
 **/
static gboolean
afsql_dd_insert_db(AFSqlDestDriver *self)
{
  GString *table = NULL;
  GString *insert_command = NULL;
  LogMessage *msg;
  gboolean success = TRUE;
  LogPathOptions path_options = LOG_PATH_OPTIONS_INIT;

  if (!afsql_dd_ensure_initialized_connection(self))
    return FALSE;

  /* connection established, try to insert a message */
  msg = log_queue_pop_head(self->queue, &path_options);
  if (!msg)
    return TRUE;

  msg_set_context(msg);

  table = afsql_dd_ensure_accessible_database_table(self, msg);

  if (!table)
    {
      success = FALSE;
      goto out;
    }

  if (afsql_dd_should_begin_new_transaction(self) && !afsql_dd_begin_transaction(self))
    {
      success = FALSE;
      goto out;
    }

  insert_command = afsql_dd_build_insert_command(self, msg, table);
  success = afsql_dd_run_query(self, insert_command->str, FALSE, NULL);

  if (success && self->flush_lines_queued != -1)
    {
      self->flush_lines_queued++;

      if (afsql_dd_should_commit_transaction(self) && !afsql_dd_commit_transaction(self))
        {
          /* Assuming that in case of error, the queue is rewound by afsql_dd_commit_transaction() */
          afsql_dd_rollback_transaction(self);

          g_string_free(insert_command, TRUE);
          msg_set_context(NULL);

          return FALSE;
        }
    }

 out:

  if (table != NULL)
    g_string_free(table, TRUE);

  if (insert_command != NULL)
    g_string_free(insert_command, TRUE);

  msg_set_context(NULL);

  if (success)
    {
      log_msg_ack(msg, &path_options, AT_PROCESSED);
      log_msg_unref(msg);
      step_sequence_number(&self->seq_num);
      self->failed_message_counter = 0;
    }
  else
    {
      if (self->failed_message_counter < self->num_retries - 1)
        {
          if (!afsql_dd_handle_insert_row_error_depending_on_connection_availability(self, msg, &path_options))
            return FALSE;

          self->failed_message_counter++;
        }
      else
        {
          msg_error("Multiple failures while inserting this record into the database, message dropped",
                    evt_tag_int("attempts", self->num_retries),
                    NULL);
          stats_counter_inc(self->dropped_messages);
          log_msg_drop(msg, &path_options);
          self->failed_message_counter = 0;
          success = TRUE;
        }
    }
  return success;
}

static void
afsql_dd_message_became_available_in_the_queue(gpointer user_data)
{
  AFSqlDestDriver *self = (AFSqlDestDriver *) user_data;

  g_mutex_lock(self->db_thread_mutex);
  g_cond_signal(self->db_thread_wakeup_cond);
  g_mutex_unlock(self->db_thread_mutex);
}

/* assumes that db_thread_mutex is held */
static void
afsql_dd_wait_for_suspension_wakeup(AFSqlDestDriver *self)
{
  /* we got suspended, probably because of a connection error,
   * during this time we only get wakeups if we need to be
   * terminated. */
  if (!self->db_thread_terminate)
    g_cond_timed_wait(self->db_thread_wakeup_cond, self->db_thread_mutex, &self->db_thread_suspend_target);
  self->db_thread_suspended = FALSE;
}

/**
 * afsql_dd_database_thread:
 *
 * This is the thread inserting records into the database.
 **/
static void
afsql_dd_database_thread(gpointer arg)
{
  AFSqlDestDriver *self = (AFSqlDestDriver *) arg;

  msg_verbose("Database thread started",
              evt_tag_str("driver", self->super.super.id),
              NULL);
  while (!self->db_thread_terminate)
    {
      g_mutex_lock(self->db_thread_mutex);
      if (self->db_thread_suspended)
        {
          afsql_dd_wait_for_suspension_wakeup(self);
          /* we loop back to check if the thread was requested to terminate */
        }
      else if (!log_queue_check_items(self->queue, NULL, afsql_dd_message_became_available_in_the_queue, self, NULL))
        {
          /* we have nothing to INSERT into the database, let's wait we get some new stuff */

          if (self->flush_lines_queued > 0)
            {
              if (!afsql_dd_commit_transaction(self))
                {
                  if (!afsql_dd_rollback_transaction(self))
                    {
                      afsql_dd_disconnect(self);
                      afsql_dd_suspend(self);
                    }
                  g_mutex_unlock(self->db_thread_mutex);
                  continue;
                }
            }
          else if (!self->db_thread_terminate)
            {
              g_cond_wait(self->db_thread_wakeup_cond, self->db_thread_mutex);
            }

          /* we loop back to check if the thread was requested to terminate */
        }
      g_mutex_unlock(self->db_thread_mutex);

      if (self->db_thread_terminate)
        break;

      if (!afsql_dd_insert_db(self))
        {
          afsql_dd_disconnect(self);
          afsql_dd_suspend(self);
        }
    }

  while (log_queue_get_length(self->queue) > 0)
    {
      if(!afsql_dd_insert_db(self))
        {
          goto exit;
        }
    }

  if (self->flush_lines_queued > 0)
    {
      /* we can't do anything with the return value here. if commit isn't
       * successful, we get our backlog back, but we have no chance
       * submitting that back to the SQL engine.
       */

      if (!afsql_dd_commit_transaction(self))
        afsql_dd_rollback_transaction(self);
    }
exit:
  afsql_dd_disconnect(self);

  msg_verbose("Database thread finished",
              evt_tag_str("driver", self->super.super.id),
              NULL);
}

static void
afsql_dd_stop_thread(gpointer user_data)
{
  AFSqlDestDriver *self = (AFSqlDestDriver *)user_data;
  g_mutex_lock(self->db_thread_mutex);
  self->db_thread_terminate = TRUE;
  g_cond_signal(self->db_thread_wakeup_cond);
  g_mutex_unlock(self->db_thread_mutex);
}

static void
afsql_dd_start_thread(AFSqlDestDriver *self)
{
  main_loop_create_worker_thread(afsql_dd_database_thread, afsql_dd_stop_thread, self, &self->worker_options);
}

static gchar *
afsql_dd_format_stats_instance(AFSqlDestDriver *self)
{
  static gchar persist_name[64];

  g_snprintf(persist_name, sizeof(persist_name),
             "%s,%s,%s,%s,%s",
             self->type, self->host, self->port, self->database, self->table->template);
  return persist_name;
}

static gchar *
afsql_dd_format_persist_sequence_number(AFSqlDestDriver *self)
{
  static gchar persist_name[256];
  g_snprintf(persist_name, sizeof(persist_name),
  "afsql_dd_sequence_number(%s,%s,%s,%s,%s)",
  self->type,self->host, self->port, self->database, self->table->template);
  return persist_name;
}

static gchar *
afsql_dd_format_persist_name(AFSqlDestDriver *self)
{
  static gchar persist_name_old[256];
  static gchar persist_name_new[256];

  g_snprintf(persist_name_old, sizeof(persist_name_old),
             "afsql_dd_qfile(%s,%s,%s,%s)",
             self->type, self->host, self->port, self->database);

  g_snprintf(persist_name_new, sizeof(persist_name_new),
             "afsql_dd_qfile(%s,%s,%s,%s,%s)",
             self->type, self->host, self->port, self->database, self->table->template);

  /*
    Lookup for old style persist name because of regression
  */
  if (persist_state_lookup_string (log_pipe_get_config((LogPipe *)self)->state,persist_name_old, NULL, NULL))
    {
      persist_state_rename_entry(log_pipe_get_config((LogPipe *)self)->state,persist_name_old,persist_name_new);
    }

  return persist_name_new;
}

static gboolean
afsql_dd_init(LogPipe *s)
{
  AFSqlDestDriver *self = (AFSqlDestDriver *) s;
  GlobalConfig *cfg = log_pipe_get_config(s);
  gint len_cols, len_values;

  if (!server_mode)
    {
      msg_error("syslog-ng running in client/relay mode, SQL destination is unavailable", NULL);
      return FALSE;
    }

  if (!log_dest_driver_init_method(s))
    return FALSE;

  if (!self->columns || !self->indexes || !self->values)
    {
      msg_error("Default columns, values and indexes must be specified for database destinations",
                evt_tag_str("database type", self->type),
                NULL);
      return FALSE;
    }
  stats_lock();
  stats_register_counter(0, SCS_SQL | SCS_DESTINATION, self->super.super.id, afsql_dd_format_stats_instance(self), SC_TYPE_STORED, &self->stored_messages);
  stats_register_counter(0, SCS_SQL | SCS_DESTINATION, self->super.super.id, afsql_dd_format_stats_instance(self), SC_TYPE_DROPPED, &self->dropped_messages);
  stats_unlock();

  self->seq_num = GPOINTER_TO_INT(cfg_persist_config_fetch(cfg, afsql_dd_format_persist_sequence_number(self)));
  if (!self->seq_num)
    init_sequence_number(&self->seq_num);

  self->queue = log_dest_driver_acquire_queue(&self->super, afsql_dd_format_persist_name(self));
  if (self->queue == NULL)
    {
      return FALSE;
    }
  else
    {
      if (self->flags & AFSQL_DDF_EXPLICIT_COMMITS)
        log_queue_set_use_backlog(self->queue, TRUE);
    }
  log_queue_set_counters(self->queue, self->stored_messages, self->dropped_messages);
  if (!self->fields)
    {
      GList *col, *value;
      gint i;

      len_cols = g_list_length(self->columns);
      len_values = g_list_length(self->values);
      if (len_cols != len_values)
        {
          msg_error("The number of columns and values do not match",
                    evt_tag_int("len_columns", len_cols),
                    evt_tag_int("len_values", len_values),
                    NULL);
          goto error;
        }
      self->fields_len = len_cols;
      self->fields = g_new0(AFSqlField, len_cols);

      for (i = 0, col = self->columns, value = self->values; col && value; i++, col = col->next, value = value->next)
        {
          gchar *space;

          space = strchr(col->data, ' ');
          if (space)
            {
              self->fields[i].name = g_strndup(col->data, space - (gchar *) col->data);
              while (*space == ' ')
                space++;
              if (*space != '\0')
                self->fields[i].type = g_strdup(space);
              else
                self->fields[i].type = g_strdup("text");
            }
          else
            {
              self->fields[i].name = g_strdup(col->data);
              self->fields[i].type = g_strdup("text");
            }
          if (!afsql_dd_check_sql_identifier(self->fields[i].name, FALSE))
            {
              msg_error("Column name is not a proper SQL name",
                        evt_tag_str("column", self->fields[i].name),
                        NULL);
              return FALSE;
            }

          if (GPOINTER_TO_UINT(value->data) > 4096)
            {
              self->fields[i].value = log_template_new(cfg, NULL);
              log_template_compile(self->fields[i].value, (gchar *) value->data, NULL);
            }
          else
            {
              switch (GPOINTER_TO_UINT(value->data))
                {
                case AFSQL_COLUMN_DEFAULT:
                  self->fields[i].flags |= AFSQL_FF_DEFAULT;
                  break;
                default:
                  g_assert_not_reached();
                  break;
                }
            }
        }
    }

  self->time_reopen = cfg->time_reopen;

  log_template_options_init(&self->template_options, cfg);

  if (self->flush_lines == -1)
    self->flush_lines = cfg->flush_lines;

  if ((self->flags & AFSQL_DDF_EXPLICIT_COMMITS) && (self->flush_lines > 0))
    self->flush_lines_queued = 0;

  if (!dbi_initialized)
    {
      gint rc = dbi_initialize_r(NULL, &dbi_instance);

      if (rc < 0)
        {
          /* NOTE: errno might be unreliable, but that's all we have */
          msg_error("Unable to initialize database access (DBI)",
                    evt_tag_int("rc", rc),
                    evt_tag_errno("error", errno),
                    NULL);
          goto error;
        }
      else if (rc == 0)
        {
          msg_error("The database access library (DBI) reports no usable SQL drivers, perhaps DBI drivers are not installed properly",
                    NULL);
          goto error;
        }
      else
        {
          dbi_initialized = TRUE;
        }
    }

  afsql_dd_start_thread(self);
  return TRUE;

 error:

  stats_lock();
  stats_unregister_counter(SCS_SQL | SCS_DESTINATION, self->super.super.id, afsql_dd_format_stats_instance(self), SC_TYPE_STORED, &self->stored_messages);
  stats_unregister_counter(SCS_SQL | SCS_DESTINATION, self->super.super.id, afsql_dd_format_stats_instance(self), SC_TYPE_DROPPED, &self->dropped_messages);
  stats_unlock();

  return FALSE;
}

static gboolean
afsql_dd_deinit(LogPipe *s)
{
  AFSqlDestDriver *self = (AFSqlDestDriver *) s;

  afsql_dd_stop_thread(self);
  log_queue_reset_parallel_push(self->queue);

  log_queue_set_counters(self->queue, NULL, NULL);
  cfg_persist_config_add(log_pipe_get_config(s), afsql_dd_format_persist_sequence_number(self), GINT_TO_POINTER(self->seq_num), NULL, FALSE);

  stats_lock();
  stats_unregister_counter(SCS_SQL | SCS_DESTINATION, self->super.super.id, afsql_dd_format_stats_instance(self), SC_TYPE_STORED, &self->stored_messages);
  stats_unregister_counter(SCS_SQL | SCS_DESTINATION, self->super.super.id, afsql_dd_format_stats_instance(self), SC_TYPE_DROPPED, &self->dropped_messages);
  stats_unlock();
  if (!log_dest_driver_deinit_method(s))
    return FALSE;

  return TRUE;
}


static void
afsql_dd_queue(LogPipe *s, LogMessage *msg, const LogPathOptions *path_options, gpointer user_data)
{
  AFSqlDestDriver *self = (AFSqlDestDriver *) s;
  LogPathOptions local_options;

  log_dest_driver_counter_inc(s);

  if (!path_options->flow_control_requested)
    path_options = log_msg_break_ack(msg, path_options, &local_options);

  log_queue_push_tail(self->queue, msg, path_options);
}

static void
afsql_dd_free(LogPipe *s)
{
  AFSqlDestDriver *self = (AFSqlDestDriver *) s;
  gint i;

  log_template_options_destroy(&self->template_options);
  if (self->queue)
    log_queue_unref(self->queue);
  for (i = 0; i < self->fields_len; i++)
    {
      g_free(self->fields[i].name);
      g_free(self->fields[i].type);
      log_template_unref(self->fields[i].value);
    }

  g_free(self->fields);
  g_free(self->type);
  g_free(self->host);
  g_free(self->port);
  g_free(self->user);
  g_free(self->password);
  g_free(self->database);
  g_free(self->encoding);
  if (self->null_value)
    g_free(self->null_value);
  string_list_free(self->columns);
  if (self->enable_indexes)
    string_list_free(self->indexes);
  string_list_free(self->values);
  log_template_unref(self->table);
  g_hash_table_destroy(self->validated_tables);
  if (self->session_statements)
    string_list_free(self->session_statements);
  g_mutex_free(self->db_thread_mutex);
  g_cond_free(self->db_thread_wakeup_cond);
  log_dest_driver_free(s);
}

LogDriver *
afsql_dd_new(void)
{
  AFSqlDestDriver *self = g_new0(AFSqlDestDriver, 1);

  log_dest_driver_init_instance(&self->super);
  self->super.super.super.init = afsql_dd_init;
  self->super.super.super.deinit = afsql_dd_deinit;
  self->super.super.super.queue = afsql_dd_queue;
  self->super.super.super.free_fn = afsql_dd_free;

  self->type = g_strdup("mysql");
  self->host = g_strdup("");
  self->port = g_strdup("");
  self->user = g_strdup("syslog-ng");
  self->password = g_strdup("");
  self->database = g_strdup("logs");
  self->encoding = g_strdup("UTF-8");
  self->ignore_tns_config = FALSE;
  self->transaction_active = FALSE;

  self->table = log_template_new(configuration, NULL);
  log_template_compile(self->table, "messages", NULL);
  self->failed_message_counter = 0;

  self->flush_lines = -1;
  self->flush_lines_queued = -1;
  self->session_statements = NULL;
  self->num_retries = MAX_FAILED_ATTEMPTS;

  self->validated_tables = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  log_template_options_defaults(&self->template_options);

  self->db_thread_wakeup_cond = g_cond_new();
  self->db_thread_mutex = g_mutex_new();

  self->worker_options.is_output_thread = TRUE;
  return &self->super.super;
}

gint
afsql_dd_lookup_flag(const gchar *flag)
{
  if (strcmp(flag, "explicit-commits") == 0 || strcmp(flag, "explicit_commits") == 0)
    return AFSQL_DDF_EXPLICIT_COMMITS;
  else if (strcmp(flag, "dont-create-tables") == 0 || strcmp(flag, "dont_create_tables") == 0)
    return AFSQL_DDF_DONT_CREATE_TABLES;
  else
    msg_warning("Unknown SQL flag",
                evt_tag_str("flag", flag),
                NULL);
  return 0;
}

#endif
