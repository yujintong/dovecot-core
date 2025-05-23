/* Copyright (c) 2005-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "ostream.h"
#include "str.h"
#include "strescape.h"
#include "str-parse.h"
#include "stats-dist.h"
#include "time-util.h"
#include "dict-private.h"
#include "dict-client.h"
#include "dict-settings.h"
#include "dict-connection.h"
#include "dict-commands.h"
#include "main.h"

#define DICT_OUTPUT_OPTIMAL_SIZE 1024

struct dict_cmd_func {
	enum dict_protocol_cmd cmd;
	int (*func)(struct dict_connection_cmd *cmd, const char *const *args);
};

struct dict_connection_cmd {
	const struct dict_cmd_func *cmd;
	struct dict_connection *conn;
	struct timeval start_timeval;
	struct event *event;
	char *reply;

	struct dict_iterate_context *iter;
	enum dict_iterate_flags iter_flags;

	unsigned int async_reply_id;
	unsigned int trans_id; /* obsolete */
	unsigned int rows;

	bool uncork_pending;
};

struct dict_command_stats cmd_stats;

static int cmd_iterate_flush(struct dict_connection_cmd *cmd);

static bool dict_connection_cmds_try_output_more(struct dict_connection *conn);

static void dict_connection_cmd_free(struct dict_connection_cmd *cmd)
{
	const char *error;

	if (dict_iterate_deinit(&cmd->iter, &error) < 0)
		e_error(cmd->event, "dict_iterate() failed: %s", error);
	i_free(cmd->reply);
	if (cmd->uncork_pending)
		o_stream_uncork(cmd->conn->conn.output);

	if (dict_connection_unref(cmd->conn) && !cmd->conn->destroyed)
		connection_input_resume(&cmd->conn->conn);
	event_unref(&cmd->event);
	i_free(cmd);
}

static void dict_connection_cmd_remove(struct dict_connection_cmd *cmd)
{
	struct dict_connection_cmd *const *cmds;
	unsigned int i, count;

	cmds = array_get(&cmd->conn->cmds, &count);
	for (i = 0; i < count; i++) {
		if (cmds[i] == cmd) {
			array_delete(&cmd->conn->cmds, i, 1);
			dict_connection_cmd_free(cmd);
			return;
		}
	}
	i_unreached();
}

static void dict_connection_cmd_try_flush(struct dict_connection_cmd **_cmd)
{
	struct dict_connection_cmd *cmd = *_cmd;

	*_cmd = NULL;

	i_assert(cmd->async_reply_id != 0);
	i_assert(cmd->reply != NULL);

	o_stream_nsend_str(cmd->conn->conn.output, t_strdup_printf("%c%u\t%s",
		DICT_PROTOCOL_REPLY_ASYNC_REPLY,
		cmd->async_reply_id, cmd->reply));
	dict_connection_cmd_remove(cmd);
}

static void dict_connection_cmd_async(struct dict_connection_cmd *cmd)
{
	i_assert(cmd->async_reply_id == 0);
	cmd->async_reply_id = ++cmd->conn->async_id_counter;
	if (cmd->async_reply_id == 0)
		cmd->async_reply_id = ++cmd->conn->async_id_counter;
	o_stream_nsend_str(cmd->conn->conn.output, t_strdup_printf("%c%u\n",
		DICT_PROTOCOL_REPLY_ASYNC_ID, cmd->async_reply_id));
}

static void
cmd_stats_update(struct dict_connection_cmd *cmd, struct stats_dist *stats)
{
	long long diff;

	if (!server_settings->verbose_proctitle)
		return;

	diff = timeval_diff_usecs(&ioloop_timeval, &cmd->start_timeval);
	if (diff < 0)
		diff = 0;
	stats_dist_add(stats, diff);
	dict_proctitle_update_later();
}

static void
dict_cmd_reply_handle_stats(struct dict_connection_cmd *cmd,
			    string_t *str, struct stats_dist *stats)
{
	io_loop_time_refresh();
	cmd_stats_update(cmd, stats);

	str_printfa(str, "\t%ld\t%u\t%ld\t%u",
		    (long)cmd->start_timeval.tv_sec,
		    (unsigned int)cmd->start_timeval.tv_usec,
		    (long)ioloop_timeval.tv_sec,
		    (unsigned int)ioloop_timeval.tv_usec);
}

static void
cmd_lookup_write_reply(const char *const *values, string_t *str)
{
	string_t *tmp;

	i_assert(values[0] != NULL);

	if (values[1] == NULL) {
		str_append_c(str, DICT_PROTOCOL_REPLY_OK);
		str_append_tabescaped(str, values[0]);
		return;
	}
	/* the results get double-tabescaped so they end up becoming a single
	   parameter */
	tmp = t_str_new(128);
	for (unsigned int i = 0; values[i] != NULL; i++) {
		str_append_c(tmp, '\t');
		str_append_tabescaped(tmp, values[i]);
	}
	str_append_c(str, DICT_PROTOCOL_REPLY_MULTI_OK);
	str_append_tabescaped(str, str_c(tmp) + 1);
}

static void
cmd_lookup_callback(const struct dict_lookup_result *result,
		    struct dict_connection_cmd *cmd)
{
	string_t *str = t_str_new(128);

	event_set_name(cmd->event, "dict_server_lookup_finished");
	if (result->ret > 0) {
		cmd_lookup_write_reply(result->values, str);
		e_debug(cmd->event, "Lookup finished");
	} else if (result->ret == 0) {
		event_add_str(cmd->event, "key_not_found", "yes");
		str_append_c(str, DICT_PROTOCOL_REPLY_NOTFOUND);
		e_debug(cmd->event, "Lookup finished without results");
	} else {
		event_add_str(cmd->event, "error", result->error);
		e_error(cmd->event, "Lookup failed: %s", result->error);
		str_append_c(str, DICT_PROTOCOL_REPLY_FAIL);
		str_append_tabescaped(str, result->error);
	}
	dict_cmd_reply_handle_stats(cmd, str, cmd_stats.lookups);
	str_append_c(str, '\n');

	cmd->reply = i_strdup(str_c(str));
	dict_connection_cmd_try_flush(&cmd);
}

static int cmd_lookup(struct dict_connection_cmd *cmd, const char *const *args)
{
	const char *username;

	if (str_array_length(args) < 1) {
		e_error(cmd->event, "LOOKUP: broken input");
		return -1;
	}
	username = args[1];

	if (str_begins_with(args[0], DICT_PATH_PRIVATE) &&
	    (username == NULL || username[0] == '\0')) {
		e_error(cmd->event, "LOOKUP: private dict key requires username");
		return -1;
	}

	/* <key> [<username>] */
	dict_connection_cmd_async(cmd);
	event_add_str(cmd->event, "key", args[0]);
	event_add_str(cmd->event, "user", username);
	if (username == NULL || username[0] == '\0') {
		event_set_append_log_prefix(cmd->event, t_strdup_printf(
			"LOOKUP %s: ", args[0]));
	} else {
		event_set_append_log_prefix(cmd->event, t_strdup_printf(
			"LOOKUP %s (user %s): ", args[0], username));
	}
	const struct dict_op_settings set = {
		.username = username,
	};
	dict_lookup_async(cmd->conn->dict, &set, args[0], cmd_lookup_callback, cmd);
	return 1;
}

static bool dict_connection_flush_if_full(struct dict_connection *conn)
{
	if (o_stream_get_buffer_used_size(conn->conn.output) >
	    DICT_OUTPUT_OPTIMAL_SIZE) {
		if (o_stream_flush(conn->conn.output) <= 0) {
			/* continue later when there's more space
			   in output buffer */
			o_stream_set_flush_pending(conn->conn.output, TRUE);
			conn->iter_flush_pending = TRUE;
			return FALSE;
		}
		/* flushed everything, continue */
	}
	return TRUE;
}

static void
cmd_iterate_flush_finish(struct dict_connection_cmd *cmd, string_t *str)
{
	const char *error;

	event_set_name(cmd->event, "dict_server_iteration_finished");
	str_truncate(str, 0);
	if (dict_iterate_deinit(&cmd->iter, &error) < 0) {
		event_add_str(cmd->event, "error", error);
		e_error(cmd->event, "dict_iterate() failed: %s", error);
		str_printfa(str, "%c%s", DICT_PROTOCOL_REPLY_FAIL, error);
	} else {
		event_add_int(cmd->event, "rows", cmd->rows);
		e_debug(cmd->event, "Iteration finished");
	}
	dict_cmd_reply_handle_stats(cmd, str, cmd_stats.iterations);
	str_append_c(str, '\n');

	cmd->reply = i_strdup(str_c(str));
}

static int cmd_iterate_flush(struct dict_connection_cmd *cmd)
{
	string_t *str = t_str_new(256);
	const char *key, *const *values;

	if (cmd->conn->destroyed) {
		cmd_iterate_flush_finish(cmd, str);
		return 1;
	}

	if (!dict_connection_flush_if_full(cmd->conn))
		return 0;

	while (dict_iterate_values(cmd->iter, &key, &values)) {
		cmd->rows++;
		str_truncate(str, 0);
		if (cmd->async_reply_id != 0) {
			str_append_c(str, DICT_PROTOCOL_REPLY_ASYNC_REPLY);
			str_printfa(str, "%u\t", cmd->async_reply_id);
		}
		str_append_c(str, DICT_PROTOCOL_REPLY_OK);
		str_append_tabescaped(str, key);
		str_append_c(str, '\t');
		if ((cmd->iter_flags & DICT_ITERATE_FLAG_NO_VALUE) == 0) {
			str_append_tabescaped(str, values[0]);
			for (unsigned int i = 1; values[i] != NULL; i++) {
				str_append_c(str, '\t');
				str_append_tabescaped(str, values[i]);
			}
		}
		str_append_c(str, '\n');
		o_stream_nsend(cmd->conn->conn.output, str_data(str), str_len(str));

		if (!dict_connection_flush_if_full(cmd->conn))
			return 0;
	}
	if (dict_iterate_has_more(cmd->iter)) {
		/* wait for the next iteration callback */
		return 0;
	}

	cmd_iterate_flush_finish(cmd, str);
	return 1;
}

static void cmd_iterate_callback(struct dict_connection_cmd *cmd)
{
	struct dict_connection *conn = cmd->conn;

	dict_connection_ref(conn);
	o_stream_cork(conn->conn.output);
	/* Don't uncork if we're just waiting for more input from the dict
	   driver. Some dict drivers (e.g. dict-client) don't do any kind of
	   buffering internally, so this callback can write out only a single
	   iteration. By leaving the ostream corked it doesn't result in many
	   tiny writes. However, we could be here also because the connection
	   output buffer is full already, in which case don't want to leave a
	   cork. */
	conn->iter_flush_pending = FALSE;
	cmd->uncork_pending = FALSE;
	if (dict_connection_cmds_try_output_more(conn)) {
		/* NOTE: cmd may be freed now */
		o_stream_uncork(conn->conn.output);
	} else if (conn->iter_flush_pending) {
		/* Don't leave the stream uncorked or we might get stuck. */
		o_stream_uncork(conn->conn.output);
	} else {
		/* It's possible that the command gets finished via some other
		   code path. To make sure this doesn't cause hangs, uncork the
		   output when command gets freed. */
		cmd->uncork_pending = TRUE;
	}
	dict_connection_unref_safe(conn);
}

static int cmd_iterate(struct dict_connection_cmd *cmd, const char *const *args)
{
	const char *username;
	unsigned int flags;
	uint64_t max_rows;

	if (str_array_length(args) < 3 ||
	    str_to_uint(args[0], &flags) < 0 ||
	    str_to_uint64(args[1], &max_rows) < 0) {
		e_error(cmd->event, "ITERATE: broken input");
		return -1;
	}
	dict_connection_cmd_async(cmd);
	username = args[3];

	if (str_begins_with(args[2], DICT_PATH_PRIVATE) &&
	    (username == NULL || username[0] == '\0')) {
		e_error(cmd->event, "ITERATE: private dict key requires username");
		return -1;
	}

	const struct dict_op_settings set = {
		.username = username,
	};

	/* <flags> <max_rows> <path> [<username>] */
	flags |= DICT_ITERATE_FLAG_ASYNC;
	event_add_str(cmd->event, "key", args[2]);
	event_add_str(cmd->event, "user", username);
	if (username == NULL || username[0] == '\0') {
		event_set_append_log_prefix(cmd->event, t_strdup_printf(
			"ITERATE %s: ", args[2]));
	} else {
		event_set_append_log_prefix(cmd->event, t_strdup_printf(
			"ITERATE %s (user %s): ", args[2], username));
	}
	cmd->iter = dict_iterate_init(cmd->conn->dict, &set, args[2], flags);
	cmd->iter_flags = flags;
	if (max_rows > 0)
		dict_iterate_set_limit(cmd->iter, max_rows);
	dict_iterate_set_async_callback(cmd->iter, cmd_iterate_callback, cmd);
	(void)dict_connection_cmds_try_output_more(cmd->conn);
	return 1;
}

static struct dict_connection_transaction *
dict_connection_transaction_lookup(struct dict_connection *conn,
				   unsigned int id)
{
	struct dict_connection_transaction *transaction;

	if (!array_is_created(&conn->transactions))
		return NULL;

	array_foreach_modifiable(&conn->transactions, transaction) {
		if (transaction->id == id)
			return transaction;
	}
	return NULL;
}

static void
dict_connection_transaction_array_remove(struct dict_connection *conn,
					 unsigned int id)
{
	const struct dict_connection_transaction *transactions;
	unsigned int i, count;

	transactions = array_get(&conn->transactions, &count);
	for (i = 0; i < count; i++) {
		if (transactions[i].id == id) {
			i_assert(transactions[i].ctx == NULL);
			array_delete(&conn->transactions, i, 1);
			return;
		}
	}
	i_unreached();
}

static int cmd_begin(struct dict_connection_cmd *cmd, const char *const *args)
{
	struct dict_connection_transaction *trans;
	unsigned int id, args_count = str_array_length(args);

	if (args_count < 1) {
		e_error(cmd->event, "BEGIN: broken input");
		return -1;
	}
	event_set_append_log_prefix(cmd->event, "BEGIN: ");

	struct dict_op_settings set = {
		.username = args[1],
	};

	/* <id> [<username> [<expire secs>]] */
	if (str_to_uint(args[0], &id) < 0) {
		e_error(cmd->event, "Invalid transaction ID %s", args[0]);
		return -1;
	}
	if (args_count >= 3 && str_to_uint(args[2], &set.expire_secs) < 0) {
		e_error(cmd->event, "Invalid expire_secs %s", args[2]);
		return -1;
	}
	if (dict_connection_transaction_lookup(cmd->conn, id) != NULL) {
		e_error(cmd->event, "Transaction ID %u already exists", id);
		return -1;
	}

	if (!array_is_created(&cmd->conn->transactions))
		i_array_init(&cmd->conn->transactions, 4);

	trans = array_append_space(&cmd->conn->transactions);
	trans->id = id;
	trans->conn = cmd->conn;
	trans->ctx = dict_transaction_begin(cmd->conn->dict, &set);
	return 0;
}

static int
dict_connection_transaction_lookup_parse(struct dict_connection *conn,
					 const char *id_str,
					 struct dict_connection_transaction **trans_r)
{
	unsigned int id;

	if (str_to_uint(id_str, &id) < 0) {
		e_error(conn->conn.event, "Invalid transaction ID %s", id_str);
		return -1;
	}
	*trans_r = dict_connection_transaction_lookup(conn, id);
	if (*trans_r == NULL) {
		e_error(conn->conn.event, "Transaction ID %u doesn't exist", id);
		return -1;
	}
	return 0;
}

static void
cmd_commit_finish(struct dict_connection_cmd *cmd,
		  const struct dict_commit_result *result, bool async)
{
	string_t *str = t_str_new(64);
	char chr;

	event_set_name(cmd->event, "dict_server_transaction_finished");
	switch (result->ret) {
	case DICT_COMMIT_RET_OK:
		chr = DICT_PROTOCOL_REPLY_OK;
		break;
	case DICT_COMMIT_RET_NOTFOUND:
		event_add_str(cmd->event, "key_not_found", "yes");
		chr = DICT_PROTOCOL_REPLY_NOTFOUND;
		break;
	case DICT_COMMIT_RET_WRITE_UNCERTAIN:
		i_assert(result->error != NULL);
		event_add_str(cmd->event, "write_uncertain", "yes");
		event_add_str(cmd->event, "error", result->error);
		chr = DICT_PROTOCOL_REPLY_WRITE_UNCERTAIN;
		break;
	case DICT_COMMIT_RET_FAILED:
	default:
		i_assert(result->error != NULL);
		event_add_str(cmd->event, "error", result->error);
		chr = DICT_PROTOCOL_REPLY_FAIL;
		break;
	}
	if (async)
		str_append_c(str, DICT_PROTOCOL_REPLY_ASYNC_COMMIT);
	str_printfa(str, "%c%u", chr, cmd->trans_id);
	if (chr != DICT_PROTOCOL_REPLY_OK &&
	    chr != DICT_PROTOCOL_REPLY_NOTFOUND) {
		str_append_c(str, '\t');
		str_append_tabescaped(str, result->error);
	}
	dict_cmd_reply_handle_stats(cmd, str, cmd_stats.commits);
	str_append_c(str, '\n');
	cmd->reply = i_strdup(str_c(str));

	if (result->ret < 0)
		e_debug(cmd->event, "Transaction finished: %s", result->error);
	else
		e_debug(cmd->event, "Transaction finished");
	dict_connection_transaction_array_remove(cmd->conn, cmd->trans_id);
	dict_connection_cmd_try_flush(&cmd);
}

static void cmd_commit_callback(const struct dict_commit_result *result,
				struct dict_connection_cmd *cmd)
{
	cmd_commit_finish(cmd, result, FALSE);
}

static int
cmd_commit(struct dict_connection_cmd *cmd, const char *const *args)
{
	struct dict_connection_transaction *trans;

	if (dict_connection_transaction_lookup_parse(cmd->conn, args[0], &trans) < 0)
		return -1;
	cmd->trans_id = trans->id;
	event_add_str(cmd->event, "user", trans->ctx->set.username);

	if (trans->ctx->set.username == NULL ||
	    trans->ctx->set.username[0] == '\0')
		event_set_append_log_prefix(cmd->event, "COMMIT: ");
	else {
		event_set_append_log_prefix(cmd->event, t_strdup_printf(
			"COMMIT (user %s): ", trans->ctx->set.username));
	}

	dict_connection_cmd_async(cmd);
	dict_transaction_commit_async(&trans->ctx, cmd_commit_callback, cmd);
	return 1;
}

static int
cmd_rollback(struct dict_connection_cmd *cmd, const char *const *args)
{
	struct dict_connection_transaction *trans;

	if (dict_connection_transaction_lookup_parse(cmd->conn, args[0], &trans) < 0)
		return -1;

	event_add_str(cmd->event, "user", trans->ctx->set.username);
	dict_transaction_rollback(&trans->ctx);
	dict_connection_transaction_array_remove(cmd->conn, trans->id);
	return 0;
}

static int cmd_set(struct dict_connection_cmd *cmd, const char *const *args)
{
	struct dict_connection_transaction *trans;

	/* <id> <key> <value> */
	if (str_array_length(args) != 3) {
		e_error(cmd->event, "SET: broken input");
		return -1;
	}

	if (dict_connection_transaction_lookup_parse(cmd->conn, args[0], &trans) < 0)
		return -1;
	if (str_begins_with(args[1], DICT_PATH_PRIVATE) &&
	    (trans->ctx->set.username == NULL ||
	     trans->ctx->set.username[0] == '\0')) {
		e_error(cmd->event, "SET: private dict key requires username");
		return -1;
	}

	event_add_str(cmd->event, "user", trans->ctx->set.username);
        dict_set(trans->ctx, args[1], args[2]);
	return 0;
}

static int cmd_unset(struct dict_connection_cmd *cmd, const char *const *args)
{
	struct dict_connection_transaction *trans;

	/* <id> <key> */
	if (str_array_length(args) != 2) {
		e_error(cmd->event, "UNSET: broken input");
		return -1;
	}

	if (dict_connection_transaction_lookup_parse(cmd->conn, args[0], &trans) < 0)
		return -1;
	if (str_begins_with(args[1], DICT_PATH_PRIVATE) &&
	    (trans->ctx->set.username == NULL ||
	     trans->ctx->set.username[0] == '\0')) {
		e_error(cmd->event, "UNSET: private dict key requires username");
		return -1;
	}

        dict_unset(trans->ctx, args[1]);
	return 0;
}

static int
cmd_atomic_inc(struct dict_connection_cmd *cmd, const char *const *args)
{
	struct dict_connection_transaction *trans;
	long long diff;

	/* <id> <key> <diff> */
	if (str_array_length(args) != 3 ||
	    str_to_llong(args[2], &diff) < 0) {
		e_error(cmd->event, "ATOMIC_INC: broken input");
		return -1;
	}

	if (dict_connection_transaction_lookup_parse(cmd->conn, args[0], &trans) < 0)
		return -1;
	if (str_begins_with(args[1], DICT_PATH_PRIVATE) &&
	    (trans->ctx->set.username == NULL ||
	     trans->ctx->set.username[0] == '\0')) {
		e_error(cmd->event, "ATOMIC_INC: private dict key requires username");
		return -1;
	}

        dict_atomic_inc(trans->ctx, args[1], diff);
	return 0;
}

static int cmd_timestamp(struct dict_connection_cmd *cmd, const char *const *args)
{
	struct dict_connection_transaction *trans;
	long long tv_sec;
	unsigned int tv_nsec;

	/* <id> <secs> <nsecs> */
	if (str_array_length(args) != 3 ||
	    str_to_llong(args[1], &tv_sec) < 0 ||
	    str_to_uint(args[2], &tv_nsec) < 0) {
		e_error(cmd->event, "TIMESTAMP: broken input");
		return -1;
	}

	if (dict_connection_transaction_lookup_parse(cmd->conn, args[0], &trans) < 0)
		return -1;

	struct timespec ts = {
		.tv_sec = tv_sec,
		.tv_nsec = tv_nsec
	};
        dict_transaction_set_timestamp(trans->ctx, &ts);
	return 0;
}

static int
cmd_hide_log_values(struct dict_connection_cmd *cmd, const char *const *args)
{
	struct dict_connection_transaction *trans;
	const char *error;
	bool value;

	/* <id> <hide_log_values> */
	if (str_array_length(args) != 2 ||
	    str_parse_get_bool(args[1], &value, &error) < 0) {
		e_error(cmd->event, "HIDE_LOG_VALUES: broken input");
		return -1;
	}

	if (dict_connection_transaction_lookup_parse(cmd->conn, args[0], &trans) < 0)
		return -1;

	dict_transaction_set_hide_log_values(trans->ctx, value);
	return 0;
}

static const struct dict_cmd_func cmds[] = {
	{ DICT_PROTOCOL_CMD_LOOKUP, cmd_lookup },
	{ DICT_PROTOCOL_CMD_ITERATE, cmd_iterate },
	{ DICT_PROTOCOL_CMD_BEGIN, cmd_begin },
	{ DICT_PROTOCOL_CMD_COMMIT, cmd_commit },
	{ DICT_PROTOCOL_CMD_ROLLBACK, cmd_rollback },
	{ DICT_PROTOCOL_CMD_SET, cmd_set },
	{ DICT_PROTOCOL_CMD_UNSET, cmd_unset },
	{ DICT_PROTOCOL_CMD_ATOMIC_INC, cmd_atomic_inc },
	{ DICT_PROTOCOL_CMD_TIMESTAMP, cmd_timestamp },
	{ DICT_PROTOCOL_CMD_HIDE_LOG_VALUES, cmd_hide_log_values },

	{ 0, NULL }
};

static const struct dict_cmd_func *dict_command_find(enum dict_protocol_cmd cmd)
{
	unsigned int i;

	for (i = 0; cmds[i].cmd != '\0'; i++) {
		if (cmds[i].cmd == cmd)
			return &cmds[i];
	}
	return NULL;
}

int dict_command_input(struct dict_connection *conn, const char *line)
{
	const struct dict_cmd_func *cmd_func;
	struct dict_connection_cmd *cmd;
	int ret;
	const char *const *args;

	cmd_func = dict_command_find((enum dict_protocol_cmd)*line);
	if (cmd_func == NULL) {
		e_error(conn->conn.event, "Unknown command %c", *line);
		return -1;
	}

	cmd = i_new(struct dict_connection_cmd, 1);
	cmd->conn = conn;
	cmd->event = event_create(cmd->conn->conn.event);
	event_add_str(cmd->event, "dict_name", conn->name);
	cmd->cmd = cmd_func;
	cmd->start_timeval = ioloop_timeval;
	array_push_back(&conn->cmds, &cmd);
	dict_connection_ref(conn);

	args = t_strsplit_tabescaped(line + 1);
	if ((ret = cmd_func->func(cmd, args)) <= 0) {
		dict_connection_cmd_remove(cmd);
		return ret;
	}
	return 0;
}

static bool dict_connection_cmds_try_output_more(struct dict_connection *conn)
{
	struct dict_connection_cmd *cmd;

	/* only iterators may be returning a lot of data */
	array_foreach_elem(&conn->cmds, cmd) {
		if (cmd->iter == NULL) {
			/* not an iterator */
			continue;
		}

		int ret;
		T_BEGIN {
			ret = cmd_iterate_flush(cmd);
		} T_END;
		if (ret == 0) {
			/* unfinished */
		} else {
			dict_connection_cmd_try_flush(&cmd);
			/* cmd should be freed now, restart output */
			return TRUE;
		}
		/* try to flush the rest */
	}
	return FALSE;
}

void dict_connection_cmds_output_more(struct dict_connection *conn)
{
	while (array_count(&conn->cmds) > 0) {
		if (!dict_connection_cmds_try_output_more(conn))
			break;
	}
}

void dict_commands_init(void)
{
	cmd_stats.lookups = stats_dist_init();
	cmd_stats.iterations = stats_dist_init();
	cmd_stats.commits = stats_dist_init();
}

void dict_commands_deinit(void)
{
	stats_dist_deinit(&cmd_stats.lookups);
	stats_dist_deinit(&cmd_stats.iterations);
	stats_dist_deinit(&cmd_stats.commits);
}
