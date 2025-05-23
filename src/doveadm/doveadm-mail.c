/* Copyright (c) 2009-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "lib-signals.h"
#include "path-util.h"
#include "ioloop.h"
#include "istream.h"
#include "istream-dot.h"
#include "istream-seekable.h"
#include "str.h"
#include "strescape.h"
#include "unichar.h"
#include "module-dir.h"
#include "wildcard-match.h"
#include "settings.h"
#include "master-service.h"
#include "mail-user.h"
#include "mail-namespace.h"
#include "mail-storage.h"
#include "mail-storage-settings.h"
#include "mail-storage-service.h"
#include "mail-storage-hooks.h"
#include "mail-search-build.h"
#include "mail-search-parser.h"
#include "mailbox-list-iter.h"
#include "doveadm.h"
#include "doveadm-client.h"
#include "client-connection.h"
#include "doveadm-settings.h"
#include "doveadm-print.h"
#include "doveadm-dsync.h"
#include "doveadm-mail.h"

#include <stdio.h>

#define DOVEADM_MAIL_CMD_INPUT_TIMEOUT_MSECS (5*60*1000)

struct force_resync_cmd_context {
	struct doveadm_mail_cmd_context ctx;
	const char *mailbox;
	bool fsck;
};

void (*hook_doveadm_mail_init)(struct doveadm_mail_cmd_context *ctx);
struct doveadm_mail_cmd_module_register
	doveadm_mail_cmd_module_register = { 0 };
char doveadm_mail_cmd_hide = '\0';

bool doveadm_is_killed(void)
{
	return master_service_is_killed(master_service);
}

void doveadm_mail_failed_error(struct doveadm_mail_cmd_context *ctx,
			       enum mail_error error)
{
	int exit_code = EX_TEMPFAIL;

	switch (error) {
	case MAIL_ERROR_NONE:
		i_unreached();
	case MAIL_ERROR_TEMP:
	case MAIL_ERROR_UNAVAILABLE:
		break;
	case MAIL_ERROR_NOTPOSSIBLE:
	case MAIL_ERROR_EXISTS:
	case MAIL_ERROR_CONVERSION:
	case MAIL_ERROR_INVALIDDATA:
		exit_code = DOVEADM_EX_NOTPOSSIBLE;
		break;
	case MAIL_ERROR_PARAMS:
		exit_code = EX_USAGE;
		break;
	case MAIL_ERROR_PERM:
		exit_code = EX_NOPERM;
		break;
	case MAIL_ERROR_NOQUOTA:
		exit_code = EX_CANTCREAT;
		break;
	case MAIL_ERROR_NOTFOUND:
		exit_code = DOVEADM_EX_NOTFOUND;
		break;
	case MAIL_ERROR_EXPUNGED:
		break;
	case MAIL_ERROR_INUSE:
	case MAIL_ERROR_LIMIT:
		exit_code = DOVEADM_EX_NOTPOSSIBLE;
		break;
	case MAIL_ERROR_LOOKUP_ABORTED:
	case MAIL_ERROR_INTERRUPTED:
		break;
	}
	/* tempfail overrides all other exit codes, otherwise use whatever
	   error happened first */
	if (ctx->exit_code == 0 || exit_code == EX_TEMPFAIL)
		ctx->exit_code = exit_code;
}

void doveadm_mail_failed_storage(struct doveadm_mail_cmd_context *ctx,
				 struct mail_storage *storage)
{
	enum mail_error error;

	mail_storage_get_last_error(storage, &error);
	doveadm_mail_failed_error(ctx, error);
}

void doveadm_mail_failed_mailbox(struct doveadm_mail_cmd_context *ctx,
				 struct mailbox *box)
{
	doveadm_mail_failed_storage(ctx, mailbox_get_storage(box));
}

void doveadm_mail_failed_list(struct doveadm_mail_cmd_context *ctx,
			      struct mailbox_list *list)
{
	enum mail_error error;

	mailbox_list_get_last_error(list, &error);
	doveadm_mail_failed_error(ctx, error);
}

struct doveadm_mail_cmd_context *
doveadm_mail_cmd_alloc_size(size_t size)
{
	struct doveadm_mail_cmd_context *ctx;
	pool_t pool;

	i_assert(size >= sizeof(struct doveadm_mail_cmd_context));

	pool = pool_alloconly_create("doveadm mail cmd", 1024);
	ctx = p_malloc(pool, size);
	ctx->pool = pool;
	ctx->cmd_input_fd = -1;
	return ctx;
}

static int
cmd_purge_run(struct doveadm_mail_cmd_context *ctx, struct mail_user *user)
{
	struct mail_namespace *ns;
	struct mail_storage *storage;
	int ret = 0;

	for (ns = user->namespaces; ns != NULL; ns = ns->next) {
		if (ns->type != MAIL_NAMESPACE_TYPE_PRIVATE ||
		    ns->alias_for != NULL)
			continue;

		storage = mail_namespace_get_default_storage(ns);
		if (mail_storage_purge(storage) < 0) {
			e_error(ctx->cctx->event,
				"Purging namespace %s failed: %s", ns->set->name,
				mail_storage_get_last_internal_error(storage, NULL));
			doveadm_mail_failed_storage(ctx, storage);
			ret = -1;
		}
	}
	return ret;
}

static struct doveadm_mail_cmd_context *cmd_purge_alloc(void)
{
	struct doveadm_mail_cmd_context *ctx;

	ctx = doveadm_mail_cmd_alloc(struct doveadm_mail_cmd_context);
	ctx->v.run = cmd_purge_run;
	return ctx;
}

static void doveadm_mail_cmd_input_input(struct doveadm_mail_cmd_context *ctx)
{
	const unsigned char *data;
	size_t size;

	while (i_stream_read_more(ctx->cmd_input, &data, &size) > 0)
		i_stream_skip(ctx->cmd_input, size);
	if (!ctx->cmd_input->eof)
		return;

	if (ctx->cmd_input->stream_errno != 0) {
		e_error(ctx->cctx->event, "read(%s) failed: %s",
			i_stream_get_name(ctx->cmd_input),
			i_stream_get_error(ctx->cmd_input));
	}
	io_loop_stop(current_ioloop);
}

static void doveadm_mail_cmd_input_timeout(struct doveadm_mail_cmd_context *ctx)
{
	struct istream *input;

	input = i_stream_create_error_str(ETIMEDOUT, "Timed out in %u secs",
			DOVEADM_MAIL_CMD_INPUT_TIMEOUT_MSECS/1000);
	i_stream_set_name(input, i_stream_get_name(ctx->cmd_input));
	i_stream_destroy(&ctx->cmd_input);
	ctx->cmd_input = input;
	ctx->exit_code = EX_TEMPFAIL;
	io_loop_stop(current_ioloop);
}

static void doveadm_mail_cmd_input_read(struct doveadm_mail_cmd_context *ctx)
{
	struct ioloop *ioloop;
	struct io *io;
	struct timeout *to;

	ioloop = io_loop_create();
	/* Read the pending input from stream. Delay adding the IO in case
	   we're reading from a file. That would cause a panic with epoll. */
	io_loop_set_running(ioloop);
	doveadm_mail_cmd_input_input(ctx);
	if (io_loop_is_running(ioloop)) {
		io = io_add(ctx->cmd_input_fd, IO_READ,
			    doveadm_mail_cmd_input_input, ctx);
		to = timeout_add(DOVEADM_MAIL_CMD_INPUT_TIMEOUT_MSECS,
				 doveadm_mail_cmd_input_timeout, ctx);
		io_loop_run(ioloop);
		io_remove(&io);
		timeout_remove(&to);
	}
	io_loop_destroy(&ioloop);

	i_assert(ctx->cmd_input->eof);
	i_stream_seek(ctx->cmd_input, 0);
}

void doveadm_mail_get_input(struct doveadm_mail_cmd_context *ctx)
{
	const struct doveadm_cmd_context *cctx = ctx->cctx;
	bool cli = (cctx->conn_type == DOVEADM_CONNECTION_TYPE_CLI);
	struct istream *inputs[2];

	if (ctx->cmd_input != NULL)
		return;

	if (!cli && cctx->input == NULL) {
		ctx->cmd_input = i_stream_create_error_str(EINVAL,
			"Input stream missing (provide with file parameter)");
		return;
	}

	if (!cli)
		inputs[0] = i_stream_create_dot(cctx->input,
						ISTREAM_DOT_TRIM_TRAIL |
						ISTREAM_DOT_LOOSE_EOT);
	else {
		inputs[0] = i_stream_create_fd(STDIN_FILENO, 1024*1024);
		i_stream_set_name(inputs[0], "stdin");
	}
	inputs[1] = NULL;
	ctx->cmd_input_fd = i_stream_get_fd(inputs[0]);
	ctx->cmd_input = i_stream_create_seekable_path(inputs, 1024*256,
						       "/tmp/doveadm.");
	i_stream_set_name(ctx->cmd_input, i_stream_get_name(inputs[0]));
	i_stream_unref(&inputs[0]);

	doveadm_mail_cmd_input_read(ctx);
}

const char *const *
doveadm_mail_get_forward_fields(struct doveadm_mail_cmd_context *ctx)
{
	if (!array_is_created(&ctx->proxy_forward_fields))
		return NULL;

	array_append_zero(&ctx->proxy_forward_fields);
	array_pop_back(&ctx->proxy_forward_fields);
	return array_front(&ctx->proxy_forward_fields);
}

struct mailbox *
doveadm_mailbox_find(struct mail_user *user, const char *mailbox)
{
	struct mail_namespace *ns;

	if (!uni_utf8_str_is_valid(mailbox)) {
		i_fatal_status(EX_DATAERR,
			       "Mailbox name not valid UTF-8: %s", mailbox);
	}

	ns = mail_namespace_find(user->namespaces, mailbox);
	return mailbox_alloc(ns->list, mailbox, MAILBOX_FLAG_IGNORE_ACLS);
}

struct mail_search_args *
doveadm_mail_build_search_args(const char *const args[])
{
	struct mail_search_parser *parser;
	struct mail_search_args *sargs;
	const char *error, *charset = "UTF-8";

	parser = mail_search_parser_init_cmdline(args);
	if (mail_search_build(mail_search_register_get_human(),
			      parser, &charset, &sargs, &error) < 0)
		i_fatal("%s", error);
	mail_search_parser_deinit(&parser);
	return sargs;
}

static int cmd_force_resync_box(struct doveadm_mail_cmd_context *_ctx,
				const struct mailbox_info *info)
{
	struct force_resync_cmd_context *ctx =
		container_of(_ctx, struct force_resync_cmd_context, ctx);

	enum mailbox_flags flags = MAILBOX_FLAG_IGNORE_ACLS;
	struct mailbox *box;
	int ret = 0;

	if (ctx->fsck)
		flags |= MAILBOX_FLAG_FSCK;

	box = mailbox_alloc(info->ns->list, info->vname, flags);
	if (mailbox_open(box) < 0) {
		e_error(ctx->ctx.cctx->event,
			"Opening mailbox %s failed: %s", info->vname,
			mailbox_get_last_internal_error(box, NULL));
		doveadm_mail_failed_mailbox(_ctx, box);
		ret = -1;
	} else if (mailbox_sync(box, MAILBOX_SYNC_FLAG_FORCE_RESYNC |
				MAILBOX_SYNC_FLAG_FIX_INCONSISTENT) < 0) {
		e_error(ctx->ctx.cctx->event,
			"Forcing a resync on mailbox %s failed: %s",
			info->vname, mailbox_get_last_internal_error(box, NULL));
		doveadm_mail_failed_mailbox(_ctx, box);
		ret = -1;
	}
	mailbox_free(&box);
	return ret;
}

static int cmd_force_resync_prerun(struct doveadm_mail_cmd_context *ctx ATTR_UNUSED,
				   struct mail_storage_service_user *service_user,
				   const char **error_r ATTR_UNUSED)
{
	struct settings_instance *set_instance =
		mail_storage_service_user_get_settings_instance(service_user);
	settings_override(set_instance,
			  "*/mailbox_list_index_very_dirty_syncs", "no",
			  SETTINGS_OVERRIDE_TYPE_CODE);
	return 0;
}

static int cmd_force_resync_run(struct doveadm_mail_cmd_context *_ctx,
				struct mail_user *user)
{
	struct force_resync_cmd_context *ctx =
		container_of(_ctx, struct force_resync_cmd_context, ctx);

	const enum mailbox_list_iter_flags iter_flags =
		MAILBOX_LIST_ITER_NO_AUTO_BOXES |
		MAILBOX_LIST_ITER_RETURN_NO_FLAGS |
		MAILBOX_LIST_ITER_STAR_WITHIN_NS |
		MAILBOX_LIST_ITER_RAW_LIST |
		MAILBOX_LIST_ITER_FORCE_RESYNC;
	const enum mail_namespace_type ns_mask = MAIL_NAMESPACE_TYPE_MASK_ALL;
	struct mailbox_list_iterate_context *iter;
	const struct mailbox_info *info;
	int ret = 0;

	const char *const patterns[] = {
		ctx->mailbox,
		NULL
	};
	iter = mailbox_list_iter_init_namespaces(
		user->namespaces, patterns, ns_mask, iter_flags);
	while ((info = mailbox_list_iter_next(iter)) != NULL) {
		if ((info->flags & (MAILBOX_NOSELECT |
				    MAILBOX_NONEXISTENT)) == 0) T_BEGIN {
			if (cmd_force_resync_box(_ctx, info) < 0)
				ret = -1;
		} T_END;
	}
	if (mailbox_list_iter_deinit(&iter) < 0) {
		e_error(ctx->ctx.cctx->event,
			"Listing mailboxes failed: %s",
			mailbox_list_get_last_internal_error(user->namespaces->list, NULL));
		doveadm_mail_failed_list(_ctx, user->namespaces->list);
		ret = -1;
	}
	return ret;
}

static void
cmd_force_resync_init(struct doveadm_mail_cmd_context *_ctx)
{
	struct doveadm_cmd_context *cctx = _ctx->cctx;
	struct force_resync_cmd_context *ctx =
		container_of(_ctx, struct force_resync_cmd_context, ctx);

	ctx->fsck = doveadm_cmd_param_flag(cctx, "fsck");
	if (!doveadm_cmd_param_str(cctx, "mailbox-mask", &ctx->mailbox))
		doveadm_mail_help_name("force-resync");
}

static struct doveadm_mail_cmd_context *cmd_force_resync_alloc(void)
{
	struct force_resync_cmd_context *ctx;

	ctx = doveadm_mail_cmd_alloc(struct force_resync_cmd_context);
	ctx->ctx.v.init = cmd_force_resync_init;
	ctx->ctx.v.run = cmd_force_resync_run;
	ctx->ctx.v.prerun = cmd_force_resync_prerun;
	return &ctx->ctx;
}

static void
doveadm_mail_ctx_to_storage_service_input(struct doveadm_mail_cmd_context *ctx,
					  struct mail_storage_service_input *input_r)
{
	const struct doveadm_cmd_context *cctx = ctx->cctx;

	i_zero(input_r);
	input_r->service = "doveadm";
	input_r->remote_ip = cctx->remote_ip;
	input_r->remote_port = cctx->remote_port;
	input_r->local_ip = cctx->local_ip;
	input_r->local_port = cctx->local_port;
	input_r->username = cctx->username;
	input_r->forward_fields = doveadm_mail_get_forward_fields(ctx);
}

static int
doveadm_mail_next_user(struct doveadm_mail_cmd_context *ctx,
		       const char **error_r)
{
	const struct doveadm_cmd_context *cctx = ctx->cctx;
	struct mail_storage_service_input input;
	const char *error, *ip;
	int ret;

	i_assert(cctx != NULL);

	ip = net_ip2addr(&cctx->remote_ip);
	if (ip[0] == '\0')
		i_set_failure_prefix("doveadm(%s): ", cctx->username);
	else
		i_set_failure_prefix("doveadm(%s,%s): ", ip, cctx->username);
	if (ctx->cmd_input != NULL)
		i_stream_seek(ctx->cmd_input, 0);

	/* see if we want to execute this command via (another)
	   doveadm server */
	ret = doveadm_mail_server_user(ctx, error_r);
	if (ret != 0)
		return ret;

	doveadm_mail_ctx_to_storage_service_input(ctx, &input);
	ret = mail_storage_service_lookup(ctx->storage_service, &input,
					  &ctx->cur_service_user, &error);
	if (ret <= 0) {
		if (ret < 0) {
			*error_r = t_strdup_printf("User lookup failed: %s",
						   error);
		}
		return ret;
	}

	if (doveadm_print_is_initialized() && !ctx->iterate_single_user)
		doveadm_print_sticky("username", cctx->username);

	if (ctx->v.prerun != NULL) {
		T_BEGIN {
			ret = ctx->v.prerun(ctx, ctx->cur_service_user, error_r);
		} T_END_PASS_STR_IF(ret < 0, error_r);
		if (ret < 0) {
			mail_storage_service_user_unref(&ctx->cur_service_user);
			return -1;
		}
	}

	bool dropping_privs = HAS_ANY_BITS(ctx->service_flags,
				 	   MAIL_STORAGE_SERVICE_FLAG_TEMP_PRIV_DROP);
	uid_t cur_uid = geteuid();
	const char *cur_cwd;
	if (t_get_working_dir(&cur_cwd, error_r) < 0) {
		mail_storage_service_user_unref(&ctx->cur_service_user);
		return -1;
	}

	ret = mail_storage_service_next(ctx->storage_service,
					ctx->cur_service_user,
					&ctx->cur_mail_user, error_r);
	if (ret < 0) {
		mail_storage_service_user_unref(&ctx->cur_service_user);
		if (dropping_privs)
			mail_storage_service_restore_privileges(cur_uid, cur_cwd,
								cctx->event);
		return ret;
	}

	/* Create the event outside the active ioloop context, so if run()
	   switches the ioloop context it won't try to pop out the event_reason
	   from global events. */
	struct ioloop_context *cur_ctx =
		io_loop_get_current_context(current_ioloop);
	io_loop_context_deactivate(cur_ctx);
	struct event_reason *reason =
		event_reason_begin(event_reason_code_prefix("doveadm", "cmd_",
							    ctx->cmd->name));
	io_loop_context_activate(cur_ctx);

	T_BEGIN {
		if (ctx->v.run(ctx, ctx->cur_mail_user) < 0) {
			i_assert(ctx->exit_code != 0);
		}
	} T_END;
	mail_user_deinit(&ctx->cur_mail_user);
	mail_storage_service_user_unref(&ctx->cur_service_user);
	/* User deinit may still do some work, so finish the reason after it.
	   Also, this needs to be after the ioloop context is deactivated. */
	event_reason_end(&reason);
	if (dropping_privs)
		mail_storage_service_restore_privileges(cur_uid, cur_cwd,
							cctx->event);
	return 1;
}

int doveadm_mail_single_user(struct doveadm_mail_cmd_context *ctx,
			     const char **error_r)
{
	const struct doveadm_cmd_context *cctx = ctx->cctx;

	i_assert(cctx->username != NULL);

	doveadm_mail_ctx_to_storage_service_input(ctx, &ctx->storage_service_input);
	ctx->storage_service = mail_storage_service_init(master_service,
							 ctx->service_flags);
	T_BEGIN {
		ctx->v.init(ctx);
	} T_END;
	if (ctx->exit_code != 0) {
		/* return success, so caller won't overwrite exit_code */
		return 1;
	}

	doveadm_print_header_disallow(TRUE);
	if (hook_doveadm_mail_init != NULL)
		hook_doveadm_mail_init(ctx);

	return doveadm_mail_next_user(ctx, error_r);
}

static void
doveadm_mail_all_users(struct doveadm_mail_cmd_context *ctx,
		       const char *wildcard_user)
{
	struct doveadm_cmd_context *cctx = ctx->cctx;
	unsigned int user_idx;
	const char *ip, *user, *error;
	int ret;

	ctx->service_flags |= MAIL_STORAGE_SERVICE_FLAG_USERDB_LOOKUP;

	doveadm_mail_ctx_to_storage_service_input(ctx, &ctx->storage_service_input);
	ctx->storage_service = mail_storage_service_init(master_service,
							 ctx->service_flags);

	T_BEGIN {
		ctx->v.init(ctx);
	} T_END;
	if (ctx->exit_code != 0)
		return;
	doveadm_print_header_disallow(TRUE);

	if (wildcard_user != NULL) {
		mail_storage_service_all_init_mask(ctx->storage_service,
						   wildcard_user);
	}

	if (hook_doveadm_mail_init != NULL)
		hook_doveadm_mail_init(ctx);

	user_idx = 0;
	while ((ret = ctx->v.get_next_user(ctx, &user)) > 0) {
		if (wildcard_user != NULL) {
			if (!wildcard_match_icase(user, wildcard_user))
				continue;
		}
		cctx->username = user;
		T_BEGIN {
			ret = doveadm_mail_next_user(ctx, &error);
			if (ret < 0)
				e_error(ctx->cctx->event, "%s", error);
			else if (ret == 0)
				e_info(ctx->cctx->event,
				       "User no longer exists, skipping");
		} T_END;
		if (ret == -1)
			break;
		if (doveadm_verbose) {
			if (++user_idx % 100 == 0) {
				printf("\r%d", user_idx);
				fflush(stdout);
			}
		}
		if (doveadm_is_killed()) {
			ret = -1;
			break;
		}
	}
	if (doveadm_verbose)
		printf("\n");
	ip = net_ip2addr(&cctx->remote_ip);
	if (ip[0] == '\0')
		i_set_failure_prefix("doveadm: ");
	else
		i_set_failure_prefix("doveadm(%s): ", ip);
	if (ret < 0) {
		e_error(ctx->cctx->event, "Failed to iterate through some users");
		ctx->exit_code = EX_TEMPFAIL;
	}
}

static void
doveadm_mail_cmd_init_noop(struct doveadm_mail_cmd_context *ctx ATTR_UNUSED)
{
}

static int
doveadm_mail_cmd_get_next_user(struct doveadm_mail_cmd_context *ctx,
			       const char **username_r)
{
	if (ctx->users_list_input == NULL)
		return mail_storage_service_all_next(ctx->storage_service, username_r);

	*username_r = i_stream_read_next_line(ctx->users_list_input);
	if (ctx->users_list_input->stream_errno != 0) {
		e_error(ctx->cctx->event, "read(%s) failed: %s",
			i_stream_get_name(ctx->users_list_input),
			i_stream_get_error(ctx->users_list_input));
		return -1;
	}
	return *username_r != NULL ? 1 : 0;
}

static void
doveadm_mail_cmd_deinit_noop(struct doveadm_mail_cmd_context *ctx ATTR_UNUSED)
{
}

struct doveadm_mail_cmd_context *
doveadm_mail_cmd_init(const struct doveadm_mail_cmd *cmd,
		      const struct doveadm_settings *set)
{
	struct doveadm_mail_cmd_context *ctx;

	ctx = cmd->alloc();
	ctx->set = set;
	ctx->cmd = cmd;
	ctx->proxy_ttl = DOVEADM_PROXY_TTL;
	if (ctx->v.init == NULL)
		ctx->v.init = doveadm_mail_cmd_init_noop;
	if (ctx->v.get_next_user == NULL)
		ctx->v.get_next_user = doveadm_mail_cmd_get_next_user;
	if (ctx->v.deinit == NULL)
		ctx->v.deinit = doveadm_mail_cmd_deinit_noop;
	if (!doveadm_print_is_initialized()) {
		/* alloc() should call doveadm_print_init(). It's too late
		   afterwards. */
		doveadm_print_init_disallow(TRUE);
	}

	p_array_init(&ctx->module_contexts, ctx->pool, 5);
	return ctx;
}

static struct doveadm_mail_cmd_context *
doveadm_mail_cmdline_init(const struct doveadm_mail_cmd *cmd)
{
	struct doveadm_mail_cmd_context *ctx;

	ctx = doveadm_mail_cmd_init(cmd, doveadm_settings);
	ctx->service_flags |= MAIL_STORAGE_SERVICE_FLAG_NO_LOG_INIT;
	if (doveadm_debug)
		ctx->service_flags |= MAIL_STORAGE_SERVICE_FLAG_DEBUG;
	return ctx;
}

static void
doveadm_mail_cmd_exec(struct doveadm_mail_cmd_context *ctx,
		      const char *wildcard_user)
{
	const struct doveadm_cmd_context *cctx = ctx->cctx;
	bool cli = (cctx->conn_type == DOVEADM_CONNECTION_TYPE_CLI);
	int ret;
	const char *error;

	if (ctx->v.preinit != NULL) T_BEGIN {
		ctx->v.preinit(ctx);
	} T_END;

	ctx->iterate_single_user = wildcard_user == NULL && ctx->users_list_input == NULL;
	if (doveadm_print_is_initialized() && !ctx->iterate_single_user) {
		doveadm_print_header("username", "Username",
				     DOVEADM_PRINT_HEADER_FLAG_STICKY |
				     DOVEADM_PRINT_HEADER_FLAG_HIDE_TITLE);
	}

	if (ctx->iterate_single_user) {
		if (cctx->username == NULL)
			i_fatal_status(EX_USAGE, "USER environment is missing and -u option not used");
		if (!cli) {
			/* we may access multiple users */
			ctx->service_flags |= MAIL_STORAGE_SERVICE_FLAG_TEMP_PRIV_DROP;
		}

		ret = doveadm_mail_single_user(ctx, &error);
		if (ret < 0) {
			/* user lookup/init failed somehow */
			doveadm_exit_code = EX_TEMPFAIL;
			e_error(ctx->cctx->event, "%s", error);
		} else if (ret == 0) {
			doveadm_exit_code = EX_NOUSER;
			e_error(ctx->cctx->event, "User doesn't exist");
		}
	} else {
		ctx->service_flags |= MAIL_STORAGE_SERVICE_FLAG_TEMP_PRIV_DROP;
		doveadm_mail_all_users(ctx, wildcard_user);
	}
	doveadm_mail_server_flush(ctx);
	doveadm_mail_cmd_deinit(ctx);
	doveadm_print_flush();

	/* service deinit unloads mail plugins, so do it late */
	mail_storage_service_deinit(&ctx->storage_service);

	if (ctx->exit_code != 0)
		doveadm_exit_code = ctx->exit_code;
}

void doveadm_mail_cmd_deinit(struct doveadm_mail_cmd_context *ctx)
{
	T_BEGIN {
		ctx->v.deinit(ctx);
	} T_END;
	if (ctx->search_args != NULL)
		mail_search_args_unref(&ctx->search_args);
}

void doveadm_mail_cmd_free(struct doveadm_mail_cmd_context *ctx)
{
	i_stream_unref(&ctx->users_list_input);
	i_stream_unref(&ctx->cmd_input);
	pool_unref(&ctx->pool);
}

void doveadm_mail_help(const struct doveadm_mail_cmd *cmd)
{
	fprintf(stderr, "doveadm %s "DOVEADM_CMD_MAIL_USAGE_PREFIX" %s\n",
		cmd->name, cmd->usage_args == NULL ? "" : cmd->usage_args);
	lib_exit(EX_USAGE);
}

void doveadm_mail_try_help_name(const char *cmd_name)
{
	const struct doveadm_cmd_ver2 *cmd2;

	cmd2 = doveadm_cmd_find_ver2(cmd_name);
	if (cmd2 != NULL)
		help_ver2(cmd2);
}

void doveadm_mail_help_name(const char *cmd_name)
{
	doveadm_mail_try_help_name(cmd_name);
	i_fatal("Missing help for command %s", cmd_name);
}

static struct doveadm_cmd_ver2 doveadm_cmd_force_resync_ver2 = {
	.name = "force-resync",
	.mail_cmd = cmd_force_resync_alloc,
	.usage = DOVEADM_CMD_MAIL_USAGE_PREFIX "[-f] <mailbox mask>",
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_MAIL_COMMON
DOVEADM_CMD_PARAM('f', "fsck", CMD_PARAM_BOOL, 0)
DOVEADM_CMD_PARAM('\0', "mailbox-mask", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAMS_END
};

static struct doveadm_cmd_ver2 doveadm_cmd_purge_ver2 = {
	.name = "purge",
	.mail_cmd = cmd_purge_alloc,
	.usage = DOVEADM_CMD_MAIL_USAGE_PREFIX,
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_MAIL_COMMON
DOVEADM_CMD_PARAMS_END
};

static struct doveadm_cmd_ver2 *mail_commands_ver2[] = {
	&doveadm_cmd_dsync_backup,
	&doveadm_cmd_dsync_mirror,
	&doveadm_cmd_dsync_server,
	&doveadm_cmd_mailbox_metadata_set_ver2,
	&doveadm_cmd_mailbox_metadata_unset_ver2,
	&doveadm_cmd_mailbox_metadata_get_ver2,
	&doveadm_cmd_mailbox_metadata_list_ver2,
	&doveadm_cmd_mailbox_status_ver2,
	&doveadm_cmd_mailbox_list_ver2,
	&doveadm_cmd_mailbox_create_ver2,
	&doveadm_cmd_mailbox_delete_ver2,
	&doveadm_cmd_mailbox_rename_ver2,
	&doveadm_cmd_mailbox_subscribe_ver2,
	&doveadm_cmd_mailbox_unsubscribe_ver2,
	&doveadm_cmd_mailbox_update_ver2,
	&doveadm_cmd_mailbox_path_ver2,
	&doveadm_cmd_fetch_ver2,
	&doveadm_cmd_save_ver2,
	&doveadm_cmd_index_ver2,
	&doveadm_cmd_altmove_ver2,
	&doveadm_cmd_deduplicate_ver2,
	&doveadm_cmd_expunge_ver2,
	&doveadm_cmd_flags_add_ver2,
	&doveadm_cmd_flags_remove_ver2,
	&doveadm_cmd_flags_replace_ver2,
	&doveadm_cmd_import_ver2,
	&doveadm_cmd_force_resync_ver2,
	&doveadm_cmd_purge_ver2,
	&doveadm_cmd_search_ver2,
	&doveadm_cmd_copy_ver2,
	&doveadm_cmd_move_ver2,
	&doveadm_cmd_mailbox_cache_decision,
	&doveadm_cmd_mailbox_cache_remove,
	&doveadm_cmd_mailbox_cache_purge,
	&doveadm_cmd_rebuild_attachments,
	&doveadm_cmd_mail_fs_get,
	&doveadm_cmd_mail_fs_put,
	&doveadm_cmd_mail_fs_copy,
	&doveadm_cmd_mail_fs_stat,
	&doveadm_cmd_mail_fs_metadata,
	&doveadm_cmd_mail_fs_delete,
	&doveadm_cmd_mail_fs_iter,
	&doveadm_cmd_mail_fs_iter_dirs,
	&doveadm_cmd_mail_dict_get,
	&doveadm_cmd_mail_dict_set,
	&doveadm_cmd_mail_dict_unset,
	&doveadm_cmd_mail_dict_inc,
	&doveadm_cmd_mail_dict_iter,
};

void doveadm_mail_init(void)
{
	unsigned int i;

	for (i = 0; i < N_ELEMENTS(mail_commands_ver2); i++)
		doveadm_cmd_register_ver2(mail_commands_ver2[i]);
}

void doveadm_mail_init_finish(void)
{
	struct module_dir_load_settings mod_set;

	i_zero(&mod_set);
	mod_set.abi_version = DOVECOT_ABI_VERSION;
	mod_set.require_init_funcs = TRUE;
	mod_set.debug = doveadm_debug;
	mod_set.binary_name = "doveadm";

	/* load all configured mail plugins */
	if (array_is_created(&doveadm_settings->mail_plugins) &&
	    array_not_empty(&doveadm_settings->mail_plugins)) {
		mail_storage_service_modules =
			module_dir_load_missing(mail_storage_service_modules,
						doveadm_settings->mail_plugin_dir,
						settings_boollist_get(&doveadm_settings->mail_plugins),
						&mod_set);
	}
	/* keep mail_storage_init() referenced so that its _deinit() doesn't
	   try to free doveadm plugins' hooks too early. */
	mail_storage_init();
}

void doveadm_mail_deinit(void)
{
	mail_storage_deinit();
	module_dir_unload(&mail_storage_service_modules);
}

static void
doveadm_cmdv2_wrapper_parse_common_options(struct doveadm_mail_cmd_context *mctx,
					   const char **wildcard_user_r)
{
	struct doveadm_cmd_context *cctx = mctx->cctx;
	bool tcp_server = cctx->conn_type == DOVEADM_CONNECTION_TYPE_TCP;
	const char *socket_path, *value_str;

	mctx->service_flags |= MAIL_STORAGE_SERVICE_FLAG_USERDB_LOOKUP;
	*wildcard_user_r = NULL;
	if (doveadm_cmd_param_flag(cctx, "all-users")) {
		*wildcard_user_r = "*";
	} else if (doveadm_cmd_param_istream(cctx, "user-file", &mctx->users_list_input)) {
		i_stream_ref(mctx->users_list_input);
	} else if (doveadm_cmd_param_str(cctx, "user", &value_str)) {
		if (!tcp_server)
			cctx->username = value_str;

		if (strchr(value_str, '*') != NULL ||
		    strchr(value_str, '?') != NULL) {
			if (!tcp_server) {
				*wildcard_user_r = value_str;
				cctx->username = NULL;
			}
		}
	} else if (doveadm_server) {
		/* Protocol sets this in correct place, don't require a
		   command line parameter */
	} else if (doveadm_cmd_param_flag(cctx, "no-userdb-lookup")) {
		mctx->service_flags &=
			ENUM_NEGATE(MAIL_STORAGE_SERVICE_FLAG_USERDB_LOOKUP);
	} else {
		i_fatal("One of -u, -F, -A or --no-userdb-lookup must be provided");
	}

	if (doveadm_cmd_param_str(cctx, "socket-path", &socket_path)) {
		struct doveadm_settings *set =
			p_memdup(doveadm_settings->pool, doveadm_settings,
				 sizeof(*doveadm_settings));
		set->doveadm_socket_path = p_strdup(set->pool, socket_path);
		if (set->doveadm_worker_count == 0)
			set->doveadm_worker_count = 1;
		doveadm_settings = mctx->set = set;
	}

	if (doveadm_cmd_param_istream(cctx, "file", &mctx->cmd_input))
		i_stream_ref(mctx->cmd_input);

	(void)doveadm_cmd_param_uint32(cctx, "trans-flags", &mctx->transaction_flags);
}

static void
doveadm_cmdv2_wrapper_generate_full_arg(struct doveadm_mail_cmd_context *mctx,
					const struct doveadm_cmd_param *arg,
					ARRAY_TYPE(const_string) *opt_args,
					ARRAY_TYPE(const_string) *pos_args)
{
	if (!arg->value_set ||
	    strcmp(arg->name, "socket-path") == 0 ||
	    strcmp(arg->name, "trans-flags") == 0 ||
	    strcmp(arg->name, "file") == 0 ||
	    strcmp(arg->name, "all-users") == 0 ||
	    strcmp(arg->name, "user-file") == 0 ||
	    strcmp(arg->name, "no-userdb-lookup") == 0)
		return;

	if (strcmp(arg->name, "field") == 0 ||
	    strcmp(arg->name, "flag") == 0) {
		const char *value = p_array_const_string_join(
			mctx->pool, &arg->value.v_array, " ");
		array_push_back(pos_args, &value);
		return;
	}

	ARRAY_TYPE(const_string) *dest;
	const char *opt = NULL;

	if ((arg->flags & CMD_PARAM_FLAG_POSITIONAL) == 0) {
		dest = opt_args;
		opt = arg->short_opt != '\0' ?
			p_strdup_printf(mctx->pool, "-%c", arg->short_opt) :
			p_strdup_printf(mctx->pool, "--%s", arg->name);
	} else {
		dest = pos_args;
		if ((arg->flags & CMD_PARAM_FLAG_KEY_VALUE) != 0)
			opt = arg->name;
	}

	if (arg->type == CMD_PARAM_ARRAY) {
		const char *const *entry = NULL;
		array_foreach(&arg->value.v_array, entry) {
			if (opt != NULL) array_push_back(dest, &opt);
			array_push_back(dest, entry);
		}
		return;
	}

	const char *value = NULL;
	switch (arg->type) {
	case CMD_PARAM_BOOL:
		break;
	case CMD_PARAM_INT64:
		value = dec2str(arg->value.v_int64);
		break;
	case CMD_PARAM_IP:
		value = net_ip2addr(&arg->value.v_ip);
		break;
	case CMD_PARAM_STR:
		value = arg->value.v_string;
		break;
	default:
		i_panic("Cannot convert parameter %s to short opt", arg->name);
	}

	if (opt   != NULL) array_push_back(dest, &opt);
	if (value != NULL) array_push_back(dest, &value);
}

const char *const *
doveadm_cmdv2_wrapper_generate_args(struct doveadm_mail_cmd_context *ctx)
{
	struct doveadm_cmd_context *cctx =  ctx->cctx;
	ARRAY_TYPE(const_string) pos_args, all_args;
	p_array_init(&all_args, ctx->pool, 8);
	p_array_init(&pos_args, ctx->pool, 8);

	for (int index = 0; index < cctx->argc; index++)
		doveadm_cmdv2_wrapper_generate_full_arg(
			ctx, &cctx->argv[index], &all_args, &pos_args);

	const char *dashdash = "--";
	array_push_back(&all_args, &dashdash);
	array_append_array(&all_args, &pos_args);
	array_append_zero(&all_args);
	return array_front(&all_args);
}

void
doveadm_cmd_ver2_to_mail_cmd_wrapper(struct doveadm_cmd_context *cctx)
{
	struct doveadm_mail_cmd_context *mctx;
	struct doveadm_mail_cmd mail_cmd = {
		.alloc = cctx->cmd->mail_cmd,
		.name = cctx->cmd->name,
		.usage_args = cctx->cmd->usage
	};

	if (cctx->conn_type == DOVEADM_CONNECTION_TYPE_CLI)
		mctx = doveadm_mail_cmdline_init(&mail_cmd);
	else {
		mctx = doveadm_mail_cmd_init(&mail_cmd, doveadm_settings);
		/* doveadm-server always does userdb lookups */
		mctx->service_flags |= MAIL_STORAGE_SERVICE_FLAG_USERDB_LOOKUP;
	}
	mctx->cctx = cctx;

	const char *wildcard_user;
	doveadm_cmdv2_wrapper_parse_common_options(mctx, &wildcard_user);
	doveadm_mail_cmd_exec(mctx, wildcard_user);
	doveadm_mail_cmd_free(mctx);
}
